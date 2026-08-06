/* sway -- a tiling Wayland compositor for WOS.
 *
 * This is sway built for this machine rather than sway ported to it, and the
 * difference is worth being clear about.  Upstream sway is fifty thousand
 * lines on top of wlroots, which wants DRM, GBM, EGL, libinput, xkbcommon,
 * pixman and a libc; none of those exist here and several of them cannot,
 * because the kernel runs with the floating-point unit switched off.  What is
 * here is sway's design, on WOS's terms:
 *
 *   - the same wire protocol, through the same libwayland-shaped library, so
 *     the clients are real Wayland clients
 *   - the same tree: outputs hold workspaces, workspaces hold a tree of split
 *     containers, containers hold windows, and a window never overlaps another
 *   - the same configuration file, in the same language, read from the same
 *     place -- ~/.config/sway/config
 *   - the same IPC socket, so swaymsg works
 *
 * What it has not got is anything that needs the pieces above: no floating
 * windows (there is no pointer to drag them with), no XWayland, no output
 * hotplug, no swaybar as a separate process -- the bar is drawn here.  Those
 * are named where they come up rather than quietly missing.
 */
#ifndef SWAY_H
#define SWAY_H

#include <wkernel.h>
#include <wayland-server.h>

#define MAX_WORKSPACES 10
#define MAX_VARS       32
#define TITLE_MAX      64
#define COMMAND_MAX    192

/* The background text as it is written, and as it reads once ${CPU} and ${MEM}
 * have been replaced.  The second is the larger of the two because expanding
 * makes it longer, and a template that filled the buffer would otherwise be
 * cut off at exactly the moment a figure went from one digit to two. */
#define BACKGROUND_TEXT_MAX 128
#define BACKGROUND_LINE_MAX 224

/* The smallest and largest the cursor goes.  Whole multiples, because the
 * shape is a bitmap and half a pixel of arrow is not a thing that can be
 * drawn; four is as large as a 640x400 screen can carry without the pointer
 * covering what it is pointing at. */
#define CURSOR_SIZE_MIN 1
#define CURSOR_SIZE_MAX 4

/* How a container divides itself between its children.  i3 and sway call these
 * splith and splitv, and so does the configuration file. */
enum layout {
    L_SPLITH = 0,   /* children side by side  */
    L_SPLITV = 1,   /* children stacked       */
};

enum direction { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN };

/* `bar mode dock|hide|invisible`, which is sway's own set. */
enum bar_mode {
    BAR_INVISIBLE = 0,     /* not drawn at all                            */
    BAR_DOCK      = 1,     /* always there, with the screen made smaller  */
    BAR_HIDE      = 2,     /* over the windows, while the modifier is held */
};

/* ------------------------------------------------------------------ *
 *  Client objects
 * ------------------------------------------------------------------ */

/* A wl_shm_pool: a client's shared memory, mapped here as well.
 *
 * Reference counted because a pool outlives its resource: a client may destroy
 * the pool object while buffers carved out of it are still on the screen, and
 * the protocol says the buffers keep working. */
struct pool {
    int       fd;
    uint8_t  *data;
    uint32_t  size;
    int       refs;
};

/* A wl_buffer: a rectangle inside a pool. */
struct buffer {
    struct pool        *pool;
    struct wl_resource *resource;
    int32_t             offset, width, height, stride;
    uint32_t            format;
    int                 released;
};

struct node;

/* A window: an xdg_toplevel with a surface under it. */
struct view {
    struct wl_client   *client;
    struct wl_resource *surface;
    struct wl_resource *xdg_surface;
    struct wl_resource *toplevel;

    char title[TITLE_MAX];
    char app_id[TITLE_MAX];

    /* The double buffering the protocol is built on: a client attaches and
     * damages and only then commits, and nothing it did takes effect until it
     * does. */
    struct buffer *pending;
    int            pending_set;
    struct buffer *current;

    int x, y, w, h;          /* what the layout gave it */
    int mapped;              /* it has committed a buffer, so it can be drawn */
    uint32_t last_serial;    /* the configure it has not acknowledged yet */

    struct node *node;
    struct view *next;
};

/* ------------------------------------------------------------------ *
 *  The tree
 *
 *  A node is either a window or a split holding other nodes.  That is the
 *  whole of i3's layout model, and everything a tiling compositor does is a
 *  small operation on it.
 * ------------------------------------------------------------------ */

struct node {
    int          is_view;
    enum layout  layout;         /* when it is a split */

    struct node *parent;
    struct node *children;       /* first child   */
    struct node *next;           /* next sibling  */

    struct view *view;           /* when it is a window */

    int x, y, w, h;              /* worked out by arrange() */
};

struct workspace {
    int          number;
    char         name[16];
    struct node *root;           /* always a split */
    struct node *focus;          /* a leaf, or NULL when empty */
    struct view *fullscreen;
};

/* ------------------------------------------------------------------ *
 *  Configuration
 * ------------------------------------------------------------------ */

struct binding {
    uint32_t        code;
    uint32_t        mods;
    char            command[COMMAND_MAX];
    struct binding *next;
};

struct colours {
    uint32_t border;
    uint32_t background;
    uint32_t text;
    uint32_t indicator;
    uint32_t child_border;
};

struct config {
    uint32_t mod;                /* what $mod resolved to */
    char     terminal[64];       /* what $term resolved to, for the message */

    int      border_width;
    int      title_height;       /* 0 for `default_border none` */
    int      gaps_inner;
    int      gaps_outer;

    uint32_t background;
    struct colours focused;
    struct colours unfocused;

    /* What the bar is doing.  `hide` is sway's third mode: the bar is not
     * there and takes no room, and appears over the windows while the
     * modifier is held -- which is the one thing here that reads the modifier
     * without a binding matching. */
    enum bar_mode bar;
    int      bar_top;            /* at the top rather than the bottom */

    /* What the right-hand side of the bar says, with the same ${...} names the
     * background text uses.  Empty is the battery and the clock, which is what
     * a machine nobody has configured shows. */
    char     status_text[BACKGROUND_TEXT_MAX];

    /* `input <id> pointer_accel <-1..1>`, kept in hundredths because there is
     * no floating point here: -100 is sway's -1.0 and 100 is its 1.0.  What it
     * means for the mouse is worked out when it is set and handed to the
     * kernel, which is what turns counts into pixels. */
    int      pointer_accel;

    /* The cursor.  There is one shape and it is drawn here, so how big it is
     * and what colour it is are the compositor's to decide -- an X cursor
     * theme would answer both and there is no theme, no image decoder and
     * nothing to scale an image with. */
    int      cursor_size;        /* whole-pixel scale of the shape, 1 to 4 */
    uint32_t cursor_colour;      /* what the inside of the arrow is filled
                                  * with; the outline stays black so that it
                                  * is visible whatever it is over */

    /* What is written across the background, with ${CPU} and ${MEM} replaced
     * as they change.  Empty means the key hints a fresh machine shows. */
    char     background_text[BACKGROUND_TEXT_MAX];
};

/* How tall the bar is.  One number, so the row it is drawn on and the rows
 * left for windows cannot disagree -- they did, and a bar at the top was drawn
 * over the window under it. */
#define BAR_HEIGHT 20

/* ------------------------------------------------------------------ *
 *  The compositor
 * ------------------------------------------------------------------ */

struct sway {
    struct wl_display *display;
    struct wl_global  *seat_global;
    struct wl_global  *output_global;

    wdisplay_t screen;
    uint32_t  *back;             /* everything is composed here first */
    int        input_fd;
    int        ipc_fd;

    int  running;
    int  dirty;                  /* something changed; the screen needs redoing */
    int  usable_y, usable_h;     /* the screen minus the bar */

    struct workspace workspaces[MAX_WORKSPACES];
    int              current;

    struct view    *views;
    struct binding *bindings;
    struct config   config;

    /* Keyboards clients have asked for, so a key can be sent to the focused
     * one.  Held as resources rather than per client, because one client may
     * have several. */
    struct wl_resource *keyboards[16];
    int                 keyboard_count;

    /* The same, for pointers.  Kept apart from the keyboards because a client
     * may hold one and not the other, and an event for the wrong kind of
     * object is a protocol error rather than something ignored. */
    struct wl_resource *pointers[16];
    int                 pointer_count;

    /* Where the cursor is, and what it is over.  The position is the
     * kernel's -- it clamps to the screen -- and mirrored here so the frame
     * can be drawn without asking. */
    int          cursor_x, cursor_y;
    int          have_pointer;       /* the machine has a mouse at all */
    struct view *pointer_focus;      /* the view the pointer is inside */

    struct view *focused;        /* which view has the keyboard */
    uint32_t     mods;           /* modifiers held right now */

    char config_path[W_PATH_MAX + 1];
    char status[128];            /* the message the bar shows */

    /* The background text with its figures filled in, kept apart from the
     * template it came from: the template is the person's and changes when
     * they say so, and this changes on its own. */
    char background_line[BACKGROUND_LINE_MAX];
};

extern struct sway sway;

/* --- layout.c --- */
struct workspace *ws_current(void);
void   layout_init(void);
void   layout_add_view(struct view *v);
void   layout_remove_view(struct view *v);
void   layout_arrange(void);
void   layout_focus(struct view *v);
void   layout_focus_direction(enum direction dir);
void   layout_move_direction(enum direction dir);
void   layout_split(enum layout how);
void   layout_toggle_split(void);
void   layout_set_layout(enum layout how);
void   layout_switch_workspace(int number);
void   layout_move_to_workspace(int number);
int    layout_view_count(const struct workspace *ws);
int    layout_workspace_of(const struct view *v);
struct node *layout_first_leaf(struct node *n);

/* --- render.c --- */
void render_frame(void);
int  render_text_width(const char *s);

/* --- shell.c --- */
void shell_init(void);
/* The pointer, into whatever surface it is over.  `sx`/`sy` are surface-local:
 * measured from the client's own top-left corner, not the screen's, and not
 * from the frame the compositor drew around it. */
void shell_pointer_enter(struct view *v, int sx, int sy);
void shell_pointer_leave(struct view *v);
void shell_pointer_motion(struct view *v, uint32_t time_ms, int sx, int sy);
void shell_pointer_button(struct view *v, uint32_t time_ms, uint32_t button,
                          uint32_t state);
void shell_pointer_axis(struct view *v, uint32_t time_ms, uint32_t axis,
                        int steps);

void shell_send_key(struct view *v, uint32_t code, uint32_t state,
                    uint32_t time_ms);
void shell_send_modifiers(struct view *v, uint32_t mods);
void shell_focus_changed(struct view *old, struct view *now);
void shell_configure(struct view *v);
void shell_close(struct view *v);
void shell_release_buffer(struct buffer *b);
void shell_frame_done(void);

/* --- config.c --- */
void config_defaults(void);
int  config_load(const char *path);
void config_run_command(const char *command);
const char *config_expand(const char *word);

/* --- ipc.c --- */
int  ipc_init(const char *path);
void ipc_poll_fds(wpollfd_t *out, int *n, int max);
void ipc_handle(const wpollfd_t *fds, int count);
void ipc_shutdown(void);

/* --- sway.c --- */
void sway_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sway_spawn(const char *command);
void sway_update_usable(void);
void sway_update_background_text(void);

/* Is the bar on the screen right now?  Always in `dock`, never in
 * `invisible`, and while the modifier is held in `hide`. */
int  sway_bar_showing(void);


#endif /* SWAY_H */
