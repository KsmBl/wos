/* ls -- list directory contents.
 *
 * Follows Linux where the information exists: entries are sorted, dotfiles are
 * hidden unless -a is given, directories get a "path:" header when several
 * operands are listed, and plain output is laid out in columns.
 *
 * -l shows type, size and block count. WFS has no owners, permissions or
 * timestamps, so those Linux columns are left out rather than filled with
 * invented values.
 */

#include <wkernel.h>

#define MAX_OPERANDS 32

struct entry {
    char     name[W_NAME_MAX + 1];
    uint32_t type;
    uint32_t size;
    uint32_t blocks;
};

static int opt_long;
static int opt_all;

/* Insertion sort: directories here hold tens of entries, and it keeps the
 * comparison obvious. */
static void sort_entries(struct entry *e, int n)
{
    for (int i = 1; i < n; i++) {
        struct entry key = e[i];
        int j = i - 1;
        while (j >= 0 && strcmp(e[j].name, key.name) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

static void print_long(const struct entry *e, int count)
{
    unsigned long widest = 1;
    for (int i = 0; i < count; i++) {
        unsigned long w = 1, v = e[i].size;
        while (v >= 10) {
            v /= 10;
            w++;
        }
        if (w > widest)
            widest = w;
    }

    unsigned long total = 0;
    for (int i = 0; i < count; i++)
        total += e[i].blocks;
    wprintf("total %lu\n", total);

    for (int i = 0; i < count; i++)
        wprintf("%c %*u %5u  %s\n",
                e[i].type == W_FT_DIR ? 'd' : '-',
                (int)widest, e[i].size, e[i].blocks, e[i].name);
}

/* Lay the names out in columns that fit the terminal, like ls does. */
static void print_columns(const struct entry *e, int count)
{
    wsize_t widest = 0;
    for (int i = 0; i < count; i++) {
        wsize_t len = strlen(e[i].name);
        if (e[i].type == W_FT_DIR)
            len++;                       /* the trailing '/' */
        if (len > widest)
            widest = len;
    }

    wsize_t column = widest + 2;
    int per_line = (int)(W_CONSOLE_WIDTH / column);
    if (per_line < 1)
        per_line = 1;

    for (int i = 0; i < count; i++) {
        int last_in_row = ((i + 1) % per_line == 0) || (i + 1 == count);

        if (last_in_row) {
            wprintf("%s%s\n", e[i].name, e[i].type == W_FT_DIR ? "/" : "");
        } else {
            wsize_t len = strlen(e[i].name) + (e[i].type == W_FT_DIR ? 1 : 0);
            wprintf("%s%s", e[i].name, e[i].type == W_FT_DIR ? "/" : "");
            for (wsize_t pad = len; pad < column; pad++)
                wprintf(" ");
        }
    }
}

/* List one directory. Returns 0, or 1 if something went wrong. */
static int list_directory(const char *path)
{
    int d = wopendir(path);
    if (d < 0) {
        wfprintf(W_STDERR, "ls: %s: %s\n", path, wstrerror(-d));
        return 1;
    }

    int capacity = 32;
    int count = 0;
    struct entry *entries = malloc((wsize_t)capacity * sizeof(struct entry));
    if (!entries) {
        wfprintf(W_STDERR, "ls: out of memory\n");
        wclosedir(d);
        return 1;
    }

    wdirent_t raw;
    while (wreaddir(d, &raw) == 1) {
        if (!opt_all && raw.name[0] == '.')
            continue;

        if (count == capacity) {
            capacity *= 2;
            struct entry *bigger =
                realloc(entries, (wsize_t)capacity * sizeof(struct entry));
            if (!bigger) {
                wfprintf(W_STDERR, "ls: out of memory\n");
                free(entries);
                wclosedir(d);
                return 1;
            }
            entries = bigger;
        }

        strlcpy(entries[count].name, raw.name, sizeof(entries[count].name));
        entries[count].type   = raw.type;
        entries[count].size   = 0;
        entries[count].blocks = 0;

        /* Sizes need a stat per entry, so only pay for it with -l. */
        if (opt_long) {
            char full[W_PATH_MAX + 1];
            wsnprintf(full, sizeof(full), "%s/%s", path, raw.name);

            wstat_t st;
            if (wstat(full, &st) == 0) {
                entries[count].size   = st.size;
                entries[count].blocks = st.blocks;
            }
        }

        count++;
    }
    wclosedir(d);

    sort_entries(entries, count);

    if (opt_long)
        print_long(entries, count);
    else
        print_columns(entries, count);

    free(entries);
    return 0;
}

int main(int argc, char **argv)
{
    const char *operands[MAX_OPERANDS];
    int operand_count = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'l': opt_long = 1; break;
                case 'a': opt_all  = 1; break;
                default:
                    wfprintf(W_STDERR, "ls: invalid option -- '%c'\n", *f);
                    return 1;
                }
            }
        } else if (operand_count < MAX_OPERANDS) {
            operands[operand_count++] = argv[i];
        }
    }

    if (operand_count == 0) {
        char cwd[W_PATH_MAX + 1];
        if (wgetcwd(cwd, sizeof(cwd)) < 0)
            strlcpy(cwd, "/", sizeof(cwd));
        return list_directory(cwd);
    }

    int status = 0;

    for (int i = 0; i < operand_count; i++) {
        wstat_t st;
        int r = wstat(operands[i], &st);

        if (r < 0) {
            wfprintf(W_STDERR, "ls: %s: %s\n", operands[i], wstrerror(-r));
            status = 1;
            continue;
        }

        /* A file operand is listed as itself, not descended into. */
        if (st.type != W_FT_DIR) {
            if (opt_long)
                wprintf("- %u %5u  %s\n", st.size, st.blocks, operands[i]);
            else
                wprintf("%s\n", operands[i]);
            continue;
        }

        if (operand_count > 1) {
            if (i > 0)
                wprintf("\n");
            wprintf("%s:\n", operands[i]);
        }

        if (list_directory(operands[i]) != 0)
            status = 1;
    }

    return status;
}
