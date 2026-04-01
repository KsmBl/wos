/* Machine power-off.
 *
 * Doing this properly means parsing the ACPI tables to find the PM1a control
 * block and the S5 sleep type the firmware wants.  WOS has no ACPI parser, so
 * it writes the values the common emulators are known to use instead.  Each is
 * harmless where it is not recognised: the address decodes to nothing and the
 * write is dropped.
 *
 * On real hardware none of these will match and the machine simply halts,
 * which is why the fallback prints the message an operator needs.
 */

#include "power.h"
#include "io.h"
#include "kprintf.h"

/* ACPI PM1a_CNT: SLP_EN is bit 13, and every one of these platforms uses
 * sleep type 0 for S5 ("soft off"), except VirtualBox which wants 0x3400. */
#define SLP_EN_S5 0x2000

/* Ask one platform to power off, then wait to see whether it did.
 *
 * The write retires long before anything happens: an emulator notices the
 * request and tears the machine down from its own event loop, so the CPU
 * keeps executing for a short while afterwards.  Without this pause the next
 * attempt and the failure message run during that window, which made a
 * successful shutdown print "no ACPI soft-off available" on its way out.
 *
 * Interrupts are already disabled here, so this has to be a spin rather than
 * a hlt -- there is nothing left to wake us up. */
static void try_power_off(uint16_t port, uint16_t value)
{
    outw(port, value);

    for (volatile uint32_t i = 0; i < 20000000u; i++)
        ;
}

void power_off(void)
{
    kputs("\n[kernel] shutting down\n");

    /* Nothing to flush: WFS writes the superblock, the block bitmap and every
     * inode straight through on each change, so the volume on disk is already
     * consistent at all times. */

    __asm__ volatile("cli");

    /* QEMU 2.0 and later, both i440fx and q35: the PIIX4/ICH9 ACPI PM base is
     * at 0x600, so PM1a_CNT is at offset 4. */
    try_power_off(0x604, SLP_EN_S5);

    /* Older QEMU, and Bochs. */
    try_power_off(0xB004, SLP_EN_S5);

    /* VirtualBox. */
    try_power_off(0x4004, 0x3400);

    kputs("[kernel] no ACPI soft-off available on this machine\n");
    kputs("[kernel] it is now safe to turn off the power\n");

    for (;;)
        __asm__ volatile("hlt");
}
