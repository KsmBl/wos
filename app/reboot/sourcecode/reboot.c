/* reboot -- restart the machine. */

#include <wkernel.h>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            wprintf("usage: reboot\n");
            wprintf("Restarts the machine. Everything written to disk is\n");
            wprintf("already safe: the filesystem writes its metadata\n");
            wprintf("straight through, so there is nothing to flush first.\n");
            wprintf("Root only -- a restart ends everybody's session.\n");
            return 0;
        }
        wfprintf(W_STDERR, "reboot: unexpected argument: %s\n", argv[i]);
        return 1;
    }

    wprintf("restarting\n");

    int r = wreboot();

    /* Only reached when the kernel refused.  It does not come back having
     * tried: the last thing it does is fault the processor into a reset, and
     * that always works. */
    if (r == -W_EPERM)
        wfprintf(W_STDERR, "reboot: only root can restart this machine\n");
    else
        wfprintf(W_STDERR, "reboot: %s\n", wstrerror(-r));

    return 1;
}
