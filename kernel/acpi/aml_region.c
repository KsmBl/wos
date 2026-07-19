/* Operation regions: where a field read stops being interpretation and becomes
 * an inb(), a memory access, or a conversation with the embedded controller.
 *
 * A Field() declares names as runs of bits at offsets into a region.  The bits
 * are rarely aligned to anything: a battery's state might be four bits at bit
 * 37 of an embedded-controller region.  So every read gathers whole units of
 * the region's access width, shifts, and masks; every write does the same and
 * puts back the bits it is not changing, because a read-modify-write of a
 * hardware register is the only way to set some of it.
 *
 * The embedded controller is the interesting one, and the reason an AML
 * interpreter gets real numbers out of a laptop at all.  Its *command
 * interface* is architectural -- port 0x62 for data, 0x66 for command and
 * status, and the read command is 0x80 on every machine ever built.  What is
 * machine-specific is which offset inside the controller holds the charge, and
 * that comes out of the DSDT.  Firmware describes the layout, the standard
 * describes the transport, and between them there is nothing left to guess.
 */

#include "aml_internal.h"
#include "io.h"
#include "pci.h"
#include "paging.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

/* ------------------------------------------------------------------ *
 *  The embedded controller
 * ------------------------------------------------------------------ */

/* The ACPI-defined registers.  Both are byte ports, and both are at these
 * addresses on any machine with a controller unless the ECDT says otherwise --
 * which is what the ECDT is read for below. */
static uint16_t ec_data = 0x62;
static uint16_t ec_cmd  = 0x66;
static bool     ec_found;

#define EC_STATUS_OBF 0x01     /* the controller has a byte for us   */
#define EC_STATUS_IBF 0x02     /* the controller has not taken ours  */

#define EC_READ  0x80
#define EC_WRITE 0x81

bool aml_ec_present(void) { return ec_found; }

/* The controller is slow in the way a separate microcontroller is slow -- tens
 * of microseconds for a byte, occasionally far worse while it is doing
 * something else.  Waiting is therefore bounded in wall-clock terms rather
 * than in loop counts, and a controller that never answers fails the read
 * rather than hanging the machine. */
static bool ec_wait(uint8_t mask, bool set)
{
    for (uint32_t spin = 0; spin < 500000; spin++) {
        uint8_t status = inb(ec_cmd);

        if (((status & mask) != 0) == set)
            return true;

        __asm__ volatile("pause");
    }

    return false;
}

bool aml_ec_read(uint8_t offset, uint8_t *out)
{
    if (!ec_found)
        return false;

    if (!ec_wait(EC_STATUS_IBF, false))
        return false;
    outb(ec_cmd, EC_READ);

    if (!ec_wait(EC_STATUS_IBF, false))
        return false;
    outb(ec_data, offset);

    if (!ec_wait(EC_STATUS_OBF, true))
        return false;

    *out = inb(ec_data);
    return true;
}

bool aml_ec_write(uint8_t offset, uint8_t value)
{
    if (!ec_found)
        return false;

    if (!ec_wait(EC_STATUS_IBF, false))
        return false;
    outb(ec_cmd, EC_WRITE);

    if (!ec_wait(EC_STATUS_IBF, false))
        return false;
    outb(ec_data, offset);

    if (!ec_wait(EC_STATUS_IBF, false))
        return false;
    outb(ec_data, value);

    return true;
}

/* Two ways to find the controller, in the order of how much they are worth
 * trusting.  The ECDT is a table that exists to say where it is; failing that,
 * a PNP0C09 device in the namespace is one, and its ports are in its _CRS --
 * which needs resource-template parsing, so this settles for the architectural
 * defaults and only notes that a controller was declared. */
static void ec_from_ecdt(void)
{
    uint32_t len = 0;
    uint8_t *ecdt = acpi_table_bytes("ECDT", &len);

    if (!ecdt || len < 65)
        return;

    /* Two generic address structures, at offsets 36 and 48.  Each is a space
     * id, three sizing bytes, then a 64-bit address. */
    uint8_t  ctrl_space = ecdt[36];
    uint64_t ctrl_addr  = 0;
    uint8_t  data_space = ecdt[48];
    uint64_t data_addr  = 0;

    for (int i = 0; i < 8; i++) {
        ctrl_addr |= (uint64_t)ecdt[40 + i] << (i * 8);
        data_addr |= (uint64_t)ecdt[52 + i] << (i * 8);
    }

    /* Only an I/O-space controller is understood; a memory-mapped one exists
     * in the specification and on nothing this runs on. */
    if (ctrl_space == 1 && data_space == 1 && ctrl_addr && data_addr) {
        ec_cmd  = (uint16_t)ctrl_addr;
        ec_data = (uint16_t)data_addr;
        ec_found = true;
        kprintf("ec     : at 0x%x/0x%x, from the ECDT\n", ec_data, ec_cmd);
    }
}

static void ec_from_namespace(aml_node_t *device, void *user)
{
    (void)device;
    bool *found = user;
    *found = true;
}

void aml_ec_init(void)
{
    ec_from_ecdt();

    if (ec_found)
        return;

    bool declared = false;
    aml_each_device("PNP0C09", ec_from_namespace, &declared);

    if (!declared)
        return;

    /* A declared controller with no ECDT.  The ports are almost certainly the
     * architectural ones; a read that comes back is the proof, so one is
     * attempted before believing it. */
    ec_found = true;

    uint8_t probe;
    if (!aml_ec_read(0, &probe)) {
        ec_found = false;
        kprintf("ec     : declared in the namespace but not answering\n");
        return;
    }

    kprintf("ec     : at 0x%x/0x%x, the architectural ports\n", ec_data, ec_cmd);
}

/* ------------------------------------------------------------------ *
 *  Reaching an address in each space
 * ------------------------------------------------------------------ */

/* One access-width unit out of a region, at a byte offset within it. */
static bool region_read(aml_node_t *region, uint64_t offset, uint8_t width,
                        uint64_t *out)
{
    uint64_t addr = region->offset + offset;

    switch (region->space) {
    case AML_REGION_MEMORY: {
        /* boot.S identity maps the first gigabyte, which is where firmware
         * puts these.  Anything above it is refused rather than read through
         * a mapping that does not exist. */
        if (addr + width > LOW_MEMORY_LIMIT)
            return false;

        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
        uint64_t v = 0;

        for (uint8_t i = 0; i < width; i++)
            v |= (uint64_t)p[i] << (i * 8);

        *out = v;
        return true;
    }

    case AML_REGION_IO: {
        uint16_t port = (uint16_t)addr;

        switch (width) {
        case 1: *out = inb(port); return true;
        case 2: *out = inw(port); return true;
        case 4: *out = inl(port); return true;
        default: return false;
        }
    }

    case AML_REGION_PCI: {
        /* The region's own offset is the register; which device it is on comes
         * from the parent, and reaching that means _ADR on the enclosing
         * device.  Nothing the battery needs lives in configuration space, so
         * this is refused rather than read off the wrong function. */
        return false;
    }

    case AML_REGION_EC: {
        uint64_t v = 0;

        for (uint8_t i = 0; i < width; i++) {
            uint8_t byte;

            if (!aml_ec_read((uint8_t)(addr + i), &byte))
                return false;

            v |= (uint64_t)byte << (i * 8);
        }

        *out = v;
        return true;
    }

    default:
        return false;
    }
}

static bool region_write(aml_node_t *region, uint64_t offset, uint8_t width,
                         uint64_t value)
{
    uint64_t addr = region->offset + offset;

    switch (region->space) {
    case AML_REGION_MEMORY: {
        if (addr + width > LOW_MEMORY_LIMIT)
            return false;

        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;

        for (uint8_t i = 0; i < width; i++)
            p[i] = (uint8_t)(value >> (i * 8));
        return true;
    }

    case AML_REGION_IO: {
        uint16_t port = (uint16_t)addr;

        switch (width) {
        case 1: outb(port, (uint8_t)value);  return true;
        case 2: outw(port, (uint16_t)value); return true;
        case 4: outl(port, (uint32_t)value); return true;
        default: return false;
        }
    }

    case AML_REGION_EC:
        for (uint8_t i = 0; i < width; i++)
            if (!aml_ec_write((uint8_t)(addr + i), (uint8_t)(value >> (i * 8))))
                return false;
        return true;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ *
 *  Fields
 * ------------------------------------------------------------------ */

aml_object_t *aml_field_read(aml_object_t *field)
{
    if (!field || field->type != AML_FIELD || !field->field.region)
        return NULL;

    aml_node_t *region = field->field.region;
    uint8_t     width  = field->field.access ? field->field.access : 1;
    uint32_t    bits   = field->field.bit_len;

    if (bits == 0 || bits > 64)
        return NULL;

    /* Where the field starts, rounded down to a unit the region can be read
     * in, and how far into that unit it begins. */
    uint32_t unit_bits  = (uint32_t)width * 8;
    uint32_t first_unit = field->field.bit_offset / unit_bits;
    uint32_t shift      = field->field.bit_offset % unit_bits;
    uint32_t units      = (shift + bits + unit_bits - 1) / unit_bits;

    uint64_t value = 0;

    for (uint32_t i = 0; i < units; i++) {
        uint64_t chunk = 0;

        if (!region_read(region, (uint64_t)(first_unit + i) * width, width,
                         &chunk))
            return NULL;

        /* Units past the first contribute the bits above what has already
         * been gathered.  A field wider than 64 bits was refused above, so
         * this cannot shift off the end of the value. */
        if (i * unit_bits < 64)
            value |= chunk << (i * unit_bits);
    }

    value >>= shift;

    if (bits < 64)
        value &= (1ULL << bits) - 1;

    return aml_integer(value);
}

bool aml_field_write(aml_object_t *field, uint64_t value)
{
    if (!field || field->type != AML_FIELD || !field->field.region)
        return false;

    aml_node_t *region = field->field.region;
    uint8_t     width  = field->field.access ? field->field.access : 1;
    uint32_t    bits   = field->field.bit_len;

    if (bits == 0 || bits > 64)
        return false;

    uint32_t unit_bits  = (uint32_t)width * 8;
    uint32_t first_unit = field->field.bit_offset / unit_bits;
    uint32_t shift      = field->field.bit_offset % unit_bits;
    uint32_t units      = (shift + bits + unit_bits - 1) / unit_bits;

    uint64_t mask = (bits < 64) ? ((1ULL << bits) - 1) : ~0ULL;

    for (uint32_t i = 0; i < units; i++) {
        uint64_t offset = (uint64_t)(first_unit + i) * width;
        uint64_t chunk  = 0;

        /* A field that does not fill its unit has to keep the neighbouring
         * bits, which means reading before writing.  A write-only register
         * would be corrupted by that, and firmware marks those WriteAsZeros;
         * this does not model the update rule, so it preserves, which is the
         * safe half of being wrong. */
        bool partial = (units > 1) || (shift != 0) || (bits != unit_bits);

        if (partial && !region_read(region, offset, width, &chunk))
            return false;

        uint32_t unit_shift = i * unit_bits;
        uint64_t unit_mask;
        uint64_t unit_value;

        if (i == 0) {
            unit_mask  = mask << shift;
            unit_value = (value & mask) << shift;
        } else {
            uint32_t done = unit_bits - shift + (i - 1) * unit_bits;

            unit_mask  = mask >> done;
            unit_value = (value & mask) >> done;
        }

        if (unit_bits < 64) {
            uint64_t keep = (1ULL << unit_bits) - 1;
            unit_mask  &= keep;
            unit_value &= keep;
        }

        chunk = (chunk & ~unit_mask) | (unit_value & unit_mask);

        if (!region_write(region, offset, width, chunk))
            return false;

        (void)unit_shift;
    }

    return true;
}
