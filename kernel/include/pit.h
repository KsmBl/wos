/* Programmable Interval Timer (8253/8254), channel 0.
 *
 * Drives IRQ0 at a fixed frequency; this is both the system uptime source and
 * the preemption tick for the scheduler.
 */
#ifndef WOS_PIT_H
#define WOS_PIT_H

#include "types.h"

#define PIT_HZ 100u     /* one tick every 10 ms */

void pit_init(uint32_t frequency);

/* Ticks since boot. Wraps after roughly 497 days at 100 Hz. */
uint32_t pit_ticks(void);

/* Milliseconds since boot. */
uint32_t pit_uptime_ms(void);

/* Milliseconds since boot, from a source that cannot be blocked.
 *
 * pit_uptime_ms() counts interrupts, and an interrupt has to be delivered and
 * handled before it counts.  That makes it useless -- worse than useless -- to
 * anything waiting with the kernel lock held, because the tick arrives on the
 * boot processor and the handler must take that same lock to run.  A wait on
 * one processor then blocks the clock on another, the deadline never passes,
 * and the machine stops with every core alive and none of them able to move.
 *
 * That is not a hypothetical: it is what froze this system the first time it
 * was asked to ping anything from a processor other than the first.
 *
 * This reads the processor's own cycle counter instead.  Nothing has to be
 * delivered, nothing has to be acknowledged, and no lock can hold it up, so a
 * deadline expressed in these always arrives.  Every timeout that runs inside
 * the kernel should use it. */
uint64_t time_now_ms(void);

/* Busy-wait for `ms` milliseconds. Requires interrupts to be enabled. */
void pit_sleep(uint32_t ms);

#endif /* WOS_PIT_H */
