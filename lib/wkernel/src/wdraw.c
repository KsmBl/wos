/* Drawing into a rectangle of pixels.  See wdraw.h for what this is for. */

#include <wdraw.h>

void wdraw_fill(const wcanvas_t *c, int x, int y, int w, int h,
                uint32_t colour)
{
    if (!c || !c->pixels)
        return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width)  w = c->width - x;
    if (y + h > c->height) h = c->height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++) {
        uint32_t *at = c->pixels + (uint64_t)(y + row) * c->stride + x;

        for (int col = 0; col < w; col++)
            at[col] = colour;
    }
}

void wdraw_frame(const wcanvas_t *c, int x, int y, int w, int h, int thickness,
                 uint32_t colour)
{
    if (thickness <= 0 || w <= 0 || h <= 0)
        return;

    wdraw_fill(c, x, y, w, thickness, colour);                  /* top    */
    wdraw_fill(c, x, y + h - thickness, w, thickness, colour);  /* bottom */
    wdraw_fill(c, x, y, thickness, h, colour);                  /* left   */
    wdraw_fill(c, x + w - thickness, y, thickness, h, colour);  /* right  */
}

void wdraw_char(const wcanvas_t *c, int x, int y, char ch, uint32_t fg)
{
    if (!c || !c->pixels)
        return;

    const unsigned char *glyph = wglyph8x16((unsigned char)ch);

    for (int row = 0; row < WDRAW_CELL_H; row++) {
        unsigned char bits = glyph[row];
        int           at_y = y + row;

        if (!bits || at_y < 0 || at_y >= c->height)
            continue;

        uint32_t *at = c->pixels + (uint64_t)at_y * c->stride;

        for (int col = 0; col < WDRAW_CELL_W; col++) {
            int at_x = x + col;

            if ((bits & (0x80 >> col)) && at_x >= 0 && at_x < c->width)
                at[at_x] = fg;
        }
    }
}

int wdraw_text(const wcanvas_t *c, int x, int y, const char *s, uint32_t fg)
{
    for (; s && *s; s++, x += WDRAW_CELL_W)
        wdraw_char(c, x, y, *s, fg);

    return x;
}

int wdraw_text_max(const wcanvas_t *c, int x, int y, const char *s,
                   uint32_t fg, int max_px)
{
    int at = x;

    for (; s && *s; s++) {
        if (at + WDRAW_CELL_W > x + max_px)
            break;

        wdraw_char(c, at, y, *s, fg);
        at += WDRAW_CELL_W;
    }

    return at;
}

int wdraw_text_width(const char *s)
{
    return s ? (int)strlen(s) * WDRAW_CELL_W : 0;
}

void wdraw_text_fit(const wcanvas_t *c, int x, int y, const char *s,
                    uint32_t fg, int room_px)
{
    int room = room_px / WDRAW_CELL_W;
    int len  = s ? (int)strlen(s) : 0;

    if (room <= 0 || !len)
        return;

    if (len <= room) {
        wdraw_text(c, x, y, s, fg);
        return;
    }

    /* Too narrow for a word and an ellipsis both: dots alone at least say
     * that something was left out. */
    if (room <= 3) {
        for (int i = 0; i < room; i++)
            wdraw_char(c, x + i * WDRAW_CELL_W, y, '.', fg);
        return;
    }

    for (int i = 0; i < room - 3; i++)
        wdraw_char(c, x + i * WDRAW_CELL_W, y, s[i], fg);
    for (int i = room - 3; i < room; i++)
        wdraw_char(c, x + i * WDRAW_CELL_W, y, '.', fg);
}

void wdraw_text_fit_tail(const wcanvas_t *c, int x, int y, const char *s,
                         uint32_t fg, int room_px)
{
    int room = room_px / WDRAW_CELL_W;
    int len  = s ? (int)strlen(s) : 0;

    if (room <= 0 || !len)
        return;

    if (len <= room) {
        wdraw_text(c, x, y, s, fg);
        return;
    }

    if (room <= 3) {
        for (int i = 0; i < room; i++)
            wdraw_char(c, x + i * WDRAW_CELL_W, y, '.', fg);
        return;
    }

    wdraw_text(c, x, y, "...", fg);
    wdraw_text(c, x + 3 * WDRAW_CELL_W, y, s + len - (room - 3), fg);
}

/* 'X' is the outline, '.' is the inside, a space is nothing at all.  Eleven
 * by fifteen: enough to be seen on a 640x400 screen and small enough not to
 * cover what it is pointing at. */
static const char *const cursor_shape[WDRAW_CURSOR_H] = {
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

void wdraw_cursor(const wcanvas_t *c, int x, int y, int size, uint32_t fill)
{
    if (size < 1)
        size = 1;

    for (int row = 0; row < WDRAW_CURSOR_H; row++) {
        const char *line = cursor_shape[row];

        for (int col = 0; line[col]; col++) {
            if (line[col] == ' ')
                continue;

            wdraw_fill(c, x + col * size, y + row * size, size, size,
                       line[col] == 'X' ? 0x000000 : fill);
        }
    }
}
