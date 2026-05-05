/* chsh -- change a user's login shell, the one started for them at boot or by
 * su.  After Unix's chsh.
 *
 *   chsh                     show the current shell and the choices
 *   chsh <shell>             set your own login shell
 *   chsh -u <user> <shell>   set another user's (root or usereditor only)
 *
 * A shell is named either by app -- `whell`, `fish` -- which becomes
 * /app/<name>/launch, or by an explicit path.  The change takes effect the
 * next time a shell starts for that user: their next su, or the next boot for
 * root.
 */

#include <wkernel.h>

/* Turn a shell argument into a launch path: a bare name maps to
 * /app/<name>/launch, a path is taken as given. */
static void resolve(const char *arg, char *out, int size)
{
    if (strchr(arg, '/'))
        strlcpy(out, arg, (wsize_t)size);
    else
        wsnprintf(out, (wsize_t)size, "/app/%s/launch", arg);
}

/* Print the caller's current shell, and the shells that are installed. */
static void show(void)
{
    char shell[W_SHELL_MAX + 1];
    if (wgetshell(-1, shell, sizeof(shell)) == 0)
        wprintf("Current shell: %s\n", shell);

    static const char *const known[] = { "whell", "fish" };
    wprintf("\nAvailable shells (chsh <name>):\n");
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        char path[W_SHELL_MAX + 1];
        wstat_t st;
        resolve(known[i], path, sizeof(path));
        if (wstat(path, &st) == 0 && st.type == W_FT_FILE)
            wprintf("  %-8s %s\n", known[i], path);
    }
}

int main(int argc, char **argv)
{
    const char *user = NULL;
    const char *want = NULL;

    if (argc == 1) {
        show();
        return 0;
    }

    if (strcmp(argv[1], "-u") == 0) {
        if (argc != 4) {
            wfprintf(W_STDERR, "usage: chsh -u <user> <shell>\n");
            return 2;
        }
        user = argv[2];
        want = argv[3];
    } else {
        if (argc != 2) {
            wfprintf(W_STDERR, "usage: chsh [<shell>] | chsh -u <user> <shell>\n");
            return 2;
        }
        want = argv[1];
    }

    /* Default the target to the current user. */
    wuser_t me;
    if (wuserinfo(-1, &me) < 0) {
        wfprintf(W_STDERR, "chsh: cannot identify the current user\n");
        return 1;
    }
    if (!user)
        user = me.name;

    char path[W_SHELL_MAX + 1];
    resolve(want, path, sizeof(path));

    /* Refuse a shell that is not actually there, the way chsh checks
     * /etc/shells -- a bad login shell would leave the account unusable. */
    wstat_t st;
    if (wstat(path, &st) < 0 || st.type != W_FT_FILE) {
        wfprintf(W_STDERR, "chsh: %s is not an executable\n", path);
        return 1;
    }

    int r = wsetshell(user, path);
    if (r < 0) {
        if (-r == W_EPERM)
            wfprintf(W_STDERR, "chsh: you may not change %s's shell\n", user);
        else if (-r == W_ENOENT)
            wfprintf(W_STDERR, "chsh: no such user: %s\n", user);
        else
            wfprintf(W_STDERR, "chsh: %s\n", wstrerror(-r));
        return 1;
    }

    wprintf("Shell for %s is now %s.\n", user, path);
    wprintf("It takes effect the next time a shell starts for %s"
            " (su, or the next boot).\n", user);
    return 0;
}
