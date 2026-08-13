/* WFS -- the WOS filesystem, on-disk format.
 *
 * Shared verbatim between the kernel and the host-side mkwfs tool, so there is
 * exactly one definition of the layout.  Compile with -DWOS_KERNEL inside the
 * kernel; the host tool gets <stdint.h> instead.
 *
 * Layout of the volume, in blocks:
 *
 *   0                     superblock
 *   1 .. bitmap_blocks    block bitmap, one bit per block, 1 = allocated
 *   .. inode_blocks       inode table, 16 inodes per block
 *   data_start ..         file and directory contents
 *
 * The block bitmap is the authority for used and free disk space, which is
 * what wdiskinfo() reports.
 *
 * Directories are ordinary files whose contents are fixed 32-byte records, so
 * reading a directory needs no variable-length record parsing.  "." and ".."
 * are real entries, which makes path resolution uniform.
 */
#ifndef WOS_WFS_H
#define WOS_WFS_H

#ifdef WOS_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

/* "WFS2".  The 1 was the same layout without a modification time in the inode.
 * The field had to come out of the direct block pointers, which moved
 * everything after them, so a version 1 volume cannot be read here at all --
 * the magic says so plainly rather than letting the driver find file contents
 * where the block numbers used to be. */
#define WFS_MAGIC        0x32534657u   /* "WFS2" */
#define WFS_MAGIC_V1     0x31534657u   /* "WFS1", recognised only to say so */
#define WFS_BLOCK_SIZE   1024u
#define WFS_SECTOR_SIZE  512u
#define WFS_SECTORS_PER_BLOCK (WFS_BLOCK_SIZE / WFS_SECTOR_SIZE)

/* Inode 0 means "no inode", so the root is 1. */
#define WFS_INVALID_INO  0u
#define WFS_ROOT_INO     1u

#define WFS_INODE_SIZE   64u
#define WFS_INODES_PER_BLOCK (WFS_BLOCK_SIZE / WFS_INODE_SIZE)

/* Block pointers held directly in the inode, plus one single-indirect block.
 * That gives 11 KiB + 256 KiB = 267 KiB per file, comfortably more than any
 * program or source file this system stores.
 *
 * Eleven rather than the twelve a Unix inode traditionally has, because the
 * inode is 64 bytes and was exactly full: the modification time is where the
 * twelfth pointer was.  The kilobyte it costs is the cheapest thing in the
 * structure -- the largest file on the disk is the kernel, at about 216 KiB. */
#define WFS_DIRECT       11u
#define WFS_PTRS_PER_BLOCK (WFS_BLOCK_SIZE / 4u)
#define WFS_MAX_FILE_SIZE ((WFS_DIRECT + WFS_PTRS_PER_BLOCK) * WFS_BLOCK_SIZE)

/* Inode types. These match W_FT_* in wabi.h. */
#define WFS_TYPE_FREE 0
#define WFS_TYPE_FILE 1
#define WFS_TYPE_DIR  2

#define WFS_NAME_MAX  27               /* 28-byte field including the NUL */
#define WFS_DIRENT_SIZE 32u
#define WFS_DIRENTS_PER_BLOCK (WFS_BLOCK_SIZE / WFS_DIRENT_SIZE)

struct wfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t data_start;
    uint32_t root_inode;
};

/* Exactly 64 bytes; 16 fit in a block with no padding.
 *
 * `mtime` is seconds since 1970, from the CMOS clock, and is what tells a make
 * whether a target is older than what it was built from.  Thirty-two bits of
 * seconds run out in 2106, which is long enough for a filesystem whose largest
 * file is a quarter of a megabyte.  Zero means "never written", which is what a
 * volume made by a tool that did not know about the field would say -- and what
 * the root directory says until something is written in it. */
struct wfs_inode {
    uint16_t type;                     /* WFS_TYPE_*                       */
    uint16_t links;                    /* directory entries pointing here  */
    uint32_t size;                     /* length in bytes                  */
    uint32_t blocks;                   /* data blocks plus the indirect one */
    uint32_t mtime;                    /* last modified, seconds since 1970 */
    uint32_t direct[WFS_DIRECT];
    uint32_t indirect;
};

/* Exactly 32 bytes; 32 fit in a block. A zero inode marks a free slot. */
struct wfs_dirent {
    uint32_t ino;
    char     name[WFS_NAME_MAX + 1];
};

/* Both structures are read out of a block by their offset within it, so a
 * change that alters either size silently moves every record after the first.
 * The build is the place to find that out. */
_Static_assert(sizeof(struct wfs_inode) == WFS_INODE_SIZE,
               "the inode must be exactly WFS_INODE_SIZE bytes");
_Static_assert(sizeof(struct wfs_dirent) == WFS_DIRENT_SIZE,
               "the directory entry must be exactly WFS_DIRENT_SIZE bytes");

#endif /* WOS_WFS_H */
