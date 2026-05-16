/* RAM disk backed by a Multiboot module. See ramdisk.h. */

#include "ramdisk.h"
#include "paging.h"
#include "string.h"

#define SECTOR_BYTES 512u

static uint8_t *base;
static uint32_t sectors;

bool ramdisk_init(uint64_t phys, uint64_t bytes)
{
    if (!phys || bytes < SECTOR_BYTES)
        return false;

    /* The low gigabyte is identity mapped in every address space, so a module
     * that fits inside it is directly addressable.  GRUB loads modules low,
     * but a kernel that trusted that blindly would fault instead of saying
     * why. */
    if (phys + bytes > LOW_MEMORY_LIMIT)
        return false;

    base    = (uint8_t *)(uintptr_t)phys;
    sectors = (uint32_t)(bytes / SECTOR_BYTES);
    return true;
}

bool ramdisk_present(void) { return base != NULL; }

uint32_t ramdisk_sector_count(void) { return sectors; }

/* Both directions need the same bounds check; `count` is a uint8_t, so the
 * sum cannot overflow a uint32_t. */
static bool in_range(uint32_t lba, uint8_t count)
{
    return base && count && lba + count <= sectors;
}

bool ramdisk_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    if (!in_range(lba, count))
        return false;

    memcpy(buf, base + (uint64_t)lba * SECTOR_BYTES,
           (uint64_t)count * SECTOR_BYTES);
    return true;
}

bool ramdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    if (!in_range(lba, count))
        return false;

    memcpy(base + (uint64_t)lba * SECTOR_BYTES, buf,
           (uint64_t)count * SECTOR_BYTES);
    return true;
}
