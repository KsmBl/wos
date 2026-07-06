/* The Wayland wire format: a connection, and messages on it.
 *
 * Shared by the client and the server halves, because the format is the same
 * in both directions.  A message is:
 *
 *     uint32  object id
 *     uint32  (size << 16) | opcode        size counts this header
 *     ...     arguments, each padded to four bytes
 *
 * and an argument is packed according to one letter of the message's
 * signature.  Nothing here knows what any particular message means; it packs
 * and unpacks whatever the interface tables describe, which is what makes
 * adding an interface a matter of describing it.
 *
 * Buffering is not an optimisation here, it is what stops two programs
 * deadlocking on each other.  A send blocks when the socket is full, so a
 * compositor writing events to a client that is itself blocked writing
 * requests would stop both of them forever.  Everything is queued in memory
 * and flushed only as much as a poll has promised there is room for.
 */
#ifndef WAYLAND_WIRE_H
#define WAYLAND_WIRE_H

#include "wayland-util.h"

/* The largest message the protocol allows, and so the size of both buffers.
 * The same figure libwayland uses. */
#define WL_BUFFER_SIZE 4096

struct wl_connection;

/* One message sitting in the input buffer, not yet consumed. */
struct wl_msg_view {
    uint32_t       id;
    uint32_t       opcode;
    uint32_t       size;        /* including the eight byte header */
    const uint8_t *body;
    uint32_t       body_len;
};

struct wl_connection *wl_connection_create(int fd);
void wl_connection_destroy(struct wl_connection *c);
int  wl_connection_fd(const struct wl_connection *c);

/* True once the peer has gone or the connection has been broken by a protocol
 * error.  Everything below turns into a no-op after that. */
int  wl_connection_broken(const struct wl_connection *c);
void wl_connection_break(struct wl_connection *c);

/* Queue a message.  `args` holds one entry per letter of the signature, in
 * order; the 'h' entries name descriptors, which are queued to travel with the
 * message's last byte.  Returns 0, or -1 if the message does not fit. */
int wl_connection_queue(struct wl_connection *c, uint32_t id, uint32_t opcode,
                        const struct wl_message *msg,
                        const union wl_argument *args);

/* Push queued bytes out, no more than the socket has promised room for.
 * Whatever does not go stays queued.  Returns 0, or -1 if the peer has gone. */
int wl_connection_flush(struct wl_connection *c);

/* Bytes still waiting to be flushed: what a poll loop watches W_POLLOUT for. */
uint32_t wl_connection_pending(const struct wl_connection *c);

/* Take whatever has arrived into the input buffer.  Returns the number of
 * bytes read, 0 at end of file, or -1. */
int wl_connection_read(struct wl_connection *c);

/* Look at the next complete message without consuming it.  Returns 1 if there
 * is one, 0 if the rest has not arrived, -1 if what arrived is not a message. */
int wl_connection_next(struct wl_connection *c, struct wl_msg_view *out);

/* Drop the message wl_connection_next() returned. */
void wl_connection_consume(struct wl_connection *c, uint32_t size);

/* Unpack a message body according to a signature.
 *
 * Strings and arrays point into the connection's own input buffer and are
 * valid until the message is consumed, which is the same rule libwayland's
 * dispatcher works under.  Descriptors are taken from the ones that arrived
 * with the message and become the caller's to close.
 *
 * Returns the number of arguments unpacked, or -1 if the body does not match
 * the signature -- a short message, or a string running off the end. */
int wl_connection_unpack(struct wl_connection *c, const char *signature,
                         const uint8_t *body, uint32_t body_len,
                         union wl_argument *args, int max);

/* How many arguments a signature describes, ignoring the version prefix. */
int wl_signature_count(const char *signature);

#endif /* WAYLAND_WIRE_H */
