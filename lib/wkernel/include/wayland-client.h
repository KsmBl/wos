/* wayland-client.h -- writing a Wayland client.
 *
 * The names, the shapes and the order of arguments are libwayland's.  A client
 * written for a Linux desktop is written against these functions, so keeping
 * them identical is the difference between a protocol that is compatible and
 * one that merely resembles the real thing:
 *
 *     struct wl_display  *d = wl_display_connect(NULL);
 *     struct wl_registry *r = wl_display_get_registry(d);
 *     wl_registry_add_listener(r, &registry_listener, state);
 *     wl_display_roundtrip(d);
 *
 * is the opening of every Wayland client there has ever been, and it is the
 * opening of one here.
 *
 * Two things upstream has that this has not.  There is no wl_event_queue and
 * no wl_display_prepare_read(): those exist to let several threads dispatch
 * one connection, and WOS processes have one thread.  And wl_fixed_t has no
 * floating-point conversions, because the kernel never turns the FPU on.
 * Neither absence changes a single byte on the wire.
 */
#ifndef WAYLAND_CLIENT_H
#define WAYLAND_CLIENT_H

#include "wayland-util.h"
#include "wayland-protocol.h"

/* Where display servers answer.  Wayland names a display rather than a path --
 * "wayland-0" is the usual one -- and on Linux the name is looked up under
 * $XDG_RUNTIME_DIR.  WOS has no environment, so the runtime directory is a
 * fixed one: the filesystem held in memory, which is the right place for a
 * name that means nothing after a reboot. */
#define WL_RUNTIME_DIR   "/ramdisk"
#define WL_DEFAULT_DISPLAY "wayland-0"

/* Every object is one of these.  Upstream they are separate opaque types that
 * a proxy is cast to and from, and they are here too, for the same reason: it
 * makes wl_surface_attach(surface, buffer, 0, 0) fail to compile if `surface`
 * is a wl_output. */
struct wl_proxy;
struct wl_display;
struct wl_registry;
struct wl_callback;
struct wl_compositor;
struct wl_shm;
struct wl_shm_pool;
struct wl_buffer;
struct wl_surface;
struct wl_region;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_output;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

/* ==================================================================== *
 *  The connection
 * ==================================================================== */

/**
 * Connect to a display server.
 * @param name The display name, e.g. "wayland-0"; NULL means the default.
 * @return The display, or NULL if nothing is listening there.
 */
struct wl_display *wl_display_connect(const char *name);

void wl_display_disconnect(struct wl_display *display);

/** The socket, for a program that waits on it alongside its own descriptors. */
int wl_display_get_fd(struct wl_display *display);

/** Send everything queued. @return 0, or -1 if the server has gone. */
int wl_display_flush(struct wl_display *display);

/**
 * Wait for events and deliver them to listeners.
 * @return How many were dispatched, or -1 if the connection has broken.
 */
int wl_display_dispatch(struct wl_display *display);

/** Deliver what has already arrived, without waiting. */
int wl_display_dispatch_pending(struct wl_display *display);

/**
 * Send everything queued and wait until the server has dealt with all of it.
 *
 * This is wl_display.sync with the waiting done for you: the reply to a sync
 * cannot arrive before everything sent ahead of it has been handled, so its
 * arrival is the proof that the server is caught up.
 *
 * @return The number of events dispatched, or -1.
 */
int wl_display_roundtrip(struct wl_display *display);

/** The last protocol error the server reported, or 0. */
int wl_display_get_error(struct wl_display *display);

/* ==================================================================== *
 *  Proxies
 *
 *  The generic layer the typed calls below are built on.  A client rarely
 *  needs these directly, and a client porting code from upstream may.
 * ==================================================================== */

void      wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...);
struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *proxy,
                                              uint32_t opcode,
                                              const struct wl_interface *iface,
                                              ...);
struct wl_proxy *wl_proxy_marshal_constructor_versioned(
        struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *iface, uint32_t version, ...);

int   wl_proxy_add_listener(struct wl_proxy *proxy,
                            void (**implementation)(void), void *data);
void  wl_proxy_destroy(struct wl_proxy *proxy);
uint32_t wl_proxy_get_id(struct wl_proxy *proxy);
uint32_t wl_proxy_get_version(struct wl_proxy *proxy);
void *wl_proxy_get_user_data(struct wl_proxy *proxy);
void  wl_proxy_set_user_data(struct wl_proxy *proxy, void *data);
const char *wl_proxy_get_class(struct wl_proxy *proxy);

/* ==================================================================== *
 *  Listeners
 *
 *  One structure of function pointers per interface, in event order.  The
 *  order is the protocol's: the dispatcher finds the handler by the opcode's
 *  position, so a member out of place is a handler called for the wrong event.
 * ==================================================================== */

struct wl_display_listener {
    void (*error)(void *data, struct wl_display *display, void *object_id,
                  uint32_t code, const char *message);
    void (*delete_id)(void *data, struct wl_display *display, uint32_t id);
};

struct wl_registry_listener {
    void (*global)(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version);
    void (*global_remove)(void *data, struct wl_registry *registry,
                          uint32_t name);
};

struct wl_callback_listener {
    void (*done)(void *data, struct wl_callback *callback,
                 uint32_t callback_data);
};

struct wl_shm_listener {
    void (*format)(void *data, struct wl_shm *shm, uint32_t format);
};

struct wl_buffer_listener {
    void (*release)(void *data, struct wl_buffer *buffer);
};

struct wl_surface_listener {
    void (*enter)(void *data, struct wl_surface *surface,
                  struct wl_output *output);
    void (*leave)(void *data, struct wl_surface *surface,
                  struct wl_output *output);
};

struct wl_seat_listener {
    void (*capabilities)(void *data, struct wl_seat *seat,
                         uint32_t capabilities);
    void (*name)(void *data, struct wl_seat *seat, const char *name);
};

struct wl_keyboard_listener {
    void (*keymap)(void *data, struct wl_keyboard *keyboard, uint32_t format,
                   int32_t fd, uint32_t size);
    void (*enter)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface, struct wl_array *keys);
    void (*leave)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface);
    void (*key)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                uint32_t time, uint32_t key, uint32_t state);
    void (*modifiers)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                      uint32_t mods_depressed, uint32_t mods_latched,
                      uint32_t mods_locked, uint32_t group);
    void (*repeat_info)(void *data, struct wl_keyboard *keyboard, int32_t rate,
                        int32_t delay);
};

/* The pointer's events, in the order the protocol declares them.
 *
 * Coordinates are wl_fixed_t and surface-local: measured from this surface's
 * own corner, in 24.8 fixed point, so wl_fixed_to_int() is what turns one into
 * a pixel.  A compositor that draws its own decoration never includes it here,
 * so (0,0) is the client's first pixel and not the frame's.
 *
 * `axis` carries a distance rather than a count of notches, positive meaning
 * the content should move up -- the direction a finger would push it. */
struct wl_pointer_listener {
    void (*enter)(void *data, struct wl_pointer *pointer, uint32_t serial,
                  struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy);
    void (*leave)(void *data, struct wl_pointer *pointer, uint32_t serial,
                  struct wl_surface *surface);
    void (*motion)(void *data, struct wl_pointer *pointer, uint32_t time,
                   wl_fixed_t sx, wl_fixed_t sy);
    void (*button)(void *data, struct wl_pointer *pointer, uint32_t serial,
                   uint32_t time, uint32_t button, uint32_t state);
    void (*axis)(void *data, struct wl_pointer *pointer, uint32_t time,
                 uint32_t axis, wl_fixed_t value);
};

struct wl_output_listener {
    void (*geometry)(void *data, struct wl_output *output, int32_t x, int32_t y,
                     int32_t physical_width, int32_t physical_height,
                     int32_t subpixel, const char *make, const char *model,
                     int32_t transform);
    void (*mode)(void *data, struct wl_output *output, uint32_t flags,
                 int32_t width, int32_t height, int32_t refresh);
    void (*done)(void *data, struct wl_output *output);
    void (*scale)(void *data, struct wl_output *output, int32_t factor);
    void (*name)(void *data, struct wl_output *output, const char *name);
    void (*description)(void *data, struct wl_output *output,
                        const char *description);
};

struct xdg_wm_base_listener {
    void (*ping)(void *data, struct xdg_wm_base *base, uint32_t serial);
};

struct xdg_surface_listener {
    void (*configure)(void *data, struct xdg_surface *surface, uint32_t serial);
};

struct xdg_toplevel_listener {
    void (*configure)(void *data, struct xdg_toplevel *toplevel, int32_t width,
                      int32_t height, struct wl_array *states);
    void (*close)(void *data, struct xdg_toplevel *toplevel);
    void (*configure_bounds)(void *data, struct xdg_toplevel *toplevel,
                             int32_t width, int32_t height);
    void (*wm_capabilities)(void *data, struct xdg_toplevel *toplevel,
                            struct wl_array *capabilities);
};

/* ==================================================================== *
 *  The typed calls
 *
 *  What wayland-scanner generates upstream, written out.  Every one is a
 *  proxy call underneath; the types are the point of them.
 * ==================================================================== */

#define WL_PROXY(p) ((struct wl_proxy *)(p))

static inline int wl_registry_add_listener(struct wl_registry *registry,
        const struct wl_registry_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(registry),
                                 (void (**)(void))listener, data);
}

static inline int wl_callback_add_listener(struct wl_callback *callback,
        const struct wl_callback_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(callback),
                                 (void (**)(void))listener, data);
}

static inline int wl_shm_add_listener(struct wl_shm *shm,
        const struct wl_shm_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(shm), (void (**)(void))listener, data);
}

static inline int wl_buffer_add_listener(struct wl_buffer *buffer,
        const struct wl_buffer_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(buffer),
                                 (void (**)(void))listener, data);
}

static inline int wl_seat_add_listener(struct wl_seat *seat,
        const struct wl_seat_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(seat), (void (**)(void))listener, data);
}

static inline int wl_keyboard_add_listener(struct wl_keyboard *keyboard,
        const struct wl_keyboard_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(keyboard),
                                 (void (**)(void))listener, data);
}

static inline int wl_pointer_add_listener(struct wl_pointer *pointer,
        const struct wl_pointer_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(pointer),
                                 (void (**)(void))listener, data);
}

static inline int wl_output_add_listener(struct wl_output *output,
        const struct wl_output_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(output),
                                 (void (**)(void))listener, data);
}

static inline int xdg_wm_base_add_listener(struct xdg_wm_base *base,
        const struct xdg_wm_base_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(base), (void (**)(void))listener, data);
}

static inline int xdg_surface_add_listener(struct xdg_surface *surface,
        const struct xdg_surface_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(surface),
                                 (void (**)(void))listener, data);
}

static inline int xdg_toplevel_add_listener(struct xdg_toplevel *toplevel,
        const struct xdg_toplevel_listener *listener, void *data)
{
    return wl_proxy_add_listener(WL_PROXY(toplevel),
                                 (void (**)(void))listener, data);
}

/* --- wl_display --- */

static inline struct wl_registry *wl_display_get_registry(struct wl_display *d)
{
    return (struct wl_registry *)wl_proxy_marshal_constructor(
            WL_PROXY(d), WL_DISPLAY_GET_REGISTRY, &wl_registry_interface, NULL);
}

static inline struct wl_callback *wl_display_sync(struct wl_display *d)
{
    return (struct wl_callback *)wl_proxy_marshal_constructor(
            WL_PROXY(d), WL_DISPLAY_SYNC, &wl_callback_interface, NULL);
}

/* --- wl_registry --- */

/* bind is the one request whose new object's type is not fixed by the
 * protocol, so the interface has to be named on the wire. */
static inline void *wl_registry_bind(struct wl_registry *registry,
                                     uint32_t name,
                                     const struct wl_interface *interface,
                                     uint32_t version)
{
    return wl_proxy_marshal_constructor_versioned(
            WL_PROXY(registry), WL_REGISTRY_BIND, interface, version,
            name, interface->name, version, NULL);
}

/* --- wl_compositor --- */

static inline struct wl_surface *
wl_compositor_create_surface(struct wl_compositor *compositor)
{
    return (struct wl_surface *)wl_proxy_marshal_constructor(
            WL_PROXY(compositor), WL_COMPOSITOR_CREATE_SURFACE,
            &wl_surface_interface, NULL);
}

static inline struct wl_region *
wl_compositor_create_region(struct wl_compositor *compositor)
{
    return (struct wl_region *)wl_proxy_marshal_constructor(
            WL_PROXY(compositor), WL_COMPOSITOR_CREATE_REGION,
            &wl_region_interface, NULL);
}

/* --- wl_shm --- */

static inline struct wl_shm_pool *
wl_shm_create_pool(struct wl_shm *shm, int32_t fd, int32_t size)
{
    return (struct wl_shm_pool *)wl_proxy_marshal_constructor(
            WL_PROXY(shm), WL_SHM_CREATE_POOL, &wl_shm_pool_interface,
            NULL, fd, size);
}

static inline struct wl_buffer *
wl_shm_pool_create_buffer(struct wl_shm_pool *pool, int32_t offset,
                          int32_t width, int32_t height, int32_t stride,
                          uint32_t format)
{
    return (struct wl_buffer *)wl_proxy_marshal_constructor(
            WL_PROXY(pool), WL_SHM_POOL_CREATE_BUFFER, &wl_buffer_interface,
            NULL, offset, width, height, stride, format);
}

static inline void wl_shm_pool_destroy(struct wl_shm_pool *pool)
{
    wl_proxy_marshal(WL_PROXY(pool), WL_SHM_POOL_DESTROY);
    wl_proxy_destroy(WL_PROXY(pool));
}

static inline void wl_buffer_destroy(struct wl_buffer *buffer)
{
    wl_proxy_marshal(WL_PROXY(buffer), WL_BUFFER_DESTROY);
    wl_proxy_destroy(WL_PROXY(buffer));
}

/* --- wl_surface --- */

static inline void wl_surface_attach(struct wl_surface *surface,
                                     struct wl_buffer *buffer, int32_t x,
                                     int32_t y)
{
    wl_proxy_marshal(WL_PROXY(surface), WL_SURFACE_ATTACH, buffer, x, y);
}

static inline void wl_surface_damage(struct wl_surface *surface, int32_t x,
                                     int32_t y, int32_t width, int32_t height)
{
    wl_proxy_marshal(WL_PROXY(surface), WL_SURFACE_DAMAGE, x, y, width, height);
}

static inline void wl_surface_commit(struct wl_surface *surface)
{
    wl_proxy_marshal(WL_PROXY(surface), WL_SURFACE_COMMIT);
}

static inline struct wl_callback *wl_surface_frame(struct wl_surface *surface)
{
    return (struct wl_callback *)wl_proxy_marshal_constructor(
            WL_PROXY(surface), WL_SURFACE_FRAME, &wl_callback_interface, NULL);
}

static inline void wl_surface_destroy(struct wl_surface *surface)
{
    wl_proxy_marshal(WL_PROXY(surface), WL_SURFACE_DESTROY);
    wl_proxy_destroy(WL_PROXY(surface));
}

/* --- wl_seat --- */

static inline struct wl_keyboard *wl_seat_get_keyboard(struct wl_seat *seat)
{
    return (struct wl_keyboard *)wl_proxy_marshal_constructor(
            WL_PROXY(seat), WL_SEAT_GET_KEYBOARD, &wl_keyboard_interface, NULL);
}

static inline struct wl_pointer *wl_seat_get_pointer(struct wl_seat *seat)
{
    return (struct wl_pointer *)wl_proxy_marshal_constructor(
            WL_PROXY(seat), WL_SEAT_GET_POINTER, &wl_pointer_interface, NULL);
}

/* --- xdg_shell --- */

static inline void xdg_wm_base_pong(struct xdg_wm_base *base, uint32_t serial)
{
    wl_proxy_marshal(WL_PROXY(base), XDG_WM_BASE_PONG, serial);
}

static inline struct xdg_surface *
xdg_wm_base_get_xdg_surface(struct xdg_wm_base *base,
                            struct wl_surface *surface)
{
    return (struct xdg_surface *)wl_proxy_marshal_constructor(
            WL_PROXY(base), XDG_WM_BASE_GET_XDG_SURFACE,
            &xdg_surface_interface, NULL, surface);
}

static inline struct xdg_toplevel *
xdg_surface_get_toplevel(struct xdg_surface *surface)
{
    return (struct xdg_toplevel *)wl_proxy_marshal_constructor(
            WL_PROXY(surface), XDG_SURFACE_GET_TOPLEVEL,
            &xdg_toplevel_interface, NULL);
}

static inline void xdg_surface_ack_configure(struct xdg_surface *surface,
                                             uint32_t serial)
{
    wl_proxy_marshal(WL_PROXY(surface), XDG_SURFACE_ACK_CONFIGURE, serial);
}

static inline void xdg_surface_set_window_geometry(struct xdg_surface *surface,
        int32_t x, int32_t y, int32_t width, int32_t height)
{
    wl_proxy_marshal(WL_PROXY(surface), XDG_SURFACE_SET_WINDOW_GEOMETRY,
                     x, y, width, height);
}

static inline void xdg_toplevel_set_title(struct xdg_toplevel *toplevel,
                                          const char *title)
{
    wl_proxy_marshal(WL_PROXY(toplevel), XDG_TOPLEVEL_SET_TITLE, title);
}

static inline void xdg_toplevel_set_app_id(struct xdg_toplevel *toplevel,
                                           const char *app_id)
{
    wl_proxy_marshal(WL_PROXY(toplevel), XDG_TOPLEVEL_SET_APP_ID, app_id);
}

static inline void xdg_toplevel_destroy(struct xdg_toplevel *toplevel)
{
    wl_proxy_marshal(WL_PROXY(toplevel), XDG_TOPLEVEL_DESTROY);
    wl_proxy_destroy(WL_PROXY(toplevel));
}

#endif /* WAYLAND_CLIENT_H */
