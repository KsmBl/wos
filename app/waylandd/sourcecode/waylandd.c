/* waylandd -- a Wayland display server with nothing on the screen.
 *
 * This is not the compositor.  `sway` is the compositor: it owns the display,
 * tiles windows and runs programs.  This is the smallest server that is still
 * a real one -- it answers on a socket, speaks the protocol through the same
 * library sway does, and advertises a wl_compositor that hands out surfaces
 * which are never drawn.
 *
 * It is worth having for two reasons.  It is what a client is tested against
 * when the question is whether the *client* is right, with no screen and no
 * window management in the way.  And it is the reference for what the protocol
 * library alone gives you: everything below is wl_display, wl_registry and
 * wl_compositor, and none of it is written here -- the library implements the
 * first two, and the third is thirty lines.
 *
 * Run it on an address of its own, so it does not fight the compositor for the
 * default one:
 *
 *     waylandd /ramdisk/wayland-test
 *     wlprobe  /ramdisk/wayland-test
 */

#include <wkernel.h>
#include <wayland-server.h>
#include <stdarg.h>

#define DEFAULT_DISPLAY "wayland-0"
#define LOG_PATH        "/ramdisk/waylandd.log"

static int log_fd = -1;

static void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

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
 *  wl_surface -- accepted, remembered, never drawn
 * ------------------------------------------------------------------ */

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    logf("  surface %u attached buffer %u at %d,%d", wl_resource_get_id(r),
         buffer ? wl_resource_get_id(buffer) : 0, x, y);
}

static void surface_nothing(struct wl_client *c, struct wl_resource *r)
{
}

static void surface_rect(struct wl_client *c, struct wl_resource *r,
                         int32_t x, int32_t y, int32_t w, int32_t h)
{
}

static void surface_frame(struct wl_client *c, struct wl_resource *r,
                          uint32_t id)
{
    /* A frame callback asks to be told when to draw the next one.  With no
     * screen there is no next frame to wait for, so it fires at once -- which
     * is a truthful answer, and keeps a client that draws in a frame callback
     * from stopping after the first one. */
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cb)
        return;

    wl_resource_post_event(cb, WL_CALLBACK_DONE, wticks());
    wl_resource_destroy(cb);
}

static void surface_region(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *region)
{
}

static void surface_int(struct wl_client *c, struct wl_resource *r, int32_t v)
{
}

/* In request order: that is how the dispatcher finds them. */
static const struct {
    void (*destroy)(struct wl_client *, struct wl_resource *);
    void (*attach)(struct wl_client *, struct wl_resource *,
                   struct wl_resource *, int32_t, int32_t);
    void (*damage)(struct wl_client *, struct wl_resource *, int32_t, int32_t,
                   int32_t, int32_t);
    void (*frame)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*set_opaque_region)(struct wl_client *, struct wl_resource *,
                              struct wl_resource *);
    void (*set_input_region)(struct wl_client *, struct wl_resource *,
                             struct wl_resource *);
    void (*commit)(struct wl_client *, struct wl_resource *);
    void (*set_buffer_transform)(struct wl_client *, struct wl_resource *,
                                 int32_t);
    void (*set_buffer_scale)(struct wl_client *, struct wl_resource *, int32_t);
    void (*damage_buffer)(struct wl_client *, struct wl_resource *, int32_t,
                          int32_t, int32_t, int32_t);
} surface_implementation = {
    surface_destroy, surface_attach, surface_rect, surface_frame,
    surface_region, surface_region, surface_nothing, surface_int,
    surface_int, surface_rect,
};

/* ------------------------------------------------------------------ *
 *  wl_compositor
 * ------------------------------------------------------------------ */

static void compositor_create_surface(struct wl_client *c,
                                      struct wl_resource *r, uint32_t id)
{
    struct wl_resource *s = wl_resource_create(c, &wl_surface_interface,
                                               wl_resource_get_version(r), id);
    if (!s) {
        wl_resource_post_error(r, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "id %u is already in use", id);
        return;
    }

    wl_resource_set_implementation(s, &surface_implementation, NULL, NULL);
    logf("  created surface %u", id);
}

static void compositor_create_region(struct wl_client *c,
                                     struct wl_resource *r, uint32_t id)
{
    struct wl_resource *reg = wl_resource_create(c, &wl_region_interface,
                                                 wl_resource_get_version(r), id);
    if (reg)
        wl_resource_set_implementation(reg, NULL, NULL, NULL);
}

static const struct {
    void (*create_surface)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*create_region)(struct wl_client *, struct wl_resource *, uint32_t);
} compositor_implementation = {
    compositor_create_surface, compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface,
                                               (int)version, id);
    if (!r)
        return;

    wl_resource_set_implementation(r, &compositor_implementation, NULL, NULL);
    logf("  bound wl_compositor version %u as %u", version, id);
}

/* ------------------------------------------------------------------ *
 *  The server
 * ------------------------------------------------------------------ */

static void client_arrived(struct wl_client *c, void *data)
{
    logf("client connected on descriptor %d", wl_client_get_fd(c));
}

static void client_left(struct wl_client *c, void *data)
{
    logf("client on descriptor %d disconnected", wl_client_get_fd(c));
}

int main(int argc, char **argv)
{
    const char *name = (argc > 1) ? argv[1] : DEFAULT_DISPLAY;

    log_fd = wopen(LOG_PATH, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);

    struct wl_display *display = wl_display_create();
    if (!display) {
        wfprintf(W_STDERR, "waylandd: out of memory\n");
        return 1;
    }

    int r = wl_display_add_socket(display, name);
    if (r < 0) {
        wfprintf(W_STDERR, "waylandd: cannot listen on %s: %s\n", name,
                 wstrerror(-r));
        if (-r == W_EEXIST)
            wfprintf(W_STDERR, "waylandd: something is already the display "
                               "server there -- `systemctl status sway`\n");
        logf("cannot listen on %s: %s", name, wstrerror(-r));
        return 1;
    }

    wl_display_set_client_callbacks(display, client_arrived, client_left, NULL);
    wl_global_create(display, &wl_compositor_interface, 6, NULL, compositor_bind);

    logf("waylandd listening on %s, advertising wl_compositor", name);

    for (;;) {
        wpollfd_t watch[W_POLL_MAX];
        int       n = wl_display_poll_fds(display, watch, W_POLL_MAX);

        /* A timeout rather than an indefinite wait, so a service asked to stop
         * leaves within a second even with nothing connected. */
        if (wpoll(watch, n, 1000) > 0)
            wl_display_handle(display, watch, n);
        else
            wl_display_flush_clients(display);
    }
}
