/* time -- show or set the wall clock.
 *
 *   time                          print the current date and time
 *   time HH:MM[:SS]               set the time of day, keeping the date
 *   time YYYY-MM-DD HH:MM[:SS]    set the whole date and time
 *
 * The clock is the CMOS real-time clock; under QEMU it starts from the host's
 * time.  Setting it is root only, since it is a system-wide setting.
 */

#include <wkernel.h>

static const char *const weekday[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const month[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* Day of week by Sakamoto's method: 0 = Sunday. */
static int day_of_week(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void print_time(const wtime_t *t)
{
    int wd = day_of_week(t->year, t->month, t->day);
    wprintf("%s %s %d %d, %02d:%02d:%02d\n",
            weekday[wd], month[t->month], t->day, t->year,
            t->hour, t->minute, t->second);
}

static int scan_hms(const char *s, int *h, int *m, int *sec);

/* Parse "HH:MM" or "HH:MM:SS" into a wtime_t's time fields. */
static int parse_clock(const char *s, wtime_t *t)
{
    int h = -1, m = -1, sec = 0;
    if (scan_hms(s, &h, &m, &sec) < 2)
        return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59)
        return -1;
    t->hour = h; t->minute = m; t->second = sec;
    return 0;
}

/* A tiny "H:M:S" scanner, since the library has no sscanf. Returns the count
 * of fields filled (2 or 3). */
static int scan_hms(const char *s, int *h, int *m, int *sec)
{
    int n = 0, val = 0, have = 0;
    int *dst[3] = { h, m, sec };

    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); have = 1; }
        else if (*p == ':' || *p == '\0') {
            if (have && n < 3) { *dst[n++] = val; }
            val = 0; have = 0;
            if (*p == '\0') break;
        } else return n;
    }
    return n;
}

/* Parse "YYYY-MM-DD" into the date fields. */
static int parse_date(const char *s, wtime_t *t)
{
    int y = 0, mo = 0, d = 0, field = 0, val = 0, have = 0;
    int *dst[3] = { &y, &mo, &d };

    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); have = 1; }
        else if (*p == '-' || *p == '\0') {
            if (!have || field >= 3) return -1;
            *dst[field++] = val; val = 0; have = 0;
            if (*p == '\0') break;
        } else return -1;
    }
    if (field != 3 || mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1970)
        return -1;
    t->year = y; t->month = mo; t->day = d;
    return 0;
}

int main(int argc, char **argv)
{
    wtime_t now;
    if (wtime_get(&now) < 0) {
        wfprintf(W_STDERR, "time: cannot read the clock\n");
        return 1;
    }

    if (argc == 1) {
        print_time(&now);
        return 0;
    }

    /* Setting: either "HH:MM[:SS]" alone, or "YYYY-MM-DD HH:MM[:SS]". */
    wtime_t set = now;
    const char *clock;

    if (argc == 2) {
        clock = argv[1];
    } else if (argc == 3) {
        if (parse_date(argv[1], &set) < 0) {
            wfprintf(W_STDERR, "time: bad date '%s' (want YYYY-MM-DD)\n", argv[1]);
            return 2;
        }
        clock = argv[2];
    } else {
        wfprintf(W_STDERR, "usage: time [YYYY-MM-DD] [HH:MM[:SS]]\n");
        return 2;
    }

    if (parse_clock(clock, &set) < 0) {
        wfprintf(W_STDERR, "time: bad time '%s' (want HH:MM[:SS])\n", clock);
        return 2;
    }

    int r = wtime_set(&set);
    if (r < 0) {
        if (-r == W_EPERM)
            wfprintf(W_STDERR, "time: only root may set the clock\n");
        else
            wfprintf(W_STDERR, "time: %s\n", wstrerror(-r));
        return 1;
    }

    wtime_get(&now);
    wprintf("Clock set to: ");
    print_time(&now);
    return 0;
}
