/* wayland-server.h -- writing a Wayland compositor.
 *
 * libwayland-server's names and shapes, for the same reason the client header
 * has libwayland-client's: a compositor written against the real thing should
 * read the same here.  Globals are advertised with wl_global_create(), objects
 * come into being with wl_resource_create(), a request handler takes
 * (client, resource, ...) and events go out with wl_resource_post_event().
 *
 * The one real departure is the event loop.  Upstream has wl_event_loop, which
 * owns the waiting and calls back into the compositor.  A compositor on WOS has
 * its own descriptors to wait on -- the keyboard, its IPC socket -- and cannot
 * hand the waiting to a library that does not know about them.  So the display
 * exposes its descriptors and is told what happened to them, and the
 * compositor keeps the loop.  It is a smaller idea than wl_event_loop and it
 * fits a system where the compositor is also the thing that owns the screen.
 */
#ifndef WAYLAND_SERVER_H
#define WAYLAND_SERVER_H

#include "wayland-util.h"
#include "wayland-protocol.h"

struct wl_display;
struct wl_client;
struct wl_resource;
struct wl_global;

/* Called when a client binds a global.  The compositor's job is to create the
 * resource for `id` and give it an implementation. */
typedef void (*wl_global_bind_func_t)(struct wl_client *client, void *data,
                                      uint32_t version, uint32_t id);

/* Called as a resource goes away, whether the client destroyed it or the
 * client itself went. */
typedef void (*wl_resource_destroy_func_t)(struct wl_resource *resource);

/* Called when a client connects or disconnects, so the compositor can forget
 * whatever it was keeping for it. */
typedef void (*wl_client_func_t)(struct wl_client *client, void *data);

/* ==================================================================== *
 *  The display
 * ==================================================================== */

struct wl_display *wl_display_create(void);
void wl_display_destroy(struct wl_display *display);

/**
 * Answer to a display name, e.g. "wayland-0".
 * @return 0, or a negative error -- `-W_EEXIST` when something else is already
 *         listening there, which is what a second compositor gets.
 */
int wl_display_add_socket(struct wl_display *display, const char *name);

/** Serial numbers, which order events against each other. */
uint32_t wl_display_next_serial(struct wl_display *display);

/** Tell the compositor when clients arrive and leave. */
void wl_display_set_client_callbacks(struct wl_display *display,
                                     wl_client_func_t created,
                                     wl_client_func_t destroyed, void *data);

/* --- the loop the compositor owns --- */

/**
 * Fill `out` with the descriptors the display needs waited on: its listening
 * socket, and one per client. `W_POLLOUT` is asked for only where there is
 * something queued to write.
 * @return How many were written.
 */
int wl_display_poll_fds(struct wl_display *display, wpollfd_t *out, int max);

/**
 * Act on what a poll reported: accept new clients, read requests and dispatch
 * them, flush what is queued. Descriptors that are not the display's are
 * ignored, so a compositor may pass its whole poll set.
 * @return The number of requests dispatched.
 */
int wl_display_handle(struct wl_display *display, const wpollfd_t *fds,
                      int count);

/** Push queued events to every client. */
void wl_display_flush_clients(struct wl_display *display);

/* ==================================================================== *
 *  Globals
 * ==================================================================== */

struct wl_global *wl_global_create(struct wl_display *display,
                                   const struct wl_interface *interface,
                                   int version, void *data,
                                   wl_global_bind_func_t bind);
void wl_global_destroy(struct wl_global *global);
void *wl_global_get_user_data(struct wl_global *global);

/* ==================================================================== *
 *  Clients and resources
 * ==================================================================== */

void wl_client_destroy(struct wl_client *client);
int  wl_client_get_fd(struct wl_client *client);
void wl_client_flush(struct wl_client *client);
struct wl_display *wl_client_get_display(struct wl_client *client);

/** Whatever the compositor wants to hang off a client. */
void *wl_client_get_user_data(struct wl_client *client);
void  wl_client_set_user_data(struct wl_client *client, void *data);

/**
 * Bring an object into existence.
 *
 * `id` is the number the client chose in the request that created it. Passing
 * 0 asks the server to choose one, which is how a compositor creates an object
 * a client did not ask for.
 */
struct wl_resource *wl_resource_create(struct wl_client *client,
                                       const struct wl_interface *interface,
                                       int version, uint32_t id);

/**
 * Give a resource its request handlers.
 *
 * `implementation` is a structure of function pointers in request order, each
 * taking `(struct wl_client *, struct wl_resource *, ...)`. A null pointer
 * where a request should be is answered with `wl_display.error`, rather than
 * silently doing nothing -- a client that asked for something and got no reply
 * and no error would wait forever.
 */
void wl_resource_set_implementation(struct wl_resource *resource,
                                    const void *implementation, void *data,
                                    wl_resource_destroy_func_t destroy);

void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode, ...);

/** Tell the client it has broken the protocol, and drop it. */
void wl_resource_post_error(struct wl_resource *resource, uint32_t code,
                            const char *msg, ...);

/**
 * Destroy a resource and tell the client the id is free again.
 *
 * The second half matters: a client reuses ids, and libwayland asserts if it
 * reuses one the server has not released.
 */
void wl_resource_destroy(struct wl_resource *resource);

uint32_t wl_resource_get_id(struct wl_resource *resource);
uint32_t wl_resource_get_version(struct wl_resource *resource);
void    *wl_resource_get_user_data(struct wl_resource *resource);
void     wl_resource_set_user_data(struct wl_resource *resource, void *data);
struct wl_client *wl_resource_get_client(struct wl_resource *resource);
const struct wl_interface *wl_resource_get_interface(struct wl_resource *r);

/** True if this resource is of that interface: what a compositor checks before
 *  believing an object argument is the type it hoped for. */
int wl_resource_instance_of(struct wl_resource *resource,
                            const struct wl_interface *interface);

/** Find one of a client's objects by id, or NULL. */
struct wl_resource *wl_client_get_object(struct wl_client *client, uint32_t id);

#endif /* WAYLAND_SERVER_H */
