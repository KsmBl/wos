/* Putting it on the screen.
 *
 * Everything is composed into a back buffer and the whole thing is handed to
 * the kernel in one go.  Drawing straight onto the framebuffer would be
 * faster in the cases where only a little changed, and would tear in all the
 * others: a window moving while the screen is being scanned out is visible as
 * a torn edge, and the whole point of a compositor is that the screen only
 * ever shows finished frames.
 *
 * There is no alpha blending and no scaling.  A client's pixels are copied
 * into its tile as they are, and clipped where the tile ends -- a window that
 * has not yet redrawn itself at the size it was told is cropped rather than
 * stretched, which is honest and lasts only until its next frame.
 */

#include "sway.h"
#include <wdraw.h>

/* Everything is composed into one back buffer, so the canvas is made once and
 * used by every helper below.  The primitives themselves are the library's --
 * see wdraw.h -- because a compositor's fill and a client's fill are the same
 * arithmetic, and they were both here until they disagreed. */
static wcanvas_t canvas;

static void fill(int x, int y, int w, int h, uint32_t colour)
{
    wdraw_fill(&canvas, x, y, w, h, colour);
}

/* An outline `width` thick just inside the rectangle. */
static void frame_rect(int x, int y, int w, int h, int width, uint32_t colour)
{
    wdraw_frame(&canvas, x, y, w, h, width, colour);
}

/* Text, stopping at `max_width` pixels rather than running over whatever comes
 * next.  Returns where it got to. */
static int draw_text(int x, int y, const char *text, uint32_t fg, int max_width)
{
    return wdraw_text_max(&canvas, x, y, text, fg, max_width);
}

static int text_width(const char *s)
{
    return wdraw_text_width(s);
}

/* The same, for the pointer code: a click on the bar has to be matched against
 * the blocks draw_bar() drew, and both have to measure them the same way. */
int render_text_width(const char *s)
{
    return text_width(s);
}

/* ------------------------------------------------------------------ *
 *  Windows
 * ------------------------------------------------------------------ */

/* A client's pixels, clipped into the space the layout gave it. */
static void draw_buffer(struct buffer *b, int x, int y, int w, int h)
{
    if (!b || !b->pool || !b->pool->data)
        return;

    int sw = (int)sway.screen.width;
    int sh = (int)sway.screen.height;

    int copy_w = b->width  < w ? b->width  : w;
    int copy_h = b->height < h ? b->height : h;

    for (int row = 0; row < copy_h; row++) {
        int py = y + row;
        if (py < 0 || py >= sh)
            continue;

        /* Re-checked against the pool for every row.  The client owns this
         * memory and can have shrunk nothing, but the arithmetic that reaches
         * into it is ours, and a compositor reading past a pool would be
         * reading another process's pages. */
        uint64_t offset = (uint64_t)b->offset + (uint64_t)row * b->stride;
        if (offset + (uint64_t)copy_w * 4 > b->pool->size)
            break;

        const uint32_t *src = (const uint32_t *)(b->pool->data + offset);
        uint32_t       *dst = sway.back + (uint64_t)py * sw + x;

        int n = copy_w;
        if (x + n > sw)
            n = sw - x;

        for (int col = 0; col < n; col++)
            dst[col] = src[col] & 0x00FFFFFFu;   /* opaque: nothing blends */
    }
}

static void draw_view(struct view *v, int focused)
{
    const struct colours *c = focused ? &sway.config.focused
                                      : &sway.config.unfocused;

    int title = sway.config.title_height;
    int bw    = sway.config.border_width;

    /* The title bar, and the border, are the compositor's decoration -- the
     * client is never told about them and never draws them.  That is what
     * server-side decoration means, and it is why every window here looks the
     * same whatever toolkit drew its contents. */
    if (title > 0) {
        fill(v->x, v->y, v->w, title, c->background);

        const char *name = v->title[0] ? v->title
                         : v->app_id[0] ? v->app_id : "window";

        draw_text(v->x + 4, v->y + (title - 16) / 2, name, c->text, v->w - 8);
    }

    int inner_x = v->x + bw;
    int inner_y = v->y + title + bw;
    int inner_w = v->w - 2 * bw;
    int inner_h = v->h - title - 2 * bw;

    if (inner_w < 1 || inner_h < 1)
        return;

    /* Behind the client, so a window that has not redrawn at its new size
     * shows the compositor's own background rather than whatever was on the
     * screen before. */
    fill(inner_x, inner_y, inner_w, inner_h, 0x101010);
    draw_buffer(v->current, inner_x, inner_y, inner_w, inner_h);

    frame_rect(v->x, v->y + title, v->w, v->h - title, bw, c->child_border);
    if (title > 0)
        frame_rect(v->x, v->y, v->w, v->h, bw, c->border);
}

static void draw_tree(struct node *n)
{
    if (!n)
        return;

    if (n->is_view) {
        if (n->view && n->view->mapped)
            draw_view(n->view, n->view == sway.focused);
        return;
    }

    for (struct node *c = n->children; c; c = c->next)
        draw_tree(c);
}

/* ------------------------------------------------------------------ *
 *  The bar
 *
 *  Upstream this is swaybar, a separate Wayland client that the compositor
 *  starts and that draws through the same protocol as everything else.  Here
 *  it is drawn in place.  A separate bar would need layer-shell -- a surface
 *  that is not a window and does not tile -- and that is a protocol this
 *  compositor has not got, so a bar as a client would be a window taking up a
 *  tile, which is not a bar.
 * ------------------------------------------------------------------ */

static void draw_bar(void)
{
    if (!sway.config.bar)
        return;

    int h = BAR_HEIGHT;
    int y = sway.config.bar_top ? 0 : (int)sway.screen.height - h;
    int w = (int)sway.screen.width;

    fill(0, y, w, h, 0x222222);

    int at = 0;

    /* One block per workspace that has something on it, plus the current one
     * even when it is empty -- which is how a person can tell where they are. */
    for (int i = 0; i < MAX_WORKSPACES; i++) {
        struct workspace *ws = &sway.workspaces[i];
        int              here = (i == sway.current);

        if (!here && layout_view_count(ws) == 0)
            continue;

        int width = text_width(ws->name) + 16;

        fill(at, y, width, h, here ? sway.config.focused.background : 0x333333);
        draw_text(at + 8, y + 2, ws->name,
                  here ? sway.config.focused.text : 0x888888, width - 8);

        if (here)
            fill(at, y, width, 2, sway.config.focused.indicator);

        at += width;
    }

    /* The focused window's title, then the status on the right. */
    if (sway.focused) {
        const char *name = sway.focused->title[0] ? sway.focused->title
                                                  : "window";
        draw_text(at + 12, y + 2, name, 0xCCCCCC, w - at - 12 -
                  text_width(sway.status) - 16);
    }

    if (sway.status[0])
        draw_text(w - text_width(sway.status) - 8, y + 2, sway.status,
                  0xAAAAAA, text_width(sway.status) + 8);
}

/* ------------------------------------------------------------------ *
 *  A frame
 * ------------------------------------------------------------------ */

/* What the screen says when nothing is running on it.  A blank screen and a
 * broken compositor look identical, and the first thing anybody needs to know
 * is which key opens a window. */
static void draw_empty_workspace(void)
{
    char line[96];
    const char *mod = (sway.config.mod == W_MOD_LOGO) ? "Super"
                    : (sway.config.mod == W_MOD_ALT)  ? "Alt" : "the modifier";

    int cx = (int)sway.screen.width / 2;
    int cy = sway.usable_y + sway.usable_h / 2;

    wsnprintf(line, sizeof(line), "%s + Return   open %s", mod,
              sway.config.terminal);
    draw_text(cx - text_width(line) / 2, cy - 24, line, 0x666666,
              (int)sway.screen.width);

    wsnprintf(line, sizeof(line), "%s + Shift + Q   close the window", mod);
    draw_text(cx - text_width(line) / 2, cy, line, 0x555555,
              (int)sway.screen.width);

    wsnprintf(line, sizeof(line), "%s + Shift + E   leave sway", mod);
    draw_text(cx - text_width(line) / 2, cy + 24, line, 0x555555,
              (int)sway.screen.width);
}

/* The cursor.
 *
 * Drawn by the compositor in one shape, rather than composited from whatever
 * surface a client set with wl_pointer.set_cursor.  A client-drawn cursor is a
 * surface that has to be blended at the hotspot on every frame, and this
 * compositor does not blend -- so the choice is one shape that is always
 * right, or a client's shape drawn opaquely over a square of its own
 * background, which looks worse than no cursor at all.
 *
 * The shape is the arrow every desktop has: an outline in black with a white
 * fill, so it stays visible over a dark window and a light one alike.  Eleven
 * rows is enough to read at 640x400 and small enough not to cover what is
 * being pointed at.
 */
static const char *const cursor_shape[] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X....XXXXX ",
    "X..X.X     ",
    "X.X  X.X   ",
    "XX   X.X   ",
    "X     X.X  ",
    "      XXX  ",
};

static void draw_cursor(void)
{
    if (!sway.have_pointer)
        return;

    int rows = (int)(sizeof(cursor_shape) / sizeof(cursor_shape[0]));

    for (int row = 0; row < rows; row++) {
        const char *line = cursor_shape[row];

        for (int col = 0; line[col]; col++) {
            if (line[col] == ' ')
                continue;

            fill(sway.cursor_x + col, sway.cursor_y + row, 1, 1,
                 line[col] == 'X' ? 0x000000 : 0xFFFFFF);
        }
    }
}

void render_frame(void)
{
    struct workspace *ws = ws_current();

    if (!sway.back)
        return;

    canvas = wcanvas(sway.back, (int)sway.screen.width,
                     (int)sway.screen.height);

    fill(0, 0, (int)sway.screen.width, (int)sway.screen.height,
         sway.config.background);

    if (ws->fullscreen && ws->fullscreen->mapped) {
        /* Nothing else, not even the bar: that is what fullscreen means. */
        draw_buffer(ws->fullscreen->current, 0, 0, (int)sway.screen.width,
                    (int)sway.screen.height);
    } else {
        if (layout_view_count(ws) == 0)
            draw_empty_workspace();
        else
            draw_tree(ws->root);

        draw_bar();
    }

    /* Last, and over everything including a fullscreen window: a cursor
     * behind what it is pointing at is not a cursor. */
    draw_cursor();

    wblit_t blit = {
        sway.back, sway.screen.width, 0, 0,
        (int32_t)sway.screen.width, (int32_t)sway.screen.height,
    };
    wdisplayblit(&blit);

    sway.dirty = 0;
}
