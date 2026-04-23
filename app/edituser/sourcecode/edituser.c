/* edituser -- add and remove roles.
 *
 *   edituser <name>                 show what roles they hold
 *   edituser <name> +appeditor      grant a role
 *   edituser <name> -usereditor     take one away
 *
 * Several changes can be given at once and are applied in order, so
 * "edituser bob -appeditor +usereditor" does both.
 *
 * The kernel replaces the whole bitmask in one call, so this reads the current
 * roles first and sends the result. That means two people editing the same
 * user at the same moment would have one overwrite the other -- there is no
 * locking here, and with one console there is no way to try.
 */

#include <wkernel.h>

static const struct {
    const char  *name;
    unsigned int bit;
    const char  *what;
} role_table[] = {
    { "appeditor",  W_ROLE_APPEDITOR,  "may write under /app" },
    { "usereditor", W_ROLE_USEREDITOR, "may write /userconfig: add users, "
                                       "set passwords, change roles" },
};

#define ROLE_COUNT ((int)(sizeof(role_table) / sizeof(role_table[0])))

static void usage(void)
{
    wfprintf(W_STDERR, "usage: edituser <name> [+role] [-role] ...\n\n");
    wfprintf(W_STDERR, "Roles:\n");
    for (int i = 0; i < ROLE_COUNT; i++)
        wfprintf(W_STDERR, "  %-11s %s\n", role_table[i].name,
                 role_table[i].what);
    wfprintf(W_STDERR, "\nWith no change given, prints what the user holds.\n");
}

static void print_roles(const wuser_t *u)
{
    wprintf("%s (uid %u):", u->name, u->uid);

    if (u->uid == W_ROOT_UID) {
        wprintf(" root -- every permission, roles do not apply\n");
        return;
    }

    if (u->roles == 0) {
        wprintf(" no roles\n");
        return;
    }

    for (int i = 0; i < ROLE_COUNT; i++)
        if (u->roles & role_table[i].bit)
            wprintf(" %s", role_table[i].name);
    wprintf("\n");
}

/* Find a user by name in the list, since there is no lookup-by-name call. */
static int find_user(const char *name, wuser_t *out)
{
    wuser_t all[W_MAX_USERS];
    int n = wuserlist(all, W_MAX_USERS);

    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, name) == 0) {
            *out = all[i];
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *name = argv[1];
    wuser_t     target;

    if (find_user(name, &target) < 0) {
        wfprintf(W_STDERR, "edituser: %s: no such user\n", name);
        return 1;
    }

    /* No changes asked for: just report. */
    if (argc == 2) {
        print_roles(&target);
        return 0;
    }

    unsigned int roles = target.roles;

    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        int adding;

        if (arg[0] == '+')
            adding = 1;
        else if (arg[0] == '-')
            adding = 0;
        else {
            wfprintf(W_STDERR,
                     "edituser: %s: expected +role or -role\n", arg);
            usage();
            return 1;
        }

        int found = 0;
        for (int r = 0; r < ROLE_COUNT; r++) {
            if (strcmp(arg + 1, role_table[r].name) == 0) {
                if (adding)
                    roles |= role_table[r].bit;
                else
                    roles &= ~role_table[r].bit;
                found = 1;
                break;
            }
        }

        if (!found) {
            wfprintf(W_STDERR, "edituser: %s: no such role\n", arg + 1);
            usage();
            return 1;
        }
    }

    if (roles == target.roles) {
        wprintf("No change.\n");
        print_roles(&target);
        return 0;
    }

    int r = wsetroles(name, roles);

    if (r < 0) {
        if (-r == W_EPERM)
            wfprintf(W_STDERR,
                     "edituser: not permitted (needs root or usereditor%s)\n",
                     target.uid == W_ROOT_UID
                         ? ", and root's roles cannot be changed" : "");
        else
            wfprintf(W_STDERR, "edituser: %s\n", wstrerror(-r));
        return 1;
    }

    if (find_user(name, &target) == 0)
        print_roles(&target);

    return 0;
}
