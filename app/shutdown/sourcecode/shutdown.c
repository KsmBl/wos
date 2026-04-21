/* shutdown -- power the machine off. */

#include <wkernel.h>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            wprintf("usage: shutdown\n");
            wprintf("Powers the machine off. Everything written to disk is\n");
            wprintf("already safe: the filesystem writes its metadata\n");
            wprintf("straight through, so there is nothing to flush first.\n");
            return 0;
        }
        wfprintf(W_STDERR, "shutdown: unexpected argument: %s\n", argv[i]);
        return 1;
    }

    wprintf("shutting down\n");

    int r = wshutdown();

    /* Only reached if the machine has no soft-off the kernel can drive, and
     * even then the kernel halts rather than coming back here. */
    wfprintf(W_STDERR, "shutdown: %s\n", wstrerror(-r));
    return 1;
}
