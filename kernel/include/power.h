/* Powering the machine off. */
#ifndef WOS_POWER_H
#define WOS_POWER_H

#include "types.h"

/* Shut the machine down.
 *
 * Tries the ACPI soft-off sequence that the common emulators respond to.  If
 * none of them does, this parks the CPU with interrupts disabled rather than
 * returning, so it is always safe to treat as the end of execution.
 */
void power_off(void) __attribute__((noreturn));

#endif /* WOS_POWER_H */
