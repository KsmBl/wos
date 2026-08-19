/* Reading a Makefile: variables, rules, and `$(...)` expansion.
 *
 * The whole file is read into memory first.  Makefiles here are a few hundred
 * bytes, so there is nothing to be gained by streaming one -- and having the
 * whole text in one place is what makes joining continuation lines two lines
 * of code.
 */

#include "make.h"

rule_t      rules[MAX_RULES];
int         rule_count;
const char *default_goal;

static var_t vars[MAX_VARS];
static int   var_count;

/* Where the current line came from, so a complaint can name it.  Until a file
 * is open that is the command line, which is where the first variables are
 * assigned from. */
static const char *current_file = "make: command line";
static int         current_line;

char *xstrdup(const char *s)
{
    wsize_t n = strlen(s) + 1;
    char   *copy = malloc(n);

    if (!copy) {
        wfprintf(W_STDERR, "make: out of memory\n");
        wexit(2);
    }

    memcpy(copy, s, n);
    return copy;
}

static void complain(const char *what)
{
    wfprintf(W_STDERR, "%s:%d: %s\n", current_file, current_line, what);
}

/* ------------------------------------------------------------------ *
 *  Variables
 * ------------------------------------------------------------------ */

static var_t *var_lookup(const char *name)
{
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    return NULL;
}

int var_set(const char *name, const char *value, int immediate, int append,
            int from_argv)
{
    char expanded[MAKE_EXPAND_MAX];

    /* `:=` settles the value here; `=` keeps the text and settles it at every
     * use.  An appended value follows whichever kind it is joining. */
    if (immediate) {
        if (expand(value, NULL, expanded, sizeof(expanded)) < 0)
            return -1;
        value = expanded;
    }

    var_t *v = var_lookup(name);

    if (v) {
        /* A variable given on the command line is the answer, and an
         * assignment in the file is not an error -- it is simply overridden,
         * which is what lets `make CC=tcc` work on a Makefile that sets CC. */
        if (v->from_argv && !from_argv)
            return 0;

        if (append) {
            char joined[MAKE_EXPAND_MAX];
            int  n = wsnprintf(joined, sizeof(joined), "%s %s", v->value, value);

            if (n < 0 || n >= (int)sizeof(joined)) {
                complain("the value of this variable is too long");
                return -1;
            }

            free(v->value);
            v->value = xstrdup(joined);
            return 0;
        }

        free(v->value);
        v->value      = xstrdup(value);
        v->recursive  = !immediate;
        v->from_argv  = v->from_argv || from_argv;
        return 0;
    }

    if (var_count == MAX_VARS) {
        complain("too many variables");
        return -1;
    }

    v = &vars[var_count++];
    v->name      = xstrdup(name);
    v->value     = xstrdup(value);
    v->recursive = !immediate;
    v->from_argv = from_argv;
    return 0;
}

const char *var_get(const char *name)
{
    var_t *v = var_lookup(name);
    return v ? v->value : "";
}

/* ------------------------------------------------------------------ *
 *  Expansion
 * ------------------------------------------------------------------ */

/* Append `text` to `out`, refusing rather than overflowing. */
static int append_text(char *out, int size, int *len, const char *text)
{
    while (*text) {
        if (*len + 1 >= size) {
            complain("this line is too long once its variables are expanded");
            return -1;
        }
        out[(*len)++] = *text++;
    }
    out[*len] = '\0';
    return 0;
}

static int expand_depth;

int expand(const char *in, const rule_t *ctx, char *out, int size)
{
    int len = 0;

    out[0] = '\0';

    /* A variable defined as itself -- `CFLAGS = $(CFLAGS) -O2`, the mistake
     * every Makefile author makes once -- would otherwise expand forever. */
    if (++expand_depth > 32) {
        complain("a variable is defined in terms of itself");
        expand_depth--;
        return -1;
    }

    while (*in) {
        if (*in != '$') {
            if (len + 1 >= size) {
                complain("this line is too long once its variables are expanded");
                expand_depth--;
                return -1;
            }
            out[len++] = *in++;
            out[len] = '\0';
            continue;
        }

        in++;                                   /* past the '$' */

        /* `$$` is how a recipe gets a dollar of its own through to the
         * command line. */
        if (*in == '$') {
            if (append_text(out, size, &len, "$") < 0) {
                expand_depth--;
                return -1;
            }
            in++;
            continue;
        }

        /* The automatic variables: what this rule is building, the first
         * thing it is built from, and all of them. */
        if (*in == '@' || *in == '<' || *in == '^') {
            char which = *in++;

            if (!ctx) {
                complain("$@, $< and $^ mean nothing outside a recipe");
                expand_depth--;
                return -1;
            }

            if (which == '@') {
                if (append_text(out, size, &len, ctx->target) < 0)
                    goto fail;
            } else if (which == '<') {
                if (ctx->prereq_count > 0 &&
                    append_text(out, size, &len, ctx->prereq[0]) < 0)
                    goto fail;
            } else {
                for (int i = 0; i < ctx->prereq_count; i++) {
                    if (i && append_text(out, size, &len, " ") < 0)
                        goto fail;
                    if (append_text(out, size, &len, ctx->prereq[i]) < 0)
                        goto fail;
                }
            }
            continue;
        }

        if (*in != '(' && *in != '{') {
            complain("a '$' here has to be followed by (, {, $, @, < or ^");
            expand_depth--;
            return -1;
        }

        char close = (*in == '(') ? ')' : '}';
        in++;

        char name[128];
        int  n = 0;
        while (*in && *in != close) {
            if (n + 1 >= (int)sizeof(name)) {
                complain("this variable name is far too long");
                expand_depth--;
                return -1;
            }
            name[n++] = *in++;
        }
        name[n] = '\0';

        if (*in != close) {
            complain("a variable reference is missing its closing bracket");
            expand_depth--;
            return -1;
        }
        in++;

        var_t *v = var_lookup(name);
        if (!v)
            continue;               /* unset: expands to nothing, as in make */

        if (v->recursive) {
            /* Kept as text, so it is expanded now -- which is what lets a
             * variable be defined before the ones it mentions. */
            char inner[MAKE_EXPAND_MAX];
            if (expand(v->value, ctx, inner, sizeof(inner)) < 0)
                goto fail;
            if (append_text(out, size, &len, inner) < 0)
                goto fail;
        } else if (append_text(out, size, &len, v->value) < 0) {
            goto fail;
        }
    }

    expand_depth--;
    return 0;

fail:
    expand_depth--;
    return -1;
}

/* ------------------------------------------------------------------ *
 *  Rules
 * ------------------------------------------------------------------ */

rule_t *rule_find(const char *target)
{
    for (int i = 0; i < rule_count; i++)
        if (strcmp(rules[i].target, target) == 0)
            return &rules[i];
    return NULL;
}

static rule_t *rule_add(const char *target)
{
    rule_t *r = rule_find(target);

    /* A target named twice takes the second set of prerequisites and recipe.
     * Real make merges prerequisites and warns about the second recipe; this
     * says so and stops, because in a Makefile of this size it is a mistake
     * rather than a technique. */
    if (r) {
        complain("this target already has a rule");
        return NULL;
    }

    if (rule_count == MAX_RULES) {
        complain("too many rules");
        return NULL;
    }

    r = &rules[rule_count++];
    memset(r, 0, sizeof(*r));
    r->target = xstrdup(target);
    return r;
}

/* Split `list` on whitespace, in place: each word is NUL-terminated where its
 * separator was and a pointer to it goes in `out`.  Returns how many, or -1 if
 * there are more than `max`. */
static int split_words(char *list, char **out, int max)
{
    int count = 0;
    char *p = list;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        if (count == max)
            return -1;

        out[count++] = p;

        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
    }

    return count;
}

/* Split `list` on whitespace, adding each word as a prerequisite. */
static int add_prereqs(rule_t *r, char *list)
{
    char *word[MAX_PREREQS];
    int   count = split_words(list, word, MAX_PREREQS);

    if (count < 0) {
        complain("too many prerequisites in one rule");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (r->prereq_count == MAX_PREREQS) {
            complain("too many prerequisites in one rule");
            return -1;
        }
        r->prereq[r->prereq_count++] = xstrdup(word[i]);
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Lines
 * ------------------------------------------------------------------ */

static void strip_comment(char *line)
{
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
}

static void strip_trailing_space(char *line)
{
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r'))
        line[--n] = '\0';
}

/* An assignment is a name, then `=`, `:=` or `+=`.  A rule is a name, then a
 * plain `:`.  Telling them apart is a matter of which comes first, since a
 * target may contain no '=' and a variable name no ':'. */
static int is_assignment(const char *line, int *immediate, int *append,
                         int *name_len, const char **value)
{
    const char *p = line;

    while (*p && *p != '=' && *p != ':')
        p++;

    if (*p == ':' && p[1] != '=')
        return 0;                                   /* a rule */
    if (!*p)
        return 0;                                   /* neither */

    *immediate = (*p == ':');
    *append    = (p > line && p[-1] == '+');
    *name_len  = (int)(p - line) - (*append ? 1 : 0);
    *value     = p + (*immediate ? 2 : 1);

    while (**value == ' ' || **value == '\t')
        (*value)++;

    /* `= x` with nothing before it is not an assignment to anything. */
    while (*name_len > 0 && (line[*name_len - 1] == ' ' ||
                             line[*name_len - 1] == '\t'))
        (*name_len)--;

    return *name_len > 0;
}

/* Read the whole file. Returns a NUL-terminated buffer to free, or NULL. */
static char *slurp(const char *path)
{
    wstat_t st;
    int r = wstat(path, &st);

    if (r < 0) {
        wfprintf(W_STDERR, "make: %s: %s\n", path, wstrerror(-r));
        return NULL;
    }

    int fd = wopen(path, W_O_RDONLY);
    if (fd < 0) {
        wfprintf(W_STDERR, "make: %s: %s\n", path, wstrerror(-fd));
        return NULL;
    }

    char *text = malloc(st.size + 1);
    if (!text) {
        wfprintf(W_STDERR, "make: out of memory reading %s\n", path);
        wclose(fd);
        return NULL;
    }

    wsize_t got = 0;
    while (got < st.size) {
        int n = wread(fd, text + got, st.size - got);
        if (n <= 0)
            break;
        got += (wsize_t)n;
    }
    wclose(fd);

    text[got] = '\0';
    return text;
}

/* Take the next logical line out of `*at`: one text line, with a trailing
 * backslash joining it to the one after.  Returns 0 at the end of the file. */
static int next_line(char **at, char *out, int size)
{
    if (!**at)
        return 0;

    int len = 0;

    for (;;) {
        char *p   = *at;
        char *end = strchr(p, '\n');

        /* The last line of a file that does not end in a newline is still a
         * line, and ends where the text does. */
        if (!end)
            end = p + strlen(p);

        *at = *end ? end + 1 : end;

        current_line++;

        int n = (int)(end - p);
        int joins = (n > 0 && p[n - 1] == '\\');
        if (joins)
            n--;                                    /* drop the backslash */

        if (len + n >= size) {
            complain("this line is too long");
            return -1;
        }

        memcpy(out + len, p, (wsize_t)n);
        len += n;
        out[len] = '\0';

        if (!joins || !**at)
            return 1;

        /* Continuation lines are joined by a space, the way make does it, so
         * a list of prerequisites split over three lines is still a list. */
        if (len + 1 < size)
            out[len++] = ' ';
        out[len] = '\0';
    }
}

int makefile_read(const char *path)
{
    char *text = slurp(path);
    if (!text)
        return -1;

    char *at = text;
    char  line[MAKE_LINE_MAX];

    /* The rules the last `target:` line named: a recipe line belongs to all of
     * them, which is what makes `a b: c` two rules with one recipe. */
    rule_t *current[MAX_TARGETS];
    int     current_count = 0;

    current_file = path;
    current_line = 0;

    int status = 0;

    for (;;) {
        int got = next_line(&at, line, sizeof(line));
        if (got < 0) {
            status = -1;
            break;
        }
        if (got == 0)
            break;

        /* A tab starts a recipe line, and only where a rule is open.  The rest
         * of the line goes through untouched -- no comment stripping, because
         * a '#' in a recipe belongs to the command, not to the Makefile. */
        if (line[0] == '\t') {
            if (current_count == 0) {
                complain("a recipe line with no rule above it");
                status = -1;
                break;
            }

            const char *body = line + 1;
            while (*body == ' ' || *body == '\t')
                body++;
            if (!*body)
                continue;                       /* a tab and nothing else */

            for (int i = 0; i < current_count; i++) {
                rule_t *r = current[i];

                if (r->recipe_count == MAX_RECIPE_LINES) {
                    complain("too many commands in one recipe");
                    status = -1;
                    break;
                }
                r->recipe[r->recipe_count++] = xstrdup(body);
            }
            if (status < 0)
                break;
            continue;
        }

        strip_comment(line);
        strip_trailing_space(line);

        char *start = line;
        while (*start == ' ')
            start++;

        if (!*start)
            continue;                           /* blank, or all comment */

        /* Anything that is not a recipe closes the rule above it. */
        current_count = 0;

        int immediate = 0, append = 0, name_len = 0;
        const char *value = NULL;

        if (is_assignment(start, &immediate, &append, &name_len, &value)) {
            char name[128];

            if (name_len >= (int)sizeof(name)) {
                complain("this variable name is far too long");
                status = -1;
                break;
            }
            memcpy(name, start, (wsize_t)name_len);
            name[name_len] = '\0';

            if (var_set(name, value, immediate, append, 0) < 0) {
                status = -1;
                break;
            }
            continue;
        }

        char *colon = strchr(start, ':');
        if (!colon) {
            complain("this is neither an assignment nor a rule");
            status = -1;
            break;
        }

        *colon = '\0';

        /* The target side is expanded too, so `$(NAME).o: ...` works. */
        char targets[MAKE_EXPAND_MAX];
        char prereqs[MAKE_EXPAND_MAX];

        if (expand(start, NULL, targets, sizeof(targets)) < 0 ||
            expand(colon + 1, NULL, prereqs, sizeof(prereqs)) < 0) {
            status = -1;
            break;
        }

        char *named_target[MAX_TARGETS];
        int   target_count = split_words(targets, named_target, MAX_TARGETS);

        if (target_count < 0) {
            complain("too many targets on one line");
            status = -1;
            break;
        }
        if (target_count == 0) {
            complain("a rule with nothing before the colon");
            status = -1;
            break;
        }

        /* `.PHONY: all clean` names targets to build whatever the times say,
         * rather than describing one of its own. */
        if (strcmp(named_target[0], ".PHONY") == 0) {
            char *named[MAX_PREREQS];
            int   count = split_words(prereqs, named, MAX_PREREQS);

            if (count < 0) {
                complain("too many targets named in one .PHONY");
                status = -1;
                break;
            }

            for (int i = 0; i < count; i++) {
                rule_t *r = rule_find(named[i]);

                /* The rule itself may not have been read yet, so an empty one
                 * is made here and filled in when its own line arrives. */
                if (!r) {
                    r = rule_add(named[i]);
                    if (!r) {
                        status = -1;
                        break;
                    }
                }
                r->phony = 1;
            }
            if (status < 0)
                break;
            continue;
        }

        /* One rule per target named, all sharing what follows. */
        for (int i = 0; i < target_count; i++) {
            rule_t *r = rule_find(named_target[i]);

            /* An empty rule made earlier by .PHONY is this rule, not a clash
             * with it. */
            if (!r || r->prereq_count || r->recipe_count || !r->phony)
                r = rule_add(named_target[i]);

            if (!r) {
                status = -1;
                break;
            }
            current[current_count++] = r;

            /* The first target of the first rule is what `make` alone builds.
             * Names beginning with a dot are make's own -- .PHONY and the like
             * -- and are never the goal. */
            if (!default_goal && r->target[0] != '.')
                default_goal = r->target;
        }
        if (status < 0)
            break;

        for (int i = 0; i < current_count; i++) {
            char copy[MAKE_EXPAND_MAX];
            strlcpy(copy, prereqs, sizeof(copy));

            if (add_prereqs(current[i], copy) < 0) {
                status = -1;
                break;
            }
        }
        if (status < 0)
            break;
    }

    free(text);
    return status;
}
