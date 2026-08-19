/* What can be learned about a battery without running the firmware's code.
 *
 * A modern laptop reports its charge through an ACPI method -- _BST -- which
 * reads the embedded controller and returns a package.  Calling it means
 * interpreting AML, and WOS has no interpreter: the one piece it reads (the
 * sleep type for soft-off) is a constant sitting in a fixed shape, not a
 * program.  So the charge level is out of reach, and this says so rather than
 * inventing one.
 *
 * What is reachable is everything static.  The ACPI tables say whether a
 * battery and a mains adapter are declared at all; the SMBIOS tables describe
 * the pack itself -- who made it, what chemistry, how much it holds when new.
 * That is the difference between "this is a laptop with a 45 Wh lithium-ion
 * battery" and "this machine runs on mains", which is most of what the
 * question is usually asking.
 */
#ifndef WOS_BATTERY_H
#define WOS_BATTERY_H

#include "types.h"
#include "wosconfig.h"
#include "string.h"
#include "wabi.h"

/* Read the firmware's description of the machine's battery, if it has one.
 * Needs the heap, paging and acpi_init(), and is boot-time only for the same
 * reason acpi_init() is. */
#if CONFIG_BATTERY

void battery_init(void);

/* Fill in what was found.  Safe at any time; everything was read at boot. */
void battery_info(wbattery_t *out);

/* One line for the boot log. */
void battery_print_report(void);


#else
/* Without the driver: a machine that reports no battery, which is what a
 * desktop reports anyway. */
static inline void battery_init(void) { }
static inline void battery_info(wbattery_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->charge_percent = -1;   /* -1 is "cannot be read", which is true */
        out->ac_online = -1;
    }
}
static inline void battery_print_report(void) { }
#endif /* CONFIG_BATTERY */

#endif /* WOS_BATTERY_H */
