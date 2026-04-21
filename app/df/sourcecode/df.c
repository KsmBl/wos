/* df -- show disk use.
 *
 * The numbers come from the filesystem's block bitmap, so `Used` counts
 * blocks that are genuinely allocated, including the superblock, the bitmap
 * and the inode table.
 */

#include <wkernel.h>

/* Scale factor for the unit flags -b, -k, -m and -h. */
struct unit {
    unsigned long divisor;
    int           human;      /* format with whuman() instead of dividing */
};

static int parse_unit(int argc, char **argv, struct unit *u,
                      unsigned long default_divisor, const char *cmd)
{
    u->divisor = default_divisor;
    u->human   = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            wfprintf(W_STDERR, "%s: unexpected argument: %s\n", cmd, argv[i]);
            return -1;
        }

        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'b': u->divisor = 1;           u->human = 0; break;
            case 'k': u->divisor = 1024;        u->human = 0; break;
            case 'm': u->divisor = 1024 * 1024; u->human = 0; break;
            case 'h': u->human   = 1;                         break;
            default:
                wfprintf(W_STDERR, "%s: invalid option -- '%c'\n", cmd, *f);
                return -1;
            }
        }
    }
    return 0;
}

/* Render one figure according to the selected unit. */
static void format_value(char *buf, wsize_t size, unsigned long bytes,
                         const struct unit *u)
{
    if (u->human)
        wsnprintf(buf, size, "%s", whuman(bytes));
    else
        wsnprintf(buf, size, "%lu", bytes / u->divisor);
}

int main(int argc, char **argv)
{
    struct unit u;

    if (parse_unit(argc, argv, &u, 1024, "df") < 0)
        return 1;

    wdiskinfo_t d;
    int r = wdiskinfo(&d);
    if (r < 0) {
        wfprintf(W_STDERR, "df: %s\n", wstrerror(-r));
        return 1;
    }

    if (d.total_bytes == 0) {
        wfprintf(W_STDERR, "df: no filesystem is mounted\n");
        return 1;
    }

    char total[24], used[24], avail[24];

    format_value(total, sizeof(total), d.total_bytes, &u);
    format_value(used, sizeof(used), d.used_bytes, &u);
    format_value(avail, sizeof(avail), d.free_bytes, &u);

    /* A disk with anything at all on it should read as at least 1%, never 0%. */
    unsigned long percent = 0;
    if (d.total_blocks)
        percent = (d.total_blocks - d.free_blocks) * 100 / d.total_blocks;
    if (percent == 0 && d.free_blocks != d.total_blocks)
        percent = 1;

    wprintf("%-12s %10s %10s %10s %4s %s\n",
            "Filesystem", u.human ? "Size" : "1K-blocks",
            "Used", u.human ? "Avail" : "Available", "Use%", "Mounted on");
    wprintf("%-12s %10s %10s %10s %3lu%% %s\n",
            "wfs", total, used, avail, percent, "/");

    wprintf("\ninodes: %lu used, %lu free, %lu total\n",
            (unsigned long)(d.total_inodes - d.free_inodes),
            (unsigned long)d.free_inodes, (unsigned long)d.total_inodes);

    return 0;
}
