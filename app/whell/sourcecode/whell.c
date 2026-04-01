/* whell -- the WOS shell.
 *
 * Reads a line, splits it, and either runs a builtin or launches a program.
 *
 * Command lookup follows the WOS layout rather than a PATH variable: every
 * application lives in /app/<name>/ with its executable at
 * /app/<name>/launch, so a bare command name maps straight onto that path.
 * A name containing a '/' is treated as a path and run directly.
 *
 * Line editing (backspace, and Ctrl+C to discard a line) is handled by the
 * kernel's console, which is line buffered like a Linux terminal, so a read
 * here returns a whole line and the shell does no editing of its own.
 */

#include "whell.h"

struct builtin {
    const char *name;
    int (*run)(int argc, char **argv);
};

static const struct builtin builtins[] = {
    { "ls",   cmd_ls   },
    { "cd",   cmd_cd   },
    { "pwd",  cmd_pwd  },
    { "free", cmd_free },
    { "df",   cmd_df   },
    { "ps",   cmd_ps   },
    { "cat",  cmd_cat  },
    { "rm",   cmd_rm   },
    { "mkdir", cmd_mkdir },
    { "touch", cmd_touch },
    { "help", cmd_help },
    { "shutdown", cmd_shutdown },
};

static int should_exit;
static int exit_status;

static void print_prompt(void)
{
    char cwd[W_PATH_MAX + 1];

    if (wgetcwd(cwd, sizeof(cwd)) < 0)
        strlcpy(cwd, "?", sizeof(cwd));

    wprintf("wos:%s$ ", cwd);
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

static int run_command(int argc, char **argv)
{
    if (strcmp(argv[0], "exit") == 0) {
        should_exit = 1;
        exit_status = (argc > 1) ? atoi(argv[1]) : 0;
        return exit_status;
    }

    for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (strcmp(argv[0], builtins[i].name) == 0)
            return builtins[i].run(argc, argv);

    return run_program(argc, argv);
}

int main(int argc, char **argv)
{
    char  line[WHELL_LINE_MAX];
    char *args[WHELL_MAX_ARGS + 1];

    wprintf("\nwhell -- the WOS shell. Type `help` for the builtins.\n\n");

    /* Start where the user's files are, not at the root. */
    wchdir("/home");

    while (!should_exit) {
        print_prompt();

        int len = wgetline(line, sizeof(line));
        if (len < 0) {
            /* The console cannot really reach end of file, so this is a real
             * error; reporting and stopping beats spinning on it. */
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
