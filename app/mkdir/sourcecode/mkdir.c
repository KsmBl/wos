/* mkdir -- create directories. */

#include <wkernel.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        wfprintf(W_STDERR, "mkdir: no directory given\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        int r = wmkdir(argv[i]);

        if (r < 0) {
            wfprintf(W_STDERR, "mkdir: %s: %s\n", argv[i], wstrerror(-r));
            status = 1;
        }
    }

    return status;
}
