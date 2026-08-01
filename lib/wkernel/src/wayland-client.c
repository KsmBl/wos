/* The client half of the protocol library.  See wayland-client.h.
 *
 * The interesting problem here is the one libwayland solves with libffi.  An
 * event arrives as a signature and a list of arguments; the handler for it is
 * a function whose shape is different for every event.  Something has to call
 * a function of unknown shape with a list of values, and there is no libffi.
 *
 * There does not need to be.  Every Wayland argument is a 32-bit integer or a
 * pointer, and the x86-64 calling convention passes both of those the same
 * way: first in registers, then on the stack, with the caller cleaning up.  So
 * a handler can be called through one function type wide enough for the
 * longest event, with the unused slots left as whatever they are -- the callee
 * declares fewer parameters and never looks at them.  wl_output.geometry, with
 * eight arguments, is the one that sets the width.
 *
 * That would break for a floating-point argument, which travels in a different
 * set of registers.  The protocol has none: wl_fixed_t is an integer, which is
 * exactly why the protocol chose it.
 */

#include "wayland-client.h"
#include "wayland-wire.h"
#include <stdarg.h>

struct wl_proxy {
    struct wl_display        *display;
    const struct wl_interface *interface;
    uint32_t                  id;
    uint32_t                  version;
    void                    (**listener)(void);
    void                     *user_data;
    struct wl_proxy          *next;
};

struct wl_display {
    struct wl_proxy       proxy;      /* object 1, and first so a cast works */
    struct wl_connection *conn;
    struct wl_proxy      *objects;
    uint32_t              next_id;
    int                   error;
    int                   dispatched;
};

/* The widest shape a handler can have: data, the object, and eight arguments.
 * See the note at the top of the file. */
typedef void (*wl_handler_t)(void *, void *, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* ------------------------------------------------------------------ *
 *  Objects
 * ------------------------------------------------------------------ */

/* Found by walking the list.  A client holds a handful of objects -- a
 * surface, a buffer or two, a keyboard -- so a list is not worth improving on,
 * and it keeps a destroyed proxy from leaving a hole for a later id to fall
 * into. */
static struct wl_proxy *find_object(struct wl_display *d, uint32_t id)
{
    for (struct wl_proxy *p = d->objects; p; p = p->next)
        if (p->id == id)
            return p;
    return NULL;
}

static struct wl_proxy *proxy_create(struct wl_display *d,
                                     const struct wl_interface *iface,
                                     uint32_t id, uint32_t version)
{
    struct wl_proxy *p = malloc(sizeof(*p));
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    p->display   = d;
    p->interface = iface;
    p->id        = id;
    p->version   = version;

    p->next    = d->objects;
    d->objects = p;
    return p;
}

void wl_proxy_destroy(struct wl_proxy *proxy)
{
    if (!proxy || proxy->id == 1)
        return;                       /* wl_display goes with the display */

    struct wl_display *d = proxy->display;

    for (struct wl_proxy **at = &d->objects; *at; at = &(*at)->next)
        if (*at == proxy) {
            *at = proxy->next;
            break;
        }

    free(proxy);
}

uint32_t wl_proxy_get_id(struct wl_proxy *proxy)      { return proxy->id; }
uint32_t wl_proxy_get_version(struct wl_proxy *proxy) { return proxy->version; }
void *wl_proxy_get_user_data(struct wl_proxy *proxy)  { return proxy->user_data; }
void wl_proxy_set_user_data(struct wl_proxy *p, void *d) { p->user_data = d; }

const char *wl_proxy_get_class(struct wl_proxy *proxy)
{
    return proxy->interface->name;
}

int wl_proxy_add_listener(struct wl_proxy *proxy,
                          void (**implementation)(void), void *data)
{
    if (proxy->listener)
        return -1;                    /* upstream refuses a second one too */

    proxy->listener  = implementation;
    proxy->user_data = data;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Sending
 * ------------------------------------------------------------------ */

/* Read one request's arguments out of a varargs list, following the
 * signature.  The 'n' slot is consumed and discarded: upstream's generated
 * code passes NULL there, and the id is ours to choose rather than the
 * caller's to supply. */
static int collect_args(const char *signature, va_list ap,
                        union wl_argument *args, uint32_t new_id)
{
    const char *s = signature;
    int         n = 0;

    while (*s >= '0' && *s <= '9')
        s++;

    for (; *s; s++) {
        if (*s == '?')
            continue;
        if (n >= WL_MAX_ARGS)
            return -1;

        switch (*s) {
        case 'i': args[n].i = va_arg(ap, int32_t);  break;
        case 'u': args[n].u = va_arg(ap, uint32_t); break;
        case 'f': args[n].f = va_arg(ap, wl_fixed_t); break;
        case 's': args[n].s = va_arg(ap, const char *); break;
        case 'a': args[n].a = va_arg(ap, struct wl_array *); break;
        case 'h': args[n].h = va_arg(ap, int32_t); break;

        case 'o': {
            struct wl_proxy *o = va_arg(ap, struct wl_proxy *);
            args[n].u = o ? o->id : 0;
            break;
        }

        case 'n':
            (void)va_arg(ap, void *);
            args[n].n = new_id;
            break;

        default:
            return -1;
        }
        n++;
    }

    return n;
}

static void marshal(struct wl_proxy *proxy, uint32_t opcode, uint32_t new_id,
                    va_list ap)
{
    struct wl_display *d = proxy->display;

    if (opcode >= (uint32_t)proxy->interface->method_count)
        return;

    const struct wl_message *msg = &proxy->interface->methods[opcode];
    union wl_argument        args[WL_MAX_ARGS];

    memset(args, 0, sizeof(args));
    if (collect_args(msg->signature, ap, args, new_id) < 0)
        return;

    wl_connection_queue(d->conn, proxy->id, opcode, msg, args);
}

void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...)
{
    va_list ap;
    va_start(ap, opcode);
    marshal(proxy, opcode, 0, ap);
    va_end(ap);
}

struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *proxy,
                                              uint32_t opcode,
                                              const struct wl_interface *iface,
                                              ...)
{
    struct wl_display *d  = proxy->display;
    struct wl_proxy   *np = proxy_create(d, iface, d->next_id++, proxy->version);
    if (!np)
        return NULL;

    va_list ap;
    va_start(ap, iface);
    marshal(proxy, opcode, np->id, ap);
    va_end(ap);

    return np;
}

struct wl_proxy *wl_proxy_marshal_constructor_versioned(
        struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *iface, uint32_t version, ...)
{
    struct wl_display *d  = proxy->display;
    struct wl_proxy   *np = proxy_create(d, iface, d->next_id++, version);
    if (!np)
        return NULL;

    va_list ap;
    va_start(ap, version);
    marshal(proxy, opcode, np->id, ap);
    va_end(ap);

    return np;
}

/* ------------------------------------------------------------------ *
 *  Receiving
 * ------------------------------------------------------------------ */

/* Turn the object arguments of an event into proxies, and hand the lot to the
 * listener.  Everything is widened to 64 bits because that is the size of the
 * registers the arguments travel in. */
static void deliver(struct wl_display *d, struct wl_proxy *target,
                    const struct wl_message *msg, uint32_t opcode,
                    union wl_argument *args, int count)
{
    if (!target->listener)
        return;

    void (*fn)(void) = target->listener[opcode];
    if (!fn)
        return;                       /* a handler the client left out */

    uint64_t    slot[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const char *s       = msg->signature;
    int         n       = 0;

    while (*s >= '0' && *s <= '9')
        s++;

    for (; *s && n < 8; s++) {
        if (*s == '?')
            continue;

        switch (*s) {
        case 'o': {
            /* An id on the wire; the object it names, to the listener. */
            struct wl_proxy *o = args[n].u ? find_object(d, args[n].u) : NULL;
            slot[n] = (uint64_t)(unsigned long)o;
            break;
        }
        case 'n': {
            /* An object the *server* created.  Nothing in the protocols here
             * does this, but a client is entitled to be handed one, and
             * silently dropping it would leave the two sides disagreeing about
             * what exists. */
            const struct wl_interface *iface =
                msg->types ? msg->types[n] : NULL;
            struct wl_proxy *o = iface
                ? proxy_create(d, iface, args[n].u, target->version) : NULL;
            slot[n] = (uint64_t)(unsigned long)o;
            break;
        }
        case 's': slot[n] = (uint64_t)(unsigned long)args[n].s; break;
        case 'a': slot[n] = (uint64_t)(unsigned long)args[n].a; break;
        default:  slot[n] = args[n].u;                      break;
        }
        n++;
    }

    (void)count;

    ((wl_handler_t)fn)(target->user_data, target, slot[0], slot[1], slot[2],
                       slot[3], slot[4], slot[5], slot[6], slot[7]);
}

/* wl_display's own two events are the library's business before they are the
 * client's: an error breaks the connection, and delete_id is what makes an id
 * safe to forget. */
static void handle_display_event(struct wl_display *d, uint32_t opcode,
                                 union wl_argument *args)
{
    if (opcode == WL_DISPLAY_ERROR) {
        d->error = (int)args[1].u;
        wl_connection_break(d->conn);
    } else if (opcode == WL_DISPLAY_DELETE_ID) {
        struct wl_proxy *p = find_object(d, args[0].u);
        if (p)
            wl_proxy_destroy(p);
    }
}

static int dispatch_queued(struct wl_display *d)
{
    struct wl_msg_view view;
    int                delivered = 0;

    while (wl_connection_next(d->conn, &view) == 1) {
        struct wl_proxy *target = find_object(d, view.id);

        if (!target) {
            /* An event for an object we have already destroyed.  Normal: the
             * two sides pass each other, and the server has not seen the
             * destroy yet.  Skipping it is what upstream does. */
            wl_connection_consume(d->conn, view.size);
            continue;
        }

        if (view.opcode >= (uint32_t)target->interface->event_count) {
            wl_connection_break(d->conn);
            return -1;
        }

        const struct wl_message *msg = &target->interface->events[view.opcode];
        union wl_argument        args[WL_MAX_ARGS];

        memset(args, 0, sizeof(args));
        int n = wl_connection_unpack(d->conn, msg->signature, view.body,
                                     view.body_len, args, WL_MAX_ARGS);
        if (n < 0) {
            wl_connection_break(d->conn);
            return -1;
        }

        if (target == &d->proxy)
            handle_display_event(d, view.opcode, args);

        deliver(d, target, msg, view.opcode, args, n);

        /* After the listener, because strings point into the buffer being
         * consumed. */
        wl_connection_consume(d->conn, view.size);
        delivered++;
    }

    return delivered;
}

int wl_display_dispatch_pending(struct wl_display *d)
{
    return dispatch_queued(d);
}

int wl_display_dispatch(struct wl_display *d)
{
    int n = dispatch_queued(d);
    if (n != 0)
        return n;

    if (wl_display_flush(d) < 0)
        return -1;
    if (wl_connection_read(d->conn) <= 0)
        return -1;

    return dispatch_queued(d);
}

int wl_display_flush(struct wl_display *d)
{
    return wl_connection_flush(d->conn);
}

int wl_display_get_fd(struct wl_display *d)
{
    return wl_connection_fd(d->conn);
}

int wl_display_get_error(struct wl_display *d)
{
    return d->error;
}

/* ------------------------------------------------------------------ *
 *  Roundtrip
 * ------------------------------------------------------------------ */

static void roundtrip_done(void *data, struct wl_callback *callback,
                           uint32_t serial)
{
    (void)callback;
    (void)serial;
    *(int *)data = 1;
}

static const struct wl_callback_listener roundtrip_listener = {
    roundtrip_done,
};

int wl_display_roundtrip(struct wl_display *d)
{
    int done  = 0;
    int total = 0;

    struct wl_callback *cb = wl_display_sync(d);
    if (!cb)
        return -1;

    wl_callback_add_listener(cb, &roundtrip_listener, &done);

    while (!done) {
        int n = wl_display_dispatch(d);
        if (n < 0)
            return -1;
        total += n;
    }

    return total;
}

/* ------------------------------------------------------------------ *
 *  Connecting
 * ------------------------------------------------------------------ */

/* Why the last wl_display_connect() failed.
 *
 * Upstream this is errno, which WOS has not got: a syscall here returns the
 * reason and the caller reads it.  wl_display_connect() returns a pointer and
 * has nowhere to put one, so it is left here -- and it matters now that a
 * connection can be refused for a reason other than nobody listening.  A
 * client that told somebody to start a display server when the truth is that
 * the one running belongs to another user would be sending them the wrong
 * way. */
static int connect_error;

int wl_display_connect_error(void)
{
    return connect_error;
}

struct wl_display *wl_display_connect(const char *name)
{
    char path[W_PATH_MAX + 1];

    if (!name)
        name = WL_DEFAULT_DISPLAY;

    /* A name, not a path -- unless it is one, which is what an absolute name
     * means upstream as well. */
    if (name[0] == '/')
        strlcpy(path, name, sizeof(path));
    else
        wsnprintf(path, sizeof(path), "%s/%s", WL_RUNTIME_DIR, name);

    int fd = wconnect(path);
    if (fd < 0) {
        connect_error = fd;
        return NULL;
    }

    connect_error = 0;

    struct wl_display *d = malloc(sizeof(*d));
    if (!d) {
        wclose(fd);
        return NULL;
    }

    memset(d, 0, sizeof(*d));
    d->conn = wl_connection_create(fd);
    if (!d->conn) {
        wclose(fd);
        free(d);
        return NULL;
    }

    /* Object 1 is wl_display and exists on both sides before anything is
     * asked for, so it is not created, only described.  Ids the client
     * chooses start at 2. */
    d->proxy.display   = d;
    d->proxy.interface = &wl_display_interface;
    d->proxy.id        = 1;
    d->proxy.version   = 1;
    d->objects         = &d->proxy;
    d->next_id         = 2;

    return d;
}

void wl_display_disconnect(struct wl_display *d)
{
    if (!d)
        return;

    struct wl_proxy *p = d->objects;
    while (p) {
        struct wl_proxy *next = p->next;
        if (p != &d->proxy)
            free(p);
        p = next;
    }

    wl_connection_destroy(d->conn);
    free(d);
}
