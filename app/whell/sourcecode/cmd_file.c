/* rm and touch -- remove and create files. */

#include "whell.h"

static int opt_recursive;
static int opt_force;

/* Report a failure unless -f asked us to keep quiet.  Returns the exit status
 * the caller should accumulate. */
static int fail(const char *path, int err)
{
    if (opt_force)
        return 0;

    wfprintf(W_STDERR, "rm: %s: %s\n", path, wstrerror(-err));
    return 1;
}

static int remove_tree(const char *path);

/* Delete everything inside a directory, then the directory itself.
 *
 * The children are collected before any of them is deleted.  wreaddir walks
 * by entry index, so removing an entry mid-iteration shifts the ones after it
 * and the walk would skip files. */
static int remove_children(const char *path)
{
    int d = wopendir(path);
    if (d < 0)
        return fail(path, d);

    int capacity = 32;
    int count = 0;
    char (*names)[W_NAME_MAX + 1] = malloc((wsize_t)capacity * (W_NAME_MAX + 1));

    if (!names) {
        wfprintf(W_STDERR, "rm: out of memory\n");
        wclosedir(d);
        return 1;
    }

    wdirent_t e;
    while (wreaddir(d, &e) == 1) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;

        if (count == capacity) {
            capacity *= 2;
            void *bigger = realloc(names, (wsize_t)capacity * (W_NAME_MAX + 1));
            if (!bigger) {
                wfprintf(W_STDERR, "rm: out of memory\n");
                free(names);
                wclosedir(d);
                return 1;
            }
            names = bigger;
        }

        strlcpy(names[count], e.name, W_NAME_MAX + 1);
        count++;
    }
    wclosedir(d);

    int status = 0;
    for (int i = 0; i < count; i++) {
        char child[W_PATH_MAX + 1];
        wsnprintf(child, sizeof(child), "%s/%s", path, names[i]);
        if (remove_tree(child) != 0)
            status = 1;
    }

    free(names);
    return status;
}

static int remove_tree(const char *path)
{
    wstat_t st;
    int r = wstat(path, &st);

    if (r < 0)
        return fail(path, r);

    if (st.type != W_FT_DIR) {
        r = wunlink(path);
        return (r < 0) ? fail(path, r) : 0;
    }

    if (!opt_recursive) {
        wfprintf(W_STDERR, "rm: %s: is a directory\n", path);
        return 1;
    }

    if (remove_children(path) != 0)
        return 1;

    r = wrmdir(path);
    return (r < 0) ? fail(path, r) : 0;
}

int cmd_rm(int argc, char **argv)
{
    opt_recursive = 0;
    opt_force     = 0;

    const char *operands[WHELL_MAX_ARGS];
    int operand_count = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'r':
                case 'R': opt_recursive = 1; break;
                case 'f': opt_force     = 1; break;
                default:
                    wfprintf(W_STDERR, "rm: invalid option -- '%c'\n", *f);
                    return 1;
                }
            }
        } else {
            operands[operand_count++] = argv[i];
        }
    }

    if (operand_count == 0) {
        /* -f with nothing to remove is not an error, as in Linux. */
        if (opt_force)
            return 0;
        wfprintf(W_STDERR, "rm: no file given\n");
        return 1;
    }

    int status = 0;
    for (int i = 0; i < operand_count; i++) {
        /* Refuse the two that can only be mistakes: "rm -r ." would delete
         * the working directory out from under the shell, and ".." its
         * parent. Linux refuses both for the same reason. */
        const char *base = strrchr(operands[i], '/');
        base = base ? base + 1 : operands[i];

        if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
            wfprintf(W_STDERR,
                     "rm: refusing to remove '.' or '..': skipping %s\n",
                     operands[i]);
            status = 1;
            continue;
        }

        if (remove_tree(operands[i]) != 0)
            status = 1;
    }

    return status;
}

int cmd_mkdir(int argc, char **argv)
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

int cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        wfprintf(W_STDERR, "touch: no file given\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        /* Without W_O_TRUNC an existing file keeps its contents.  WFS stores
         * no timestamps, so unlike Linux there is nothing to update on a file
         * that already exists -- touch simply succeeds. */
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
