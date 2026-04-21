/* touch -- create files.
 *
 * WFS stores no timestamps, so unlike Linux there is nothing to update on a
 * file that already exists; touch simply succeeds and leaves it alone.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        wfprintf(W_STDERR, "touch: no file given\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        /* Without W_O_TRUNC an existing file keeps its contents. */
        int fd = wopen(argv[i], W_O_WRONLY | W_O_CREAT);

        if (fd < 0) {
            wfprintf(W_STDERR, "touch: %s: %s\n", argv[i], wstrerror(-fd));
            status = 1;
            continue;
        }

        wclose(fd);
    }

    return status;
}
