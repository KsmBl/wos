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

/* The same instant as one number: seconds since 1970-01-01 00:00:00, which is
 * what a filesystem stores and what comparing two files for age needs.  The RTC
 * is read as UTC, since nothing here knows about time zones.
 *
 * Called on every write that changes an inode.  That is affordable because the
 * whole path runs under the kernel lock -- the CMOS is one pair of ports and
 * two processors reading it at once would interleave their accesses -- and
 * because a handful of port reads costs nothing beside the disk write it
 * accompanies. */
uint32_t rtc_epoch(void);

#endif /* WOS_RTC_H */
