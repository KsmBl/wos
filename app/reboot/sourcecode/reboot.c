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
            return 0;
        }
        wfprintf(W_STDERR, "reboot: unexpected argument: %s\n", argv[i]);
        return 1;
    }

    wprintf("restarting\n");

    int r = wreboot();

    /* Effectively unreachable: the kernel's last resort is a triple fault, so
     * it does not come back having tried. */
    wfprintf(W_STDERR, "reboot: %s\n", wstrerror(-r));
    return 1;
}
