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

/* Turn on I/O space and bus mastering in the device's command register, which
 * a DMA-driven NIC needs before it can do anything. */
void pci_enable_bus_master(const pci_device_t *dev);

#endif /* WOS_PCI_H */
