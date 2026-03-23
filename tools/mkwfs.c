/* mkwfs -- build a WFS disk image from a directory tree.
 *
 * Runs on the host, not in WOS.  It shares include/wfs.h with the kernel, so
 * the layout it writes and the layout the kernel expects cannot drift apart.
 *
 *   usage: mkwfs <image> <size-in-MiB> <staging-directory>
 *
 * The whole image is built in memory and written out once, which keeps the
 * block helpers trivial compared to seeking around a file descriptor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "wfs.h"

static uint8_t              *img;
static uint32_t              image_blocks;
static struct wfs_superblock *sb;
static uint8_t              *bitmap;
static struct wfs_inode     *inodes;

static void die(const char *msg)
{
    fprintf(stderr, "mkwfs: %s\n", msg);
    exit(1);
}

static uint8_t *block_ptr(uint32_t block)
{
    if (block >= image_blocks)
        die("block out of range");
    return img + (size_t)block * WFS_BLOCK_SIZE;
}

static void bitmap_mark(uint32_t block)
{
    bitmap[block / 8] |= (uint8_t)(1u << (block % 8));
}

static int bitmap_test(uint32_t block)
{
    return (bitmap[block / 8] >> (block % 8)) & 1;
}

static uint32_t block_alloc(void)
{
    for (uint32_t b = sb->data_start; b < sb->total_blocks; b++) {
        if (!bitmap_test(b)) {
            bitmap_mark(b);
            sb->free_blocks--;
            memset(block_ptr(b), 0, WFS_BLOCK_SIZE);
            return b;
        }
    }
    die("out of disk space -- make the image larger");
    return 0;
}

static uint32_t inode_alloc(uint16_t type)
{
    for (uint32_t i = WFS_ROOT_INO; i < sb->total_inodes; i++) {
        if (inodes[i].type == WFS_TYPE_FREE) {
            memset(&inodes[i], 0, sizeof(inodes[i]));
            inodes[i].type  = type;
            inodes[i].links = 1;
            sb->free_inodes--;
            return i;
        }
    }
    die("out of inodes");
    return 0;
}

/* File-relative block index to disk block, allocating as needed. */
static uint32_t bmap(uint32_t ino, uint32_t index)
{
    struct wfs_inode *in = &inodes[ino];

    if (index < WFS_DIRECT) {
        if (!in->direct[index]) {
            in->direct[index] = block_alloc();
            in->blocks++;
        }
        return in->direct[index];
    }

    index -= WFS_DIRECT;
    if (index >= WFS_PTRS_PER_BLOCK)
        die("file exceeds the maximum WFS file size");

    if (!in->indirect) {
        in->indirect = block_alloc();
        in->blocks++;
    }

    uint32_t *table = (uint32_t *)block_ptr(in->indirect);
    if (!table[index]) {
        table[index] = block_alloc();
        in->blocks++;
    }
    return table[index];
}

static void file_write(uint32_t ino, uint32_t offset, const void *buf,
                       uint32_t len)
{
    const uint8_t *src = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t index = (offset + done) / WFS_BLOCK_SIZE;
        uint32_t inpos = (offset + done) % WFS_BLOCK_SIZE;
        uint32_t chunk = WFS_BLOCK_SIZE - inpos;
        if (chunk > len - done)
            chunk = len - done;

        memcpy(block_ptr(bmap(ino, index)) + inpos, src + done, chunk);
        done += chunk;
    }

    if (offset + len > inodes[ino].size)
        inodes[ino].size = offset + len;
}

static void dir_add(uint32_t dir_ino, const char *name, uint32_t ino)
{
    struct wfs_dirent ent;

    if (strlen(name) > WFS_NAME_MAX) {
        fprintf(stderr, "mkwfs: name too long (max %u): %s\n",
                WFS_NAME_MAX, name);
        exit(1);
    }

    memset(&ent, 0, sizeof(ent));
    ent.ino = ino;
    snprintf(ent.name, sizeof(ent.name), "%s", name);

    file_write(dir_ino, inodes[dir_ino].size, &ent, sizeof(ent));
}

static uint32_t make_dir(uint32_t parent, const char *name)
{
    uint32_t ino = inode_alloc(WFS_TYPE_DIR);

    /* "." and ".." are real entries so the kernel's path resolution needs no
     * special cases for them. */
    dir_add(ino, ".", ino);
    dir_add(ino, "..", parent);

    if (name)
        dir_add(parent, name, ino);

    return ino;
}

static void add_host_file(uint32_t parent, const char *name, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mkwfs: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0)
        die("cannot determine file size");
    if ((uint32_t)size > WFS_MAX_FILE_SIZE) {
        fprintf(stderr, "mkwfs: %s is %ld bytes, over the %u byte limit\n",
                path, size, WFS_MAX_FILE_SIZE);
        exit(1);
    }

    uint8_t *data = malloc((size_t)size ? (size_t)size : 1);
    if (!data)
        die("out of memory");
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size)
        die("short read");
    fclose(f);

    uint32_t ino = inode_alloc(WFS_TYPE_FILE);
    if (size > 0)
        file_write(ino, 0, data, (uint32_t)size);
    inodes[ino].size = (uint32_t)size;
    dir_add(parent, name, ino);

    free(data);
    printf("  %8ld  %s\n", size, path);
}

/* Recursively copy a host directory into the image. */
static void add_host_dir(uint32_t parent, const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "mkwfs: cannot open directory %s: %s\n",
                path, strerror(errno));
        exit(1);
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);

        struct stat st;
        if (stat(child, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            uint32_t ino = make_dir(parent, e->d_name);
            add_host_dir(ino, child);
        } else if (S_ISREG(st.st_mode)) {
            add_host_file(parent, e->d_name, child);
        }
    }

    closedir(d);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <image> <size-in-MiB> <staging-directory>\n",
                argv[0]);
        return 1;
    }

    const char *image_path = argv[1];
    uint32_t    size_mb    = (uint32_t)strtoul(argv[2], NULL, 10);
    const char *staging    = argv[3];

    if (size_mb < 1)
        die("image must be at least 1 MiB");

    image_blocks = size_mb * 1024u * 1024u / WFS_BLOCK_SIZE;

    img = calloc(image_blocks, WFS_BLOCK_SIZE);
    if (!img)
        die("out of memory allocating the image");

    /* Lay out the volume: superblock, bitmap, inode table, then data.
     * One inode per 32 blocks is generous for a system that stores a few
     * programs and their source. */
    uint32_t bitmap_blocks = (image_blocks + WFS_BLOCK_SIZE * 8 - 1)
                             / (WFS_BLOCK_SIZE * 8);
    uint32_t total_inodes  = image_blocks / 32;
    if (total_inodes < 64)
        total_inodes = 64;
    total_inodes = (total_inodes + WFS_INODES_PER_BLOCK - 1)
                   / WFS_INODES_PER_BLOCK * WFS_INODES_PER_BLOCK;
    uint32_t inode_blocks = total_inodes / WFS_INODES_PER_BLOCK;

    sb = (struct wfs_superblock *)img;
    sb->magic         = WFS_MAGIC;
    sb->block_size    = WFS_BLOCK_SIZE;
    sb->total_blocks  = image_blocks;
    sb->total_inodes  = total_inodes;
    sb->free_inodes   = total_inodes - WFS_ROOT_INO;  /* inode 0 is never used */
    sb->bitmap_start  = 1;
    sb->bitmap_blocks = bitmap_blocks;
    sb->inode_start   = 1 + bitmap_blocks;
    sb->inode_blocks  = inode_blocks;
    sb->data_start    = sb->inode_start + inode_blocks;
    sb->root_inode    = WFS_ROOT_INO;
    sb->free_blocks   = image_blocks - sb->data_start;

    bitmap = block_ptr(sb->bitmap_start);
    inodes = (struct wfs_inode *)block_ptr(sb->inode_start);

    /* Metadata blocks are permanently allocated. */
    for (uint32_t b = 0; b < sb->data_start; b++)
        bitmap_mark(b);

    /* The root directory points at itself for both "." and "..". */
    uint32_t root = inode_alloc(WFS_TYPE_DIR);
    if (root != WFS_ROOT_INO)
        die("root did not land in inode 1");
    dir_add(root, ".", root);
    dir_add(root, "..", root);

    printf("mkwfs: building %s (%u MiB)\n", image_path, size_mb);
    printf("  %u blocks of %u bytes, %u inodes, data starts at block %u\n",
           sb->total_blocks, sb->block_size, sb->total_inodes, sb->data_start);
    printf("  contents:\n");

    add_host_dir(root, staging);

    FILE *out = fopen(image_path, "wb");
    if (!out) {
        fprintf(stderr, "mkwfs: cannot create %s: %s\n",
                image_path, strerror(errno));
        return 1;
    }
    if (fwrite(img, WFS_BLOCK_SIZE, image_blocks, out) != image_blocks)
        die("short write");
    fclose(out);

    uint32_t used = sb->total_blocks - sb->free_blocks;
    printf("  %u/%u blocks used (%u KiB), %u inodes free\n",
           used, sb->total_blocks, used * WFS_BLOCK_SIZE / 1024,
           sb->free_inodes);

    free(img);
    return 0;
}
