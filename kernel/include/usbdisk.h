/* USB mass storage, as a disk.
 *
 * Same shape as the ATA driver: 512-byte sectors, synchronous, no caching.
 * Underneath it is the bulk-only transport every USB stick speaks -- a SCSI
 * command wrapped in a 31-byte header, the data, and a 13-byte status.
 */
#ifndef WOS_USBDISK_H
#define WOS_USBDISK_H

#include "types.h"

/* Find a USB disk and get it ready.  False if there is no USB controller, no
 * device on it, or the device is not a mass storage one. */
bool usbdisk_init(void);

bool usbdisk_present(void);

/* Total 512-byte sectors, as the device reports them. */
uint32_t usbdisk_sector_count(void);

/* Read/write `count` sectors from LBA `lba`.  False on any error. */
bool usbdisk_read_sectors(uint32_t lba, uint8_t count, void *buf);
bool usbdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* Why no disk was found, in a few words.  Meaningless once one has been. */
const char *usbdisk_error(void);

/* What the device calls itself, for the boot log: vendor and product from its
 * INQUIRY response, or "" if it never answered one. */
const char *usbdisk_name(void);

#endif /* WOS_USBDISK_H */
