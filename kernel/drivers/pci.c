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

/* Fill in one device's details from its bus address. */
static pci_device_t describe(uint8_t bus, uint8_t slot, uint8_t func)
{
    pci_device_t dev = { 0 };
    uint32_t id = pci_read32(bus, slot, func, 0x00);

    dev.found  = true;
    dev.bus    = bus;
    dev.slot   = slot;
    dev.func   = func;
    dev.vendor = (uint16_t)(id & 0xFFFF);
    dev.device = (uint16_t)(id >> 16);
    dev.bar0   = pci_read32(bus, slot, func, 0x10);
    dev.irq    = (uint8_t)(pci_read32(bus, slot, func, 0x3C) & 0xFF);

    return dev;
}

/* Find a device by what it is rather than who made it, which is the only way
 * to find a host controller: every vendor's is a different device id, and they
 * all implement the same register interface because the class code says so. */
pci_device_t pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    pci_device_t none = { 0 };

    for (int bus = 0; bus < 256; bus++)
        for (int slot = 0; slot < 32; slot++)
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot,
                                         (uint8_t)func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;

                uint32_t klass = pci_read32((uint8_t)bus, (uint8_t)slot,
                                            (uint8_t)func, 0x08);

                if ((uint8_t)(klass >> 24) == class_code &&
                    (uint8_t)(klass >> 16) == subclass &&
                    (uint8_t)(klass >> 8)  == prog_if)
                    return describe((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
            }

    return none;
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

uint64_t pci_bar_address(const pci_device_t *dev, int index)
{
    uint8_t  off = (uint8_t)(0x10 + index * 4);
    uint32_t low = pci_read32(dev->bus, dev->slot, dev->func, off);

    if (low & 1)                       /* an I/O BAR, not memory */
        return 0;

    uint64_t addr = low & ~0xFULL;

    /* Type 2 in bits [2:1] means the address continues in the next register. */
    if (((low >> 1) & 3) == 2)
        addr |= (uint64_t)pci_read32(dev->bus, dev->slot, dev->func,
                                     (uint8_t)(off + 4)) << 32;

    return addr;
}
