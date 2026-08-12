/* wauncher -- the programs on this machine, by typing part of the name.
 *
 *     wauncher
 *
 * Bound to Super+Q under sway.  Type, and the list narrows; Enter runs what is
 * selected and the window goes away.  That is the whole of it, and it is the
 * whole of rofi's `-show run` as well: a launcher's one job is to stop being on
 * screen.
 *
 * There is no list of applications to read here -- no .desktop files, no menu
 * spec, nothing that says what a program is called or which icon it has.  What
 * there is instead is a rule the whole system already keeps: every program is
 * installed as `/app/<name>/launch`, and a bare name means exactly that path to
 * whell and to sway alike.  So the launcher lists the directory.  A program
 * that is installed is listed, the moment it is installed, and there is no
 * second file to keep in step with the first.
 *
 * **Which of them want a terminal.**  Half of these are `ls` and `df` and
 * `ping`: programs that print.  Starting one the way sway starts a window would
 * run it with nowhere for its output to go, and pressing Enter would look like
 * pressing nothing.  Nothing on disk says which is which -- so this asks the
 * binary.  A Wayland client is a program that called wl_display_connect(), and
 * that call carries the name of the socket it connects to into the executable
 * with it.  A program with "wayland-0" in it opens a window; one without it
 * prints, and is given a terminal to print into.  The answer is a fact about
 * the file rather than a guess about the name, and it costs one read of a
 * binary that is already going to be read in a moment if it is chosen.
 *
 * **It asks sway to start things, rather than starting them.**  `exec <name>`
 * over the IPC socket is what a keybinding does, which means the program that
 * opens is sway's child and not this window's -- so it survives this window
 * closing a tenth of a second later, and it is started the same way, as the
 * same user, with the same rules about what a bare name means.  A launcher that
 * spawned its own children would have to outlive them to be their parent, and
 * then it would not be a launcher.
 *
 * It is an ordinary Wayland client: no privilege sway does not give everything
 * else, and nothing here that could not be typed at swaymsg.  It draws its own
 * pixels with the kernel's 8x16 font, in thunar's and swaysettings' colours,
 * because two windows on one machine should look like one machine.
 *
 * There are no floating windows in this compositor, so it opens as a tile like
 * anything else.  A launcher that takes half the screen for the second and a
 * half it is on screen is a smaller problem than a compositor that grew a
 * second kind of window to hold it.
 */

#include <wkernel.h>
#include <wayland-client.h>
#include <wdraw.h>
#include <wipc.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ *
 *  Metrics
 * ------------------------------------------------------------------ */

#define CELL_W      8               /* the kernel font, which is all there is */
#define CELL_H      16

#define HEADER_H    28
#define QUERY_H     32
#define STATUS_H    22
#define ROW_H       20
#define PAD         8

#define MIN_WIDTH   260
#define MIN_HEIGHT  180

/* The palette thunar and swaysettings use.  Three windows drawn by three
 * programs that share no line of layout still have to look like one system,
 * and on a machine with no theme engine that means agreeing by hand. */
#define C_WINDOW    0xF6F5F4
#define C_HEADER    0xDEDAD6
#define C_BORDER    0xC4C0BC
#define C_TEXT      0x2E3436
#define C_DIM       0x8A8E8F
#define C_SELECT    0x3584E4
#define C_SEL_TEXT  0xFFFFFF
#define C_SEL_DIM   0xCFE3FB        /* the tag on a selected row */
#define C_ENTRY     0xFFFFFF
#define C_ACCENT    0x3584E4
#define C_WARN      0xC01C28

/* ------------------------------------------------------------------ *
 *  The programs
 * ------------------------------------------------------------------ */

#define MAX_APPS  128
#define QUERY_MAX 64

/* What starting one of these means.  KIND_UNKNOWN is "not looked at yet":
 * finding out costs a read of the executable, so it is done for the rows on
 * screen rather than for fifty programs at startup, and remembered. */
enum kind {
    KIND_UNKNOWN,
    KIND_WINDOW,                    /* a Wayland client: it opens its own */
    KIND_TERMINAL,                  /* it prints, so it is given a terminal */
};

struct app_entry {
    char      name[W_NAME_MAX + 1];
    enum kind kind;
};

static struct app_entry apps[MAX_APPS];
static int              app_count;

/* What the typing has left: indices into apps[], best match first. */
static int shown[MAX_APPS];
static int shown_count;

static char query[QUERY_MAX];
static int  query_len;
static int  caret;

static int selected;                /* into shown[] */
static int top;                     /* the first row drawn */

/* What went wrong, in place of the keys along the bottom.  Everything this
 * window does right ends with it closing, so the status line only ever has bad
 * news on it. */
static char status[128];

static void complain(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void complain(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    wvsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Where `needle` starts in `haystack`, or -1.  Both are folded to lower case:
 * somebody typing "HT" is looking for htop. */
static int find_fold(const char *haystack, const char *needle)
{
    if (!*needle)
        return 0;

    for (int i = 0; haystack[i]; i++) {
        int j = 0;

        while (needle[j] && lower(haystack[i + j]) == lower(needle[j]))
            j++;

        if (!needle[j])
            return i;
    }

    return -1;
}

/* Read the directory every program on this machine lives in.
 *
 * An entry counts when it has a `launch` inside it, which is the same rule
 * whell uses to decide that a bare word is a command -- a directory under /app
 * with no executable in it is a source tree somebody left behind, not a
 * program. */
static void load_apps(void)
{
    int d = wopendir("/app");

    if (d < 0) {
        complain("/app: %s", wstrerror(-d));
        return;
    }

    wdirent_t e;

    while (app_count < MAX_APPS && wreaddir(d, &e) == 1) {
        if (e.name[0] == '.' || e.type != W_FT_DIR)
            continue;

        char    path[W_PATH_MAX + 1];
        wstat_t st;

        wsnprintf(path, sizeof(path), "/app/%s/launch", e.name);
        if (wstat(path, &st) != 0 || st.type != W_FT_FILE)
            continue;

        strlcpy(apps[app_count].name, e.name, sizeof(apps[app_count].name));
        apps[app_count].kind = KIND_UNKNOWN;
        app_count++;
    }

    wclosedir(d);

    /* Insertion sort: fifty programs, and it keeps the ones that are already
     * in order in order. */
    for (int i = 1; i < app_count; i++) {
        struct app_entry hold = apps[i];
        int              j    = i - 1;

        while (j >= 0 && strcmp(apps[j].name, hold.name) > 0) {
            apps[j + 1] = apps[j];
            j--;
        }

        apps[j + 1] = hold;
    }
}

/* Does this program open a window?
 *
 * The question is really "did it call wl_display_connect()", and the answer is
 * in the executable: that function builds its path from WL_DEFAULT_DISPLAY, so
 * the string is linked in exactly when the program that might need it is.  A
 * program that prints does not pull the client library in and does not carry
 * the name of a socket it never opens.
 *
 * Read in blocks, with the tail of each one carried over: a name that fell
 * across a block boundary would otherwise be a program quietly given the wrong
 * kind of start. */
static enum kind kind_of(struct app_entry *a)
{
    if (a->kind != KIND_UNKNOWN)
        return a->kind;

    const char *needle = WL_DEFAULT_DISPLAY;
    int         len    = (int)strlen(needle);

    char path[W_PATH_MAX + 1];
    wsnprintf(path, sizeof(path), "/app/%s/launch", a->name);

    int fd = wopen(path, W_O_RDONLY);
    if (fd < 0) {
        /* Unreadable is not "prints", but a terminal is the start that shows
         * the error rather than swallowing it. */
        a->kind = KIND_TERMINAL;
        return a->kind;
    }

    static char block[8192];
    int         carried = 0;

    a->kind = KIND_TERMINAL;

    for (;;) {
        int got = wread(fd, block + carried, sizeof(block) - (wsize_t)carried);

        if (got <= 0)
            break;

        int have = carried + got;

        for (int i = 0; i + len <= have; i++) {
            int j = 0;

            while (j < len && block[i + j] == needle[j])
                j++;

            if (j == len) {
                a->kind = KIND_WINDOW;
                wclose(fd);
                return a->kind;
            }
        }

        carried = len - 1 < have ? len - 1 : have;
        for (int i = 0; i < carried; i++)
            block[i] = block[have - carried + i];
    }

    wclose(fd);
    return a->kind;
}

/* ------------------------------------------------------------------ *
 *  Narrowing the list
 * ------------------------------------------------------------------ */

/* Rebuild shown[] for what has been typed.
 *
 * Programs whose name *starts* with the query come first, in name order, then
 * the ones that merely contain it.  Typing "fi" should offer fish before
 * fastfetch, which contains an "f" and an "i" in the wrong places; and an exact
 * name is always the first row, so a name typed in full and Enter is a
 * launcher that never surprises anybody. */
static void refilter(void)
{
    shown_count = 0;

    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < app_count; i++) {
            int at = find_fold(apps[i].name, query);

            if (at < 0)
                continue;
            if ((pass == 0) != (at == 0))
                continue;

            shown[shown_count++] = i;
        }

    /* Back to the top on every keystroke, deliberately.  Carrying the old
     * selection over to whichever row it landed on in the new list is how a
     * launcher ends up running the program under the highlight rather than the
     * one that was typed: the best match is the first row, and it is the row
     * Enter should be pointing at the moment the typing stops. */
    selected = 0;
    top      = 0;
}

/* ------------------------------------------------------------------ *
 *  Starting one
 * ------------------------------------------------------------------ */

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
    struct {
        struct wl_buffer *buffer;
        uint32_t         *pixels;
        int               busy;
    } frames[2];
    int pool_bytes;

    int width, height;
    int configured;
    int running;
    int redraw;

    uint32_t mods;
    int      ptr_x, ptr_y;
    uint32_t last_click_ms;
    int      last_click_row;
} app;

/* Ask sway to start a command line, exactly as a keybinding would.
 *
 * `in_terminal` wraps it in `wlterm -e`, which is the same thing thunar does
 * to open an editor: the terminal is a sibling window that connects to the
 * compositor itself, so it does not go when this does. */
static void start(const char *command, int in_terminal)
{
    char line[256];

    wsnprintf(line, sizeof(line), in_terminal ? "exec wlterm -e %s" : "exec %s",
              command);

    int r = wipc_command(WIPC_SWAY_SOCKET, line);

    if (r < 0) {
        complain("sway would not start %s: %s", command, wstrerror(-r));
        return;
    }

    /* Gone, having done the one thing it is for.  sway reports a program it
     * could not start on the bar, which is where somebody who pressed the key
     * is already looking -- and is why this does not stay open to say so
     * itself. */
    app.running = 0;
}

/* Enter.  `other_way` is Shift+Enter: run a window program in a terminal, or a
 * printing one without.  The detection is a fact about the binary and is right
 * nearly always, and "nearly" is why the other way is one key away. */
static void run_selected(int other_way)
{
    if (shown_count > 0) {
        struct app_entry *a  = &apps[shown[selected]];
        int               in = kind_of(a) == KIND_TERMINAL;

        start(a->name, other_way ? !in : in);
        return;
    }

    /* Nothing matched, and something was typed: run it as it was typed.  That
     * is how a launcher takes `ping 10.0.2.2` -- a command with an argument
     * matches no program name and is not meant to.  A first word that names a
     * program still decides where it runs; anything else gets a terminal,
     * because a command nobody here has heard of is far more likely to print
     * than to draw. */
    if (query_len == 0)
        return;

    char first[W_NAME_MAX + 1];
    int  n = 0;

    while (query[n] && query[n] != ' ' && n < W_NAME_MAX) {
        first[n] = query[n];
        n++;
    }
    first[n] = '\0';

    int in = 1;

    for (int i = 0; i < app_count; i++)
        if (strcmp(apps[i].name, first) == 0)
            in = kind_of(&apps[i]) == KIND_TERMINAL;

    start(query, other_way ? !in : in);
}

/* ------------------------------------------------------------------ *
 *  Drawing
 * ------------------------------------------------------------------ */

static wcanvas_t canvas;

static void fill(int x, int y, int w, int h, uint32_t colour)
{
    wdraw_fill(&canvas, x, y, w, h, colour);
}

static void draw_text(int x, int y, const char *s, uint32_t fg)
{
    wdraw_text(&canvas, x, y, s, fg);
}

static void draw_text_fit(int x, int y, const char *s, uint32_t fg, int room)
{
    wdraw_text_fit(&canvas, x, y, s, fg, room);
}

static int list_y(void)
{
    return HEADER_H + QUERY_H;
}

static int list_height(void)
{
    int h = app.height - list_y() - STATUS_H;

    return h < 0 ? 0 : h;
}

static int rows_visible(void)
{
    return list_height() / ROW_H;
}

/* The row under a pixel, or -1 for none. */
static int row_at(int y)
{
    if (y < list_y() || y >= list_y() + rows_visible() * ROW_H)
        return -1;

    int row = top + (y - list_y()) / ROW_H;

    return row < shown_count ? row : -1;
}

static void scroll_to_selection(void)
{
    int rows = rows_visible();

    if (rows <= 0)
        return;

    if (selected < top)
        top = selected;
    if (selected >= top + rows)
        top = selected - rows + 1;
    if (top > shown_count - rows)
        top = shown_count - rows;
    if (top < 0)
        top = 0;
}

static void draw_header(void)
{
    char count[32];

    fill(0, 0, app.width, HEADER_H, C_HEADER);
    fill(0, HEADER_H - 1, app.width, 1, C_BORDER);

    draw_text(PAD, (HEADER_H - CELL_H) / 2, "wauncher", C_TEXT);

    wsnprintf(count, sizeof(count), "%d program%s", app_count,
              app_count == 1 ? "" : "s");

    int w = wdraw_text_width(count);
    if (PAD + wdraw_text_width("wauncher") + PAD * 2 + w <= app.width)
        draw_text(app.width - PAD - w, (HEADER_H - CELL_H) / 2, count, C_DIM);
}

/* The field being typed into.  It is always focused -- there is nowhere else
 * for a key to go in a window whose only job is to be typed at -- so the caret
 * is always drawn and there is no focus ring to draw around anything. */
static void draw_query(void)
{
    int y = HEADER_H + (QUERY_H - CELL_H - 8) / 2;
    int h = CELL_H + 8;
    int w = app.width - PAD * 2;

    fill(0, HEADER_H, app.width, QUERY_H, C_WINDOW);
    fill(PAD, y, w, h, C_ENTRY);
    wdraw_border(&canvas, PAD, y, w, h, C_BORDER);

    int text_x = PAD + 6;
    int text_y = y + 4;
    int room   = w - 12;

    if (query_len == 0) {
        draw_text_fit(text_x, text_y, "Type a program's name", C_DIM, room);
    } else {
        draw_text_fit(text_x, text_y, query, C_TEXT, room);
    }

    /* The caret sits between characters, so it is drawn at the width of what
     * comes before it rather than at a multiple of the cell -- which is the
     * same number here, and would stop being it the day the font is not
     * fixed-width. */
    int before = caret * CELL_W;

    if (text_x + before < PAD + w - 2)
        fill(text_x + before, text_y, 1, CELL_H, C_ACCENT);
}

static void draw_rows(void)
{
    int y0   = list_y();
    int rows = rows_visible();

    fill(0, y0, app.width, list_height(), C_WINDOW);

    if (shown_count == 0) {
        const char *what = app_count == 0
                         ? "No programs in /app"
                         : "Nothing matches -- Enter runs what you typed";

        draw_text_fit(PAD, y0 + PAD, what, C_DIM, app.width - PAD * 2);
        return;
    }

    for (int i = 0; i < rows; i++) {
        int index = top + i;

        if (index >= shown_count)
            break;

        struct app_entry *a   = &apps[shown[index]];
        int               y   = y0 + i * ROW_H;
        int               sel = (index == selected);

        if (sel)
            fill(0, y, app.width, ROW_H, C_SELECT);

        /* What Enter would do to this row, in the row.  A launcher that opened
         * a terminal for one name and a window for the next without ever
         * saying so would be a launcher nobody could predict. */
        const char *tag = kind_of(a) == KIND_WINDOW ? "window" : "terminal";
        int         tw  = wdraw_text_width(tag);
        int         ty  = y + (ROW_H - CELL_H) / 2;

        int room = app.width - PAD * 3 - tw;

        draw_text_fit(PAD, ty, a->name, sel ? C_SEL_TEXT : C_TEXT,
                      room < 0 ? 0 : room);

        if (app.width > PAD * 3 + tw + CELL_W * 4)
            draw_text(app.width - PAD - tw, ty, tag,
                      sel ? C_SEL_DIM : C_DIM);
    }
}

static void draw_status(void)
{
    int y = app.height - STATUS_H;

    fill(0, y, app.width, STATUS_H, C_HEADER);
    fill(0, y, app.width, 1, C_BORDER);

    int ty = y + (STATUS_H - CELL_H) / 2;

    if (status[0]) {
        draw_text_fit(PAD, ty, status, C_WARN, app.width - PAD * 2);
        return;
    }

    draw_text(PAD, ty, "Enter run   Esc close", C_DIM);

    char right[32];

    if (query_len && shown_count)
        wsnprintf(right, sizeof(right), "%d of %d", shown_count, app_count);
    else if (query_len)
        wsnprintf(right, sizeof(right), "no match");
    else
        right[0] = '\0';

    if (right[0]) {
        int w = wdraw_text_width(right);

        if (wdraw_text_width("Enter run   Esc close") + PAD * 3 + w <=
            app.width)
            draw_text(app.width - PAD - w, ty, right, C_DIM);
    }
}

static void draw(uint32_t *pixels)
{
    canvas = wcanvas(pixels, app.width, app.height);

    scroll_to_selection();

    fill(0, 0, app.width, app.height, C_WINDOW);
    draw_header();
    draw_query();
    draw_rows();
    draw_status();
}

/* ------------------------------------------------------------------ *
 *  Typing
 * ------------------------------------------------------------------ */

#define KEY_TAB    0x200
#define KEY_ENTER  0x202

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
    case 28:
    case 96:  return KEY_ENTER;
    case 1:   return W_KEY_ESCAPE;
    case 15:  return KEY_TAB;
    default:  return 0;
    }
}

static void query_insert(uint32_t ch)
{
    if (ch < 0x20 || ch > 0x7E || query_len + 1 >= QUERY_MAX)
        return;

    for (int i = query_len; i > caret; i--)
        query[i] = query[i - 1];

    query[caret++] = (char)ch;
    query[++query_len] = '\0';

    refilter();
    app.redraw = 1;
}

static void query_delete(int before)
{
    int at = before ? caret - 1 : caret;

    if (at < 0 || at >= query_len)
        return;

    for (int i = at; i < query_len; i++)
        query[i] = query[i + 1];

    query_len--;
    if (before)
        caret--;

    refilter();
    app.redraw = 1;
}

static void move_selection(int by)
{
    if (shown_count == 0)
        return;

    selected += by;

    if (selected < 0)
        selected = 0;
    if (selected >= shown_count)
        selected = shown_count - 1;

    app.redraw = 1;
}

static void key_pressed(uint32_t code)
{
    int      key = special_key(code);
    uint32_t ch  = key ? 0 : wkeychar(code, app.mods);

    status[0] = '\0';

    switch (key) {
    case W_KEY_ESCAPE:
        /* Escape clears the typing first and closes on the second press, which
         * is what a person who mistyped a name means by it. */
        if (query_len) {
            query[0] = '\0';
            query_len = caret = 0;
            refilter();
            app.redraw = 1;
        } else {
            app.running = 0;
        }
        return;

    case KEY_ENTER:
        run_selected((app.mods & W_MOD_SHIFT) != 0);
        return;

    case W_KEY_UP:
        move_selection(-1);
        return;

    case W_KEY_DOWN:
    case KEY_TAB:
        move_selection(1);
        return;

    case W_KEY_PGUP:
        move_selection(-(rows_visible() ? rows_visible() : 1));
        return;

    case W_KEY_PGDN:
        move_selection(rows_visible() ? rows_visible() : 1);
        return;

    /* Left, Right, Home and End belong to the text, not to the list: this is a
     * field being typed into, and the list has the arrows that point at it. */
    case W_KEY_LEFT:
        if (caret > 0)
            caret--;
        app.redraw = 1;
        return;

    case W_KEY_RIGHT:
        if (caret < query_len)
            caret++;
        app.redraw = 1;
        return;

    case W_KEY_HOME:
        caret = 0;
        app.redraw = 1;
        return;

    case W_KEY_END:
        caret = query_len;
        app.redraw = 1;
        return;

    case W_KEY_DELETE:
        query_delete(0);
        return;

    default:
        break;
    }

    if (ch == '\b' || ch == 127) {
        query_delete(1);
        return;
    }

    if (ch == '\n' || ch == '\r') {
        run_selected((app.mods & W_MOD_SHIFT) != 0);
        return;
    }

    if (ch)
        query_insert(ch);
}

/* ------------------------------------------------------------------ *
 *  The pointer
 *
 *  A launcher is a keyboard program and this one is written as one, but a
 *  window on a machine with a mouse that could not be clicked would be the odd
 *  one out.
 * ------------------------------------------------------------------ */

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          int32_t x, int32_t y)
{
    app.ptr_x = x;
    app.ptr_y = y;
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, int32_t x, int32_t y)
{
    app.ptr_x = x;
    app.ptr_y = y;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || button != W_BTN_LEFT)
        return;

    int row = row_at(app.ptr_y);

    if (row < 0)
        return;

    /* Two clicks on the same row, close enough together, is one double click --
     * thunar's quarter second, because two windows that disagreed about how
     * fast a double click is would be one machine that disagreed with
     * itself. */
    int same    = (row == app.last_click_row);
    int quickly = (time - app.last_click_ms) < 400;

    selected   = row;
    app.redraw = 1;

    if (same && quickly) {
        app.last_click_ms  = 0;
        app.last_click_row = -1;
        run_selected(0);
        return;
    }

    app.last_click_ms  = time;
    app.last_click_row = row;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    /* The selection moves with the list rather than being left behind off the
     * top of it: there is one selection here and it is also the cursor. */
    move_selection(value > 0 ? 3 : -3);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
};

/* ------------------------------------------------------------------ *
 *  The keyboard
 * ------------------------------------------------------------------ */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    if (fd >= 0)
        wclose(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        key_pressed(key);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    app.mods = depressed | latched | locked;
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
 *  Buffers
 * ------------------------------------------------------------------ */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    int *busy = data;

    *busy = 0;
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

static int pool_create(void)
{
    int stride = app.width * 4;
    int one    = stride * app.height;

    pool_release();

    app.pool_bytes = one * 2;
    app.shm_fd     = wshmopen((unsigned int)app.pool_bytes);

    if (app.shm_fd < 0) {
        wfprintf(W_STDERR, "wauncher: no memory for a %dx%d window: %s\n",
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

    /* The descriptor belongs to the connection now: queueing a message takes
     * every descriptor in it. */
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
                               &app.frames[i].busy);
    }

    return 0;
}

static void present(void)
{
    if (!app.configured || !app.pool)
        return;

    int at = -1;

    for (int i = 0; i < 2; i++)
        if (!app.frames[i].busy) {
            at = i;
            break;
        }

    /* Both still with the compositor: the next release brings one back, and
     * drawing into a buffer being read would show half of each. */
    if (at < 0)
        return;

    draw(app.frames[at].pixels);

    wl_surface_attach(app.surface, app.frames[at].buffer, 0, 0);
    wl_surface_damage(app.surface, 0, 0, app.width, app.height);
    wl_surface_commit(app.surface);

    app.frames[at].busy = 1;
    app.redraw          = 0;
}

/* ------------------------------------------------------------------ *
 *  Being told how big to be
 * ------------------------------------------------------------------ */

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    if (width > 0)
        app.width = width < MIN_WIDTH ? MIN_WIDTH : width;
    if (height > 0)
        app.height = height < MIN_HEIGHT ? MIN_HEIGHT : height;
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
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

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
    app.width          = 460;
    app.height         = 400;
    app.shm_fd         = -1;
    app.running        = 1;
    app.last_click_row = -1;

    if (argc > 1) {
        int asked = strcmp(argv[1], "-h") == 0 ||
                    strcmp(argv[1], "--help") == 0;

        wfprintf(asked ? W_STDOUT : W_STDERR,
                 "usage: wauncher\n\n"
                 "  The programs in /app, by typing part of the name.  Enter\n"
                 "  runs the selected one -- in a window if it is a Wayland\n"
                 "  client, in a terminal if it prints -- and Shift+Enter\n"
                 "  runs it the other way.  Escape closes it.\n\n"
                 "  Bound to Super+Q under sway.\n");

        return asked ? 0 : 1;
    }

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        int why = wl_display_connect_error();

        wfprintf(W_STDERR, "wauncher: no display server to connect to: %s\n",
                 wstrerror(-why));
        wfprintf(W_STDERR, -why == W_EPERM
                 ? "wauncher: that session belongs to another user\n"
                 : "wauncher: this is a Wayland client -- start sway first, "
                   "or type the name at the shell\n");
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, NULL);

    /* Two roundtrips: the first brings the globals, the second the events the
     * things bound in the first sent back. */
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.wm_base) {
        wfprintf(W_STDERR, "wauncher: that display server has no %s\n",
                 !app.compositor ? "wl_compositor"
                 : !app.shm      ? "wl_shm" : "xdg_wm_base");
        return 1;
    }

    app.surface     = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, NULL);

    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);

    xdg_toplevel_set_title(app.toplevel, "wauncher");
    xdg_toplevel_set_app_id(app.toplevel, "wauncher");

    /* A commit with no buffer: it says the window is ready to be told its size,
     * and nothing can be drawn until it has been. */
    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    load_apps();
    refilter();

    while (app.running) {
        wl_display_flush(app.display);

        wpollfd_t watch[1];

        watch[0].fd      = wl_display_get_fd(app.display);
        watch[0].events  = W_POLLIN;
        watch[0].revents = 0;

        if (wpoll(watch, 1, 100) > 0 && (watch[0].revents & W_POLLIN)) {
            if (wl_display_dispatch(app.display) < 0)
                break;
        }

        if (app.redraw)
            present();
    }

    pool_release();
    wl_display_disconnect(app.display);
    return 0;
}
