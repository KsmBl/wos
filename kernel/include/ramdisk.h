/* RAM disk: a WFS volume the bootloader loaded into memory.
 *
 * On a machine no disk driver here can reach -- a USB stick behind a hub, or an
 * old controller that is neither ATA nor xHCI -- the bootloader loads
 * build/wos.img into memory and the filesystem is mounted straight out of it.
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

/* Bytes of memory it occupies. */
uint64_t ramdisk_bytes(void);

/* Give the memory back and forget the disk.  For when the volume turned out to
 * be on a real device after all, and this copy of it is just a duplicate of
 * what is on the disk taking up tens of megabytes. */
void ramdisk_release(void);

/* Read/write `count` sectors starting at LBA `lba`.  Return false when the
 * request runs past the end of the disk. */
bool ramdisk_read_sectors(uint32_t lba, uint8_t count, void *buf);
bool ramdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf);

#endif /* WOS_RAMDISK_H */
