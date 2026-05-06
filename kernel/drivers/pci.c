/* Minimal PCI configuration access. See pci.h. */

#include "pci.h"
#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t off)
{
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    outl(PCI_ADDR, config_address(bus, slot, func, off));
    return inl(PCI_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                 uint32_t value)
{
    outl(PCI_ADDR, config_address(bus, slot, func, off));
    outl(PCI_DATA, value);
}

pci_device_t pci_find(uint16_t vendor, uint16_t device)
{
    pci_device_t dev = { 0 };

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot,
                                         (uint8_t)func, 0x00);
                uint16_t v = (uint16_t)(id & 0xFFFF);
                uint16_t d = (uint16_t)(id >> 16);

                if (v == 0xFFFF)
                    continue;           /* nothing in this function */

                if (v == vendor && d == device) {
                    dev.found  = true;
                    dev.bus    = (uint8_t)bus;
                    dev.slot   = (uint8_t)slot;
                    dev.func   = (uint8_t)func;
                    dev.vendor = v;
                    dev.device = d;
                    dev.bar0   = pci_read32((uint8_t)bus, (uint8_t)slot,
                                            (uint8_t)func, 0x10);
                    dev.irq    = (uint8_t)(pci_read32((uint8_t)bus, (uint8_t)slot,
                                            (uint8_t)func, 0x3C) & 0xFF);
                    return dev;
                }

                /* If this is not a multi-function device, function 0 is all
                 * there is. */
                if (func == 0) {
                    uint32_t header = pci_read32((uint8_t)bus, (uint8_t)slot,
                                                 0, 0x0C);
                    if (!((header >> 16) & 0x80))
                        break;
                }
            }
        }
    }
    return dev;
}

void pci_enable_bus_master(const pci_device_t *dev)
{
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0001;      /* I/O space enable   */
    cmd |= 0x0004;      /* bus master enable  */
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
