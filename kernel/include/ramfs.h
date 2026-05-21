/* /ramdisk: a filesystem that is only ever in memory.
 *
 * Everything else lives on the disk and survives a reboot.  This one does not,
 * and is not meant to: it is scratch space -- somewhere to put a build, a
 * download, a temporary file -- that costs nothing when it is empty and is gone
 * when the machine stops.
 *
 * It has no fixed size.  A file grows a page at a time out of ordinary free
 * memory, and gives every page back when it shrinks or is deleted, so what it
 * takes from the machine is what is in it and nothing more.
 *
 * The calls mirror the WFS ones exactly, so the layer above can pick between
 * the two by path and be otherwise unaware there are two.
 */
#ifndef WOS_RAMFS_H
#define WOS_RAMFS_H

#include "types.h"
#include "wfs.h"
#include "wabi.h"

/* Where it is mounted.  A path is this directory, or below it. */
#define RAMFS_MOUNT "/ramdisk"

/* Build the empty root directory.  Called once at boot. */
void ramfs_init(void);

/* True if this absolute path belongs to the RAM disk. */
bool ramfs_owns(const char *abs);

int  ramfs_lookup(const char *path, uint32_t *ino_out);
int  ramfs_read_inode(uint32_t ino, struct wfs_inode *out);
int  ramfs_read(uint32_t ino, uint32_t offset, void *buf, uint32_t len);
int  ramfs_write(uint32_t ino, uint32_t offset, const void *buf, uint32_t len);
int  ramfs_create(const char *path, uint16_t type, uint32_t *ino_out);
int  ramfs_unlink(const char *path);
int  ramfs_truncate(uint32_t ino);
int  ramfs_readdir(uint32_t ino, uint32_t index, wdirent_t *out);

/* What it is holding, and what it could still grow into. */
void ramfs_statfs(wdiskinfo_t *out);

/* Bytes currently held, for the boot log and for `df`. */
uint64_t ramfs_used_bytes(void);

#endif /* WOS_RAMFS_H */
