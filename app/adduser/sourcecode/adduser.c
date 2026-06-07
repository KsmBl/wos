/* adduser -- create a user, asking for a password.
 *
 *   adduser [-a] [-u] [-f] <name>
 *
 * -a grants appeditor (write access to /app), -u usereditor (write access to
 * /userconfig: adding users, setting passwords and changing roles) and -f
 * editfreq (changing the processor's clock). Root and holders of usereditor
 * may run this; anyone else is refused by the kernel, not by this program.
 */

#include <wkernel.h>

static void usage(void)
{
    wfprintf(W_STDERR, "usage: adduser [-a] [-u] [-f] <name>\n");
    wfprintf(W_STDERR, "  -a  appeditor:  may write under /app\n");
    wfprintf(W_STDERR, "  -u  usereditor: may write /userconfig -- add users,\n");
    wfprintf(W_STDERR, "                  set passwords and change roles\n");
    wfprintf(W_STDERR, "  -f  editfreq:   may change the processor's clock\n");
}

int main(int argc, char **argv)
{
    unsigned int roles = 0;
    const char  *name = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'a': roles |= W_ROLE_APPEDITOR;  break;
                case 'u': roles |= W_ROLE_USEREDITOR; break;
                case 'f': roles |= W_ROLE_EDITFREQ;   break;
                default:
                    wfprintf(W_STDERR, "adduser: invalid option -- '%c'\n", *f);
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

    wprintf("Creating user %s\n", name);

    char first[128], second[128];

    if (wgetpass("Password: ", first, sizeof(first)) < 0)
        return 1;
    if (wgetpass("Retype password: ", second, sizeof(second)) < 0)
        return 1;

    if (strcmp(first, second) != 0) {
        wfprintf(W_STDERR, "adduser: the passwords do not match\n");
        return 1;
    }

    if (first[0] == '\0')
        wprintf("No password given; %s will be able to log in without one.\n",
                name);

    int uid = wuseradd(name, first, roles);

    if (uid < 0) {
        if (-uid == W_EPERM)
            wfprintf(W_STDERR,
                     "adduser: not permitted (needs root or usereditor)\n");
        else if (-uid == W_EEXIST)
            wfprintf(W_STDERR, "adduser: %s already exists\n", name);
        else if (-uid == W_EINVAL)
            wfprintf(W_STDERR,
                     "adduser: %s is not a usable name; it becomes a path, so "
                     "'/', ':', '.' and newlines are refused\n", name);
        else
            wfprintf(W_STDERR, "adduser: %s\n", wstrerror(-uid));
        return 1;
    }

    wprintf("Created %s (uid %d)\n", name, uid);
    wprintf("  home     /home/%s\n", name);
    wprintf("  password /userconfig/%s/password (root only)\n", name);

    if (roles == 0)
        wprintf("  roles    none -- may write only in its own home\n");
    else
        wprintf("  roles   %s%s%s\n",
                (roles & W_ROLE_APPEDITOR)  ? " appeditor"  : "",
                (roles & W_ROLE_USEREDITOR) ? " usereditor" : "",
                (roles & W_ROLE_EDITFREQ)   ? " editfreq"   : "");

    return 0;
}
