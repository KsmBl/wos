/* ATA (IDE) disk driver, PIO mode, primary bus master.
 *
 * Polled rather than interrupt driven: the filesystem is the only caller, all
 * of its requests are synchronous anyway, and polling removes a whole class of
 * race between the IRQ and the request that issued it.
 */
#ifndef WOS_ATA_H
#define WOS_ATA_H

#include "types.h"
#include "wosconfig.h"

/* Probe the primary master. Returns false if no drive answered, in which case
 * every other call fails. */
#if CONFIG_ATA

bool ata_init(void);

bool ata_present(void);

/* Total addressable 512-byte sectors on the drive. */
uint32_t ata_sector_count(void);

/* Read/write `count` sectors starting at LBA `lba`.  `count` must be 1..255.
 * Return false on a device error or timeout. */
bool ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);


#else

/* Built without the disk driver: no disk, and the filesystem finds nothing to
 * mount rather than failing to link. */
static inline bool ata_init(void) { return false; }
static inline bool ata_present(void) { return false; }
static inline uint32_t ata_sector_count(void) { return 0; }
static inline bool ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{ (void)lba; (void)count; (void)buf; return false; }
static inline bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{ (void)lba; (void)count; (void)buf; return false; }

#endif /* CONFIG_ATA */

#endif /* WOS_ATA_H */
