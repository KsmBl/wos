/* mv -- move or rename files.
 *
 *     mv old.txt new.txt          rename it
 *     mv notes.txt /home/root     move it, keeping its name
 *     mv a.txt b.txt /ramdisk     several, into a directory
 *
 * One syscall does the work: wrename() moves a directory entry rather than
 * bytes, so moving a file is the same amount of work whatever is in it.  That
 * is also why this cannot move between the disk and /ramdisk -- those are two
 * filesystems, and an entry in one means nothing in the other.  Copying it
 * across silently is what `cp` would be for, and this says so instead.
 */

#include <wkernel.h>

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (!slash || !slash[1])
        return path;
    return slash + 1;
}

static int is_dir(const char *path)
{
    wstat_t st;

    return wstat(path, &st) == 0 && st.type == W_FT_DIR;
}

/* Move one thing to one place, where the place may be a directory to put it
 * in rather than the name to give it. */
static int move(const char *from, const char *to)
{
    char joined[W_PATH_MAX + 1];

    if (is_dir(to)) {
        const char *name = basename_of(from);

        if (to[0] == '/' && to[1] == '\0')
            wsnprintf(joined, sizeof(joined), "/%s", name);
        else
            wsnprintf(joined, sizeof(joined), "%s/%s", to, name);

        to = joined;
    }

    int r = wrename(from, to);

    if (r == 0)
        return 0;

    /* The two failures worth explaining rather than naming: one is a thing
     * this program cannot do, and the other is a thing nobody should want. */
    if (-r == W_EXDEV)
        wfprintf(W_STDERR, "mv: %s and %s are on different filesystems; "
                           "a move between them would be a copy\n", from, to);
    else if (-r == W_EISDIR)
        wfprintf(W_STDERR, "mv: %s is a directory; it is not replaced\n", to);
    else
        wfprintf(W_STDERR, "mv: cannot move %s to %s: %s\n", from, to,
                 wstrerror(-r));

    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        wfprintf(W_STDERR, "usage: mv <from> <to>\n"
                           "       mv <file> ... <directory>\n");
        return 1;
    }

    const char *last = argv[argc - 1];

    /* Several sources need somewhere to put them; two arguments are a rename
     * unless the second names a directory, which move() works out. */
    if (argc > 3 && !is_dir(last)) {
        wfprintf(W_STDERR, "mv: %s is not a directory, so it cannot hold "
                           "%d files\n", last, argc - 2);
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc - 1; i++)
        status |= move(argv[i], last);

    return status;
}
