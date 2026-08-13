/* The preprocessor: #include, #define, #if, and macro expansion.
 *
 * It works on the token list the lexer produced and hands back another one:
 * no directives, no macro names, no newlines.  Working on lists rather than on
 * text is what keeps it small -- expanding a macro is splicing a copy of its
 * body into the input, and rescanning is simply carrying on from there.
 *
 * The one subtlety is recursion.  `#define f f(x)` must expand once and stop,
 * so every token that came out of a macro remembers which macros it came out
 * of, and a name is not expanded again inside its own expansion.  That list is
 * the hide set, and it is why the rule is a property of tokens rather than of
 * a stack.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcc.h"

struct macro_set {
    struct macro_set *next;
    const char       *name;
};

typedef struct macro {
    struct macro *next;
    const char   *name;
    int           is_function;
    int           param_count;
    char        **params;
    int           is_variadic;    /* the last parameter is ...          */
    token_t      *body;
} macro_t;

static macro_t *macros;

#define MAX_INCLUDE_PATHS 16
static const char *include_paths[MAX_INCLUDE_PATHS];
static int         include_path_count;

/* How deep #include may nest.  A header that includes itself without a guard
 * would otherwise be an out-of-memory rather than a message. */
#define MAX_INCLUDE_DEPTH 32
static int include_depth;

void pp_add_include_path(const char *path)
{
    if (include_path_count == MAX_INCLUDE_PATHS)
        fatal("too many -I paths");

    include_paths[include_path_count++] = path;
}

/* ------------------------------------------------------------------ *
 *  Token list helpers
 * ------------------------------------------------------------------ */

static token_t *copy_token(const token_t *t)
{
    token_t *copy = wcc_alloc(sizeof(*copy));

    *copy = *t;
    copy->next = NULL;
    return copy;
}

/* A copy of a whole list, so a macro body can be spliced without the original
 * being consumed. */
static token_t *copy_list(const token_t *t)
{
    token_t  head;
    token_t *tail = &head;

    head.next = NULL;
    for (; t; t = t->next)
        tail = tail->next = copy_token(t);

    return head.next;
}

static token_t *list_end(token_t *t)
{
    while (t && t->next)
        t = t->next;
    return t;
}

static int in_hideset(const token_t *t, const char *name)
{
    for (struct macro_set *h = t->hidden; h; h = h->next)
        if (strcmp(h->name, name) == 0)
            return 1;
    return 0;
}

static void add_hideset(token_t *list, const char *name)
{
    for (token_t *t = list; t; t = t->next) {
        struct macro_set *h = wcc_alloc(sizeof(*h));
        h->name   = name;
        h->next   = t->hidden;
        t->hidden = h;
    }
}

static macro_t *find_macro(const token_t *t)
{
    if (t->kind != TK_IDENT)
        return NULL;

    for (macro_t *m = macros; m; m = m->next)
        if ((int)strlen(m->name) == t->len &&
            memcmp(m->name, t->text, (size_t)t->len) == 0)
            return m;

    return NULL;
}

static void undefine(const char *name)
{
    macro_t **link = &macros;

    while (*link) {
        if (strcmp((*link)->name, name) == 0) {
            *link = (*link)->next;
            return;
        }
        link = &(*link)->next;
    }
}

static macro_t *define_macro(const char *name, token_t *body)
{
    macro_t *m = wcc_alloc(sizeof(*m));

    undefine(name);
    m->name = name;
    m->body = body;
    m->next = macros;
    macros  = m;
    return m;
}

/* ------------------------------------------------------------------ *
 *  Files
 * ------------------------------------------------------------------ */

char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *text = wcc_alloc((size_t)size + 2);
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);

    text[got] = '\0';

    /* A file that does not end in a newline still ends a directive, and every
     * loop here is simpler if it can assume one. */
    if (got > 0 && text[got - 1] != '\n') {
        text[got]     = '\n';
        text[got + 1] = '\0';
    }

    return text;
}

static char *join_path(const char *dir, const char *name)
{
    size_t dir_len = strlen(dir);
    char  *path    = wcc_alloc(dir_len + strlen(name) + 2);

    strcpy(path, dir);
    if (dir_len && dir[dir_len - 1] != '/')
        strcat(path, "/");
    strcat(path, name);
    return path;
}

/* The directory a file is in, for a "quoted" include. */
static char *directory_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (!slash)
        return wcc_strdup(".");

    return wcc_strndup(path, (size_t)(slash - path));
}

/* ------------------------------------------------------------------ *
 *  Directives
 * ------------------------------------------------------------------ */

static token_t *expand_list(token_t *tokens);

/* Everything up to the end of the line, unlinked from what follows. */
static token_t *take_line(token_t **at)
{
    token_t  head;
    token_t *tail = &head;
    token_t *t    = *at;

    head.next = NULL;
    while (t && t->kind != TK_NEWLINE && t->kind != TK_EOF) {
        tail = tail->next = copy_token(t);
        t = t->next;
    }

    *at = t;
    return head.next;
}

static void skip_line(token_t **at)
{
    while (*at && (*at)->kind != TK_NEWLINE && (*at)->kind != TK_EOF)
        *at = (*at)->next;
}

/* ---- #if, and the constant expression it takes ---- */

/* A tiny recursive-descent evaluator over an already-expanded token list.
 * Integers only, which is all #if has ever been able to see. */
typedef struct {
    token_t    *at;
    const char *file;
    int         line;
} eval_t;

static long eval_conditional(eval_t *e);

static int eval_punct(eval_t *e, const char *op)
{
    if (e->at && e->at->kind == TK_PUNCT && token_is(e->at, op)) {
        e->at = e->at->next;
        return 1;
    }
    return 0;
}

static long eval_primary(eval_t *e)
{
    if (!e->at)
        error_at(e->file, e->line, "the #if expression ends too early");

    if (eval_punct(e, "(")) {
        long value = eval_conditional(e);
        if (!eval_punct(e, ")"))
            error_at(e->file, e->line, "missing ')' in the #if expression");
        return value;
    }

    if (e->at->kind == TK_NUMBER || e->at->kind == TK_CHAR) {
        long value = e->at->value;
        e->at = e->at->next;
        return value;
    }

    /* Any identifier left after expansion is not defined, and the standard
     * says an undefined name in a #if is zero. */
    if (e->at->kind == TK_IDENT) {
        e->at = e->at->next;
        return 0;
    }

    error_at(e->file, e->line, "this cannot appear in a #if expression");
    return 0;
}

static long eval_unary(eval_t *e)
{
    if (eval_punct(e, "!"))  return !eval_unary(e);
    if (eval_punct(e, "-"))  return -eval_unary(e);
    if (eval_punct(e, "+"))  return  eval_unary(e);
    if (eval_punct(e, "~"))  return ~eval_unary(e);

    return eval_primary(e);
}

static long eval_mul(eval_t *e)
{
    long value = eval_unary(e);

    for (;;) {
        if (eval_punct(e, "*"))      value *= eval_unary(e);
        else if (eval_punct(e, "/")) { long d = eval_unary(e);
                                       value = d ? value / d : 0; }
        else if (eval_punct(e, "%")) { long d = eval_unary(e);
                                       value = d ? value % d : 0; }
        else return value;
    }
}

static long eval_add(eval_t *e)
{
    long value = eval_mul(e);

    for (;;) {
        if (eval_punct(e, "+"))      value += eval_mul(e);
        else if (eval_punct(e, "-")) value -= eval_mul(e);
        else return value;
    }
}

static long eval_shift(eval_t *e)
{
    long value = eval_add(e);

    for (;;) {
        if (eval_punct(e, "<<"))      value <<= eval_add(e);
        else if (eval_punct(e, ">>")) value >>= eval_add(e);
        else return value;
    }
}

static long eval_relational(eval_t *e)
{
    long value = eval_shift(e);

    for (;;) {
        if (eval_punct(e, "<="))      value = value <= eval_shift(e);
        else if (eval_punct(e, ">=")) value = value >= eval_shift(e);
        else if (eval_punct(e, "<"))  value = value <  eval_shift(e);
        else if (eval_punct(e, ">"))  value = value >  eval_shift(e);
        else return value;
    }
}

static long eval_equality(eval_t *e)
{
    long value = eval_relational(e);

    for (;;) {
        if (eval_punct(e, "=="))      value = value == eval_relational(e);
        else if (eval_punct(e, "!=")) value = value != eval_relational(e);
        else return value;
    }
}

static long eval_bitand(eval_t *e)
{
    long value = eval_equality(e);
    while (e->at && token_is(e->at, "&") && e->at->kind == TK_PUNCT) {
        e->at = e->at->next;
        value &= eval_equality(e);
    }
    return value;
}

static long eval_bitxor(eval_t *e)
{
    long value = eval_bitand(e);
    while (eval_punct(e, "^"))
        value ^= eval_bitand(e);
    return value;
}

static long eval_bitor(eval_t *e)
{
    long value = eval_bitxor(e);
    while (eval_punct(e, "|"))
        value |= eval_bitxor(e);
    return value;
}

static long eval_and(eval_t *e)
{
    long value = eval_bitor(e);

    /* Both sides are evaluated whatever the left one said: there is nothing
     * with an effect in a #if, and a parser that stopped early would leave
     * the tokens it skipped for the next operator to trip over. */
    while (eval_punct(e, "&&")) {
        long right = eval_bitor(e);
        value = value && right;
    }
    return value;
}

static long eval_or(eval_t *e)
{
    long value = eval_and(e);

    while (eval_punct(e, "||")) {
        long right = eval_and(e);
        value = value || right;
    }
    return value;
}

static long eval_conditional(eval_t *e)
{
    long value = eval_or(e);

    if (eval_punct(e, "?")) {
        long then_value = eval_conditional(e);
        if (!eval_punct(e, ":"))
            error_at(e->file, e->line, "missing ':' in the #if expression");
        long else_value = eval_conditional(e);
        return value ? then_value : else_value;
    }

    return value;
}

/* `defined X` and `defined(X)` are replaced before anything is expanded --
 * otherwise the name would be expanded first and there would be nothing left
 * to ask about. */
static token_t *replace_defined(token_t *line)
{
    token_t  head;
    token_t *tail = &head;

    head.next = NULL;

    for (token_t *t = line; t; ) {
        if (t->kind == TK_IDENT && token_is(t, "defined")) {
            const token_t *at = t;
            t = t->next;

            int parens = 0;
            if (t && token_is(t, "(")) {
                parens = 1;
                t = t->next;
            }
            if (!t || t->kind != TK_IDENT)
                error_at(at->file, at->line, "defined needs a name");

            int defined = find_macro(t) != NULL;
            t = t->next;

            if (parens) {
                if (!t || !token_is(t, ")"))
                    error_at(at->file, at->line, "defined( needs a ')'");
                t = t->next;
            }

            token_t *num = wcc_alloc(sizeof(*num));
            num->kind  = TK_NUMBER;
            num->value = defined;
            num->text  = defined ? "1" : "0";
            num->len   = 1;
            num->file  = at->file;
            num->line  = at->line;

            tail = tail->next = num;
            continue;
        }

        tail = tail->next = copy_token(t);
        t = t->next;
    }

    return head.next;
}

static long eval_condition(token_t *line, const char *file, int line_number)
{
    token_t *expanded = expand_list(replace_defined(line));

    eval_t e = { expanded, file, line_number };
    long value = eval_conditional(&e);

    if (e.at)
        error_at(file, line_number, "trailing tokens in the #if expression");

    return value;
}

/* ------------------------------------------------------------------ *
 *  Macro expansion
 * ------------------------------------------------------------------ */

/* One argument of a function-like macro invocation. */
typedef struct {
    token_t *tokens;
} macro_arg_t;

/* Read the arguments of a function-like macro.  `*at` is on the '(' and is
 * left just past the ')'. */
static macro_arg_t *read_args(macro_t *m, token_t **at, const token_t *site)
{
    macro_arg_t *args = wcc_alloc(sizeof(*args) * (size_t)(m->param_count + 1));
    token_t     *t    = (*at)->next;          /* past the '(' */
    int          n    = 0;

    /* A macro of no parameters, called with none: `f()`. */
    if (m->param_count == 0 && token_is(t, ")")) {
        *at = t->next;
        return args;
    }

    for (;;) {
        token_t  head;
        token_t *tail  = &head;
        int      depth = 0;

        head.next = NULL;

        for (;;) {
            if (!t || t->kind == TK_EOF)
                error_at(site->file, site->line,
                         "unterminated argument list for macro %s", m->name);

            if (depth == 0 && token_is(t, ")"))
                break;

            /* The last parameter of a variadic macro swallows the commas. */
            if (depth == 0 && token_is(t, ",") &&
                !(m->is_variadic && n == m->param_count - 1))
                break;

            if (token_is(t, "("))
                depth++;
            else if (token_is(t, ")"))
                depth--;

            if (t->kind != TK_NEWLINE)
                tail = tail->next = copy_token(t);
            t = t->next;
        }

        if (n < m->param_count)
            args[n].tokens = head.next;
        n++;

        if (token_is(t, ",")) {
            t = t->next;
            continue;
        }
        break;
    }

    if (!token_is(t, ")"))
        error_at(site->file, site->line, "expected ')' after the arguments");

    /* Fewer arguments than parameters is allowed only where the missing one
     * is a variadic tail, which is then empty. */
    if (n != m->param_count &&
        !(m->is_variadic && n == m->param_count - 1))
        error_at(site->file, site->line,
                 "macro %s takes %d argument%s, %d given",
                 m->name, m->param_count,
                 m->param_count == 1 ? "" : "s", n);

    *at = t->next;
    return args;
}

static int param_index(macro_t *m, const token_t *t)
{
    if (t->kind != TK_IDENT)
        return -1;

    for (int i = 0; i < m->param_count; i++)
        if ((int)strlen(m->params[i]) == t->len &&
            memcmp(m->params[i], t->text, (size_t)t->len) == 0)
            return i;

    return -1;
}

/* #x -- the argument's spelling, as a string literal. */
static token_t *stringify(const token_t *arg, const token_t *site)
{
    buffer_t text;

    buf_init(&text);
    for (const token_t *t = arg; t; t = t->next) {
        if (t != arg && t->has_space)
            buf_byte(&text, ' ');

        /* A string inside a string needs its quotes and backslashes escaped
         * again, which is the whole difficulty of this operator. */
        if (t->kind == TK_STRING) {
            for (int i = 0; i < t->len; i++) {
                if (t->text[i] == '"' || t->text[i] == '\\')
                    buf_byte(&text, '\\');
                buf_byte(&text, t->text[i]);
            }
        } else {
            buf_put(&text, t->text, t->len);
        }
    }
    buf_byte(&text, '\0');

    token_t *s = wcc_alloc(sizeof(*s));
    s->kind       = TK_STRING;
    s->string     = (char *)text.data;
    s->string_len = text.len;
    s->text       = s->string;
    s->len        = text.len - 1;
    s->file       = site->file;
    s->line       = site->line;
    return s;
}

/* a ## b -- the two spellings joined and read again as one token. */
static token_t *paste(const token_t *left, const token_t *right)
{
    buffer_t text;

    buf_init(&text);
    buf_put(&text, left->text, left->len);
    buf_put(&text, right->text, right->len);
    buf_byte(&text, '\n');
    buf_byte(&text, '\0');

    token_t *joined = lex(left->file, (char *)text.data);
    if (!joined || joined->kind == TK_EOF)
        error_at(left->file, left->line, "## produced nothing");

    token_t *result = copy_token(joined);
    result->line = left->line;
    result->file = left->file;
    return result;
}

/* Put the arguments into a copy of the body, handling # and ##. */
static token_t *substitute(macro_t *m, macro_arg_t *args, const token_t *site)
{
    token_t  head;
    token_t *tail = &head;

    head.next = NULL;

    for (token_t *t = m->body; t; ) {
        /* # parameter */
        if (token_is(t, "#") && t->next && param_index(m, t->next) >= 0) {
            token_t *s = stringify(args[param_index(m, t->next)].tokens, site);
            s->has_space = t->has_space;
            tail = tail->next = s;
            t = t->next->next;
            continue;
        }

        /* something ## something */
        if (t->next && token_is(t->next, "##") && t->next->next) {
            token_t *left_arg  = NULL;
            token_t *right;
            int      li = param_index(m, t);
            int      ri = param_index(m, t->next->next);

            left_arg = (li >= 0) ? copy_list(args[li].tokens) : copy_token(t);
            right    = (ri >= 0) ? copy_list(args[ri].tokens)
                                 : copy_token(t->next->next);

            /* An empty argument on either side leaves the other alone, which
             * is what makes `x ## __VA_ARGS__` work with no arguments. */
            if (!left_arg) {
                if (right) {
                    tail->next = right;
                    tail = list_end(right);
                }
            } else if (!right) {
                tail->next = left_arg;
                tail = list_end(left_arg);
            } else {
                token_t *last = list_end(left_arg);
                token_t *joined = paste(last, right);

                /* Everything but the last token of the left side comes
                 * through untouched; the last one is joined. */
                if (left_arg != last) {
                    tail->next = left_arg;
                    for (token_t *q = left_arg; q; q = q->next)
                        if (q->next == last) {
                            q->next = NULL;
                            break;
                        }
                    tail = list_end(left_arg);
                }

                tail = tail->next = joined;
                if (right->next) {
                    tail->next = right->next;
                    tail = list_end(right->next);
                }
            }

            t = t->next->next->next;
            continue;
        }

        int index = param_index(m, t);
        if (index >= 0) {
            /* An argument is expanded before it is put in, unless it was
             * stringified or pasted -- which the two cases above took. */
            token_t *value = expand_list(copy_list(args[index].tokens));

            if (value) {
                value->has_space = t->has_space;
                tail->next = value;
                tail = list_end(value);
            }
            t = t->next;
            continue;
        }

        tail = tail->next = copy_token(t);
        t = t->next;
    }

    return head.next;
}

/* Expand every macro in a list, and in what those expansions produce. */
static token_t *expand_list(token_t *tokens)
{
    token_t  head;
    token_t *tail = &head;

    head.next = NULL;

    while (tokens) {
        if (tokens->kind != TK_IDENT) {
            token_t *t = tokens;
            tokens = tokens->next;
            t->next = NULL;
            tail = tail->next = t;
            continue;
        }

        macro_t *m = find_macro(tokens);

        if (!m || in_hideset(tokens, m->name)) {
            token_t *t = tokens;
            tokens = tokens->next;
            t->next = NULL;
            tail = tail->next = t;
            continue;
        }

        if (!m->is_function) {
            token_t *body = copy_list(m->body);

            add_hideset(body, m->name);
            if (body) {
                body->has_space      = tokens->has_space;
                body->at_line_start  = tokens->at_line_start;
                list_end(body)->next = tokens->next;
                tokens = body;
            } else {
                tokens = tokens->next;      /* an empty macro */
            }
            continue;
        }

        /* A function-like macro that is not followed by '(' is not an
         * invocation at all, and stands as an ordinary identifier. */
        token_t *after = tokens->next;
        while (after && after->kind == TK_NEWLINE)
            after = after->next;

        if (!after || !token_is(after, "(")) {
            token_t *t = tokens;
            tokens = tokens->next;
            t->next = NULL;
            tail = tail->next = t;
            continue;
        }

        const token_t *site = tokens;
        token_t       *at   = after;
        macro_arg_t   *args = read_args(m, &at, site);
        token_t       *body = substitute(m, args, site);

        add_hideset(body, m->name);
        if (body) {
            body->has_space = site->has_space;
            list_end(body)->next = at;
            tokens = body;
        } else {
            tokens = at;
        }
    }

    return head.next;
}

/* ------------------------------------------------------------------ *
 *  The directive walk
 * ------------------------------------------------------------------ */

static token_t *preprocess_tokens(token_t *tokens, const char *from_file);

/* #define NAME body  /  #define NAME(a, b) body */
static void do_define(token_t *line)
{
    if (!line || line->kind != TK_IDENT)
        error_at(line ? line->file : "?", line ? line->line : 0,
                 "#define needs a name");

    const char *name = wcc_strndup(line->text, (size_t)line->len);
    token_t    *rest = line->next;

    /* `#define f(x)` is a function-like macro only when the '(' touches the
     * name; `#define f (x)` defines f as the tokens `(x)`. */
    if (rest && token_is(rest, "(") && !rest->has_space) {
        macro_t *m = define_macro(name, NULL);
        char    *params[64];
        int      count = 0;

        m->is_function = 1;
        rest = rest->next;

        while (rest && !token_is(rest, ")")) {
            if (count)
                {
                    if (!token_is(rest, ","))
                        error_at(rest->file, rest->line,
                                 "expected ',' between macro parameters");
                    rest = rest->next;
                }

            if (!rest)
                break;

            if (token_is(rest, "...")) {
                m->is_variadic = 1;
                params[count++] = wcc_strdup("__VA_ARGS__");
                rest = rest->next;
                break;
            }

            if (rest->kind != TK_IDENT)
                error_at(rest->file, rest->line, "bad macro parameter");

            if (count == 64)
                error_at(rest->file, rest->line, "too many macro parameters");

            params[count++] = wcc_strndup(rest->text, (size_t)rest->len);
            rest = rest->next;
        }

        if (!rest || !token_is(rest, ")"))
            error_at(line->file, line->line,
                     "missing ')' in the parameter list of %s", name);

        m->param_count = count;
        m->params      = wcc_alloc(sizeof(char *) * (size_t)(count ? count : 1));
        for (int i = 0; i < count; i++)
            m->params[i] = params[i];

        m->body = rest->next;
        return;
    }

    define_macro(name, rest);
}

/* #include "file" or #include <file>, after macro expansion if it is neither. */
static token_t *do_include(token_t *line, const char *from_file,
                           token_t *rest_of_file)
{
    if (!line)
        error_at(from_file, 0, "#include needs a file");

    char *name = NULL;
    int   angled = 0;

    if (line->kind == TK_STRING) {
        name = wcc_strdup(line->string);
    } else if (token_is(line, "<")) {
        buffer_t text;
        buf_init(&text);

        for (token_t *t = line->next; t && !token_is(t, ">"); t = t->next) {
            if (t->has_space && text.len)
                buf_byte(&text, ' ');
            buf_put(&text, t->text, t->len);
        }
        buf_byte(&text, '\0');

        name   = (char *)text.data;
        angled = 1;
    } else {
        /* `#include SOMETHING`: expand and try again. */
        token_t *expanded = expand_list(copy_list(line));
        if (!expanded || expanded == line)
            error_at(line->file, line->line, "cannot make sense of this #include");
        return do_include(expanded, from_file, rest_of_file);
    }

    char *path = NULL;
    char *text = NULL;

    /* A quoted include starts beside the file that asked for it. */
    if (!angled) {
        path = join_path(directory_of(from_file), name);
        text = read_file(path);
    }

    for (int i = 0; !text && i < include_path_count; i++) {
        path = join_path(include_paths[i], name);
        text = read_file(path);
    }

    if (!text)
        error_at(line->file, line->line, "cannot open %s", name);

    if (++include_depth > MAX_INCLUDE_DEPTH)
        error_at(line->file, line->line,
                 "#include nested more than %d deep at %s",
                 MAX_INCLUDE_DEPTH, name);

    token_t *included = preprocess_tokens(lex(path, text), path);
    include_depth--;

    if (!included)
        return rest_of_file;

    list_end(included)->next = rest_of_file;
    return included;
}

/* Skip to the #else, #elif or #endif that matches the directive we are in. */
static token_t *skip_conditional(token_t *t, int stop_at_else)
{
    int depth = 0;

    while (t && t->kind != TK_EOF) {
        if (t->at_line_start && token_is(t, "#")) {
            token_t *name = t->next;

            if (name && name->kind == TK_IDENT) {
                if (token_is(name, "if") || token_is(name, "ifdef") ||
                    token_is(name, "ifndef")) {
                    depth++;
                } else if (token_is(name, "endif")) {
                    if (depth == 0)
                        return t;
                    depth--;
                } else if (depth == 0 && stop_at_else &&
                           (token_is(name, "else") || token_is(name, "elif"))) {
                    return t;
                }
            }
        }
        t = t->next;
    }

    return t;
}

/* One #if...#endif region, as a stack of what has already been taken. */
typedef struct cond {
    struct cond *next;
    int          taken;      /* some branch of this #if was included */
    int          in_else;
} cond_t;

static token_t *preprocess_tokens(token_t *tokens, const char *from_file)
{
    token_t  head;
    token_t *tail = &head;
    cond_t  *conditions = NULL;

    head.next = NULL;

    while (tokens && tokens->kind != TK_EOF) {
        if (tokens->kind == TK_NEWLINE) {
            tokens = tokens->next;
            continue;
        }

        if (!(tokens->at_line_start && token_is(tokens, "#"))) {
            /* Ordinary text: expand it and take it. */
            token_t *t = tokens;
            token_t *line = NULL;
            token_t *line_tail = NULL;

            /* Gather up to the end of the line so that a macro invocation
             * whose arguments run onto the next line still works: the
             * arguments are read from the input list, not from this run. */
            while (t && t->kind != TK_EOF && t->kind != TK_NEWLINE) {
                token_t *copy = copy_token(t);
                if (line_tail)
                    line_tail = line_tail->next = copy;
                else
                    line = line_tail = copy;
                t = t->next;
            }

            token_t *expanded = expand_list(line);
            if (expanded) {
                tail->next = expanded;
                tail = list_end(expanded);
            }
            tokens = t;
            continue;
        }

        token_t *name = tokens->next;
        token_t *at   = name ? name->next : NULL;

        if (!name || name->kind != TK_IDENT) {
            /* `#` alone on a line is a null directive. */
            skip_line(&tokens);
            continue;
        }

        if (token_is(name, "define")) {
            token_t *line = take_line(&at);
            do_define(line);
            tokens = at;
            continue;
        }

        if (token_is(name, "undef")) {
            if (!at || at->kind != TK_IDENT)
                error_at(name->file, name->line, "#undef needs a name");
            undefine(wcc_strndup(at->text, (size_t)at->len));
            skip_line(&tokens);
            continue;
        }

        if (token_is(name, "include")) {
            token_t *line = take_line(&at);
            tokens = do_include(line, from_file, at);
            continue;
        }

        if (token_is(name, "if") || token_is(name, "ifdef") ||
            token_is(name, "ifndef")) {
            int value;

            if (token_is(name, "if")) {
                token_t *line = take_line(&at);
                value = eval_condition(line, name->file, name->line) != 0;
            } else {
                if (!at || at->kind != TK_IDENT)
                    error_at(name->file, name->line, "%.*s needs a name",
                             name->len, name->text);
                value = find_macro(at) != NULL;
                if (token_is(name, "ifndef"))
                    value = !value;
                skip_line(&at);
            }

            cond_t *c = wcc_alloc(sizeof(*c));
            c->taken = value;
            c->next  = conditions;
            conditions = c;

            tokens = value ? at : skip_conditional(at, 1);
            continue;
        }

        if (token_is(name, "elif")) {
            if (!conditions)
                error_at(name->file, name->line, "#elif without #if");

            if (conditions->taken) {
                tokens = skip_conditional(at, 0);
                continue;
            }

            token_t *line = take_line(&at);
            int value = eval_condition(line, name->file, name->line) != 0;

            conditions->taken = value;
            tokens = value ? at : skip_conditional(at, 1);
            continue;
        }

        if (token_is(name, "else")) {
            if (!conditions)
                error_at(name->file, name->line, "#else without #if");

            conditions->in_else = 1;
            skip_line(&at);
            tokens = conditions->taken ? skip_conditional(at, 0) : at;
            conditions->taken = 1;
            continue;
        }

        if (token_is(name, "endif")) {
            if (!conditions)
                error_at(name->file, name->line, "#endif without #if");

            conditions = conditions->next;
            skip_line(&tokens);
            continue;
        }

        if (token_is(name, "error")) {
            buffer_t text;
            buf_init(&text);
            for (token_t *t = at; t && t->kind != TK_NEWLINE; t = t->next) {
                if (t != at)
                    buf_byte(&text, ' ');
                buf_put(&text, t->text, t->len);
            }
            buf_byte(&text, '\0');
            error_at(name->file, name->line, "#error %s", (char *)text.data);
        }

        if (token_is(name, "warning")) {
            skip_line(&tokens);
            continue;
        }

        if (token_is(name, "pragma") || token_is(name, "line")) {
            /* Nothing here has a pragma worth obeying, and #line only moves
             * the numbers in messages. */
            skip_line(&tokens);
            continue;
        }

        error_at(name->file, name->line, "unknown directive #%.*s",
                 name->len, name->text);
    }

    return head.next;
}

/* ------------------------------------------------------------------ *
 *  What is defined before the first line is read
 * ------------------------------------------------------------------ */

static void define_text(const char *name, const char *value)
{
    char *text = wcc_alloc(strlen(value) + 2);

    strcpy(text, value);
    strcat(text, "\n");

    define_macro(wcc_strdup(name), lex("<built-in>", text));
}

void pp_define_from_argument(const char *text)
{
    const char *equals = strchr(text, '=');

    if (!equals) {
        define_text(text, "1");
        return;
    }

    char *name = wcc_strndup(text, (size_t)(equals - text));
    define_text(name, equals + 1);
}

static void define_builtins(void)
{
    define_text("__STDC__", "1");
    define_text("__STDC_VERSION__", "199901L");
    define_text("__STDC_HOSTED__", "1");
    define_text("__x86_64__", "1");
    define_text("__LP64__", "1");
    define_text("__WOS__", "1");
    define_text("__wcc__", "1");

    /* The types the freestanding headers are written in terms of. */
    define_text("__SIZE_TYPE__", "unsigned long");
    define_text("__PTRDIFF_TYPE__", "long");
    define_text("__INTPTR_TYPE__", "long");
    define_text("__UINTPTR_TYPE__", "unsigned long");
    define_text("__CHAR_BIT__", "8");
}

token_t *preprocess(token_t *tokens)
{
    static int builtins_done;

    if (!builtins_done) {
        define_builtins();
        builtins_done = 1;
    }

    const char *file = tokens ? tokens->file : "<stdin>";
    token_t    *out  = preprocess_tokens(tokens, file);

    /* Every directive walk stops at the end of its own file, so the list that
     * comes back has no end marker on it.  The parser wants one. */
    token_t *end = wcc_alloc(sizeof(*end));
    end->kind = TK_EOF;
    end->file = file;
    end->text = "";
    end->at_line_start = 1;

    if (!out)
        return end;

    list_end(out)->next = end;
    return out;
}
