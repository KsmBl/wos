/* ACPI: enough of it to power the machine off.  See acpi.h. */

#include "acpi.h"
#include "paging.h"
#include "kheap.h"
#include "fbcon.h"
#include "string.h"
#include "io.h"

/* ------------------------------------------------------------------ *
 *  Table formats
 * ------------------------------------------------------------------ */

struct rsdp {
    char     signature[8];          /* "RSD PTR " */
    uint8_t  checksum;              /* over the first 20 bytes */
    char     oem_id[6];
    uint8_t  revision;              /* 0 for 1.0, 2 for 2.0 and later */
    uint32_t rsdt_address;
    /* 2.0 and later only */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;     /* over the whole structure */
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Generic address structure: where a register lives and how wide it is. */
struct acpi_gas {
    uint8_t  space_id;              /* 0 memory, 1 I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

#define GAS_SYSTEM_IO 1

struct acpi_fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;
    struct acpi_gas x_pm1b_cnt_blk;
} __attribute__((packed));

/* The structure above is the current layout; a table written to an older
 * revision simply stops early, and the fields past its end are not there to be
 * read.  QEMU's own FADT is 116 bytes, an ACPI 1.0 table with none of the
 * extended addresses -- so every field beyond the legacy block is used only
 * after asking whether the table is long enough to hold it. */
#define FADT_HAS(fadt, field)                                     \
    ((fadt)->header.length >=                                     \
     __builtin_offsetof(struct acpi_fadt, field) + sizeof((fadt)->field))

/* Anything shorter than this cannot say where the power management block is,
 * which is the whole reason for reading it. */
#define FADT_MINIMUM __builtin_offsetof(struct acpi_fadt, pm2_cnt_blk)

/* PM1 control register bits. */
#define SLP_TYP_SHIFT 10
#define SLP_EN        (1 << 13)
#define SCI_EN        (1 << 0)

/* ------------------------------------------------------------------ *
 *  What we learned
 * ------------------------------------------------------------------ */

static uint16_t pm1a_cnt, pm1b_cnt;
static uint16_t slp_typ_a, slp_typ_b;
static uint16_t smi_cmd;
static uint8_t  acpi_enable_value;
static bool     have_s5;

bool acpi_can_power_off(void) { return have_s5 && pm1a_cnt != 0; }

void acpi_power_info(uint16_t *port, uint16_t *sleep_type)
{
    *port       = acpi_can_power_off() ? pm1a_cnt : 0;
    *sleep_type = acpi_can_power_off() ? slp_typ_a : 0;
}

/* ------------------------------------------------------------------ *
 *  Reaching the tables
 * ------------------------------------------------------------------ */

/* A window above the framebuffer aperture for looking at physical memory the
 * identity map does not reach -- ACPI tables sit just below 4 GiB on most
 * machines, and the kernel maps only the first gigabyte.
 *
 * While it is in place the window shadows the identity mapping of the RAM at
 * the same addresses, so nothing else may run in it: every table is copied out
 * to the heap (which is far below) and the window put back immediately.  It
 * needs no allocation of its own, because the page directory covering it was
 * built at boot. */
#define WINDOW      (FBCON_APERTURE + FBCON_APERTURE_SIZE)
#define WINDOW_SIZE (8UL * 1024 * 1024)

/* Copy `len` bytes of physical memory into freshly allocated heap memory. */
static void *copy_physical(uint64_t phys, uint64_t len)
{
    if (!len || len > WINDOW_SIZE)
        return NULL;

    void *out = kmalloc(len);
    if (!out)
        return NULL;

    /* Addresses the kernel can already reach need no window at all. */
    if (phys + len <= LOW_MEMORY_LIMIT) {
        memcpy(out, (const void *)(uintptr_t)phys, len);
        return out;
    }

    uint64_t base   = phys & ~0x1FFFFFUL;      /* 2 MiB aligned */
    uint64_t offset = phys - base;
    uint64_t span   = offset + len;

    if (span > WINDOW_SIZE) {
        kfree(out);
        return NULL;
    }

    for (uint64_t off = 0; off < span; off += 0x200000)
        if (!paging_map_huge(WINDOW + off, base + off, PTE_WRITE)) {
            kfree(out);
            return NULL;
        }

    memcpy(out, (const void *)(uintptr_t)(WINDOW + offset), len);

    /* Put the identity mapping back before anything else can notice it gone. */
    for (uint64_t off = 0; off < span; off += 0x200000)
        paging_map_huge(WINDOW + off, WINDOW + off, PTE_WRITE);

    return out;
}

static bool checksum_ok(const void *data, uint64_t len)
{
    const uint8_t *p = data;
    uint8_t sum = 0;

    while (len--)
        sum = (uint8_t)(sum + *p++);

    return sum == 0;
}

/* Read a table's header to learn its length, then copy the whole thing. */
static struct acpi_header *copy_table(uint64_t phys)
{
    struct acpi_header *head = copy_physical(phys, sizeof(*head));
    if (!head)
        return NULL;

    uint32_t len = head->length;
    kfree(head);

    if (len < sizeof(struct acpi_header))
        return NULL;

    struct acpi_header *table = copy_physical(phys, len);
    if (!table)
        return NULL;

    if (!checksum_ok(table, len)) {
        kfree(table);
        return NULL;
    }

    return table;
}

/* ------------------------------------------------------------------ *
 *  Finding the root pointer
 * ------------------------------------------------------------------ */

static bool rsdp_valid(const struct rsdp *r)
{
    if (memcmp(r->signature, "RSD PTR ", 8) != 0)
        return false;
    if (!checksum_ok(r, 20))
        return false;
    if (r->revision >= 2 && !checksum_ok(r, r->length))
        return false;
    return true;
}

static const struct rsdp *scan_range(uint64_t from, uint64_t to)
{
    for (uint64_t a = from; a + sizeof(struct rsdp) <= to; a += 16) {
        const struct rsdp *r = (const struct rsdp *)(uintptr_t)a;
        if (rsdp_valid(r))
            return r;
    }
    return NULL;
}

/* Look where a BIOS leaves it: the extended BIOS data area near the top of
 * conventional memory, then the read-only region at the top of the first
 * megabyte.  Both are inside the identity map, so this is a plain search.
 *
 * The specification says to take the address of the extended data area from the
 * word at 0x40E, which cannot be done here: page zero is left unmapped so that
 * a null dereference faults instead of quietly working, and 0x40E is inside it.
 * Searching the 128 KiB the area is always allocated from finds the same thing
 * -- and a wrong hit is not really possible, since a candidate has to carry the
 * signature and two checksums. */
static const struct rsdp *scan_for_rsdp(void)
{
    const struct rsdp *r = scan_range(0x80000, 0xA0000);

    return r ? r : scan_range(0xE0000, 0x100000);
}

/* ------------------------------------------------------------------ *
 *  The one piece of AML we read
 * ------------------------------------------------------------------ */

/* Find \_S5 in the DSDT and take the two sleep type values out of it.
 *
 * The object is a package of small integers, and the shape it takes in
 * practice is fixed enough to read without an interpreter:
 *
 *     08          NameOp                (sometimes 08 5C, with a root prefix)
 *     5F 53 35 5F "_S5_"
 *     12          PackageOp
 *     xx          package length
 *     04          element count
 *     0A xx       SLP_TYPa              (or a one-byte constant, 00 or 01)
 *     0A xx       SLP_TYPb
 *
 * Anything that does not match is left alone rather than guessed at: writing a
 * wrong sleep type to the power management block is a way to hang a machine,
 * not to switch it off. */
static bool parse_s5(const uint8_t *aml, uint32_t len)
{
    if (len < 8)
        return false;

    for (uint32_t i = 0; i + 8 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4) != 0)
            continue;

        /* Only a name declaration counts; the same four bytes can appear
         * inside a method that merely refers to it. */
        bool named = (i >= 1 && aml[i - 1] == 0x08) ||
                     (i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\');
        if (!named)
            continue;

        const uint8_t *p = aml + i + 4;
        if (*p++ != 0x12)                     /* PackageOp */
            continue;

        /* The package length's top two bits say how many bytes it spans; the
         * element count follows it. */
        p += ((*p & 0xC0) >> 6) + 2;
        if (p >= aml + len)
            return false;

        if (*p == 0x0A)                       /* BytePrefix */
            p++;
        if (p >= aml + len)
            return false;
        slp_typ_a = (uint16_t)(*p++ & 0x07);

        if (p < aml + len && *p == 0x0A)
            p++;
        slp_typ_b = (p < aml + len) ? (uint16_t)(*p & 0x07) : 0;

        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ *
 *  Bringing it together
 * ------------------------------------------------------------------ */

/* The address of a PM1 control block: the 32-bit field, or the extended one
 * when the 32-bit field is empty and the extended one is present and in I/O
 * space. */
static uint16_t pm1_address(uint32_t legacy, const struct acpi_gas *extended)
{
    if (legacy)
        return (uint16_t)legacy;
    if (extended && extended->space_id == GAS_SYSTEM_IO && extended->address)
        return (uint16_t)extended->address;
    return 0;
}

/* Walk the root table's list and copy out the first entry whose signature
 * matches.  `min_length` rejects a table too short to hold what the caller is
 * about to read out of it. */
static struct acpi_header *find_table(const struct acpi_header *root, bool xsdt,
                                      const char *signature, uint32_t min_length)
{
    uint32_t entries = (root->length - sizeof(*root)) / (xsdt ? 8 : 4);
    const uint8_t *list = (const uint8_t *)(root + 1);

    for (uint32_t i = 0; i < entries; i++) {
        uint64_t phys;

        if (xsdt)
            memcpy(&phys, list + i * 8, 8);
        else {
            uint32_t p32;
            memcpy(&p32, list + i * 4, 4);
            phys = p32;
        }

        struct acpi_header *table = copy_table(phys);
        if (!table)
            continue;

        if (memcmp(table->signature, signature, 4) == 0 &&
            table->length >= min_length)
            return table;

        kfree(table);
    }

    return NULL;
}

/* Where the root table was, so a later caller can go back for a table this
 * file has no interest in itself. */
static uint64_t root_phys;
static bool     root_is_xsdt;

void *acpi_table(const char *signature, uint32_t *length_out)
{
    if (length_out)
        *length_out = 0;
    if (!root_phys)
        return NULL;

    struct acpi_header *root = copy_table(root_phys);
    if (!root)
        return NULL;

    struct acpi_header *table =
        find_table(root, root_is_xsdt, signature, sizeof(*root));
    kfree(root);

    if (table && length_out)
        *length_out = table->length;
    return table;
}

void acpi_init(const struct multiboot_info *mbi)
{
    const struct rsdp *rsdp;
    struct rsdp copy;

    /* The UEFI loader passes the address on; a BIOS boot has to go looking. */
    if (mbi->flags & MB_FLAG_WOS_RSDP) {
        struct rsdp *from_firmware = copy_physical(mbi->rsdp, sizeof(copy));
        if (!from_firmware)
            return;
        copy = *from_firmware;
        kfree(from_firmware);

        if (!rsdp_valid(&copy))
            return;
        rsdp = &copy;
    } else {
        rsdp = scan_for_rsdp();
        if (!rsdp)
            return;
    }

    bool xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    struct acpi_header *root =
        copy_table(xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    if (!root)
        return;

    /* Remember how to get back here: acpi_table() serves the tables other
     * subsystems want -- the MADT for the processor list, and the rest. */
    root_phys    = xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;
    root_is_xsdt = xsdt;

    const struct acpi_fadt *fadt =
        (const struct acpi_fadt *)find_table(root, xsdt, "FACP", FADT_MINIMUM);
    kfree(root);
    if (!fadt)
        return;

    pm1a_cnt = pm1_address(fadt->pm1a_cnt_blk,
                           FADT_HAS(fadt, x_pm1a_cnt_blk) ? &fadt->x_pm1a_cnt_blk
                                                          : NULL);
    pm1b_cnt = pm1_address(fadt->pm1b_cnt_blk,
                           FADT_HAS(fadt, x_pm1b_cnt_blk) ? &fadt->x_pm1b_cnt_blk
                                                          : NULL);
    smi_cmd  = (uint16_t)fadt->smi_cmd;
    acpi_enable_value = fadt->acpi_enable;

    uint64_t dsdt_phys = fadt->dsdt;
    if (!dsdt_phys && FADT_HAS(fadt, x_dsdt))
        dsdt_phys = fadt->x_dsdt;
    kfree((struct acpi_fadt *)fadt);

    struct acpi_header *dsdt = copy_table(dsdt_phys);
    if (!dsdt)
        return;

    have_s5 = parse_s5((const uint8_t *)(dsdt + 1),
                       dsdt->length - sizeof(*dsdt));
    kfree(dsdt);
}

void acpi_power_off(void)
{
    if (!acpi_can_power_off())
        return;

    /* On a machine that came up in legacy mode, the chipset is not listening to
     * the power management registers yet.  Asking it to switch is a write to
     * the SMI command port; SCI_EN coming up says it happened.  A machine with
     * no command port is already in ACPI mode, which is how UEFI leaves it. */
    if (smi_cmd && !(inw(pm1a_cnt) & SCI_EN)) {
        outb(smi_cmd, acpi_enable_value);

        for (int i = 0; i < 300 && !(inw(pm1a_cnt) & SCI_EN); i++)
            for (volatile int spin = 0; spin < 100000; spin++)
                ;
    }

    outw(pm1a_cnt, (uint16_t)((slp_typ_a << SLP_TYP_SHIFT) | SLP_EN));
    if (pm1b_cnt)
        outw(pm1b_cnt, (uint16_t)((slp_typ_b << SLP_TYP_SHIFT) | SLP_EN));
}
