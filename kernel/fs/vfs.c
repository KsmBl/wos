/* File descriptors, path resolution and console routing. */

#include "vfs.h"
#include "proc.h"
#include "wfs_kernel.h"
#include "keyboard.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

void vfs_init_fds(struct process *p)
{
    memset(p->fds, 0, sizeof(p->fds));

    /* Every process starts with stdin, stdout and stderr on the console. */
    for (int i = 0; i < 3; i++)
        p->fds[i].type = FD_CONSOLE;
}

void vfs_close_all(struct process *p)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        p->fds[i].type = FD_NONE;
}

static file_t *fd_get(struct process *p, int fd)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || p->fds[fd].type == FD_NONE)
        return NULL;
    return &p->fds[fd];
}

static int fd_alloc(struct process *p)
{
    /* 0-2 are the console, so user files start at 3. */
    for (int i = 3; i < MAX_OPEN_FILES; i++)
        if (p->fds[i].type == FD_NONE)
            return i;
    return -W_EMFILE;
}

/* ------------------------------------------------------------------ *
 *  Path resolution
 * ------------------------------------------------------------------ */

/* Build a normalised absolute path: join with the working directory when the
 * input is relative, then collapse ".", ".." and repeated slashes.
 *
 * Doing this in the kernel means WFS never has to think about relative paths
 * and "/a/b/../c" resolves without touching the disk for "b". */
int vfs_resolve(struct process *p, const char *path, char *out, size_t out_size)
{
    char joined[W_PATH_MAX * 2 + 2];

    if (!path || !*path)
        return -W_EINVAL;

    if (path[0] == '/') {
        if (strlen(path) > W_PATH_MAX)
            return -W_ENAMETOOLONG;
        strlcpy(joined, path, sizeof(joined));
    } else {
        size_t cwd_len = strlen(p->cwd);
        if (cwd_len + 1 + strlen(path) > sizeof(joined) - 1)
            return -W_ENAMETOOLONG;

        strlcpy(joined, p->cwd, sizeof(joined));
        if (cwd_len == 0 || joined[cwd_len - 1] != '/')
            joined[cwd_len++] = '/';
        strlcpy(joined + cwd_len, path, sizeof(joined) - cwd_len);
    }

    /* Walk the components.  For each one accepted we remember how long the
     * output was *before* it was appended, so ".." pops it by restoring that
     * length exactly -- no rescanning and no off-by-one to get wrong. */
    size_t saved[64];
    int    depth = 0;

    out[0] = '/';
    size_t len = 1;

    const char *q = joined;
    while (*q) {
        while (*q == '/')
            q++;
        if (!*q)
            break;

        const char *begin = q;
        while (*q && *q != '/')
            q++;
        size_t comp = (size_t)(q - begin);

        if (comp == 1 && begin[0] == '.')
            continue;

        if (comp == 2 && begin[0] == '.' && begin[1] == '.') {
            if (depth > 0)
                len = saved[--depth];
            continue;               /* ".." at the root stays at the root */
        }

        if (comp > W_NAME_MAX)
            return -W_ENAMETOOLONG;
        if (depth >= (int)(sizeof(saved) / sizeof(saved[0])))
            return -W_ENAMETOOLONG;
        /* separator (unless we are at the root) + the name + the NUL */
        if (len + (len > 1 ? 1 : 0) + comp + 1 > out_size)
            return -W_ENAMETOOLONG;

        saved[depth++] = len;

        if (len > 1)
            out[len++] = '/';
        memcpy(out + len, begin, comp);
        len += comp;
    }

    out[len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Console
 * ------------------------------------------------------------------ */

static int console_read(void *buf, uint32_t len)
{
    return (int)keyboard_read(buf, len);
}

static int console_write(const void *buf, uint32_t len)
{
    const char *s = buf;
    for (uint32_t i = 0; i < len; i++)
        kputc(s[i]);
    return (int)len;
}

/* ------------------------------------------------------------------ *
 *  Descriptor operations
 * ------------------------------------------------------------------ */

int vfs_open(struct process *p, const char *path, uint32_t flags)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);

    if (r == -W_ENOENT && (flags & W_O_CREAT)) {
        r = wfs_create(abs, WFS_TYPE_FILE, &ino);
        if (r < 0)
            return r;
    } else if (r < 0) {
        return r;
    }

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    if (in.type == WFS_TYPE_DIR && (flags & W_O_ACCMODE) != W_O_RDONLY)
        return -W_EISDIR;

    int fd = fd_alloc(p);
    if (fd < 0)
        return fd;

    if ((flags & W_O_TRUNC) && in.type == WFS_TYPE_FILE) {
        r = wfs_truncate(ino);
        if (r < 0)
            return r;
        in.size = 0;
    }

    p->fds[fd].type   = (in.type == WFS_TYPE_DIR) ? FD_DIR : FD_FILE;
    p->fds[fd].ino    = ino;
    p->fds[fd].flags  = flags;
    p->fds[fd].offset = (flags & W_O_APPEND) ? in.size : 0;

    return fd;
}

int vfs_close(struct process *p, int fd)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;

    f->type = FD_NONE;
    return 0;
}

int vfs_read(struct process *p, int fd, void *buf, uint32_t len)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;
    if (len == 0)
        return 0;

    if (f->type == FD_CONSOLE)
        return console_read(buf, len);
    if (f->type == FD_DIR)
        return -W_EISDIR;
    if ((f->flags & W_O_ACCMODE) == W_O_WRONLY)
        return -W_EACCES;

    int n = wfs_read(f->ino, f->offset, buf, len);
    if (n > 0)
        f->offset += (uint32_t)n;
    return n;
}

int vfs_write(struct process *p, int fd, const void *buf, uint32_t len)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;
    if (len == 0)
        return 0;

    if (f->type == FD_CONSOLE)
        return console_write(buf, len);
    if (f->type == FD_DIR)
        return -W_EISDIR;
    if ((f->flags & W_O_ACCMODE) == W_O_RDONLY)
        return -W_EACCES;

    /* O_APPEND has to re-read the size each time: another descriptor may
     * have extended the file since this one was opened. */
    if (f->flags & W_O_APPEND) {
        struct wfs_inode in;
        if (wfs_read_inode(f->ino, &in) == 0)
            f->offset = in.size;
    }

    int n = wfs_write(f->ino, f->offset, buf, len);
    if (n > 0)
        f->offset += (uint32_t)n;
    return n;
}

int vfs_lseek(struct process *p, int fd, int32_t offset, int whence)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;
    if (f->type == FD_CONSOLE)
        return -W_ESPIPE;

    struct wfs_inode in;
    int r = wfs_read_inode(f->ino, &in);
    if (r < 0)
        return r;

    int32_t base;
    switch (whence) {
    case W_SEEK_SET: base = 0;                  break;
    case W_SEEK_CUR: base = (int32_t)f->offset; break;
    case W_SEEK_END: base = (int32_t)in.size;   break;
    default:         return -W_EINVAL;
    }

    int32_t target = base + offset;
    if (target < 0)
        return -W_EINVAL;

    f->offset = (uint32_t)target;
    return target;
}

int vfs_stat(struct process *p, const char *path, wstat_t *out)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;

    out->ino    = ino;
    out->size   = in.size;
    out->blocks = in.blocks;
    out->type   = in.type;
    return 0;
}

int vfs_unlink(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;
    if (in.type == WFS_TYPE_DIR)
        return -W_EISDIR;

    return wfs_unlink(abs);
}

int vfs_mkdir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    return wfs_create(abs, WFS_TYPE_DIR, NULL);
}

int vfs_rmdir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    return wfs_unlink(abs);
}

int vfs_opendir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    int fd = fd_alloc(p);
    if (fd < 0)
        return fd;

    p->fds[fd].type   = FD_DIR;
    p->fds[fd].ino    = ino;
    p->fds[fd].offset = 0;         /* entry index, not a byte offset */
    p->fds[fd].flags  = W_O_RDONLY;

    return fd;
}

int vfs_readdir(struct process *p, int fd, wdirent_t *out)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;
    if (f->type != FD_DIR)
        return -W_ENOTDIR;

    int r = wfs_readdir(f->ino, f->offset, out);
    if (r == 1)
        f->offset++;
    return r;
}

int vfs_chdir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    strlcpy(p->cwd, abs, sizeof(p->cwd));
    return 0;
}

int vfs_getcwd(struct process *p, char *buf, uint32_t size)
{
    uint32_t len = (uint32_t)strlen(p->cwd);

    if (size < len + 1)
        return -W_ERANGE;

    memcpy(buf, p->cwd, len + 1);
    return (int)len;
}

int vfs_read_file(struct process *p, const char *path, void **data_out,
                  uint64_t *size_out)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = wfs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = wfs_read_inode(ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_FILE)
        return -W_EISDIR;
    if (in.size == 0)
        return -W_ENOEXEC;

    void *buf = kmalloc(in.size);
    if (!buf)
        return -W_ENOMEM;

    r = wfs_read(ino, 0, buf, in.size);
    if (r != (int)in.size) {
        kfree(buf);
        return (r < 0) ? r : -W_EIO;
    }

    *data_out = buf;
    *size_out = in.size;
    return 0;
}
