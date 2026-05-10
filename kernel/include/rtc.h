/* The CMOS real-time clock -- wall-clock date and time.
 *
 * Reached through the index/data port pair 0x70/0x71.  QEMU emulates it and
 * seeds it from the host clock, so reading gives the real time; writing sets
 * the emulated clock for the rest of the run.
 */
#ifndef WOS_RTC_H
#define WOS_RTC_H

#include "wabi.h"

/* Read the current date and time. */
void rtc_read(wtime_t *out);

/* Set the date and time. */
void rtc_set(const wtime_t *t);

#endif /* WOS_RTC_H */
