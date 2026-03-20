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

#define WFS_MAGIC        0x31534657u   /* "WFS1" */
#define WFS_BLOCK_SIZE   1024u
#define WFS_SECTOR_SIZE  512u
#define WFS_SECTORS_PER_BLOCK (WFS_BLOCK_SIZE / WFS_SECTOR_SIZE)

/* Inode 0 means "no inode", so the root is 1. */
#define WFS_INVALID_INO  0u
#define WFS_ROOT_INO     1u

#define WFS_INODE_SIZE   64u
#define WFS_INODES_PER_BLOCK (WFS_BLOCK_SIZE / WFS_INODE_SIZE)

/* Block pointers held directly in the inode, plus one single-indirect block.
 * That gives 12 KiB + 256 KiB = 268 KiB per file, comfortably more than any
 * program or source file this system stores. */
#define WFS_DIRECT       12u
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

/* Exactly 64 bytes; 16 fit in a block with no padding. */
struct wfs_inode {
    uint16_t type;                     /* WFS_TYPE_*                       */
    uint16_t links;                    /* directory entries pointing here  */
    uint32_t size;                     /* length in bytes                  */
    uint32_t blocks;                   /* data blocks plus the indirect one */
    uint32_t direct[WFS_DIRECT];
    uint32_t indirect;
};

/* Exactly 32 bytes; 32 fit in a block. A zero inode marks a free slot. */
struct wfs_dirent {
    uint32_t ino;
    char     name[WFS_NAME_MAX + 1];
};

#endif /* WOS_WFS_H */
