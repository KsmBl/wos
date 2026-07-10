/* wlterm -- a terminal emulator that is a Wayland client.
 *
 * It runs a shell and shows what the shell prints, which is what every
 * terminal emulator does.  What makes it worth having is the other half: it is
 * an ordinary Wayland client, written the way any Wayland client is written,
 * and the compositor knows nothing about it beyond what the protocol says.  It
 * asks for a surface, makes it a window, puts pixels in shared memory and hands
 * over the descriptor; it is told how big it is and where the keys went.
 *
 * The terminal itself is not new.  `struct wterm` already runs a child through
 * a pair of pipes and interprets its output -- the control characters, the ANSI
 * sequences, the colours -- into a grid of cells, because vim's :term and the
 * `split` command needed exactly that.  What it could not do is draw anywhere
 * but the text console, so this reads the grid and paints it.
 *
 * Two buffers, alternating.  A client must not draw into a buffer the
 * compositor is still reading, and wl_buffer.release is how it is told which
 * one that is; drawing into the other one is the whole of double buffering.
 */

#include <wkernel.h>
#include <wterm.h>
#include <wayland-client.h>

#define CELL_W 8
#define CELL_H 16

#define MIN_COLS 20
#define MIN_ROWS 4

/* The console's sixteen colours, so a program looks the same in a window as it
 * does on the console it was written for.
 *
 * Indexed the way `struct wterm` stores them, which is ANSI's order (black,
 * red, green, yellow, blue, magenta, cyan, white) and the order the W_* names
 * use -- not the VGA hardware order, which puts blue at 1 and red at 4.  The
 * two look identical until something asks for cyan and gets brown. */
static const uint32_t palette[16] = {
    0x000000,  /* black         */
    0xAA0000,  /* red           */
    0x00AA00,  /* green         */
    0xAA5500,  /* yellow, which on a VGA palette is brown */
    0x0000AA,  /* blue          */
    0xAA00AA,  /* magenta       */
    0x00AAAA,  /* cyan          */
    0xAAAAAA,  /* white         */
    0x555555,  /* bright black  */
    0xFF5555,  /* bright red    */
    0x55FF55,  /* bright green  */
    0xFFFF55,  /* bright yellow */
    0x5555FF,  /* bright blue   */
    0xFF55FF,  /* bright magenta*/
    0x55FFFF,  /* bright cyan   */
    0xFFFFFF,  /* bright white  */
};

struct frame {
    struct wl_buffer *buffer;
    uint32_t         *pixels;
    int               busy;        /* the compositor still has it */
};

static struct {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct wl_seat       *seat;
    struct wl_keyboard   *keyboard;
    struct xdg_wm_base   *wm_base;

    struct wl_surface  *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;

    int shm_fd;
    uint8_t *pool_data;
    struct wl_shm_pool *pool;
    struct frame frames[2];
    int   pool_bytes;

    int width, height;             /* what the compositor last told us */
    int configured;                /* it has told us at least once     */
    int running;
    int redraw;

    uint32_t mods;
    struct wterm term;
    char     title[64];
} app;

/* ------------------------------------------------------------------ *
 *  Drawing
 * ------------------------------------------------------------------ */

static void draw_glyph(uint32_t *pixels, int stride_px, int x, int y, char c,
                       uint32_t fg, uint32_t bg)
{
    const unsigned char *glyph = wglyph8x16((unsigned char)c);

    for (int row = 0; row < CELL_H; row++) {
        uint32_t     *at   = pixels + (uint64_t)(y + row) * stride_px + x;
        unsigned char bits = glyph[row];

        for (int col = 0; col < CELL_W; col++)
            at[col] = (bits & (0x80 >> col)) ? fg : bg;
    }
}

/* The whole grid, every time.  A window here is a few hundred cells and the
 * memory is ordinary RAM; tracking which of them changed would cost more in
 * bookkeeping than it saves in stores. */
static void draw(struct frame *f)
{
    int cols = app.width / CELL_W;
    int rows = app.height / CELL_H;

    if (cols > app.term.cols) cols = app.term.cols;
    if (rows > app.term.rows) rows = app.term.rows;

    /* Anything the grid does not cover -- the few pixels left when the window
     * is not a whole number of cells. */
    for (int y = 0; y < app.height; y++)
        for (int x = 0; x < app.width; x++)
            f->pixels[(uint64_t)y * app.width + x] = palette[0];

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int fg = app.term.fg[r][c];
            int bg = app.term.bg[r][c];

            if (fg < 0 || fg > 15) fg = W_WHITE;
            if (bg < 0 || bg > 15) bg = W_BLACK;

            draw_glyph(f->pixels, app.width, c * CELL_W, r * CELL_H,
                       app.term.ch[r][c], palette[fg], palette[bg]);
        }
    }

    /* The cursor, as a block on the last two rows of its cell -- the same
     * shape the console draws, so the two look like one machine. */
    if (app.term.cursor_visible && app.term.cy < rows && app.term.cx < cols) {
        int x = app.term.cx * CELL_W;
        int y = app.term.cy * CELL_H;

        for (int row = CELL_H - 2; row < CELL_H; row++)
            for (int col = 0; col < CELL_W; col++)
                f->pixels[(uint64_t)(y + row) * app.width + x + col] =
                    palette[W_WHITE];
    }
}

/* ------------------------------------------------------------------ *
 *  Buffers
 * ------------------------------------------------------------------ */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct frame *f = data;
    f->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void pool_release(void)
{
    for (int i = 0; i < 2; i++) {
        if (app.frames[i].buffer)
            wl_buffer_destroy(app.frames[i].buffer);
        app.frames[i].buffer = NULL;
        app.frames[i].pixels = NULL;
        app.frames[i].busy   = 0;
    }

    if (app.pool) {
        wl_shm_pool_destroy(app.pool);
        app.pool = NULL;
    }
    if (app.pool_data) {
        wshmunmap(app.pool_data);
        app.pool_data = NULL;
    }
    if (app.shm_fd >= 0) {
        wclose(app.shm_fd);
        app.shm_fd = -1;
    }
}

/* One pool holding two buffers, which is the usual arrangement: the memory is
 * shared once and carved up on this side. */
static int pool_create(void)
{
    int stride = app.width * 4;
    int one    = stride * app.height;

    pool_release();

    app.pool_bytes = one * 2;
    app.shm_fd     = wshmopen((unsigned int)app.pool_bytes);
    if (app.shm_fd < 0) {
        wfprintf(W_STDERR, "wlterm: no memory for a %dx%d window: %s\n",
                 app.width, app.height, wstrerror(-app.shm_fd));
        return -1;
    }

    app.pool_data = wshmmap(app.shm_fd);
    if (!app.pool_data) {
        wclose(app.shm_fd);
        app.shm_fd = -1;
        return -1;
    }

    app.pool = wl_shm_create_pool(app.shm, app.shm_fd, app.pool_bytes);
    if (!app.pool)
        return -1;

    for (int i = 0; i < 2; i++) {
        app.frames[i].buffer = wl_shm_pool_create_buffer(
                app.pool, i * one, app.width, app.height, stride,
                WL_SHM_FORMAT_XRGB8888);
        if (!app.frames[i].buffer)
            return -1;

        app.frames[i].pixels = (uint32_t *)(app.pool_data + (wsize_t)i * one);
        app.frames[i].busy   = 0;

        wl_buffer_add_listener(app.frames[i].buffer, &buffer_listener,
                               &app.frames[i]);
    }

    return 0;
}

static void present(void)
{
    if (!app.configured || !app.pool)
        return;

    struct frame *f = NULL;
    for (int i = 0; i < 2; i++)
        if (!app.frames[i].busy) {
            f = &app.frames[i];
            break;
        }

    /* Both still with the compositor.  Skipping the frame is right: the next
     * release brings one back, and drawing into a buffer being read would show
     * half of each. */
    if (!f)
        return;

    draw(f);

    wl_surface_attach(app.surface, f->buffer, 0, 0);
    wl_surface_damage(app.surface, 0, 0, app.width, app.height);
    wl_surface_commit(app.surface);

    f->busy    = 1;
    app.redraw = 0;
}

/* ------------------------------------------------------------------ *
 *  Being told how big to be
 * ------------------------------------------------------------------ */

static void resize_terminal(void)
{
    int cols = app.width / CELL_W;
    int rows = app.height / CELL_H;

    if (cols < MIN_COLS) cols = MIN_COLS;
    if (rows < MIN_ROWS) rows = MIN_ROWS;
    if (cols > WTERM_MAX_C) cols = WTERM_MAX_C;
    if (rows > WTERM_MAX_R) rows = WTERM_MAX_R;

    if (rows == app.term.rows && cols == app.term.cols)
        return;

    wterm_resize(&app.term, rows, cols, 1, 1);

    /* And tell the program: a shell that thinks it has eighty columns in a
     * window with forty will wrap in the wrong places. */
    if (app.term.pid > 0)
        wsetsize(app.term.pid, rows, cols);
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    /* Zero means "you choose", which happens before a tiling compositor has
     * decided or when a window is floating. */
    if (width > 0)
        app.width = width;
    if (height > 0)
        app.height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    app.running = 0;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel,
                            int32_t width, int32_t height)
{
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel,
                                  struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure, toplevel_close, toplevel_bounds, toplevel_capabilities,
};

static void surface_configure(void *data, struct xdg_surface *surface,
                              uint32_t serial)
{
    static int last_w, last_h;

    /* Acknowledged first.  The serial says which configure is being answered,
     * and the compositor is entitled to the answer before the buffer. */
    xdg_surface_ack_configure(surface, serial);

    if (app.width != last_w || app.height != last_h || !app.pool) {
        last_w = app.width;
        last_h = app.height;

        if (pool_create() < 0) {
            app.running = 0;
            return;
        }
        resize_terminal();
    }

    app.configured = 1;
    present();
}

static const struct xdg_surface_listener surface_listener = {
    surface_configure,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    /* Answering is what tells the compositor this client is not wedged. */
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

/* ------------------------------------------------------------------ *
 *  Keys
 * ------------------------------------------------------------------ */

/* The keys that are not characters.  wterm speaks the same W_KEY_* codes the
 * console does, so this is the one translation needed between what a Wayland
 * keyboard reports and what the emulator expects. */
static int special_key(uint32_t code)
{
    switch (code) {
    case 103: return W_KEY_UP;
    case 108: return W_KEY_DOWN;
    case 105: return W_KEY_LEFT;
    case 106: return W_KEY_RIGHT;
    case 102: return W_KEY_HOME;
    case 107: return W_KEY_END;
    case 104: return W_KEY_PGUP;
    case 109: return W_KEY_PGDN;
    case 111: return W_KEY_DELETE;
    case 1:   return W_KEY_ESCAPE;
    default:  return 0;
    }
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    int special = special_key(key);
    if (special) {
        wterm_input(&app.term, special);
        return;
    }

    uint32_t c = wkeychar(key, app.mods);
    if (c)
        wterm_input(&app.term, (int)c);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    /* The compositor sends XKB's own bit positions, which is what wkeychar()
     * reads, so there is nothing to translate. */
    app.mods = depressed | locked;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    /* This compositor sends "no keymap" and the evdev codes directly, which
     * wkeychar() knows how to read.  The descriptor still arrives and is still
     * ours to close. */
    if (fd >= 0)
        wclose(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    app.redraw = 1;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    /* Every modifier is forgotten, because a release that happens while
     * another window has the keyboard is never seen here. */
    app.mods = 0;
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard,
                            int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key,
    keyboard_modifiers, keyboard_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
        app.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app.keyboard, &keyboard_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    seat_capabilities, seat_name,
};

/* ------------------------------------------------------------------ *
 *  The registry
 * ------------------------------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
}

static const struct wl_shm_listener shm_listener = { shm_format };

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    if (strcmp(interface, "wl_compositor") == 0) {
        app.compositor = wl_registry_bind(registry, name,
                                          &wl_compositor_interface,
                                          version < 4 ? version : 4);
    } else if (strcmp(interface, "wl_shm") == 0) {
        app.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(app.shm, &shm_listener, NULL);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        app.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                       version < 2 ? version : 2);
        xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, NULL);
    } else if (strcmp(interface, "wl_seat") == 0) {
        app.seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                    version < 5 ? version : 5);
        wl_seat_add_listener(app.seat, &seat_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove,
};

/* ------------------------------------------------------------------ *
 *  Starting up
 * ------------------------------------------------------------------ */

static void start_shell(int argc, char **argv)
{
    char shell[W_SHELL_MAX + 1];
    char *args[8];
    int   n = 0;

    /* Whatever the user's login shell is, so a window opens the same thing a
     * console login would. */
    if (wgetshell(-1, shell, sizeof(shell)) < 0 || !shell[0])
        strlcpy(shell, "/app/whell/launch", sizeof(shell));

    /* Everything after `-e` is the program to run instead, which is the option
     * every terminal emulator has. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            strlcpy(shell, argv[i + 1], sizeof(shell));
            if (!strchr(shell, '/'))
                wsnprintf(shell, sizeof(shell), "/app/%s/launch", argv[i + 1]);

            for (int j = i + 1; j < argc && n < 7; j++)
                args[n++] = argv[j];
            break;
        }
    }

    if (n == 0) {
        /* What the program is called.  Every WOS program is installed as
         * /app/<name>/launch, so the last component is "launch" for all of
         * them and the name is the one before it. */
        static char base[W_NAME_MAX + 1];
        const char *last = strrchr(shell, '/');

        if (last && strcmp(last, "/launch") == 0) {
            const char *start = shell;
            for (const char *s = shell; s < last; s++)
                if (*s == '/')
                    start = s + 1;

            wsize_t len = (wsize_t)(last - start);
            if (len >= sizeof(base))
                len = sizeof(base) - 1;
            memcpy(base, start, len);
            base[len] = '\0';
        } else {
            strlcpy(base, last ? last + 1 : shell, sizeof(base));
        }

        args[n++] = base;
    }
    args[n] = NULL;

    int cols = app.width / CELL_W;
    int rows = app.height / CELL_H;

    if (cols < MIN_COLS) cols = MIN_COLS;
    if (rows < MIN_ROWS) rows = MIN_ROWS;

    int r = wterm_start(&app.term, shell, args, 1, 1, rows, cols);
    if (r < 0) {
        wfprintf(W_STDERR, "wlterm: cannot start %s: %s\n", shell,
                 wstrerror(-r));
        app.running = 0;
        return;
    }

    strlcpy(app.title, args[0], sizeof(app.title));
}

int main(int argc, char **argv)
{
    app.width   = 640;
    app.height  = 400;
    app.shm_fd  = -1;
    app.running = 1;

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        wfprintf(W_STDERR, "wlterm: no display server to connect to\n");
        wfprintf(W_STDERR, "wlterm: is sway running? "
                           "`systemctl status sway`\n");
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, NULL);

    /* Two roundtrips: the first brings the list of globals, the second the
     * events the things bound in the first one sent back. */
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.wm_base) {
        wfprintf(W_STDERR, "wlterm: that display server has no %s\n",
                 !app.compositor ? "wl_compositor"
                 : !app.shm      ? "wl_shm" : "xdg_wm_base");
        wfprintf(W_STDERR, "wlterm: it cannot show a window, so there is "
                           "nothing to do here.\n");
        return 1;
    }

    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, NULL);

    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);

    xdg_toplevel_set_title(app.toplevel, "wlterm");
    xdg_toplevel_set_app_id(app.toplevel, "wlterm");

    /* A commit with no buffer: it says the window is ready to be told its
     * size, and nothing can be drawn until it has been. */
    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    start_shell(argc, argv);
    if (app.running && app.title[0])
        xdg_toplevel_set_title(app.toplevel, app.title);

    while (app.running) {
        wl_display_flush(app.display);

        wpollfd_t watch[2];
        int       n = 0;

        watch[n].fd      = wl_display_get_fd(app.display);
        watch[n].events  = W_POLLIN;
        watch[n].revents = 0;
        n++;

        if (app.term.open) {
            watch[n].fd      = app.term.out_r;
            watch[n].events  = W_POLLIN;
            watch[n].revents = 0;
            n++;
        }

        if (wpoll(watch, n, 200) > 0) {
            if (watch[0].revents & W_POLLIN) {
                if (wl_display_dispatch(app.display) < 0)
                    break;
            }
            if (n > 1 && (watch[1].revents & (W_POLLIN | W_POLLHUP))) {
                if (!wterm_pump(&app.term))
                    break;             /* the shell exited: so does the window */
                app.redraw = 1;
            }
        }

        /* Keys go to the child, and what it printed comes back through the
         * same pump -- so a keystroke is not a reason to redraw by itself. */
        if (app.term.open && app.term.dirty) {
            app.term.dirty = 0;
            app.redraw     = 1;
        }

        if (app.redraw)
            present();
    }

    if (app.term.open)
        wterm_close(&app.term);

    pool_release();
    wl_display_disconnect(app.display);
    return 0;
}
