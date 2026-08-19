/* Just enough PCI to find one device and read its resources.
 *
 * Configuration space is reached through the classic 0xCF8/0xCFC port pair: an
 * address is written to 0xCF8, then the 32-bit word at that offset is read from
 * or written to 0xCFC.  No PCIe ECAM, no bridges beyond bus 0 -- the emulated
 * machine puts everything we care about on the first bus.
 */
#ifndef WOS_PCI_H
#define WOS_PCI_H

#include "types.h"

typedef struct {
    bool     found;
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint32_t bar0;        /* first base address register, raw */
    uint8_t  irq;         /* interrupt line                   */
} pci_device_t;

/* Read/write a 32-bit configuration word. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                     uint32_t value);

/* Scan every bus/slot/function for a device with this vendor and device id. */
pci_device_t pci_find(uint16_t vendor, uint16_t device);

/* The same, for a driver that supports several device ids -- the first device
 * whose vendor matches and whose id is any of `devices`.
 *
 * This exists because a scan is not cheap.  Every slot on every bus is probed
 * with a pair of port accesses, and on an emulated machine each one of those
 * leaves the virtual processor: sixty-five thousand slots is most of a second.
 * A driver that called pci_find once per id it supports would pay that over
 * and over, which is exactly how the wireless driver put eight seconds into
 * the boot before this was noticed. */
pci_device_t pci_find_any(uint16_t vendor, const uint16_t *devices, int count);

/* The same, by class/subclass/interface -- how a host controller is found,
 * since every vendor's has its own device id but the class code is what says
 * the registers are where the specification puts them. */
/* Match any programming interface.  Some classes spell real differences in
 * that byte -- an IDE controller puts the mode of each channel there, and
 * whether it can bus-master -- so a driver that cares about the class but not
 * the variant would otherwise have to guess every value the byte can take. */
#define PCI_PROG_IF_ANY 0xFF

pci_device_t pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

/* The same, picking between several devices of one kind.  Returns a device
 * with `found` clear once `index` is past the last one. */
pci_device_t pci_find_class_index(uint8_t class_code, uint8_t subclass,
                                  uint8_t prog_if, int index);

/* Full 64-bit address of a memory BAR, with the flag bits removed.  A 64-bit
 * BAR is two consecutive registers; `index` is the first of them. */
uint64_t pci_bar_address(const pci_device_t *dev, int index);

/* The I/O port a BAR names, or 0 if that BAR is a memory one.
 *
 * The counterpart to pci_bar_address, which answers 0 for an I/O BAR -- the
 * two kinds are told apart by bit 0 and a caller always knows which it wants.
 * An IDE controller keeps its bus-master registers in an I/O BAR, which is
 * what this exists for. */
uint16_t pci_bar_io(const pci_device_t *dev, int index);

/* Turn on I/O space, memory space and bus mastering in the device's command
 * register.  A DMA-driven device needs the last of those; one whose registers
 * are memory mapped needs the second, and reads back all-ones without it. */
void pci_enable_bus_master(const pci_device_t *dev);

/* Bring a device out of a low power state, where its registers read as
 * all-ones and it ignores everything written to them. */
void pci_power_on(const pci_device_t *dev);

#endif /* WOS_PCI_H */
