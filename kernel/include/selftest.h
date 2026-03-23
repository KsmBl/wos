/* Boot-time self-tests.
 *
 * These run on every boot rather than living in a separate test harness: an OS
 * has no unit-test runner to fall back on, and a broken allocator or a missing
 * timer tick is far cheaper to find here than three subsystems later.
 */
#ifndef WOS_SELFTEST_H
#define WOS_SELFTEST_H

#include "types.h"

void selftest_interrupts(void);
void selftest_memory(void);

/* Exercises the disk and filesystem, and maintains /home/boots.txt as a
 * boot counter -- rebooting without rebuilding the image should show it
 * climbing, which is the proof that writes really reach the disk. */
void selftest_filesystem(void);

/* Deliberately dereference a null pointer to show the page-fault handler
 * working.  Panics by design; only call it as the last thing in a boot. */
void selftest_page_fault(void) __attribute__((noreturn));

#endif /* WOS_SELFTEST_H */
