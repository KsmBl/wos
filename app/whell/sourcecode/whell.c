/* whell -- the WOS shell.
 *
 * Reads a line, splits it, and either runs a builtin or launches a program.
 *
 * Command lookup follows the WOS layout rather than a PATH variable: every
 * application lives in /app/<name>/ with its executable at
 * /app/<name>/launch, so a bare command name maps straight onto that path.
 * A name containing a '/' is treated as a path and run directly.
 *
 * The shell itself is small on purpose: only cd, exit and help are builtins,
 * because only those change state belonging to the shell process.  ls, cat,
 * free and the rest are ordinary programs under /app.
 *
 * Line editing happens here rather than in the kernel, because Tab has to be
 * seen as a keystroke rather than buffered into a line.
 */

#include "whell.h"

struct builtin {
    const char *name;
    int (*run)(int argc, char **argv);
};

static int cmd_exit(int argc, char **argv);

static const struct builtin builtins[] = {
    { "cd",   cmd_cd   },
    { "help", cmd_help },
    { "exit", cmd_exit },
};

static int should_exit;
static int exit_status;

void whell_print_prompt(void)
{
    char cwd[W_PATH_MAX + 1];

    if (wgetcwd(cwd, sizeof(cwd)) < 0)
        strlcpy(cwd, "?", sizeof(cwd));

    wuser_t me;
    const char *who = (wuserinfo(-1, &me) == 0) ? me.name : "?";

    /* Root's prompt ends in '#', everyone else's in '$'. An old convention,
     * and a useful one: it says at a glance that nothing here is protected
     * from you. */
    wprintf("%s@wos:%s%c ", who, cwd,
            (wuserinfo(-1, &me) == 0 && me.uid == W_ROOT_UID) ? '#' : '$');
}

const char *whell_builtin_name(int index)
{
    if (index < 0 || index >= (int)(sizeof(builtins) / sizeof(builtins[0])))
        return NULL;
    return builtins[index].name;
}

/* Append `text` to the line and echo it. */
static void insert_text(char *buf, int *len, int size, const char *text)
{
    for (const char *p = text; *p && *len + 1 < size; p++)
        buf[(*len)++] = *p;

    buf[*len] = '\0';
    wputs(text);
}

/* Tab: extend the line as far as the matches allow, and if that adds nothing
 * because several candidates diverge, show them the way a shell does. */
static void complete_line(char *buf, int *len, int size)
{
    char add[W_PATH_MAX + 2];
    int  matches = whell_complete(buf, *len, add, sizeof(add));

    if (add[0]) {
        insert_text(buf, len, size, add);
        return;
    }

    if (matches < 2)
        return;                 /* nothing matched, or nothing left to add */

    wputs("\n");

    int rows = 0, term_w = W_CONSOLE_WIDTH;
    wconsize(&rows, &term_w);

    int column = 0;
    for (int i = 0; i < matches; i++) {
        const char *name = whell_completion_name(i);
        int width = (int)strlen(name) + (whell_completion_is_dir(i) ? 1 : 0);

        if (column + width + 2 > term_w) {
            wputs("\n");
            column = 0;
        }

        wprintf("%s%s  ", name, whell_completion_is_dir(i) ? "/" : "");
        column += width + 2;
    }

    wputs("\n");
    whell_print_prompt();
    wputs(buf);
}

/* Read one line, echoing and editing it ourselves.
 *
 * This needs the console in raw mode so that Tab arrives as a keystroke
 * instead of being buffered into a line by the kernel.  Raw mode is entered
 * and left around each line rather than held for the shell's lifetime: the
 * mode belongs to the console, not to the process, so leaving it set would
 * change how a child program's own reads behave.
 *
 * Returns the line length, or a negative error. */
static int read_line(char *buf, int size)
{
    int len = 0;

    buf[0] = '\0';
    wconsole_raw(W_CONSOLE_RAW);

    for (;;) {
        char c;
        int  n = wread(W_STDIN, &c, 1);

        if (n < 0) {
            wconsole_raw(W_CONSOLE_CANONICAL);
            return n;
        }
        if (n == 0) {
            /* On the console wread blocks, so a zero-length read means the
             * input stream itself closed -- which happens when the shell runs
             * inside vim's :term and that window is closed.  Treat it as end of
             * input and leave, the way Ctrl+D on an empty line would. */
            wconsole_raw(W_CONSOLE_CANONICAL);
            return WHELL_EOF;
        }

        if (c == '\n' || c == '\r') {
            wputs("\n");
            break;
        }

        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                buf[--len] = '\0';
                /* Back up, blank the character, back up again: correct both
                 * on the VGA console and over a serial terminal. */
                wputs("\b \b");
            }
            continue;
        }

        if (c == 0x03) {        /* Ctrl+C: abandon this line */
            wputs("^C\n");
            len = 0;
            break;
        }

        if (c == '\t') {
            complete_line(buf, &len, size);
            continue;
        }

        if ((unsigned char)c < 32)
            continue;           /* ignore the other control codes */

        if (len + 1 < size) {
            buf[len++] = c;
            buf[len] = '\0';
            wwrite(W_STDOUT, &c, 1);
        }
    }

    wconsole_raw(W_CONSOLE_CANONICAL);
    buf[len] = '\0';
    return len;
}

/* Run an external program and wait for it.  Returns its exit status, or 127
 * if it could not be found -- the same convention a POSIX shell uses. */
static int run_program(int argc, char **argv)
{
    char path[W_PATH_MAX + 1];

    if (strchr(argv[0], '/')) {
        strlcpy(path, argv[0], sizeof(path));
    } else {
        wsnprintf(path, sizeof(path), "/app/%s/launch", argv[0]);
    }

    wstat_t st;
    if (wstat(path, &st) < 0 || st.type != W_FT_FILE) {
        wfprintf(W_STDERR, "whell: %s: command not found\n", argv[0]);
        return 127;
    }

    int pid = wspawn(path, argv);
    if (pid < 0) {
        wfprintf(W_STDERR, "whell: %s: %s\n", argv[0], wstrerror(-pid));
        return 126;
    }

    int status = 0;
    int reaped = wwait(pid, &status);
    if (reaped < 0) {
        wfprintf(W_STDERR, "whell: wait failed: %s\n", wstrerror(-reaped));
        return 1;
    }

    return status;
}

static int cmd_exit(int argc, char **argv)
{
    should_exit = 1;
    exit_status = (argc > 1) ? atoi(argv[1]) : 0;
    return exit_status;
}

static int run_command(int argc, char **argv)
{
    for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (strcmp(argv[0], builtins[i].name) == 0)
            return builtins[i].run(argc, argv);

    return run_program(argc, argv);
}

int main(int argc, char **argv)
{
    char  line[WHELL_LINE_MAX];
    char *args[WHELL_MAX_ARGS + 1];

    /* `whell -c "command"` runs one command and exits, the way any shell
     * does when something wants to hand it a single line. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        strlcpy(line, argv[2], sizeof(line));

        int count = whell_parse(line, args, WHELL_MAX_ARGS + 1);
        if (count == 0)
            return 0;

        return run_command(count, args);
    }

    wprintf("\nwhell -- the WOS shell. Type `help` for an introduction.\n");
    wprintf("Tab completes commands and paths.\n\n");

    /* Start in this user's own directory: the one place they can certainly
     * write. */
    {
        wuser_t me;
        char    home[W_PATH_MAX + 1];

        if (wuserinfo(-1, &me) == 0) {
            wsnprintf(home, sizeof(home), "/home/%s", me.name);
            if (wchdir(home) < 0)
                wchdir("/");
        } else {
            wchdir("/");
        }
    }

    while (!should_exit) {
        whell_print_prompt();

        int len = read_line(line, sizeof(line));
        if (len == WHELL_EOF)
            break;              /* stdin closed (e.g. vim's :term window shut) */
        if (len < 0) {
            /* Otherwise a real read error; reporting and stopping beats
             * spinning on it. */
            wfprintf(W_STDERR, "whell: input error: %s\n", wstrerror(-len));
            return 1;
        }

        int count = whell_parse(line, args, WHELL_MAX_ARGS + 1);
        if (count == 0)
            continue;

        run_command(count, args);
    }

    wprintf("whell: goodbye\n");
    return exit_status;
}
