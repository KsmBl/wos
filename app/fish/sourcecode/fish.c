/* fish -- a friendly interactive shell for WOS.
 *
 * A WOS-native shell in the spirit of fish, not a build of the upstream one,
 * which is C++ over a full libc with fork-based job control, termios and
 * PCRE.  What carries over is what fish is actually for: the command is
 * coloured as you type it, the rest of a previous command is offered ahead of
 * the cursor, and Tab completes.
 *
 * Only cd and exit are builtins, plus history and help which describe fish
 * itself.  Everything else is a program under /app, so both shells run the
 * same ls and the same free without either one carrying a copy.
 */

#include "fish.h"

static int should_exit;
static int exit_status;
static int last_status;

/* Split on whitespace, honouring quotes, writing NULs into `line`. */
int fish_parse(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        char *out = p;
        argv[argc++] = out;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote)
                    *out++ = *p++;
                if (*p == quote)
                    p++;
            } else {
                *out++ = *p++;
            }
        }

        int at_end = (*p == '\0');
        if (!at_end)
            p++;
        *out = '\0';

        if (at_end)
            break;
    }

    argv[argc] = NULL;
    return argc;
}

static int builtin_cd(int argc, char **argv)
{
    static char previous[W_PATH_MAX + 1];
    static int  have_previous;

    char here[W_PATH_MAX + 1];
    const char *target;

    if (wgetcwd(here, sizeof(here)) < 0)
        here[0] = '\0';

    char home[W_PATH_MAX + 1];

    if (argc < 2) {
        wuser_t me;
        if (wuserinfo(-1, &me) == 0)
            wsnprintf(home, sizeof(home), "/home/%s", me.name);
        else
            strlcpy(home, "/", sizeof(home));
        target = home;
    } else if (strcmp(argv[1], "-") == 0) {
        if (!have_previous) {
            wfprintf(W_STDERR, "cd: no previous directory\n");
            return 1;
        }
        target = previous;
        wprintf("%s\n", previous);
    } else {
        target = argv[1];
    }

    int r = wchdir(target);
    if (r < 0) {
        wfprintf(W_STDERR, "cd: %s: %s\n", target, wstrerror(-r));
        return 1;
    }

    if (here[0]) {
        strlcpy(previous, here, sizeof(previous));
        have_previous = 1;
    }
    return 0;
}

static int builtin_history(int argc, char **argv)
{
    for (int i = 0; i < history_count; i++)
        wprintf("%5d  %s\n", i + 1, history[i]);
    return 0;
}

static int builtin_help(int argc, char **argv)
{
    wprintf("fish -- a friendly interactive shell for WOS\n\n");
    wprintf("While typing:\n");
    wprintf("  the command turns green when it exists, red when it does not\n");
    wprintf("  a grey suggestion continues the line from your history\n");
    wprintf("  right arrow or End accepts the suggestion\n");
    wprintf("  up/down walk the history, Tab completes\n\n");
    wprintf("Builtins: cd, history, help, exit\n");
    wprintf("Short for: ll = ls -l, la = ls -a\n");
    wprintf("Everything else is a program in /app, so `ls` runs\n");
    wprintf("/app/ls/launch. Press Tab to see what is installed.\n");
    return 0;
}

/* The two abbreviations everyone types.  fish has no alias mechanism to define
 * them with -- and one would be a configuration file, a parser for it and a
 * place to keep it, for two names -- so they are simply known here.
 *
 * Anything else on the line is kept, so `ll /app` is `ls -l /app`. */
static const struct {
    const char *name;
    const char *expands_to;
} aliases[] = {
    { "ll", "-l" },
    { "la", "-a" },
};

/* The alias names, for the editor: it colours a command green when fish knows
 * it and completes on Tab, and both would be wrong about these otherwise. */
const char *fish_alias_name(int i)
{
    if (i < 0 || (unsigned)i >= sizeof(aliases) / sizeof(aliases[0]))
        return NULL;
    return aliases[i].name;
}

/* Rewrite argv in place of an alias, into `out`.  Returns the argv to use,
 * which is the original one when the command was not an alias. */
static char **apply_alias(int *argc, char **argv, char **out)
{
    const char *flag = NULL;

    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (strcmp(argv[0], aliases[i].name) == 0)
            flag = aliases[i].expands_to;

    if (!flag)
        return argv;

    int n = 0;
    out[n++] = (char *)"ls";
    out[n++] = (char *)flag;

    for (int i = 1; i < *argc && n < FISH_MAX_ARGS; i++)
        out[n++] = argv[i];

    out[n] = NULL;
    *argc = n;
    return out;
}

/* Run a program and wait for it. */
static int spawn_and_wait(const char *path, char *const argv[])
{
    int pid = wspawn(path, argv);
    if (pid < 0)
        return -1;

    int status = 0;
    if (wwait(pid, &status) < 0)
        return -1;

    return status;
}

static int run_line(char *line)
{
    /* Parsing writes NULs into the line, so keep a copy for whell. */
    char original[FISH_LINE_MAX];
    char *parsed[FISH_MAX_ARGS + 1];
    char *expanded[FISH_MAX_ARGS + 2];

    strlcpy(original, line, sizeof(original));

    int argc = fish_parse(line, parsed, FISH_MAX_ARGS + 1);
    if (argc == 0)
        return 0;

    char **argv = apply_alias(&argc, parsed, expanded);

    if (strcmp(argv[0], "exit") == 0) {
        should_exit = 1;
        exit_status = (argc > 1) ? atoi(argv[1]) : 0;
        return exit_status;
    }
    if (strcmp(argv[0], "cd") == 0)
        return builtin_cd(argc, argv);
    if (strcmp(argv[0], "history") == 0)
        return builtin_history(argc, argv);
    if (strcmp(argv[0], "help") == 0)
        return builtin_help(argc, argv);

    /* An application, if there is one by that name. */
    char path[W_PATH_MAX + 1];

    if (strchr(argv[0], '/'))
        strlcpy(path, argv[0], sizeof(path));
    else
        wsnprintf(path, sizeof(path), "/app/%s/launch", argv[0]);

    wstat_t st;
    if (wstat(path, &st) == 0 && st.type == W_FT_FILE) {
        int r = spawn_and_wait(path, argv);
        if (r < 0) {
            wfprintf(W_STDERR, "fish: %s: cannot execute\n", argv[0]);
            return 126;
        }
        return r;
    }

    wfprintf(W_STDERR, "fish: %s: command not found\n", argv[0]);
    return 127;
}

int main(int argc, char **argv)
{
    char line[FISH_LINE_MAX];

    wprintf("\n");
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("Welcome to fish, the friendly interactive shell\n");
    wcolor_reset();
    wprintf("Type ");
    wcolor(W_GREEN | W_BRIGHT, W_DEFAULT);
    wprintf("help");
    wcolor_reset();
    wprintf(" for instructions on how to use fish\n\n");

    {
        wuser_t me;
        char    home[W_PATH_MAX + 1];

        if (wuserinfo(-1, &me) == 0) {
            wsnprintf(home, sizeof(home), "/home/%s", me.name);
            if (wchdir(home) < 0)
                wchdir("/");
        }
    }

    while (!should_exit) {
        int len = fish_read_line(line, sizeof(line));

        if (len == -2) {                 /* Ctrl+D on an empty line */
            wprintf("\n");
            break;
        }
        if (len < 0) {
            wfprintf(W_STDERR, "fish: input error\n");
            return 1;
        }
        if (len == 0)
            continue;

        fish_history_add(line);
        last_status = run_line(line);
    }

    return exit_status;
}
