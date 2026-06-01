/* 8254 PIT driver. */

#include "pit.h"
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

void pit_sleep(uint32_t ms)
{
    uint32_t target = pit_uptime_ms() + ms;
    while (pit_uptime_ms() < target)
        __asm__ volatile("hlt");
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
