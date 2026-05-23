/* Just enough ACPI to turn the machine off.
 *
 * Powering down is not something a PC can be asked to do directly: the request
 * goes to the chipset's power management block, whose address is in the ACPI
 * tables, using a sleep type that is not in the tables at all but in a bytecode
 * object inside them.  So finding out how to switch the machine off means
 * walking the table tree and reading a little AML.
 *
 * Nothing else here is interpreted.  This is not an ACPI implementation and
 * does not pretend to be one.
 */
#ifndef WOS_ACPI_H
#define WOS_ACPI_H

#include "types.h"
#include "multiboot.h"

/* Find the tables and read out what soft-off needs.  Safe to call on a machine
 * with no ACPI at all; acpi_can_power_off() then stays false. */
void acpi_init(const struct multiboot_info *mbi);

/* True once the power management block and the S5 sleep type are both known. */
bool acpi_can_power_off(void);

/* What was found, for the boot log: the PM1a control port and the S5 sleep
 * type.  Both zero when acpi_can_power_off() is false. */
void acpi_power_info(uint16_t *port, uint16_t *sleep_type);

/* Ask the chipset for soft-off.  Returns only if the machine ignored it. */
void acpi_power_off(void);

#endif /* WOS_ACPI_H */
