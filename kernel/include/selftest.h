/* Boot-time self-tests.
 *
 * These run on every boot rather than living in a separate test harness: an OS
 * has no unit-test runner to fall back on, and a broken allocator or a missing
 * timer tick is far cheaper to find here than three subsystems later.
 *
 * `make SELFTEST=0` builds without them, for a boot that goes straight to the
 * shell.  The tests then are not skipped but absent: selftest.c compiles to
 * nothing and the calls below vanish, so nothing of them is left in the kernel.
 */
#ifndef WOS_SELFTEST_H
#define WOS_SELFTEST_H

#include "types.h"

#ifdef WOS_NO_SELFTEST

static inline void selftest_interrupts(void) { }
static inline void selftest_memory(void)     { }
static inline void selftest_filesystem(void) { }
static inline void selftest_ramdisk(void)    { }
static inline void selftest_processes(void)  { }

#else

void selftest_interrupts(void);
void selftest_memory(void);

/* Exercises the disk and filesystem, and maintains /home/boots.txt as a
 * boot counter -- rebooting without rebuilding the image should show it
 * climbing, which is the proof that writes really reach the disk. */
void selftest_filesystem(void);

/* The in-memory filesystem: that it stores what is written, and that it holds
 * no more memory than the files in it need. */
void selftest_ramdisk(void);

/* Spawns the `hello` test program in ring 3: checks argument passing, exit
 * status, preemption between two processes, and that a faulting process is
 * killed without taking the system down. */
void selftest_processes(void);

/* Deliberately dereference a null pointer to show the page-fault handler
 * working.  Panics by design; only call it as the last thing in a boot. */
void selftest_page_fault(void) __attribute__((noreturn));

#endif /* WOS_NO_SELFTEST */

#endif /* WOS_SELFTEST_H */
