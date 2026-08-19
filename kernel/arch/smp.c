/* Starting the other processors, and the lock that lets them in.
 *
 * See smp.h for why there is one lock rather than a hundred.
 */

#include "smp.h"
#include "lapic.h"
#include "cpu.h"
#include "gdt.h"
#include "isr.h"
#include "pit.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "proc.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "kprintf.h"

/* Where the trampoline is copied to.  It has to be in the first megabyte and
 * page aligned, because a startup interrupt carries a page number and the
 * processor it wakes is in real mode when it gets there. */
#define TRAMPOLINE_PA   0x8000
#define TRAMPOLINE_PAGE (TRAMPOLINE_PA >> 12)

#define AP_STACK_SIZE 16384

/* How long to wait for a processor to say it is up.  A processor that never
 * answers is left out of the count rather than allowed to stop the boot: the
 * machine still works, with fewer cores than it has. */
#define AP_START_TIMEOUT_MS 200

extern uint8_t trampoline_start[], trampoline_end[];
extern uint64_t trampoline_cr3, trampoline_stack, trampoline_entry;

static smpcpu_t cpus[MAX_CPUS];
static int      cpu_count = 1;

/* Set by the processor being started, watched by the one starting it. */
static volatile int ap_reported;

static spinlock_t klock = SPINLOCK_INIT("kernel");

/* ------------------------------------------------------------------ *
 *  Which processor is this
 * ------------------------------------------------------------------ */

int smp_cpu_index(void)
{
    if (cpu_count <= 1 || !lapic_present())
        return 0;

    uint32_t id = lapic_id();

    for (int i = 0; i < cpu_count; i++)
        if (cpus[i].apic_id == id)
            return i;

    return 0;
}

smpcpu_t *smp_this(void)   { return &cpus[smp_cpu_index()]; }
smpcpu_t *smp_cpu(int i)   { return (i >= 0 && i < MAX_CPUS) ? &cpus[i] : NULL; }
int       smp_cpu_count(void) { return cpu_count; }
bool      smp_is_bsp(void) { return smp_this()->bsp; }

/* ------------------------------------------------------------------ *
 *  The kernel lock
 * ------------------------------------------------------------------ */

bool klock_held_here(void)
{
    return __atomic_load_n(&klock.owner, __ATOMIC_RELAXED) == smp_cpu_index();
}

/* Take the kernel lock, and say something if that takes absurdly long.
 *
 * A processor waiting here forever is what a deadlock looks like from the
 * inside, and the machine that froze this way gave no sign at all -- no fault,
 * no message, every core alive and none of them moving.  There was nothing to
 * read afterwards and nothing to see at the time.
 *
 * Five seconds is far longer than any legitimate hold: the longest thing the
 * kernel does with this lock held is a firmware load, and that gives the lock
 * up as it goes.  So a wait this long means something is stuck, and saying so
 * once -- naming the processor that is stuck and the one holding it -- turns
 * an unexplained hang into a starting point.
 *
 * It does not fix anything and does not pretend to.  It is the difference
 * between a machine that stops and a machine that stops and tells you where. */
void klock_acquire(void)
{
    int me = smp_cpu_index();

    if (!spin_trylock(&klock)) {
        uint64_t start = time_now_ms();
        bool     warned = false;

        while (!spin_trylock(&klock)) {
            if (!warned && time_now_ms() - start > 5000) {
                warned = true;
                kprintf("\nsmp    : cpu%d has waited 5 s for the kernel lock, "
                        "held by cpu%d -- something is stuck\n",
                        me, __atomic_load_n(&klock.owner, __ATOMIC_RELAXED));
            }
            cpu_relax();
        }
    }

    __atomic_store_n(&klock.owner, me, __ATOMIC_RELAXED);
}

void klock_release(void)
{
    __atomic_store_n(&klock.owner, -1, __ATOMIC_RELAXED);
    spin_unlock(&klock);
}

/* Take it unless this processor is already inside the kernel.
 *
 * The nested case is a page fault during a syscall, or the timer arriving
 * while a driver is in the middle of something: the same processor, already
 * holding it, and waiting for itself would be the end of the machine.  Only
 * this processor ever writes its own index into `owner`, so reading it without
 * the lock cannot go wrong: it either says us, which only we could have
 * written, or it says somebody else, which is the same as saying not us. */
bool klock_enter(void)
{
    if (cpu_count <= 1)
        return false;               /* nothing to be excluded from */

    if (klock_held_here())
        return false;

    klock_acquire();
    return true;
}

void klock_leave(bool took)
{
    if (took)
        klock_release();
}

/* Let go of the kernel lock for a moment, in the middle of a long wait.
 *
 * A driver waiting on hardware can be waiting for seconds -- a wireless
 * handshake, a firmware load, an address from a DHCP server.  Holding the one
 * kernel lock for that long stops every other processor from entering the
 * kernel at all: they cannot take a fault, service an interrupt or finish a
 * syscall, so the machine is as good as stopped even though nothing is
 * broken.
 *
 * This gives the lock up and takes it straight back, which is enough: the
 * processors queued behind it get their turn in between.  It is only safe
 * where the caller can survive kernel state changing underneath it, which is
 * why it is not called from anywhere except the polling loops of drivers that
 * hold no other state while they wait.
 *
 * Returns false if this processor did not hold the lock, in which case
 * nothing happened and nothing needed to. */
bool klock_pause(void)
{
    if (cpu_count <= 1)
        return false;
    if (!klock_held_here())
        return false;

    klock_release();

    /* A hint to the processor that this is a spin, and a window wide enough
     * for another core to actually take the lock rather than lose the race
     * back to us. */
    for (int i = 0; i < 64; i++)
        __asm__ volatile("pause");

    klock_acquire();
    return true;
}

/* ------------------------------------------------------------------ *
 *  Waking one up
 * ------------------------------------------------------------------ */

/* The first thing a started processor runs, arrived at from the trampoline
 * with a stack and nothing else. */
void smp_ap_entry(void);

void smp_ap_entry(void)
{
    /* The kernel's own descriptor tables, in this processor's copy of them:
     * the GDT because the TSS in it holds the stack this core will take
     * interrupts on, and the IDT because it is shared and only needs loading.
     */
    int index = -1;
    uint32_t id;

    lapic_init();
    id = lapic_id();

    for (int i = 0; i < MAX_CPUS; i++)
        if (cpus[i].apic_id == id && cpus[i].index == i) {
            index = i;
            break;
        }

    if (index < 0) {
        /* Started but unlisted: nothing can safely be done with it. */
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* The trampoline loaded the kernel's tables into CR3, so that is what
     * this processor is running on until the scheduler says otherwise. */
    cpus[index].space = paging_kernel_space();

    gdt_init_cpu(index);
    idt_load();

    cpus[index].online = true;
    __atomic_store_n(&ap_reported, 1, __ATOMIC_RELEASE);

    /* Everything from here touches the kernel's own structures, so it happens
     * behind the lock like any other kernel code. */
    klock_acquire();

    thread_t *idle = proc_make_idle_thread(cpus[index].stack,
                                           AP_STACK_SIZE);

    if (!idle) {
        klock_release();
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    cpus[index].idle    = idle;
    cpus[index].current = idle;
    sched_adopt_idle(idle);

    klock_release();

    /* Its own timer, because the 8259's is wired to the boot processor alone
     * and a core with no interrupt of its own would run the first thread it
     * picked up forever. */
    lapic_start_timer(PIT_HZ);

    kprintf("smp    : cpu%d (apic %u) running\n", index, id);

    /* The idle loop.  Look for work with the lock held, run it if there is
     * any, and sleep without the lock if there is not -- a processor that
     * slept holding the kernel would stop the machine rather than rest it. */
    for (;;) {
        klock_acquire();
        schedule();
        klock_release();

        __asm__ volatile("sti; hlt");
    }
}

/* The timer on a processor other than the boot one.
 *
 * The 8259's tick is the machine's clock and stays the boot processor's job --
 * counting it twice would make every timeout on the system run fast.  This one
 * exists to preempt, and to charge the time to the core that spent it. */
static void ap_timer(regs_t *regs)
{
    cpu_tick();
    sched_tick(regs);
}

static void start_one(smpcpu_t *cpu)
{
    void *stack = kmalloc(AP_STACK_SIZE);

    if (!stack) {
        kprintf("smp    : no stack for cpu%d\n", cpu->index);
        return;
    }

    cpu->stack = (uint64_t)(uintptr_t)stack;

    uint8_t *tramp = (uint8_t *)(uintptr_t)TRAMPOLINE_PA;
    uint64_t base  = (uint64_t)(uintptr_t)trampoline_start;

    /* Patch the copy, not the original: the processor reads these from where
     * it is running, which is the first megabyte. */
    *(uint64_t *)(tramp + ((uint64_t)&trampoline_cr3 - base)) =
        (uint64_t)(uintptr_t)paging_kernel_space()->pml4;
    *(uint64_t *)(tramp + ((uint64_t)&trampoline_stack - base)) =
        (uint64_t)(uintptr_t)stack + AP_STACK_SIZE;
    *(uint64_t *)(tramp + ((uint64_t)&trampoline_entry - base)) =
        (uint64_t)(uintptr_t)smp_ap_entry;

    __atomic_store_n(&ap_reported, 0, __ATOMIC_RELEASE);

    /* INIT, then two startup interrupts.  The second is what the manual asks
     * for and what real hardware sometimes needs; a processor already running
     * ignores it. */
    lapic_send_init(cpu->apic_id);
    lapic_delay_ms(10);

    lapic_send_startup(cpu->apic_id, TRAMPOLINE_PAGE);
    lapic_delay_ms(1);
    lapic_send_startup(cpu->apic_id, TRAMPOLINE_PAGE);

    for (int waited = 0; waited < AP_START_TIMEOUT_MS; waited++) {
        if (__atomic_load_n(&ap_reported, __ATOMIC_ACQUIRE))
            return;
        lapic_delay_ms(1);
    }

    kprintf("smp    : cpu%d (apic %u) did not start\n", cpu->index,
            cpu->apic_id);
    kfree(stack);
}

void smp_init(void)
{
    if (!lapic_init()) {
        kprintf("smp    : no local APIC; running on one processor\n");
        return;
    }

    lapic_calibrate();
    register_interrupt_handler(LAPIC_VECTOR_TIMER, ap_timer);

    /* This processor first, so that index 0 is always the one that booted. */
    cpus[0].apic_id = lapic_id();
    cpus[0].index   = 0;
    cpus[0].online  = true;
    cpus[0].bsp     = true;
    cpus[0].current = sched_current_thread();
    cpus[0].idle    = sched_current_thread();
    cpu_set_online(cpus[0].apic_id);

    wcpu_t list[MAX_CPUS];
    int    found = cpu_list(list, MAX_CPUS);

    /* The page the others wake up on has to be theirs alone. */
    pmm_reserve_range(TRAMPOLINE_PA, TRAMPOLINE_PA + 4096);
    memcpy((void *)(uintptr_t)TRAMPOLINE_PA, trampoline_start,
           (size_t)(trampoline_end - trampoline_start));

    for (int i = 0; i < found && cpu_count < MAX_CPUS; i++) {
        if (list[i].apic_id == cpus[0].apic_id)
            continue;

        smpcpu_t *cpu = &cpus[cpu_count];

        cpu->apic_id = list[i].apic_id;
        cpu->index   = cpu_count;
        cpu->online  = false;
        cpu->bsp     = false;

        /* Counted before it is started: the processor being woken looks
         * itself up in this table, and it runs before the call returns. */
        cpu_count++;

        start_one(cpu);

        if (cpu->online)
            cpu_set_online(cpu->apic_id);
        else
            cpu_count--;
    }

    int online = 0;
    for (int i = 0; i < cpu_count; i++)
        if (cpus[i].online)
            online++;

    kprintf("smp    : %d of %d processors running, one kernel lock between "
            "them\n", online, found);
}
