/* More than one processor.
 *
 * The machine has always had them; this kernel ran on the one it booted on and
 * left the rest halted.  Starting them is the easy half -- an interrupt with a
 * page number in it, and a page of code that repeats what boot.S did.  The
 * hard half is that every shared thing in the kernel was written when nothing
 * else could be running.
 *
 * The answer here is one lock around the whole kernel.  A processor takes it
 * on the way in -- every syscall, every interrupt, every fault, all of which
 * funnel through interrupt_dispatch() -- and drops it on the way back out to
 * ring 3.  So kernel code still runs one processor at a time, exactly as it
 * did, and its existing assumptions all still hold; what runs in parallel is
 * **user code**, which is where the time actually goes.  A machine with four
 * cores running four programs now runs them at once.
 *
 * It is a coarse answer and it is deliberately the first one: the alternative
 * is a lock per structure -- the frame allocator, the heap, the filesystem,
 * the console, every driver -- and getting one of them wrong is not a crash
 * but a corruption that shows up somewhere else an hour later.  This way the
 * kernel is as correct as it was before, and the parallelism that is there is
 * real.
 *
 * The lock is dropped around a context switch rather than held across one.  A
 * processor that switched with it held would hand the kernel to a thread that
 * has not asked for it, and a processor with nothing to run would hold it
 * while it slept -- which is the whole machine stopped behind one idle core.
 */
#ifndef WOS_SMP_H
#define WOS_SMP_H

#include "types.h"

#define MAX_CPUS 32

struct thread;
struct addrspace;

typedef struct {
    uint32_t       apic_id;
    int            index;
    bool           online;
    bool           bsp;

    /* The scheduler's two per-processor facts.  They were single globals when
     * there was one processor, which is exactly what a second one breaks. */
    struct thread *current;
    struct thread *idle;

    /* The address space this processor has in CR3.  Per core for the same
     * reason `current` is: it is a fact about a processor, not about the
     * machine. */
    struct addrspace *space;

    /* The stack this processor was started on, which is also the stack its
     * idle thread runs on. */
    uint64_t       stack;

    uint64_t       busy_ticks;
    uint64_t       idle_ticks;
} smpcpu_t;

/* Start every other processor and put it to work.  Called last in the boot
 * sequence: everything it hands out -- page tables, the heap, the process
 * table -- has to exist before anything else can touch it. */
void smp_init(void);

/* Which processor is this?  An index into the table, not the APIC id: the
 * hardware's ids need not be dense and nothing here wants a sparse array. */
int  smp_cpu_index(void);

smpcpu_t *smp_this(void);
smpcpu_t *smp_cpu(int index);

/* How many are running, which is one until smp_init() has been past. */
int  smp_cpu_count(void);

/* Is this processor the one that booted?  The 8259 and the PIT are wired to
 * it alone, so the clock and the keyboard still arrive there. */
bool smp_is_bsp(void);

/* The kernel lock.
 *
 * klock_enter() takes it unless this processor already holds it, which is what
 * makes a fault inside a syscall work: the same processor is already inside
 * the kernel, and waiting for itself would be the end of the machine.  It
 * returns whether it took it, and that answer is what klock_leave() needs. */
bool klock_enter(void);
void klock_leave(bool took);

/* The unconditional pair, for the scheduler's handoff and the idle loop. */
void klock_acquire(void);
void klock_release(void);
bool klock_held_here(void);

#endif /* WOS_SMP_H */
