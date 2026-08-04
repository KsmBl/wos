/* wipc -- i3's IPC, which is what sway kept.
 *
 * A magic string, a length, a type and a payload, over a socket.  The protocol
 * is worth having in the library rather than in each program that speaks it,
 * because the interesting property of the compositor here is not that its own
 * tools can drive it but that *anything* can: `swaymsg`, the settings window,
 * and whatever anybody writes next all send the same four fields.
 *
 * Three programs had grown their own copy of the framing -- and the compositor
 * a fourth, on the answering side.  The numbers are the same numbers; they are
 * defined once here so that they cannot drift apart.
 */
#ifndef WKERNEL_WIPC_H
#define WKERNEL_WIPC_H

#include <wkernel.h>

/* Where sway listens.  The socket belongs to whoever is running the
 * compositor, so a connection from another user is refused by the kernel. */
#define WIPC_SWAY_SOCKET "/ramdisk/sway-ipc.sock"

#define WIPC_MAGIC     "i3-ipc"
#define WIPC_MAGIC_LEN 6
#define WIPC_HEADER    (WIPC_MAGIC_LEN + 8)

/* The message types i3 defined, in i3's order. */
#define WIPC_RUN_COMMAND       0
#define WIPC_GET_WORKSPACES    1
#define WIPC_SUBSCRIBE         2
#define WIPC_GET_OUTPUTS       3
#define WIPC_GET_TREE          4
#define WIPC_GET_MARKS         5
#define WIPC_GET_BAR_CONFIG    6
#define WIPC_GET_VERSION       7
#define WIPC_GET_BINDING_MODES 8
#define WIPC_GET_CONFIG        9
#define WIPC_SEND_TICK        10

/* Put a 32-bit value into a header, little endian, and take one out. */
void     wipc_put32(uint8_t *p, uint32_t v);
uint32_t wipc_get32(const uint8_t *p);

/**
 * Send one message and read the reply.
 *
 * @param path    The socket, usually #WIPC_SWAY_SOCKET.
 * @param type    One of the `WIPC_*` message types.
 * @param payload The message body, or NULL for the types that have none.
 * @param reply   Filled with the reply body, always NUL-terminated.
 * @param size    How big @p reply is.
 *
 * @return The length of the reply, or a negative error: whatever wconnect()
 *         gave -- `-W_ENOENT` when the compositor is not running, `-W_EPERM`
 *         when it belongs to another user -- or `-W_EIO` if it said nothing
 *         back.
 */
int wipc_message(const char *path, uint32_t type, const char *payload,
                 char *reply, wsize_t size);

/**
 * Run one sway command, the way `swaymsg` does.
 *
 * @return 0 when the compositor reported success, `-W_EINVAL` when it refused
 *         the command, or the error from wipc_message().
 */
int wipc_command(const char *path, const char *command);

/**
 * Pull one value out of a flat JSON object -- enough to read a reply without
 * carrying a parser.
 *
 * Strings arrive without their quotes; anything else arrives as it was
 * written.  Only the first match is found, which is what a caller wants from
 * the reply's own header and not from a list of things inside it.
 *
 * @return 1 if the key was there, 0 if it was not.
 */
int wipc_field(const char *json, const char *key, char *out, wsize_t size);

#endif /* WKERNEL_WIPC_H */
