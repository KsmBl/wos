/* touch -- create files, and mark existing ones as changed just now.
 *
 * The second half is the useful one: a file that already exists has nothing to
 * write, so wutime() moves its modification time on its own.  That is what
 * makes anything built from it -- by `make`, say -- out of date.
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
        wstat_t st;

        if (wstat(argv[i], &st) == 0) {
            int r = wutime(argv[i]);
            if (r < 0) {
                wfprintf(W_STDERR, "touch: %s: %s\n", argv[i], wstrerror(-r));
                status = 1;
            }
            continue;
        }

        /* Without W_O_TRUNC a file that appeared in the meantime keeps its
         * contents; creating it is enough to give it a time. */
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
