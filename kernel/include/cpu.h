/* What the machine's processors are and what they are doing.
 *
 * Three separate questions, answered from three different places:
 *
 *   how many there are    the ACPI processor list, or CPUID if there is none
 *   how fast one is going measured from the performance counters each tick
 *   how hot it is         the digital thermal sensor, read on demand
 *
 * Every processor runs WOS code now, so every one of them has something to
 * report: the busy and idle ticks are charged to the core the timer fired on,
 * and each core answers for its own clock.  One that could not be started is
 * still listed, present and offline -- it is part of the machine either way.
 */
#ifndef WOS_CPU_H
#define WOS_CPU_H

#include "types.h"
#include "wabi.h"

/* Identify the processors, work out how to measure a clock on this machine and
 * take the first reading.  Needs the heap, paging and acpi_init(). */
void cpu_init(void);

/* Sample the performance counters and account the tick to whichever core it
 * happened on.  Called from the timer interrupt; a no-op before cpu_init(). */
void cpu_tick(void);

/* Say that WOS is now executing on the processor with this APIC id, so that
 * anything listing the machine's cores stops calling it offline. */
void cpu_set_online(uint32_t apic_id);

/* Fill in what is true of the machine as a whole. */
void cpu_info(wcpuinfo_t *out);

/* Copy up to `max` processor records out.  Returns how many were written,
 * which is never more than the machine has.  Readings that need the core
 * itself -- the clock, the temperature -- are taken here for the one core this
 * is running on, and left unknown for the rest. */
int cpu_list(wcpu_t *out, int max);

/* Ask this processor to run at `khz`, clamped to the range it says it will
 * work in and rounded to a step it can actually take.  Returns the clock it
 * settled on in kHz, or -W_ENODEV on a machine with no register to write --
 * which is the usual answer inside a hypervisor.
 *
 * The request applies to the processor that executes it, because that is what
 * a model-specific register is.  With only the boot core running, that is the
 * whole machine.
 *
 * Permission is the caller's business, not this function's: it is checked at
 * the syscall, where there is a process to check it against. */
int cpu_set_khz(uint32_t khz);

/* Give the clock back to the hardware's own judgement.  Returns 0, or
 * -W_ENODEV as above. */
int cpu_set_automatic(void);

/* The rate the timestamp counter advances at, in kHz.  Fixed, and unrelated to
 * how fast the core is currently going -- which is exactly what makes it
 * useful for timing short intervals.  Zero before cpu_init(). */
uint32_t cpu_tsc_khz(void);

/* A few lines for the boot log: what the processor is, what it can do, and
 * which of those things this machine will actually let the kernel see. */
void cpu_print_report(void);

#endif /* WOS_CPU_H */
