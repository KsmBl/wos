/* thunar -- a graphical file manager for the Wayland session.
 *
 *     thunar            open the working directory
 *     thunar /app       open somewhere else
 *
 * A WOS-native program in the spirit of Xfce's Thunar, not a build of the real
 * one: that wants GTK, GIO, gvfs and an icon theme.  What is kept is the shape
 * -- a places sidebar on the left, a file list on the right, a location bar
 * across the top and a status line along the bottom -- and the manner: it is
 * quiet, it starts immediately, and it does the obvious thing.
 *
 * It is an ordinary Wayland client.  There is no compositor privilege here and
 * nothing sway knows about it beyond what the protocol says: it asks for a
 * surface, makes it a toplevel, draws into shared memory and hands over the
 * descriptor.  The drawing is its own, done into that buffer a pixel at a time
 * with the console's 8x16 font, because the only text this machine has is the
 * one in the kernel and there is no toolkit to ask.
 *
 * Two things are genuinely different from the Thunar people know, and both are
 * properties of this machine rather than of this program.
 *
 * It is driven from the keyboard.  WOS has no mouse -- there is no pointer
 * device in the kernel, and sway's seat advertises a keyboard and nothing else
 * -- so there is no clicking, no drag and drop and no context menu.  Every
 * command is a key, the focused pane is drawn with a visible ring so that
 * "which list do the arrows move?" is answerable by looking, and Tab moves
 * between the two.  A file manager that waited for a click would sit there.
 *
 * And opening a file hands it to another program rather than to a handler
 * registered by a desktop file.  There is no MIME database and no
 * associations, so the rule is the one this system can actually justify: a
 * directory is entered, a file called `launch` is a WOS program and is run,
 * and anything else is opened in vim in a new terminal window.  Thunar would
 * ask the desktop what to use; this says what it will do and does that.
 */

#include <wkernel.h>
#include <wayland-client.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ *
 *  Metrics
 *
 *  In pixels, not in cells.  A file manager is not a grid of characters --
 *  the rows are taller than the font so the icons have room to breathe, and
 *  the sidebar is as wide as its longest label rather than a round number of
 *  columns.
 * ------------------------------------------------------------------ */

#define CELL_W       8             /* the kernel font, which is all there is */
#define CELL_H       16

#define TOOLBAR_H    34            /* the location bar across the top */
#define STATUS_H     24            /* the summary along the bottom    */
#define SIDEBAR_W    152
#define ROW_H        20            /* one file, one place             */
#define ICON         16
#define PAD          8

/* Below this the sidebar goes away and the list gets the whole width.  A
 * tiling compositor on a 640x400 screen gives a second window 320 pixels, and
 * a places pane 152 of those wide would leave the file names too little to be
 * read: the shortcuts are the part of the window worth losing first, because
 * every one of them is also reachable by walking there. */
#define SIDEBAR_MIN  (SIDEBAR_W + 220)

#define MIN_WIDTH    176
#define MIN_HEIGHT   (TOOLBAR_H + STATUS_H + ROW_H)

#define MAX_ENTRIES  512
#define MAX_PLACES   8

/* The palette.  Light, flat and grey-blue: near enough to the Adwaita that
 * Thunar sits on for the shape to read as the same kind of window, without
 * pretending to be a theme engine. */
#define C_WINDOW     0xF6F5F4
#define C_SIDEBAR    0xEBE8E6
#define C_HEADER     0xDEDAD6
#define C_BORDER     0xC4C0BC
#define C_TEXT       0x2E3436
#define C_DIM        0x8A8E8F
#define C_SELECT     0x3584E4      /* the selection in the focused pane   */
#define C_SELECT_OFF 0xCFCBC7      /* and in the one that is not          */
#define C_SEL_TEXT   0xFFFFFF
#define C_FOLDER     0x6DA6EE
#define C_FOLDER_TAB 0x4A8BD8
#define C_FOLDER_EDG 0x2F6DB5
#define C_PAPER      0xFDFDFD
#define C_PAPER_EDG  0x9A9996
#define C_PAPER_TXT  0xC8C6C3
#define C_ENTRY      0xFFFFFF      /* the location bar's sunken field     */
#define C_WARN       0xC01C28

/* ------------------------------------------------------------------ *
 *  What is on screen
 * ------------------------------------------------------------------ */

struct entry {
    char     name[W_NAME_MAX + 1];
    uint32_t size;
    int      is_dir;
    int      is_parent;            /* the ".." that leads out of here */
};

struct place {
    char label[20];
    char path[W_PATH_MAX + 1];
    uint32_t accent;               /* places are told apart by colour, having
                                    * no icon theme to tell them apart with */
};

/* Which pane the arrow keys move. */
enum pane { PANE_PLACES, PANE_FILES };

/* A dialog is a mode rather than a nested loop.  Reading keys inside a
 * modal loop of its own would be shorter, but the connection would stop
 * being dispatched while it ran -- and a client that does not answer
 * xdg_wm_base.ping is a client the compositor is entitled to call hung. */
enum mode { MODE_BROWSE, MODE_INPUT, MODE_CONFIRM };

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
    struct wl_pointer    *pointer;
    struct xdg_wm_base   *wm_base;

    struct wl_surface   *surface;
    struct xdg_surface  *xdg_surface;
    struct xdg_toplevel *toplevel;

    int                 shm_fd;
    uint8_t            *pool_data;
    struct wl_shm_pool *pool;
    struct frame        frames[2];
    int                 pool_bytes;

    int width, height;
    int configured;
    int running;
    int redraw;

    uint32_t mods;

    /* Where the pointer is, tracked from the motion events.  A button event
     * does not carry a position -- the protocol says the pointer is wherever
     * the last motion put it -- so a click can only be placed by having
     * followed it there. */
    int      ptr_x, ptr_y;
    int      ptr_in;

    /* For telling a double click from two clicks: when and where the last one
     * was.  Both, because two clicks on different rows are two clicks however
     * quickly they arrive. */
    uint32_t last_click_ms;
    int      last_click_row;
} app;

static struct entry entries[MAX_ENTRIES];
static int          count;
static int          selected;
static int          top;           /* first row shown */
static int          truncated;     /* the directory had more than we hold */

static struct place places[MAX_PLACES];
static int          place_count;
static int          place_at;

static enum pane focus = PANE_FILES;
static enum mode mode  = MODE_BROWSE;

static char path[W_PATH_MAX + 1] = "/";
static char status[160];
static int  status_bad;            /* it is a complaint, not a notice */
static char title[80];

/* The dialog, when there is one. */
static char dialog_prompt[64];
static char dialog_text[W_NAME_MAX + 1];
static int  dialog_len;
static void (*dialog_done)(void);   /* what Enter means, this time */

/* Where the pixels for the frame being painted are.  Held in a global so the
 * drawing helpers do not each need it threaded through them; there is exactly
 * one paint in flight at a time. */
static uint32_t *px;

/* ------------------------------------------------------------------ *
 *  The status line
 *
 *  Two ways of saying something, because the difference matters when it is
 *  read at a glance: a notice is what just happened and a complaint is what
 *  did not.  Drawing both the same colour would make "created testdir" look
 *  like something had gone wrong.
 * ------------------------------------------------------------------ */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void complain(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    wvsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);

    status_bad = 0;
}

static void complain(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    wvsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);

    status_bad = 1;
}

static void say_nothing(void)
{
    status[0]  = '\0';
    status_bad = 0;
}

/* ------------------------------------------------------------------ *
 *  Drawing primitives
 *
 *  Every one of these clips.  A window can be told to be any size at all,
 *  including smaller than the thing being drawn into it, and a file manager
 *  that wrote outside its buffer when the layout did not fit would corrupt
 *  the pool rather than merely look wrong.
 * ------------------------------------------------------------------ */

static void fill(int x, int y, int w, int h, uint32_t colour)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > app.width)  w = app.width - x;
    if (y + h > app.height) h = app.height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++) {
        uint32_t *at = px + (uint64_t)(y + row) * app.width + x;
        for (int col = 0; col < w; col++)
            at[col] = colour;
    }
}

/* One glyph, with the background left alone.  wlterm paints a cell at a time
 * because a terminal has a background colour per character; here the text sits
 * on whatever is already there, which is what lets a label cross a selection
 * bar without carrying a rectangle of its own colour with it. */
static void draw_char(int x, int y, char c, uint32_t fg)
{
    const unsigned char *glyph = wglyph8x16((unsigned char)c);

    for (int row = 0; row < CELL_H; row++) {
        unsigned char bits = glyph[row];
        int           at_y = y + row;

        if (!bits || at_y < 0 || at_y >= app.height)
            continue;

        uint32_t *at = px + (uint64_t)at_y * app.width;

        for (int col = 0; col < CELL_W; col++) {
            int at_x = x + col;

            if ((bits & (0x80 >> col)) && at_x >= 0 && at_x < app.width)
                at[at_x] = fg;
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t fg)
{
    for (; *s; s++, x += CELL_W)
        draw_char(x, y, *s, fg);
}

/* Text that has to stay inside something.  A name too long for its column is
 * cut and ends in an ellipsis, so that a truncated name looks truncated rather
 * than looking like a shorter name that happens to exist. */
static void draw_text_fit(int x, int y, const char *s, uint32_t fg, int max_px)
{
    int room = max_px / CELL_W;
    int len  = (int)strlen(s);

    if (room <= 0)
        return;

    if (len <= room) {
        draw_text(x, y, s, fg);
        return;
    }

    if (room <= 3) {
        for (int i = 0; i < room; i++)
            draw_char(x + i * CELL_W, y, '.', fg);
        return;
    }

    for (int i = 0; i < room - 3; i++)
        draw_char(x + i * CELL_W, y, s[i], fg);
    for (int i = room - 3; i < room; i++)
        draw_char(x + i * CELL_W, y, '.', fg);
}

static int text_width(const char *s)
{
    return (int)strlen(s) * CELL_W;
}

static void draw_border(int x, int y, int w, int h, uint32_t colour)
{
    fill(x, y, w, 1, colour);
    fill(x, y + h - 1, w, 1, colour);
    fill(x, y, 1, h, colour);
    fill(x + w - 1, y, 1, h, colour);
}

/* ------------------------------------------------------------------ *
 *  Icons
 *
 *  Drawn rather than loaded.  There is no image decoder here and no icon
 *  theme to read one from, and two shapes at sixteen pixels are enough to
 *  answer the only question the list is really asked: is this a folder or is
 *  it a file?
 * ------------------------------------------------------------------ */

static void draw_folder_icon(int x, int y, uint32_t body)
{
    /* The tab along the top left, then the body under it -- the shape every
     * folder icon has had since they stopped being photographs of folders. */
    fill(x,     y + 2,  7,      3,  C_FOLDER_TAB);
    fill(x,     y + 4,  ICON,   10, body);
    fill(x,     y + 4,  ICON,   1,  C_FOLDER_TAB);
    draw_border(x, y + 2, ICON, 12, C_FOLDER_EDG);
}

static void draw_file_icon(int x, int y)
{
    /* A page with the top right corner turned down.  The corner is what makes
     * it read as paper rather than as an empty box. */
    fill(x + 2, y + 1, 11, 14, C_PAPER);
    draw_border(x + 2, y + 1, 11, 14, C_PAPER_EDG);

    for (int i = 0; i < 4; i++) {
        fill(x + 9 + i, y + 1, 4 - i, 1, C_WINDOW);
        fill(x + 12 - i, y + 1 + i, 1, 1, C_PAPER_EDG);
    }
    fill(x + 9, y + 5, 4, 1, C_PAPER_EDG);

    /* Three short rules, which is what writing looks like this small. */
    for (int i = 0; i < 3; i++)
        fill(x + 4, y + 7 + i * 3, 7, 1, C_PAPER_TXT);
}

/* ------------------------------------------------------------------ *
 *  Paths
 * ------------------------------------------------------------------ */

static void join(char *out, wsize_t size, const char *dir, const char *name)
{
    if (dir[0] == '/' && dir[1] == '\0')
        wsnprintf(out, size, "/%s", name);
    else
        wsnprintf(out, size, "%s/%s", dir, name);
}

static void parent_of(char *dir)
{
    char *slash = strrchr(dir, '/');

    if (!slash || slash == dir) {
        dir[0] = '/';
        dir[1] = '\0';
        return;
    }
    *slash = '\0';
}

static const char *basename_of(const char *p)
{
    const char *slash = strrchr(p, '/');

    if (!slash || !slash[1])
        return p;
    return slash + 1;
}

/* ------------------------------------------------------------------ *
 *  Reading a directory
 * ------------------------------------------------------------------ */

static int compare(const struct entry *a, const struct entry *b)
{
    if (a->is_parent != b->is_parent)
        return a->is_parent ? -1 : 1;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void sort_entries(void)
{
    /* Insertion sort, as `fm` uses: a directory here holds a few dozen names,
     * and this is shorter than anything cleverer and stable into the bargain. */
    for (int i = 1; i < count; i++) {
        struct entry key = entries[i];
        int          j   = i - 1;

        while (j >= 0 && compare(&entries[j], &key) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void set_title(void)
{
    const char *base = basename_of(path);

    /* The folder's name, the way a file manager titles its window -- and the
     * program's name when that folder is the root and has no name of its
     * own. */
    if (strcmp(path, "/") == 0)
        strlcpy(title, "thunar", sizeof(title));
    else
        wsnprintf(title, sizeof(title), "%s - thunar", base);

    if (app.toplevel)
        xdg_toplevel_set_title(app.toplevel, title);
}

static void load(void)
{
    count     = 0;
    truncated = 0;

    int dir = wopendir(path);
    if (dir < 0) {
        complain("%s: %s", path, wstrerror(-dir));
        return;
    }

    if (strcmp(path, "/") != 0) {
        strlcpy(entries[count].name, "..", sizeof(entries[count].name));
        entries[count].is_dir    = 1;
        entries[count].is_parent = 1;
        entries[count].size      = 0;
        count++;
    }

    wdirent_t e;
    while (wreaddir(dir, &e) == 1) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;

        if (count >= MAX_ENTRIES) {
            truncated++;
            continue;
        }

        strlcpy(entries[count].name, e.name, sizeof(entries[count].name));
        entries[count].is_dir    = (e.type == W_FT_DIR);
        entries[count].is_parent = 0;
        entries[count].size      = 0;

        if (!entries[count].is_dir) {
            char    full[W_PATH_MAX + 1];
            wstat_t st;

            join(full, sizeof(full), path, e.name);
            if (wstat(full, &st) == 0)
                entries[count].size = st.size;
        }

        count++;
    }

    wclosedir(dir);
    sort_entries();

    if (selected >= count)
        selected = count ? count - 1 : 0;
    top = 0;

    /* The working directory follows what is being looked at, so that a
     * terminal opened from here opens here, and so does anything it starts.
     * Failing is not worth reporting: it only means this process may not sit
     * in that directory, and the listing above was read anyway. */
    wchdir(path);
    set_title();
}

static void go_to(const char *where)
{
    if (strlen(where) > W_PATH_MAX) {
        complain("that path is longer than the system allows");
        return;
    }

    wstat_t st;
    if (wstat(where, &st) < 0 || st.type != W_FT_DIR) {
        complain("%s is not a directory", where);
        return;
    }

    strlcpy(path, where, sizeof(path));
    selected = 0;
    top      = 0;
    load();
}

/* ------------------------------------------------------------------ *
 *  The places sidebar
 * ------------------------------------------------------------------ */

static void add_place(const char *label, const char *where, uint32_t accent)
{
    wstat_t st;

    if (place_count >= MAX_PLACES)
        return;

    /* Only what is actually there.  A shortcut to a directory this system does
     * not have is a promise the sidebar cannot keep. */
    if (wstat(where, &st) < 0 || st.type != W_FT_DIR)
        return;

    strlcpy(places[place_count].label, label, sizeof(places[place_count].label));
    strlcpy(places[place_count].path, where, sizeof(places[place_count].path));
    places[place_count].accent = accent;
    place_count++;
}

static void build_places(void)
{
    char    home[W_PATH_MAX + 1];
    wuser_t me;

    /* Home is wherever this user's directory is, which is what the kernel
     * says it is rather than what the shell was started in. */
    if (wuserinfo(wgetuid(), &me) == 0 && me.name[0])
        wsnprintf(home, sizeof(home), "/home/%s", me.name);
    else
        strlcpy(home, "/home", sizeof(home));

    add_place("Home",         home,         0x6DA6EE);
    add_place("Filesystem",   "/",          0x9A9996);
    add_place("Applications", "/app",       0x8FD07A);
    add_place("Services",     "/services",  0xE9B96E);
    add_place("RAM disk",     "/ramdisk",   0xD79BE8);
    add_place("Users",        "/userconfig", 0xE0A9A0);
}

/* ------------------------------------------------------------------ *
 *  Free space
 *
 *  Which filesystem the directory being shown is on, which is the one whose
 *  mount point is the longest prefix of it: "/ramdisk/x" is on /ramdisk and
 *  not on /, even though / is a prefix of it too.
 * ------------------------------------------------------------------ */

static void free_space(char *out, wsize_t size)
{
    wdisk_t disks[W_DISK_MAX];
    int     n    = wdisklist(disks, W_DISK_MAX);
    int     best = -1;
    wsize_t best_len = 0;

    out[0] = '\0';
    if (n <= 0)
        return;

    for (int i = 0; i < n; i++) {
        wsize_t len = strlen(disks[i].mount);

        if (strncmp(path, disks[i].mount, len) != 0)
            continue;

        /* "/ramdiskery" is not under "/ramdisk": a prefix only counts when it
         * ends where a path component ends. */
        if (len > 1 && path[len] != '\0' && path[len] != '/')
            continue;

        if (best < 0 || len > best_len) {
            best     = i;
            best_len = len;
        }
    }

    if (best < 0)
        return;

    wsnprintf(out, size, "%s free", whuman(disks[best].usage.free_bytes));
}

/* ------------------------------------------------------------------ *
 *  Painting
 * ------------------------------------------------------------------ */

/* How wide the sidebar is, which is zero when there is not room for it. */
static int sidebar_w(void)
{
    return app.width >= SIDEBAR_MIN ? SIDEBAR_W : 0;
}

static int list_x(void)      { return sidebar_w(); }
static int list_y(void)      { return TOOLBAR_H; }
static int list_width(void)  { return app.width - sidebar_w(); }
static int list_height(void) { return app.height - TOOLBAR_H - STATUS_H; }
static int rows_visible(void)
{
    int rows = list_height() / ROW_H;
    return rows > 0 ? rows : 1;
}

static void draw_toolbar(void)
{
    fill(0, 0, app.width, TOOLBAR_H, C_HEADER);
    fill(0, TOOLBAR_H - 1, app.width, 1, C_BORDER);

    /* The location bar: a sunken field with the path in it, as every file
     * manager has had since they stopped being two panes and a command line. */
    int x = PAD;
    int y = 6;
    int w = app.width - PAD * 2;
    int h = TOOLBAR_H - 12;

    if (w < 40)
        return;

    fill(x, y, w, h, C_ENTRY);
    draw_border(x, y, w, h, C_BORDER);

    draw_folder_icon(x + 4, y + (h - ICON) / 2, C_FOLDER);
    draw_text_fit(x + 4 + ICON + 6, y + (h - CELL_H) / 2, path, C_TEXT,
                  w - ICON - 20);
}

static void draw_sidebar(void)
{
    int y = TOOLBAR_H;

    if (!sidebar_w())
        return;

    fill(0, y, SIDEBAR_W, app.height - y - STATUS_H, C_SIDEBAR);
    fill(SIDEBAR_W - 1, y, 1, app.height - y - STATUS_H, C_BORDER);

    draw_text(PAD, y + 6, "PLACES", C_DIM);
    y += 6 + CELL_H + 6;

    for (int i = 0; i < place_count; i++) {
        int here = (i == place_at);
        int on   = (strcmp(places[i].path, path) == 0);

        if (y + ROW_H > app.height - STATUS_H)
            break;

        /* The selection is drawn in the focused pane's colour and in a muted
         * one otherwise, so which list the arrow keys will move is a thing
         * that can be seen rather than remembered. */
        if (here && focus == PANE_PLACES)
            fill(0, y, SIDEBAR_W - 1, ROW_H, C_SELECT);
        else if (here)
            fill(0, y, SIDEBAR_W - 1, ROW_H, C_SELECT_OFF);
        else if (on)
            fill(0, y, SIDEBAR_W - 1, ROW_H, C_WINDOW);

        uint32_t fg = (here && focus == PANE_PLACES) ? C_SEL_TEXT : C_TEXT;

        draw_folder_icon(PAD, y + (ROW_H - ICON) / 2, places[i].accent);
        draw_text_fit(PAD + ICON + 6, y + (ROW_H - CELL_H) / 2,
                      places[i].label, fg, SIDEBAR_W - PAD * 2 - ICON - 6);
        y += ROW_H;
    }
}

static void draw_list(void)
{
    int x      = list_x();
    int y0     = list_y();
    int w      = list_width();
    int h      = list_height();
    int rows   = rows_visible();

    fill(x, y0, w, h, C_WINDOW);

    /* Keep the selection on screen. */
    if (selected < top)
        top = selected;
    if (selected >= top + rows)
        top = selected - rows + 1;
    if (top < 0)
        top = 0;

    if (count == 0) {
        draw_text(x + PAD * 2, y0 + PAD * 2, "This folder is empty.", C_DIM);
        return;
    }

    for (int i = 0; i < rows; i++) {
        int index = top + i;
        int y     = y0 + i * ROW_H;

        if (index >= count)
            break;

        struct entry *e    = &entries[index];
        int           here = (index == selected);
        uint32_t      fg   = C_TEXT;

        if (here && focus == PANE_FILES) {
            fill(x, y, w, ROW_H, C_SELECT);
            fg = C_SEL_TEXT;
        } else if (here) {
            fill(x, y, w, ROW_H, C_SELECT_OFF);
        }

        if (e->is_dir)
            draw_folder_icon(x + PAD, y + (ROW_H - ICON) / 2, C_FOLDER);
        else
            draw_file_icon(x + PAD, y + (ROW_H - ICON) / 2);

        /* The size on the right, the name filling what is left of the row --
         * and the name shortened rather than allowed to run under the size. */
        char size_text[24];
        if (e->is_parent)
            strlcpy(size_text, "up", sizeof(size_text));
        else if (e->is_dir)
            strlcpy(size_text, "folder", sizeof(size_text));
        else
            strlcpy(size_text, whuman(e->size), sizeof(size_text));

        int size_w = text_width(size_text);
        int name_x = x + PAD + ICON + 8;
        int name_w = w - (name_x - x) - size_w - PAD * 2;

        draw_text_fit(name_x, y + (ROW_H - CELL_H) / 2, e->name, fg, name_w);
        draw_text(x + w - PAD - size_w, y + (ROW_H - CELL_H) / 2, size_text,
                  (here && focus == PANE_FILES) ? C_SEL_TEXT : C_DIM);
    }

    /* A directory with more in it than this holds says so where it would
     * otherwise silently stop. */
    if (truncated && top + rows >= count) {
        char more[64];

        wsnprintf(more, sizeof(more), "... and %d more, not read", truncated);
        draw_text_fit(x + PAD, y0 + (count - top) * ROW_H + 2, more, C_WARN,
                      w - PAD * 2);
    }
}

static void draw_status(void)
{
    int y = app.height - STATUS_H;
    char summary[160];

    fill(0, y, app.width, STATUS_H, C_HEADER);
    fill(0, y, app.width, 1, C_BORDER);

    if (status[0]) {
        strlcpy(summary, status, sizeof(summary));
    } else if (count > 0 && selected < count && !entries[selected].is_parent) {
        struct entry *e = &entries[selected];

        if (e->is_dir)
            wsnprintf(summary, sizeof(summary), "\"%s\" (folder)", e->name);
        else
            wsnprintf(summary, sizeof(summary), "\"%s\" (%s)", e->name,
                      whuman(e->size));
    } else {
        wsnprintf(summary, sizeof(summary), "%d item%s", count,
                  count == 1 ? "" : "s");
    }

    /* The free space first, because what is left after it is what the message
     * gets: reserving a fixed amount would cut "opening notes.txt" short in a
     * narrow window to hold room nothing was going to use. */
    char space[48];
    free_space(space, sizeof(space));

    int taken = space[0] ? text_width(space) + PAD * 2 : PAD;

    draw_text_fit(PAD, y + (STATUS_H - CELL_H) / 2, summary,
                  status_bad ? C_WARN : C_TEXT, app.width - PAD - taken);

    if (space[0])
        draw_text(app.width - PAD - text_width(space),
                  y + (STATUS_H - CELL_H) / 2, space, C_DIM);
}

/* The dialog, when one is up: a box in the middle of the window with the
 * question in it.  Drawn over everything else and last, so that what it covers
 * is still there underneath when it goes away. */
static void draw_dialog(void)
{
    int w = 380;

    /* As tall as what is in it.  A question with no field to fill in does not
     * need the room one would have taken, and leaving it there reads as a
     * control that failed to draw. */
    int h = (mode == MODE_INPUT) ? 108 : 74;

    if (w > app.width - 40)  w = app.width - 40;
    if (h > app.height - 40) h = app.height - 40;
    if (w < 80 || h < 60)
        return;

    int x = (app.width - w) / 2;
    int y = (app.height - h) / 2;

    fill(x + 3, y + 3, w, h, 0xD8D5D2);          /* a shadow, one offset deep */
    fill(x, y, w, h, C_WINDOW);
    draw_border(x, y, w, h, C_BORDER);
    fill(x, y, w, 28, C_HEADER);
    fill(x, y + 27, w, 1, C_BORDER);

    draw_text_fit(x + PAD, y + 6, dialog_prompt, C_TEXT, w - PAD * 2);

    if (mode == MODE_INPUT) {
        int fx = x + PAD;
        int fy = y + 44;
        int fw = w - PAD * 2;

        fill(fx, fy, fw, 24, C_ENTRY);
        draw_border(fx, fy, fw, 24, C_BORDER);
        draw_text_fit(fx + 5, fy + 4, dialog_text, C_TEXT, fw - 12);

        /* A caret, so an empty field looks like one that is waiting rather
         * than one that is disabled. */
        int caret = fx + 5 + text_width(dialog_text);
        if (caret < fx + fw - 3)
            fill(caret, fy + 4, 1, CELL_H, C_TEXT);

        draw_text(x + PAD, y + h - 24, "Enter  create      Esc  cancel", C_DIM);
    } else {
        draw_text(x + PAD, y + h - 24, "y  yes      n  no", C_DIM);
    }
}

static void draw(struct frame *f)
{
    px = f->pixels;

    /* The pane that is gone cannot be the focused one. */
    if (!sidebar_w())
        focus = PANE_FILES;

    /* Too small to lay out at all.  Saying so beats drawing a toolbar over a
     * status bar and leaving the window looking broken. */
    if (app.width < MIN_WIDTH || app.height < MIN_HEIGHT) {
        fill(0, 0, app.width, app.height, C_WINDOW);
        draw_text_fit(PAD, app.height / 2 - CELL_H / 2,
                      "the window is too small", C_DIM, app.width - PAD * 2);
        return;
    }

    draw_toolbar();
    draw_sidebar();
    draw_list();
    draw_status();

    if (mode != MODE_BROWSE)
        draw_dialog();
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
    /* Only when it never reached the compositor: pool_create() forgets the
     * number as soon as it hands it over, so a descriptor still here is one
     * that was opened and then not used. */
    if (app.shm_fd >= 0) {
        wclose(app.shm_fd);
        app.shm_fd = -1;
    }
}

/* One pool holding two buffers: the memory is shared once and carved up on
 * this side, which is the usual arrangement. */
static int pool_create(void)
{
    int stride = app.width * 4;
    int one    = stride * app.height;

    pool_release();

    app.pool_bytes = one * 2;
    app.shm_fd     = wshmopen((unsigned int)app.pool_bytes);
    if (app.shm_fd < 0) {
        wfprintf(W_STDERR, "thunar: no memory for a %dx%d window: %s\n",
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

    /* And the descriptor is no longer ours.  Queueing a message takes
     * ownership of every descriptor in it -- the connection closes its copy
     * once the message has gone, and closes it too if the message cannot be
     * queued at all -- so the number is forgotten here rather than in
     * pool_release().  Closing it a second time on the next resize would
     * close whatever had been handed the number in the meantime, which on
     * this connection means the socket to the compositor. */
    app.shm_fd = -1;

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
 *  Doing things
 * ------------------------------------------------------------------ */

/* Start a program and do not wait for it.  A file manager that blocked until
 * the editor it opened was finished would be a launcher, not a manager; the
 * children are collected in the event loop instead. */
static int launch(const char *program, char *const argv[])
{
    int pid = wspawn(program, argv);

    if (pid < 0) {
        complain("%s: %s", basename_of(program), wstrerror(-pid));
        return -1;
    }

    return pid;
}

static void open_in_editor(const char *file)
{
    char *argv[5];

    /* A terminal running vim, which is how a file gets edited on a machine
     * whose editor is a terminal program.  wlterm connects to the compositor
     * itself, so what opens is a sibling window and not a child of this one:
     * closing thunar afterwards leaves the editor where it is. */
    argv[0] = (char *)"wlterm";
    argv[1] = (char *)"-e";
    argv[2] = (char *)"vim";
    argv[3] = (char *)file;
    argv[4] = NULL;

    if (launch("/app/wlterm/launch", argv) >= 0)
        say("opening %s in vim", basename_of(file));
}

static void open_terminal_here(void)
{
    char *argv[2];

    argv[0] = (char *)"wlterm";
    argv[1] = NULL;

    /* No directory is passed: load() has already moved this process into the
     * directory being shown, and a child starts where its parent is. */
    if (launch("/app/wlterm/launch", argv) >= 0)
        say("terminal opened in %s", basename_of(path));
}

static void run_program(const char *file)
{
    char *argv[2];

    argv[0] = (char *)file;
    argv[1] = NULL;

    if (launch(file, argv) >= 0)
        say("started %s", basename_of(file));
}

static void open_selected(void)
{
    if (count == 0)
        return;

    struct entry *e = &entries[selected];

    if (e->is_parent) {
        parent_of(path);
        selected = 0;
        load();
        return;
    }

    if (e->is_dir) {
        char next[W_PATH_MAX + 1];

        join(next, sizeof(next), path, e->name);
        go_to(next);
        return;
    }

    char full[W_PATH_MAX + 1];
    join(full, sizeof(full), path, e->name);

    /* Every WOS program is installed as /app/<name>/launch, so a file with
     * that name is an executable and anything else is a document.  There are
     * no permission bits to ask, and guessing from the contents would mean
     * reading every file in the directory to draw the list. */
    if (strcmp(e->name, "launch") == 0)
        run_program(full);
    else
        open_in_editor(full);
}

static void do_make_directory(void)
{
    char full[W_PATH_MAX + 1];

    join(full, sizeof(full), path, dialog_text);

    int r = wmkdir(full);
    if (r < 0)
        complain("%s: %s", dialog_text, wstrerror(-r));
    else
        say("created %s", dialog_text);

    load();

    /* Land on what was just made, which is what the person who made it is
     * about to want. */
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].name, dialog_text) == 0)
            selected = i;
}

static void do_delete(void)
{
    struct entry *e = &entries[selected];
    char          full[W_PATH_MAX + 1];

    join(full, sizeof(full), path, e->name);

    int r = e->is_dir ? wrmdir(full) : wunlink(full);

    if (r < 0)
        complain("%s: %s", e->name, wstrerror(-r));
    else
        say("deleted %s", e->name);

    load();
}

static void ask(const char *prompt, void (*done)(void))
{
    strlcpy(dialog_prompt, prompt, sizeof(dialog_prompt));
    dialog_text[0] = '\0';
    dialog_len     = 0;
    dialog_done    = done;
    mode           = MODE_INPUT;
}

static void confirm(const char *question, void (*done)(void))
{
    strlcpy(dialog_prompt, question, sizeof(dialog_prompt));
    dialog_done = done;
    mode        = MODE_CONFIRM;
}

static void delete_selected(void)
{
    if (count == 0 || entries[selected].is_parent)
        return;

    struct entry *e = &entries[selected];
    char          question[96];

    /* The filesystem removes an empty directory and refuses a full one, so
     * the question says which of those this is going to be. */
    wsnprintf(question, sizeof(question), "Delete \"%s\"%s?", e->name,
              e->is_dir ? " (must be empty)" : "");

    confirm(question, do_delete);
}

/* ------------------------------------------------------------------ *
 *  Keys
 * ------------------------------------------------------------------ */

/* The keys that are not characters, in the evdev codes the compositor sends.
 * The arrows and the editing keys have W_KEY_* names already; Tab, Delete and
 * the function keys this program uses do not, so they are given codes above
 * everything W_KEY_* uses. */
#define KEY_TAB   0x200
#define KEY_F5    0x201

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
    case 15:  return KEY_TAB;
    case 63:  return KEY_F5;
    default:  return 0;
    }
}

static void key_dialog(int key, uint32_t ch)
{
    if (key == W_KEY_ESCAPE) {
        mode = MODE_BROWSE;
        say("cancelled");
        return;
    }

    if (mode == MODE_CONFIRM) {
        if (ch == 'y' || ch == 'Y') {
            mode = MODE_BROWSE;
            if (dialog_done)
                dialog_done();
        } else if (ch == 'n' || ch == 'N') {
            mode = MODE_BROWSE;
            say("left alone");
        }
        return;
    }

    if (ch == '\n' || ch == '\r') {
        mode = MODE_BROWSE;
        if (dialog_text[0] && dialog_done)
            dialog_done();
        return;
    }

    if (ch == '\b' || ch == 127) {
        if (dialog_len > 0)
            dialog_text[--dialog_len] = '\0';
        return;
    }

    if (ch >= ' ' && ch < 0x7F &&
        dialog_len + 1 < (int)sizeof(dialog_text)) {
        dialog_text[dialog_len++] = (char)ch;
        dialog_text[dialog_len]   = '\0';
    }
}

static void key_places(int key, uint32_t ch)
{
    switch (key) {
    case W_KEY_UP:   if (place_at > 0) place_at--; return;
    case W_KEY_DOWN: if (place_at + 1 < place_count) place_at++; return;
    case W_KEY_HOME: place_at = 0; return;
    case W_KEY_END:  place_at = place_count - 1; return;

    case W_KEY_RIGHT:
        focus = PANE_FILES;
        return;
    }

    if (ch == '\n' || ch == '\r') {
        if (place_count > 0) {
            go_to(places[place_at].path);
            focus = PANE_FILES;
        }
    }
}

static void key_files(int key, uint32_t ch)
{
    int page = rows_visible();

    switch (key) {
    case W_KEY_UP:    if (selected > 0) selected--; break;
    case W_KEY_DOWN:  if (selected + 1 < count) selected++; break;
    case W_KEY_PGUP:  selected -= page; break;
    case W_KEY_PGDN:  selected += page; break;
    case W_KEY_HOME:  selected = 0; break;
    case W_KEY_END:   selected = count - 1; break;

    case W_KEY_RIGHT:
        open_selected();
        break;

    case W_KEY_LEFT:
        /* Left goes to the shortcuts when they are there, and up a directory
         * when they are not -- which is what Left does in a file list with
         * nothing to its left. */
        if (sidebar_w())
            focus = PANE_PLACES;
        else
            { parent_of(path); selected = 0; load(); }
        break;

    case W_KEY_DELETE:
        delete_selected();
        break;

    default:
        if (ch == '\n' || ch == '\r')
            open_selected();
        else if (ch == '\b' || ch == 127)
            { parent_of(path); selected = 0; load(); }
        break;
    }

    if (selected < 0)      selected = 0;
    if (selected >= count) selected = count ? count - 1 : 0;
}

static void key_pressed(uint32_t code)
{
    int      key = special_key(code);
    uint32_t ch  = key ? 0 : wkeychar(code, app.mods);

    app.redraw = 1;

    if (mode != MODE_BROWSE) {
        key_dialog(key, ch);
        return;
    }

    say_nothing();

    /* The commands that mean the same thing whichever pane has the focus. */
    switch (key) {
    case KEY_TAB:
        if (sidebar_w())
            focus = (focus == PANE_PLACES) ? PANE_FILES : PANE_PLACES;
        return;

    case KEY_F5:
        load();
        say("reloaded");
        return;

    case W_KEY_ESCAPE:
        app.running = 0;
        return;
    }

    switch (ch) {
    case 'q':
        app.running = 0;
        return;

    case 'n':
        ask("Name for the new folder", do_make_directory);
        return;

    case 't':
        open_terminal_here();
        return;

    case 'h':
        if (place_count > 0)
            go_to(places[0].path);
        return;

    case 'u':
        parent_of(path);
        selected = 0;
        load();
        return;
    }

    if (focus == PANE_PLACES)
        key_places(key, ch);
    else
        key_files(key, ch);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    key_pressed(key);
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

/* ------------------------------------------------------------------ *
 *  The pointer
 *
 *  The window was keyboard-only because the machine had no mouse.  It has one
 *  now, and the shape Thunar has is a shape built for clicking: the sidebar is
 *  a list of places to click, and a file is opened by double-clicking it.  The
 *  keys all still work, because a file manager that needs a mouse is worse
 *  than one that does not.
 * ------------------------------------------------------------------ */

/* Which row of the sidebar a y coordinate is on, or -1.  Laid out the same way
 * draw_sidebar() lays it out -- the heading, then a row each. */
static int place_row_at(int y)
{
    int top_y = TOOLBAR_H + 6 + CELL_H + 6;

    if (y < top_y || y >= app.height - STATUS_H)
        return -1;

    int row = (y - top_y) / ROW_H;
    return row < place_count ? row : -1;
}

/* Which entry a y coordinate is on, or -1.  `top` is the first row shown, so
 * this is the scroll position plus however far down the list the click was. */
static int file_row_at(int y)
{
    if (y < list_y() || y >= list_y() + list_height())
        return -1;

    int row = top + (y - list_y()) / ROW_H;
    return (row >= 0 && row < count) ? row : -1;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    app.ptr_x  = wl_fixed_to_int(sx);
    app.ptr_y  = wl_fixed_to_int(sy);
    app.ptr_in = 1;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    app.ptr_in = 0;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    app.ptr_x  = wl_fixed_to_int(sx);
    app.ptr_y  = wl_fixed_to_int(sy);
    app.ptr_in = 1;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    if (state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;

    /* A dialog is asking for a name or a yes.  Clicking behind it would act on
     * something the person cannot see the state of, so it does not. */
    if (mode != MODE_BROWSE)
        return;

    if (button != W_BTN_LEFT && button != W_BTN_MIDDLE)
        return;

    int x = app.ptr_x;
    int y = app.ptr_y;

    say_nothing();

    /* The sidebar: one click goes there.  A place is a shortcut and a shortcut
     * that needed two clicks would not be one -- which is also how Thunar
     * itself treats them, and the one asymmetry with the file list worth
     * having. */
    if (sidebar_w() && x < sidebar_w()) {
        int row = place_row_at(y);

        focus = PANE_PLACES;

        if (row >= 0) {
            place_at = row;
            go_to(places[row].path);
            focus = PANE_FILES;
        }

        app.redraw = 1;
        return;
    }

    /* The location bar and the status line have nothing to click. */
    if (y < list_y() || y >= list_y() + list_height())
        return;

    focus = PANE_FILES;

    int row = file_row_at(y);
    if (row < 0) {
        app.redraw = 1;
        return;
    }

    /* Two clicks on the same row, close enough together, is one double click.
     * A quarter of a second is the interval every toolkit settles on, and the
     * row has to match because two clicks on different files are two clicks
     * however fast the hand was. */
    int same    = (row == app.last_click_row);
    int quickly = (time - app.last_click_ms) < 400;

    selected = row;

    if (same && quickly) {
        app.last_click_ms  = 0;
        app.last_click_row = -1;
        open_selected();
    } else {
        app.last_click_ms  = time;
        app.last_click_row = row;
    }

    app.redraw = 1;
}

/* The wheel scrolls the file list, and moves the selection with it rather than
 * leaving it behind off the top of the window -- there is one selection here
 * and it is also the cursor, so a selection that has scrolled out of sight is
 * one whose next arrow key jumps somewhere unexpected. */
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || mode != MODE_BROWSE)
        return;

    int steps = wl_fixed_to_int(value) / 10;

    if (steps == 0)
        steps = (value > 0) ? 1 : -1;

    selected += steps * 3;

    if (selected < 0)
        selected = 0;
    if (selected >= count)
        selected = count ? count - 1 : 0;

    app.redraw = 1;
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
};

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

    /* Asked for only when the compositor says there is one.  On a machine
     * with no mouse the capability never arrives, and a program that took a
     * pointer anyway and waited for motion would look broken rather than
     * keyboard-driven. */
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
        app.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
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

int main(int argc, char **argv)
{
    app.width          = 720;
    app.height         = 460;
    app.shm_fd         = -1;
    app.running        = 1;

    /* No row, so the first click can never be the second half of a double. */
    app.last_click_row = -1;

    if (argc > 1) {
        strlcpy(path, argv[1], sizeof(path));

        /* A trailing slash is how people type a directory and not how the
         * filesystem stores one. */
        wsize_t len = strlen(path);
        while (len > 1 && path[len - 1] == '/')
            path[--len] = '\0';
    } else if (wgetcwd(path, sizeof(path)) < 0) {
        strlcpy(path, "/", sizeof(path));
    }

    wstat_t st;
    if (wstat(path, &st) < 0 || st.type != W_FT_DIR) {
        wfprintf(W_STDERR, "thunar: %s is not a directory\n", path);
        return 1;
    }

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        int why = wl_display_connect_error();

        wfprintf(W_STDERR, "thunar: no display server to connect to: %s\n",
                 wstrerror(-why));
        wfprintf(W_STDERR, -why == W_EPERM
                 ? "thunar: that session belongs to another user\n"
                 : "thunar: this is a Wayland client -- start sway first, "
                   "or use `fm` on the console\n");
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, NULL);

    /* Two roundtrips: the first brings the list of globals, the second the
     * events the things bound in the first one sent back. */
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.wm_base) {
        wfprintf(W_STDERR, "thunar: that display server has no %s\n",
                 !app.compositor ? "wl_compositor"
                 : !app.shm      ? "wl_shm" : "xdg_wm_base");
        wfprintf(W_STDERR, "thunar: it cannot show a window, so there is "
                           "nothing to do here.\n");
        return 1;
    }

    app.surface     = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, NULL);

    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);

    xdg_toplevel_set_title(app.toplevel, "thunar");
    xdg_toplevel_set_app_id(app.toplevel, "thunar");

    /* A commit with no buffer: it says the window is ready to be told its
     * size, and nothing can be drawn until it has been. */
    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    build_places();
    load();

    while (app.running) {
        wl_display_flush(app.display);

        wpollfd_t watch[1];

        watch[0].fd      = wl_display_get_fd(app.display);
        watch[0].events  = W_POLLIN;
        watch[0].revents = 0;

        if (wpoll(watch, 1, 200) > 0 && (watch[0].revents & W_POLLIN)) {
            if (wl_display_dispatch(app.display) < 0)
                break;
        }

        /* Collect whatever the editor and the terminals this window started
         * have left behind.  Nothing here waits for them, so without this they
         * would sit in the process table until thunar itself exited. */
        while (wreap(NULL) >= 0)
            ;

        if (app.redraw)
            present();
    }

    pool_release();
    wl_display_disconnect(app.display);
    return 0;
}
