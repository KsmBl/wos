/* useradd -- create a user.
 *
 *   useradd [-a] [-u] <name>
 *
 * -a grants the appeditor role (write access to /app) and -u useradmin (may
 * add users and set their passwords). Root and holders of useradmin may run
 * this; anyone else is refused by the kernel.
 */

#include <wkernel.h>

static void usage(void)
{
    wfprintf(W_STDERR, "usage: useradd [-a] [-u] <name>\n");
    wfprintf(W_STDERR, "  -a  appeditor: may write under /app\n");
    wfprintf(W_STDERR, "  -u  useradmin: may add users and set passwords\n");
}

int main(int argc, char **argv)
{
    unsigned int roles = 0;
    const char  *name = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'a': roles |= W_ROLE_APPEDITOR; break;
                case 'u': roles |= W_ROLE_USERADMIN; break;
                default:
                    wfprintf(W_STDERR, "useradd: invalid option -- '%c'\n", *f);
                    return 1;
                }
            }
        } else if (!name) {
            name = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if (!name) {
        usage();
        return 1;
    }

    char first[128], second[128];

    if (wgetpass("Password: ", first, sizeof(first)) < 0)
        return 1;
    if (wgetpass("Retype password: ", second, sizeof(second)) < 0)
        return 1;

    if (strcmp(first, second) != 0) {
        wfprintf(W_STDERR, "useradd: the passwords do not match\n");
        return 1;
    }

    int uid = wuseradd(name, first, roles);

    if (uid < 0) {
        if (-uid == W_EPERM)
            wfprintf(W_STDERR,
                     "useradd: not permitted (needs root or useradmin)\n");
        else if (-uid == W_EEXIST)
            wfprintf(W_STDERR, "useradd: %s already exists\n", name);
        else if (-uid == W_EINVAL)
            wfprintf(W_STDERR,
                     "useradd: %s is not a usable name; it becomes a path, so "
                     "'/', ':', '.' and newlines are refused\n", name);
        else
            wfprintf(W_STDERR, "useradd: %s\n", wstrerror(-uid));
        return 1;
    }

    wprintf("Created %s (uid %d) with home /home/%s\n", name, uid, name);

    if (roles == 0)
        wprintf("No roles: may write only in its own home directory.\n");
    else
        wprintf("Roles:%s%s\n",
                (roles & W_ROLE_APPEDITOR) ? " appeditor" : "",
                (roles & W_ROLE_USERADMIN) ? " useradmin" : "");

    return 0;
}
