/* free and df -- memory and disk usage, in the shape Linux prints them. */

#include "whell.h"

/* Scale factor and heading for the unit flags shared by free and df. */
struct unit {
    unsigned int divisor;
    int          human;      /* format with whuman() instead of dividing */
};

static int parse_unit(int argc, char **argv, struct unit *u,
                      unsigned int default_divisor, const char *cmd)
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
            case 'b': u->divisor = 1;               u->human = 0; break;
            case 'k': u->divisor = 1024;            u->human = 0; break;
            case 'm': u->divisor = 1024 * 1024;     u->human = 0; break;
            case 'h': u->human   = 1;                             break;
            default:
                wfprintf(W_STDERR, "%s: invalid option -- '%c'\n", cmd, *f);
                return -1;
            }
        }
    }
    return 0;
}

/* Render one figure into `buf` according to the selected unit. */
static void format_value(char *buf, wsize_t size, unsigned int bytes,
                         const struct unit *u)
{
    if (u->human)
        wsnprintf(buf, size, "%s", whuman(bytes));
    else
        wsnprintf(buf, size, "%u", bytes / u->divisor);
}

int cmd_free(int argc, char **argv)
{
    struct unit u;

    /* Like Linux, default to KiB. */
    if (parse_unit(argc, argv, &u, 1024, "free") < 0)
        return 1;

    wmeminfo_t m;
    int r = wmeminfo(&m);
    if (r < 0) {
        wfprintf(W_STDERR, "free: %s\n", wstrerror(-r));
        return 1;
    }

    char total[24], used[24], freed[24];

    format_value(total, sizeof(total), m.total_bytes, &u);
    format_value(used, sizeof(used), m.used_bytes, &u);
    format_value(freed, sizeof(freed), m.free_bytes, &u);

    wprintf("%15s %11s %11s\n", "total", "used", "free");
    wprintf("Mem:   %8s %11s %11s\n", total, used, freed);

    /* WOS has no swap. The row is printed anyway so the output matches what
     * anyone reading `free` expects to see. */
    format_value(total, sizeof(total), 0, &u);
    wprintf("Swap:  %8s %11s %11s\n", total, total, total);

    return 0;
}

int cmd_df(int argc, char **argv)
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

    /* Integer percentage, rounded up the way df does, so a nearly empty disk
     * still shows 1% rather than 0%. */
    unsigned int percent = 0;
    if (d.total_blocks)
        percent = (d.total_blocks - d.free_blocks) * 100u / d.total_blocks;
    if (percent == 0 && d.free_blocks != d.total_blocks)
        percent = 1;

    wprintf("%-12s %10s %10s %10s %4s %s\n",
            "Filesystem", u.human ? "Size" : "1K-blocks",
            "Used", u.human ? "Avail" : "Available", "Use%", "Mounted on");
    wprintf("%-12s %10s %10s %10s %3u%% %s\n",
            "wfs", total, used, avail, percent, "/");

    wprintf("\ninodes: %u used, %u free, %u total\n",
            d.total_inodes - d.free_inodes, d.free_inodes, d.total_inodes);

    return 0;
}

int cmd_ps(int argc, char **argv)
{
    wprocmem_t procs[32];

    int n = wproclist(procs, 32);
    if (n < 0) {
        wfprintf(W_STDERR, "ps: %s\n", wstrerror(-n));
        return 1;
    }

    wprintf("%5s %-12s %9s %8s %8s %8s %8s %3s\n",
            "PID", "NAME", "RESIDENT", "CODE", "DATA", "HEAP", "STACK", "THR");

    for (int i = 0; i < n; i++)
        wprintf("%5d %-12s %9s %8s %8s %8s %8s %3d\n",
                procs[i].pid,
                procs[i].name[0] ? procs[i].name : "?",
                whuman(procs[i].resident_bytes),
                whuman(procs[i].code_bytes),
                whuman(procs[i].data_bytes),
                whuman(procs[i].heap_bytes),
                whuman(procs[i].stack_bytes),
                procs[i].thread_count);

    return 0;
}
