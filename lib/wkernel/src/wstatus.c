/* A line of text with the machine's figures in it.  See wstatus.h. */

#include <wstatus.h>

/* A share of everything the processors did, as a percentage.
 *
 * Both counts are cumulative since boot, so the figure is the difference
 * between two readings and not either reading: a single sample is the average
 * since the machine started, which on a machine that has been up for an hour
 * says nothing about now. */
static int cpu_percent(void)
{
    static wcpu_t   cores[W_CPU_MAX];
    static uint32_t last_busy, last_idle;
    static int      percent;

    int count = wcpulist(cores, W_CPU_MAX);

    if (count <= 0)
        return percent;

    uint32_t busy = 0, idle = 0;

    for (int i = 0; i < count; i++) {
        busy += cores[i].busy_ticks;
        idle += cores[i].idle_ticks;
    }

    uint32_t d_busy = busy - last_busy;
    uint32_t d_idle = idle - last_idle;

    last_busy = busy;
    last_idle = idle;

    /* No whole tick has passed: the previous figure stands rather than the
     * reading dropping to nothing every time it is asked twice quickly. */
    if (d_busy + d_idle > 0)
        percent = (int)(d_busy * 100 / (d_busy + d_idle));

    return percent;
}

static int mem_percent(void)
{
    wmeminfo_t m;

    if (wmeminfo(&m) < 0 || m.total_bytes == 0)
        return 0;

    return (int)(m.used_bytes * 100 / m.total_bytes);
}

/* The charge, and what it is doing.
 *
 * Everything here can be missing, and each absence means something different.
 * A machine with no battery says nothing at all rather than 0%.  A machine
 * with a battery whose charge cannot be read says so -- that is a laptop whose
 * firmware this kernel could not run, and printing a number there would be
 * inventing one.  Only on mains, with no pack, is the word on its own enough.
 */
static void battery_text(char *out, wsize_t size)
{
    wbattery_t b;

    out[0] = '\0';

    if (wbattery(&b) < 0 || (!b.present && b.ac_online < 0))
        return;

    if (!b.present) {
        strlcpy(out, b.ac_online ? "AC" : "", size);
        return;
    }

    /* A word rather than a symbol.  The arrows a real bar uses are not in the
     * 8x16 font, and the obvious ASCII stand-ins do not survive being put in
     * front of a number: "-50%" reads as minus fifty. */
    const char *what = (b.state == W_BATTERY_CHARGING)    ? "CHG"
                     : (b.state == W_BATTERY_DISCHARGING) ? "BAT"
                                                          : "AC";

    if (b.charge_percent < 0) {
        strlcpy(out, "BAT ?", size);
        return;
    }

    wsnprintf(out, size, "%s %d%%", what, b.charge_percent);
}

/* What each name says at the moment. */
static struct {
    char cpu[8];
    char mem[8];
    char time[8];
    char date[16];
    char battery[24];
} reading;

static void sample(void)
{
    static unsigned int last;
    static int          taken;

    /* wticks() counts hundredths of a second. */
    if (taken && (unsigned)(wticks() - last) < 100)
        return;

    last  = wticks();
    taken = 1;

    wsnprintf(reading.cpu, sizeof(reading.cpu), "%d%%", cpu_percent());
    wsnprintf(reading.mem, sizeof(reading.mem), "%d%%", mem_percent());

    battery_text(reading.battery, sizeof(reading.battery));

    wtime_t now;

    if (wtime_get(&now) == 0) {
        wsnprintf(reading.time, sizeof(reading.time), "%02d:%02d",
                  now.hour, now.minute);
        wsnprintf(reading.date, sizeof(reading.date), "%04d-%02d-%02d",
                  now.year, now.month, now.day);
    }
}

void wstatus_expand(const char *in, char *out, wsize_t size)
{
    static const struct {
        const char *name;
        const char *value;
    } names[] = {
        { "${CPU}",     reading.cpu     },
        { "${MEM}",     reading.mem     },
        { "${TIME}",    reading.time    },
        { "${DATE}",    reading.date    },
        { "${BATTERY}", reading.battery },
    };

    wsize_t len = 0;

    if (!out || !size)
        return;

    out[0] = '\0';

    if (!in || !*in)
        return;

    sample();

    while (*in && len + 1 < size) {
        const char *value = NULL;

        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            wsize_t n = strlen(names[i].name);

            if (strncmp(in, names[i].name, n) == 0) {
                value = names[i].value;
                in += n;
                break;
            }
        }

        if (!value) {
            out[len++] = *in++;
            continue;
        }

        for (const char *v = value; *v && len + 1 < size; v++)
            out[len++] = *v;
    }

    out[len] = '\0';
}
