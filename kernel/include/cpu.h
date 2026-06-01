/* What the machine's processors are and what they are doing.
 *
 * Three separate questions, answered from three different places:
 *
 *   how many there are    the ACPI processor list, or CPUID if there is none
 *   how fast one is going measured from the performance counters each tick
 *   how hot it is         the digital thermal sensor, read on demand
 *
 * Only the boot processor is ever running WOS code, so it is the only one with
 * anything to report.  The others are listed because they are part of the
 * machine and a program showing the CPU should say so rather than claim the
 * box has one core; when WOS learns to start them, they fill in here and
 * nothing above this file has to change.
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

/* Fill in what is true of the machine as a whole. */
void cpu_info(wcpuinfo_t *out);

/* Copy up to `max` processor records out.  Returns how many were written,
 * which is never more than the machine has.  Readings that need the core
 * itself -- the clock, the temperature -- are taken here for the one core this
 * is running on, and left unknown for the rest. */
int cpu_list(wcpu_t *out, int max);

/* The rate the timestamp counter advances at, in kHz.  Fixed, and unrelated to
 * how fast the core is currently going -- which is exactly what makes it
 * useful for timing short intervals.  Zero before cpu_init(). */
uint32_t cpu_tsc_khz(void);

/* A few lines for the boot log: what the processor is, what it can do, and
 * which of those things this machine will actually let the kernel see. */
void cpu_print_report(void);

#endif /* WOS_CPU_H */
