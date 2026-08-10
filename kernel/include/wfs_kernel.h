/* WFS driver: the kernel-side filesystem operations.
 *
 * All functions return 0 or a positive count on success, and a negative
 * W_E* code on failure.  Paths passed here must be absolute and already
 * normalised; the VFS layer is responsible for resolving relative paths
 * against a process's working directory.
 */
#ifndef WOS_WFS_KERNEL_H
#define WOS_WFS_KERNEL_H

#include "types.h"
#include "wfs.h"
#include "wabi.h"

/* Where a mounted volume was found. */
typedef enum {
    WFS_SOURCE_NONE = 0,
    WFS_SOURCE_ATA,
    WFS_SOURCE_USB,
    WFS_SOURCE_RAMDISK,
} wfs_source_t;

/* Find and validate the volume: an ATA disk first, then a USB one, then the
 * image the bootloader loaded into memory.  On a real device it may be a
 * partition rather than the whole disk, which is how a USB stick carries both
 * the loader's FAT partition and this filesystem.  False if none of them holds
 * a WFS volume. */
bool wfs_mount(void);
bool wfs_mounted(void);

wfs_source_t wfs_source(void);

/* True when the mounted volume is the copy in memory, i.e. changes are not
 * persistent. */
bool wfs_on_ramdisk(void);

/* Resolve an absolute path to an inode number. */
int wfs_lookup(const char *path, uint32_t *ino_out);

/* Load an inode by number. */
int wfs_read_inode(uint32_t ino, struct wfs_inode *out);

/* Read up to `len` bytes from `offset`. Returns the byte count, which is
 * short at end of file and 0 at or past the end. */
int wfs_read(uint32_t ino, uint32_t offset, void *buf, uint32_t len);

/* Write `len` bytes at `offset`, growing the file and allocating blocks as
 * needed. Returns the byte count written. */
int wfs_write(uint32_t ino, uint32_t offset, const void *buf, uint32_t len);

/* Create a file or directory at an absolute path.  `type` is WFS_TYPE_FILE
 * or WFS_TYPE_DIR.  Fails with -W_EEXIST if the name is taken. */
int wfs_create(const char *path, uint16_t type, uint32_t *ino_out);

/* Remove a file, or an empty directory. */
int wfs_unlink(const char *path);
int wfs_rename(const char *from, const char *to);

/* Release every block of a file and set its size to zero. */
int wfs_truncate(uint32_t ino);

/* Read directory entry number `index` (0-based, skipping free slots).
 * Returns 1 when an entry was produced, 0 at the end of the directory. */
int wfs_readdir(uint32_t ino, uint32_t index, wdirent_t *out);

/* Fill in the volume's space and inode usage. */
void wfs_statfs(wdiskinfo_t *out);

#endif /* WOS_WFS_KERNEL_H */
