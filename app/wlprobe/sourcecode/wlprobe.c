/* wlprobe -- talk to the display server the way a Wayland client would.
 *
 * It does what the opening moves of every Wayland client are: connect, ask for
 * the registry, ask for a roundtrip, and print what comes back.  That is
 * enough to tell whether the server is up and speaking the protocol, and it is
 * the piece a client library will grow out of.
 *
 * The wire format is the real one, so the bytes this sends are the bytes
 * libwayland would send:
 *
 *     uint32  object id
 *     uint32  (size << 16) | opcode        size counts this header too
 *     ...     arguments, each padded to four bytes
 *
 * Object 1 is wl_display and exists before anything is asked for.  Ids are
 * chosen by the client, counting up from 2.
 */

#include <wkernel.h>

#define SOCKET_PATH "/ramdisk/wayland-0"

#define WL_DISPLAY_ID           1
#define WL_DISPLAY_SYNC         0
#define WL_DISPLAY_GET_REGISTRY 1

/* The events this can be sent back, for naming them in the output. */
#define WL_DISPLAY_ERROR     0
#define WL_DISPLAY_DELETE_ID 1

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A request with one 32-bit argument, which is the shape of both of the ones
 * sent here: each asks the server to create an object under an id we choose. */
static int request(int fd, uint32_t object, uint32_t opcode, uint32_t arg)
{
    uint8_t message[12];

    put32(message, object);
    put32(message + 4, (12u << 16) | opcode);
    put32(message + 8, arg);

    wmsg_t msg = { message, sizeof(message), 0, 0 };
    return wsend(fd, &msg);
}

static const char *event_name(uint32_t object, uint32_t opcode)
{
    if (object == WL_DISPLAY_ID)
        return opcode == WL_DISPLAY_ERROR     ? "wl_display.error"
             : opcode == WL_DISPLAY_DELETE_ID ? "wl_display.delete_id"
                                              : "wl_display event";

    /* Every other object here is one this program asked for, and the only
     * event either of them sends is wl_callback.done. */
    return opcode == 0 ? "wl_callback.done" : "event";
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : SOCKET_PATH;

    int fd = wconnect(path);
    if (fd < 0) {
        wfprintf(W_STDERR, "wlprobe: cannot reach %s: %s\n", path,
                 wstrerror(-fd));
        if (-fd == W_ENOENT)
            wfprintf(W_STDERR, "wlprobe: is the display server running? "
                               "`systemctl status wayland`\n");
        return 1;
    }
    wprintf("connected to %s\n", path);

    request(fd, WL_DISPLAY_ID, WL_DISPLAY_GET_REGISTRY, 2);
    wprintf("sent wl_display.get_registry, asking for object 2\n");

    /* A roundtrip: the server answers this last, so its reply arriving means
     * everything sent before it has been dealt with. */
    request(fd, WL_DISPLAY_ID, WL_DISPLAY_SYNC, 3);
    wprintf("sent wl_display.sync, asking for callback 3\n");

    uint8_t buf[512];
    wmsg_t  in = { buf, sizeof(buf), 0, 0 };

    int n = wrecv(fd, &in);
    if (n <= 0) {
        wfprintf(W_STDERR, "wlprobe: the server said nothing back\n");
        wclose(fd);
        return 1;
    }

    wprintf("%d bytes back:\n", n);

    for (int at = 0; at + 8 <= n; ) {
        uint32_t object = get32(buf + at);
        uint32_t word   = get32(buf + at + 4);
        uint32_t opcode = word & 0xFFFF;
        uint32_t size   = word >> 16;

        if (size < 8 || at + (int)size > n) {
            wprintf("  malformed message of %u bytes\n", size);
            break;
        }

        wprintf("  object %u  %s", object, event_name(object, opcode));
        if (size >= 12)
            wprintf("  (%u)", get32(buf + at + 8));
        wprintf("\n");

        at += (int)size;
    }

    wclose(fd);
    return 0;
}
