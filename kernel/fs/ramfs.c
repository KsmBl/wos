/* The /ramdisk filesystem. See ramfs.h.
 *
 * A tree of nodes in the kernel heap, with file contents in whole pages taken
 * from the frame allocator.  Nothing is preallocated and nothing is kept: a
 * page is taken when a write needs one and given back the moment it is not
 * holding anything, so an empty /ramdisk costs a few hundred bytes.
 *
 * Pages come from low memory because that is the part the kernel can address
 * directly -- the identity map makes a physical address usable as a pointer,
 * which is what lets a file be read and written without mapping anything.
 */

#include "ramfs.h"
#include "rtc.h"
#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "string.h"
#include "kprintf.h"

#define BLOCK_SIZE   PAGE_SIZE
#define MAX_NODES    1024
#define ROOT_INO     1

typedef struct ramfs_node {
    uint32_t ino;
    uint16_t type;                       /* WFS_TYPE_FILE or WFS_TYPE_DIR */
    uint32_t size;
    uint32_t mtime;                      /* seconds since 1970, from the RTC */

    char     name[W_NAME_MAX + 1];
    struct ramfs_node *parent;
    struct ramfs_node *next;             /* next entry in the parent       */
    struct ramfs_node *children;

    uint64_t *blocks;                    /* physical page addresses        */
    uint32_t  block_count;               /* how many the array holds       */
} ramfs_node_t;

static ramfs_node_t *nodes[MAX_NODES];   /* indexed by inode number */
static uint32_t      next_ino = ROOT_INO;
static uint64_t      pages_held;

uint64_t ramfs_used_bytes(void) { return pages_held * BLOCK_SIZE; }

/* ------------------------------------------------------------------ *
 *  Nodes
 * ------------------------------------------------------------------ */

static ramfs_node_t *node_of(uint32_t ino)
{
    if (ino == 0 || ino >= MAX_NODES)
        return NULL;
    return nodes[ino];
}

static ramfs_node_t *node_new(const char *name, uint16_t type,
                              ramfs_node_t *parent)
{
    if (next_ino >= MAX_NODES)
        return NULL;

    ramfs_node_t *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;

    n->ino    = next_ino++;
    n->type   = type;
    n->parent = parent;
    n->mtime  = rtc_epoch();
    strlcpy(n->name, name, sizeof(n->name));

    if (parent) {
        n->next = parent->children;
        parent->children = n;
    }

    nodes[n->ino] = n;
    return n;
}

static ramfs_node_t *child_named(const ramfs_node_t *dir, const char *name,
                                 uint32_t len)
{
    for (ramfs_node_t *c = dir->children; c; c = c->next)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            return c;
    return NULL;
}

/* ------------------------------------------------------------------ *
 *  File contents
 * ------------------------------------------------------------------ */

static void *block_ptr(uint64_t phys)
{
    return (void *)(uintptr_t)phys;
}

/* Make room for `count` block pointers.  The array itself is small -- one
 * pointer per page of file -- and lives in the heap. */
static bool blocks_reserve(ramfs_node_t *n, uint32_t count)
{
    if (count <= n->block_count)
        return true;

    uint32_t grown = n->block_count ? n->block_count * 2 : 4;
    if (grown < count)
        grown = count;

    uint64_t *fresh = kzalloc(grown * sizeof(uint64_t));
    if (!fresh)
        return false;

    if (n->blocks) {
        memcpy(fresh, n->blocks, n->block_count * sizeof(uint64_t));
        kfree(n->blocks);
    }

    n->blocks = fresh;
    n->block_count = grown;
    return true;
}

/* The page holding `index`, allocated if it is not there yet. */
static void *block_get(ramfs_node_t *n, uint32_t index, bool create)
{
    if (index >= n->block_count) {
        if (!create || !blocks_reserve(n, index + 1))
            return NULL;
    }

    if (!n->blocks[index]) {
        if (!create)
            return NULL;

        uint64_t phys = pmm_alloc_frame_low();
        if (!phys)
            return NULL;

        memset(block_ptr(phys), 0, BLOCK_SIZE);
        n->blocks[index] = phys;
        pages_held++;
    }

    return block_ptr(n->blocks[index]);
}

/* Give back every page from `from` onwards. */
static void blocks_release(ramfs_node_t *n, uint32_t from)
{
    for (uint32_t i = from; i < n->block_count; i++)
        if (n->blocks[i]) {
            pmm_free_frame(n->blocks[i]);
            n->blocks[i] = 0;
            pages_held--;
        }

    if (from == 0 && n->blocks) {
        kfree(n->blocks);
        n->blocks = NULL;
        n->block_count = 0;
    }
}

/* ------------------------------------------------------------------ *
 *  Paths
 * ------------------------------------------------------------------ */

bool ramfs_owns(const char *abs)
{
    size_t len = strlen(RAMFS_MOUNT);

    if (memcmp(abs, RAMFS_MOUNT, len) != 0)
        return false;

    return abs[len] == '\0' || abs[len] == '/';
}

/* Walk to the node at `path`.  With `parent_out` set, stops one level short and
 * reports the final component instead, which is what creating needs. */
static ramfs_node_t *walk(const char *path, ramfs_node_t **parent_out,
                          const char **leaf_out, uint32_t *leaf_len)
{
    if (!ramfs_owns(path))
        return NULL;

    ramfs_node_t *dir = node_of(ROOT_INO);
    const char *p = path + strlen(RAMFS_MOUNT);

    if (parent_out) {
        *parent_out = NULL;
        *leaf_out = NULL;
        *leaf_len = 0;
    }

    for (;;) {
        while (*p == '/')
            p++;

        if (!*p) {
            /* The path ended on a directory: with a parent wanted, there is no
             * final component to report, which callers read as "the mount
             * point itself". */
            return parent_out ? NULL : dir;
        }

        const char *name = p;
        while (*p && *p != '/')
            p++;

        uint32_t len = (uint32_t)(p - name);
        if (len > W_NAME_MAX)
            return NULL;

        /* Skip any trailing slashes to see whether this is the last one. */
        const char *after = p;
        while (*after == '/')
            after++;

        if (!*after && parent_out) {
            *parent_out = dir;
            *leaf_out   = name;
            *leaf_len   = len;
            return child_named(dir, name, len);   /* may be NULL */
        }

        ramfs_node_t *next = child_named(dir, name, len);
        if (!next)
            return NULL;

        if (!*after)
            return next;

        if (next->type != WFS_TYPE_DIR)
            return NULL;

        dir = next;
    }
}

/* ------------------------------------------------------------------ *
 *  The operations
 * ------------------------------------------------------------------ */

void ramfs_init(void)
{
    if (!node_of(ROOT_INO))
        node_new("", WFS_TYPE_DIR, NULL);
}

int ramfs_lookup(const char *path, uint32_t *ino_out)
{
    ramfs_node_t *n = walk(path, NULL, NULL, NULL);
    if (!n)
        return -W_ENOENT;

    if (ino_out)
        *ino_out = n->ino;
    return 0;
}

int ramfs_read_inode(uint32_t ino, struct wfs_inode *out)
{
    ramfs_node_t *n = node_of(ino);
    if (!n)
        return -W_ENOENT;

    memset(out, 0, sizeof(*out));
    out->type   = n->type;
    out->links  = 1;
    out->size   = n->size;
    out->mtime  = n->mtime;

    /* In blocks of the size the rest of the system counts in, so that `df` and
     * `stat` do not have to know this filesystem uses pages. */
    out->blocks = (n->size + WFS_BLOCK_SIZE - 1) / WFS_BLOCK_SIZE;
    return 0;
}

int ramfs_read(uint32_t ino, uint32_t offset, void *buf, uint32_t len)
{
    ramfs_node_t *n = node_of(ino);
    if (!n)
        return -W_ENOENT;
    if (n->type != WFS_TYPE_FILE)
        return -W_EISDIR;

    if (offset >= n->size)
        return 0;

    if (len > n->size - offset)
        len = n->size - offset;

    uint8_t *out = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t index  = (offset + done) / BLOCK_SIZE;
        uint32_t within = (offset + done) % BLOCK_SIZE;
        uint32_t chunk  = BLOCK_SIZE - within;

        if (chunk > len - done)
            chunk = len - done;

        const uint8_t *page = block_get(n, index, false);

        /* A hole reads as zeroes, which is what a file written past its end
         * without filling the gap contains. */
        if (page)
            memcpy(out + done, page + within, chunk);
        else
            memset(out + done, 0, chunk);

        done += chunk;
    }

    return (int)done;
}

int ramfs_write(uint32_t ino, uint32_t offset, const void *buf, uint32_t len)
{
    ramfs_node_t *n = node_of(ino);
    if (!n)
        return -W_ENOENT;
    if (n->type != WFS_TYPE_FILE)
        return -W_EISDIR;

    const uint8_t *in = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t index  = (offset + done) / BLOCK_SIZE;
        uint32_t within = (offset + done) % BLOCK_SIZE;
        uint32_t chunk  = BLOCK_SIZE - within;

        if (chunk > len - done)
            chunk = len - done;

        uint8_t *page = block_get(n, index, true);
        if (!page)
            break;                      /* out of memory: a short write */

        memcpy(page + within, in + done, chunk);
        done += chunk;
    }

    if (offset + done > n->size)
        n->size = offset + done;

    if (done == 0 && len > 0)
        return -W_ENOSPC;

    n->mtime = rtc_epoch();
    return (int)done;
}

int ramfs_create(const char *path, uint16_t type, uint32_t *ino_out)
{
    ramfs_node_t *parent;
    const char *leaf;
    uint32_t leaf_len;

    ramfs_node_t *existing = walk(path, &parent, &leaf, &leaf_len);

    if (!parent)
        return -W_ENOENT;
    if (existing)
        return -W_EEXIST;
    if (parent->type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    char name[W_NAME_MAX + 1];
    memcpy(name, leaf, leaf_len);
    name[leaf_len] = '\0';

    ramfs_node_t *n = node_new(name, type, parent);
    if (!n)
        return -W_ENOSPC;

    /* A directory here is a list of children rather than a file of entries, so
     * nothing writes to the parent and its time has to be set by hand -- on the
     * disk the same thing happens by itself, because adding a name there *is* a
     * write. */
    parent->mtime = n->mtime;

    if (ino_out)
        *ino_out = n->ino;
    return 0;
}

/* Take a node out of its parent's list of children, without destroying it --
 * which is half of unlinking and half of renaming. */
static void detach(ramfs_node_t *parent, ramfs_node_t *n)
{
    ramfs_node_t **link = &parent->children;

    while (*link && *link != n)
        link = &(*link)->next;

    if (*link)
        *link = n->next;

    n->next = NULL;
}

static void destroy(ramfs_node_t *n)
{
    blocks_release(n, 0);
    nodes[n->ino] = NULL;
    kfree(n);
}

int ramfs_unlink(const char *path)
{
    ramfs_node_t *parent;
    const char *leaf;
    uint32_t leaf_len;

    ramfs_node_t *n = walk(path, &parent, &leaf, &leaf_len);

    if (!n || !parent)
        return -W_ENOENT;
    if (n->type == WFS_TYPE_DIR && n->children)
        return -W_ENOTEMPTY;

    detach(parent, n);
    destroy(n);
    parent->mtime = rtc_epoch();
    return 0;
}

/* The same move wfs_rename() makes, on a tree of pointers rather than on
 * directory blocks: nothing is copied, an entry changes its name and its
 * parent.  See wfs_rename() for why replacing a file this way is the safe way
 * to write one. */
int ramfs_rename(const char *from, const char *to)
{
    ramfs_node_t *from_parent, *to_parent;
    const char *from_leaf, *to_leaf;
    uint32_t from_len, to_len;

    ramfs_node_t *n        = walk(from, &from_parent, &from_leaf, &from_len);
    ramfs_node_t *existing = walk(to, &to_parent, &to_leaf, &to_len);

    if (!n || !from_parent)
        return -W_ENOENT;
    if (!to_parent)
        return -W_ENOENT;
    if (to_parent->type != WFS_TYPE_DIR)
        return -W_ENOTDIR;
    if (to_len > W_NAME_MAX)
        return -W_EINVAL;
    if (existing == n)
        return 0;

    /* Not inside itself: a directory that became its own descendant would be
     * a loop nothing walking the tree could get out of. */
    for (ramfs_node_t *at = to_parent; at; at = at->parent)
        if (at == n)
            return -W_EINVAL;

    if (existing) {
        if (existing->type == WFS_TYPE_DIR)
            return -W_EISDIR;

        detach(to_parent, existing);
        destroy(existing);
    }

    detach(from_parent, n);

    memcpy(n->name, to_leaf, to_len);
    n->name[to_len] = '\0';

    n->parent          = to_parent;
    n->next            = to_parent->children;
    to_parent->children = n;

    /* Both directories changed -- one lost a name and one gained it -- and the
     * file itself did not: its contents are exactly what they were. */
    from_parent->mtime = to_parent->mtime = rtc_epoch();

    return 0;
}

int ramfs_truncate(uint32_t ino)
{
    ramfs_node_t *n = node_of(ino);
    if (!n)
        return -W_ENOENT;

    blocks_release(n, 0);
    n->size  = 0;
    n->mtime = rtc_epoch();
    return 0;
}

int ramfs_utime(uint32_t ino, uint32_t mtime)
{
    ramfs_node_t *n = node_of(ino);
    if (!n)
        return -W_ENOENT;

    n->mtime = mtime;
    return 0;
}

/* Read one directory entry, and move the cursor past it.
 *
 * The cursor is an ordinal here rather than the byte offset wfs uses -- there
 * are no bytes to offset into, only a list -- and the walk is still from the
 * head each time.  That is a pointer chase through memory the machine already
 * has, which is a different thing entirely from wfs rereading a device, and
 * not worth holding a node pointer across calls for: the list can be added to
 * or unlinked between two of them, and a stale pointer would be far worse than
 * a short walk. */
int ramfs_readdir(uint32_t ino, uint32_t *cursor, wdirent_t *out)
{
    ramfs_node_t *n = node_of(ino);
    if (!n)
        return -W_ENOENT;
    if (n->type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    /* The list is newest first; walking it backwards would need a second pass
     * per entry, and nothing promises an order anyway. */
    uint32_t i = 0;
    for (ramfs_node_t *c = n->children; c; c = c->next, i++)
        if (i == *cursor) {
            out->ino  = c->ino;
            out->type = c->type == WFS_TYPE_DIR ? W_FT_DIR : W_FT_FILE;
            strlcpy(out->name, c->name, sizeof(out->name));
            (*cursor)++;
            return 1;
        }

    return 0;
}

void ramfs_statfs(wdiskinfo_t *out)
{
    uint64_t used = ramfs_used_bytes();

    /* There is no size to report, so the honest answer is what is in it and
     * what it could still take: the free memory it can reach, which is the
     * identity-mapped part the pages come from. */
    uint64_t free_low = pmm_free_bytes();
    if (free_low > LOW_MEMORY_LIMIT)
        free_low = LOW_MEMORY_LIMIT;

    out->block_size   = BLOCK_SIZE;
    out->total_bytes  = used + free_low;
    out->free_bytes   = free_low;
    out->used_bytes   = used;
    out->total_blocks = (uint32_t)((used + free_low) / BLOCK_SIZE);
    out->free_blocks  = (uint32_t)(free_low / BLOCK_SIZE);

    out->total_inodes = MAX_NODES - 1;
    out->free_inodes  = MAX_NODES - next_ino;
}
