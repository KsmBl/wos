/* whoami -- print the current user, and what they may do. */

#include <wkernel.h>

int main(int argc, char **argv)
{
    wuser_t me;

    int r = wuserinfo(-1, &me);
    if (r < 0) {
        wfprintf(W_STDERR, "whoami: %s\n", wstrerror(-r));
        return 1;
    }

    /* Plain output by default, so it can be read at a glance or used in a
     * prompt; -v explains the permissions. */
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        wprintf("user  : %s (uid %u)\n", me.name, me.uid);

        if (me.uid == W_ROOT_UID) {
            wprintf("roles : root -- every permission, and write access "
                    "everywhere\n");
            return 0;
        }

        wprintf("roles :");
        if (me.roles == 0)
            wprintf(" none");
        if (me.roles & W_ROLE_APPEDITOR)
            wprintf(" appeditor");
        if (me.roles & W_ROLE_USERADMIN)
            wprintf(" useradmin");
        wprintf("\n");

        wprintf("write : /home/%s\n", me.name);
        if (me.roles & W_ROLE_APPEDITOR)
            wprintf("        /app\n");
        wprintf("        (everything else is read-only; /kernel is root only)\n");
        return 0;
    }

    wprintf("%s\n", me.name);
    return 0;
}
