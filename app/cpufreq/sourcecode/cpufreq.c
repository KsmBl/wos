/* cpufreq -- show the processor's clock, and change it.
 *
 *   cpufreq              what it is doing now (the same as `cpufreq get`)
 *   cpufreq get
 *   cpufreq set <MHz>    hold the clock at that speed
 *   cpufreq inc <MHz>    ask for that much more
 *   cpufreq dec <MHz>    ask for that much less
 *   cpufreq auto         hand the decision back to the hardware
 *   cpufreq tui          a full-screen slider
 *
 * Everything is in megahertz, because that is the unit the numbers on a
 * processor are quoted in.  The hardware moves in steps of its bus reference
 * -- 100 MHz on anything recent -- so a request lands on the nearest step
 * rather than exactly where it was pointed, and the reply says where it went.
 *
 * Changing the clock needs root or the `editfreq` role.  There is one clock
 * and every process on the machine runs on it: a slow machine is slow for
 * everybody, and a fast one is hot for everybody.
 */

#include <wkernel.h>

static wcpuinfo_t cpu;

/* How fast the processor is going right now, and how hot it is, from the core
 * this program happens to be running on -- which is the only one WOS starts.
 * Either can come back unknown on a machine that will not say. */
static void read_core(unsigned *khz, int *temp)
{
    wcpu_t cores[W_CPU_MAX];
    int n = wcpulist(cores, W_CPU_MAX);

    *khz  = 0;
    *temp = W_TEMP_UNKNOWN;

    for (int i = 0; i < n; i++) {
        if (cores[i].online) {
            *khz  = cores[i].clock_khz;
            *temp = cores[i].temp_c;
            return;
        }
    }
}

/* The clock a change should be measured from: what it is being held at, or
 * failing that what it is doing, so that `inc` on an unpinned machine starts
 * from something real rather than from zero. */
static unsigned current_target(void)
{
    if (cpu.pinned_khz)
        return cpu.pinned_khz;

    unsigned khz;
    int      temp;

    read_core(&khz, &temp);
    return khz ? khz : cpu.base_khz;
}

static int apply(int khz)
{
    int r = wcpufreq(khz);

    if (r == -W_EPERM) {
        wfprintf(W_STDERR, "cpufreq: not permitted -- needs root or the "
                           "editfreq role\n");
        return 1;
    }
    if (r == -W_ENODEV) {
        wfprintf(W_STDERR, "cpufreq: this machine does not let the clock be "
                           "set\n");
        return 1;
    }
    if (r < 0) {
        wfprintf(W_STDERR, "cpufreq: %s\n", wstrerror(-r));
        return 1;
    }

    if (khz <= 0)
        wprintf("clock: back to the hardware's own judgement\n");
    else
        wprintf("clock: held at %s\n", wclock_string((unsigned)r));

    return 0;
}

/* ------------------------------------------------------------------ *
 *  cpufreq get
 * ------------------------------------------------------------------ */

static const char *how_measured(unsigned source)
{
    switch (source) {
    case W_CLOCK_APERF: return "measured";
    case W_CLOCK_TSC:   return "the base clock; this machine cannot be "
                               "measured";
    case W_CLOCK_CPUID: return "the base clock the CPU quotes; this machine "
                               "cannot be measured";
    default:            return "unknown";
    }
}

static int show(void)
{
    wcpu_t cores[W_CPU_MAX];
    int    n = wcpulist(cores, W_CPU_MAX);

    if (cpu.brand[0])
        wprintf("cpu    : %s\n", cpu.brand);

    for (int i = 0; i < n; i++) {
        wprintf("core %-2d: ", cores[i].id);

        if (!cores[i].online) {
            wprintf("not started by WOS -- nothing to report\n");
            continue;
        }

        wprintf("%s (%s)", wclock_string(cores[i].clock_khz),
                how_measured(cores[i].clock_source));
        if (cores[i].temp_c != W_TEMP_UNKNOWN)
            wprintf(", %dC of %dC", cores[i].temp_c, cores[i].temp_max_c);
        wprintf("\n");
    }

    if (cpu.min_khz)
        wprintf("range  : %s to %s", wclock_string(cpu.min_khz),
                wclock_string(cpu.max_khz));
    else
        wprintf("range  : up to %s (this machine will not say how slow it "
                "will go)", wclock_string(cpu.max_khz));

    if (cpu.step_khz)
        wprintf(", in steps of %s", wclock_string(cpu.step_khz));
    wprintf("\n");

    if (!cpu.settable)
        wprintf("setting: this machine does not let the clock be set\n");
    else if (cpu.pinned_khz)
        wprintf("setting: held at %s\n", wclock_string(cpu.pinned_khz));
    else
        wprintf("setting: automatic -- the hardware chooses\n");

    return 0;
}

/* ------------------------------------------------------------------ *
 *  cpufreq tui
 * ------------------------------------------------------------------ */

#define BAR_WIDTH  40
#define REFRESH_MS 250

/* The low end of the slider.  A machine that will not say how slow it goes
 * still cannot be asked for less than one step, so that is where the scale
 * starts -- an honest floor rather than an invented one. */
static unsigned slider_low(void)
{
    if (cpu.min_khz)
        return cpu.min_khz;
    return cpu.step_khz ? cpu.step_khz : 100000;
}

/* Where `khz` falls between the slowest and fastest this machine will go, as a
 * count of bar characters. */
static int bar_fill(unsigned khz)
{
    unsigned lo = slider_low();
    unsigned hi = cpu.max_khz;

    if (hi <= lo || khz <= lo)
        return 0;
    if (khz >= hi)
        return BAR_WIDTH;

    return (int)((khz - lo) * BAR_WIDTH / (hi - lo));
}

static void tui_draw(unsigned now_khz, int temp)
{
    wgotoxy(1, 1);
    wcolor(W_BLACK, W_CYAN);
    wprintf(" cpufreq ");
    wcolor_reset();
    wprintf("  %s", cpu.brand[0] ? cpu.brand : "the processor's clock");
    wclear_line();

    /* The slider: where the clock is, against the range the machine admits
     * to. */
    int filled = bar_fill(now_khz);

    wgotoxy(3, 3);
    wprintf("%8s [", wclock_string(slider_low()));
    wcolor(W_GREEN | W_BRIGHT, W_DEFAULT);
    for (int i = 0; i < filled; i++)
        wprintf("|");
    wcolor_reset();
    for (int i = filled; i < BAR_WIDTH; i++)
        wprintf(" ");
    wprintf("] %-8s", wclock_string(cpu.max_khz));
    wclear_line();

    wgotoxy(5, 3);
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("now");
    wcolor_reset();
    wprintf("    %-10s", wclock_string(now_khz));

    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("held at");
    wcolor_reset();
    wprintf(" %-10s", cpu.pinned_khz ? wclock_string(cpu.pinned_khz)
                                     : "automatic");

    if (temp != W_TEMP_UNKNOWN) {
        wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
        wprintf("temp");
        wcolor_reset();
        wprintf(" %dC", temp);
    }
    wclear_line();

    wgotoxy(7, 3);
    if (!cpu.settable) {
        wcolor(W_YELLOW | W_BRIGHT, W_DEFAULT);
        wprintf("This machine does not let the clock be set; "
                "the reading is live.");
        wcolor_reset();
    }
    wclear_line();

    wgotoxy(9, 1);
    wcolor(W_BLACK, W_CYAN);
    wprintf(" up/dn ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" One step ");
    wcolor(W_BLACK, W_CYAN);
    wprintf(" home/end ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" Slowest / fastest ");
    wcolor(W_BLACK, W_CYAN);
    wprintf(" a ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" Automatic ");
    wcolor(W_BLACK, W_CYAN);
    wprintf(" q ");
    wcolor(W_WHITE, W_BLACK);
    wprintf(" Quit ");
    wcolor_reset();
    wclear_line();

    /* Whatever the last action said, good or bad. */
    wgotoxy(11, 3);
}

static void tui(void)
{
    int prev = wconsole_raw(W_CONSOLE_RAW);
    wcursor(0);
    wcls();

    char message[80] = "";
    int  running = 1;

    while (running) {
        unsigned now_khz;
        int      temp;

        read_core(&now_khz, &temp);
        wcpuinfo(&cpu);                 /* pinned_khz can have changed */

        tui_draw(now_khz, temp);
        wprintf("%s", message);
        wclear_line();

        unsigned until = wticks() + REFRESH_MS / 10;
        int      key   = -1;

        while (wticks() < until) {
            if (!wpollin(W_STDIN)) {
                wsleep(20);
                continue;
            }
            key = wgetkey();
            break;
        }

        if (key < 0)
            continue;                   /* nothing pressed; just refresh */

        int want = -1;                  /* kHz to ask for, or -1 for none */
        unsigned step = cpu.step_khz ? cpu.step_khz : 100000;

        switch (key) {
        case 'q': case 'Q': case 0x03:
            running = 0;
            continue;
        case W_KEY_UP: case W_KEY_RIGHT: case '+': case '=':
            want = (int)(current_target() + step);
            break;
        case W_KEY_DOWN: case W_KEY_LEFT: case '-': case '_':
            want = (int)current_target() - (int)step;
            if (want < 0)
                want = 0;
            break;
        case W_KEY_HOME:
            want = (int)slider_low();
            break;
        case W_KEY_END:
            want = (int)cpu.max_khz;
            break;
        case 'a': case 'A':
            want = 0;
            break;
        default:
            continue;                   /* any other key just refreshes */
        }

        int r = wcpufreq(want);
        if (r == -W_EPERM)
            strlcpy(message, "not permitted -- needs root or the editfreq role",
                    sizeof(message));
        else if (r == -W_ENODEV)
            strlcpy(message, "this machine does not let the clock be set",
                    sizeof(message));
        else if (r < 0)
            wsnprintf(message, sizeof(message), "%s", wstrerror(-r));
        else if (want == 0)
            strlcpy(message, "back to the hardware's own judgement",
                    sizeof(message));
        else
            wsnprintf(message, sizeof(message), "held at %s",
                      wclock_string((unsigned)r));
    }

    wcursor(1);
    wconsole_raw(prev);
    wcls();
}

/* ------------------------------------------------------------------ *
 *  Arguments
 * ------------------------------------------------------------------ */

static void usage(void)
{
    wfprintf(W_STDERR, "usage: cpufreq [get | set <MHz> | inc <MHz> | "
                       "dec <MHz> | auto | tui]\n");
    wfprintf(W_STDERR, "  get         what the clock is doing (the default)\n");
    wfprintf(W_STDERR, "  set <MHz>   hold it there\n");
    wfprintf(W_STDERR, "  inc <MHz>   ask for that much more\n");
    wfprintf(W_STDERR, "  dec <MHz>   ask for that much less\n");
    wfprintf(W_STDERR, "  auto        let the hardware decide again\n");
    wfprintf(W_STDERR, "  tui         a full-screen slider\n");
    wfprintf(W_STDERR, "\nChanging it needs root or the editfreq role.\n");
}

/* An amount in megahertz.  Negative means it was not a number. */
static int mhz_arg(int argc, char **argv)
{
    if (argc < 3) {
        wfprintf(W_STDERR, "cpufreq: %s needs an amount in MHz\n", argv[1]);
        return -1;
    }

    int mhz = atoi(argv[2]);
    if (mhz <= 0) {
        wfprintf(W_STDERR, "cpufreq: '%s' is not an amount in MHz\n", argv[2]);
        return -1;
    }
    return mhz;
}

int main(int argc, char **argv)
{
    int r = wcpuinfo(&cpu);
    if (r < 0) {
        wfprintf(W_STDERR, "cpufreq: %s\n", wstrerror(-r));
        return 1;
    }

    if (argc < 2 || strcmp(argv[1], "get") == 0)
        return show();

    if (strcmp(argv[1], "tui") == 0) {
        tui();
        return 0;
    }

    if (strcmp(argv[1], "auto") == 0)
        return apply(0);

    if (strcmp(argv[1], "set") == 0) {
        int mhz = mhz_arg(argc, argv);
        return mhz < 0 ? 1 : apply(mhz * 1000);
    }

    if (strcmp(argv[1], "inc") == 0) {
        int mhz = mhz_arg(argc, argv);
        return mhz < 0 ? 1 : apply((int)current_target() + mhz * 1000);
    }

    if (strcmp(argv[1], "dec") == 0) {
        int mhz = mhz_arg(argc, argv);
        if (mhz < 0)
            return 1;

        int want = (int)current_target() - mhz * 1000;
        if (want < 1000)
            want = 1000;      /* still a request to slow down, not to go auto */
        return apply(want);
    }

    usage();
    return 1;
}
