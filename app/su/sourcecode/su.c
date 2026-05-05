/* su -- start a shell as another user.
 *
 * A process can drop to another user but never climb back, so this runs a
 * *new* shell rather than changing the one you are sitting in. Leaving that
 * shell returns you to the one you started from, still as whoever you were.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : "root";

    wuser_t me;
    if (wuserinfo(-1, &me) < 0) {
        wfprintf(W_STDERR, "su: cannot identify the current user\n");
        return 1;
    }

    char password[128];
    password[0] = '\0';

    /* Root is not asked: it could set the password to anything anyway, so
     * demanding it would be theatre rather than security. */
    if (me.uid != W_ROOT_UID) {
        char prompt[64];
        wsnprintf(prompt, sizeof(prompt), "Password for %s: ", target);
        if (wgetpass(prompt, password, sizeof(password)) < 0)
            return 1;
    }

    int r = wlogin(target, password);

    if (r < 0) {
        if (-r == W_ENOENT)
            wfprintf(W_STDERR, "su: %s: no such user\n", target);
        else
            wfprintf(W_STDERR, "su: authentication failure\n");
        return 1;
    }

    /* Start in the new user's home directory, the way a login would. */
    char home[W_PATH_MAX + 1];
    wsnprintf(home, sizeof(home), "/home/%s", target);
    wchdir(home);

    /* Run the user's login shell, so `chsh` decides what su gives them.  We
     * are now that user (wlogin changed our uid), so ask for our own. */
    char shell[W_SHELL_MAX + 1];
    if (wgetshell(-1, shell, sizeof(shell)) < 0)
        strlcpy(shell, "/app/whell/launch", sizeof(shell));

    char *shell_argv[] = { "shell", NULL };
    int pid = wspawn(shell, shell_argv);

    if (pid < 0) {
        wfprintf(W_STDERR, "su: cannot start %s: %s\n", shell, wstrerror(-pid));
        return 1;
    }

    int status = 0;
    wwait(pid, &status);

    return status;
}
