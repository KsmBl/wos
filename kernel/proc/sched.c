/* Round-robin scheduler.
 *
 * One circular run queue holds every thread that is not unused.  Blocked and
 * zombie threads stay on the list but are skipped, which keeps insertion and
 * removal cheap and means waking a thread is a single state change.
 */

#include "sched.h"
#include "smp.h"
#include "gdt.h"
#include "isr.h"
#include "pit.h"
#include "kprintf.h"

extern void switch_context(uint64_t *save_rsp, uint64_t new_rsp);

/* The run queue is the machine's, and every processor picks from it.  What
 * used to be `current` and `idle_thread` are now per processor, because they
 * are the two things that differ between them: which thread this core is
 * running, and which one it falls back to when there is nothing. */
static thread_t *run_queue;       /* circular, may be NULL before init */
static bool      active;

#define this_current (smp_this()->current)
#define this_idle    (smp_this()->idle)

bool sched_active(void) { return active; }

thread_t *sched_current_thread(void) { return this_current; }

/* Before the scheduler starts, the boot context is doing real work and there
 * is no idle thread yet -- so those ticks count as busy, which is what they
 * are. */
bool sched_current_is_idle(void)
{
    return active && this_current && this_current == this_idle;
}

void sched_add(thread_t *t)
{
    if (!run_queue) {
        t->next   = t;
        run_queue = t;
        return;
    }

    /* Insert just after the head; order does not matter for round-robin. */
    t->next = run_queue->next;
    run_queue->next = t;
}

void sched_remove(thread_t *t)
{
    if (!run_queue)
        return;

    thread_t *prev = run_queue;
    do {
        if (prev->next == t) {
            prev->next = t->next;
            if (run_queue == t)
                run_queue = (t->next == t) ? NULL : t->next;
            t->next = NULL;
            return;
        }
        prev = prev->next;
    } while (prev != run_queue);
}

void sched_init(thread_t *idle)
{
    sched_adopt_idle(idle);
    active = true;
}

/* This processor's idle thread.  Every core has one: it is the context the
 * core was already running in when it joined, so there is nothing to start --
 * only something to name, so that the scheduler has somewhere to go when the
 * run queue has nothing ready. */
void sched_adopt_idle(thread_t *idle)
{
    smpcpu_t *cpu = smp_this();

    cpu->idle    = idle;
    cpu->current = idle;

    idle->state   = THREAD_RUNNING;
    idle->is_idle = true;

    sched_add(idle);
}

/* Find the next thread that can run, starting after `from`.
 * Falls back to the idle thread when everything else is blocked.
 *
 * The idle thread is skipped rather than taken in turn.  It sits in the run
 * queue like any other thread, and round-robin would hand it a full timeslice
 * between every two slices of real work -- which it spends halted, waiting for
 * the timer.  A machine with one process to run was idle half the time with
 * something ready to go the whole while. */
static thread_t *pick_next(thread_t *from)
{
    if (!run_queue)
        return this_idle;

    thread_t *t = from ? from->next : run_queue;
    if (!t)
        t = run_queue;

    /* A thread another processor is running is in THREAD_RUNNING and is
     * therefore never picked here: the state field is what stops two cores
     * from taking the same thread, and every change to it happens behind the
     * kernel lock.  Every core's idle thread is skipped, not just this one's,
     * for the same reason it always was -- an idle thread taking a turn is a
     * processor halted with work waiting. */
    for (int i = 0; i < MAX_THREADS + 1; i++) {
        if (!t->is_idle && t->state == THREAD_READY)
            return t;
        /* The current thread may keep running if nothing else is ready. */
        if (t == from && t->state == THREAD_RUNNING)
            return t;
        t = t->next;
        if (!t)
            t = run_queue;
    }

    return this_idle;
}

void schedule(void)
{
    if (!active)
        return;

    smpcpu_t *cpu  = smp_this();
    thread_t *prev = cpu->current;
    thread_t *next = pick_next(prev);

    if (next == prev) {
        /* Nothing else to run. If the current thread can no longer run, the
         * idle thread must take over or we would return into a blocked
         * thread and never come back. */
        if (prev->state == THREAD_RUNNING)
            return;
        next = cpu->idle;
        if (next == prev)
            return;
    }

    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    next->state  = THREAD_RUNNING;
    cpu->current = next;

    /* The TSS tells the CPU which stack to switch to on the next ring 3 to
     * ring 0 transition, so it has to follow the thread. */
    tss_set_kernel_stack(next->kernel_stack + next->kernel_stack_size);

    if (next->proc && next->proc->space)
        paging_switch(next->proc->space);

    /* The kernel is handed over here rather than carried across.
     *
     * Holding the lock through the switch would give the kernel to whatever
     * thread is resumed next, which has not asked for it and may be about to
     * return to ring 3 -- and a processor that found nothing to run would
     * sleep holding it, which is the whole machine waiting behind one idle
     * core.  So it is released before the switch and taken again by whoever
     * comes back to this line, which is this processor, later, in whatever
     * thread it resumes.
     *
     * Everything this function decided is already recorded in the thread
     * states, under the lock, so another processor picking up work in the gap
     * cannot pick up this thread. */
    bool held = klock_held_here();

    if (held)
        klock_release();

    switch_context(&prev->rsp, next->rsp);

    if (held)
        klock_acquire();
}

void sched_yield(void)
{
    schedule();
}

void sched_block(wait_reason_t reason)
{
    if (!active) {
        /* Before the scheduler exists there is nothing to switch to, so the
         * only honest thing to do is spin with interrupts enabled. */
        __asm__ volatile("sti; hlt");
        return;
    }

    this_current->state       = THREAD_BLOCKED;
    this_current->wait_reason = reason;
    this_current->wake_at     = 0;          /* nothing but a wake will do */
    schedule();

    /* Woken -- but perhaps only to be told to go.  A thread coming out of a
     * wait holds nothing, which is what makes this the right place to leave
     * from. */
    if (proc_should_exit())
        proc_exit(-1);
}

/* Block on `reason`, but no longer than `until_tick`.
 *
 * A poll needs both halves: it waits for whatever it is watching to become
 * ready, and it has to come back anyway when the caller's timeout runs out --
 * or when something whose readiness nobody announces, like a key arriving at
 * the console, may have happened. */
void sched_block_until(wait_reason_t reason, uint32_t until_tick)
{
    if (!active) {
        while ((int32_t)(pit_ticks() - until_tick) < 0)
            __asm__ volatile("sti; hlt");
        return;
    }

    this_current->state       = THREAD_BLOCKED;
    this_current->wait_reason = reason;
    this_current->wake_at     = until_tick ? until_tick : 1;
    schedule();

    if (proc_should_exit())
        proc_exit(-1);
}

void sched_wake(wait_reason_t reason)
{
    if (!run_queue)
        return;

    thread_t *t = run_queue;
    do {
        if (t->state == THREAD_BLOCKED && t->wait_reason == reason) {
            t->state       = THREAD_READY;
            t->wait_reason = WAIT_NONE;
        }
        t = t->next;
    } while (t != run_queue);
}

/* Put the current thread to sleep until `until_tick`.
 *
 * A program with nothing to do until some moment has to be able to say so.
 * Spinning on sched_yield() would do the waiting, but the processor would be
 * fully occupied doing it -- and with the idle thread no longer taking a
 * timeslice of its own, nothing else would ever stop the machine running flat
 * out to wait. */
void sched_sleep_until(uint32_t until_tick)
{
    if (!active) {
        while ((int32_t)(pit_ticks() - until_tick) < 0)
            __asm__ volatile("sti; hlt");
        return;
    }

    sched_block_until(WAIT_TIME, until_tick);
}

/* Wake anything whose deadline has passed.  The comparison is on the signed
 * difference so that it keeps working across the tick counter's wrap. */
static void wake_expired(uint32_t now)
{
    thread_t *t = run_queue;
    if (!t)
        return;

    do {
        if (t->state == THREAD_BLOCKED && t->wake_at != 0 &&
            (int32_t)(now - t->wake_at) >= 0) {
            t->state       = THREAD_READY;
            t->wait_reason = WAIT_NONE;
        }
        t = t->next;
    } while (t != run_queue);
}

/* Called from the timer IRQ. Overrides the weak stub in pit.c. */
void sched_tick(regs_t *regs)
{
    if (!active)
        return;

    /* Charge the tick to the thread and to the process behind it.  The idle
     * thread belongs to the kernel's own process, which is not in the process
     * table, so idle time is not billed to anything a monitor can list. */
    thread_t *running = this_current;

    if (running) {
        running->cpu_ticks++;
        if (running->proc)
            running->proc->cpu_ticks++;
    }

    wake_expired(pit_ticks());

    /* A process that was asked to stop and is busy computing rather than
     * waiting leaves here.  Only from ring 3: interrupted in the kernel it
     * could be holding anything, and there is nothing to unwind it with. */
    if ((regs->cs & 3) && proc_should_exit())
        proc_exit(-1);

    /* One timeslice is one tick (10 ms). Short, but this system spends most
     * of its time waiting for a key anyway. */
    schedule();
}
