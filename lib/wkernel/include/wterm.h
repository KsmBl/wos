/* wterm -- a small terminal emulator you can embed in a window.
 *
 * It runs a child program with its stdin and stdout wired to pipes, interprets
 * the child's output -- text, the control characters, and the ANSI CSI
 * sequences a full-screen program uses -- into a grid of cells, and paints the
 * changed cells into a rectangle of the screen.  Keys are encoded and written
 * back to the child.  The child is told the window size through wconsize(), so
 * it lays itself out to fit.
 *
 * vim's :term uses one of these beside the editor; the `split` app uses two
 * side by side.  Anything that wants a program running in a sub-window can.
 */
#ifndef WKERNEL_WTERM_H
#define WKERNEL_WTERM_H

#include <wkernel.h>

/* A window never exceeds the largest console mode, so these bound the grid. */
#define WTERM_MAX_R W_CONSOLE_MAX_HEIGHT
#define WTERM_MAX_C W_CONSOLE_MAX_WIDTH

struct wterm {
    int  open;                 /* a child is running in this window       */
    int  pid;                  /* the child, or -1                        */
    int  in_w;                 /* write end -> child stdin                */
    int  out_r;                /* read end  <- child stdout               */

    int  rows, cols;           /* emulator geometry                       */
    int  oy, ox;               /* screen origin (1-based) of the top-left */

    char        ch[WTERM_MAX_R][WTERM_MAX_C];   /* what the child drew     */
    signed char fg[WTERM_MAX_R][WTERM_MAX_C];
    signed char bg[WTERM_MAX_R][WTERM_MAX_C];

    /* What is currently painted on the real screen, so render() can send
     * only the cells that changed. */
    char        sh_ch[WTERM_MAX_R][WTERM_MAX_C];
    signed char sh_fg[WTERM_MAX_R][WTERM_MAX_C];
    signed char sh_bg[WTERM_MAX_R][WTERM_MAX_C];
    int  shadow_valid;

    int  cy, cx;               /* emulated cursor within the grid         */
    int  cur_fg, cur_bg;       /* colour of text written now              */
    int  wrap_pending;         /* sitting on the last column              */
    int  cursor_visible;
    int  dirty;                /* grid changed since the last render      */

    /* ANSI escape parser. */
    int  state;
    int  params[8];
    int  nparam;
    int  priv;
    int  saved_cy, saved_cx;
};

/* Start a child in a fresh terminal window at the given screen origin and
 * size.  argv[0] is the program name; `path` is what to execute.
 *
 * Returns 0, or the negative error that stopped it -- -W_ENFILE when the
 * system has no pipes left, -W_ENOENT for a program that is not there, and
 * whatever else wspawn() reports. */
int  wterm_start(struct wterm *t, const char *path, char *const argv[],
                 int oy, int ox, int rows, int cols);

/* Drain whatever the child has produced into the emulator.  Returns 1 if the
 * child is still running, 0 if it has exited (the window should close). */
int  wterm_pump(struct wterm *t);

/* Move and resize the window: change its geometry and screen origin, keeping
 * whatever is already on the grid.  Forces a full repaint at the new size.
 * The child is not told -- use wsetsize() for that if it matters. */
void wterm_resize(struct wterm *t, int rows, int cols, int oy, int ox);

/* Forward one key (an ordinary character or a W_KEY_* code) to the child. */
void wterm_input(struct wterm *t, int key);

/* Repaint the changed cells of the terminal window. */
void wterm_render(struct wterm *t);

/* Where the hardware cursor belongs when this window has focus. */
void wterm_cursor(struct wterm *t, int *row, int *col);

/* Close the window: stop the child and release the pipes. */
void wterm_close(struct wterm *t);

#endif /* WKERNEL_WTERM_H */
