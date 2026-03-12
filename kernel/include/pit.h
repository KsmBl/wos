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

/* Busy-wait for `ms` milliseconds. Requires interrupts to be enabled. */
void pit_sleep(uint32_t ms);

#endif /* WOS_PIT_H */
