/* The battery, as far as the firmware's tables describe it.  See battery.h. */

#include "battery.h"
#include "acpi.h"
#include "aml.h"

/* ------------------------------------------------------------------ *
 *  The charge, which only the firmware's own code knows
 * ------------------------------------------------------------------ */

/* The battery device in the namespace, found once at boot.  There may be more
 * than one on a laptop with two packs; the first is the one reported, because
 * wbattery_t describes a battery rather than a set of them. */
static aml_node_t *battery_device;
static aml_node_t *adapter_device;

static void remember_battery(aml_node_t *device, void *user)
{
    (void)user;
    if (!battery_device)
        battery_device = device;
}

static void remember_adapter(aml_node_t *device, void *user)
{
    (void)user;
    if (!adapter_device)
        adapter_device = device;
}
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* ------------------------------------------------------------------ *
 *  SMBIOS
 *
 *  A table of variable-length structures the firmware leaves in memory,
 *  describing the machine as built: the board, the memory slots, the chassis,
 *  and -- structure type 22 -- the battery pack.  It is where a laptop says
 *  what its battery is, which is the one part of the answer that does not
 *  require running the firmware's code to get at.
 * ------------------------------------------------------------------ */

/* The 32-bit entry point, which every machine with SMBIOS has: even one that
 * also provides the 64-bit one keeps this for compatibility. */
struct smbios_entry {
    char     anchor[4];             /* "_SM_"                          */
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major, minor;
    uint16_t max_structure_size;
    uint8_t  revision;
    uint8_t  formatted[5];
    char     dmi_anchor[5];         /* "_DMI_"                         */
    uint8_t  dmi_checksum;
    uint16_t table_length;
    uint32_t table_address;
    uint16_t structure_count;
    uint8_t  bcd_revision;
} __attribute__((packed));

/* The 64-bit entry point, from SMBIOS 3.0.  A machine that has only this one
 * puts its tables anywhere in the address space, which is why the address is
 * a full 64 bits. */
struct smbios3_entry {
    char     anchor[5];             /* "_SM3_"                         */
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major, minor;
    uint8_t  docrev;
    uint8_t  revision;
    uint8_t  reserved;
    uint32_t table_max_length;
    uint64_t table_address;
} __attribute__((packed));

#define SMBIOS_PORTABLE_BATTERY 22

/* Type 22, up to the fields this reads.  Everything past `sbds_chemistry` is
 * OEM-specific and of no interest. */
struct smbios_battery {
    uint8_t  type;
    uint8_t  length;
    uint16_t handle;
    uint8_t  location;              /* string number                   */
    uint8_t  manufacturer;          /* string number                   */
    uint8_t  manufacture_date;      /* string number                   */
    uint8_t  serial_number;         /* string number                   */
    uint8_t  device_name;           /* string number                   */
    uint8_t  device_chemistry;      /* an enumeration; see below       */
    uint16_t design_capacity;       /* milliwatt-hours, times the
                                     * multiplier at the end           */
    uint16_t design_voltage;        /* millivolts                      */
    uint8_t  sbds_version;          /* string number                   */
    uint8_t  maximum_error;
    uint16_t sbds_serial_number;
    uint16_t sbds_manufacture_date;
    uint8_t  sbds_chemistry;        /* string number                   */
    uint8_t  capacity_multiplier;
} __attribute__((packed));

/* SMBIOS device chemistry values. */
#define SMBIOS_CHEM_LEAD    3
#define SMBIOS_CHEM_NICD    4
#define SMBIOS_CHEM_NIMH    5
#define SMBIOS_CHEM_LION    6
#define SMBIOS_CHEM_ZINCAIR 7
#define SMBIOS_CHEM_LIPOLY  8

/* ------------------------------------------------------------------ *
 *  What we found
 * ------------------------------------------------------------------ */

static bool     present;
static bool     ac_declared;
static uint32_t design_mwh;
static uint32_t design_mv;
static uint32_t chemistry;
static char     pack_name[32];
static char     pack_maker[32];
static char     pack_location[32];

/* ------------------------------------------------------------------ *
 *  Reading the tables
 * ------------------------------------------------------------------ */

static bool checksum_ok(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint8_t sum = 0;

    while (len--)
        sum = (uint8_t)(sum + *p++);

    return sum == 0;
}

/* The strings of one structure follow its fixed part, NUL-separated, and the
 * set ends with an empty one.  They are numbered from 1; 0 means "no string",
 * which is how a firmware says it has nothing to put there. */
static void copy_string(const uint8_t *structure, uint32_t limit, uint8_t index,
                        char *out, size_t cap)
{
    out[0] = '\0';
    if (index == 0)
        return;

    const char *p   = (const char *)structure + structure[1];
    const char *end = (const char *)structure + limit;

    for (uint8_t n = 1; p < end && *p; n++) {
        size_t len = strlen(p);

        if (n == index) {
            strlcpy(out, p, cap);
            return;
        }
        p += len + 1;
    }
}

/* Step over one structure: its fixed part, then its strings, then the empty
 * string that ends them.  Returns the offset of the next one, or 0 when the
 * table is malformed or finished. */
static uint32_t next_structure(const uint8_t *table, uint32_t length,
                               uint32_t at)
{
    if (at + 4 > length || table[at + 1] < 4)
        return 0;

    uint32_t p = at + table[at + 1];

    /* Two NULs in a row close the string set.  A structure with no strings at
     * all still has them, as a single empty one. */
    while (p + 1 < length && !(table[p] == 0 && table[p + 1] == 0))
        p++;

    return (p + 2 <= length) ? p + 2 : 0;
}

static uint32_t chemistry_of(uint8_t smbios_value)
{
    switch (smbios_value) {
    case SMBIOS_CHEM_LEAD:    return W_BATTERY_CHEM_LEAD;
    case SMBIOS_CHEM_NICD:    return W_BATTERY_CHEM_NICD;
    case SMBIOS_CHEM_NIMH:    return W_BATTERY_CHEM_NIMH;
    case SMBIOS_CHEM_LION:    return W_BATTERY_CHEM_LION;
    case SMBIOS_CHEM_ZINCAIR: return W_BATTERY_CHEM_ZINCAIR;
    case SMBIOS_CHEM_LIPOLY:  return W_BATTERY_CHEM_LIPOLY;
    default:                  return W_BATTERY_CHEM_UNKNOWN;
    }
}

/* Take what type 22 says about the pack. */
static void read_battery_structure(const uint8_t *table, uint32_t length,
                                   uint32_t at)
{
    struct smbios_battery b;
    uint8_t  size = table[at + 1];
    uint32_t span = length - at;

    memset(&b, 0, sizeof(b));
    memcpy(&b, table + at, size < sizeof(b) ? size : sizeof(b));

    present = true;

    copy_string(table + at, span, b.location, pack_location,
                sizeof(pack_location));
    copy_string(table + at, span, b.manufacturer, pack_maker,
                sizeof(pack_maker));
    copy_string(table + at, span, b.device_name, pack_name, sizeof(pack_name));

    /* Everything from design_capacity on arrived with SMBIOS 2.1, and a
     * shorter structure simply stops before it. */
    if (size >= __builtin_offsetof(struct smbios_battery, sbds_version)) {
        uint32_t multiplier = 1;

        /* The multiplier is the last field of the structure and later than the
         * rest; without it the capacity is already in milliwatt-hours. */
        if (size > __builtin_offsetof(struct smbios_battery,
                                      capacity_multiplier) &&
            b.capacity_multiplier > 1)
            multiplier = b.capacity_multiplier;

        design_mwh = (uint32_t)b.design_capacity * multiplier;
        design_mv  = b.design_voltage;
        chemistry  = chemistry_of(b.device_chemistry);
    }
}

static void read_smbios_table(uint64_t phys, uint32_t length)
{
    uint8_t *table = acpi_copy_physical(phys, length);
    if (!table)
        return;

    uint32_t at = 0;

    while (at + 4 <= length) {
        if (table[at] == SMBIOS_PORTABLE_BATTERY) {
            read_battery_structure(table, length, at);
            break;                       /* the first pack is the one shown */
        }

        uint32_t next = next_structure(table, length, at);
        if (next <= at)
            break;                       /* malformed, or the end of it */
        at = next;
    }

    kfree(table);
}

/* Look for the entry point where the firmware leaves it: the read-only region
 * at the top of the first megabyte, on a sixteen-byte boundary.  Both anchors
 * live there, and a machine that has the 64-bit one usually has the 32-bit one
 * beside it. */
static void find_smbios(void)
{
    for (uint64_t a = 0xF0000; a < 0x100000; a += 16) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;

        if (memcmp(p, "_SM3_", 5) == 0) {
            const struct smbios3_entry *e = (const struct smbios3_entry *)p;

            if (e->length >= sizeof(*e) && checksum_ok(e, e->length)) {
                read_smbios_table(e->table_address, e->table_max_length);
                return;
            }
        }

        if (memcmp(p, "_SM_", 4) == 0) {
            const struct smbios_entry *e = (const struct smbios_entry *)p;

            if (e->length >= sizeof(*e) && checksum_ok(e, e->length)) {
                read_smbios_table(e->table_address, e->table_length);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  What the rest of the kernel asks for
 * ------------------------------------------------------------------ */

void battery_init(void)
{
    find_smbios();

    /* The ACPI tables are the other half of the answer, and the more reliable
     * one: SMBIOS describes a machine as built, and a laptop sold with the
     * battery removed can still carry the structure. */
    if (acpi_has_battery())
        present = true;

    ac_declared = acpi_has_ac_adapter();

    /* Which device in the namespace the numbers come from.  A machine whose
     * firmware declares a battery the byte scan missed is still found here,
     * and counts as present: the namespace is the better authority, being the
     * thing the charge will actually be read through. */
    aml_each_device("PNP0C0A", remember_battery, NULL);
    aml_each_device("ACPI0003", remember_adapter, NULL);

    if (battery_device)
        present = true;
}

/* _BST is four numbers: state, present rate, remaining capacity, voltage.
 * _BIF is thirteen, of which the third is what the pack holds when full *now*
 * -- not when it was new.  A percentage against the design capacity would read
 * over 100 on a new pack and flatter an old one, so the last-full figure is
 * the one to divide by. */
static bool read_charge(int32_t *percent, uint32_t *state)
{
    if (!battery_device || !aml_ready())
        return false;

    aml_node_t *bst_node = aml_lookup(battery_device, "_BST");
    if (!bst_node)
        return false;

    aml_object_t *bst = aml_evaluate_node(bst_node, NULL, 0);
    uint64_t      raw_state = 0, remaining = 0;

    if (!bst || !aml_package_integer(bst, 0, &raw_state) ||
                !aml_package_integer(bst, 2, &remaining)) {
        aml_unref(bst);
        return false;
    }
    aml_unref(bst);

    /* The state is a bitmask, and both bits set at once is a battery the
     * firmware is confused about rather than one doing both. */
    if (raw_state & 0x01)
        *state = W_BATTERY_DISCHARGING;
    else if (raw_state & 0x02)
        *state = W_BATTERY_CHARGING;
    else
        *state = W_BATTERY_FULL;

    /* 0xFFFFFFFF is how the specification spells "unknown", and it appears
     * while a pack is settling after being plugged in. */
    if (remaining == 0xFFFFFFFFULL)
        return false;

    uint64_t full = 0;

    aml_node_t *bif = aml_lookup(battery_device, "_BIF");
    if (!bif)
        bif = aml_lookup(battery_device, "_BIX");

    if (bif) {
        aml_object_t *info = aml_evaluate_node(bif, NULL, 0);

        /* _BIX puts a revision field first, so everything in it is one place
         * further along than in _BIF. */
        uint32_t at = (bif->name[3] == 'X') ? 3 : 2;

        if (info)
            aml_package_integer(info, at, &full);
        aml_unref(info);
    }

    if (!full || full == 0xFFFFFFFFULL)
        return false;

    if (remaining > full)
        remaining = full;

    *percent = (int32_t)((remaining * 100) / full);
    return true;
}

/* _PSR on the mains adapter: 1 when it is supplying power. */
static int32_t read_ac_online(void)
{
    if (!adapter_device || !aml_ready())
        return -1;

    aml_node_t *psr = aml_lookup(adapter_device, "_PSR");
    if (!psr)
        return -1;

    aml_object_t *v = aml_evaluate_node(psr, NULL, 0);
    uint64_t      n = 0;

    if (!v || !aml_to_integer(v, &n)) {
        aml_unref(v);
        return -1;
    }

    aml_unref(v);
    return n ? 1 : 0;
}

void battery_info(wbattery_t *out)
{
    memset(out, 0, sizeof(*out));

    out->present   = present ? 1 : 0;
    out->state     = W_BATTERY_UNKNOWN;

    /* Read every time rather than cached: a charge that does not change is
     * worse than none, because it looks like a measurement. */
    out->charge_percent = -1;
    out->ac_online      = read_ac_online();

    if (present) {
        int32_t  percent = -1;
        uint32_t state   = W_BATTERY_UNKNOWN;

        if (read_charge(&percent, &state)) {
            out->charge_percent = percent;
            out->state          = state;
        }
    }

    out->design_mwh = design_mwh;
    out->design_mv  = design_mv;
    out->chemistry  = chemistry;

    strlcpy(out->name, pack_name, sizeof(out->name));
    strlcpy(out->maker, pack_maker, sizeof(out->maker));
    strlcpy(out->location, pack_location, sizeof(out->location));
}

void battery_print_report(void)
{
    if (!present) {
        kputs("battery: none; this machine runs on mains\n");
        return;
    }

    kprintf("battery: %s%s%s",
            pack_maker[0] ? pack_maker : "",
            (pack_maker[0] && pack_name[0]) ? " " : "",
            pack_name[0] ? pack_name : (pack_maker[0] ? "" : "present"));

    if (design_mwh)
        kprintf(", %u.%u Wh when new", design_mwh / 1000,
                (design_mwh % 1000) / 100);
    if (design_mv)
        kprintf(", %u.%u V", design_mv / 1000, (design_mv % 1000) / 100);

    kputs("\n");

    wbattery_t now;
    battery_info(&now);

    if (now.charge_percent >= 0)
        kprintf("battery: %d%% and %s", now.charge_percent,
                now.state == W_BATTERY_CHARGING    ? "charging"
              : now.state == W_BATTERY_DISCHARGING ? "discharging"
                                                   : "full");
    else if (battery_device)
        kputs("battery: the charge is declared but did not read");
    else
        kputs("battery: no charge to read -- the firmware declares no "
              "battery device");

    if (now.ac_online >= 0)
        kprintf(", %s mains\n", now.ac_online ? "on" : "off");
    else
        kprintf("; %s\n", ac_declared ? "a mains adapter is declared too"
                                       : "no mains adapter is declared");
}
