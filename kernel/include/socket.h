/* Local sockets: a named, bidirectional byte stream that can carry
 * descriptors.
 *
 * A pipe is one direction, anonymous, and reaches another process only by
 * being inherited across a spawn.  That is enough for a shell wiring a child's
 * output to a terminal emulator, and not enough for a display server: a client
 * has to find the compositor by name, having never been its child, talk in both
 * directions, and hand over a descriptor for a buffer of pixels rather than a
 * copy of them.  This is that -- the Unix domain socket, in the shape WOS
 * needs it.
 *
 * The name is a path but not a file.  Nothing appears on the disk; the path is
 * the address a listener answers to and it is gone when the listener closes,
 * which is also why it needs no permission model of its own beyond who can
 * reach the directory it names.
 *
 * Descriptor passing is the interesting part.  A descriptor sent this way is
 * copied into the receiver's table, with a reference taken, so the two
 * processes end up holding the same underlying object -- the same pipe, the
 * same open file -- and either may close its own without disturbing the other.
 * Each one is delivered no earlier than the byte it was sent with, so a
 * receiver that has read the message describing a buffer is holding that
 * buffer's descriptor by then and never before.
 */
#ifndef WOS_SOCKET_H
#define WOS_SOCKET_H

#include "types.h"
#include "vfs.h"
#include "wabi.h"

/* Bytes buffered in each direction, and descriptors queued in each direction.
 * Both are the figures libwayland uses for its connection buffers, so a
 * protocol written against those limits behaves here as it does there. */
#define SOCK_CAP    4096
#define SOCK_FD_CAP 28

/* How much room a poll for W_POLLOUT promises.
 *
 * W_POLLOUT says a write will not block, and a write of one byte into a buffer
 * with one byte free is the only write that claim holds for.  A protocol
 * writes whole messages, so "writable" has to mean room for one -- otherwise
 * two programs that each poll before writing can still both block on each
 * other, each having been told it was safe to go ahead.
 *
 * So a socket reports itself writable only with this much space free, and a
 * writer that flushes no more than this after a positive poll cannot block.
 * The figure is in the ABI as W_SEND_CHUNK, because it is a promise made to
 * applications rather than an internal one. */
#define SOCK_WRITE_CHUNK W_SEND_CHUNK

/* Connections a listener may have waiting to be accepted. */
#define SOCK_BACKLOG 8

struct process;

/* One descriptor in flight, and where in the byte stream it was sent. */
typedef struct {
    file_t   file;
    uint64_t at;             /* sender's byte position when it was queued */
} sock_fd_t;

/* One direction of a connection: the bytes one endpoint writes and the other
 * reads, and the descriptors travelling with them. */
typedef struct {
    uint8_t   buf[SOCK_CAP];
    uint32_t  head, tail, count;
    uint64_t  written;       /* cumulative, for placing descriptors     */
    uint64_t  read;          /* cumulative, for releasing them in order */

    sock_fd_t fds[SOCK_FD_CAP];
    uint32_t  fd_head, fd_tail, fd_count;
} sock_dir_t;

/* The part a connected pair shares.  Endpoint 0 writes dir[0] and reads
 * dir[1]; endpoint 1 does the opposite. */
typedef struct sock_conn {
    int        refs;         /* endpoints still holding it */
    bool       closed[2];
    sock_dir_t dir[2];
} sock_conn_t;

typedef struct socket {
    int  refs;               /* descriptors naming this endpoint */

    /* A listener: an address, and connections waiting to be accepted. */
    bool listening;
    char path[W_PATH_MAX + 1];
    struct socket *backlog[SOCK_BACKLOG];
    int  backlog_count;

    /* A connected endpoint: which side of which connection. */
    sock_conn_t *conn;
    int          side;
} socket_t;

/* Answer to `path` from now on.  Returns the listening socket, or NULL with
 * `*err` set to -W_EEXIST if the name is taken, or -W_ENOMEM. */
socket_t *socket_listen(const char *path, int *err);

/* Connect to whoever is listening on `path`.  Returns this end of a new
 * connection, or NULL with `*err` set: -W_ENOENT when nothing is listening,
 * -W_EBUSY when the listener's backlog is full, -W_ENOMEM otherwise.
 *
 * Returns as soon as the connection is queued; it does not wait for the
 * listener to accept it, which is what lets a client start talking
 * immediately. */
socket_t *socket_connect(const char *path, int *err);

/* Take the next waiting connection, blocking until one arrives.  NULL with
 * `*err` if `s` is not a listener. */
socket_t *socket_accept(socket_t *s, int *err);

/* Move a message.  `fds` and `fd_count` carry descriptors alongside the bytes;
 * socket_send takes a reference to each, and socket_recv hands over references
 * the caller then owns.
 *
 * Both block the way a pipe does: a send waits for room, a receive waits for
 * bytes.  A receive on a connection whose peer has gone returns 0 once the
 * buffer is empty -- end of file -- and a send to one fails with -W_EPIPE.
 *
 * socket_recv writes back through `fd_count` how many descriptors it
 * delivered. */
int socket_send(socket_t *s, const void *buf, uint32_t len,
                const file_t *fds, int fd_count);
int socket_recv(socket_t *s, void *buf, uint32_t len,
                file_t *fds, int *fd_count);

/* Would a read or a write block?  A listener is readable when a connection is
 * waiting, which is what makes accept() pollable. */
bool socket_pollin(socket_t *s);
bool socket_pollout(socket_t *s);

/* True once the other end has closed and nothing is left to read. */
bool socket_hungup(socket_t *s);

/* Adjust the reference count on an endpoint.  The last drop closes it, wakes
 * whatever the peer was waiting for, and releases any descriptors still in
 * flight -- they were never delivered, so they are the sender's to give up. */
void socket_ref(socket_t *s);
void socket_unref(socket_t *s);

#endif /* WOS_SOCKET_H */
