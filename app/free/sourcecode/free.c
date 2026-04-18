/* free -- show memory use, in the shape Linux prints it.
 *
 * The figures come from the kernel's physical frame allocator, so `used` is
 * exactly the RAM currently allocated -- including the kernel's own image,
 * heap and page tables -- and used + free == total always holds.
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
