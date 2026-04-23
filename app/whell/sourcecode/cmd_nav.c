/* The builtins that have to live inside the shell process. */

#include "whell.h"


/* Where `cd -` goes back to. */
static char previous_dir[W_PATH_MAX + 1];
static int  have_previous;

/* cd cannot be a separate program.
 *
 * A child process gets a copy of its parent's working directory, and changing
 * it changes only that copy; when the child exits, the shell is exactly where
 * it started.  So while ls, pwd and the rest are applications under /app, this
 * one has to run inside the shell itself.  Every Unix shell is built this way
 * for the same reason. */
int cmd_cd(int argc, char **argv)
{
    char here[W_PATH_MAX + 1];
    const char *target;

    if (wgetcwd(here, sizeof(here)) < 0)
        here[0] = '\0';

    char home[W_PATH_MAX + 1];

    if (argc < 2) {
        /* Bare `cd` goes to this user's own directory. */
        wuser_t me;
        if (wuserinfo(-1, &me) == 0)
            wsnprintf(home, sizeof(home), "/home/%s", me.name);
        else
            strlcpy(home, "/", sizeof(home));
        target = home;
    } else if (strcmp(argv[1], "-") == 0) {
        if (!have_previous) {
            wfprintf(W_STDERR, "cd: OLDPWD not set\n");
            return 1;
        }
        target = previous_dir;
        /* Like Linux, `cd -` announces where it landed. */
        wprintf("%s\n", previous_dir);
    } else {
        target = argv[1];
    }

    int r = wchdir(target);
    if (r < 0) {
        wfprintf(W_STDERR, "cd: %s: %s\n", target, wstrerror(-r));
        return 1;
    }

    if (here[0]) {
        strlcpy(previous_dir, here, sizeof(previous_dir));
        have_previous = 1;
    }

    return 0;
}

int cmd_help(int argc, char **argv)
{
    wprintf("whell -- the WOS shell\n\n");
    wprintf("Builtins, which must run inside the shell itself:\n");
    wprintf("  cd [path | -]            change directory (no argument: your home)\n");
    wprintf("  exit [status]            leave the shell\n");
    wprintf("  help                     this text\n\n");
    wprintf("Everything else is a program in /app. Typing a bare name runs\n");
    wprintf("/app/<name>/launch, so `ls` runs /app/ls/launch. Press Tab to\n");
    wprintf("see what is installed.\n\n");
    wprintf("Installed alongside the shell: ls, pwd, cat, free, df, ps, rm,\n");
    wprintf("mkdir, touch, clear, shutdown, whoami, passwd, su, adduser,\n");
    wprintf("edituser, ");
    wprintf("fish, vim, htop, fastfetch.\n");
    return 0;
}
