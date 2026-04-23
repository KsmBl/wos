/* passwd -- change a password.
 *
 * The kernel does the checking and the storing; this program never sees a
 * hash. That is what makes it safe for an ordinary user to run without setuid:
 * there is nothing privileged here to abuse.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    wuser_t me;

    if (wuserinfo(-1, &me) < 0) {
        wfprintf(W_STDERR, "passwd: cannot identify the current user\n");
        return 1;
    }

    /* No argument means your own account. */
    const char *target = (argc > 1) ? argv[1] : me.name;

    wuser_t victim;
    if (wuserinfo(-1, &victim) < 0)
        return 1;

    /* Confirm the account exists before asking for anything. */
    int found = 0;
    wuser_t all[W_MAX_USERS];
    int n = wuserlist(all, W_MAX_USERS);
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, target) == 0) {
            victim = all[i];
            found = 1;
            break;
        }
    }
    if (!found) {
        wfprintf(W_STDERR, "passwd: %s: no such user\n", target);
        return 1;
    }

    int privileged = (me.uid == W_ROOT_UID) || (me.roles & W_ROLE_USEREDITOR);

    if (!privileged && victim.uid != me.uid) {
        wfprintf(W_STDERR,
                 "passwd: you may only change your own password\n");
        return 1;
    }

    wprintf("Changing password for %s\n", target);

    char old[128];
    old[0] = '\0';

    /* A privileged caller is not asked for the old password -- the kernel
     * would ignore it anyway. */
    if (!privileged) {
        if (wgetpass("Current password: ", old, sizeof(old)) < 0)
            return 1;
    }

    char first[128], second[128];

    if (wgetpass("New password: ", first, sizeof(first)) < 0)
        return 1;
    if (wgetpass("Retype new password: ", second, sizeof(second)) < 0)
        return 1;

    if (strcmp(first, second) != 0) {
        wfprintf(W_STDERR, "passwd: the passwords do not match\n");
        return 1;
    }

    int r = wpasswd(target, old, first);

    if (r < 0) {
        /* Say which of the two failures it was: "not permitted" and "wrong
         * password" are different problems and want different reactions. */
        if (-r == W_EACCES)
            wfprintf(W_STDERR, "passwd: the current password is wrong\n");
        else if (-r == W_EPERM)
            wfprintf(W_STDERR, "passwd: not permitted\n");
        else
            wfprintf(W_STDERR, "passwd: %s\n", wstrerror(-r));
        return 1;
    }

    if (first[0] == '\0')
        wprintf("Password removed; %s can now log in without one.\n", target);
    else
        wprintf("Password updated.\n");

    return 0;
}
