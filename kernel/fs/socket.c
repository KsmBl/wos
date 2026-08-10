/* Local sockets.  See socket.h for the model. */

#include "socket.h"
#include "sched.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* Every listener in the system.  A handful is plenty: one display server, one
 * or two services beside it.  They are searched linearly by name, which at
 * this size is faster than anything cleverer. */
#define MAX_LISTENERS 8

static socket_t *listeners[MAX_LISTENERS];

/* ------------------------------------------------------------------ *
 *  Endpoints
 * ------------------------------------------------------------------ */

static socket_t *socket_alloc(void)
{
    socket_t *s = kmalloc(sizeof(*s));
    if (!s)
        return NULL;

    memset(s, 0, sizeof(*s));
    s->refs = 1;
    return s;
}

/* This endpoint's outgoing and incoming directions.  Endpoint 0 writes dir[0]
 * and reads dir[1]; endpoint 1 does the opposite. */
static sock_dir_t *out_dir(socket_t *s) { return &s->conn->dir[s->side]; }
static sock_dir_t *in_dir(socket_t *s)  { return &s->conn->dir[!s->side]; }

static bool peer_gone(socket_t *s)
{
    return !s->conn || s->conn->closed[!s->side];
}

static void conn_unref(sock_conn_t *c)
{
    if (!c)
        return;
    if (--c->refs <= 0)
        kfree(c);
}

/* Give back every descriptor still queued in a direction.  These were sent and
 * never delivered, so the references belong to nobody now. */
static void drop_queued_fds(sock_dir_t *d)
{
    while (d->fd_count > 0) {
        vfs_fd_drop(&d->fds[d->fd_tail].file);
        d->fd_tail = (d->fd_tail + 1) % SOCK_FD_CAP;
        d->fd_count--;
    }
}

void socket_ref(socket_t *s)
{
    if (s)
        s->refs++;
}

void socket_unref(socket_t *s)
{
    if (!s || --s->refs > 0)
        return;

    if (s->listening) {
        for (int i = 0; i < MAX_LISTENERS; i++)
            if (listeners[i] == s)
                listeners[i] = NULL;

        /* Connections queued on a listener that closed were never accepted;
         * the far end finds out the same way it would if we had accepted and
         * immediately closed. */
        for (int i = 0; i < s->backlog_count; i++)
            socket_unref(s->backlog[i]);
    }

    if (s->conn) {
        s->conn->closed[s->side] = true;

        /* Nothing will ever read what we have not sent, and nothing will ever
         * be sent to us again. */
        drop_queued_fds(out_dir(s));
        drop_queued_fds(in_dir(s));

        conn_unref(s->conn);
    }

    kfree(s);

    /* A peer blocked on this connection has to wake up and see it end. */
    sched_wake(WAIT_SOCKET);
}

/* ------------------------------------------------------------------ *
 *  Making connections
 * ------------------------------------------------------------------ */

static socket_t *find_listener(const char *path)
{
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (listeners[i] && strcmp(listeners[i]->path, path) == 0)
            return listeners[i];
    return NULL;
}

socket_t *socket_listen(const char *path, uint32_t owner, int *err)
{
    if (find_listener(path)) {
        *err = -W_EEXIST;
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (!listeners[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        *err = -W_ENFILE;
        return NULL;
    }

    socket_t *s = socket_alloc();
    if (!s) {
        *err = -W_ENOMEM;
        return NULL;
    }

    s->listening = true;
    s->owner     = owner;
    strlcpy(s->path, path, sizeof(s->path));
    listeners[slot] = s;

    *err = 0;
    return s;
}

socket_t *socket_connect(const char *path, uint32_t uid, int *err)
{
    socket_t *l = find_listener(path);
    if (!l) {
        *err = -W_ENOENT;
        return NULL;
    }

    /* Whose socket it is decides who may speak into it.  Root is not stopped,
     * because root can already become anybody; everyone else talks to their
     * own session and not to somebody else's. */
    if (uid != l->owner && uid != W_ROOT_UID) {
        *err = -W_EPERM;
        return NULL;
    }

    if (l->backlog_count >= SOCK_BACKLOG) {
        *err = -W_EBUSY;
        return NULL;
    }

    sock_conn_t *conn = kmalloc(sizeof(*conn));
    socket_t    *mine = socket_alloc();
    socket_t    *theirs = socket_alloc();

    if (!conn || !mine || !theirs) {
        if (conn)   kfree(conn);
        if (mine)   kfree(mine);
        if (theirs) kfree(theirs);
        *err = -W_ENOMEM;
        return NULL;
    }

    memset(conn, 0, sizeof(*conn));
    conn->refs = 2;

    mine->conn   = conn;
    mine->side   = 0;
    theirs->conn = conn;
    theirs->side = 1;

    l->backlog[l->backlog_count++] = theirs;

    /* Whoever is blocked in accept() can proceed. */
    sched_wake(WAIT_SOCKET);

    *err = 0;
    return mine;
}

socket_t *socket_accept(socket_t *s, int *err)
{
    if (!s || !s->listening) {
        *err = -W_EBADF;
        return NULL;
    }

    while (s->backlog_count == 0)
        sched_block(WAIT_SOCKET);

    socket_t *c = s->backlog[0];
    for (int i = 1; i < s->backlog_count; i++)
        s->backlog[i - 1] = s->backlog[i];
    s->backlog_count--;

    *err = 0;
    return c;
}

/* ------------------------------------------------------------------ *
 *  Moving messages
 * ------------------------------------------------------------------ */

int socket_send(socket_t *s, const void *buf, uint32_t len,
                const file_t *fds, int fd_count)
{
    if (!s || s->listening || !s->conn)
        return -W_EBADF;
    if (fd_count < 0 || fd_count > SOCK_FD_CAP)
        return -W_EINVAL;

    sock_dir_t *d = out_dir(s);

    /* Descriptors go first and together: a message is not delivered in pieces
     * as far as its descriptors are concerned, and a queue with room for only
     * some of them would split it. */
    while ((int)(SOCK_FD_CAP - d->fd_count) < fd_count) {
        if (peer_gone(s))
            return -W_EPIPE;
        sched_block(WAIT_SOCKET);
    }

    for (int i = 0; i < fd_count; i++) {
        sock_fd_t *slot = &d->fds[d->fd_head];

        slot->file = fds[i];
        slot->at   = d->written + len;   /* delivered with the last byte */
        vfs_fd_retain(&slot->file);

        d->fd_head = (d->fd_head + 1) % SOCK_FD_CAP;
        d->fd_count++;
    }

    const uint8_t *in = buf;
    uint32_t n = 0;

    while (n < len) {
        if (peer_gone(s))
            return n > 0 ? (int)n : -W_EPIPE;

        while (d->count == SOCK_CAP && !peer_gone(s))
            sched_block(WAIT_SOCKET);

        if (peer_gone(s))
            return n > 0 ? (int)n : -W_EPIPE;

        while (n < len && d->count < SOCK_CAP) {
            d->buf[d->head] = in[n++];
            d->head = (d->head + 1) % SOCK_CAP;
            d->count++;
            d->written++;
        }

        sched_wake(WAIT_SOCKET);
    }

    /* A message with descriptors and no bytes still has to wake the reader:
     * there is nothing for it to block on otherwise. */
    if (len == 0 && fd_count > 0)
        sched_wake(WAIT_SOCKET);

    return (int)n;
}

int socket_recv(socket_t *s, void *buf, uint32_t len, file_t *fds,
                int *fd_count)
{
    int want_fds = fd_count ? *fd_count : 0;

    if (fd_count)
        *fd_count = 0;

    if (!s || s->listening || !s->conn)
        return -W_EBADF;

    sock_dir_t *d = in_dir(s);

    /* Wait for something to read.  A message that is only descriptors counts
     * as something, or a client that sent one and nothing else would leave the
     * receiver asleep holding it. */
    while (d->count == 0 && d->fd_count == 0 && !peer_gone(s))
        sched_block(WAIT_SOCKET);

    uint8_t *out = buf;
    uint32_t n = 0;

    while (n < len && d->count > 0) {
        out[n++] = d->buf[d->tail];
        d->tail = (d->tail + 1) % SOCK_CAP;
        d->count--;
        d->read++;
    }

    /* Hand over every descriptor whose place in the stream has now been read
     * past.  One sent with a later byte stays queued until that byte is
     * delivered, which is what keeps a descriptor from arriving before the
     * message that explains it. */
    while (want_fds > 0 && d->fd_count > 0 && d->fds[d->fd_tail].at <= d->read) {
        *fds++ = d->fds[d->fd_tail].file;
        d->fd_tail = (d->fd_tail + 1) % SOCK_FD_CAP;
        d->fd_count--;
        want_fds--;
        if (fd_count)
            (*fd_count)++;
    }

    /* A sender blocked on a full buffer can now make progress. */
    sched_wake(WAIT_SOCKET);

    return (int)n;
}

/* ------------------------------------------------------------------ *
 *  Readiness
 * ------------------------------------------------------------------ */

bool socket_pollin(socket_t *s)
{
    if (!s)
        return true;                 /* a bad socket must not hang a poller */
    if (s->listening)
        return s->backlog_count > 0;
    if (!s->conn)
        return true;

    sock_dir_t *d = in_dir(s);
    return d->count > 0 || d->fd_count > 0 || peer_gone(s);
}

bool socket_pollout(socket_t *s)
{
    if (!s || s->listening || !s->conn)
        return false;
    if (peer_gone(s))
        return true;                 /* the write will fail, but not block */

    return out_dir(s)->count + SOCK_WRITE_CHUNK <= SOCK_CAP;
}

bool socket_hungup(socket_t *s)
{
    if (!s || s->listening || !s->conn)
        return false;

    return peer_gone(s) && in_dir(s)->count == 0 && in_dir(s)->fd_count == 0;
}
