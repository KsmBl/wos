/* RAM disk: a WFS volume the bootloader loaded into memory.
 *
 * On a machine with no drive the ATA driver can reach -- booting off a USB
 * stick, which needs a USB host controller and a mass-storage driver this
 * kernel does not have -- GRUB loads build/wos.img as a Multiboot module and
 * the filesystem is mounted straight out of RAM instead.
 *
 * It presents the same 512-byte sector interface as ata.h, so wfs.c talks to
 * either without caring which it got.  Writes land in memory and are lost at
 * power off: there is nowhere to write them back to.
 */
#ifndef WOS_RAMDISK_H
#define WOS_RAMDISK_H

#include "types.h"

/* Adopt the region [phys, phys+bytes) as the RAM disk.  The caller must have
 * reserved it from the frame allocator first.  Returns false if the region is
 * unusable -- empty, or outside the identity-mapped low gigabyte, where the
 * kernel could not address it without mapping it. */
bool ramdisk_init(uint64_t phys, uint64_t bytes);

bool ramdisk_present(void);

uint32_t ramdisk_sector_count(void);

/* Read/write `count` sectors starting at LBA `lba`.  Return false when the
 * request runs past the end of the disk. */
bool ramdisk_read_sectors(uint32_t lba, uint8_t count, void *buf);
bool ramdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf);

#endif /* WOS_RAMDISK_H */
