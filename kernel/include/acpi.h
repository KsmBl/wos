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

/* The reset register the firmware described, when it described one this kernel
 * can write: an address in system I/O space.  Restarts the machine. */
bool acpi_can_reset(void);
void acpi_reset(void);

/* Copy the table with this four-character signature out of physical memory,
 * or NULL if the machine has not got one.  The caller owns the copy and frees
 * it with kfree(); `length_out` receives its length in bytes.
 *
 * Reaching a table means mapping a window over physical memory that shadows
 * the identity map while it is open, so this is for boot time only, before
 * anything else is running to notice.  Subsystems that need ACPI data at
 * runtime read it once during their own init and keep what they found.
 *
 * Only valid after acpi_init(); before that there is no root table to search. */
void *acpi_table(const char *signature, uint32_t *length_out);

/* The `index`th table with this signature, counting from 0, or NULL once
 * there are no more.  A machine has any number of SSDTs and the AML loader
 * has to read all of them, so it walks with this until it comes back empty. */
void *acpi_table_nth(const char *signature, int index, uint32_t *length_out);

/* The DSDT, which is the one table the root table does not list -- the FADT
 * points at it, and acpi_init() is where that gets read.  Same ownership as
 * acpi_table(): the caller frees the copy, or keeps it forever. */
void *acpi_dsdt(uint32_t *length_out);

/* Copy `len` bytes of physical memory onto the heap, through the same window
 * and with the same boot-time-only restriction.  The firmware leaves more than
 * ACPI tables above the identity map -- the SMBIOS structures, for one -- and
 * this is how they are reached.  NULL if the range cannot be mapped or the heap
 * is full; the caller frees what it gets. */
void *acpi_copy_physical(uint64_t phys, uint64_t len);

/* What the machine's own description says about a battery.
 *
 * Both are read out of the DSDT during acpi_init(), by looking for the objects
 * a battery and a mains adapter are declared with.  Neither says anything
 * about the charge: that comes from a method which reads the embedded
 * controller, and running one means interpreting bytecode.  Whether the
 * machine has a battery at all is still worth knowing, and is the difference
 * between a laptop and a desktop. */
bool acpi_has_battery(void);
bool acpi_has_ac_adapter(void);

#endif /* WOS_ACPI_H */
