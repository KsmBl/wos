/* waylandd -- the Wayland display server, as far as it goes.
 *
 * It owns the socket clients connect to, speaks the Wayland wire format, and
 * implements wl_display: the one object that exists before a client has asked
 * for anything.  A client can connect, ask for the registry, ask for a
 * roundtrip, and be answered correctly.
 *
 * What it does not have yet is anything to advertise.  A compositor's registry
 * lists wl_compositor, wl_shm, wl_seat, xdg_wm_base and the rest, and every one
 * of those needs a piece that WOS has not got: shared memory for buffers, a
 * surface to composite into the framebuffer, a keymap.  So the registry is
 * announced and comes back empty, which is a true statement about this machine
 * rather than a promise it cannot keep.  A client that binds anything is told
 * so through wl_display.error, which is what the protocol says to do.
 *
 * The wire format is the real one, so what is here is compatible rather than
 * merely similar:
 *
 *     uint32  object id
 *     uint32  (size << 16) | opcode        size counts this header too
 *     ...     arguments, each padded to four bytes
 *
 * Run through the service manager: `systemctl start wayland`.  It logs to a
 * file rather than to the console, because a service has no terminal to be
 * rude to.
 */

#include <wkernel.h>
#include <stdarg.h>

#define SOCKET_PATH "/ramdisk/wayland-0"
#define LOG_PATH    "/ramdisk/wayland.log"

#define MAX_CLIENTS 8
#define BUF_SIZE    4096

/* The one object every connection starts with. */
#define WL_DISPLAY_ID 1

/* wl_display requests, and its events. */
#define WL_DISPLAY_SYNC         0
#define WL_DISPLAY_GET_REGISTRY 1
#define WL_DISPLAY_ERROR_EVENT  0
#define WL_DISPLAY_DELETE_ID    1

/* wl_display error codes, from the protocol. */
#define WL_DISPLAY_ERROR_INVALID_OBJECT 0
#define WL_DISPLAY_ERROR_INVALID_METHOD 1

/* wl_registry's one request, and wl_callback's one event. */
#define WL_REGISTRY_BIND   0
#define WL_CALLBACK_DONE   0

struct client {
    int      fd;
    int      in_use;
    uint32_t registry;               /* the id it asked for, or 0 */
    uint8_t  buf[BUF_SIZE];
    uint32_t have;                   /* bytes buffered            */
};

static struct client clients[MAX_CLIENTS];
static int           log_fd = -1;

/* ------------------------------------------------------------------ *
 *  Logging
 * ------------------------------------------------------------------ */

static void logf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void logf(const char *fmt, ...)
{
    char    line[192];
    va_list ap;

    va_start(ap, fmt);
    int n = wvsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;

    line[n]     = '\n';
    line[n + 1] = '\0';

    if (log_fd >= 0)
        wwrite(log_fd, line, (wsize_t)(n + 1));
}

/* ------------------------------------------------------------------ *
 *  The wire format
 * ------------------------------------------------------------------ */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Build a message header and send it with `len` bytes of arguments already in
 * `args`.  Every message is a whole number of 32-bit words. */
static void send_message(struct client *c, uint32_t object, uint32_t opcode,
                         const uint8_t *args, uint32_t len)
{
    uint8_t  out[256];
    uint32_t size = 8 + len;

    if (size > sizeof(out))
        return;

    put32(out, object);
    put32(out + 4, (size << 16) | (opcode & 0xFFFF));
    if (len)
        memcpy(out + 8, args, len);

    wmsg_t msg = { out, size, 0, 0 };
    wsend(c->fd, &msg);
}

/* wl_display.error: the object it was about, a code, and a message.  A string
 * on the wire is its length including the terminator, then the bytes, padded
 * out to a multiple of four. */
static void send_error(struct client *c, uint32_t object, uint32_t code,
                       const char *text)
{
    uint8_t  args[128];
    uint32_t at  = 0;
    uint32_t len = (uint32_t)strlen(text) + 1;

    if (8 + 4 + ((len + 3) & ~3u) > sizeof(args))
        return;

    put32(args + at, object); at += 4;
    put32(args + at, code);   at += 4;
    put32(args + at, len);    at += 4;

    memcpy(args + at, text, len);
    at += len;
    while (at & 3)
        args[at++] = 0;

    send_message(c, WL_DISPLAY_ID, WL_DISPLAY_ERROR_EVENT, args, at);
}

/* ------------------------------------------------------------------ *
 *  wl_display
 * ------------------------------------------------------------------ */

static void handle_sync(struct client *c, const uint8_t *args, uint32_t len)
{
    if (len < 4)
        return;

    uint32_t callback = get32(args);
    uint8_t  done[4];

    /* wl_callback.done carries the event serial; nothing here counts events
     * yet, so it is zero -- which clients treat as an opaque value. */
    put32(done, 0);
    send_message(c, callback, WL_CALLBACK_DONE, done, 4);

    /* The callback is used up.  Telling the client the id is free again is
     * what lets it reuse the number, and libwayland asserts if we do not. */
    uint8_t deleted[4];
    put32(deleted, callback);
    send_message(c, WL_DISPLAY_ID, WL_DISPLAY_DELETE_ID, deleted, 4);

    logf("  sync -> callback %u done", callback);
}

static void handle_get_registry(struct client *c, const uint8_t *args,
                                uint32_t len)
{
    if (len < 4)
        return;

    c->registry = get32(args);

    /* Nothing to announce.  A compositor lists wl_compositor, wl_shm, wl_seat
     * and the rest here; each one needs a piece this system has not built yet,
     * and announcing an interface that cannot be bound would be worse than
     * announcing none. */
    logf("  get_registry -> %u (no globals to announce yet)", c->registry);
}

/* One complete message from a client. */
static void dispatch(struct client *c, uint32_t object, uint32_t opcode,
                     const uint8_t *args, uint32_t len)
{
    if (object == WL_DISPLAY_ID) {
        switch (opcode) {
        case WL_DISPLAY_SYNC:
            handle_sync(c, args, len);
            return;
        case WL_DISPLAY_GET_REGISTRY:
            handle_get_registry(c, args, len);
            return;
        default:
            logf("  wl_display has no request %u", opcode);
            send_error(c, object, WL_DISPLAY_ERROR_INVALID_METHOD,
                       "wl_display has no such request");
            return;
        }
    }

    if (object == c->registry && opcode == WL_REGISTRY_BIND) {
        logf("  bind refused: this display server advertises nothing yet");
        send_error(c, object, WL_DISPLAY_ERROR_INVALID_OBJECT,
                   "this display server advertises no globals yet");
        return;
    }

    logf("  request %u on unknown object %u", opcode, object);
    send_error(c, object, WL_DISPLAY_ERROR_INVALID_OBJECT,
               "no such object");
}

/* Take whole messages out of a client's buffer, leaving any partial one for
 * the next time round. */
static void consume(struct client *c)
{
    uint32_t at = 0;

    while (c->have - at >= 8) {
        const uint8_t *m = c->buf + at;

        uint32_t object = get32(m);
        uint32_t word   = get32(m + 4);
        uint32_t opcode = word & 0xFFFF;
        uint32_t size   = word >> 16;

        if (size < 8 || size > BUF_SIZE) {
            logf("  malformed message (size %u); dropping the client", size);
            wclose(c->fd);
            c->in_use = 0;
            return;
        }
        if (c->have - at < size)
            break;                       /* the rest has not arrived */

        dispatch(c, object, opcode, m + 8, size - 8);
        at += size;
    }

    if (at && at < c->have)
        memmove(c->buf, c->buf + at, c->have - at);
    c->have -= at;
}

/* ------------------------------------------------------------------ *
 *  The server
 * ------------------------------------------------------------------ */

static struct client *free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].in_use)
            return &clients[i];
    return NULL;
}

static void accept_client(int listener)
{
    int fd = waccept(listener);
    if (fd < 0) {
        logf("accept failed: %s", wstrerror(-fd));
        return;
    }

    struct client *c = free_slot();
    if (!c) {
        logf("refusing a client: all %d slots are in use", MAX_CLIENTS);
        wclose(fd);
        return;
    }

    memset(c, 0, sizeof(*c));
    c->fd     = fd;
    c->in_use = 1;
    logf("client connected on descriptor %d", fd);
}

static void read_client(struct client *c)
{
    if (c->have >= BUF_SIZE) {
        logf("client %d sent more than a buffer without a message in it", c->fd);
        wclose(c->fd);
        c->in_use = 0;
        return;
    }

    wmsg_t msg = { c->buf + c->have, BUF_SIZE - c->have, 0, 0 };
    int    n   = wrecv(c->fd, &msg);

    if (n <= 0) {
        logf("client %d disconnected", c->fd);
        wclose(c->fd);
        c->in_use = 0;
        return;
    }

    c->have += (uint32_t)n;
    consume(c);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv[1][0] == '/') ? argv[1] : SOCKET_PATH;

    log_fd = wopen(LOG_PATH, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);

    int listener = wlisten(path);
    if (listener < 0) {
        logf("cannot listen on %s: %s", path, wstrerror(-listener));
        wfprintf(W_STDERR, "waylandd: cannot listen on %s: %s\n", path,
                 wstrerror(-listener));
        return 1;
    }

    logf("waylandd listening on %s", path);

    for (;;) {
        wpollfd_t watch[MAX_CLIENTS + 1];
        int       n = 0;

        watch[n].fd     = listener;
        watch[n].events = W_POLLIN;
        n++;

        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].in_use) {
                watch[n].fd     = clients[i].fd;
                watch[n].events = W_POLLIN;
                n++;
            }

        /* A timeout rather than an indefinite wait, so that a service asked to
         * stop leaves within a second even with nothing connected. */
        if (wpoll(watch, n, 1000) <= 0)
            continue;

        if (watch[0].revents & W_POLLIN)
            accept_client(listener);

        for (int i = 1; i < n; i++) {
            if (!(watch[i].revents & (W_POLLIN | W_POLLHUP)))
                continue;

            for (int j = 0; j < MAX_CLIENTS; j++)
                if (clients[j].in_use && clients[j].fd == watch[i].fd) {
                    read_client(&clients[j]);
                    break;
                }
        }
    }
}
