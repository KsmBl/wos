/* The local APIC.
 *
 * Memory mapped at a fixed physical address the firmware puts in an MSR, and
 * already inside the kernel's identity map, so it is reached by pointer like
 * anything else.  Every access is a full 32-bit word at a 16-byte aligned
 * offset; the hardware faults on anything narrower.
 */

#include "lapic.h"
#include "cpu.h"
#include "pit.h"
#include "paging.h"
#include "kprintf.h"

#define IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1u << 11)

#define REG_ID        0x020
#define REG_VERSION   0x030
#define REG_TPR       0x080
#define REG_EOI       0x0B0
#define REG_SVR       0x0F0
#define REG_ICR_LOW   0x300
#define REG_ICR_HIGH  0x310
#define REG_LVT_TIMER 0x320
#define REG_LVT_LINT0 0x350
#define REG_LVT_LINT1 0x360
#define REG_TIMER_INIT 0x380
#define REG_TIMER_CUR  0x390
#define REG_TIMER_DIV  0x3E0

#define SVR_ENABLE    (1u << 8)

#define ICR_INIT      (5u << 8)
#define ICR_STARTUP   (6u << 8)
#define ICR_PHYSICAL  0
#define ICR_ASSERT    (1u << 14)
#define ICR_LEVEL     (1u << 15)
#define ICR_PENDING   (1u << 12)

#define LVT_PERIODIC  (1u << 17)
#define LVT_MASKED    (1u << 16)

/* Delivery modes, in bits 10:8 of a local vector table entry. */
#define LVT_NMI       (4u << 8)
#define LVT_EXTINT    (7u << 8)

#define TIMER_DIVIDE_16 0x3

static volatile uint8_t *lapic;         /* NULL when there is no local APIC */
static uint32_t          ticks_per_second;   /* of the timer's own counting */

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* A delay measured on the timestamp counter rather than on the tick.
 *
 * Everything here runs with interrupts still off -- the other processors are
 * started before the boot processor ever enables them -- so the PIT's tick
 * counter is standing still and sleeping on it would sleep forever.  The
 * timestamp counter runs regardless of what the interrupt flag says. */
void lapic_delay_ms(uint32_t ms)
{
    uint32_t khz = cpu_tsc_khz();

    if (!khz) {
        /* No calibrated counter: spin a generous fixed number of times, which
         * is only ever used on a machine slow enough not to care. */
        for (volatile uint64_t i = 0; i < (uint64_t)ms * 200000; i++)
            ;
        return;
    }

    uint64_t until = rdtsc() + (uint64_t)khz * ms;

    while (rdtsc() < until)
        __asm__ volatile("pause");
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value),
                       "d"((uint32_t)(value >> 32)));
}

static void put(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(lapic + reg) = value;
}

static uint32_t get(uint32_t reg)
{
    return *(volatile uint32_t *)(lapic + reg);
}

bool lapic_present(void)
{
    return lapic != NULL;
}

uint32_t lapic_id(void)
{
    /* Bits 31:24, which is where the eight-bit id lives in xAPIC mode. */
    return lapic ? (get(REG_ID) >> 24) : 0;
}

void lapic_eoi(void)
{
    if (lapic)
        put(REG_EOI, 0);
}

bool lapic_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 1, EDX bit 9: there is a local APIC on this processor. */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));

    if (!(edx & (1u << 9)))
        return false;

    uint64_t base = read_msr(IA32_APIC_BASE);

    /* The firmware has already chosen the address; taking it from the MSR
     * rather than assuming 0xFEE00000 costs nothing and is the documented way
     * to find it.  The enable bit is set back in case something cleared it. */
    write_msr(IA32_APIC_BASE, base | APIC_BASE_ENABLE);

    /* Bit 8 says this is the processor that booted, which the firmware set
     * long before any of this ran. */
    bool     bsp  = (base & (1ULL << 8)) != 0;
    uint64_t phys = base & ~0xFFFULL;

    /* It has to be mapped, and it is nowhere near the memory that already is:
     * the identity map covers the first gigabyte and this sits just under four,
     * where the chipset puts its registers rather than where the RAM is.
     *
     * Uncached, which is not an optimisation the other way round -- these are
     * registers with side effects, and a write to the end-of-interrupt one
     * that sat in a cache line would acknowledge nothing.
     *
     * Every processor shares the kernel's page tables, so mapping it once here
     * maps it for all of them; a core that runs this later finds it done. */
    if (!paging_translate(paging_kernel_space(), phys))
        paging_map(paging_kernel_space(), phys, phys,
                   PTE_WRITE | PTE_UNCACHED);

    lapic = (volatile uint8_t *)(uintptr_t)phys;

    /* Accept every priority: this kernel has no interrupt priorities and a
     * task priority left high by the firmware would silently drop them. */
    put(REG_TPR, 0);

    /* The spurious vector register is also the switch that turns the whole
     * thing on.  A spurious interrupt is one the controller could not take
     * back in time; the handler for it does nothing, and must not EOI. */
    put(REG_SVR, SVR_ENABLE | LAPIC_VECTOR_SPURIOUS);

    /* The two local pins, and the one thing about them that must not be got
     * wrong: **LINT0 is how the 8259 reaches the processor**.
     *
     * Before the local APIC is enabled the interrupt line runs straight into
     * the processor's INTR pin.  Enabling it puts the local APIC in the way,
     * and the legacy line then arrives on LINT0 -- so a masked LINT0 is a
     * machine with no timer, no keyboard and no disk, which halts with
     * interrupts enabled and never wakes up again.  It is delivered in ExtINT
     * mode, which means "the vector comes from the 8259 rather than from this
     * register", and that is the whole of virtual wire mode.
     *
     * Only on the processor that booted: the 8259 is wired to one processor
     * and delivering its interrupts to all of them would run every handler
     * four times.  LINT1 is the non-maskable pin, which the firmware wires to
     * whatever watchdog the board has. */
    if (bsp) {
        put(REG_LVT_LINT0, LVT_EXTINT);
        put(REG_LVT_LINT1, LVT_NMI);
    } else {
        put(REG_LVT_LINT0, LVT_MASKED);
        put(REG_LVT_LINT1, LVT_MASKED);
    }

    put(REG_LVT_TIMER, LVT_MASKED);

    return true;
}

/* Wait for the last inter-processor interrupt to be accepted.  Bounded: a
 * processor that is not there never accepts, and a kernel that waited forever
 * for it would be a kernel that does not boot on that machine. */
static void wait_for_delivery(void)
{
    for (int i = 0; i < 1000000; i++) {
        if (!(get(REG_ICR_LOW) & ICR_PENDING))
            return;
        __asm__ volatile("pause");
    }
}

static void send_ipi(uint32_t apic_id, uint32_t command)
{
    put(REG_ICR_HIGH, apic_id << 24);
    put(REG_ICR_LOW, command);
    wait_for_delivery();
}

void lapic_send_init(uint32_t apic_id)
{
    if (!lapic)
        return;

    send_ipi(apic_id, ICR_INIT | ICR_PHYSICAL | ICR_ASSERT | ICR_LEVEL);
    send_ipi(apic_id, ICR_INIT | ICR_PHYSICAL | ICR_LEVEL);   /* deassert */
}

void lapic_send_startup(uint32_t apic_id, uint8_t vector_page)
{
    if (!lapic)
        return;

    /* The vector *is* the address: the processor starts in real mode at
     * vector << 12, which is why the trampoline has to live in the first
     * megabyte on a page boundary. */
    send_ipi(apic_id, ICR_STARTUP | ICR_PHYSICAL | ICR_ASSERT | vector_page);
}

void lapic_calibrate(void)
{
    if (!lapic)
        return;

    put(REG_TIMER_DIV, TIMER_DIVIDE_16);
    put(REG_TIMER_INIT, 0xFFFFFFFF);

    /* Fifty milliseconds against the timestamp counter, whose speed the
     * processor was already asked for at startup.  Long enough that the
     * reading is not mostly the cost of taking it. */
    uint64_t t0 = rdtsc();
    uint32_t c0 = get(REG_TIMER_CUR);

    lapic_delay_ms(50);

    uint32_t c1 = get(REG_TIMER_CUR);
    uint64_t t1 = rdtsc();

    put(REG_TIMER_INIT, 0);                  /* stop it again */

    uint32_t khz = cpu_tsc_khz();
    uint64_t us  = khz ? ((t1 - t0) * 1000) / khz : 50000;

    if (!us)
        us = 1;

    /* It counts down, so the difference is how far it got. */
    ticks_per_second = (uint32_t)(((uint64_t)(c0 - c1) * 1000000) / us);

    kprintf("lapic  : timer at %u ticks/s, divided by 16\n",
            ticks_per_second);
}

void lapic_start_timer(uint32_t hz)
{
    if (!lapic || !ticks_per_second || !hz)
        return;

    put(REG_TIMER_DIV, TIMER_DIVIDE_16);
    put(REG_LVT_TIMER, LAPIC_VECTOR_TIMER | LVT_PERIODIC);
    put(REG_TIMER_INIT, ticks_per_second / hz);
}
