/* swaysettings -- the compositor's settings, in a window.
 *
 *     swaysettings
 *
 * sway is configured by a file of commands, and the same commands can be sent
 * to it while it runs.  That is a good design and a poor way to find out what
 * the settings are: `pointer_accel 0.4` says nothing about which way 0.4 is,
 * and the only way to see a background colour is to write one down and reload.
 * This window is the other half -- the settings that have a value worth seeing
 * as you change it, with the value shown as what it does rather than as what it
 * is called.
 *
 * There are three of them, because those are the three this compositor has that
 * are worth a slider:
 *
 *     background colour     output * bg #rrggbb solid_color
 *     mouse speed           input * pointer_accel <-1..1>
 *     bar position          bar position top|bottom
 *
 * It works the way a settings window should: **every change happens as you make
 * it**, sent over sway's IPC socket exactly as `swaymsg` would send it, so the
 * screen behind the window changes colour while the slider is being dragged and
 * the mouse gets faster under the hand that is speeding it up.  Nothing is
 * written to disk until Save, and Reload is sway's own `reload` -- it rereads
 * the file, so it is also how to throw away a change that was tried and not
 * liked.
 *
 * It is an ordinary Wayland client.  It has no privilege sway does not give
 * every client, and it changes nothing itself: it asks the compositor, in the
 * compositor's language, over the socket anything else could use.  The one
 * thing it reads without asking is the pointer speed, from the kernel that
 * owns it, and only to know where to start the slider when the file says
 * nothing about it.
 *
 * The drawing is its own, a pixel at a time into shared memory with the
 * kernel's 8x16 font, because there is no toolkit here to ask.  It is built to
 * be clicked -- a settings window that had to be typed at would be a worse
 * configuration file -- but every control is reachable with Tab and the arrow
 * keys as well, because a machine whose mouse is set too slow to use is exactly
 * the machine somebody opens this on.
 */

#include <wkernel.h>
#include <wayland-client.h>
#include <wdraw.h>
#include <wipc.h>
#include <wstatus.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ *
 *  Metrics
 * ------------------------------------------------------------------ */

#define CELL_W      8               /* the kernel font, which is all there is */
#define CELL_H      16

#define HEADER_H    30
#define STATUS_H    22
#define PAD         10
#define GAP         8

#define SLIDER_H    18
#define KNOB_W      10
#define GROOVE_H    6

#define BUTTON_H    26
#define BUTTON_W    78
#define SEG_W       88

#define PREVIEW     52              /* the square of colour being mixed */
#define SWATCH_W    26
#define SWATCH_H    18
#define SIZE_W      52              /* one of the four cursor sizes */
#define ENTRY_H     24              /* the text field */
#define SCROLLBAR_W 6

#define MIN_WIDTH   260
#define MIN_HEIGHT  200

/* The palette, the same light grey-blue thunar uses.  Two windows drawn by two
 * programs that never share a line of code still have to look like one system,
 * and on a machine with no theme engine that means agreeing by hand. */
#define C_WINDOW    0xF6F5F4
#define C_HEADER    0xDEDAD6
#define C_BORDER    0xC4C0BC
#define C_SUNKEN    0xCFCBC7
#define C_TEXT      0x2E3436
#define C_DIM       0x8A8E8F
#define C_ACCENT    0x3584E4        /* the filled part of a slider, and Save */
#define C_ACCENT_LO 0x1C71D8
#define C_ON_ACCENT 0xFFFFFF
#define C_BUTTON    0xFFFFFF
#define C_WARN      0xC01C28

/* ------------------------------------------------------------------ *
 *  The settings
 * ------------------------------------------------------------------ */

/* sway's own defaults, so a machine with no configuration file starts the
 * sliders where the compositor actually is rather than at zero. */
#define DEFAULT_BG          0x101820
#define DEFAULT_ACCEL       0
#define DEFAULT_BAR_TOP     1
#define DEFAULT_CURSOR_SIZE 1
#define DEFAULT_CURSOR_COL  0xFFFFFF

#define CURSOR_SIZE_MIN 1
#define CURSOR_SIZE_MAX 4

/* As long as the compositor's own buffer for it, so that what fits in the
 * field is what fits in the file. */
#define TEXT_MAX 128

struct settings {
    uint32_t background;        /* 0xRRGGBB */
    int      accel;             /* pointer_accel in hundredths, -100..100 */
    int      bar_top;           /* 1 for top, 0 for bottom */

    int      cursor_size;       /* whole-pixel scale of the arrow, 1 to 4 */
    uint32_t cursor_colour;     /* what the inside of the arrow is filled with */

    /* The text across the background, exactly as it sits in the file: `${CPU}`
     * and `\n` are kept as the characters they are, because they are
     * instructions to the compositor rather than to this window.  Showing a
     * reading in the field being typed in would mean the field no longer said
     * what would be saved. */
    char     text[TEXT_MAX];
};

static struct settings now;      /* what the screen is doing */
static struct settings on_disk;  /* what the file says, for the unsaved mark */

/* The colours offered as one click each.  Mixing a colour from three sliders is
 * exact and slow, and each of these two lists is what its colour is usually
 * wanted for: a background is a dark colour that is not quite black, and a
 * cursor is a bright one that will be seen.  The sliders remain the way to any
 * other. */
static const uint32_t bg_presets[] = {
    0x101820,   /* sway's own */
    0x000000,
    0x2E3436,
    0x1A2B45,
    0x14342B,
    0x3B2540,
    0x4A3B2A,
    0x777777,
};

static const uint32_t cursor_presets[] = {
    0xFFFFFF,
    0xC0C0C0,
    0xFFD24A,
    0xFF4444,
    0x4AE07A,
    0x4AC0FF,
    0xC77DFF,
    0x000000,
};

#define PRESET_COUNT ((int)(sizeof(bg_presets) / sizeof(bg_presets[0])))

/* ------------------------------------------------------------------ *
 *  Controls
 *
 *  One table of rectangles, worked out once per frame and used by both the
 *  drawing and the clicking.  A control whose picture and whose hit box are
 *  computed in two places is a control that eventually stops agreeing with
 *  itself, and the arithmetic is the same arithmetic either way.
 * ------------------------------------------------------------------ */

enum control {
    CTL_BG_R, CTL_BG_G, CTL_BG_B,
    CTL_TEXT,
    CTL_TOP, CTL_BOTTOM,
    CTL_ACCEL,
    CTL_SIZE1, CTL_SIZE2, CTL_SIZE3, CTL_SIZE4,
    CTL_CUR_R, CTL_CUR_G, CTL_CUR_B,
    CTL_SAVE, CTL_RELOAD, CTL_CLOSE,

    /* Everything above this is in the Tab order.  The swatches are not: they
     * are a shortcut to a colour the sliders can also reach, and sixteen more
     * stops would make Tab the long way round the window. */
    FOCUSABLE,

    CTL_BG_PRESET  = FOCUSABLE,
    CTL_CUR_PRESET = CTL_BG_PRESET + PRESET_COUNT,
    CONTROL_COUNT  = CTL_CUR_PRESET + PRESET_COUNT
};

struct rect {
    int x, y, w, h;
};

static struct rect rects[CONTROL_COUNT];

static int focused  = CTL_BG_R;
static int dragging = -1;       /* the slider the button is held down on */
static int caret;               /* where typing goes in the text field */

static int inside(const struct rect *r, int x, int y)
{
    return r->w > 0 && r->h > 0 &&
           x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static int is_slider(int control)
{
    return (control >= CTL_BG_R && control <= CTL_BG_B) ||
           control == CTL_ACCEL ||
           (control >= CTL_CUR_R && control <= CTL_CUR_B);
}

/* A colour slider belongs to one of the two colours, and the three of them
 * together are R, G and B in that order. */
static int slider_is_cursor(int control)
{
    return control >= CTL_CUR_R && control <= CTL_CUR_B;
}

static int slider_shift(int control)
{
    int index = slider_is_cursor(control) ? control - CTL_CUR_R
                                          : control - CTL_BG_R;

    return 16 - index * 8;
}

/* The three buttons along the bottom stay where they are while everything
 * above them scrolls: a settings window whose Save can be scrolled out of
 * reach is a settings window that cannot be saved. */
static int is_pinned(int control)
{
    return control == CTL_SAVE || control == CTL_RELOAD ||
           control == CTL_CLOSE;
}

/* ------------------------------------------------------------------ *
 *  What is on screen
 * ------------------------------------------------------------------ */

struct frame {
    struct wl_buffer *buffer;
    uint32_t         *pixels;
    int               busy;
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

    /* Where the pointer is.  A button event does not carry a position -- the
     * protocol says the pointer is wherever the last motion left it -- so a
     * click can only be placed by having followed it there. */
    int ptr_x, ptr_y;
} app;


static char config_path[W_PATH_MAX + 1];
static char status[160];
static int  status_bad;

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

/* ------------------------------------------------------------------ *
 *  Numbers, as sway writes them
 * ------------------------------------------------------------------ */

/* pointer_accel is written with a decimal point everywhere it is documented,
 * and there is no floating point in a WOS program to print one with -- the
 * kernel runs with the unit switched off.  Hundredths, printed by hand. */
static void accel_text(char *out, wsize_t size, int hundredths)
{
    int value = hundredths < 0 ? -hundredths : hundredths;

    wsnprintf(out, size, "%s%d.%02d", hundredths < 0 ? "-" : "",
              value / 100, value % 100);
}

/* The same straight line sway's parser draws through the same three points:
 * -1 is a quarter speed, 0 leaves the mouse alone, 1 is four times.  Kept
 * identical on purpose -- this window shows a percentage next to the slider,
 * and a percentage that did not match what the compositor would do with the
 * number would be a lie told in the name of being helpful. */
static int percent_from_accel(int accel)
{
    return accel <= 0 ? 100 + accel * 3 / 4 : 100 + accel * 3;
}

static int accel_from_percent(int percent)
{
    return percent <= 100 ? (percent - 100) * 4 / 3 : (percent - 100) / 3;
}

static int clamp(int value, int low, int high)
{
    if (value < low)  return low;
    if (value > high) return high;
    return value;
}

/* "#rrggbb", "rrggbb", or the eight-digit form with an alpha this machine
 * cannot blend and therefore drops. */
static int parse_colour(const char *s, int len, uint32_t *out)
{
    uint32_t value = 0;
    int      digits = 0;

    if (len > 0 && *s == '#') {
        s++;
        len--;
    }

    for (int i = 0; i < len; i++, digits++) {
        int d;

        if (s[i] >= '0' && s[i] <= '9')      d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return 0;

        value = value * 16 + (uint32_t)d;
    }

    if (digits == 8) {
        *out = (value >> 8) & 0xFFFFFF;
        return 1;
    }
    if (digits == 6) {
        *out = value & 0xFFFFFF;
        return 1;
    }
    return 0;
}

/* A number with a decimal point, in hundredths.  atoi() would read "0.5" as
 * the one value that means "change nothing", so half speed would arrive as
 * none of it. */
static int parse_hundredths(const char *s, int len, int *out)
{
    int sign = 1, whole = 0, frac = 0, scale = 10, digits = 0, i = 0;

    if (i < len && (s[i] == '-' || s[i] == '+'))
        sign = (s[i++] == '-') ? -1 : 1;

    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, digits++)
        whole = whole * 10 + (s[i] - '0');

    if (i < len && s[i] == '.') {
        i++;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, digits++) {
            if (scale) {
                frac += (s[i] - '0') * scale;
                scale /= 10;
            }
        }
    }

    if (!digits || i != len)
        return 0;

    *out = sign * (whole * 100 + frac);
    return 1;
}

/* ------------------------------------------------------------------ *
 *  Talking to sway
 *
 *  i3's IPC, which is what sway kept: a magic string, a length, a type and a
 *  payload, over a socket.  This is `swaymsg` with no printing -- deliberately
 *  the same wire, because a settings window that reached into the compositor
 *  some other way would be able to set things `swaymsg` could not, and then
 *  the file it writes would no longer be the whole truth.
 * ------------------------------------------------------------------ */

static int sway_missing;            /* said so once; do not keep saying it */

/* A sway command, exactly as it would be written in the file.  Silent when it
 * works, which is most of the time and every time a slider moves. */
static int sway_command(const char *command)
{
    int r = wipc_command(WIPC_SWAY_SOCKET, command);

    if (r == -W_EINVAL) {
        complain("sway refused: %s", command);
        return r;
    }

    if (r < 0) {
        if (!sway_missing) {
            sway_missing = 1;
            complain("sway is not answering (%s); changes will only be saved",
                     wstrerror(-r));
        }
        return r;
    }

    sway_missing = 0;
    return 0;
}

/* Which file sway actually read.  Asking is better than guessing: sway looks
 * in two places and this has to write the one it found, or Save would write a
 * file nothing reads. */
static void find_config_path(void)
{
    static char reply[4096];

    if (wipc_message(WIPC_SWAY_SOCKET, WIPC_GET_VERSION, NULL, reply,
                     sizeof(reply)) > 0 &&
        wipc_field(reply, "loaded_config_file_name", config_path,
                   sizeof(config_path)) && config_path[0])
        return;

    /* No sway, or a sway too old to say.  Look where it would have looked. */
    char    home[W_PATH_MAX + 1];
    wuser_t user;
    wstat_t st;

    if (wuserinfo(wgetuid(), &user) == 0 && user.name[0])
        wsnprintf(home, sizeof(home), "/home/%s", user.name);
    else
        strlcpy(home, "/home/root", sizeof(home));

    wsnprintf(config_path, sizeof(config_path), "%s/.config/sway/config", home);
    if (wstat(config_path, &st) == 0)
        return;

    if (wstat("/etc/sway/config", &st) == 0)
        strlcpy(config_path, "/etc/sway/config", sizeof(config_path));

    /* And if neither exists the home one stands: Save creates it, which is
     * where sway will look first next time it starts. */
}

/* ------------------------------------------------------------------ *
 *  Applying
 * ------------------------------------------------------------------ */

enum {
    APPLY_BG            = 1,
    APPLY_ACCEL         = 2,
    APPLY_BAR           = 4,
    APPLY_CURSOR_SIZE   = 8,
    APPLY_CURSOR_COLOUR = 16,
    APPLY_TEXT          = 32,
};

static int          apply_pending;
static unsigned int last_apply;         /* wticks(), which counts hundredths */

/* Sending a command per pixel of a drag would open a socket sixty times a
 * second for a colour nobody can see change that finely.  A tenth of a second
 * is slow enough to be cheap and fast enough that the screen still follows the
 * hand; the release always sends, so the value that ends up in force is the
 * one the slider ended on and never one tenth of a second before it. */
#define APPLY_INTERVAL 10               /* ticks: a tenth of a second */

static void apply_now(void)
{
    char command[96];

    if (apply_pending & APPLY_BG) {
        wsnprintf(command, sizeof(command), "output * bg #%06x solid_color",
                  now.background);
        sway_command(command);
    }

    if (apply_pending & APPLY_ACCEL) {
        char value[16];

        accel_text(value, sizeof(value), now.accel);
        wsnprintf(command, sizeof(command), "input * pointer_accel %s", value);
        sway_command(command);
    }

    if (apply_pending & APPLY_BAR) {
        wsnprintf(command, sizeof(command), "bar position %s",
                  now.bar_top ? "top" : "bottom");
        sway_command(command);
    }

    if (apply_pending & APPLY_CURSOR_SIZE) {
        wsnprintf(command, sizeof(command), "cursor size %d", now.cursor_size);
        sway_command(command);
    }

    if (apply_pending & APPLY_CURSOR_COLOUR) {
        wsnprintf(command, sizeof(command), "cursor color #%06x",
                  now.cursor_colour);
        sway_command(command);
    }

    if (apply_pending & APPLY_TEXT) {
        /* Quoted, so that runs of spaces in it survive: the parser at the far
         * end splits a line into words and joins them back with one space
         * between, and only a quoted word comes through as it was typed. */
        char quoted[TEXT_MAX + 32];

        wsnprintf(quoted, sizeof(quoted), "background_text \"%s\"", now.text);
        sway_command(quoted);
    }

    apply_pending = 0;
    last_apply    = wticks();
}

/* Ask for a change to reach the compositor.  While a slider is being dragged
 * it waits for the interval; otherwise it goes at once. */
static void apply(int what)
{
    apply_pending |= what;

    if (dragging < 0 || (unsigned)(wticks() - last_apply) >= APPLY_INTERVAL)
        apply_now();
}

static int unsaved(void)
{
    return now.background    != on_disk.background ||
           now.accel         != on_disk.accel ||
           now.bar_top       != on_disk.bar_top ||
           now.cursor_size   != on_disk.cursor_size ||
           now.cursor_colour != on_disk.cursor_colour ||
           strcmp(now.text, on_disk.text) != 0;
}

/* ------------------------------------------------------------------ *
 *  The configuration file
 *
 *  Read to find out where the sliders start, and written to keep them there.
 *  Written by editing rather than by generating: the file is mostly comments
 *  and key bindings that this window knows nothing about, and a settings
 *  program that rewrote it from what it understands would quietly delete
 *  everything it does not.  So a line that sets one of these three has its
 *  value replaced where it stands, and a setting the file never mentions is
 *  appended at the end.
 * ------------------------------------------------------------------ */

#define CONFIG_MAX 32768

static char  text[CONFIG_MAX];
static int   text_len;
static int   text_complete;           /* the whole file fitted in the buffer */

/* One line, split on whitespace, with each word's offset kept so that a value
 * can be replaced without disturbing the spacing, the identifier, or anything
 * else that was on the line. */
#define MAX_WORDS 12

struct words {
    const char *at[MAX_WORDS];
    int         len[MAX_WORDS];
    int         count;
};

/* A line is a length and not a string -- it is a slice of the file still
 * sitting in its buffer -- so the brace that opens a block is looked for by
 * hand rather than with strchr(). */
static int line_has(const char *line, int len, char c)
{
    for (int i = 0; i < len; i++)
        if (line[i] == c)
            return 1;

    return 0;
}

static void split_words(const char *line, int len, struct words *w)
{
    int i = 0;

    w->count = 0;

    while (i < len && w->count < MAX_WORDS) {
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;
        if (i >= len)
            break;

        int start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t')
            i++;

        w->at[w->count]  = line + start;
        w->len[w->count] = i - start;
        w->count++;
    }
}

static int word_is(const struct words *w, int index, const char *name)
{
    if (index >= w->count)
        return 0;

    int len = (int)strlen(name);

    return w->len[index] == len && strncmp(w->at[index], name, (wsize_t)len) == 0;
}

/* What a line sets, if anything, and which of its words holds the value. */
enum setting {
    SET_NONE, SET_BG, SET_ACCEL, SET_BAR,
    SET_CURSOR_SIZE, SET_CURSOR_COLOUR, SET_TEXT,
    SET_COUNT
};

/* `in_block` is the first word of the heading of the block this line is
 * inside -- "bar" or "input" -- because the lines of a block are the same
 * commands with the heading left off:
 *
 *     bar { position top }        is        bar position top
 *     input * { pointer_accel 1 } is        input * pointer_accel 1
 *
 * so both spellings have to be recognised, and a file that uses the block form
 * has to be edited in the block rather than have a second setting appended
 * underneath it that contradicts it.
 */
static enum setting line_sets(const struct words *w, const char *in_block,
                              int *value_index)
{
    if (in_block && strcmp(in_block, "bar") == 0) {
        if (word_is(w, 0, "position") && w->count > 1) {
            *value_index = 1;
            return SET_BAR;
        }
        return SET_NONE;
    }

    if (in_block && strcmp(in_block, "input") == 0) {
        if (word_is(w, 0, "pointer_accel") && w->count > 1) {
            *value_index = 1;
            return SET_ACCEL;
        }
        return SET_NONE;
    }

    if (in_block)
        return SET_NONE;

    /* `output <name> bg <colour> [solid_color]` */
    if (word_is(w, 0, "output") && w->count > 3 && word_is(w, 2, "bg")) {
        *value_index = 3;
        return SET_BG;
    }

    /* `input <identifier> pointer_accel <value>` */
    if (word_is(w, 0, "input") && w->count > 3 &&
        word_is(w, 2, "pointer_accel")) {
        *value_index = 3;
        return SET_ACCEL;
    }

    /* `bar position <where>`, and `bar <id> position <where>` -- sway allows a
     * bar to be named even where, as here, there is only one. */
    if (word_is(w, 0, "bar")) {
        if (word_is(w, 1, "position") && w->count > 2) {
            *value_index = 2;
            return SET_BAR;
        }
        if (w->count > 3 && word_is(w, 2, "position")) {
            *value_index = 3;
            return SET_BAR;
        }
    }

    /* `cursor size <n>` and `cursor color <#rrggbb>` */
    if (word_is(w, 0, "cursor") && w->count > 2) {
        if (word_is(w, 1, "size")) {
            *value_index = 2;
            return SET_CURSOR_SIZE;
        }
        if (word_is(w, 1, "color") || word_is(w, 1, "colour")) {
            *value_index = 2;
            return SET_CURSOR_COLOUR;
        }
    }

    /* `background_text <the rest of the line>`.  The value is everything after
     * the first word rather than one word of it, and the caller is told where
     * it starts; how far it runs is the end of the line, which is why this is
     * the one setting whose length is worked out there. */
    if (word_is(w, 0, "background_text") && w->count > 1) {
        *value_index = 1;
        return SET_TEXT;
    }

    return SET_NONE;
}

/* Walk the file a line at a time, telling the caller what each line sets.
 * Both reading and writing go through this, so the line Save edits is
 * guaranteed to be the line the sliders were loaded from. */
typedef void (*line_fn)(const char *line, int len, enum setting what,
                        int value_at, int value_len, void *data);

/* How far a setting's value runs.  One word for all of them but the desktop
 * text, whose value is the whole of the rest of the line: it is a sentence
 * with spaces in it rather than a number, and stopping at the first space
 * would read one word of it and write the file back with the rest deleted. */
static int value_span(const struct words *w, int index, const char *line,
                      int len, enum setting what)
{
    if (what != SET_TEXT)
        return w->len[index];

    int at = (int)(w->at[index] - line);

    while (len > at && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;

    return len - at;
}

static void walk_config(line_fn fn, void *data)
{
    char block[16] = "";
    int  depth     = 0;
    int  at        = 0;

    while (at <= text_len) {
        int start = at;

        while (at < text_len && text[at] != '\n')
            at++;

        int         len  = at - start;
        const char *line = text + start;

        at++;                                   /* past the newline */

        /* A '#' begins a comment only at the start of a line: anywhere else it
         * is a colour, and treating `client.focused #4c7899` as a comment
         * would lose the line. */
        int lead = 0;
        while (lead < len && (line[lead] == ' ' || line[lead] == '\t'))
            lead++;

        int is_comment = (lead >= len || line[lead] == '#');

        struct words w;
        split_words(line, len, &w);

        enum setting what     = SET_NONE;
        int          value_at = 0, value_len = 0;

        if (!is_comment) {
            if (depth > 0) {
                if (line_has(line, len, '}')) {
                    if (--depth == 0)
                        block[0] = '\0';
                } else if (line_has(line, len, '{')) {
                    depth++;
                } else if (depth == 1 && block[0]) {
                    int index;
                    what = line_sets(&w, block, &index);
                    if (what != SET_NONE) {
                        value_at  = (int)(w.at[index] - line);
                        value_len = value_span(&w, index, line, len, what);
                    }
                }
            } else if (line_has(line, len, '{')) {
                depth++;
                if (w.count > 0 && w.len[0] < (int)sizeof(block)) {
                    memcpy(block, w.at[0], (wsize_t)w.len[0]);
                    block[w.len[0]] = '\0';

                    /* Only the two headings whose contents are commands this
                     * program knows.  Anything else is a block to be stepped
                     * over with its lines left alone. */
                    if (strcmp(block, "bar") != 0 && strcmp(block, "input") != 0)
                        block[0] = '\0';
                }
            } else {
                int index;
                what = line_sets(&w, NULL, &index);
                if (what != SET_NONE) {
                    value_at  = (int)(w.at[index] - line);
                    value_len = value_span(&w, index, line, len, what);
                }
            }
        }

        fn(line, len, what, value_at, value_len, data);

        if (start >= text_len)
            break;
    }
}

static int read_config_file(void)
{
    text_len      = 0;
    text[0]       = '\0';
    text_complete = 1;

    int fd = wopen(config_path, W_O_RDONLY);
    if (fd < 0)
        return fd;

    int n = wread(fd, text, sizeof(text) - 1);

    /* One more byte's worth would mean the file is bigger than this buffer.
     * Saving a truncated copy over somebody's configuration is the one
     * unrecoverable thing this program could do, so it is remembered here and
     * refused there. */
    if (n == (int)sizeof(text) - 1)
        text_complete = 0;

    wclose(fd);

    if (n < 0)
        return n;

    text_len   = n;
    text[n]    = '\0';
    return 0;
}

static void collect(const char *line, int len, enum setting what, int value_at,
                    int value_len, void *data)
{
    (void)len;
    (void)data;

    const char *value = line + value_at;

    switch (what) {
    case SET_BG:
        parse_colour(value, value_len, &now.background);
        break;

    case SET_ACCEL:
        if (parse_hundredths(value, value_len, &now.accel))
            now.accel = clamp(now.accel, -100, 100);
        break;

    case SET_BAR:
        if (value_len == 3 && strncmp(value, "top", 3) == 0)
            now.bar_top = 1;
        else if (value_len == 6 && strncmp(value, "bottom", 6) == 0)
            now.bar_top = 0;
        break;

    case SET_CURSOR_SIZE: {
        int size = 0;

        for (int i = 0; i < value_len; i++) {
            if (value[i] < '0' || value[i] > '9')
                return;
            size = size * 10 + (value[i] - '0');
        }

        now.cursor_size = clamp(size, CURSOR_SIZE_MIN, CURSOR_SIZE_MAX);
        break;
    }

    case SET_CURSOR_COLOUR:
        parse_colour(value, value_len, &now.cursor_colour);
        break;

    case SET_TEXT: {
        /* The quotes are the file's way of keeping the spaces, not part of
         * what is written on the screen, so the field holds what is between
         * them.  Everything else -- ${CPU}, a `\n` -- is kept exactly as it
         * was written, because it is the compositor that reads it. */
        int from = 0, to = value_len;

        if (to - from >= 2 && value[from] == '"' && value[to - 1] == '"') {
            from++;
            to--;
        }

        if (to - from >= (int)sizeof(now.text))
            to = from + (int)sizeof(now.text) - 1;

        memcpy(now.text, value + from, (wsize_t)(to - from));
        now.text[to - from] = '\0';
        break;
    }

    case SET_NONE:
    case SET_COUNT:
        break;
    }
}

/* Where the sliders start.
 *
 * The file first, because that is what Save will write and what `reload` would
 * put back.  Where the file says nothing, sway's own default -- except for the
 * pointer, which is asked of the kernel that is actually moving it: sway's
 * default is applied at startup, so on a file with no pointer_accel line the
 * kernel's answer and the default agree, and on a machine where something else
 * changed it the slider starts where the mouse really is. */
static void load_settings(void)
{
    now.background    = DEFAULT_BG;
    now.accel         = DEFAULT_ACCEL;
    now.bar_top       = DEFAULT_BAR_TOP;
    now.cursor_size   = DEFAULT_CURSOR_SIZE;
    now.cursor_colour = DEFAULT_CURSOR_COL;
    now.text[0]       = '\0';

    int speed = wpointerspeed(-1);
    if (speed > 0)
        now.accel = clamp(accel_from_percent(speed), -100, 100);

    int r = read_config_file();

    if (r == 0)
        walk_config(collect, NULL);

    on_disk = now;

    if (r == -W_ENOENT)
        say("%s does not exist yet; Save will create it", config_path);
    else if (r < 0)
        complain("cannot read %s: %s", config_path, wstrerror(-r));
    else if (!text_complete)
        complain("%s is larger than %d bytes; Save is refused", config_path,
                 CONFIG_MAX);
    else
        say("read %s", config_path);
}

/* --- writing it back --- */

struct writer {
    char *out;
    int   len;
    int   size;
    int   found[SET_COUNT];         /* indexed by enum setting */
};

static void put_bytes(struct writer *w, const char *s, int len)
{
    if (w->len + len > w->size)
        len = w->size - w->len;
    if (len <= 0)
        return;

    memcpy(w->out + w->len, s, (wsize_t)len);
    w->len += len;
}

static void put_str(struct writer *w, const char *s)
{
    put_bytes(w, s, (int)strlen(s));
}

/* The text of a setting's value, as it goes into the file. */
static void value_text(enum setting what, char *out, wsize_t size)
{
    switch (what) {
    case SET_BG:
        wsnprintf(out, size, "#%06x", now.background);
        break;
    case SET_ACCEL:
        accel_text(out, size, now.accel);
        break;
    case SET_BAR:
        strlcpy(out, now.bar_top ? "top" : "bottom", size);
        break;
    case SET_CURSOR_SIZE:
        wsnprintf(out, size, "%d", now.cursor_size);
        break;
    case SET_CURSOR_COLOUR:
        wsnprintf(out, size, "#%06x", now.cursor_colour);
        break;
    case SET_TEXT:
        /* Quoted so that the spaces in it survive being split into words and
         * joined back together by the parser that reads this. */
        wsnprintf(out, size, "\"%s\"", now.text);
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void rewrite(const char *line, int len, enum setting what, int value_at,
                    int value_len, void *data)
{
    struct writer *w = data;

    /* A second line setting the same thing is left exactly as it was.  sway
     * takes the last one, so rewriting only the first would produce a file
     * whose visible value is not the one in force. */
    if (what != SET_NONE && !w->found[what]) {
        char value[TEXT_MAX + 4];

        value_text(what, value, sizeof(value));

        put_bytes(w, line, value_at);
        put_str(w, value);
        put_bytes(w, line + value_at + value_len, len - value_at - value_len);

        w->found[what] = 1;
    } else {
        put_bytes(w, line, len);
    }

    put_str(w, "\n");
}

/* Make sure the directories above a path exist.  A file in ~/.config/sway on a
 * machine where nobody has ever configured anything is three directories that
 * are not there yet, and a Save that failed for that reason would look like a
 * Save that failed. */
static void make_parents(const char *path)
{
    char dir[W_PATH_MAX + 1];

    strlcpy(dir, path, sizeof(dir));

    for (char *at = dir + 1; *at; at++) {
        if (*at != '/')
            continue;

        *at = '\0';
        wmkdir(dir);
        *at = '/';
    }
}

static void save_settings(void)
{
    static char out[CONFIG_MAX + 1024];
    struct writer w = { out, 0, (int)sizeof(out), { 0, 0, 0, 0 } };

    /* Reread rather than trusting what was read at startup: the file may have
     * been edited by hand since, and writing a stale copy back would undo
     * that edit without ever having shown it. */
    int r = read_config_file();

    if (r < 0 && r != -W_ENOENT) {
        complain("cannot read %s: %s", config_path, wstrerror(-r));
        return;
    }

    if (!text_complete) {
        complain("%s is larger than %d bytes; not written", config_path,
                 CONFIG_MAX);
        return;
    }

    if (r == 0)
        walk_config(rewrite, &w);

    /* Whatever the file never mentioned goes at the end, under a heading that
     * says where it came from -- so the next person to read the file knows why
     * a handful of settings are sitting on their own after the bindings.
     *
     * The desktop text is the one that may legitimately be nothing at all, and
     * an empty `background_text ""` in the file is worth writing: it is how a
     * text that was set and then cleared stays cleared. */
    static const struct {
        enum setting what;
        const char  *before;
        const char  *after;
    } lines[] = {
        { SET_BG,            "output * bg ",           " solid_color\n" },
        { SET_TEXT,          "background_text ",       "\n" },
        { SET_BAR,           "bar position ",          "\n" },
        { SET_ACCEL,         "input * pointer_accel ", "\n" },
        { SET_CURSOR_SIZE,   "cursor size ",           "\n" },
        { SET_CURSOR_COLOUR, "cursor color ",          "\n" },
    };

    int missing = 0;

    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        if (!w.found[lines[i].what])
            missing = 1;

    if (missing) {
        char value[TEXT_MAX + 4];

        put_str(&w, "\n### Written by swaysettings\n\n");

        for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
            if (w.found[lines[i].what])
                continue;

            value_text(lines[i].what, value, sizeof(value));
            put_str(&w, lines[i].before);
            put_str(&w, value);
            put_str(&w, lines[i].after);
        }
    }

    make_parents(config_path);

    /* Beside the real file, not over it.  Writing straight into somebody's
     * configuration has a moment in the middle where the file is half of the
     * old one and half of the new, and a machine that stopped there -- out of
     * disk, powered off -- would leave a session that cannot start.  The new
     * text goes to a name nobody reads and then takes the real one in a single
     * step, so the file is only ever one whole version or the other. */
    char temp[W_PATH_MAX + 1];

    wsnprintf(temp, sizeof(temp), "%s.new", config_path);

    int fd = wopen(temp, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
    if (fd < 0) {
        complain("cannot write %s: %s", temp, wstrerror(-fd));
        return;
    }

    int written = 0;
    while (written < w.len) {
        int n = wwrite(fd, w.out + written, (wsize_t)(w.len - written));

        if (n <= 0) {
            wclose(fd);
            wunlink(temp);
            complain("could not finish writing %s", temp);
            return;
        }
        written += n;
    }

    wclose(fd);

    int moved = wrename(temp, config_path);
    if (moved < 0) {
        wunlink(temp);
        complain("cannot replace %s: %s", config_path, wstrerror(-moved));
        return;
    }

    on_disk = now;
    say("saved to %s", config_path);
}

/* sway's own reload: it rereads the file, which is also how a change that was
 * tried and not liked is thrown away.  The window is reloaded from the same
 * file afterwards, so what is on the sliders is what is now in force. */
static void reload_settings(void)
{
    int r = sway_command("reload");

    load_settings();
    apply_pending = 0;

    if (r == 0)
        say("reloaded %s", config_path);
}

/* ------------------------------------------------------------------ *
 *  Drawing
 *
 *  The primitives are the library's -- see wdraw.h -- and everything below
 *  goes through `canvas`, which is the frame currently being painted.  What is
 *  left here is the handful of shapes this window has and no other program
 *  does: a box, a focus ring, a slider, a button.
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

/* The same, saying where it ended, for text built out of pieces. */
static int draw_text_at(int x, int y, const char *s, uint32_t fg)
{
    return wdraw_text(&canvas, x, y, s, fg);
}

static void draw_text_fit(int x, int y, const char *s, uint32_t fg, int room)
{
    /* A path is cut at the front, because the end of one says more than the
     * beginning; everything else is cut at the back. */
    if (s && s[0] == '/')
        wdraw_text_fit_tail(&canvas, x, y, s, fg, room);
    else
        wdraw_text_fit(&canvas, x, y, s, fg, room);
}

static int text_width(const char *s)
{
    return wdraw_text_width(s);
}

static void draw_border(int x, int y, int w, int h, uint32_t colour)
{
    wdraw_border(&canvas, x, y, w, h, colour);
}

static void draw_box(const struct rect *r, uint32_t body, uint32_t edge)
{
    fill(r->x, r->y, r->w, r->h, body);
    draw_border(r->x, r->y, r->w, r->h, edge);
}

/* The ring that says which control the keyboard is on.  Outside the control
 * rather than on it, so that it does not change what the control looks like --
 * a focused button that drew its own border in another colour would read as a
 * different kind of button. */
static void draw_focus_ring(const struct rect *r)
{
    draw_border(r->x - 3, r->y - 3, r->w + 6, r->h + 6, C_ACCENT);
}

/* ------------------------------------------------------------------ *
 *  Widgets
 * ------------------------------------------------------------------ */

/* Where the knob's centre sits for a value, and the value a position means.
 * One pair of functions, so that the knob is always drawn where a click on it
 * would leave it. */
static int knob_x(const struct rect *r, int value, int low, int high)
{
    int span = r->w - KNOB_W;

    if (span <= 0 || high == low)
        return r->x + r->w / 2;

    return r->x + KNOB_W / 2 + (value - low) * span / (high - low);
}

static int value_at_x(const struct rect *r, int x, int low, int high)
{
    int span = r->w - KNOB_W;

    if (span <= 0)
        return low;

    int value = low + (x - r->x - KNOB_W / 2) * (high - low) / span;

    return clamp(value, low, high);
}

static void draw_slider(int control, int value, int low, int high)
{
    const struct rect *r = &rects[control];

    if (r->w <= 0)
        return;

    int groove_y = r->y + (r->h - GROOVE_H) / 2;
    int at       = knob_x(r, value, low, high);

    fill(r->x, groove_y, r->w, GROOVE_H, C_SUNKEN);
    draw_border(r->x, groove_y, r->w, GROOVE_H, C_BORDER);

    /* The filled part reads as "how much", which is the whole point of a
     * slider over a number: it can be seen without being read. */
    if (at > r->x)
        fill(r->x + 1, groove_y + 1, at - r->x - 1, GROOVE_H - 2, C_ACCENT);

    struct rect knob = { at - KNOB_W / 2, r->y, KNOB_W, r->h };
    draw_box(&knob, C_BUTTON, C_ACCENT_LO);
    fill(knob.x + KNOB_W / 2, knob.y + 4, 1, knob.h - 8, C_ACCENT);

    if (focused == control)
        draw_focus_ring(r);
}

static void draw_button(int control, const char *label, int primary)
{
    const struct rect *r = &rects[control];

    if (r->w <= 0)
        return;

    draw_box(r, primary ? C_ACCENT : C_BUTTON, primary ? C_ACCENT_LO : C_BORDER);

    int tx = r->x + (r->w - text_width(label)) / 2;
    int ty = r->y + (r->h - CELL_H) / 2;

    draw_text_fit(tx, ty, label, primary ? C_ON_ACCENT : C_TEXT, r->w - 8);

    if (focused == control)
        draw_focus_ring(r);
}

/* One half of the bar-position control.  Two buttons of which exactly one is
 * filled: with two choices and no third, a pair of radio buttons costs a
 * column of circles to say what a filled half already says. */
static void draw_segment(int control, const char *label, int active)
{
    const struct rect *r = &rects[control];

    if (r->w <= 0)
        return;

    draw_box(r, active ? C_ACCENT : C_BUTTON, active ? C_ACCENT_LO : C_BORDER);

    int tx = r->x + (r->w - text_width(label)) / 2;
    int ty = r->y + (r->h - CELL_H) / 2;

    draw_text_fit(tx, ty, label, active ? C_ON_ACCENT : C_TEXT, r->w - 8);

    if (focused == control)
        draw_focus_ring(r);
}

/* ------------------------------------------------------------------ *
 *  The layout
 *
 *  Worked out from the window's size every frame, into the one table both the
 *  drawing and the clicking read.  Anything that does not fit ends up with a
 *  zero width, which draws nothing and cannot be clicked -- so a window made
 *  too small loses controls from the bottom rather than drawing them on top of
 *  each other.
 * ------------------------------------------------------------------ */

static int label_w;             /* the widest label in the colour sections */
static int section_y[5];        /* where each heading goes */
static int content_h;           /* how tall everything above the buttons is */
static int scroll;              /* how far down it has been pushed */

enum section { SEC_BG, SEC_TEXT, SEC_BAR, SEC_SPEED, SEC_CURSOR };

/* The strip the scrolling controls live in: everything between the header and
 * the row of buttons.  The buttons and the status line are not in it, which is
 * what makes them stay put. */
static int view_top(void)
{
    return HEADER_H;
}

static int view_bottom(void)
{
    int bottom = app.height - STATUS_H - PAD - BUTTON_H - PAD;

    return bottom < view_top() ? view_top() : bottom;
}

static void layout(void)
{
    for (int i = 0; i < CONTROL_COUNT; i++)
        rects[i] = (struct rect){ 0, 0, 0, 0 };

    label_w = 2 * CELL_W;                       /* "R " and its friends */

    int right     = app.width - PAD;
    int value_w   = 4 * CELL_W;                 /* "255" and a space */
    int slider_x  = PAD + PREVIEW + GAP + label_w;
    int slider_w  = right - slider_x - value_w;

    /* Content coordinates: the top of the first section is 0, and the scroll
     * is taken off at the end.  Laying out in the window's coordinates and
     * scrolling the drawing separately would leave the hit boxes behind. */
    int y = PAD;

    /* --- the background colour --- */

    section_y[SEC_BG] = y;
    y += CELL_H + 6;

    for (int i = 0; i < 3; i++)
        rects[CTL_BG_R + i] = (struct rect){
            slider_x, y + i * (SLIDER_H + 4), slider_w, SLIDER_H
        };

    y += 3 * (SLIDER_H + 4);

    int swatch_y = y + 4;
    for (int i = 0; i < PRESET_COUNT; i++) {
        int x = PAD + i * (SWATCH_W + 4);

        if (x + SWATCH_W > right)
            break;

        rects[CTL_BG_PRESET + i] = (struct rect){
            x, swatch_y, SWATCH_W, SWATCH_H
        };
    }

    y = swatch_y + SWATCH_H + GAP + 6;

    /* --- the text on it --- */

    section_y[SEC_TEXT] = y;
    y += CELL_H + 6;

    rects[CTL_TEXT] = (struct rect){ PAD, y, right - PAD, ENTRY_H };

    /* The line under the field says what ${CPU} and ${MEM} do, and is part of
     * the field's own height for the same reason "slower" and "faster" are
     * part of the slider's: a hint that is sometimes there and sometimes not
     * would move everything below it. */
    y += ENTRY_H + CELL_H + GAP + 6;

    /* --- the bar --- */

    section_y[SEC_BAR] = y;
    y += CELL_H + 6;

    rects[CTL_TOP]    = (struct rect){ PAD, y, SEG_W, BUTTON_H };
    rects[CTL_BOTTOM] = (struct rect){ PAD + SEG_W - 1, y, SEG_W, BUTTON_H };

    y += BUTTON_H + GAP + 6;

    /* --- the mouse's speed --- */

    section_y[SEC_SPEED] = y;
    y += CELL_H + 6;

    int reading_w = 13 * CELL_W;                /* "-0.40  (25%)" */

    rects[CTL_ACCEL] = (struct rect){
        PAD, y, right - PAD - reading_w, SLIDER_H
    };

    y += SLIDER_H + CELL_H + GAP + 6;

    /* --- the cursor: how big, and what colour --- */

    section_y[SEC_CURSOR] = y;
    y += CELL_H + 6;

    for (int i = 0; i < 4; i++)
        rects[CTL_SIZE1 + i] = (struct rect){
            PAD + i * (SIZE_W - 1), y, SIZE_W, BUTTON_H
        };

    y += BUTTON_H + GAP;

    for (int i = 0; i < 3; i++)
        rects[CTL_CUR_R + i] = (struct rect){
            slider_x, y + i * (SLIDER_H + 4), slider_w, SLIDER_H
        };

    y += 3 * (SLIDER_H + 4);

    swatch_y = y + 4;
    for (int i = 0; i < PRESET_COUNT; i++) {
        int x = PAD + i * (SWATCH_W + 4);

        if (x + SWATCH_W > right)
            break;

        rects[CTL_CUR_PRESET + i] = (struct rect){
            x, swatch_y, SWATCH_W, SWATCH_H
        };
    }

    content_h = swatch_y + SWATCH_H + PAD;

    /* How far it can be pushed, now that the height of both is known. */
    int slack = content_h - (view_bottom() - view_top());

    if (slack < 0)
        slack = 0;
    if (scroll > slack)
        scroll = slack;
    if (scroll < 0)
        scroll = 0;

    /* Everything laid out above moves together: the controls and the headings
     * over them are one picture, and an offset applied to one of them would
     * take the headings off the sections they name. */
    int offset = view_top() - scroll;

    for (int i = 0; i < FOCUSABLE; i++)
        rects[i].y += offset;
    for (int i = CTL_BG_PRESET; i < CONTROL_COUNT; i++)
        rects[i].y += offset;
    for (unsigned i = 0; i < sizeof(section_y) / sizeof(section_y[0]); i++)
        section_y[i] += offset;

    /* --- the buttons, pinned along the bottom --- */

    int buttons_y = app.height - STATUS_H - PAD - BUTTON_H;

    rects[CTL_CLOSE]  = (struct rect){ right - BUTTON_W, buttons_y,
                                       BUTTON_W, BUTTON_H };
    rects[CTL_RELOAD] = (struct rect){ right - 2 * BUTTON_W - GAP, buttons_y,
                                       BUTTON_W, BUTTON_H };
    rects[CTL_SAVE]   = (struct rect){ right - 3 * BUTTON_W - 2 * GAP,
                                       buttons_y, BUTTON_W, BUTTON_H };

    /* Anything that has run off the side of the window is not drawn and not
     * clickable.  Running off the top or the bottom is what scrolling is for,
     * and is handled where things are drawn and where they are clicked. */
    for (int i = 0; i < CONTROL_COUNT; i++) {
        struct rect *r = &rects[i];

        if (r->w < 12 || r->x < 0 || r->x + r->w > app.width)
            *r = (struct rect){ 0, 0, 0, 0 };
    }
}

/* Bring a control fully into view, for when the keyboard moved to one that is
 * off the strip: Tab has to be able to reach what it focuses. */
static void scroll_to(int control)
{
    const struct rect *r = &rects[control];

    if (!r->w || is_pinned(control))
        return;

    int top    = view_top();
    int bottom = view_bottom();

    if (r->y - 4 < top)
        scroll -= top - (r->y - 4);
    else if (r->y + r->h + 4 > bottom)
        scroll += (r->y + r->h + 4) - bottom;

    if (scroll < 0)
        scroll = 0;

    app.redraw = 1;
}

/* ------------------------------------------------------------------ *
 *  Drawing the window
 * ------------------------------------------------------------------ */

static void draw_header(void)
{
    fill(0, 0, app.width, HEADER_H, C_HEADER);
    fill(0, HEADER_H - 1, app.width, 1, C_BORDER);

    draw_text(PAD, (HEADER_H - CELL_H) / 2, "swaysettings", C_TEXT);

    /* Which file Save will write.  A settings window that did not say would
     * leave the answer to be guessed, and there are two places it could be. */
    int left = PAD + text_width("swaysettings") + 2 * GAP;
    int room = app.width - PAD - left;

    if (room >= 8 * CELL_W)
        draw_text_fit(app.width - PAD - room, (HEADER_H - CELL_H) / 2,
                      config_path, C_DIM, room);
}

static void draw_heading(int index, const char *label)
{
    draw_text(PAD, section_y[index], label, C_TEXT);
    fill(PAD + text_width(label) + GAP, section_y[index] + CELL_H / 2,
         app.width - PAD - (PAD + text_width(label) + GAP), 1, C_BORDER);
}

/* A colour, its three sliders, its swatches and a square of the thing itself.
 * Written once and used by both colours: a background and a cursor are chosen
 * the same way, and two copies of this would be two copies to keep agreeing. */
static void draw_colour_section(int first_slider, int first_swatch,
                                const uint32_t *swatches, uint32_t colour)
{
    static const char *const names[] = { "R", "G", "B" };

    struct rect preview = { PAD, rects[first_slider].y, PREVIEW, PREVIEW };

    if (rects[first_slider].w) {
        draw_box(&preview, colour, C_BORDER);

        char hex[16];
        wsnprintf(hex, sizeof(hex), "#%06x", colour);
        draw_text_fit(PAD, preview.y + PREVIEW + 4, hex, C_DIM, PREVIEW + GAP);
    }

    for (int i = 0; i < 3; i++) {
        const struct rect *r = &rects[first_slider + i];

        if (!r->w)
            continue;

        int value = (int)((colour >> (16 - i * 8)) & 0xFF);

        draw_text(r->x - label_w, r->y + (SLIDER_H - CELL_H) / 2, names[i],
                  C_DIM);
        draw_slider(first_slider + i, value, 0, 255);

        char reading[8];
        wsnprintf(reading, sizeof(reading), "%d", value);
        draw_text(r->x + r->w + CELL_W, r->y + (SLIDER_H - CELL_H) / 2,
                  reading, C_TEXT);
    }

    for (int i = 0; i < PRESET_COUNT; i++) {
        const struct rect *r = &rects[first_swatch + i];

        if (!r->w)
            continue;

        draw_box(r, swatches[i], colour == swatches[i] ? C_ACCENT : C_BORDER);

        if (colour == swatches[i])
            draw_border(r->x + 1, r->y + 1, r->w - 2, r->h - 2, C_ACCENT);
    }
}

static void draw_background_section(void)
{
    draw_heading(SEC_BG, "Background");
    draw_colour_section(CTL_BG_R, CTL_BG_PRESET, bg_presets, now.background);
}

/* The field the desktop text is typed into.
 *
 * Sunken, like every text field since the first one, and scrolled sideways
 * when what is in it is longer than it is: the caret has to stay visible, and
 * a field that showed the beginning of a line being typed at the end would
 * hide the thing being typed. */
static void draw_text_section(void)
{
    const struct rect *r = &rects[CTL_TEXT];

    draw_heading(SEC_TEXT, "Desktop text");

    if (!r->w)
        return;

    draw_box(r, C_BUTTON, C_BORDER);
    fill(r->x + 1, r->y + 1, r->w - 2, 1, C_SUNKEN);        /* a sunken lip */

    int room  = (r->w - 2 * 6) / CELL_W;
    int from  = 0;

    if (room > 0 && caret > room)
        from = caret - room;

    const char *shown = now.text + from;
    int text_y = r->y + (ENTRY_H - CELL_H) / 2;

    draw_text_fit(r->x + 6, text_y, shown, C_TEXT, r->w - 12);

    /* The caret, when this is where the keys are going. */
    if (focused == CTL_TEXT) {
        int at = r->x + 6 + (caret - from) * CELL_W;

        if (at < r->x + r->w - 2)
            fill(at, r->y + 4, 1, ENTRY_H - 8, C_ACCENT);

        draw_focus_ring(r);
    }

    /* Under the field: what the text will actually say once the compositor
     * has filled the figures in, or -- when there is nothing to fill in -- what
     * the names are.  The field itself holds the template, because that is
     * what gets saved; this is the other half of the answer. */
    char expanded[TEXT_MAX + 64];

    wstatus_expand(now.text, expanded, sizeof(expanded));

    if (now.text[0] && strcmp(expanded, now.text) != 0) {
        int at = draw_text_at(r->x, r->y + ENTRY_H + 2, "shows: ", C_DIM);

        /* A line break in the template is two characters here, and the
         * preview is one line: it is drawn as the arrow it is rather than
         * breaking the line the layout counted on. */
        for (const char *p = expanded; *p; p++)
            at = draw_text_at(at, r->y + ENTRY_H + 2,
                              *p == '\n' ? "\x1a" : (char[]){ *p, 0 }, C_TEXT);
    } else {
        draw_text_fit(r->x, r->y + ENTRY_H + 2,
                      "${CPU} ${MEM} ${TIME} ${DATE} ${BATTERY} become "
                      "figures; \\n breaks a line", C_DIM, r->w);
    }
}

static void draw_cursor_section(void)
{
    static const char *const sizes[] = { "1x", "2x", "3x", "4x" };

    draw_heading(SEC_CURSOR, "Cursor");

    for (int i = 0; i < 4; i++)
        draw_segment(CTL_SIZE1 + i, sizes[i], now.cursor_size == i + 1);

    /* The arrow itself, at the size and colour chosen, beside the sizes: the
     * only honest preview of a cursor is the cursor. */
    const struct rect *r = &rects[CTL_SIZE1];

    if (r->w) {
        /* The same shape the compositor draws, from the same place, so that a
         * preview of the cursor cannot stop being one. */
        int size = now.cursor_size;
        int x    = PAD + 4 * (SIZE_W - 1) + GAP * 2;
        int y    = r->y + (BUTTON_H - WDRAW_CURSOR_H * size) / 2;

        if (y < r->y - 12)
            y = r->y - 12;

        wdraw_cursor(&canvas, x, y, size, now.cursor_colour);
    }

    draw_colour_section(CTL_CUR_R, CTL_CUR_PRESET, cursor_presets,
                        now.cursor_colour);
}

static void draw_mouse_section(void)
{
    const struct rect *r = &rects[CTL_ACCEL];

    draw_heading(SEC_SPEED, "Mouse speed");

    if (!r->w)
        return;

    draw_slider(CTL_ACCEL, now.accel, -100, 100);

    /* The number sway would read, and what it does -- because "0.40" is the
     * setting and "250%" is the answer to the question that was actually
     * being asked. */
    char value[16], reading[32];

    accel_text(value, sizeof(value), now.accel);
    wsnprintf(reading, sizeof(reading), "%s (%d%%)", value,
              percent_from_accel(now.accel));

    draw_text_fit(r->x + r->w + CELL_W, r->y + (SLIDER_H - CELL_H) / 2, reading,
                  C_TEXT, app.width - PAD - (r->x + r->w + CELL_W));

    /* Which end is which, once, under the ends themselves. */
    if (r->w > 24 * CELL_W) {
        draw_text(r->x, r->y + SLIDER_H + 2, "slower", C_DIM);
        draw_text(r->x + r->w - text_width("faster"), r->y + SLIDER_H + 2,
                  "faster", C_DIM);
    }
}

static void draw_bar_section(void)
{
    draw_heading(SEC_BAR, "Bar position");

    draw_segment(CTL_TOP, "Top", now.bar_top);
    draw_segment(CTL_BOTTOM, "Bottom", !now.bar_top);
}

/* How much of the window there is, and where in it you are.  Only when there
 * is more than fits: a scrollbar on a window with nothing to scroll is a
 * control that does nothing, drawn where a control would be. */
static void draw_scrollbar(void)
{
    int view_h = view_bottom() - view_top();

    if (view_h <= 0 || content_h <= view_h)
        return;

    int x = app.width - SCROLLBAR_W - 2;
    int h = view_h * view_h / content_h;
    int y = view_top() + (view_h - h) * scroll / (content_h - view_h);

    if (h < 16)
        h = 16;

    /* Dark enough to be seen against the window it is on: a scrollbar drawn in
     * two shades of the background is a scrollbar nobody knows is there, and
     * this is the only thing saying there is more window than is showing. */
    fill(x, view_top(), SCROLLBAR_W, view_h, C_SUNKEN);
    fill(x, y, SCROLLBAR_W, h, C_DIM);
}

static void draw_status(void)
{
    int y = app.height - STATUS_H;

    fill(0, y, app.width, STATUS_H, C_HEADER);
    fill(0, y, app.width, 1, C_BORDER);

    int text_y = y + (STATUS_H - CELL_H) / 2;
    int room   = app.width - 2 * PAD;

    /* The unsaved mark takes its space from the message rather than sitting on
     * top of it: a status line that overwrote the reason for the change with
     * the fact of it would be the wrong way round. */
    if (unsaved()) {
        const char *mark = "unsaved";
        int         w    = text_width(mark);

        draw_text(app.width - PAD - w, text_y, mark, C_WARN);
        room -= w + GAP;
    }

    if (status[0])
        draw_text_fit(PAD, text_y, status, status_bad ? C_WARN : C_TEXT, room);
}

static void draw(struct frame *f)
{
    canvas = wcanvas(f->pixels, app.width, app.height);

    layout();

    fill(0, 0, app.width, app.height, C_WINDOW);

    /* The scrolling half first, then everything that stays put painted over
     * the top of it.  A clip rectangle threaded through every primitive would
     * be the other way to keep a scrolled control out of the header, and this
     * is the same result in one line: what is pinned is drawn last and is
     * opaque. */
    draw_background_section();
    draw_text_section();
    draw_bar_section();
    draw_mouse_section();
    draw_cursor_section();

    draw_scrollbar();

    fill(0, 0, app.width, HEADER_H, C_WINDOW);
    draw_header();

    fill(0, view_bottom(), app.width, app.height - view_bottom(), C_WINDOW);

    draw_button(CTL_SAVE, "Save", 1);
    draw_button(CTL_RELOAD, "Reload", 0);
    draw_button(CTL_CLOSE, "Close", 0);

    draw_status();
}

/* ------------------------------------------------------------------ *
 *  Changing something
 * ------------------------------------------------------------------ */

static void set_channel(int control, int value)
{
    uint32_t *colour = slider_is_cursor(control) ? &now.cursor_colour
                                                 : &now.background;
    int shift = slider_shift(control);

    value = clamp(value, 0, 255);

    *colour &= ~(0xFFu << shift);
    *colour |= (uint32_t)value << shift;

    apply(slider_is_cursor(control) ? APPLY_CURSOR_COLOUR : APPLY_BG);
    app.redraw = 1;
}

static int channel_of(int control)
{
    uint32_t colour = slider_is_cursor(control) ? now.cursor_colour
                                                : now.background;

    return (int)((colour >> slider_shift(control)) & 0xFF);
}

static void set_cursor_size(int size)
{
    now.cursor_size = clamp(size, CURSOR_SIZE_MIN, CURSOR_SIZE_MAX);

    apply(APPLY_CURSOR_SIZE);
    app.redraw = 1;
}

static void set_cursor_colour(uint32_t colour)
{
    now.cursor_colour = colour;

    apply(APPLY_CURSOR_COLOUR);
    app.redraw = 1;
}

static void set_accel(int hundredths)
{
    now.accel = clamp(hundredths, -100, 100);

    apply(APPLY_ACCEL);
    app.redraw = 1;
}

static void set_bar_top(int top)
{
    if (now.bar_top == top)
        return;

    now.bar_top = top;

    apply(APPLY_BAR);
    app.redraw = 1;
}

static void set_background(uint32_t colour)
{
    now.background = colour;

    apply(APPLY_BG);
    app.redraw = 1;
}

/* A slider dragged or clicked to a position. */
static void slider_to(int control, int x)
{
    const struct rect *r = &rects[control];

    if (!r->w)
        return;

    if (control == CTL_ACCEL)
        set_accel(value_at_x(r, x, -100, 100));
    else
        set_channel(control, value_at_x(r, x, 0, 255));
}

/* A slider nudged, by an arrow key or by the wheel.  The step is the smallest
 * change worth making by hand: one of 256 for a colour, and a twentieth of the
 * pointer's range, which is about as fine as the difference can be felt. */
static void slider_by(int control, int steps)
{
    if (control == CTL_ACCEL)
        set_accel(now.accel + steps * 5);
    else if (is_slider(control))
        set_channel(control, channel_of(control) + steps);
}

static void activate(int control)
{
    switch (control) {
    case CTL_TOP:    set_bar_top(1);      break;
    case CTL_BOTTOM: set_bar_top(0);      break;
    case CTL_SIZE1:
    case CTL_SIZE2:
    case CTL_SIZE3:
    case CTL_SIZE4:  set_cursor_size(control - CTL_SIZE1 + 1); break;
    case CTL_SAVE:   save_settings();     break;
    case CTL_RELOAD: reload_settings();   break;
    case CTL_CLOSE:  app.running = 0;     break;
    default: break;
    }

    app.redraw = 1;
}

/* --- the text field --- */

static void text_changed(void)
{
    apply(APPLY_TEXT);
    app.redraw = 1;
}

static void text_insert(uint32_t ch)
{
    wsize_t len = strlen(now.text);

    /* A double quote would end the quoting this is written back inside, and
     * there is no escape for one in a sway configuration file.  Refusing the
     * character is better than accepting it and writing a file that no longer
     * parses. */
    if (ch == '"' || ch < 0x20 || ch > 0x7E || len + 1 >= sizeof(now.text))
        return;

    if (caret > (int)len)
        caret = (int)len;

    memmove(now.text + caret + 1, now.text + caret, len - caret + 1);
    now.text[caret++] = (char)ch;

    text_changed();
}

static void text_delete(int before)
{
    wsize_t len = strlen(now.text);
    int     at  = before ? caret - 1 : caret;

    if (at < 0 || at >= (int)len)
        return;

    memmove(now.text + at, now.text + at + 1, len - at);

    if (before)
        caret--;

    text_changed();
}

/* ------------------------------------------------------------------ *
 *  The pointer
 * ------------------------------------------------------------------ */

/* A scrolled control only counts where it can be seen.  Half of one showing
 * under the header is half a control that must not answer a click that landed
 * on the header. */
static int visible(int control, int y)
{
    return is_pinned(control) || (y >= view_top() && y < view_bottom());
}

static int control_at(int x, int y)
{
    for (int i = 0; i < CONTROL_COUNT; i++)
        if (inside(&rects[i], x, y) && visible(i, y))
            return i;

    /* A click a little above or below a slider still belongs to it: the groove
     * is six pixels tall and nobody aims at six pixels, least of all with the
     * pointer they have opened this window to speed up. */
    static const int sliders[] = {
        CTL_BG_R, CTL_BG_G, CTL_BG_B, CTL_ACCEL,
        CTL_CUR_R, CTL_CUR_G, CTL_CUR_B,
    };

    for (unsigned i = 0; i < sizeof(sliders) / sizeof(sliders[0]); i++) {
        struct rect r = rects[sliders[i]];

        if (!r.w)
            continue;

        r.y -= 4;
        r.h += 8;

        if (inside(&r, x, y) && visible(sliders[i], y))
            return sliders[i];
    }

    return -1;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    app.ptr_x = wl_fixed_to_int(sx);
    app.ptr_y = wl_fixed_to_int(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    /* A button release that happens outside the window is never seen here, so
     * a drag that left is a drag that has ended. */
    if (dragging >= 0) {
        dragging = -1;
        apply_now();
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    app.ptr_x = wl_fixed_to_int(sx);
    app.ptr_y = wl_fixed_to_int(sy);

    if (dragging >= 0)
        slider_to(dragging, app.ptr_x);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    if (button != W_BTN_LEFT)
        return;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (dragging >= 0) {
            dragging = -1;
            apply_now();            /* the value it was let go on, exactly */
        }
        return;
    }

    int control = control_at(app.ptr_x, app.ptr_y);

    if (control < 0)
        return;

    /* Clicking anything moves the keyboard there too, so Tab carries on from
     * where the hand left off rather than from where it was before. */
    if (control < FOCUSABLE)
        focused = control;

    if (is_slider(control)) {
        dragging = control;
        slider_to(control, app.ptr_x);
        return;
    }

    if (control == CTL_TEXT) {
        /* The caret goes where it was clicked, counted in characters from
         * where the field starts showing them. */
        int len = (int)strlen(now.text);

        caret = (app.ptr_x - rects[CTL_TEXT].x - 6 + CELL_W / 2) / CELL_W;
        caret = clamp(caret, 0, len);
        return;
    }

    if (control >= CTL_CUR_PRESET) {
        set_cursor_colour(cursor_presets[control - CTL_CUR_PRESET]);
        return;
    }

    if (control >= CTL_BG_PRESET) {
        set_background(bg_presets[control - CTL_BG_PRESET]);
        return;
    }

    activate(control);
}

/* The wheel over a slider adjusts it, and anywhere else scrolls the window.
 * A slider that scrolled the window when the pointer was on it would be a
 * slider that could not be adjusted with the wheel at all, and a window that
 * only scrolled from the scrollbar would need the scrollbar aimed at. */
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    /* What a turn of the wheel is doing stays what it is doing until the hand
     * stops.  Deciding afresh on every notch reads the control under the
     * pointer, and the whole point of scrolling is that the controls are
     * moving past it: a scroll that started on blank space would carry on into
     * whichever slider arrived underneath and drag that instead. */
    static int          target;             /* the control, or -1 for the page */
    static unsigned int last_notch;

    int control;

    if (last_notch && (unsigned)(wticks() - last_notch) < 50) {
        control = target;
    } else {
        control = control_at(app.ptr_x, app.ptr_y);
        target  = control;
    }

    last_notch = wticks();

    if (!is_slider(control)) {
        int steps = wl_fixed_to_int(value) / 10;

        if (steps == 0)
            steps = value > 0 ? 1 : -1;

        scroll += steps * (SLIDER_H + 4) * 2;

        if (scroll < 0)
            scroll = 0;                 /* layout() caps the other end */

        app.redraw = 1;
        return;
    }

    int steps = wl_fixed_to_int(value) / 10;

    if (steps == 0)
        steps = value > 0 ? 1 : -1;

    /* Down the wheel is down the value, which on a horizontal slider means
     * left -- the direction every toolkit picked and none can justify beyond
     * everybody having picked it. */
    slider_by(control, -steps);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
};

/* ------------------------------------------------------------------ *
 *  The keyboard
 *
 *  Everything the mouse can do, because the reason to open this window may be
 *  that the mouse is unusable.
 * ------------------------------------------------------------------ */

#define KEY_TAB 0x200

#define KEY_DELETE 0x201

static int special_key(uint32_t code)
{
    switch (code) {
    case 103: return W_KEY_UP;
    case 108: return W_KEY_DOWN;
    case 105: return W_KEY_LEFT;
    case 106: return W_KEY_RIGHT;
    case 102: return W_KEY_HOME;
    case 107: return W_KEY_END;
    case 111: return KEY_DELETE;
    case 1:   return W_KEY_ESCAPE;
    case 15:  return KEY_TAB;
    default:  return 0;
    }
}

static void move_focus(int by)
{
    focused = (focused + by + FOCUSABLE) % FOCUSABLE;

    /* Typing starts at the end of what is already there, which is where a
     * person who has just arrived at a field wants to be. */
    if (focused == CTL_TEXT)
        caret = (int)strlen(now.text);

    scroll_to(focused);
    app.redraw = 1;
}

/* Keys while the text field has them.  Returns 1 when it dealt with the key,
 * because a field being typed into has to take the letters that are also this
 * window's shortcuts -- somebody typing "sway" into it is not asking for
 * Save, Reload and Save again. */
static int key_in_text(int key, uint32_t ch)
{
    switch (key) {
    case W_KEY_LEFT:
        if (caret > 0)
            caret--;
        app.redraw = 1;
        return 1;

    case W_KEY_RIGHT:
        if (caret < (int)strlen(now.text))
            caret++;
        app.redraw = 1;
        return 1;

    case W_KEY_HOME:
        caret = 0;
        app.redraw = 1;
        return 1;

    case W_KEY_END:
        caret = (int)strlen(now.text);
        app.redraw = 1;
        return 1;

    case KEY_DELETE:
        text_delete(0);
        return 1;

    case W_KEY_ESCAPE:
        /* Out of the field rather than out of the window: Escape in a text
         * field means "stop typing", and closing the window on it would be a
         * surprise with the person's hands already on the keys. */
        focused = CTL_TOP;
        app.redraw = 1;
        return 1;

    case 0:
        break;

    default:
        return 0;                       /* Tab and the arrows still navigate */
    }

    if (ch == '\b' || ch == 127) {
        text_delete(1);
        return 1;
    }

    if (ch == '\n' || ch == '\r')
        return 1;                       /* already applied; nothing to submit */

    if (ch) {
        text_insert(ch);
        return 1;
    }

    return 0;
}

static void key_pressed(uint32_t code)
{
    int      key = special_key(code);
    uint32_t ch  = key ? 0 : wkeychar(code, app.mods);

    if (focused == CTL_TEXT && key != KEY_TAB && key != W_KEY_UP &&
        key != W_KEY_DOWN && key_in_text(key, ch))
        return;

    switch (key) {
    case KEY_TAB:
        move_focus(app.mods & W_MOD_SHIFT ? -1 : 1);
        return;

    case W_KEY_DOWN:
        move_focus(1);
        return;

    case W_KEY_UP:
        move_focus(-1);
        return;

    case W_KEY_LEFT:
        if (is_slider(focused))
            slider_by(focused, -1);
        else if (focused == CTL_BOTTOM)
            set_bar_top(1), focused = CTL_TOP;
        else if (focused > CTL_SIZE1 && focused <= CTL_SIZE4)
            set_cursor_size(--focused - CTL_SIZE1 + 1);
        return;

    case W_KEY_RIGHT:
        if (is_slider(focused))
            slider_by(focused, 1);
        else if (focused == CTL_TOP)
            set_bar_top(0), focused = CTL_BOTTOM;
        else if (focused >= CTL_SIZE1 && focused < CTL_SIZE4)
            set_cursor_size(++focused - CTL_SIZE1 + 1);
        return;

    case W_KEY_HOME:
        if (focused == CTL_ACCEL)
            set_accel(-100);
        else if (is_slider(focused))
            set_channel(focused, 0);
        return;

    case W_KEY_END:
        if (focused == CTL_ACCEL)
            set_accel(100);
        else if (is_slider(focused))
            set_channel(focused, 255);
        return;

    case W_KEY_ESCAPE:
        app.running = 0;
        return;

    default:
        break;
    }

    if (ch == '\n' || ch == '\r' || ch == ' ') {
        if (!is_slider(focused))
            activate(focused);
        return;
    }

    /* The three things worth a letter of their own, so that the common case is
     * one key rather than a walk through the Tab order. */
    if (ch == 's' || ch == 'S') {
        save_settings();
        app.redraw = 1;
    } else if (ch == 'r' || ch == 'R') {
        reload_settings();
        app.redraw = 1;
    } else if (ch == 'q' || ch == 'Q') {
        app.running = 0;
    }
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
    app.mods = depressed | locked;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    /* This compositor sends "no keymap" and the evdev codes themselves, which
     * wkeychar() reads.  The descriptor still arrives and is still ours. */
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

static int pool_create(void)
{
    int stride = app.width * 4;
    int one    = stride * app.height;

    pool_release();

    app.pool_bytes = one * 2;
    app.shm_fd     = wshmopen((unsigned int)app.pool_bytes);

    if (app.shm_fd < 0) {
        wfprintf(W_STDERR, "swaysettings: no memory for a %dx%d window: %s\n",
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
     * every descriptor in it, and closing this a second time would close
     * whatever had been given the number in the meantime. */
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

    /* Both still with the compositor: the next release brings one back, and
     * drawing into a buffer being read would show half of each. */
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
    app.width   = 470;
    app.height  = 350;
    app.shm_fd  = -1;
    app.running = 1;

    if (argc > 1) {
        int asked = strcmp(argv[1], "-h") == 0 ||
                    strcmp(argv[1], "--help") == 0;

        wfprintf(asked ? W_STDOUT : W_STDERR,
                 "usage: swaysettings\n\n"
                 "  The compositor's background colour, mouse speed and bar\n"
                 "  position, in a window.  Every change happens as it is\n"
                 "  made; Save writes them to sway's configuration file and\n"
                 "  Reload rereads it.\n");

        return asked ? 0 : 1;
    }

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        int why = wl_display_connect_error();

        wfprintf(W_STDERR, "swaysettings: no display server to connect to: "
                           "%s\n", wstrerror(-why));
        wfprintf(W_STDERR, -why == W_EPERM
                 ? "swaysettings: that session belongs to another user\n"
                 : "swaysettings: this is a Wayland client -- start sway "
                   "first, or edit the file with vim\n");
        return 1;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, NULL);

    /* Two roundtrips: the first brings the globals, the second the events the
     * things bound in the first sent back. */
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    if (!app.compositor || !app.shm || !app.wm_base) {
        wfprintf(W_STDERR, "swaysettings: that display server has no %s\n",
                 !app.compositor ? "wl_compositor"
                 : !app.shm      ? "wl_shm" : "xdg_wm_base");
        return 1;
    }

    app.surface     = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, NULL);

    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, NULL);

    xdg_toplevel_set_title(app.toplevel, "swaysettings");
    xdg_toplevel_set_app_id(app.toplevel, "swaysettings");

    /* A commit with no buffer: it says the window is ready to be told its
     * size, and nothing can be drawn until it has been. */
    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    find_config_path();
    load_settings();

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

        /* A drag that has stopped moving still has a change waiting on the
         * interval, and no further event would arrive to send it. */
        if (apply_pending &&
            (unsigned)(wticks() - last_apply) >= APPLY_INTERVAL)
            apply_now();

        if (app.redraw)
            present();
    }

    /* Whatever the last movement was, in force before the window goes: a
     * settings program that exited half a tenth of a second early would leave
     * the machine on a value nobody chose. */
    if (apply_pending)
        apply_now();

    pool_release();
    wl_display_disconnect(app.display);
    return 0;
}
