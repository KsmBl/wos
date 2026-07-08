/* The protocol: what clients are allowed to ask for, and what they are told.
 *
 * Six globals are advertised, and between them they are everything a window
 * needs: wl_compositor makes surfaces, wl_shm turns shared memory into
 * buffers, xdg_wm_base turns a surface into a window, wl_seat carries the
 * keyboard, wl_output describes the screen, and wl_subcompositor is not here
 * because nothing needs it yet.
 *
 * Two rules run through all of it.
 *
 * Everything a client does is double buffered: attach, damage and the rest
 * only take effect at commit.  That is not a detail of the implementation, it
 * is the protocol's central promise -- a client is never seen half way through
 * changing its mind.
 *
 * And a window's size is not the client's to choose.  A tiling compositor
 * tells it, through xdg_toplevel.configure, and the client draws what it was
 * told.  The handshake is strict about order: the size first, then the
 * xdg_surface.configure that makes it real, then the client's ack, and only
 * then is a buffer worth looking at.
 */

#include "sway.h"

/* ------------------------------------------------------------------ *
 *  Pools and buffers
 * ------------------------------------------------------------------ */

static void pool_unref(struct pool *p)
{
    if (!p || --p->refs > 0)
        return;

    if (p->data)
        wshmunmap(p->data);
    wclose(p->fd);
    free(p);
}

static void buffer_destroy_resource(struct wl_resource *resource)
{
    struct buffer *b = wl_resource_get_user_data(resource);
    if (!b)
        return;

    /* A buffer a window is still showing must not take its memory with it.
     * The view keeps a pointer, so the pool stays mapped until the view lets
     * go, and the buffer itself is freed here only once nothing points at it. */
    for (struct view *v = sway.views; v; v = v->next) {
        if (v->current == b)
            v->current = NULL;
        if (v->pending == b) {
            v->pending     = NULL;
            v->pending_set = 0;
        }
    }

    pool_unref(b->pool);
    free(b);
}

static void buffer_destroy(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static const struct {
    void (*destroy)(struct wl_client *, struct wl_resource *);
} buffer_implementation = { buffer_destroy };

void shell_release_buffer(struct buffer *b)
{
    if (b && b->resource && !b->released) {
        wl_resource_post_event(b->resource, WL_BUFFER_RELEASE);
        b->released = 1;
    }
}

static void pool_create_buffer(struct wl_client *c, struct wl_resource *r,
                               uint32_t id, int32_t offset, int32_t width,
                               int32_t height, int32_t stride, uint32_t format)
{
    struct pool *p = wl_resource_get_user_data(r);

    if (width <= 0 || height <= 0 || stride < width * 4 || offset < 0) {
        wl_resource_post_error(r, 0, "a %dx%d buffer with stride %d is not a "
                               "buffer", width, height, stride);
        return;
    }

    /* Checked against the pool's real size rather than against what the client
     * said the pool was.  The size is the one fact about shared memory the
     * sender cannot misreport, and it is what stops a buffer claiming to be
     * larger than the memory behind it. */
    int64_t needed = (int64_t)offset + (int64_t)stride * height;
    if (needed > (int64_t)p->size) {
        wl_resource_post_error(r, 1, "a buffer needing %d bytes does not fit "
                               "in a pool of %u", (int)needed, p->size);
        return;
    }

    if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888) {
        wl_resource_post_error(r, 0, "format %u is not one of the two "
                               "advertised", format);
        return;
    }

    struct buffer *b = malloc(sizeof(*b));
    if (!b)
        return;

    memset(b, 0, sizeof(*b));
    b->pool   = p;
    b->offset = offset;
    b->width  = width;
    b->height = height;
    b->stride = stride;
    b->format = format;
    p->refs++;

    b->resource = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!b->resource) {
        pool_unref(p);
        free(b);
        return;
    }

    wl_resource_set_implementation(b->resource, &buffer_implementation, b,
                                   buffer_destroy_resource);
}

static void pool_destroy(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static void pool_resize(struct wl_client *c, struct wl_resource *r, int32_t size)
{
    /* The protocol only allows a pool to grow, and growing means the client
     * gave the kernel a bigger object -- which it cannot, here: a WOS shared
     * memory object is fixed at creation.  A client that needs more room makes
     * another pool, which is what every toolkit does anyway when a window is
     * resized. */
    struct pool *p = wl_resource_get_user_data(r);

    if ((uint32_t)size > p->size)
        wl_resource_post_error(r, 0, "this compositor cannot grow a pool past "
                               "the %u bytes it was made with", p->size);
}

static const struct {
    void (*create_buffer)(struct wl_client *, struct wl_resource *, uint32_t,
                          int32_t, int32_t, int32_t, int32_t, uint32_t);
    void (*destroy)(struct wl_client *, struct wl_resource *);
    void (*resize)(struct wl_client *, struct wl_resource *, int32_t);
} pool_implementation = { pool_create_buffer, pool_destroy, pool_resize };

static void pool_release_resource(struct wl_resource *r)
{
    pool_unref(wl_resource_get_user_data(r));
}

static void shm_create_pool(struct wl_client *c, struct wl_resource *r,
                            uint32_t id, int32_t fd, int32_t size)
{
    int real = wshmsize(fd);

    if (real < 0) {
        wl_resource_post_error(r, 0, "that descriptor is not shared memory");
        wclose(fd);
        return;
    }
    if (size <= 0 || size > real) {
        wl_resource_post_error(r, 0, "a pool of %d bytes was offered %d",
                               size, real);
        wclose(fd);
        return;
    }

    struct pool *p = malloc(sizeof(*p));
    if (!p) {
        wclose(fd);
        return;
    }

    memset(p, 0, sizeof(*p));
    p->fd   = fd;
    p->size = (uint32_t)size;
    p->data = wshmmap(fd);
    p->refs = 1;

    if (!p->data) {
        wl_resource_post_error(r, 0, "the compositor could not map that pool");
        wclose(fd);
        free(p);
        return;
    }

    struct wl_resource *res = wl_resource_create(c, &wl_shm_pool_interface, 1, id);
    if (!res) {
        pool_unref(p);
        return;
    }

    wl_resource_set_implementation(res, &pool_implementation, p,
                                   pool_release_resource);
}

static const struct {
    void (*create_pool)(struct wl_client *, struct wl_resource *, uint32_t,
                        int32_t, int32_t);
} shm_implementation = { shm_create_pool };

static void shm_bind(struct wl_client *client, void *data, uint32_t version,
                     uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, &wl_shm_interface,
                                               (int)version, id);
    if (!r)
        return;

    wl_resource_set_implementation(r, &shm_implementation, NULL, NULL);

    /* The two every compositor must support.  Advertising more would be a
     * promise to convert them. */
    wl_resource_post_event(r, WL_SHM_FORMAT, WL_SHM_FORMAT_ARGB8888);
    wl_resource_post_event(r, WL_SHM_FORMAT, WL_SHM_FORMAT_XRGB8888);
}

/* ------------------------------------------------------------------ *
 *  wl_surface
 * ------------------------------------------------------------------ */

/* Frame callbacks a surface has asked for and not yet been given.  A client
 * that draws in a frame callback stops drawing entirely if one is dropped, so
 * these are answered even when nothing was rendered. */
#define MAX_FRAME_CALLBACKS 4

static struct wl_resource *frame_callbacks[MAX_FRAME_CALLBACKS * 8];
static int                 frame_callback_count;

static void surface_destroy_resource(struct wl_resource *resource)
{
    struct view *v = wl_resource_get_user_data(resource);
    if (!v)
        return;

    layout_remove_view(v);

    for (struct view **at = &sway.views; *at; at = &(*at)->next)
        if (*at == v) {
            *at = v->next;
            break;
        }

    free(v);
}

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    struct view *v = wl_resource_get_user_data(r);

    v->pending     = buffer ? wl_resource_get_user_data(buffer) : NULL;
    v->pending_set = 1;
}

static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
    /* Every commit repaints the whole screen here, so damage tracking would
     * change nothing.  Accepting it and ignoring it is correct: damage is a
     * hint about what the compositor may skip, never a restriction on what it
     * may draw. */
}

static void surface_frame(struct wl_client *c, struct wl_resource *r,
                          uint32_t id)
{
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cb)
        return;

    if (frame_callback_count < (int)(sizeof(frame_callbacks) /
                                     sizeof(frame_callbacks[0])))
        frame_callbacks[frame_callback_count++] = cb;
    else
        wl_resource_destroy(cb);
}

static void surface_region(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *region)
{
    /* Opaque and input regions are optimisations for a compositor that blends
     * and a pointer that has to be routed.  There is neither here. */
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    struct view *v = wl_resource_get_user_data(r);

    if (v->pending_set) {
        /* The old buffer goes back to the client as the new one arrives, which
         * is what lets it draw the next frame into the one it is not showing. */
        if (v->current && v->current != v->pending)
            shell_release_buffer(v->current);

        v->current     = v->pending;
        v->pending     = NULL;
        v->pending_set = 0;

        if (v->current)
            v->current->released = 0;
    }

    /* A window appears when it first has both a role and something to show.
     * Before that it is a surface the client is still setting up, and putting
     * it on the screen would show whatever was in the memory. */
    if (!v->mapped && v->toplevel && v->current) {
        v->mapped = 1;
        layout_add_view(v);
    }

    sway.dirty = 1;
}

static void surface_int(struct wl_client *c, struct wl_resource *r, int32_t v)
{
    /* Buffer transform and scale: this compositor draws at one scale and no
     * rotation, so both are accepted and mean nothing. */
}

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
    surface_destroy, surface_attach, surface_damage, surface_frame,
    surface_region, surface_region, surface_commit, surface_int, surface_int,
    surface_damage,
};

/* Answer every frame callback that has been asked for.  Called after a render,
 * and also when nothing was rendered -- a client waiting on one has stopped
 * drawing until it comes. */
void shell_frame_done(void)
{
    uint32_t now = wuptime_ms();

    for (int i = 0; i < frame_callback_count; i++) {
        wl_resource_post_event(frame_callbacks[i], WL_CALLBACK_DONE, now);
        wl_resource_destroy(frame_callbacks[i]);
    }
    frame_callback_count = 0;
}

/* ------------------------------------------------------------------ *
 *  wl_compositor and wl_region
 * ------------------------------------------------------------------ */

static void region_destroy(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static void region_rect(struct wl_client *c, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t w, int32_t h)
{
}

static const struct {
    void (*destroy)(struct wl_client *, struct wl_resource *);
    void (*add)(struct wl_client *, struct wl_resource *, int32_t, int32_t,
                int32_t, int32_t);
    void (*subtract)(struct wl_client *, struct wl_resource *, int32_t, int32_t,
                     int32_t, int32_t);
} region_implementation = { region_destroy, region_rect, region_rect };

static void compositor_create_surface(struct wl_client *c,
                                      struct wl_resource *r, uint32_t id)
{
    struct view *v = malloc(sizeof(*v));
    if (!v)
        return;

    memset(v, 0, sizeof(*v));
    v->client = c;

    v->surface = wl_resource_create(c, &wl_surface_interface,
                                    wl_resource_get_version(r), id);
    if (!v->surface) {
        free(v);
        return;
    }

    wl_resource_set_implementation(v->surface, &surface_implementation, v,
                                   surface_destroy_resource);

    v->next    = sway.views;
    sway.views = v;
}

static void compositor_create_region(struct wl_client *c,
                                     struct wl_resource *r, uint32_t id)
{
    struct wl_resource *reg = wl_resource_create(c, &wl_region_interface,
                                                 wl_resource_get_version(r), id);
    if (reg)
        wl_resource_set_implementation(reg, &region_implementation, NULL, NULL);
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
    if (r)
        wl_resource_set_implementation(r, &compositor_implementation, NULL, NULL);
}

/* ------------------------------------------------------------------ *
 *  xdg_shell: where windows come from
 * ------------------------------------------------------------------ */

static void toplevel_destroy(struct wl_client *c, struct wl_resource *r)
{
    struct view *v = wl_resource_get_user_data(r);

    if (v) {
        layout_remove_view(v);
        v->toplevel = NULL;
        v->mapped   = 0;
    }
    wl_resource_destroy(r);
}

static void toplevel_set_title(struct wl_client *c, struct wl_resource *r,
                               const char *title)
{
    struct view *v = wl_resource_get_user_data(r);

    strlcpy(v->title, title ? title : "", sizeof(v->title));
    sway.dirty = 1;
}

static void toplevel_set_app_id(struct wl_client *c, struct wl_resource *r,
                                const char *app_id)
{
    struct view *v = wl_resource_get_user_data(r);

    strlcpy(v->app_id, app_id ? app_id : "", sizeof(v->app_id));
}

static void toplevel_set_fullscreen(struct wl_client *c, struct wl_resource *r,
                                    struct wl_resource *output)
{
    struct view *v = wl_resource_get_user_data(r);

    ws_current()->fullscreen = v;
    layout_arrange();
}

static void toplevel_unset_fullscreen(struct wl_client *c,
                                      struct wl_resource *r)
{
    struct view *v = wl_resource_get_user_data(r);

    if (ws_current()->fullscreen == v)
        ws_current()->fullscreen = NULL;
    layout_arrange();
}

static void toplevel_nothing(struct wl_client *c, struct wl_resource *r)
{
}

static void toplevel_object(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *o)
{
}

static void toplevel_menu(struct wl_client *c, struct wl_resource *r,
                          struct wl_resource *seat, uint32_t serial,
                          int32_t x, int32_t y)
{
}

static void toplevel_move(struct wl_client *c, struct wl_resource *r,
                          struct wl_resource *seat, uint32_t serial)
{
    /* Moving and resizing are the client asking to be dragged, and a tiling
     * compositor decides both.  Ignoring it is what sway does. */
}

static void toplevel_resize(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *seat, uint32_t serial,
                            uint32_t edges)
{
}

static void toplevel_size(struct wl_client *c, struct wl_resource *r,
                          int32_t w, int32_t h)
{
    /* Minimum and maximum sizes: a tiled window gets the size of its tile. */
}

static const struct {
    void (*destroy)(struct wl_client *, struct wl_resource *);
    void (*set_parent)(struct wl_client *, struct wl_resource *,
                       struct wl_resource *);
    void (*set_title)(struct wl_client *, struct wl_resource *, const char *);
    void (*set_app_id)(struct wl_client *, struct wl_resource *, const char *);
    void (*show_window_menu)(struct wl_client *, struct wl_resource *,
                             struct wl_resource *, uint32_t, int32_t, int32_t);
    void (*move)(struct wl_client *, struct wl_resource *, struct wl_resource *,
                 uint32_t);
    void (*resize)(struct wl_client *, struct wl_resource *,
                   struct wl_resource *, uint32_t, uint32_t);
    void (*set_max_size)(struct wl_client *, struct wl_resource *, int32_t,
                         int32_t);
    void (*set_min_size)(struct wl_client *, struct wl_resource *, int32_t,
                         int32_t);
    void (*set_maximized)(struct wl_client *, struct wl_resource *);
    void (*unset_maximized)(struct wl_client *, struct wl_resource *);
    void (*set_fullscreen)(struct wl_client *, struct wl_resource *,
                           struct wl_resource *);
    void (*unset_fullscreen)(struct wl_client *, struct wl_resource *);
    void (*set_minimized)(struct wl_client *, struct wl_resource *);
} toplevel_implementation = {
    toplevel_destroy, toplevel_object, toplevel_set_title,
    toplevel_set_app_id, toplevel_menu, toplevel_move, toplevel_resize,
    toplevel_size, toplevel_size, toplevel_nothing, toplevel_nothing,
    toplevel_set_fullscreen, toplevel_unset_fullscreen, toplevel_nothing,
};

static void xdg_surface_get_toplevel_(struct wl_client *c,
                                      struct wl_resource *r, uint32_t id)
{
    struct view *v = wl_resource_get_user_data(r);

    v->toplevel = wl_resource_create(c, &xdg_toplevel_interface,
                                     wl_resource_get_version(r), id);
    if (!v->toplevel)
        return;

    wl_resource_set_implementation(v->toplevel, &toplevel_implementation, v,
                                   NULL);

    /* Told its size before it has drawn anything.  A client is entitled to
     * wait for this and many do, so a compositor that leaves it until the
     * first commit gets a window that never appears. */
    int count = layout_view_count(ws_current());
    v->w = (int)sway.screen.width - 2 * sway.config.gaps_outer;
    v->h = sway.usable_h - 2 * sway.config.gaps_outer;
    if (count > 0) {
        if (ws_current()->root->layout == L_SPLITH)
            v->w /= (count + 1);
        else
            v->h /= (count + 1);
    }

    shell_configure(v);
}

static void xdg_surface_destroy_(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static void xdg_surface_ack(struct wl_client *c, struct wl_resource *r,
                            uint32_t serial)
{
    struct view *v = wl_resource_get_user_data(r);

    if (v && v->last_serial == serial)
        v->last_serial = 0;
}

static void xdg_surface_geometry(struct wl_client *c, struct wl_resource *r,
                                 int32_t x, int32_t y, int32_t w, int32_t h)
{
}

static void xdg_surface_popup(struct wl_client *c, struct wl_resource *r,
                              uint32_t id, struct wl_resource *parent,
                              struct wl_resource *positioner)
{
    wl_resource_post_error(r, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                           "this compositor has no popups: there is no pointer "
                           "to dismiss one with");
}

static const struct {
    void (*destroy)(struct wl_client *, struct wl_resource *);
    void (*get_toplevel)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*get_popup)(struct wl_client *, struct wl_resource *, uint32_t,
                      struct wl_resource *, struct wl_resource *);
    void (*set_window_geometry)(struct wl_client *, struct wl_resource *,
                                int32_t, int32_t, int32_t, int32_t);
    void (*ack_configure)(struct wl_client *, struct wl_resource *, uint32_t);
} xdg_surface_implementation = {
    xdg_surface_destroy_, xdg_surface_get_toplevel_, xdg_surface_popup,
    xdg_surface_geometry, xdg_surface_ack,
};

static void wm_base_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
                                    uint32_t id, struct wl_resource *surface)
{
    if (!wl_resource_instance_of(surface, &wl_surface_interface)) {
        wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE,
                               "that object is not a wl_surface");
        return;
    }

    struct view *v = wl_resource_get_user_data(surface);

    v->xdg_surface = wl_resource_create(c, &xdg_surface_interface,
                                        wl_resource_get_version(r), id);
    if (!v->xdg_surface)
        return;

    wl_resource_set_implementation(v->xdg_surface, &xdg_surface_implementation,
                                   v, NULL);
}

static void wm_base_pong(struct wl_client *c, struct wl_resource *r,
                         uint32_t serial)
{
}

static void wm_base_destroy(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static void wm_base_positioner(struct wl_client *c, struct wl_resource *r,
                               uint32_t id)
{
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface,
                                               wl_resource_get_version(r), id);
    if (p)
        wl_resource_set_implementation(p, NULL, NULL, NULL);
}

static const struct {
    void (*destroy)(struct wl_client *, struct wl_resource *);
    void (*create_positioner)(struct wl_client *, struct wl_resource *,
                              uint32_t);
    void (*get_xdg_surface)(struct wl_client *, struct wl_resource *, uint32_t,
                            struct wl_resource *);
    void (*pong)(struct wl_client *, struct wl_resource *, uint32_t);
} wm_base_implementation = {
    wm_base_destroy, wm_base_positioner, wm_base_get_xdg_surface, wm_base_pong,
};

static void wm_base_bind(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, &xdg_wm_base_interface,
                                               (int)version, id);
    if (r)
        wl_resource_set_implementation(r, &wm_base_implementation, NULL, NULL);
}

/* Tell a window how big it is.
 *
 * The order matters and is the protocol's: the toplevel's size and state
 * first, then the xdg_surface configure carrying the serial that makes the
 * pair atomic.  A client acknowledges the serial and only then draws. */
void shell_configure(struct view *v)
{
    if (!v->toplevel || !v->xdg_surface)
        return;

    struct wl_array states;
    wl_array_init(&states);

    uint32_t *s = wl_array_add(&states, sizeof(uint32_t));
    if (s)
        *s = XDG_TOPLEVEL_STATE_TILED_LEFT;

    /* A tiled window is told so, and told when it has the keyboard: that is
     * how a toolkit knows to draw a flat edge rather than a rounded floating
     * one, and an active title bar rather than a dimmed one. */
    if ((s = wl_array_add(&states, sizeof(uint32_t))))
        *s = XDG_TOPLEVEL_STATE_TILED_RIGHT;
    if ((s = wl_array_add(&states, sizeof(uint32_t))))
        *s = XDG_TOPLEVEL_STATE_TILED_TOP;
    if ((s = wl_array_add(&states, sizeof(uint32_t))))
        *s = XDG_TOPLEVEL_STATE_TILED_BOTTOM;

    if (sway.focused == v && (s = wl_array_add(&states, sizeof(uint32_t))))
        *s = XDG_TOPLEVEL_STATE_ACTIVATED;

    if (ws_current()->fullscreen == v &&
        (s = wl_array_add(&states, sizeof(uint32_t))))
        *s = XDG_TOPLEVEL_STATE_FULLSCREEN;

    int inner_w = v->w;
    int inner_h = v->h - sway.config.title_height;
    int border  = sway.config.border_width;

    inner_w -= 2 * border;
    inner_h -= 2 * border;
    if (inner_w < 1) inner_w = 1;
    if (inner_h < 1) inner_h = 1;

    wl_resource_post_event(v->toplevel, XDG_TOPLEVEL_CONFIGURE, inner_w,
                           inner_h, &states);
    wl_array_release(&states);

    v->last_serial = wl_display_next_serial(sway.display);
    wl_resource_post_event(v->xdg_surface, XDG_SURFACE_CONFIGURE,
                           v->last_serial);
}

void shell_close(struct view *v)
{
    if (v && v->toplevel)
        wl_resource_post_event(v->toplevel, XDG_TOPLEVEL_CLOSE);
}

/* ------------------------------------------------------------------ *
 *  wl_seat and wl_keyboard
 * ------------------------------------------------------------------ */

static void keyboard_release(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static const struct {
    void (*release)(struct wl_client *, struct wl_resource *);
} keyboard_implementation = { keyboard_release };

static void keyboard_gone(struct wl_resource *r)
{
    for (int i = 0; i < sway.keyboard_count; i++)
        if (sway.keyboards[i] == r) {
            sway.keyboards[i] = sway.keyboards[--sway.keyboard_count];
            return;
        }
}

/* The descriptor wl_keyboard.keymap has to carry.
 *
 * The event always carries one, even when the format is "no keymap", so there
 * has to be something to send.  It is a page of shared memory with nothing in
 * it, and the format says so honestly: WOS has no XKB keymap to offer, and a
 * client here reads the evdev codes directly.  Sending a made-up keymap would
 * be worse than saying there is none.
 *
 * A fresh one every time, and not as waste.  Sending a descriptor gives it
 * away -- the connection closes it once it has gone -- so a cached one is
 * closed after the first keyboard and its number is handed straight back to
 * the next thing that asks: a client's socket, or a client's pool of pixels.
 * The second keyboard would then "send the keymap" by sending somebody else's
 * live descriptor, and close it.  A page is a small price for that not
 * happening. */
static int keymap_fd(void)
{
    return wshmopen(4096);
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
                              uint32_t id)
{
    struct wl_resource *kb = wl_resource_create(c, &wl_keyboard_interface,
                                                wl_resource_get_version(r), id);
    if (!kb)
        return;

    wl_resource_set_implementation(kb, &keyboard_implementation, NULL,
                                   keyboard_gone);

    if (sway.keyboard_count < (int)(sizeof(sway.keyboards) /
                                    sizeof(sway.keyboards[0])))
        sway.keyboards[sway.keyboard_count++] = kb;

    int fd = keymap_fd();
    if (fd >= 0)
        wl_resource_post_event(kb, WL_KEYBOARD_KEYMAP,
                               WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 1);

    if (wl_resource_get_version(kb) >= 4)
        wl_resource_post_event(kb, WL_KEYBOARD_REPEAT_INFO, 25, 600);

    /* A keyboard created while a window of this client already has the focus
     * has to be told so, or the client waits for an enter that has been and
     * gone. */
    if (sway.focused && sway.focused->client == c) {
        struct wl_array keys;
        wl_array_init(&keys);
        wl_resource_post_event(kb, WL_KEYBOARD_ENTER,
                               wl_display_next_serial(sway.display),
                               sway.focused->surface, &keys);
        wl_array_release(&keys);
    }
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
                             uint32_t id)
{
    /* Created and never sent anything: this machine has no pointer, and the
     * capabilities event has already said so.  A client that asks anyway gets
     * an object that works and is silent, which is better than an error --
     * plenty of toolkits ask unconditionally. */
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
                                               wl_resource_get_version(r), id);
    if (p)
        wl_resource_set_implementation(p, NULL, NULL, NULL);
}

static void seat_get_touch(struct wl_client *c, struct wl_resource *r,
                           uint32_t id)
{
}

static void seat_release(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static const struct {
    void (*get_pointer)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*get_keyboard)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*get_touch)(struct wl_client *, struct wl_resource *, uint32_t);
    void (*release)(struct wl_client *, struct wl_resource *);
} seat_implementation = {
    seat_get_pointer, seat_get_keyboard, seat_get_touch, seat_release,
};

static void seat_bind(struct wl_client *client, void *data, uint32_t version,
                      uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, &wl_seat_interface,
                                               (int)version, id);
    if (!r)
        return;

    wl_resource_set_implementation(r, &seat_implementation, NULL, NULL);

    /* Keyboard only, and truthfully: there is no mouse driver, so claiming a
     * pointer would leave clients waiting for motion that never comes. */
    wl_resource_post_event(r, WL_SEAT_CAPABILITIES, WL_SEAT_CAPABILITY_KEYBOARD);

    if (version >= 2)
        wl_resource_post_event(r, WL_SEAT_NAME, "seat0");
}

/* Every keyboard belonging to a client, since one client may hold several. */
static void for_each_keyboard_of(struct wl_client *c, uint32_t opcode,
                                 struct view *v, uint32_t a, uint32_t b,
                                 uint32_t d, uint32_t e)
{
    for (int i = 0; i < sway.keyboard_count; i++) {
        struct wl_resource *kb = sway.keyboards[i];

        if (wl_resource_get_client(kb) != c)
            continue;

        if (opcode == WL_KEYBOARD_KEY)
            wl_resource_post_event(kb, WL_KEYBOARD_KEY, a, b, d, e);
        else
            wl_resource_post_event(kb, WL_KEYBOARD_MODIFIERS, a, b, d, e, 0);
    }
}

void shell_send_key(struct view *v, uint32_t code, uint32_t state,
                    uint32_t time_ms)
{
    if (!v)
        return;

    for_each_keyboard_of(v->client, WL_KEYBOARD_KEY,
                         v, wl_display_next_serial(sway.display), time_ms,
                         code, state);
}

void shell_send_modifiers(struct view *v, uint32_t mods)
{
    if (!v)
        return;

    /* Sent as depressed modifiers with nothing latched or locked, in XKB's own
     * bit positions -- which is what the kernel already reports, so there is
     * nothing to translate. */
    for_each_keyboard_of(v->client, WL_KEYBOARD_MODIFIERS, v,
                         wl_display_next_serial(sway.display), mods, 0, 0);
}

void shell_focus_changed(struct view *old, struct view *now)
{
    uint32_t serial;

    if (old && old->surface) {
        serial = wl_display_next_serial(sway.display);

        for (int i = 0; i < sway.keyboard_count; i++)
            if (wl_resource_get_client(sway.keyboards[i]) == old->client)
                wl_resource_post_event(sway.keyboards[i], WL_KEYBOARD_LEAVE,
                                       serial, old->surface);

        /* The window is no longer active, and its configure has to say so. */
        shell_configure(old);
    }

    if (now && now->surface) {
        struct wl_array keys;
        wl_array_init(&keys);

        serial = wl_display_next_serial(sway.display);

        for (int i = 0; i < sway.keyboard_count; i++)
            if (wl_resource_get_client(sway.keyboards[i]) == now->client)
                wl_resource_post_event(sway.keyboards[i], WL_KEYBOARD_ENTER,
                                       serial, now->surface, &keys);

        wl_array_release(&keys);

        shell_send_modifiers(now, sway.mods);
        shell_configure(now);
    }
}

/* ------------------------------------------------------------------ *
 *  wl_output
 * ------------------------------------------------------------------ */

static void output_release(struct wl_client *c, struct wl_resource *r)
{
    wl_resource_destroy(r);
}

static const struct {
    void (*release)(struct wl_client *, struct wl_resource *);
} output_implementation = { output_release };

static void output_bind(struct wl_client *client, void *data, uint32_t version,
                        uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, &wl_output_interface,
                                               (int)version, id);
    if (!r)
        return;

    wl_resource_set_implementation(r, &output_implementation, NULL, NULL);

    /* Physical size is reported as zero, which the protocol defines as
     * unknown.  There is no EDID here and no way to ask the monitor, and a
     * made-up figure would give a client a made-up DPI to scale by. */
    wl_resource_post_event(r, WL_OUTPUT_GEOMETRY, 0, 0, 0, 0,
                           WL_OUTPUT_SUBPIXEL_UNKNOWN, "WOS", "framebuffer",
                           WL_OUTPUT_TRANSFORM_NORMAL);

    wl_resource_post_event(r, WL_OUTPUT_MODE,
                           WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                           (int32_t)sway.screen.width,
                           (int32_t)sway.screen.height, 60000);

    if (version >= 2) {
        wl_resource_post_event(r, WL_OUTPUT_SCALE, 1);
        wl_resource_post_event(r, WL_OUTPUT_DONE);
    }
    if (version >= 4) {
        wl_resource_post_event(r, WL_OUTPUT_NAME, "WOS-1");
        wl_resource_post_event(r, WL_OUTPUT_DESCRIPTION,
                               "the framebuffer the console was using");
    }
}

/* ------------------------------------------------------------------ *
 *  Setting up
 * ------------------------------------------------------------------ */

void shell_init(void)
{
    wl_global_create(sway.display, &wl_compositor_interface, 6, NULL,
                     compositor_bind);
    wl_global_create(sway.display, &wl_shm_interface, 1, NULL, shm_bind);
    wl_global_create(sway.display, &xdg_wm_base_interface, 6, NULL,
                     wm_base_bind);

    sway.seat_global = wl_global_create(sway.display, &wl_seat_interface, 9,
                                        NULL, seat_bind);
    sway.output_global = wl_global_create(sway.display, &wl_output_interface, 4,
                                          NULL, output_bind);
}
