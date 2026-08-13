/* The clock, in the shapes C asks for it.
 *
 * wkernel reports the time as a date already broken down, because that is what
 * something printing a clock wants.  C asks for a count of seconds instead, so
 * this converts between the two -- and back again for localtime(), which wants
 * the broken-down form after all.
 */

#include <time.h>

#include "wkernel.h"

time_t time(time_t *t)
{
    wtime_t now;
    time_t  seconds = 0;

    if (wtime_get(&now) == 0)
        seconds = (time_t)wtime_to_epoch(&now);

    if (t)
        *t = seconds;

    return seconds;
}

clock_t clock(void)
{
    /* Timer ticks since this program started would be the right answer and
     * there is nothing to read it from: wticks() counts from boot, for the
     * whole machine.  A program measuring itself subtracts two of these, which
     * is what clock() is for, and the difference is right even though neither
     * end is. */
    return (clock_t)wticks();
}

/* Days in each month of a non-leap year. February is corrected where it is
 * used, which is twice, rather than by keeping two tables. */
static const int month_days[12] = { 31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31 };

static int is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

struct tm *gmtime(const time_t *t)
{
    /* One structure, overwritten by every call: the standard's arrangement,
     * and the reason a program that wants to keep a result copies it. */
    static struct tm broken;

    wtime_t when;
    wtime_from_epoch((unsigned int)(t ? *t : 0), &when);

    broken.tm_sec   = when.second;
    broken.tm_min   = when.minute;
    broken.tm_hour  = when.hour;
    broken.tm_mday  = when.day;
    broken.tm_mon   = when.month - 1;      /* C counts months from zero */
    broken.tm_year  = when.year - 1900;
    broken.tm_isdst = 0;

    /* 1970-01-01 was a Thursday, which is where the 4 comes from. */
    long days = (t ? *t : 0) / 86400;
    broken.tm_wday = (int)((days + 4) % 7);

    int yday = 0;
    for (int m = 1; m < when.month; m++) {
        yday += month_days[m - 1];
        if (m == 2 && is_leap(when.year))
            yday++;
    }
    broken.tm_yday = yday + when.day - 1;

    return &broken;
}

/* There are no time zones on this machine, so local time is UTC.  Saying that
 * plainly beats a conversion that would add zero. */
struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}

time_t mktime(struct tm *tm)
{
    wtime_t when;

    when.year   = tm->tm_year + 1900;
    when.month  = tm->tm_mon + 1;
    when.day    = tm->tm_mday;
    when.hour   = tm->tm_hour;
    when.minute = tm->tm_min;
    when.second = tm->tm_sec;

    return (time_t)wtime_to_epoch(&when);
}
