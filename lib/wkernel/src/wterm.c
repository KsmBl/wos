/* wterm -- a small terminal emulator, and the plumbing that runs a child
 * program inside a window.  See wterm.h for the model.
 *
 * The host spawns the child (a shell, asciiquarium, ...) with its stdin and
 * stdout wired to pipes through wspawn_io().  Whatever the child writes is fed
 * here byte by byte and interpreted -- printable text, the handful of control
 * characters, and the ANSI CSI sequences a full-screen program uses -- into a
 * grid of cells.  wterm_render() then blits the changed cells into the
 * window's corner of the real screen.  Keys travel the other way:
 * wterm_input() encodes them and writes them to the child's stdin.
 *
 * The child believes it owns a screen of exactly `rows` by `cols`, which is
 * what wconsize() tells it, so it lays itself out to fit the window.
 */

#include "wterm.h"

#define ESC 0x1B

enum { ST_NORMAL = 0, ST_ESC, ST_CSI };

/* ANSI SGR colour codes 30-37 (and 40-47) are in the same order as the W_*
 * colours, so the code maps straight onto the value with no lookup table. */

static void term_reset_grid(struct wterm *t)
{
    for (int y = 0; y < t->rows; y++)
        for (int x = 0; x < t->cols; x++) {
            t->ch[y][x] = ' ';
            t->fg[y][x] = (signed char)t->cur_fg;
            t->bg[y][x] = (signed char)t->cur_bg;
        }
}

static void term_scroll(struct wterm *t)
{
    for (int y = 1; y < t->rows; y++)
        for (int x = 0; x < t->cols; x++) {
            t->ch[y - 1][x] = t->ch[y][x];
            t->fg[y - 1][x] = t->fg[y][x];
            t->bg[y - 1][x] = t->bg[y][x];
        }

    int last = t->rows - 1;
    for (int x = 0; x < t->cols; x++) {
        t->ch[last][x] = ' ';
        t->fg[last][x] = (signed char)t->cur_fg;
        t->bg[last][x] = (signed char)t->cur_bg;
    }
}

static void term_newline(struct wterm *t)
{
    t->cx = 0;
    if (++t->cy >= t->rows) {
        t->cy = t->rows - 1;
        term_scroll(t);
    }
}

static void term_putc(struct wterm *t, char c)
{
    if (t->wrap_pending) {
        t->wrap_pending = 0;
        term_newline(t);
    }

    if (t->cy < 0) t->cy = 0;
    if (t->cx < 0) t->cx = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
    if (t->cx >= t->cols) t->cx = t->cols - 1;

    t->ch[t->cy][t->cx] = c;
    t->fg[t->cy][t->cx] = (signed char)t->cur_fg;
    t->bg[t->cy][t->cx] = (signed char)t->cur_bg;

    if (t->cx + 1 >= t->cols)
        t->wrap_pending = 1;         /* defer, like a real terminal */
    else
        t->cx++;
}

static void term_erase(struct wterm *t, int y0, int x0, int y1, int x1)
{
    for (int y = y0; y <= y1 && y < t->rows; y++) {
        int xs = (y == y0) ? x0 : 0;
        int xe = (y == y1) ? x1 : t->cols - 1;
        for (int x = xs; x <= xe && x < t->cols; x++) {
            if (x < 0) continue;
            t->ch[y][x] = ' ';
            t->fg[y][x] = (signed char)t->cur_fg;
            t->bg[y][x] = (signed char)t->cur_bg;
        }
    }
}

static void term_sgr(struct wterm *t)
{
    int n = t->nparam ? t->nparam : 1;   /* a lone "\033[m" means reset */

    for (int i = 0; i < n; i++) {
        int p = t->params[i];

        if (p == 0) {
            t->cur_fg = W_WHITE;
            t->cur_bg = W_BLACK;
        } else if (p == 1) {
            t->cur_fg |= W_BRIGHT;
        } else if (p == 22) {
            t->cur_fg &= ~W_BRIGHT;
        } else if (p == 7) {
            int tmp = t->cur_fg & 7;
            t->cur_fg = (t->cur_fg & W_BRIGHT) | (t->cur_bg & 7);
            t->cur_bg = tmp;
        } else if (p >= 30 && p <= 37) {
            t->cur_fg = (t->cur_fg & W_BRIGHT) | (p - 30);
        } else if (p == 39) {
            t->cur_fg = (t->cur_fg & W_BRIGHT) | W_WHITE;
        } else if (p >= 40 && p <= 47) {
            t->cur_bg = p - 40;
        } else if (p == 49) {
            t->cur_bg = W_BLACK;
        } else if (p >= 90 && p <= 97) {
            t->cur_fg = (p - 90) | W_BRIGHT;
        } else if (p >= 100 && p <= 107) {
            t->cur_bg = p - 100;
        }
    }
}

static void term_csi(struct wterm *t, char final)
{
    int p0 = (t->nparam > 0) ? t->params[0] : 0;
    int p1 = (t->nparam > 1) ? t->params[1] : 0;

    t->wrap_pending = 0;

    switch (final) {
    case 'H': case 'f':                       /* cursor position, 1-based */
        t->cy = (p0 > 0) ? p0 - 1 : 0;
        t->cx = (p1 > 0) ? p1 - 1 : 0;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        break;
    case 'A': t->cy -= (p0 > 0) ? p0 : 1; if (t->cy < 0) t->cy = 0; break;
    case 'B': t->cy += (p0 > 0) ? p0 : 1; if (t->cy >= t->rows) t->cy = t->rows - 1; break;
    case 'C': t->cx += (p0 > 0) ? p0 : 1; if (t->cx >= t->cols) t->cx = t->cols - 1; break;
    case 'D': t->cx -= (p0 > 0) ? p0 : 1; if (t->cx < 0) t->cx = 0; break;

    case 'J':                                 /* erase in display */
        if (p0 == 0)      term_erase(t, t->cy, t->cx, t->rows - 1, t->cols - 1);
        else if (p0 == 1) term_erase(t, 0, 0, t->cy, t->cx);
        else              term_erase(t, 0, 0, t->rows - 1, t->cols - 1);
        break;

    case 'K':                                 /* erase in line */
        if (p0 == 0)      term_erase(t, t->cy, t->cx, t->cy, t->cols - 1);
        else if (p0 == 1) term_erase(t, t->cy, 0, t->cy, t->cx);
        else              term_erase(t, t->cy, 0, t->cy, t->cols - 1);
        break;

    case 'm': term_sgr(t); break;

    case 's': t->saved_cy = t->cy; t->saved_cx = t->cx; break;
    case 'u': t->cy = t->saved_cy; t->cx = t->saved_cx; break;

    case 'h': if (t->priv && p0 == 25) t->cursor_visible = 1; break;
    case 'l': if (t->priv && p0 == 25) t->cursor_visible = 0; break;

    default: break;
    }
}

static void term_feed(struct wterm *t, char c)
{
    switch (t->state) {
    case ST_NORMAL:
        t->dirty = 1;
        if (c == ESC) { t->state = ST_ESC; return; }
        switch (c) {
        case '\n': t->wrap_pending = 0; term_newline(t); break;
        case '\r': t->wrap_pending = 0; t->cx = 0; break;
        case '\b':
            t->wrap_pending = 0;
            if (t->cx > 0) t->cx--;
            break;
        case '\t':
            do { term_putc(t, ' '); } while (t->cx % 8 != 0 && t->cx < t->cols - 1);
            break;
        default:
            if ((unsigned char)c >= 32)
                term_putc(t, c);
            break;
        }
        return;

    case ST_ESC:
        if (c == '[') {
            t->state  = ST_CSI;
            t->nparam = 0;
            t->priv   = 0;
            for (int i = 0; i < 8; i++)
                t->params[i] = 0;
        } else {
            t->state = ST_NORMAL;    /* a sequence we do not handle */
        }
        return;

    case ST_CSI:
        if (c == '?') {
            t->priv = 1;
        } else if (c >= '0' && c <= '9') {
            if (t->nparam == 0) t->nparam = 1;
            if (t->nparam <= 8)
                t->params[t->nparam - 1] = t->params[t->nparam - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (t->nparam < 8) t->nparam++;
        } else {
            term_csi(t, c);
            t->state = ST_NORMAL;
        }
        return;
    }
}

/* ------------------------------------------------------------------ *
 *  Public interface
 * ------------------------------------------------------------------ */

int wterm_start(struct wterm *t, const char *path, char *const argv[],
               int oy, int ox, int rows, int cols)
{
    memset(t, 0, sizeof(*t));

    if (rows > WTERM_MAX_R) rows = WTERM_MAX_R;
    if (cols > WTERM_MAX_C) cols = WTERM_MAX_C;

    t->rows = rows;
    t->cols = cols;
    t->oy   = oy;
    t->ox   = ox;
    t->cur_fg = W_WHITE;
    t->cur_bg = W_BLACK;
    t->cursor_visible = 1;
    t->pid = -1;
    term_reset_grid(t);

    /* The reason is passed back rather than flattened into -1.  A window that
     * opens and then cannot start a shell is a confusing thing to be told
     * nothing about, and "out of pipes" and "no such program" want different
     * responses from whoever reads it. */
    int in[2], out[2];
    int r = wpipe(in);
    if (r < 0)
        return r;

    r = wpipe(out);
    if (r < 0) {
        wclose(in[0]); wclose(in[1]);
        return r;
    }

    wspawnio_t io = { in[0], out[1], rows, cols };
    int pid = wspawn_io(path, argv, &io);

    /* Whatever happens next, our copies of the child's ends must go: only then
     * does out[0] reach end of file when the child exits. */
    wclose(in[0]);
    wclose(out[1]);

    if (pid < 0) {
        wclose(in[1]);
        wclose(out[0]);
        return pid;
    }

    t->pid   = pid;
    t->in_w  = in[1];
    t->out_r = out[0];
    t->open  = 1;
    return 0;
}

int wterm_pump(struct wterm *t)
{
    if (!t->open)
        return 0;

    char buf[512];
    while (wpollin(t->out_r)) {
        int n = wread(t->out_r, buf, sizeof(buf));
        if (n <= 0) {
            /* End of file: the child has closed its output and exited. */
            wterm_close(t);
            return 0;
        }
        for (int i = 0; i < n; i++)
            term_feed(t, buf[i]);
    }
    return 1;
}

void wterm_input(struct wterm *t, int key)
{
    if (!t->open)
        return;

    const char *seq = 0;
    char one[2];

    switch (key) {
    case W_KEY_UP:     seq = "\033[A";  break;
    case W_KEY_DOWN:   seq = "\033[B";  break;
    case W_KEY_RIGHT:  seq = "\033[C";  break;
    case W_KEY_LEFT:   seq = "\033[D";  break;
    case W_KEY_HOME:   seq = "\033[H";  break;
    case W_KEY_END:    seq = "\033[F";  break;
    case W_KEY_DELETE: seq = "\033[3~"; break;
    case W_KEY_PGUP:   seq = "\033[5~"; break;
    case W_KEY_PGDN:   seq = "\033[6~"; break;
    case W_KEY_ESCAPE: one[0] = 0x1B; one[1] = 0; seq = one; break;
    case '\n': case '\r': one[0] = '\n'; one[1] = 0; seq = one; break;
    default:
        if (key >= 0 && key < 0x100) {
            one[0] = (char)key;
            one[1] = 0;
            seq = one;
        }
        break;
    }

    if (seq && *seq)
        wwrite(t->in_w, seq, (int)strlen(seq));
}

void wterm_render(struct wterm *t)
{
    if (!t->open)
        return;
    if (t->shadow_valid && !t->dirty)
        return;                          /* nothing new since the last paint */

    int last_fg = -100, last_bg = -100;

    for (int y = 0; y < t->rows; y++) {
        int run = 0;   /* is the cursor already at the next cell to write? */
        for (int x = 0; x < t->cols; x++) {
            char c  = t->ch[y][x];
            int  fg = t->fg[y][x];
            int  bg = t->bg[y][x];

            if (t->shadow_valid &&
                t->sh_ch[y][x] == c &&
                t->sh_fg[y][x] == (signed char)fg &&
                t->sh_bg[y][x] == (signed char)bg) {
                run = 0;
                continue;
            }

            if (!run)
                wgotoxy(t->oy + y, t->ox + x);

            if (fg != last_fg || bg != last_bg) {
                wcolor(fg, bg);
                last_fg = fg;
                last_bg = bg;
            }

            wwrite(W_STDOUT, &c, 1);

            t->sh_ch[y][x] = c;
            t->sh_fg[y][x] = (signed char)fg;
            t->sh_bg[y][x] = (signed char)bg;
            run = 1;
        }
    }

    t->shadow_valid = 1;
    t->dirty = 0;
    wcolor_reset();
}

void wterm_resize(struct wterm *t, int rows, int cols, int oy, int ox)
{
    if (rows > WTERM_MAX_R) rows = WTERM_MAX_R;
    if (cols > WTERM_MAX_C) cols = WTERM_MAX_C;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;

    /* The grid arrays are always full size, so growing the window just exposes
     * columns and rows that were already there (blank); existing content in
     * the top-left keeps its place. */
    t->rows = rows;
    t->cols = cols;
    t->oy   = oy;
    t->ox   = ox;

    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cx >= cols) t->cx = cols - 1;

    t->shadow_valid = 0;        /* geometry moved: repaint every cell */
    t->dirty = 1;
}

void wterm_cursor(struct wterm *t, int *row, int *col)
{
    int cy = t->cy, cx = t->cx;
    if (cy >= t->rows) cy = t->rows - 1;
    if (cx >= t->cols) cx = t->cols - 1;
    *row = t->oy + cy;
    *col = t->ox + cx;
}

void wterm_close(struct wterm *t)
{
    if (!t->open)
        return;

    /* Closing our ends tells the child, whichever way it is waiting: its
     * stdin reaches end of file and its stdout write breaks, so a program
     * that watches for either -- the shell, asciiquarium -- stops on its own.
     * Then reap it so it does not linger as a zombie. */
    wclose(t->in_w);
    wclose(t->out_r);

    if (t->pid > 0) {
        int status = 0;
        wwait(t->pid, &status);
    }

    t->open = 0;
    t->pid  = -1;
}
