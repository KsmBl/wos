/* 8254 PIT driver. */

#include "pit.h"
#include "smp.h"
#include "cpu.h"
#include "isr.h"
#include "pic.h"
#include "io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

/* The PIT's input clock, in Hz. */
#define PIT_BASE_FREQUENCY 1193182u

static volatile uint32_t ticks;
static uint32_t          tick_hz = PIT_HZ;

/* Weak hook: once the scheduler exists it defines this symbol and gets a
 * chance to preempt on every tick.  Until then the empty version runs. */
__attribute__((weak)) void sched_tick(regs_t *regs)
{
    (void)regs;
}

static void pit_irq(regs_t *regs)
{
    ticks++;

    /* Before the scheduler gets a chance to switch away: the tick that just
     * elapsed belongs to whatever was running during it, not to whatever runs
     * next. */
    cpu_tick();

    sched_tick(regs);
}

uint32_t pit_ticks(void)
{
    return ticks;
}

uint32_t pit_uptime_ms(void)
{
    return ticks * (1000u / tick_hz);
}

/* See pit.h.  The cycle counter is free-running and per-processor; the rate
 * comes from cpu_init, which measured it against this timer long before
 * anything needed a deadline.
 *
 * A machine whose counter rate could not be measured falls back to counting
 * interrupts, which is what this used to do everywhere.  That machine can
 * still deadlock the way described in pit.h -- but it is a machine whose
 * processor would not say how fast it runs, and there is nothing better to
 * offer it. */
uint64_t time_now_ms(void)
{
    static uint32_t khz;

    if (!khz) {
        khz = cpu_tsc_khz();
        if (!khz)
            return pit_uptime_ms();
    }

    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));

    return (((uint64_t)hi << 32) | lo) / khz;
}

/* Wait, without stopping the rest of the machine while doing it.
 *
 * Two things here are deliberate.  The clock is the cycle counter rather than
 * the tick count, because a driver calling this usually holds the kernel lock
 * and the tick is advanced by a handler that needs the same lock -- waiting on
 * it would be waiting for something this processor is itself preventing.
 *
 * The kernel lock is *not* given up while sleeping, deliberately.  Most
 * callers are drivers part-way through a hardware sequence, and letting a
 * second processor into the same driver would be worse than the delay.  Where
 * a subsystem is serialised in its own right -- the network stack, behind
 * net_claim -- its polling loop gives the lock up itself. */
void pit_sleep(uint32_t ms)
{
    uint64_t target = time_now_ms() + ms;

    while (time_now_ms() < target)
        __asm__ volatile("pause");
}

void pit_init(uint32_t frequency)
{
    if (frequency == 0)
        frequency = PIT_HZ;
    tick_hz = frequency;

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;           /* slowest the hardware can go: ~18.2 Hz */

    /* 0x36 = channel 0, access lo/hi byte, mode 3 (square wave), binary. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    register_interrupt_handler(IRQ_TIMER, pit_irq);
    pic_clear_mask(0);
}
