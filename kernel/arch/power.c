/* Machine power-off.
 *
 * The real way is ACPI: the tables say where the chipset's power management
 * block is and which sleep type means "off", and acpi.c reads both at boot.
 * That is the only thing that works on real hardware.
 *
 * The fixed addresses below are kept for the emulators, several of which
 * recognise a write to a known port without any of the tables being consulted,
 * and two of which WOS is developed on.  Each is harmless where it is not
 * recognised: the address decodes to nothing and the write is dropped.
 */

#include "power.h"
#include "acpi.h"
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
static void settle(void)
{
    for (volatile uint32_t i = 0; i < 20000000u; i++)
        ;
}

static void try_power_off(uint16_t port, uint16_t value)
{
    outw(port, value);
    settle();
}

void power_off(void)
{
    kputs("\n[kernel] shutting down\n");

    /* Nothing to flush: WFS writes the superblock, the block bitmap and every
     * inode straight through on each change, so the volume on disk is already
     * consistent at all times. */

    __asm__ volatile("cli");

    /* What the machine itself says to do, when it said anything. */
    if (acpi_can_power_off()) {
        acpi_power_off();

        for (volatile uint32_t i = 0; i < 20000000u; i++)
            ;
    }

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

/* Restarting the machine.
 *
 * There is no single way to do it, so this is four in the order they are worth
 * trying.  Each is harmless where it is not recognised: an unclaimed port
 * decodes to nothing and the write is dropped.
 *
 * The last one is not a trick, it is the architecture: a fault while handling
 * a fault while handling a fault is defined to reset the processor, and a
 * table with no entries in it faults on the first interrupt.  Every machine
 * does it, which is what makes it the honest last resort.
 */
void power_reboot(void)
{
    kputs("\n[kernel] restarting\n");

    __asm__ volatile("cli");

    /* What the firmware said to do. */
    if (acpi_can_reset()) {
        acpi_reset();
        settle();
    }

    /* The chipset's reset control register: set the system-reset bit, then
     * pulse it.  This is what PCs have done since the ICH, and what QEMU's
     * q35 answers to. */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    settle();

    /* The keyboard controller's pulse-reset line, which is how the PC did it
     * before there was a chipset register for it, and which the emulators
     * still implement.  Wait for the input buffer to be free first, or the
     * command is dropped rather than obeyed. */
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        ;
    outb(0x64, 0xFE);
    settle();

    /* And the architecture's own answer.  An IDT with no entries, then an
     * interrupt: the fault cannot be delivered, nor can the fault about the
     * fault, and a triple fault resets the processor. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed))
        nothing = { 0, 0 };

    __asm__ volatile("lidt %0; int3" : : "m"(nothing));

    for (;;)
        __asm__ volatile("hlt");
}
