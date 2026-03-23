/* ATA PIO driver for the primary bus master. */

#include "ata.h"
#include "io.h"

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

/* Generous, but bounded: a wedged controller must not hang the kernel. */
#define ATA_TIMEOUT 1000000

static bool     drive_present;
static uint32_t drive_sectors;

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

    return true;
}

bool ata_present(void) { return drive_present; }

uint32_t ata_sector_count(void) { return drive_sectors; }

bool ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    if (!drive_present || count == 0)
        return false;

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
