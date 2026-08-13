/* <time.h> -- the wall clock, and how long this program has been running.
 *
 * There are no time zones on this machine and nothing that knows about them,
 * so localtime() and gmtime() are the same function: the CMOS clock is read as
 * UTC and reported as it stands.  Saying so is better than pretending to a
 * conversion that would be the identity anyway.
 */
#ifndef WLIBC_TIME_H
#define WLIBC_TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

/* clock() counts in timer ticks, and the timer runs at 100 Hz. */
#define CLOCKS_PER_SEC 100

struct tm {
    int tm_sec;     /* 0-59                        */
    int tm_min;     /* 0-59                        */
    int tm_hour;    /* 0-23                        */
    int tm_mday;    /* 1-31                        */
    int tm_mon;     /* 0-11 -- January is 0        */
    int tm_year;    /* years since 1900            */
    int tm_wday;    /* 0-6 -- Sunday is 0          */
    int tm_yday;    /* 0-365                       */
    int tm_isdst;   /* always 0: no daylight saving here */
};

/* Seconds since 1970-01-01 UTC, from the machine's clock.  Also stored through
 * `t` when that is not NULL, as the standard has it. */
time_t time(time_t *t);

/* Processor time this program has used, in CLOCKS_PER_SEC units. */
clock_t clock(void);

/* Both fill in the same static structure, which the next call overwrites --
 * the standard's rule, and the reason a program that wants to keep one copies
 * it. */
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);

/* The other direction: a broken-down time back to seconds.  The fields are
 * taken as given; nothing is normalised. */
time_t mktime(struct tm *tm);

#endif /* WLIBC_TIME_H */
