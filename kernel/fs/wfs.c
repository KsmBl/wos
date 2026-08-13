/* WFS driver.
 *
 * The block bitmap is cached in RAM because every allocation consults it, but
 * changes are written straight back to disk along with the superblock: an OS
 * that loses the free list on a hard reset has a corrupt volume, and there is
 * no fsck here to repair one.
 */

#include "wfs_kernel.h"
#include "ata.h"
#include "ramdisk.h"
#include "usbdisk.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "rtc.h"

static struct wfs_superblock sb;
static uint8_t              *block_bitmap;   /* sb.bitmap_blocks * BLOCK_SIZE */
static bool                  mounted;

/* ------------------------------------------------------------------ *
 *  Block I/O
 *
 *  The volume lives on one of three devices, and may be a partition rather
 *  than the whole of it.  wfs_mount() settles both questions; everything below
 *  goes through these two functions, so the rest of the driver never asks.
 * ------------------------------------------------------------------ */

static wfs_source_t source;
static uint32_t     partition_lba;    /* first sector of the volume */

static bool sectors_read(uint32_t lba, uint8_t count, void *buf)
{
    switch (source) {
    case WFS_SOURCE_ATA:     return ata_read_sectors(lba, count, buf);
    case WFS_SOURCE_USB:     return usbdisk_read_sectors(lba, count, buf);
    case WFS_SOURCE_RAMDISK: return ramdisk_read_sectors(lba, count, buf);
    default:                 return false;
    }
}

static bool sectors_write(uint32_t lba, uint8_t count, const void *buf)
{
    switch (source) {
    case WFS_SOURCE_ATA:     return ata_write_sectors(lba, count, buf);
    case WFS_SOURCE_USB:     return usbdisk_write_sectors(lba, count, buf);
    case WFS_SOURCE_RAMDISK: return ramdisk_write_sectors(lba, count, buf);
    default:                 return false;
    }
}

static bool block_read(uint32_t block, void *buf)
{
    return sectors_read(partition_lba + block * WFS_SECTORS_PER_BLOCK,
                        WFS_SECTORS_PER_BLOCK, buf);
}

static bool block_write(uint32_t block, const void *buf)
{
    return sectors_write(partition_lba + block * WFS_SECTORS_PER_BLOCK,
                         WFS_SECTORS_PER_BLOCK, buf);
}

static bool sync_superblock(void)
{
    uint8_t buf[WFS_BLOCK_SIZE];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(sb));
    return block_write(0, buf);
}

/* Write back only the bitmap block holding `block`'s bit. */
static bool sync_bitmap_for(uint32_t block)
{
    uint32_t index = block / (WFS_BLOCK_SIZE * 8);
    return block_write(sb.bitmap_start + index,
                       block_bitmap + index * WFS_BLOCK_SIZE);
}

/* ------------------------------------------------------------------ *
 *  Block allocation
 * ------------------------------------------------------------------ */

static bool bitmap_test(uint32_t block)
{
    return (block_bitmap[block / 8] & (1u << (block % 8))) != 0;
}

/* Allocate one zeroed data block. Returns 0 when the volume is full. */
static uint32_t block_alloc(void)
{
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++) {
        if (bitmap_test(b))
            continue;

        block_bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
        sb.free_blocks--;

        uint8_t zero[WFS_BLOCK_SIZE];
        memset(zero, 0, sizeof(zero));
        if (!block_write(b, zero))
            return 0;

        sync_bitmap_for(b);
        sync_superblock();
        return b;
    }
    return 0;
}

static void block_free(uint32_t block)
{
    if (block < sb.data_start || block >= sb.total_blocks)
        return;
    if (!bitmap_test(block))
        return;

    block_bitmap[block / 8] &= (uint8_t)~(1u << (block % 8));
    sb.free_blocks++;

    sync_bitmap_for(block);
    sync_superblock();
}

/* ------------------------------------------------------------------ *
 *  Inodes
 * ------------------------------------------------------------------ */

int wfs_read_inode(uint32_t ino, struct wfs_inode *out)
{
    if (ino == WFS_INVALID_INO || ino >= sb.total_inodes)
        return -W_EINVAL;

    uint8_t  buf[WFS_BLOCK_SIZE];
    uint32_t block = sb.inode_start + ino / WFS_INODES_PER_BLOCK;

    if (!block_read(block, buf))
        return -W_EIO;

    memcpy(out, buf + (ino % WFS_INODES_PER_BLOCK) * WFS_INODE_SIZE,
           sizeof(*out));
    return 0;
}

static int write_inode(uint32_t ino, const struct wfs_inode *in)
{
    if (ino == WFS_INVALID_INO || ino >= sb.total_inodes)
        return -W_EINVAL;

    uint8_t  buf[WFS_BLOCK_SIZE];
    uint32_t block = sb.inode_start + ino / WFS_INODES_PER_BLOCK;

    if (!block_read(block, buf))
        return -W_EIO;

    memcpy(buf + (ino % WFS_INODES_PER_BLOCK) * WFS_INODE_SIZE, in, sizeof(*in));

    if (!block_write(block, buf))
        return -W_EIO;

    return 0;
}

/* Find a free inode, claim it and initialise it to `type`. */
static int inode_alloc(uint16_t type, uint32_t *ino_out)
{
    struct wfs_inode in;

    for (uint32_t i = WFS_ROOT_INO; i < sb.total_inodes; i++) {
        if (wfs_read_inode(i, &in) < 0)
            continue;
        if (in.type != WFS_TYPE_FREE)
            continue;

        memset(&in, 0, sizeof(in));
        in.type  = type;
        in.links = 1;
        in.mtime = rtc_epoch();

        int r = write_inode(i, &in);
        if (r < 0)
            return r;

        sb.free_inodes--;
        sync_superblock();

        *ino_out = i;
        return 0;
    }
    return -W_ENOSPC;
}

static void inode_free(uint32_t ino)
{
    struct wfs_inode in;

    memset(&in, 0, sizeof(in));
    write_inode(ino, &in);

    sb.free_inodes++;
    sync_superblock();
}

int wfs_utime(uint32_t ino, uint32_t mtime)
{
    struct wfs_inode in;
    int r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    in.mtime = mtime;
    return write_inode(ino, &in);
}

/* ------------------------------------------------------------------ *
 *  Block mapping
 * ------------------------------------------------------------------ */

/* Translate a file-relative block index to a disk block, optionally
 * allocating.  `*dirty` is set when the inode itself changed. */
static int bmap(struct wfs_inode *in, uint32_t index, bool alloc,
                uint32_t *out, bool *dirty)
{
    if (index < WFS_DIRECT) {
        if (in->direct[index] == 0) {
            if (!alloc) {
                *out = 0;
                return 0;
            }
            uint32_t b = block_alloc();
            if (!b)
                return -W_ENOSPC;
            in->direct[index] = b;
            in->blocks++;
            *dirty = true;
        }
        *out = in->direct[index];
        return 0;
    }

    index -= WFS_DIRECT;
    if (index >= WFS_PTRS_PER_BLOCK)
        return -W_EFBIG;

    if (in->indirect == 0) {
        if (!alloc) {
            *out = 0;
            return 0;
        }
        uint32_t b = block_alloc();
        if (!b)
            return -W_ENOSPC;
        in->indirect = b;
        in->blocks++;      /* the indirect block itself occupies space */
        *dirty = true;
    }

    uint32_t table[WFS_PTRS_PER_BLOCK];
    if (!block_read(in->indirect, table))
        return -W_EIO;

    if (table[index] == 0) {
        if (!alloc) {
            *out = 0;
            return 0;
        }
        uint32_t b = block_alloc();
        if (!b)
            return -W_ENOSPC;
        table[index] = b;
        in->blocks++;
        *dirty = true;
        if (!block_write(in->indirect, table))
            return -W_EIO;
    }

    *out = table[index];
    return 0;
}

int wfs_truncate(uint32_t ino)
{
    struct wfs_inode in;
    int r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    for (uint32_t i = 0; i < WFS_DIRECT; i++) {
        if (in.direct[i]) {
            block_free(in.direct[i]);
            in.direct[i] = 0;
        }
    }

    if (in.indirect) {
        uint32_t table[WFS_PTRS_PER_BLOCK];
        if (block_read(in.indirect, table)) {
            for (uint32_t i = 0; i < WFS_PTRS_PER_BLOCK; i++)
                if (table[i])
                    block_free(table[i]);
        }
        block_free(in.indirect);
        in.indirect = 0;
    }

    in.size   = 0;
    in.blocks = 0;
    in.mtime  = rtc_epoch();
    return write_inode(ino, &in);
}

/* ------------------------------------------------------------------ *
 *  File read and write
 * ------------------------------------------------------------------ */

int wfs_read(uint32_t ino, uint32_t offset, void *buf, uint32_t len)
{
    struct wfs_inode in;
    int r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    if (offset >= in.size)
        return 0;
    if (offset + len > in.size)
        len = in.size - offset;

    uint8_t *dst  = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t index  = (offset + done) / WFS_BLOCK_SIZE;
        uint32_t inpos  = (offset + done) % WFS_BLOCK_SIZE;
        uint32_t chunk  = WFS_BLOCK_SIZE - inpos;
        if (chunk > len - done)
            chunk = len - done;

        uint32_t block;
        bool     dirty = false;
        r = bmap(&in, index, false, &block, &dirty);
        if (r < 0)
            return r;

        if (block == 0) {
            /* A hole reads as zeroes. */
            memset(dst + done, 0, chunk);
        } else {
            uint8_t tmp[WFS_BLOCK_SIZE];
            if (!block_read(block, tmp))
                return -W_EIO;
            memcpy(dst + done, tmp + inpos, chunk);
        }
        done += chunk;
    }

    return (int)done;
}

int wfs_write(uint32_t ino, uint32_t offset, const void *buf, uint32_t len)
{
    struct wfs_inode in;
    int r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    if (offset + len > WFS_MAX_FILE_SIZE)
        return -W_EFBIG;

    const uint8_t *src   = buf;
    uint32_t       done  = 0;
    bool           dirty = false;

    while (done < len) {
        uint32_t index = (offset + done) / WFS_BLOCK_SIZE;
        uint32_t inpos = (offset + done) % WFS_BLOCK_SIZE;
        uint32_t chunk = WFS_BLOCK_SIZE - inpos;
        if (chunk > len - done)
            chunk = len - done;

        uint32_t block;
        r = bmap(&in, index, true, &block, &dirty);
        if (r < 0)
            break;          /* out of space: keep what we managed to write */

        uint8_t tmp[WFS_BLOCK_SIZE];

        /* A partial block has to be read first so the untouched bytes
         * survive the write-back. */
        if (chunk != WFS_BLOCK_SIZE) {
            if (!block_read(block, tmp))
                return -W_EIO;
        }

        memcpy(tmp + inpos, src + done, chunk);
        if (!block_write(block, tmp))
            return -W_EIO;

        done += chunk;
    }

    if (offset + done > in.size) {
        in.size = offset + done;
        dirty = true;
    }

    /* Any write at all changes the modification time, even one that overwrote
     * bytes in a block the file already had -- which is the case the size and
     * the block list would otherwise have nothing to say about.  It is also
     * what gives a directory a time: adding and removing names is a write to
     * the directory's own contents. */
    if (done > 0) {
        in.mtime = rtc_epoch();
        dirty = true;
    }

    if (dirty)
        write_inode(ino, &in);

    if (done == 0 && len > 0)
        return r < 0 ? r : -W_ENOSPC;

    return (int)done;
}

/* ------------------------------------------------------------------ *
 *  Directories
 * ------------------------------------------------------------------ */

/* Find `name` in directory `dir_ino`. */
static int dir_find(uint32_t dir_ino, const char *name, uint32_t *ino_out)
{
    struct wfs_inode dir;
    int r = wfs_read_inode(dir_ino, &dir);
    if (r < 0)
        return r;
    if (dir.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    struct wfs_dirent ent;
    for (uint32_t off = 0; off < dir.size; off += WFS_DIRENT_SIZE) {
        r = wfs_read(dir_ino, off, &ent, sizeof(ent));
        if (r <= 0)
            break;
        if (ent.ino != WFS_INVALID_INO && strcmp(ent.name, name) == 0) {
            *ino_out = ent.ino;
            return 0;
        }
    }
    return -W_ENOENT;
}

/* Add an entry, reusing a free slot if there is one. */
static int dir_add(uint32_t dir_ino, const char *name, uint32_t ino)
{
    if (strlen(name) > WFS_NAME_MAX)
        return -W_ENAMETOOLONG;

    struct wfs_inode dir;
    int r = wfs_read_inode(dir_ino, &dir);
    if (r < 0)
        return r;

    struct wfs_dirent ent;
    uint32_t slot = dir.size;      /* default: append */

    for (uint32_t off = 0; off < dir.size; off += WFS_DIRENT_SIZE) {
        if (wfs_read(dir_ino, off, &ent, sizeof(ent)) <= 0)
            break;
        if (ent.ino == WFS_INVALID_INO) {
            slot = off;
            break;
        }
    }

    memset(&ent, 0, sizeof(ent));
    ent.ino = ino;
    strlcpy(ent.name, name, sizeof(ent.name));

    r = wfs_write(dir_ino, slot, &ent, sizeof(ent));
    return (r == (int)sizeof(ent)) ? 0 : (r < 0 ? r : -W_EIO);
}

static int dir_remove(uint32_t dir_ino, const char *name)
{
    struct wfs_inode dir;
    int r = wfs_read_inode(dir_ino, &dir);
    if (r < 0)
        return r;

    struct wfs_dirent ent;
    for (uint32_t off = 0; off < dir.size; off += WFS_DIRENT_SIZE) {
        if (wfs_read(dir_ino, off, &ent, sizeof(ent)) <= 0)
            break;
        if (ent.ino != WFS_INVALID_INO && strcmp(ent.name, name) == 0) {
            memset(&ent, 0, sizeof(ent));
            r = wfs_write(dir_ino, off, &ent, sizeof(ent));
            return (r < 0) ? r : 0;
        }
    }
    return -W_ENOENT;
}

/* True if the directory holds nothing but "." and "..". */
static bool dir_is_empty(uint32_t dir_ino)
{
    struct wfs_inode dir;
    if (wfs_read_inode(dir_ino, &dir) < 0)
        return false;

    struct wfs_dirent ent;
    for (uint32_t off = 0; off < dir.size; off += WFS_DIRENT_SIZE) {
        if (wfs_read(dir_ino, off, &ent, sizeof(ent)) <= 0)
            break;
        if (ent.ino == WFS_INVALID_INO)
            continue;
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
            continue;
        return false;
    }
    return true;
}

int wfs_readdir(uint32_t ino, uint32_t index, wdirent_t *out)
{
    struct wfs_inode dir;
    int r = wfs_read_inode(ino, &dir);
    if (r < 0)
        return r;
    if (dir.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    struct wfs_dirent ent;
    uint32_t seen = 0;

    for (uint32_t off = 0; off < dir.size; off += WFS_DIRENT_SIZE) {
        if (wfs_read(ino, off, &ent, sizeof(ent)) <= 0)
            break;
        if (ent.ino == WFS_INVALID_INO)
            continue;

        if (seen == index) {
            struct wfs_inode target;
            out->ino  = ent.ino;
            out->type = (wfs_read_inode(ent.ino, &target) == 0)
                          ? target.type : W_FT_FILE;
            strlcpy(out->name, ent.name, sizeof(out->name));
            return 1;
        }
        seen++;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Path resolution
 * ------------------------------------------------------------------ */

int wfs_lookup(const char *path, uint32_t *ino_out)
{
    if (!mounted)
        return -W_EIO;
    if (path[0] != '/')
        return -W_EINVAL;       /* the VFS makes paths absolute before this */

    uint32_t ino = sb.root_inode;
    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        char name[WFS_NAME_MAX + 1];
        uint32_t n = 0;
        while (*p && *p != '/') {
            if (n >= WFS_NAME_MAX)
                return -W_ENAMETOOLONG;
            name[n++] = *p++;
        }
        name[n] = '\0';

        int r = dir_find(ino, name, &ino);
        if (r < 0)
            return r;
    }

    *ino_out = ino;
    return 0;
}

/* Split "/a/b/c" into the parent inode and the final component "c". */
static int resolve_parent(const char *path, uint32_t *parent_ino,
                          char *name_out, size_t name_size)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -W_EINVAL;

    if (strlen(slash + 1) == 0)
        return -W_EINVAL;       /* trailing slash: no final component */
    if (strlen(slash + 1) > WFS_NAME_MAX)
        return -W_ENAMETOOLONG;

    strlcpy(name_out, slash + 1, name_size);

    /* "/foo" has an empty parent string, which means the root. */
    if (slash == path) {
        *parent_ino = sb.root_inode;
        return 0;
    }

    char parent[W_PATH_MAX + 1];
    size_t len = (size_t)(slash - path);
    if (len > W_PATH_MAX)
        return -W_ENAMETOOLONG;
    memcpy(parent, path, len);
    parent[len] = '\0';

    return wfs_lookup(parent, parent_ino);
}

int wfs_create(const char *path, uint16_t type, uint32_t *ino_out)
{
    if (!mounted)
        return -W_EIO;

    uint32_t parent;
    char     name[WFS_NAME_MAX + 1];

    int r = resolve_parent(path, &parent, name, sizeof(name));
    if (r < 0)
        return r;

    uint32_t existing;
    if (dir_find(parent, name, &existing) == 0)
        return -W_EEXIST;

    uint32_t ino;
    r = inode_alloc(type, &ino);
    if (r < 0)
        return r;

    if (type == WFS_TYPE_DIR) {
        /* "." and ".." are real entries, so path resolution needs no
         * special cases for them. */
        r = dir_add(ino, ".", ino);
        if (r == 0)
            r = dir_add(ino, "..", parent);
        if (r < 0) {
            inode_free(ino);
            return r;
        }
    }

    r = dir_add(parent, name, ino);
    if (r < 0) {
        wfs_truncate(ino);
        inode_free(ino);
        return r;
    }

    if (ino_out)
        *ino_out = ino;
    return 0;
}

int wfs_unlink(const char *path)
{
    if (!mounted)
        return -W_EIO;

    uint32_t parent;
    char     name[WFS_NAME_MAX + 1];

    int r = resolve_parent(path, &parent, name, sizeof(name));
    if (r < 0)
        return r;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -W_EINVAL;

    uint32_t ino;
    r = dir_find(parent, name, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    if (in.type == WFS_TYPE_DIR && !dir_is_empty(ino))
        return -W_ENOTEMPTY;

    r = dir_remove(parent, name);
    if (r < 0)
        return r;

    wfs_truncate(ino);
    inode_free(ino);
    return 0;
}

/* Give something a different name, or a different place, without moving what
 * is in it.
 *
 * A directory entry is a name and an inode number, so this moves the number
 * and leaves the blocks where they are -- which is what makes rename the way
 * to replace a file safely: the new contents are written under a temporary
 * name and then take the real one in a single step, and a machine that loses
 * power in the middle has either the old file or the new one and never half of
 * either.
 *
 * The order matters and is the opposite of the obvious one: the new entry goes
 * in *before* the old one comes out, so the inode is never nameless.  Removing
 * first and failing to add would lose the file. */
int wfs_rename(const char *from, const char *to)
{
    if (!mounted)
        return -W_EIO;

    uint32_t from_parent, to_parent;
    char     from_name[WFS_NAME_MAX + 1], to_name[WFS_NAME_MAX + 1];

    int r = resolve_parent(from, &from_parent, from_name, sizeof(from_name));
    if (r < 0)
        return r;

    r = resolve_parent(to, &to_parent, to_name, sizeof(to_name));
    if (r < 0)
        return r;

    if (strcmp(from_name, ".") == 0 || strcmp(from_name, "..") == 0 ||
        strcmp(to_name, ".") == 0   || strcmp(to_name, "..") == 0)
        return -W_EINVAL;

    uint32_t ino;
    r = dir_find(from_parent, from_name, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    if (from_parent == to_parent && strcmp(from_name, to_name) == 0)
        return 0;                       /* already where it is being put */

    /* A directory cannot be moved inside itself.  The walk up from the
     * destination is what catches it: a tree that reached its own child would
     * be a loop with no way back to the root, and everything that walks a path
     * would follow it forever. */
    if (in.type == WFS_TYPE_DIR) {
        uint32_t at = to_parent;

        for (int steps = 0; steps < 256; steps++) {
            if (at == ino)
                return -W_EINVAL;
            if (at == WFS_ROOT_INO)
                break;

            uint32_t up;
            if (dir_find(at, "..", &up) < 0 || up == at)
                break;
            at = up;
        }
    }

    /* Something already called that is replaced, which is what makes this an
     * atomic swap rather than a two-step one.  A directory is not, because
     * replacing one would silently discard whatever is inside it. */
    uint32_t existing;
    if (dir_find(to_parent, to_name, &existing) == 0) {
        if (existing == ino)
            return 0;

        struct wfs_inode victim;
        r = wfs_read_inode(existing, &victim);
        if (r < 0)
            return r;
        if (victim.type == WFS_TYPE_DIR)
            return -W_EISDIR;

        r = dir_remove(to_parent, to_name);
        if (r < 0)
            return r;

        wfs_truncate(existing);
        inode_free(existing);
    }

    r = dir_add(to_parent, to_name, ino);
    if (r < 0)
        return r;

    r = dir_remove(from_parent, from_name);
    if (r < 0) {
        dir_remove(to_parent, to_name);         /* put it back as it was */
        return r;
    }

    /* ".." is a real entry here, so a directory that changed parents has to be
     * told: it is what `cd ..` reads. */
    if (in.type == WFS_TYPE_DIR && from_parent != to_parent) {
        dir_remove(ino, "..");
        dir_add(ino, "..", to_parent);
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Mount and statistics
 * ------------------------------------------------------------------ */

void wfs_statfs(wdiskinfo_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!mounted)
        return;

    out->block_size   = sb.block_size;
    out->total_blocks = sb.total_blocks;
    out->free_blocks  = sb.free_blocks;
    out->total_inodes = sb.total_inodes;
    out->free_inodes  = sb.free_inodes;
    out->total_bytes  = sb.total_blocks * sb.block_size;
    out->free_bytes   = sb.free_blocks * sb.block_size;
    out->used_bytes   = out->total_bytes - out->free_bytes;
}

bool wfs_mounted(void) { return mounted; }

bool wfs_on_ramdisk(void) { return source == WFS_SOURCE_RAMDISK; }

wfs_source_t wfs_source(void) { return source; }

/* Read the first block of the volume and keep it if it is a WFS superblock.
 *
 * A volume from before inodes had a modification time is recognised and
 * refused: its block pointers are one field earlier than this driver looks for
 * them, so mounting it would find file contents wherever a block number
 * happened to land.  Saying which build made it, and what to do about it, is
 * worth the four lines -- the alternative is an unmountable disk with no
 * explanation. */
static bool superblock_ok(void)
{
    uint8_t buf[WFS_BLOCK_SIZE];

    if (!block_read(0, buf))
        return false;

    memcpy(&sb, buf, sizeof(sb));

    if (sb.magic == WFS_MAGIC_V1) {
        kputs("wfs    : this volume predates file times (WFS1); "
              "rebuild the image with make\n");
        return false;
    }

    return sb.magic == WFS_MAGIC;
}

/* Look for the volume on one device: at the very start of it, or inside one of
 * the four partitions an MBR describes.
 *
 * A USB stick has to be partitioned, because the firmware needs a FAT
 * partition it can read the loader out of, and WFS has to live somewhere else
 * on the same device.  A plain disk image has no partition table at all and the
 * volume starts at sector zero, which is the first thing tried. */
static bool find_volume(wfs_source_t device)
{
    source        = device;
    partition_lba = 0;

    if (superblock_ok())
        return true;

    uint8_t mbr[512];
    if (!sectors_read(0, 1, mbr))
        return false;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return false;

    for (int i = 0; i < 4; i++) {
        const uint8_t *entry = mbr + 0x1BE + i * 16;

        uint32_t start = (uint32_t)entry[8]        | ((uint32_t)entry[9] << 8) |
                        ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        uint32_t count = (uint32_t)entry[12]        | ((uint32_t)entry[13] << 8) |
                        ((uint32_t)entry[14] << 16) | ((uint32_t)entry[15] << 24);

        if (!start || !count)
            continue;

        partition_lba = start;
        if (superblock_ok())
            return true;
    }

    partition_lba = 0;
    source = WFS_SOURCE_NONE;
    return false;
}

/* Everything after the superblock has been found: check it is a volume this
 * driver can work with, and read the block bitmap into memory.  False leaves
 * the caller free to try another device. */
static bool adopt_volume(void)
{
    if (sb.block_size != WFS_BLOCK_SIZE) {
        kprintf("wfs    : unsupported block size %u\n", sb.block_size);
        return false;
    }

    /* The whole bitmap is cached, because every allocation consults it.  That
     * is a megabyte of heap for every 8 GiB of disk, so a big enough volume
     * does not fit in the arena -- and the answer is to say which knob to turn
     * and carry on with a smaller volume, not to leave the machine with no
     * filesystem at all. */
    uint32_t bitmap_bytes = sb.bitmap_blocks * WFS_BLOCK_SIZE;
    block_bitmap = kmalloc(bitmap_bytes);
    if (!block_bitmap) {
        kprintf("wfs    : %s volume needs %s of heap for its block bitmap; "
                "raise KHEAP_MB\n",
                fmt_bytes((uint64_t)sb.total_blocks * WFS_BLOCK_SIZE),
                fmt_bytes(bitmap_bytes));
        return false;
    }

    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        if (!block_read(sb.bitmap_start + i,
                        block_bitmap + i * WFS_BLOCK_SIZE)) {
            kprintf("wfs    : failed to read the block bitmap\n");
            kfree(block_bitmap);
            block_bitmap = NULL;
            return false;
        }
    }

    mounted = true;
    return true;
}

bool wfs_mount(void)
{
    /* Real devices first, in the order they are likely to be the system: an
     * ATA disk, then a USB one, both of which keep what is written to them.
     * The module the bootloader loaded is the fallback, and is only a copy in
     * memory -- writes to it are lost at the next boot.
     *
     * Each is tried all the way to a working mount, so a device that holds a
     * volume this driver cannot take on -- too large for the heap, an
     * unreadable bitmap -- moves on to the next rather than leaving the machine
     * with nothing mounted. */
    if (ata_present() && find_volume(WFS_SOURCE_ATA) && adopt_volume())
        return true;

    if (usbdisk_present() && find_volume(WFS_SOURCE_USB) && adopt_volume())
        return true;

    if (ramdisk_present() && find_volume(WFS_SOURCE_RAMDISK) && adopt_volume()) {
        kputs("wfs    : no volume on a disk; using the boot module\n");
        return true;
    }

    if (ata_present() || usbdisk_present() || ramdisk_present())
        kputs("wfs    : no filesystem could be mounted from any device\n");
    else
        kputs("wfs    : no disk and no boot module; nothing to mount\n");

    source = WFS_SOURCE_NONE;
    return false;
}
