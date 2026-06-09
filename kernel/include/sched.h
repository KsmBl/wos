/* The scheduler: preemptive round-robin over runnable threads. */
#ifndef WOS_SCHED_H
#define WOS_SCHED_H

#include "types.h"
#include "proc.h"

void sched_init(thread_t *idle);

/* Add a thread to the run queue. */
void sched_add(thread_t *t);

/* Remove a thread from the run queue (used when it becomes a zombie). */
void sched_remove(thread_t *t);

/* Pick the next runnable thread and switch to it.  Safe to call from a
 * syscall, from an IRQ handler, or from a thread giving up the CPU. */
void schedule(void);

/* Give up the rest of this timeslice. */
void sched_yield(void);

/* Block the current thread until something wakes `reason`. */
void sched_block(wait_reason_t reason);

/* Make every thread blocked on `reason` runnable again. Safe from an IRQ. */
void sched_wake(wait_reason_t reason);

/* Block on `reason`, but no longer than `until_tick`.  Whichever comes first
 * wakes the thread. */
void sched_block_until(wait_reason_t reason, uint32_t until_tick);

/* Block the current thread until the tick counter reaches `until_tick`.
 * Returns as soon as possible after that, not exactly then: the thread has to
 * be scheduled again like any other. */
void sched_sleep_until(uint32_t until_tick);

/* True once the scheduler is running; before that, blocking is not possible. */
bool sched_active(void);

/* True when the processor has nothing to run and is sitting in the idle
 * thread.  This is what makes a tick countable as busy or not, and so is the
 * whole basis of a CPU usage figure. */
bool sched_current_is_idle(void);

#endif /* WOS_SCHED_H */
