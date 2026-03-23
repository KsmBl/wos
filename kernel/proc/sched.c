/* Round-robin scheduler.
 *
 * One circular run queue holds every thread that is not unused.  Blocked and
 * zombie threads stay on the list but are skipped, which keeps insertion and
 * removal cheap and means waking a thread is a single state change.
 */

#include "sched.h"
#include "gdt.h"
#include "isr.h"
#include "kprintf.h"

extern void switch_context(uint32_t *save_esp, uint32_t new_esp);

static thread_t *run_queue;       /* circular, may be NULL before init */
static thread_t *current;
static thread_t *idle_thread;
static bool      active;

bool sched_active(void) { return active; }

thread_t *sched_current_thread(void) { return current; }

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
    idle_thread = idle;
    current     = idle;
    idle->state = THREAD_RUNNING;
    sched_add(idle);
    active = true;
}

/* Find the next thread that can run, starting after `from`.
 * Falls back to the idle thread when everything else is blocked. */
static thread_t *pick_next(thread_t *from)
{
    if (!run_queue)
        return idle_thread;

    thread_t *t = from ? from->next : run_queue;
    if (!t)
        t = run_queue;

    for (int i = 0; i < MAX_THREADS + 1; i++) {
        if (t->state == THREAD_READY)
            return t;
        /* The current thread may keep running if nothing else is ready. */
        if (t == from && t->state == THREAD_RUNNING)
            return t;
        t = t->next;
        if (!t)
            t = run_queue;
    }

    return idle_thread;
}

void schedule(void)
{
    if (!active)
        return;

    thread_t *prev = current;
    thread_t *next = pick_next(prev);

    if (next == prev) {
        /* Nothing else to run. If the current thread can no longer run, the
         * idle thread must take over or we would return into a blocked
         * thread and never come back. */
        if (prev->state == THREAD_RUNNING)
            return;
        next = idle_thread;
        if (next == prev)
            return;
    }

    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    current = next;

    /* The TSS tells the CPU which stack to switch to on the next ring 3 to
     * ring 0 transition, so it has to follow the thread. */
    tss_set_kernel_stack(next->kernel_stack + next->kernel_stack_size);

    if (next->proc && next->proc->space)
        paging_switch(next->proc->space);

    switch_context(&prev->esp, next->esp);
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

    current->state       = THREAD_BLOCKED;
    current->wait_reason = reason;
    schedule();
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

/* Called from the timer IRQ. Overrides the weak stub in pit.c. */
void sched_tick(regs_t *regs)
{
    (void)regs;

    if (!active)
        return;

    if (current)
        current->cpu_ticks++;

    /* One timeslice is one tick (10 ms). Short, but this system spends most
     * of its time waiting for a key anyway. */
    schedule();
}
