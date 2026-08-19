/* USB mass storage, as a disk.
 *
 * Same shape as the ATA driver: 512-byte sectors, synchronous, no caching.
 * Underneath it is the bulk-only transport every USB stick speaks -- a SCSI
 * command wrapped in a 31-byte header, the data, and a 13-byte status.
 */
#ifndef WOS_USBDISK_H
#define WOS_USBDISK_H

#include "types.h"
#include "wosconfig.h"

/* Find a USB disk and get it ready.  False if there is no USB controller, no
 * device on it, or the device is not a mass storage one. */
#if CONFIG_XHCI

bool usbdisk_init(void);

bool usbdisk_present(void);

/* Total 512-byte sectors, as the device reports them. */
uint32_t usbdisk_sector_count(void);

/* Read/write `count` sectors from LBA `lba`.  False on any error. */
bool usbdisk_read_sectors(uint32_t lba, uint8_t count, void *buf);
bool usbdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* Why no disk was found, in a few words.  Meaningless once one has been. */
const char *usbdisk_error(void);

/* Print what the probe saw: each device, what it claimed to be, and what became
 * of it.  Kept so a machine that found no disk can show its working after the
 * boot log has scrolled away. */
void usbdisk_print_report(void);

/* What the device calls itself, for the boot log: vendor and product from its
 * INQUIRY response, or "" if it never answered one. */
const char *usbdisk_name(void);


#else

/* Built without the USB host controller: there is no USB disk, and everything
 * that asks is told so plainly rather than having to know why. */
static inline bool usbdisk_init(void) { return false; }
static inline bool usbdisk_present(void) { return false; }
static inline uint32_t usbdisk_sector_count(void) { return 0; }
static inline bool usbdisk_read_sectors(uint32_t lba, uint8_t count, void *buf)
{ (void)lba; (void)count; (void)buf; return false; }
static inline bool usbdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{ (void)lba; (void)count; (void)buf; return false; }
static inline const char *usbdisk_error(void)
{ return "this kernel was built without USB support"; }
static inline void usbdisk_print_report(void) { }
static inline const char *usbdisk_name(void) { return ""; }

#endif /* CONFIG_XHCI */

#endif /* WOS_USBDISK_H */
