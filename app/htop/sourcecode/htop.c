/* htop -- a full-screen process and resource monitor.
 *
 * A WOS-native program in the spirit of htop, not a build of the upstream one:
 * that wants ncurses, /proc, signals and ioctl.  This draws with the ANSI
 * escapes the console understands and reads its figures from wkernel.
 *
 * The display refreshes on a timer while staying responsive to keys, which is
 * what wpollin() is for -- blocking in wread() would freeze the display until
 * someone pressed something.
 */

#include <wkernel.h>

#define REFRESH_TICKS 100          /* 100 ticks at 100 Hz = one second */
#define METER_WIDTH   40
#define MAX_PROCS     32

static wprocmem_t procs[MAX_PROCS];
static int        proc_count;
static int        selected;

/* The console size, read at startup so htop fills whatever resolution is set
 * rather than assuming 80x25. */
static int cols = 80, rows = 25;

/* Express used/total out of `out_of`, in 32-bit arithmetic only.
 *
 * WOS links no libgcc, so there is no 64-bit division to fall back on: the
 * obvious `used * out_of / total` would overflow on a multi-gigabyte total.
 * Halving both sides keeps the ratio while bringing the product back in
 * range. */
static unsigned scale_to(unsigned used, unsigned total, unsigned out_of)
{
    if (total == 0 || out_of == 0)
        return 0;

    while (used > 0xFFFFFFFFu / out_of) {
        used  /= 2;
        total /= 2;
    }

    if (total == 0)
        return out_of;

    return used * out_of / total;
}

/* Colour a meter by how full it is, the way htop does: green while there is
 * plenty, yellow as it fills, red when it is nearly gone. */
static int meter_colour(unsigned used, unsigned total)
{
    unsigned percent = scale_to(used, total, 100);

    if (percent >= 90)
        return W_RED | W_BRIGHT;
    if (percent >= 70)
        return W_YELLOW | W_BRIGHT;
    return W_GREEN | W_BRIGHT;
}

/* One labelled bar: "Mem[||||||      5.3M/255.8M]". */
static void draw_meter(int row, const char *label, unsigned used,
                       unsigned total)
{
    unsigned filled = scale_to(used, total, METER_WIDTH);
    if (filled > METER_WIDTH)
        filled = METER_WIDTH;

    /* A non-zero amount should always show at least one bar, or a small but
     * real allocation looks like nothing at all. */
    if (filled == 0 && used > 0)
        filled = 1;

    wgotoxy(row, 3);
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("%-4s", label);
    wcolor_reset();
    wprintf("[");

    wcolor(meter_colour(used, total), W_DEFAULT);
    for (unsigned i = 0; i < filled; i++)
        wprintf("|");
    wcolor_reset();
    for (unsigned i = filled; i < METER_WIDTH; i++)
        wprintf(" ");

    /* The reading sits inside the bracket, right-aligned, as htop does. */
    char reading[32];
    wsnprintf(reading, sizeof(reading), "%s/%s", whuman(used), whuman(total));

    wgotoxy(row, 3 + 4 + 1 + METER_WIDTH - (int)strlen(reading));
    wcolor(W_WHITE | W_BRIGHT, W_DEFAULT);
    wprintf("%s", reading);
    wcolor_reset();
    wprintf("]");
}

/* The same line the uptime command prints, from the same function: two places
 * showing the same number is two chances to show different ones. */
static void draw_uptime(void)
{
    char field[32];
    wsnprintf(field, sizeof(field), " up %s ", wuptime_string());

    int col = cols - (int)strlen(field);
    if (col < 1)
        col = 1;

    wgotoxy(1, col);
    wcolor(W_BLACK, W_CYAN);
    wprintf("%s", field);
    wcolor_reset();
}

static void draw_header(void)
{
    wgotoxy(1, 1);
    wcolor(W_BLACK, W_CYAN);
    wprintf(" htop ");
    wcolor_reset();
    wprintf("  a process monitor for WOS");

    draw_uptime();
}

static void draw_summary(wmeminfo_t *mem, wdiskinfo_t *disk)
{
    draw_meter(3, "Mem", mem->used_bytes, mem->total_bytes);
    draw_meter(4, "Dsk", disk->used_bytes, disk->total_bytes);

    int threads = 0;
    for (int i = 0; i < proc_count; i++)
        threads += procs[i].thread_count;

    wgotoxy(6, 3);
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("Tasks");
    wcolor_reset();
    wprintf(": %d      ", proc_count);

    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("Threads");
    wcolor_reset();
    wprintf(": %d      ", threads);

    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("Kernel");
    wcolor_reset();
    wprintf(": %s   ", whuman(mem->kernel_bytes));
}

static void draw_process_table(void)
{
    /* Column headings, in reverse video across the full width. */
    wgotoxy(8, 1);
    wcolor(W_BLACK, W_GREEN);
    wprintf("  %5s %-12s %9s %8s %8s %8s %8s %4s",
            "PID", "COMMAND", "RESIDENT", "CODE", "DATA", "HEAP", "STACK",
            "THR");
    /* The heading row is 71 characters wide; pad it out so the reverse-video
     * bar reaches the edge of the screen. */
    for (int i = 0; i < cols - 71; i++)
        wprintf(" ");
    wcolor_reset();

    for (int i = 0; i < proc_count && i < rows - 11; i++) {
        wgotoxy(9 + i, 1);

        if (i == selected)
            wcolor(W_BLACK, W_CYAN);

        wprintf("  %5d %-12s %9s %8s %8s %8s %8s %4d",
                procs[i].pid,
                procs[i].name[0] ? procs[i].name : "?",
                whuman(procs[i].resident_bytes),
                whuman(procs[i].code_bytes),
                whuman(procs[i].data_bytes),
                whuman(procs[i].heap_bytes),
                whuman(procs[i].stack_bytes),
                procs[i].thread_count);

        if (i == selected) {
            /* Pad the highlight to the full width so the selected row reads
             * as a bar rather than as coloured text. */
            for (int c = 0; c < cols - 71; c++)
                wprintf(" ");
        }

        wcolor_reset();
        wclear_line();
    }

    /* Blank out rows left behind by a process that has since exited. */
    for (int i = proc_count; i < rows - 11; i++) {
        wgotoxy(9 + i, 1);
        wclear_line();
    }
}

static void draw_footer(void)
{
    wgotoxy(rows, 1);
    wcolor(W_BLACK, W_CYAN);
    wprintf(" up/dn ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" Select ");
    wcolor(W_BLACK, W_CYAN);
    wprintf(" q ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" Quit   ");
    wcolor(W_BLACK, W_CYAN);
    wprintf(" r ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" Refresh now ");
    wcolor_reset();
    wclear_line();
}

static void redraw(void)
{
    wmeminfo_t  mem;
    wdiskinfo_t disk;

    wmeminfo(&mem);
    wdiskinfo(&disk);
    proc_count = wproclist(procs, MAX_PROCS);

    if (proc_count > 0 && selected >= proc_count)
        selected = proc_count - 1;

    draw_header();
    draw_summary(&mem, &disk);
    draw_process_table();
    draw_footer();
}

int main(int argc, char **argv)
{
    int running = 1;

    wconsize(&rows, &cols);
    if (cols < 72) cols = 72;      /* the table needs a minimum width */
    if (rows < 14) rows = 14;

    wconsole_raw(W_CONSOLE_RAW);
    wcursor(0);
    wcls();

    while (running) {
        redraw();

        /* Wait out the refresh interval, but react to a key the moment one
         * arrives rather than sleeping through it. */
        unsigned until = wticks() + REFRESH_TICKS;
        while (wticks() < until) {
            if (!wpollin(W_STDIN)) {
                wyield();
                continue;
            }

            int key = wgetkey();

            if (key == 'q' || key == 'Q' || key == 0x03) {
                running = 0;
            } else if ((key == W_KEY_UP || key == 'k') && selected > 0) {
                selected--;
            } else if ((key == W_KEY_DOWN || key == 'j') &&
                       selected + 1 < proc_count) {
                selected++;
            } else if (key == W_KEY_HOME) {
                selected = 0;
            } else if (key == W_KEY_END && proc_count > 0) {
                selected = proc_count - 1;
            }
            break;      /* redraw immediately, whatever the key was */
        }
    }

    /* Both the cursor and the input mode outlive this process, so they have
     * to be handed back the way they were found. */
    wcursor(1);
    wconsole_raw(W_CONSOLE_CANONICAL);
    wcls();

    return 0;
}
