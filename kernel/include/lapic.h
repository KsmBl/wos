/* The local APIC: the interrupt controller each processor has of its own.
 *
 * The 8259 pair this kernel started with is a single controller wired to a
 * single processor, and it stays exactly that -- IRQ 0 through 15 keep
 * arriving on the processor that booted.  What the local APIC adds is the two
 * things a second processor cannot be had without: a way to send it an
 * interrupt (which is how it is started at all), and a timer of its own (which
 * is how anything running on it is ever preempted).
 */
#ifndef WOS_LAPIC_H
#define WOS_LAPIC_H

#include "types.h"

/* The vectors the local APIC raises.  Above the 8259's 32-47 and above the
 * syscall gate at 128, so nothing collides. */
#define LAPIC_VECTOR_TIMER    0xF0
#define LAPIC_VECTOR_SPURIOUS 0xFF

/* True once this processor's local APIC is enabled and answering. */
bool lapic_init(void);

/* Is there one at all?  A machine without it runs on one processor, which is
 * what this kernel did until now and still does. */
bool lapic_present(void);

uint32_t lapic_id(void);

/* End of interrupt, for anything the local APIC delivered.  The 8259's EOI is
 * a different register on a different chip and both are needed, each for its
 * own interrupts. */
void lapic_eoi(void);

/* Start another processor: the INIT/SIPI dance, with the startup vector given
 * as the physical page the trampoline was copied to. */
void lapic_send_init(uint32_t apic_id);
void lapic_send_startup(uint32_t apic_id, uint8_t vector_page);

/* Work out how fast the timer counts, against the PIT.  Done once on the boot
 * processor: the bus clock the local APIC counts is the same for all of them,
 * and the processor being started has no other clock to calibrate against. */
void lapic_calibrate(void);

/* A delay that does not need the tick.  Every processor is started before the
 * boot processor enables interrupts, so the PIT's count is not moving yet. */
void lapic_delay_ms(uint32_t ms);

/* A periodic interrupt on this processor at the given rate. */
void lapic_start_timer(uint32_t hz);

#endif /* WOS_LAPIC_H */
