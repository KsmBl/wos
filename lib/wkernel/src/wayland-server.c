/* The server half of the protocol library.  See wayland-server.h.
 *
 * A request arrives as an id, an opcode and a body.  The id names one of the
 * client's objects, which knows its interface; the interface's request list
 * turns the opcode into a signature and the signature turns the body into
 * arguments; the object's implementation turns the opcode into a handler.
 * None of that is specific to any interface, which is why a compositor here
 * adds one by describing it rather than by writing a parser.
 *
 * The handler is called through one wide function type, for the reason set out
 * at the top of wayland-client.c: every Wayland argument is an integer or a
 * pointer, and those travel identically on x86-64, so a handler that declares
 * fewer parameters simply ignores the rest.
 *
 * Two objects exist for every client without being asked for.  Object 1 is
 * wl_display, and it is handled here rather than by the compositor -- sync and
 * get_registry mean the same thing in every compositor there has ever been.
 * The registry it hands out is handled here too, up to the point where a bind
 * is a question only the compositor can answer.
 */

#include "wayland-server.h"
#include "wayland-wire.h"
#include <stdarg.h>

#define MAX_CLIENTS 16
#define MAX_GLOBALS 32

struct wl_resource {
    struct wl_client          *client;
    const struct wl_interface *interface;
    uint32_t                   id;
    uint32_t                   version;
    const void                *implementation;
    void                      *user_data;
    wl_resource_destroy_func_t destroy;
    struct wl_resource        *next;
};

struct wl_client {
    struct wl_display    *display;
    struct wl_connection *conn;
    struct wl_resource   *objects;
    struct wl_resource   *display_resource;
    void                 *user_data;
    int                   in_use;

    /* Ids the server picks for objects the client did not ask for.  The
     * protocol reserves everything from 0xff000000 up for exactly this, so the
     * two sides can both invent ids without ever colliding. */
    uint32_t              server_id;
};

struct wl_global {
    struct wl_display         *display;
    const struct wl_interface *interface;
    int                        version;
    void                      *data;
    wl_global_bind_func_t      bind;
    uint32_t                   name;
    int                        in_use;
};

struct wl_display {
    int              listen_fd;
    char             path[W_PATH_MAX + 1];

    struct wl_client clients[MAX_CLIENTS];
    struct wl_global globals[MAX_GLOBALS];

    uint32_t         serial;
    uint32_t         next_global_name;

    wl_client_func_t on_create;
    wl_client_func_t on_destroy;
    void            *client_data;
};

typedef void (*wl_request_t)(void *, void *, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#define SERVER_ID_BASE 0xff000000u

/* ------------------------------------------------------------------ *
 *  The display
 * ------------------------------------------------------------------ */

struct wl_display *wl_display_create(void)
{
    struct wl_display *d = malloc(sizeof(*d));
    if (!d)
        return NULL;

    memset(d, 0, sizeof(*d));
    d->listen_fd        = -1;
    d->next_global_name = 1;
    return d;
}

int wl_display_add_socket(struct wl_display *d, const char *name)
{
    if (!name)
        name = "wayland-0";

    if (name[0] == '/')
        strlcpy(d->path, name, sizeof(d->path));
    else
        wsnprintf(d->path, sizeof(d->path), "%s/%s", "/ramdisk", name);

    int fd = wlisten(d->path);
    if (fd < 0)
        return fd;

    d->listen_fd = fd;
    return 0;
}

uint32_t wl_display_next_serial(struct wl_display *d)
{
    return ++d->serial;
}

void wl_display_set_client_callbacks(struct wl_display *d,
                                     wl_client_func_t created,
                                     wl_client_func_t destroyed, void *data)
{
    d->on_create   = created;
    d->on_destroy  = destroyed;
    d->client_data = data;
}

/* ------------------------------------------------------------------ *
 *  Resources
 * ------------------------------------------------------------------ */

struct wl_resource *wl_client_get_object(struct wl_client *c, uint32_t id)
{
    for (struct wl_resource *r = c->objects; r; r = r->next)
        if (r->id == id)
            return r;
    return NULL;
}

struct wl_resource *wl_resource_create(struct wl_client *client,
                                       const struct wl_interface *interface,
                                       int version, uint32_t id)
{
    if (id == 0)
        id = client->server_id++;

    /* A client that asks for an id it is already using has broken the
     * protocol.  Letting it through would leave two objects answering to one
     * number, and every later message about either of them ambiguous. */
    if (wl_client_get_object(client, id))
        return NULL;

    struct wl_resource *r = malloc(sizeof(*r));
    if (!r)
        return NULL;

    memset(r, 0, sizeof(*r));
    r->client    = client;
    r->interface = interface;
    r->id        = id;
    r->version   = (uint32_t)version;

    r->next          = client->objects;
    client->objects  = r;
    return r;
}

void wl_resource_set_implementation(struct wl_resource *r,
                                    const void *implementation, void *data,
                                    wl_resource_destroy_func_t destroy)
{
    r->implementation = implementation;
    r->user_data      = data;
    r->destroy        = destroy;
}

uint32_t wl_resource_get_id(struct wl_resource *r)      { return r->id; }
uint32_t wl_resource_get_version(struct wl_resource *r) { return r->version; }
void *wl_resource_get_user_data(struct wl_resource *r)  { return r->user_data; }
void wl_resource_set_user_data(struct wl_resource *r, void *d) { r->user_data = d; }
struct wl_client *wl_resource_get_client(struct wl_resource *r) { return r->client; }

const struct wl_interface *wl_resource_get_interface(struct wl_resource *r)
{
    return r->interface;
}

int wl_resource_instance_of(struct wl_resource *r,
                            const struct wl_interface *interface)
{
    return r && r->interface == interface;
}

void wl_resource_post_event(struct wl_resource *r, uint32_t opcode, ...)
{
    if (!r || opcode >= (uint32_t)r->interface->event_count)
        return;

    const struct wl_message *msg = &r->interface->events[opcode];
    union wl_argument        args[WL_MAX_ARGS];
    const char              *s   = msg->signature;
    int                      n   = 0;

    memset(args, 0, sizeof(args));

    while (*s >= '0' && *s <= '9')
        s++;

    va_list ap;
    va_start(ap, opcode);

    for (; *s; s++) {
        if (*s == '?')
            continue;
        if (n >= WL_MAX_ARGS)
            break;

        switch (*s) {
        case 'i': args[n].i = va_arg(ap, int32_t);            break;
        case 'u': args[n].u = va_arg(ap, uint32_t);           break;
        case 'f': args[n].f = va_arg(ap, wl_fixed_t);         break;
        case 's': args[n].s = va_arg(ap, const char *);       break;
        case 'a': args[n].a = va_arg(ap, struct wl_array *);  break;
        case 'h': args[n].h = va_arg(ap, int32_t);            break;
        case 'n': args[n].n = va_arg(ap, uint32_t);           break;

        case 'o': {
            /* Events name objects by resource; the wire wants the id. */
            struct wl_resource *o = va_arg(ap, struct wl_resource *);
            args[n].u = o ? o->id : 0;
            break;
        }

        default:
            break;
        }
        n++;
    }

    va_end(ap);

    wl_connection_queue(r->client->conn, r->id, opcode, msg, args);
}

void wl_resource_post_error(struct wl_resource *r, uint32_t code,
                            const char *fmt, ...)
{
    char    text[192];
    va_list ap;

    va_start(ap, fmt);
    wvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    struct wl_client *c = r->client;

    if (c->display_resource)
        wl_resource_post_event(c->display_resource, WL_DISPLAY_ERROR,
                               r, code, text);

    /* A protocol error is fatal by definition: the two sides no longer agree
     * about what exists, and nothing after it can be trusted.  Flush first, so
     * the client is told why before the connection goes. */
    wl_client_flush(c);
    wl_connection_break(c->conn);
}

void wl_resource_destroy(struct wl_resource *r)
{
    if (!r)
        return;

    struct wl_client *c  = r->client;
    uint32_t          id = r->id;

    if (r->destroy)
        r->destroy(r);

    for (struct wl_resource **at = &c->objects; *at; at = &(*at)->next)
        if (*at == r) {
            *at = r->next;
            break;
        }

    free(r);

    /* Only ids the client chose are its to reuse, so only those are worth
     * telling it about. */
    if (id < SERVER_ID_BASE && c->display_resource && c->in_use)
        wl_resource_post_event(c->display_resource, WL_DISPLAY_DELETE_ID, id);
}

/* ------------------------------------------------------------------ *
 *  Globals
 * ------------------------------------------------------------------ */

/* Tell one client about one global.  Sent when a client asks for the registry,
 * and again to every registry when a global appears later. */
static void announce(struct wl_client *c, struct wl_global *g);

struct wl_global *wl_global_create(struct wl_display *d,
                                   const struct wl_interface *interface,
                                   int version, void *data,
                                   wl_global_bind_func_t bind)
{
    for (int i = 0; i < MAX_GLOBALS; i++) {
        struct wl_global *g = &d->globals[i];
        if (g->in_use)
            continue;

        g->display   = d;
        g->interface = interface;
        g->version   = version;
        g->data      = data;
        g->bind      = bind;
        g->name      = d->next_global_name++;
        g->in_use    = 1;

        /* Anyone already holding a registry has to hear about it. */
        for (int j = 0; j < MAX_CLIENTS; j++)
            if (d->clients[j].in_use)
                announce(&d->clients[j], g);

        return g;
    }

    return NULL;
}

void wl_global_destroy(struct wl_global *g)
{
    if (!g || !g->in_use)
        return;

    struct wl_display *d = g->display;

    for (int j = 0; j < MAX_CLIENTS; j++) {
        struct wl_client *c = &d->clients[j];
        if (!c->in_use)
            continue;

        for (struct wl_resource *r = c->objects; r; r = r->next)
            if (r->interface == &wl_registry_interface)
                wl_resource_post_event(r, WL_REGISTRY_GLOBAL_REMOVE, g->name);
    }

    g->in_use = 0;
}

void *wl_global_get_user_data(struct wl_global *g) { return g->data; }

static void announce(struct wl_client *c, struct wl_global *g)
{
    for (struct wl_resource *r = c->objects; r; r = r->next)
        if (r->interface == &wl_registry_interface)
            wl_resource_post_event(r, WL_REGISTRY_GLOBAL, g->name,
                                   g->interface->name, (uint32_t)g->version);
}

/* ------------------------------------------------------------------ *
 *  wl_display and wl_registry, which every compositor implements alike
 * ------------------------------------------------------------------ */

static void registry_bind(struct wl_client *c, struct wl_resource *registry,
                          uint32_t name, const char *interface,
                          uint32_t version, uint32_t id)
{
    struct wl_display *d = c->display;

    for (int i = 0; i < MAX_GLOBALS; i++) {
        struct wl_global *g = &d->globals[i];

        if (!g->in_use || g->name != name)
            continue;

        /* The client names the interface as well as the number, so the two can
         * be checked against each other.  A mismatch means the client and the
         * compositor disagree about what this global is, and everything after
         * it would be nonsense. */
        if (interface && strcmp(interface, g->interface->name) != 0) {
            wl_resource_post_error(registry, WL_DISPLAY_ERROR_INVALID_OBJECT,
                                   "global %u is %s, not %s", name,
                                   g->interface->name, interface);
            return;
        }

        if (version > (uint32_t)g->version)
            version = (uint32_t)g->version;

        g->bind(c, g->data, version, id);
        return;
    }

    wl_resource_post_error(registry, WL_DISPLAY_ERROR_INVALID_OBJECT,
                           "there is no global %u", name);
}

static const struct {
    void (*bind)(struct wl_client *, struct wl_resource *, uint32_t,
                 const char *, uint32_t, uint32_t);
} registry_implementation = { registry_bind };

static void display_sync(struct wl_client *c, struct wl_resource *display,
                         uint32_t id)
{
    struct wl_resource *callback =
        wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!callback)
        return;

    /* Done immediately, because a sync means "when you have dealt with
     * everything I sent before this" and everything before it has just been
     * dealt with -- the requests came in order and were handled in order. */
    wl_resource_post_event(callback, WL_CALLBACK_DONE,
                           wl_display_next_serial(c->display));
    wl_resource_destroy(callback);
    (void)display;
}

static void display_get_registry(struct wl_client *c,
                                 struct wl_resource *display, uint32_t id)
{
    struct wl_resource *registry =
        wl_resource_create(c, &wl_registry_interface, 1, id);
    if (!registry)
        return;

    wl_resource_set_implementation(registry, &registry_implementation, NULL,
                                   NULL);

    for (int i = 0; i < MAX_GLOBALS; i++)
        if (c->display->globals[i].in_use)
            wl_resource_post_event(registry, WL_REGISTRY_GLOBAL,
                                   c->display->globals[i].name,
                                   c->display->globals[i].interface->name,
                                   (uint32_t)c->display->globals[i].version);

    (void)display;
}

static const struct {
    void (*sync)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*get_registry)(struct wl_client *, struct wl_resource *, uint32_t);
} display_implementation = { display_sync, display_get_registry };

/* ------------------------------------------------------------------ *
 *  Clients
 * ------------------------------------------------------------------ */

int wl_client_get_fd(struct wl_client *c) { return wl_connection_fd(c->conn); }
struct wl_display *wl_client_get_display(struct wl_client *c) { return c->display; }
void *wl_client_get_user_data(struct wl_client *c) { return c->user_data; }
void wl_client_set_user_data(struct wl_client *c, void *d) { c->user_data = d; }

void wl_client_flush(struct wl_client *c)
{
    if (c->in_use)
        wl_connection_flush(c->conn);
}

void wl_client_destroy(struct wl_client *c)
{
    if (!c->in_use)
        return;

    if (c->display->on_destroy)
        c->display->on_destroy(c, c->display->client_data);

    /* Cleared first, so the destroy handlers below do not try to send this
     * client anything on the way out. */
    c->in_use = 0;

    struct wl_resource *r = c->objects;
    while (r) {
        struct wl_resource *next = r->next;
        if (r->destroy)
            r->destroy(r);
        free(r);
        r = next;
    }
    c->objects          = NULL;
    c->display_resource = NULL;

    wl_connection_destroy(c->conn);
    c->conn = NULL;
}

static struct wl_client *client_create(struct wl_display *d, int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct wl_client *c = &d->clients[i];
        if (c->in_use)
            continue;

        memset(c, 0, sizeof(*c));
        c->display   = d;
        c->conn      = wl_connection_create(fd);
        if (!c->conn)
            return NULL;

        c->in_use    = 1;
        c->server_id = SERVER_ID_BASE;

        /* Object 1 exists before the client says anything. */
        c->display_resource =
            wl_resource_create(c, &wl_display_interface, 1, 1);
        if (c->display_resource)
            wl_resource_set_implementation(c->display_resource,
                                           &display_implementation, NULL, NULL);

        if (d->on_create)
            d->on_create(c, d->client_data);

        return c;
    }

    return NULL;
}

/* ------------------------------------------------------------------ *
 *  Dispatching requests
 * ------------------------------------------------------------------ */

static int dispatch_client(struct wl_client *c)
{
    struct wl_msg_view view;
    int                handled = 0;

    while (c->in_use && wl_connection_next(c->conn, &view) == 1) {
        struct wl_resource *target = wl_client_get_object(c, view.id);

        if (!target) {
            /* Unlike the client side, this is not a race the server can be
             * relaxed about: a server only destroys an object at the client's
             * request, so an id it does not know is one the client invented. */
            if (c->display_resource)
                wl_resource_post_error(c->display_resource,
                                       WL_DISPLAY_ERROR_INVALID_OBJECT,
                                       "no object with id %u", view.id);
            return handled;
        }

        if (view.opcode >= (uint32_t)target->interface->method_count) {
            wl_resource_post_error(target, WL_DISPLAY_ERROR_INVALID_METHOD,
                                   "%s has no request %u",
                                   target->interface->name, view.opcode);
            return handled;
        }

        const struct wl_message *msg = &target->interface->methods[view.opcode];
        union wl_argument        args[WL_MAX_ARGS];

        memset(args, 0, sizeof(args));
        int n = wl_connection_unpack(c->conn, msg->signature, view.body,
                                     view.body_len, args, WL_MAX_ARGS);
        if (n < 0) {
            wl_resource_post_error(target, WL_DISPLAY_ERROR_INVALID_METHOD,
                                   "%s.%s does not look like that",
                                   target->interface->name, msg->name);
            return handled;
        }

        void (**impl)(void) = (void (**)(void))target->implementation;
        void (*fn)(void)    = impl ? impl[view.opcode] : NULL;

        if (!fn) {
            /* Nothing implements it.  Saying so is important: a client that
             * asked for an object and was neither given one nor told why would
             * wait for it forever. */
            wl_resource_post_error(target, WL_DISPLAY_ERROR_IMPLEMENTATION,
                                   "%s.%s is not implemented here",
                                   target->interface->name, msg->name);
            return handled;
        }

        uint64_t    slot[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        const char *s       = msg->signature;
        int         k       = 0;

        while (*s >= '0' && *s <= '9')
            s++;

        for (; *s && k < 8; s++) {
            if (*s == '?')
                continue;

            switch (*s) {
            case 'o': {
                struct wl_resource *o =
                    args[k].u ? wl_client_get_object(c, args[k].u) : NULL;
                slot[k] = (uint64_t)(unsigned long)o;
                break;
            }
            case 's': slot[k] = (uint64_t)(unsigned long)args[k].s; break;
            case 'a': slot[k] = (uint64_t)(unsigned long)args[k].a; break;
            default:  slot[k] = args[k].u;                          break;
            }
            k++;
        }

        ((wl_request_t)fn)(c, target, slot[0], slot[1], slot[2], slot[3],
                           slot[4], slot[5], slot[6], slot[7]);

        /* After the handler: strings point into the buffer this frees. */
        if (c->in_use)
            wl_connection_consume(c->conn, view.size);
        handled++;
    }

    return handled;
}

/* ------------------------------------------------------------------ *
 *  The loop the compositor drives
 * ------------------------------------------------------------------ */

int wl_display_poll_fds(struct wl_display *d, wpollfd_t *out, int max)
{
    int n = 0;

    if (d->listen_fd >= 0 && n < max) {
        out[n].fd      = d->listen_fd;
        out[n].events  = W_POLLIN;
        out[n].revents = 0;
        n++;
    }

    for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
        struct wl_client *c = &d->clients[i];
        if (!c->in_use)
            continue;

        out[n].fd      = wl_connection_fd(c->conn);
        out[n].events  = W_POLLIN;
        if (wl_connection_pending(c->conn))
            out[n].events |= W_POLLOUT;
        out[n].revents = 0;
        n++;
    }

    return n;
}

int wl_display_handle(struct wl_display *d, const wpollfd_t *fds, int count)
{
    int handled = 0;

    for (int i = 0; i < count; i++) {
        if (fds[i].fd != d->listen_fd || !(fds[i].revents & W_POLLIN))
            continue;

        int fd = waccept(d->listen_fd);
        if (fd < 0)
            continue;
        if (!client_create(d, fd))
            wclose(fd);              /* full: better refused than half-served */
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct wl_client *c = &d->clients[i];
        if (!c->in_use)
            continue;

        int  fd       = wl_connection_fd(c->conn);
        int  readable = 0;
        int  gone     = 0;

        for (int j = 0; j < count; j++) {
            if (fds[j].fd != fd)
                continue;
            if (fds[j].revents & W_POLLIN)
                readable = 1;
            if (fds[j].revents & W_POLLHUP)
                gone = 1;
        }

        if (readable) {
            if (wl_connection_read(c->conn) > 0)
                handled += dispatch_client(c);
        }

        if (!c->in_use)
            continue;

        wl_connection_flush(c->conn);

        if (wl_connection_broken(c->conn) || (gone && !readable))
            wl_client_destroy(c);
    }

    return handled;
}

void wl_display_flush_clients(struct wl_display *d)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (d->clients[i].in_use)
            wl_connection_flush(d->clients[i].conn);
}

void wl_display_destroy(struct wl_display *d)
{
    if (!d)
        return;

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (d->clients[i].in_use)
            wl_client_destroy(&d->clients[i]);

    if (d->listen_fd >= 0)
        wclose(d->listen_fd);

    free(d);
}
