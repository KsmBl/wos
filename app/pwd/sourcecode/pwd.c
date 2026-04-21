/* pwd -- print the working directory.
 *
 * A child process inherits its parent's working directory, so unlike cd this
 * works perfectly well as a separate program: it reports the directory the
 * shell handed it.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    char cwd[W_PATH_MAX + 1];

    int r = wgetcwd(cwd, sizeof(cwd));
    if (r < 0) {
        wfprintf(W_STDERR, "pwd: %s\n", wstrerror(-r));
        return 1;
    }

    wprintf("%s\n", cwd);
    return 0;
}
