/* Spinlocks, and the one rule that makes them necessary.
 *
 * Until there was a second processor, "interrupts off" was mutual exclusion:
 * nothing else could be running, so nothing else could see a half-finished
 * structure.  With another core executing at the same time that stops being
 * true -- clearing IF on this processor says nothing at all about what the
 * other one is doing -- and every shared thing in the kernel needs something
 * that both processors can agree on.
 *
 * A lock here is one word and a bus-locked exchange, which is the smallest
 * thing that works.  There is no fairness, no queueing and no sleeping: the
 * kernel holds these for the length of a syscall at most, and a processor that
 * spends that long waiting is a processor that had nothing else to do anyway.
 *
 * `pause` in the wait loop is not a delay.  It tells the processor that this
 * is a spin loop, which stops it speculating a long chain of loads that will
 * all have to be thrown away when the value finally changes -- and on a
 * hyper-threaded core it hands the pipeline to the other thread.
 */
#ifndef WOS_SPINLOCK_H
#define WOS_SPINLOCK_H

#include "types.h"

typedef struct {
    volatile uint32_t locked;
    int               owner;      /* cpu index holding it, -1 when free */
    const char       *name;       /* for the panic when one is held too long */
} spinlock_t;

#define SPINLOCK_INIT(n) { 0, -1, (n) }

static inline void spin_init(spinlock_t *l, const char *name)
{
    l->locked = 0;
    l->owner  = -1;
    l->name   = name;
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline bool spin_trylock(spinlock_t *l)
{
    return __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

static inline void spin_lock(spinlock_t *l)
{
    for (;;) {
        if (spin_trylock(l))
            return;

        /* Read plainly until it looks free, then try the locked exchange
         * again: a bus-locked write on every attempt would keep the cache
         * line moving between processors and slow down the one holding it. */
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            cpu_relax();
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/* Interrupts, saved and restored around a critical section.  Still needed
 * beside the lock rather than instead of it: a lock stops the other processor,
 * and clearing IF stops this one's own timer from arriving in the middle. */
static inline bool interrupts_enabled(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1u << 9)) != 0;
}

static inline bool interrupts_off(void)
{
    bool was = interrupts_enabled();

    __asm__ volatile("cli" ::: "memory");
    return was;
}

static inline void interrupts_restore(bool were_on)
{
    if (were_on)
        __asm__ volatile("sti" ::: "memory");
}

#endif /* WOS_SPINLOCK_H */
