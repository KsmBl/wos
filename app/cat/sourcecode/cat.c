/* cat -- print files.
 *
 * Continues past a file it cannot open, reporting each failure, so one bad
 * operand does not hide the rest.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        wfprintf(W_STDERR, "cat: no file given\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        int fd = wopen(argv[i], W_O_RDONLY);
        if (fd < 0) {
            wfprintf(W_STDERR, "cat: %s: %s\n", argv[i], wstrerror(-fd));
            status = 1;
            continue;
        }

        char buf[512];
        int  n;
        while ((n = wread(fd, buf, sizeof(buf))) > 0)
            wwrite(W_STDOUT, buf, (wsize_t)n);

        if (n < 0) {
            wfprintf(W_STDERR, "cat: %s: %s\n", argv[i], wstrerror(-n));
            status = 1;
        }

        wclose(fd);
    }

    return status;
}
