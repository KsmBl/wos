/* File descriptors, path resolution and console routing. */

#include "vfs.h"
#include "proc.h"
#include "pipe.h"
#include "socket.h"
#include "shm.h"
#include "sched.h"
#include "pit.h"
#include "wfs_kernel.h"
#include "ramfs.h"
#include "keyboard.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "user.h"

void vfs_init_fds(struct process *p)
{
    memset(p->fds, 0, sizeof(p->fds));

    /* Every process starts with stdin, stdout and stderr on the console. */
    for (int i = 0; i < 3; i++)
        p->fds[i].type = FD_CONSOLE;
}

void vfs_fd_retain(file_t *f)
{
    if (!f)
        return;
    if (f->type == FD_PIPE)
        pipe_ref(f->pipe, f->write_end);
    else if (f->type == FD_SOCKET)
        socket_ref(f->sock);
    else if (f->type == FD_SHM)
        shm_ref(f->shm);
}

void vfs_fd_drop(file_t *f)
{
    if (!f)
        return;
    if (f->type == FD_PIPE)
        pipe_unref(f->pipe, f->write_end);
    else if (f->type == FD_SOCKET)
        socket_unref(f->sock);
    else if (f->type == FD_SHM)
        shm_unref(f->shm);
}

/* Release one descriptor, dropping whatever reference it holds. */
static void fd_release(file_t *f)
{
    vfs_fd_drop(f);

    f->type      = FD_NONE;
    f->pipe      = NULL;
    f->sock      = NULL;
    f->shm       = NULL;
    f->write_end = false;
}

void vfs_close_all(struct process *p)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        fd_release(&p->fds[i]);
}

/* True if this process reads its standard input from the real console rather
 * than from a pipe.  Console modes (raw vs canonical) belong to the physical
 * keyboard, so a program whose stdin is a pipe must not change them. */
bool vfs_stdin_is_console(struct process *p)
{
    return p->fds[W_STDIN].type == FD_CONSOLE;
}

/* Give a child the parent's standard descriptors, so output redirected to a
 * pipe is inherited the way it is across a Unix fork+exec.  Without this a
 * program run from a shell inside vim's :term would write past the shell to
 * the real console.  A pipe end gains a reference, balanced when the child
 * closes it. */
void vfs_inherit_stdio(struct process *child, struct process *parent)
{
    for (int i = 0; i < 3; i++) {
        file_t *src = &parent->fds[i];
        child->fds[i] = *src;
        vfs_fd_retain(src);
    }
}

/* ------------------------------------------------------------------ *
 *  Which filesystem
 *
 *  There are two: the disk, and /ramdisk, which is held in memory and is gone
 *  at the next boot.  A path decides between them, and a descriptor remembers
 *  what its path decided, because the inode numbers of the two mean nothing to
 *  each other.  Everything below this point goes through these.
 * ------------------------------------------------------------------ */

static int fs_lookup(const char *abs, uint32_t *ino)
{
    return ramfs_owns(abs) ? ramfs_lookup(abs, ino) : wfs_lookup(abs, ino);
}

/* Both of them, with the space each is using.
 *
 * This is the only place that knows there are two filesystems and where each
 * one hangs, so it is the only place that can list them.  wfs_statfs() answers
 * for the disk and ramfs_statfs() for the one in memory; neither knows the
 * other exists. */
int vfs_disklist(wdisk_t *out, int max)
{
    int n = 0;

    if (n < max && wfs_mounted()) {
        const char *device;

        switch (wfs_source()) {
        case WFS_SOURCE_ATA:     device = "ATA disk";        break;
        case WFS_SOURCE_USB:     device = "USB disk";        break;
        case WFS_SOURCE_RAMDISK: device = "copy in memory";  break;
        default:                 device = "disk";            break;
        }

        memset(&out[n], 0, sizeof(out[n]));
        strlcpy(out[n].mount, "/", sizeof(out[n].mount));
        strlcpy(out[n].device, device, sizeof(out[n].device));
        out[n].persistent = wfs_on_ramdisk() ? 0 : 1;
        wfs_statfs(&out[n].usage);
        n++;
    }

    if (n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        strlcpy(out[n].mount, RAMFS_MOUNT, sizeof(out[n].mount));
        strlcpy(out[n].device, "memory", sizeof(out[n].device));
        out[n].persistent = 0;
        ramfs_statfs(&out[n].usage);
        n++;
    }

    return n;
}

static int fs_create(const char *abs, uint16_t type, uint32_t *ino)
{
    return ramfs_owns(abs) ? ramfs_create(abs, type, ino)
                           : wfs_create(abs, type, ino);
}

static int fs_unlink(const char *abs)
{
    return ramfs_owns(abs) ? ramfs_unlink(abs) : wfs_unlink(abs);
}

static int fs_read_inode(bool ram, uint32_t ino, struct wfs_inode *out)
{
    return ram ? ramfs_read_inode(ino, out) : wfs_read_inode(ino, out);
}

static int fs_read(bool ram, uint32_t ino, uint32_t off, void *buf, uint32_t len)
{
    return ram ? ramfs_read(ino, off, buf, len) : wfs_read(ino, off, buf, len);
}

static int fs_write(bool ram, uint32_t ino, uint32_t off, const void *buf,
                    uint32_t len)
{
    return ram ? ramfs_write(ino, off, buf, len)
               : wfs_write(ino, off, buf, len);
}

static int fs_truncate(bool ram, uint32_t ino)
{
    return ram ? ramfs_truncate(ino) : wfs_truncate(ino);
}

static int fs_readdir(bool ram, uint32_t ino, uint32_t index, wdirent_t *out)
{
    return ram ? ramfs_readdir(ino, index, out) : wfs_readdir(ino, index, out);
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

int vfs_fd_install(struct process *p, const file_t *f)
{
    int fd = fd_alloc(p);
    if (fd < 0)
        return fd;

    p->fds[fd] = *f;
    return fd;
}

int vfs_pipe(struct process *p, int out[2])
{
    int rfd = fd_alloc(p);
    if (rfd < 0)
        return rfd;

    /* Claim the read slot before allocating the write one, so the two cannot
     * come back equal. */
    p->fds[rfd].type = FD_CONSOLE;      /* placeholder to reserve the slot */

    int wfd = fd_alloc(p);
    if (wfd < 0) {
        p->fds[rfd].type = FD_NONE;
        return wfd;
    }

    pipe_t *pp = pipe_create();
    if (!pp) {
        p->fds[rfd].type = FD_NONE;
        return -W_ENFILE;
    }

    /* pipe_create() already counts one reader and one writer -- these two. */
    p->fds[rfd].type      = FD_PIPE;
    p->fds[rfd].pipe      = pp;
    p->fds[rfd].write_end = false;

    p->fds[wfd].type      = FD_PIPE;
    p->fds[wfd].pipe      = pp;
    p->fds[wfd].write_end = true;

    out[0] = rfd;
    out[1] = wfd;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Sockets
 *
 *  The descriptor layer's side of socket.c: turning endpoints into descriptor
 *  numbers, and descriptor numbers back into endpoints.  A socket address is a
 *  path, so it is resolved the way every other path is -- relative names work,
 *  and a listener and a client that name the same place agree on it.
 * ------------------------------------------------------------------ */

/* Defined with the rest of the permission rules, far below. */
static bool may_write(struct process *p, const char *abs);

static int socket_fd(struct process *p, socket_t *s)
{
    file_t f;

    memset(&f, 0, sizeof(f));
    f.type = FD_SOCKET;
    f.sock = s;

    int fd = vfs_fd_install(p, &f);
    if (fd < 0)
        socket_unref(s);        /* nowhere to put it; give the endpoint up */

    return fd;
}

/* Resolve a socket address.  It is a path, but no file is created or looked
 * up: only the name matters, and only to whoever is listening on it. */
static int socket_path(struct process *p, const char *path, char *out)
{
    return vfs_resolve(p, path, out, W_PATH_MAX + 1);
}

int vfs_listen(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = socket_path(p, path, abs);
    if (r < 0)
        return r;

    /* Answering to a name is a kind of writing there: a process that could
     * not create a file in a directory has no business owning an address in
     * it either. */
    if (!may_write(p, abs))
        return -W_EACCES;

    int err = 0;
    socket_t *s = socket_listen(abs, &err);
    if (!s)
        return err;

    return socket_fd(p, s);
}

int vfs_connect(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = socket_path(p, path, abs);
    if (r < 0)
        return r;

    int err = 0;
    socket_t *s = socket_connect(abs, &err);
    if (!s)
        return err;

    return socket_fd(p, s);
}

int vfs_accept(struct process *p, int fd)
{
    file_t *f = fd_get(p, fd);
    if (!f || f->type != FD_SOCKET)
        return -W_EBADF;

    int err = 0;
    socket_t *s = socket_accept(f->sock, &err);
    if (!s)
        return err;

    return socket_fd(p, s);
}

int vfs_send(struct process *p, int fd, const void *buf, uint32_t len,
             const int *fds, int fd_count)
{
    file_t *f = fd_get(p, fd);
    if (!f || f->type != FD_SOCKET)
        return -W_EBADF;
    if (fd_count < 0 || fd_count > W_SEND_MAX_FDS)
        return -W_EINVAL;

    /* Collect the descriptors being passed before anything is sent: one bad
     * number should fail the whole call rather than half of it. */
    file_t passing[W_SEND_MAX_FDS];

    for (int i = 0; i < fd_count; i++) {
        file_t *src = fd_get(p, fds[i]);
        if (!src)
            return -W_EBADF;
        passing[i] = *src;
    }

    return socket_send(f->sock, buf, len, passing, fd_count);
}

int vfs_recv(struct process *p, int fd, void *buf, uint32_t len,
             int *fds, int *fd_count)
{
    file_t *f = fd_get(p, fd);
    if (!f || f->type != FD_SOCKET)
        return -W_EBADF;

    int want = fd_count ? *fd_count : 0;
    if (want < 0 || want > W_SEND_MAX_FDS)
        return -W_EINVAL;

    file_t arrived[W_SEND_MAX_FDS];
    int    n = want;

    int r = socket_recv(f->sock, buf, len, arrived, &n);
    if (r < 0) {
        if (fd_count)
            *fd_count = 0;
        return r;
    }

    /* Each arriving descriptor already carries its reference; installing it
     * transfers that reference into this process's table.  A table with no
     * room left means the reference has nowhere to go, and dropping it is the
     * only honest thing to do -- the alternative is leaking it forever. */
    int installed = 0;
    for (int i = 0; i < n; i++) {
        int got = vfs_fd_install(p, &arrived[i]);
        if (got < 0) {
            vfs_fd_drop(&arrived[i]);
            continue;
        }
        fds[installed++] = got;
    }

    if (fd_count)
        *fd_count = installed;
    return r;
}

/* What is true of one descriptor right now. */
static int16_t poll_state(struct process *p, int fd, int16_t events)
{
    file_t *f = fd_get(p, fd);
    int16_t r = 0;

    if (!f)
        return W_POLLERR;

    switch (f->type) {
    case FD_SOCKET:
        if ((events & W_POLLIN) && socket_pollin(f->sock))   r |= W_POLLIN;
        if ((events & W_POLLOUT) && socket_pollout(f->sock)) r |= W_POLLOUT;
        if (socket_hungup(f->sock))                          r |= W_POLLHUP;
        break;

    case FD_PIPE:
        if (f->write_end) {
            if (events & W_POLLOUT) r |= W_POLLOUT;   /* a write may block */
        } else if ((events & W_POLLIN) && pipe_pollin(f->pipe)) {
            r |= W_POLLIN;
        }
        break;

    case FD_CONSOLE:
        if ((events & W_POLLIN) && keyboard_has_data()) r |= W_POLLIN;
        if (events & W_POLLOUT)                         r |= W_POLLOUT;
        break;

    case FD_NONE:
        r = W_POLLERR;
        break;

    default:
        /* A file or a directory is always ready: a read returns something
         * even if that something is the end of it. */
        r = (int16_t)(events & (W_POLLIN | W_POLLOUT));
        break;
    }

    return r;
}

/* Wait until one of `fds` is ready, or the timeout runs out.
 *
 * The sleep is bounded by a tick as well as by the caller's timeout, so a
 * descriptor whose readiness nothing announces -- the console, whose keyboard
 * interrupt wakes a different reason -- is still noticed promptly.  Sockets
 * wake it immediately, which is what matters for a display protocol. */
int vfs_poll(struct process *p, wpollfd_t *fds, int count, int timeout_ms)
{
    uint32_t deadline = pit_ticks() + (uint32_t)((timeout_ms > 0)
                                                 ? (timeout_ms * PIT_HZ + 999) / 1000
                                                 : 0);

    for (;;) {
        int ready = 0;

        for (int i = 0; i < count; i++) {
            fds[i].revents = poll_state(p, fds[i].fd, fds[i].events);
            if (fds[i].revents)
                ready++;
        }

        if (ready || timeout_ms == 0)
            return ready;

        if (timeout_ms > 0 && (int32_t)(pit_ticks() - deadline) >= 0)
            return 0;

        uint32_t next = pit_ticks() + 1;
        if (timeout_ms > 0 && (int32_t)(next - deadline) > 0)
            next = deadline;

        sched_block_until(WAIT_SOCKET, next);
    }
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
 *  Write permission
 *
 * WFS stores no per-file owner or mode -- an inode is 64 bytes and has no
 * room for one -- so permission is decided by path instead.  That is coarser
 * than Unix, but it expresses the rules this system actually has:
 *
 *   root                          may write anywhere
 *   /kernel                       root only, whatever roles a user holds
 *   /app                          needs W_ROLE_APPEDITOR
 *   /userconfig                   needs W_ROLE_USEREDITOR
 *   /userconfig/<name>/password   root only -- overrides the line above
 *   /home/<user>                  that user's own directory, and below it
 *   anywhere else                 read-only
 *
 * Reading is unrestricted apart from two things, both root-only:
 *
 *   /userconfig/<name>/password   nobody else may read a password, which is
 *                                 the whole point of keeping them in their own
 *                                 subdirectories -- a usereditor can set one
 *                                 through the kernel but cannot read it back
 *   /home/<other>                 home directories are private
 *
 * /home itself still lists, so you can see which accounts exist; the account
 * list in /userconfig/users already says that much.
 *
 * Note the ordering: the password rule is checked before the /userconfig rule,
 * so the more specific path wins.  Written the other way round, usereditor
 * would silently have gained access to every password on the system.
 * ------------------------------------------------------------------ */

/* True if `path` is `prefix`, or something inside it.  Compares whole
 * components, so "/apple" does not count as being under "/app". */
static bool path_within(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);

    if (strncmp(path, prefix, n) != 0)
        return false;

    return path[n] == '\0' || path[n] == '/';
}

/* True for /userconfig/<anything>/password.
 *
 * Matched by shape rather than by looking the user up, so a stale directory
 * left behind by a removed account is still protected. */
static bool is_password_file(const char *abs)
{
    if (!path_within(abs, "/userconfig"))
        return false;

    const char *last = strrchr(abs, '/');
    if (!last || strcmp(last + 1, "password") != 0)
        return false;

    /* Exactly /userconfig/<name>/password: three separators, no more, so a
     * deeper path cannot pose as one. */
    int slashes = 0;
    for (const char *c = abs; *c; c++)
        if (*c == '/')
            slashes++;

    return slashes == 3;
}

/* Build this process's own home directory. */
static bool home_of(struct process *p, char *out, size_t cap)
{
    wuser_t me;

    if (user_by_uid(p->uid, &me) < 0)
        return false;

    size_t at = 0;
    for (const char *s = "/home/"; *s && at + 1 < cap; s++)
        out[at++] = *s;
    for (const char *s = me.name; *s && at + 1 < cap; s++)
        out[at++] = *s;
    out[at] = '\0';

    return true;
}

/* Decide whether `p` may read `abs`. */
static bool may_read(struct process *p, const char *abs)
{
    if (p->uid == W_ROOT_UID)
        return true;

    if (is_password_file(abs))
        return false;

    /* A home directory belongs to one user.  /home itself is still listable,
     * so you can see that other accounts exist -- which the account list
     * already tells you -- but not what is inside them. */
    if (path_within(abs, "/home")) {
        if (strcmp(abs, "/home") == 0)
            return true;

        char home[W_PATH_MAX + 1];
        if (!home_of(p, home, sizeof(home)))
            return false;

        return path_within(abs, home);
    }

    return true;
}

/* Decide whether `p` may create, modify or delete `abs`, which must already
 * be resolved and normalised -- otherwise "/home/bob/../../app/x" would slip
 * past the prefix tests. */
static bool may_write(struct process *p, const char *abs)
{
    if (p->uid == W_ROOT_UID)
        return true;

    if (path_within(abs, "/kernel"))
        return false;

    /* Checked before the /userconfig rule below, so the specific case wins. */
    if (is_password_file(abs))
        return false;

    if (path_within(abs, "/userconfig"))
        return user_has_role(p->uid, W_ROLE_USEREDITOR);

    if (path_within(abs, "/app"))
        return user_has_role(p->uid, W_ROLE_APPEDITOR);

    /* A unit file decides what the machine runs at boot, so editing one is
     * the same permission as starting and stopping services by hand. */
    if (path_within(abs, "/services"))
        return user_has_role(p->uid, W_ROLE_SYSCTLEDIT);

    char home[W_PATH_MAX + 1];
    if (!home_of(p, home, sizeof(home)))
        return false;

    return path_within(abs, home);
}

/* Resolve `path` and check write permission in one step, since every caller
 * that changes something needs both. */
static int resolve_for_write(struct process *p, const char *path,
                             char *out, size_t out_size)
{
    int r = vfs_resolve(p, path, out, out_size);
    if (r < 0)
        return r;

    return may_write(p, out) ? 0 : -W_EACCES;
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

    /* Both directions are checked before the file is even looked up, so a
     * refusal cannot be told apart from the file not existing by timing. */
    if (!may_read(p, abs))
        return -W_EACCES;

    if ((flags & W_O_ACCMODE) != W_O_RDONLY ||
        (flags & (W_O_CREAT | W_O_TRUNC | W_O_APPEND))) {
        if (!may_write(p, abs))
            return -W_EACCES;
    }

    bool ram = ramfs_owns(abs);
    uint32_t ino;
    r = fs_lookup(abs, &ino);

    if (r == -W_ENOENT && (flags & W_O_CREAT)) {
        r = fs_create(abs, WFS_TYPE_FILE, &ino);
        if (r < 0)
            return r;
    } else if (r < 0) {
        return r;
    }

    struct wfs_inode in;
    r = fs_read_inode(ram, ino, &in);
    if (r < 0)
        return r;

    if (in.type == WFS_TYPE_DIR && (flags & W_O_ACCMODE) != W_O_RDONLY)
        return -W_EISDIR;

    int fd = fd_alloc(p);
    if (fd < 0)
        return fd;

    if ((flags & W_O_TRUNC) && in.type == WFS_TYPE_FILE) {
        r = fs_truncate(ram, ino);
        if (r < 0)
            return r;
        in.size = 0;
    }

    p->fds[fd].type   = (in.type == WFS_TYPE_DIR) ? FD_DIR : FD_FILE;
    p->fds[fd].ino    = ino;
    p->fds[fd].ram    = ram;
    p->fds[fd].flags  = flags;
    p->fds[fd].offset = (flags & W_O_APPEND) ? in.size : 0;

    return fd;
}

int vfs_close(struct process *p, int fd)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;

    fd_release(f);
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
    if (f->type == FD_PIPE)
        return f->write_end ? -W_EBADF : pipe_read(f->pipe, buf, len);
    if (f->type == FD_SOCKET)
        return socket_recv(f->sock, buf, len, NULL, NULL);
    if (f->type == FD_SHM)
        return -W_EINVAL;        /* shared memory is mapped, not read */
    if (f->type == FD_DIR)
        return -W_EISDIR;
    if ((f->flags & W_O_ACCMODE) == W_O_WRONLY)
        return -W_EACCES;

    int n = fs_read(f->ram, f->ino, f->offset, buf, len);
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
    if (f->type == FD_PIPE)
        return f->write_end ? pipe_write(f->pipe, buf, len) : -W_EBADF;
    if (f->type == FD_SOCKET)
        return socket_send(f->sock, buf, len, NULL, 0);
    if (f->type == FD_SHM)
        return -W_EINVAL;        /* shared memory is mapped, not written */
    if (f->type == FD_DIR)
        return -W_EISDIR;
    if ((f->flags & W_O_ACCMODE) == W_O_RDONLY)
        return -W_EACCES;

    /* O_APPEND has to re-read the size each time: another descriptor may
     * have extended the file since this one was opened. */
    if (f->flags & W_O_APPEND) {
        struct wfs_inode in;
        if (fs_read_inode(f->ram, f->ino, &in) == 0)
            f->offset = in.size;
    }

    int n = fs_write(f->ram, f->ino, f->offset, buf, len);
    if (n > 0)
        f->offset += (uint32_t)n;
    return n;
}

int vfs_lseek(struct process *p, int fd, int32_t offset, int whence)
{
    file_t *f = fd_get(p, fd);
    if (!f)
        return -W_EBADF;
    if (f->type == FD_CONSOLE || f->type == FD_PIPE)
        return -W_ESPIPE;

    struct wfs_inode in;
    int r = fs_read_inode(f->ram, f->ino, &in);
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

    /* Size and type are worth hiding too, not just contents. */
    if (!may_read(p, abs))
        return -W_EACCES;

    uint32_t ino;
    r = fs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = fs_read_inode(ramfs_owns(abs), ino, &in);
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
    int  r = resolve_for_write(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = fs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = fs_read_inode(ramfs_owns(abs), ino, &in);
    if (r < 0)
        return r;
    if (in.type == WFS_TYPE_DIR)
        return -W_EISDIR;

    return fs_unlink(abs);
}

int vfs_mkdir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = resolve_for_write(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    return fs_create(abs, WFS_TYPE_DIR, NULL);
}

int vfs_rmdir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = resolve_for_write(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    uint32_t ino;
    r = fs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = fs_read_inode(ramfs_owns(abs), ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    return fs_unlink(abs);
}

int vfs_opendir(struct process *p, const char *path)
{
    char abs[W_PATH_MAX + 1];
    int  r = vfs_resolve(p, path, abs, sizeof(abs));
    if (r < 0)
        return r;

    if (!may_read(p, abs))
        return -W_EACCES;

    uint32_t ino;
    r = fs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = fs_read_inode(ramfs_owns(abs), ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_DIR)
        return -W_ENOTDIR;

    int fd = fd_alloc(p);
    if (fd < 0)
        return fd;

    p->fds[fd].type   = FD_DIR;
    p->fds[fd].ino    = ino;
    p->fds[fd].ram    = ramfs_owns(abs);
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

    int r = fs_readdir(f->ram, f->ino, f->offset, out);
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

    /* Standing in a directory you cannot read is only a way to reach its
     * contents by relative path. */
    if (!may_read(p, abs))
        return -W_EACCES;

    uint32_t ino;
    r = fs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = fs_read_inode(ramfs_owns(abs), ino, &in);
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

    if (!may_read(p, abs))
        return -W_EACCES;

    bool ram = ramfs_owns(abs);
    uint32_t ino;
    r = fs_lookup(abs, &ino);
    if (r < 0)
        return r;

    struct wfs_inode in;
    r = fs_read_inode(ram, ino, &in);
    if (r < 0)
        return r;
    if (in.type != WFS_TYPE_FILE)
        return -W_EISDIR;
    if (in.size == 0)
        return -W_ENOEXEC;

    void *buf = kmalloc(in.size);
    if (!buf)
        return -W_ENOMEM;

    r = fs_read(ram, ino, 0, buf, in.size);
    if (r != (int)in.size) {
        kfree(buf);
        return (r < 0) ? r : -W_EIO;
    }

    *data_out = buf;
    *size_out = in.size;
    return 0;
}
