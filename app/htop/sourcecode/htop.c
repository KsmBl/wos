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
#define POLL_MS       30           /* how often to look for a keystroke */
#define MAX_PROCS     32

/* A meter narrower than this has no bar left once the label and the reading
 * are in it, so a console too thin for two columns gets one. */
#define METER_MIN_WIDTH 34
#define METER_MAX_WIDTH 56

/* Every meter's label is padded to this, so the bars of the CPUs, the memory
 * and the disks all start in the same column.  It is the width of "/ramdisk",
 * the longest label there is. */
#define LABEL_WIDTH 8

static wprocmem_t procs[MAX_PROCS];
static int        proc_count;
static int        selected;

/* The console size, read at startup so htop fills whatever resolution is set
 * rather than assuming 80x25. */
static int cols = 80, rows = 25;

/* The processors, and the tick counts they were last seen with: a load is a
 * change over an interval, so the previous sample has to be kept to have an
 * interval at all. */
static wcpuinfo_t cpu;
static wcpu_t     cores[W_CPU_MAX];
static int        core_count;
static unsigned   last_busy[W_CPU_MAX];
static unsigned   last_idle[W_CPU_MAX];
static unsigned   load_tenths[W_CPU_MAX];

static wdisk_t disks[W_DISK_MAX];
static int     disk_count;

/* Express used/total out of `out_of`, in 32-bit arithmetic only.
 *
 * WOS links no libgcc, so there is no 64-bit division to fall back on: the
 * obvious `used * out_of / total` would overflow on a multi-gigabyte total.
 * Halving both sides keeps the ratio while bringing the product back in
 * range. */
static unsigned scale_to(unsigned long used, unsigned long total, unsigned out_of)
{
    if (total == 0 || out_of == 0)
        return 0;

    while (used > (unsigned long)-1 / out_of) {
        used  /= 2;
        total /= 2;
    }

    if (total == 0)
        return out_of;

    return used * out_of / total;
}

/* Colour a meter by how full it is, the way htop does: green while there is
 * plenty, yellow as it fills, red when it is nearly gone. */
static int meter_colour(unsigned long used, unsigned long total)
{
    unsigned percent = scale_to(used, total, 100);

    if (percent >= 90)
        return W_RED | W_BRIGHT;
    if (percent >= 70)
        return W_YELLOW | W_BRIGHT;
    return W_GREEN | W_BRIGHT;
}

/* One labelled bar: "Mem     [||||||      5.3M/255.8M]".
 *
 * `width` is the whole field, label and brackets included, so that meters laid
 * out side by side can be given a column each and will fill it.  `dim` draws
 * the bar in grey for a reading that is not a measurement -- a core the kernel
 * never started has nothing to show, and an empty green bar would say it was
 * idle rather than absent. */
static void draw_meter(int row, int col, int width, const char *label,
                       unsigned long used, unsigned long total,
                       const char *reading, int dim)
{
    int bar = width - LABEL_WIDTH - 2;
    if (bar < 1)
        bar = 1;

    unsigned filled = dim ? 0 : scale_to(used, total, (unsigned)bar);
    if (filled > (unsigned)bar)
        filled = (unsigned)bar;

    /* A non-zero amount should always show at least one bar, or a small but
     * real allocation looks like nothing at all. */
    if (filled == 0 && used > 0 && !dim)
        filled = 1;

    wgotoxy(row, col);
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("%-*s", LABEL_WIDTH, label);
    wcolor_reset();
    wprintf("[");

    wcolor(meter_colour(used, total), W_DEFAULT);
    for (unsigned i = 0; i < filled; i++)
        wprintf("|");
    wcolor_reset();
    for (int i = (int)filled; i < bar; i++)
        wprintf(" ");

    /* The reading sits inside the bracket, right-aligned, as htop does. */
    int len = (int)strlen(reading);
    if (len > bar)
        len = bar;

    wgotoxy(row, col + LABEL_WIDTH + 1 + bar - len);
    wcolor(dim ? W_WHITE : (W_WHITE | W_BRIGHT), W_DEFAULT);
    wprintf("%.*s", len, reading);
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

    /* What the machine calls itself, when it would say and there is room for
     * it beside the uptime; otherwise what this program is. */
    const char *what = cpu.brand[0] ? cpu.brand : "a process monitor for WOS";
    int room = cols - 8 - 20;
    if (room < 1)
        room = 1;

    wprintf("  %.*s", room, what);
    wclear_line();

    draw_uptime();
}

/* ------------------------------------------------------------------ *
 *  The meters
 * ------------------------------------------------------------------ */

/* What a core is doing, in the space of a meter's reading: how busy, how fast
 * and how hot -- leaving out whichever of those this machine will not say
 * rather than printing a zero and calling it a measurement. */
static void core_reading(int i, char *out, wsize_t size)
{
    const wcpu_t *c = &cores[i];

    if (!c->online) {
        strlcpy(out, "not started", size);
        return;
    }

    wsize_t at = (wsize_t)wsnprintf(out, size, "%u.%u%%",
                                    load_tenths[i] / 10, load_tenths[i] % 10);

    if (c->clock_khz && at < size)
        at += (wsize_t)wsnprintf(out + at, size - at, "  %s",
                                 wclock_string(c->clock_khz));

    if (c->temp_c != W_TEMP_UNKNOWN && at < size)
        wsnprintf(out + at, size - at, "  %dC", c->temp_c);
}

/* Lay the per-core meters out in as many columns as the console is wide
 * enough for, and return the row after the last one.
 *
 * Every core the machine has gets a line, including the ones WOS never
 * started: they are part of the machine, and a monitor that listed only the
 * core it happens to be running on would report a four-core laptop as a
 * single-core one. */
static int draw_cpu_meters(int row, int width, int columns, int max_rows)
{
    int lines = (core_count + columns - 1) / columns;
    int shown = core_count;

    /* A machine with more cores than the console has room for gets as many as
     * fit and a count of the rest.  Silently dropping them would be a lie of
     * the same kind as not listing them at all. */
    if (lines > max_rows) {
        lines = max_rows > 0 ? max_rows : 1;
        shown = (lines - 1) * columns;
    }

    for (int i = 0; i < shown; i++) {
        char label[10], reading[40];

        wsnprintf(label, sizeof(label), "CPU%d", cores[i].id);
        core_reading(i, reading, sizeof(reading));

        int line = row + i / columns;
        int col  = 2 + (i % columns) * (width + 1);

        draw_meter(line, col, width, label,
                   load_tenths[i], 1000, reading, !cores[i].online);
    }

    if (shown < core_count) {
        wgotoxy(row + lines - 1, 2);
        wprintf("... and %d more core%s, off the bottom of this console",
                core_count - shown, core_count - shown == 1 ? "" : "s");
        wclear_line();
    }

    return row + lines;
}

static int draw_summary(int row, wmeminfo_t *mem)
{
    int columns = 1;
    while ((cols - 2) / (columns + 1) >= METER_MIN_WIDTH + 1 &&
           columns < core_count)
        columns++;

    int width = (cols - 2) / columns - (columns > 1 ? 1 : 0);
    if (width > METER_MAX_WIDTH)
        width = METER_MAX_WIDTH;

    /* What is left for the cores once the memory bar, the disks, the two
     * blank lines, the Tasks line, the table's heading, one process row and
     * the footer have had theirs. */
    int max_cpu_rows = rows - row - disk_count - 6;
    if (max_cpu_rows < 1)
        max_cpu_rows = 1;

    row = draw_cpu_meters(row, width, columns, max_cpu_rows);

    char reading[40];
    wsnprintf(reading, sizeof(reading), "%s/%s",
              whuman(mem->used_bytes), whuman(mem->total_bytes));
    draw_meter(row++, 2, width, "Mem", mem->used_bytes, mem->total_bytes,
               reading, 0);

    /* One bar per filesystem, labelled by where it is mounted.  The disk and
     * the scratch space in memory fill up independently, and a single figure
     * covering both would be a number describing nothing. */
    for (int i = 0; i < disk_count; i++) {
        wsnprintf(reading, sizeof(reading), "%s/%s",
                  whuman(disks[i].usage.used_bytes),
                  whuman(disks[i].usage.total_bytes));

        draw_meter(row++, 2, width, disks[i].mount,
                   disks[i].usage.used_bytes, disks[i].usage.total_bytes,
                   reading, 0);
    }

    row++;

    int threads = 0;
    for (int i = 0; i < proc_count; i++)
        threads += procs[i].thread_count;

    wgotoxy(row, 3);
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
    wprintf(": %s", whuman(mem->kernel_bytes));
    wclear_line();

    return row + 2;
}

/* ------------------------------------------------------------------ *
 *  The process table
 * ------------------------------------------------------------------ */

/* The heading and every row are this wide; the rest of the line is padding, so
 * that the reverse-video bar reaches the edge of the screen. */
#define TABLE_WIDTH 71

static void draw_process_table(int row, int max_rows)
{
    wgotoxy(row, 1);
    wcolor(W_BLACK, W_GREEN);
    wprintf("  %5s %-12s %9s %8s %8s %8s %8s %4s",
            "PID", "COMMAND", "RESIDENT", "CODE", "DATA", "HEAP", "STACK",
            "THR");
    for (int i = 0; i < cols - TABLE_WIDTH; i++)
        wprintf(" ");
    wcolor_reset();

    for (int i = 0; i < proc_count && i < max_rows; i++) {
        wgotoxy(row + 1 + i, 1);

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
            for (int c = 0; c < cols - TABLE_WIDTH; c++)
                wprintf(" ");
        }

        wcolor_reset();
        wclear_line();
    }

    /* Blank out rows left behind by a process that has since exited. */
    for (int i = proc_count; i < max_rows; i++) {
        wgotoxy(row + 1 + i, 1);
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

/* ------------------------------------------------------------------ *
 *  Sampling
 * ------------------------------------------------------------------ */

/* Take a new set of core readings and work out what each one has been doing
 * since the last set.
 *
 * A key pressed between refreshes redraws immediately, which can land here
 * with barely any time elapsed and no whole tick to divide; the previous
 * figure stands rather than the display dropping to zero every keystroke. */
static void sample_cores(void)
{
    core_count = wcpulist(cores, W_CPU_MAX);
    if (core_count < 0)
        core_count = 0;

    for (int i = 0; i < core_count; i++) {
        unsigned busy = cores[i].busy_ticks - last_busy[i];
        unsigned idle = cores[i].idle_ticks - last_idle[i];

        if (busy + idle > 0)
            load_tenths[i] = busy * 1000u / (busy + idle);

        last_busy[i] = cores[i].busy_ticks;
        last_idle[i] = cores[i].idle_ticks;
    }
}

static void redraw(void)
{
    wmeminfo_t mem;

    wmeminfo(&mem);

    disk_count = wdisklist(disks, W_DISK_MAX);
    if (disk_count < 0)
        disk_count = 0;

    sample_cores();

    proc_count = wproclist(procs, MAX_PROCS);
    if (proc_count > 0 && selected >= proc_count)
        selected = proc_count - 1;

    draw_header();

    int row = draw_summary(3, &mem);

    /* Whatever is left between the meters and the footer is the table. */
    int max_rows = rows - row - 2;
    if (max_rows < 1)
        max_rows = 1;

    draw_process_table(row, max_rows);
    draw_footer();
}

int main(int argc, char **argv)
{
    int running = 1;

    wconsize(&rows, &cols);
    if (cols < TABLE_WIDTH + 1) cols = TABLE_WIDTH + 1;  /* the table's minimum */
    if (rows < 14) rows = 14;

    /* Fixed for the life of the program: which processor this is does not
     * change, and neither does what it is called. */
    if (wcpuinfo(&cpu) < 0)
        cpu.count = 0;

    wconsole_raw(W_CONSOLE_RAW);
    wcursor(0);
    wcls();

    while (running) {
        redraw();

        /* Wait out the refresh interval, but react to a key soon after one
         * arrives rather than sleeping through it.
         *
         * Sleeping in short slices rather than yielding in a tight loop is
         * what lets the machine actually be idle while htop is on screen --
         * and a monitor that pinned the processor at 100% just by being open
         * would be measuring itself. */
        unsigned until = wticks() + REFRESH_TICKS;
        while (wticks() < until) {
            if (!wpollin(W_STDIN)) {
                wsleep(POLL_MS);
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
