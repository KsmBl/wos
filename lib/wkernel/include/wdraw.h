/* wdraw -- drawing into a rectangle of pixels.
 *
 * Every graphical program on this machine has the same problem and had the
 * same answer to it: a buffer of 32-bit pixels, a rectangle fill that clips, a
 * glyph from the kernel's 8x16 font, a line of text, a line of text that has
 * to fit, and a one-pixel border.  Three programs had grown their own copy of
 * those six functions -- the compositor, the file manager and the settings
 * window -- and the copies had begun to disagree: the cursor was scaled in one
 * and not in the other, and the same "draw this text, cut it if it does not
 * fit" was cut at the front in one place and at the back in another.
 *
 * So they live here, once.  Nothing in this header knows about windows or
 * about Wayland: it draws into a `wcanvas_t`, which is somebody else's memory
 * and their business to present -- the compositor's back buffer, a client's
 * shared-memory frame, or anything else that is pixels in a row.
 *
 * Everything clips.  A window can be told to be any size at all, including
 * smaller than what is being drawn into it, and a program that wrote outside
 * its buffer would corrupt a shared-memory pool rather than merely look wrong.
 */
#ifndef WKERNEL_WDRAW_H
#define WKERNEL_WDRAW_H

#include <wkernel.h>

/* The font is the kernel's, and it is the only one: 8 pixels wide, 16 tall. */
#define WDRAW_CELL_W W_CELL_WIDTH
#define WDRAW_CELL_H W_CELL_HEIGHT

/* Somewhere to draw.  `stride` is the distance from one row to the next in
 * pixels, which is the width for a buffer of its own and something larger for
 * a rectangle inside a bigger one. */
typedef struct {
    uint32_t *pixels;
    int       width;
    int       height;
    int       stride;
} wcanvas_t;

/* The usual case: a buffer that is exactly as wide as it is. */
static inline wcanvas_t wcanvas(uint32_t *pixels, int width, int height)
{
    wcanvas_t c = { pixels, width, height, width };
    return c;
}

/* A rectangle of one colour, clipped to the canvas. */
void wdraw_fill(const wcanvas_t *c, int x, int y, int w, int h,
                uint32_t colour);

/* An outline `thickness` pixels wide, drawn just inside the rectangle. */
void wdraw_frame(const wcanvas_t *c, int x, int y, int w, int h, int thickness,
                 uint32_t colour);

static inline void wdraw_border(const wcanvas_t *c, int x, int y, int w, int h,
                                uint32_t colour)
{
    wdraw_frame(c, x, y, w, h, 1, colour);
}

/* One glyph, leaving the background as it was -- which is what lets a label
 * cross a selection bar without carrying a rectangle of its own colour. */
void wdraw_char(const wcanvas_t *c, int x, int y, char ch, uint32_t fg);

/* A line of text.  Returns where it ended, so a caller can carry on drawing
 * after it. */
int wdraw_text(const wcanvas_t *c, int x, int y, const char *s, uint32_t fg);

/* The same, stopping after `max_px` pixels rather than running over whatever
 * comes next.  Nothing is added to say it was cut: use wdraw_text_fit() when
 * the reader needs to know. */
int wdraw_text_max(const wcanvas_t *c, int x, int y, const char *s,
                   uint32_t fg, int max_px);

/* Text that has to stay inside something, ending in an ellipsis when it does
 * not: a name that was cut should look cut rather than look like a shorter
 * name that happens to exist. */
void wdraw_text_fit(const wcanvas_t *c, int x, int y, const char *s,
                    uint32_t fg, int room_px);

/* The same, keeping the *end* of the string instead of the beginning, for
 * paths -- where the last component says more than the first. */
void wdraw_text_fit_tail(const wcanvas_t *c, int x, int y, const char *s,
                         uint32_t fg, int room_px);

/* How wide a string will be, in pixels. */
int wdraw_text_width(const char *s);

/* ------------------------------------------------------------------ *
 *  The pointer
 *
 *  The arrow the compositor draws.  It is here rather than in the compositor
 *  because it is drawn in two places -- on the screen, and in the settings
 *  window that previews it -- and a preview of a shape defined somewhere else
 *  is a preview that stops being one.
 * ------------------------------------------------------------------ */

#define WDRAW_CURSOR_W 11
#define WDRAW_CURSOR_H 15

/* Drawn at `size` times its size: one cell of the shape becomes a square of
 * `size` pixels.  Whole multiples only -- scaling a bitmap by anything else
 * has to decide what half a pixel of edge looks like, and there is nothing
 * here to blend it with.
 *
 * The point of the arrow is at (x, y) whatever the size, so a bigger cursor
 * still points at the same pixel.  The outline is always black: that is what
 * makes a white arrow visible on a white window and a black one visible on a
 * dark background. */
void wdraw_cursor(const wcanvas_t *c, int x, int y, int size, uint32_t fill);

#endif /* WKERNEL_WDRAW_H */
