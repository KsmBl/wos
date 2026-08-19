/* ATA driver for the primary bus, by DMA where the controller offers it and by
 * PIO where it does not.
 *
 * PIO moves a sector by asking the processor to run five hundred and twelve
 * bytes through a sixteen-bit port, one word per instruction, with the drive
 * setting the pace.  The processor does nothing else for the duration and the
 * ceiling is a few megabytes a second no matter how fast the disk is.
 *
 * Bus-master DMA has the controller do the moving.  A table of physical
 * regions is handed over, the drive is told to start, and the processor is
 * free until it finishes.  Every PCI IDE controller made in the last thirty
 * years can do it, including the one QEMU emulates, and it is the difference
 * between a disk that reads at PIO speed and one that reads at its own.
 *
 * The PIO path is kept, and not only as a fallback: it is what runs before the
 * PCI bus has been scanned, and on a controller that turns out not to have a
 * bus-master interface at all.
 */

#include "ata.h"
#include "pci.h"
#include "dma.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"

#define ATA_IO_BASE   0x1F0
#define ATA_CTRL_BASE 0x3F6

#define ATA_SECTOR_BYTES 512u
#define ATA_SECTOR_WORDS (ATA_SECTOR_BYTES / 2u)

/* Registers, as offsets from ATA_IO_BASE. */
#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1
#define ATA_REG_FEATURES  1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA_LO    3
#define ATA_REG_LBA_MID   4
#define ATA_REG_LBA_HI    5
#define ATA_REG_DRIVE     6
#define ATA_REG_STATUS    7
#define ATA_REG_COMMAND   7

/* Status register bits. */
#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_RDY  0x40
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_FLUSH_CACHE   0xE7
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_CMD_READ_DMA      0xC8
#define ATA_CMD_WRITE_DMA     0xCA

/* Generous, but bounded: a wedged controller must not hang the kernel. */
#define ATA_TIMEOUT 1000000

/* The bus master interface, as offsets from the base in BAR4. */
#define BM_COMMAND 0
#define BM_STATUS  2
#define BM_PRDT    4

#define BM_CMD_START 0x01
#define BM_CMD_READ  0x08      /* set: device to memory */

#define BM_ST_ERROR     0x02
#define BM_ST_INTERRUPT 0x04

static bool     drive_present;
static uint32_t drive_sectors;

/* ------------------------------------------------------------------ *
 *  Bus-master DMA
 * ------------------------------------------------------------------ */

/* Defined below, with the rest of the PIO path: waiting on the drive and
 * programming an LBA are the same whichever way the bytes will move. */
static bool ata_wait_ready(void);
static void ata_setup_lba(uint32_t lba, uint8_t count);

/* One physical region descriptor.  The controller reads these directly, so the
 * layout is the hardware's and not ours. */
struct prd {
    uint32_t addr;             /* physical, below 4 GiB      */
    uint16_t bytes;            /* 0 means 65536              */
    uint16_t flags;            /* 0x8000 on the last entry   */
} __attribute__((packed));

static uint16_t     bm_base;         /* 0 until a controller is found */
static dma_buffer_t bm_prdt;         /* the descriptor table          */
static dma_buffer_t bm_bounce;       /* what the controller reads and writes */

/* The bounce buffer's size, and so the largest transfer DMA will take on.
 * Sixty-four kilobytes is one region descriptor's worth and 128 sectors, which
 * is more than the 8-bit sector count can ask for anyway. */
#define BM_BOUNCE_BYTES 65536u

/* Why bounce at all: the caller's buffer is very often on the kernel stack or
 * in the heap, where the bytes are contiguous virtually but nothing promises
 * they are contiguous physically -- and a region descriptor describes physical
 * memory.  Copying through a buffer that was allocated for the purpose is one
 * memcpy against the certainty of not scribbling on whatever else happens to
 * live in the next physical page. */

static void ata_dma_init(void)
{
    /* Class 1 (mass storage), subclass 1 (IDE).  The programming interface
     * byte is not matched on because it says which channels are in native mode
     * rather than what the controller is -- but bit 7 of it is worth reading
     * once found, because that is the bit that says there is a bus-master
     * interface behind BAR4 at all. */
    pci_device_t dev = pci_find_class(0x01, 0x01, PCI_PROG_IF_ANY);

    if (!dev.found)
        return;

    uint8_t prog_if = (uint8_t)(pci_read32(dev.bus, dev.slot, dev.func, 0x08)
                                >> 8);
    if (!(prog_if & 0x80))
        return;

    /* BAR4 holds the bus master registers, and it is an I/O BAR rather than a
     * memory one -- which is why it is read with pci_bar_io and not the
     * address form, whose answer for an I/O BAR is zero. */
    uint16_t bar4 = pci_bar_io(&dev, 4);
    if (!bar4)
        return;

    bm_prdt = dma_alloc(sizeof(struct prd), 8);
    if (!bm_prdt.virt)
        return;

    bm_bounce = dma_alloc(BM_BOUNCE_BYTES, 0x10000);
    if (!bm_bounce.virt) {
        dma_free(&bm_prdt);
        return;
    }

    /* A region may not cross a 64 KiB boundary, which is why the buffer was
     * asked for on one. */
    if ((bm_prdt.phys >> 32) || (bm_bounce.phys >> 32) ||
        ((bm_bounce.phys & 0xFFFF) != 0)) {
        dma_free(&bm_bounce);
        dma_free(&bm_prdt);
        return;
    }

    pci_enable_bus_master(&dev);
    bm_base = bar4;

    kprintf("ata    : bus-master DMA at 0x%x\n", bm_base);
}

/* Run one transfer.  False means the caller should fall back to PIO, which is
 * always correct if slower. */
static bool ata_dma_transfer(uint32_t lba, uint8_t count, bool read)
{
    uint32_t bytes = (uint32_t)count * ATA_SECTOR_BYTES;

    struct prd *prd = bm_prdt.virt;
    prd->addr  = (uint32_t)bm_bounce.phys;
    prd->bytes = (uint16_t)(bytes == 65536u ? 0 : bytes);
    prd->flags = 0x8000;                 /* the last, and only, region */

    /* Stop anything in flight, then clear the sticky error and interrupt bits
     * by writing them back -- they are latched, and a stale one would be read
     * as this transfer finishing before it started. */
    outb(bm_base + BM_COMMAND, 0);
    outb(bm_base + BM_STATUS,
         (uint8_t)(inb(bm_base + BM_STATUS) | BM_ST_ERROR | BM_ST_INTERRUPT));

    outl(bm_base + BM_PRDT, (uint32_t)bm_prdt.phys);
    outb(bm_base + BM_COMMAND, read ? BM_CMD_READ : 0);

    if (!ata_wait_ready())
        return false;

    ata_setup_lba(lba, count);
    outb(ATA_IO_BASE + ATA_REG_COMMAND,
         read ? ATA_CMD_READ_DMA : ATA_CMD_WRITE_DMA);

    /* Only now: the drive must have the command before the controller starts
     * moving bytes for it. */
    outb(bm_base + BM_COMMAND,
         (uint8_t)((read ? BM_CMD_READ : 0) | BM_CMD_START));

    bool ok = false;

    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(bm_base + BM_STATUS);

        if (st & BM_ST_ERROR)
            break;

        /* The interrupt bit is raised whether or not anyone is listening for
         * the interrupt itself, which is what makes polling here legitimate
         * rather than a shortcut. */
        if (st & BM_ST_INTERRUPT) {
            ok = true;
            break;
        }
    }

    outb(bm_base + BM_COMMAND, 0);
    outb(bm_base + BM_STATUS,
         (uint8_t)(inb(bm_base + BM_STATUS) | BM_ST_ERROR | BM_ST_INTERRUPT));

    if (!ok)
        return false;

    /* The drive has its own opinion, and a controller that finished moving
     * bytes says nothing about whether the drive liked the command. */
    uint8_t status = inb(ATA_IO_BASE + ATA_REG_STATUS);
    if (status & (ATA_SR_ERR | ATA_SR_DF))
        return false;

    return true;
}

/* Reading the alternate status register four times is the standard ~400 ns
 * delay that lets the drive put valid bits on the bus. */
static void ata_delay(void)
{
    for (int i = 0; i < 4; i++)
        (void)inb(ATA_CTRL_BASE);
}

/* Wait for BSY to clear. Returns false on timeout. */
static bool ata_wait_ready(void)
{
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(ATA_IO_BASE + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY))
            return true;
    }
    return false;
}

/* Wait for BSY to clear and DRQ to appear, failing on an error bit. */
static bool ata_wait_drq(void)
{
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(ATA_IO_BASE + ATA_REG_STATUS);

        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return false;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            return true;
    }
    return false;
}

/* Select the master drive and program a 28-bit LBA request. */
static void ata_setup_lba(uint32_t lba, uint8_t count)
{
    outb(ATA_IO_BASE + ATA_REG_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_IO_BASE + ATA_REG_FEATURES, 0);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT, count);
    outb(ATA_IO_BASE + ATA_REG_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_IO_BASE + ATA_REG_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
}

bool ata_init(void)
{
    uint16_t identify[256];

    /* Select the master and issue IDENTIFY. */
    outb(ATA_IO_BASE + ATA_REG_DRIVE, 0xA0);
    ata_delay();
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA_LO, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA_MID, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA_HI, 0);
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();

    /* A status of zero means nothing is attached at all. */
    if (inb(ATA_IO_BASE + ATA_REG_STATUS) == 0)
        return false;

    if (!ata_wait_ready() || !ata_wait_drq())
        return false;

    insw(ATA_IO_BASE + ATA_REG_DATA, identify, 256);

    /* Words 60-61 hold the 28-bit LBA sector count. */
    drive_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    drive_present = true;

    /* Only now, and only if there is something to talk to: the frame allocator
     * is up by the time this runs, and there is no sense taking two buffers
     * for a bus with no drive on it. */
    ata_dma_init();

    return true;
}

bool ata_present(void) { return drive_present; }

uint32_t ata_sector_count(void) { return drive_sectors; }

bool ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    if (!drive_present || count == 0)
        return false;

    /* DMA when there is a controller for it, PIO when there is not or when the
     * transfer did not come back clean.  A failed DMA attempt has left the
     * drive idle and the bus master stopped, so retrying by the other road is
     * safe rather than hopeful. */
    if (bm_base && (uint32_t)count * ATA_SECTOR_BYTES <= BM_BOUNCE_BYTES) {
        if (ata_dma_transfer(lba, count, true)) {
            memcpy(buf, bm_bounce.virt, (uint32_t)count * ATA_SECTOR_BYTES);
            return true;
        }
    }

    if (!ata_wait_ready())
        return false;

    ata_setup_lba(lba, count);
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    uint8_t *dst = buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq())
            return false;
        insw(ATA_IO_BASE + ATA_REG_DATA, dst, ATA_SECTOR_WORDS);
        dst += ATA_SECTOR_BYTES;
    }
    return true;
}

bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    if (!drive_present || count == 0)
        return false;

    if (bm_base && (uint32_t)count * ATA_SECTOR_BYTES <= BM_BOUNCE_BYTES) {
        memcpy(bm_bounce.virt, buf, (uint32_t)count * ATA_SECTOR_BYTES);

        if (ata_dma_transfer(lba, count, false)) {
            /* The drive may still be holding this in its own cache, and the
             * PIO path flushes for the same reason. */
            outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
            return ata_wait_ready();
        }
    }

    if (!ata_wait_ready())
        return false;

    ata_setup_lba(lba, count);
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    const uint8_t *src = buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq())
            return false;
        /* The drive wants PIO writes one word at a time; `rep outsw` is
         * allowed but some controllers need the pacing, so keep it simple. */
        outsw(ATA_IO_BASE + ATA_REG_DATA, src, ATA_SECTOR_WORDS);
        src += ATA_SECTOR_BYTES;
    }

    /* Without an explicit flush the data may sit in the drive's write cache
     * and be lost, which would defeat the point of a persistent disk. */
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
    return ata_wait_ready();
}
