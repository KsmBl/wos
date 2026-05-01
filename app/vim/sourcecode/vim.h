/* vim -- a modal text editor for WOS.
 *
 * Shared declarations. The buffer is an array of lines, each its own
 * allocation, which keeps line insertion and deletion to a pointer shuffle.
 */
#ifndef WOS_VIM_H
#define WOS_VIM_H

#include <wkernel.h>

#define MAX_LINES 4096

/* The bottom row is the status and command line, so the text occupies
 * everything above it. */
#define TEXT_ROWS (W_CONSOLE_HEIGHT - 1)

enum mode {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_COMMAND
};

/* The buffer. */
extern char *lines[MAX_LINES];
extern int   line_count;
extern char  filename[W_PATH_MAX + 1];
extern int   modified;

int  line_set(int at, const char *text);
int  line_insert(int at, const char *text);
void line_remove(int at);
void buffer_new(void);
void buffer_free(void);
int  buffer_load(const char *path);
int  buffer_save(const char *path);

/* ---------------------------------------------------------------- *
 *  The :term window -- a terminal emulator running a child program
 * ---------------------------------------------------------------- */

/* A terminal window never exceeds the console, so these bound its grid. */
#define TERM_MAX_R W_CONSOLE_HEIGHT
#define TERM_MAX_C W_CONSOLE_WIDTH

/* One cell of the emulated screen: a character and its colours. */
struct term {
    int  open;                 /* a child is running in this window       */
    int  pid;                  /* the child, or -1                        */
    int  in_w;                 /* write end -> child stdin                */
    int  out_r;                /* read end  <- child stdout               */

    int  rows, cols;           /* emulator geometry                       */
    int  oy, ox;               /* screen origin (1-based) of the top-left */

    char        ch[TERM_MAX_R][TERM_MAX_C];   /* what the child drew      */
    signed char fg[TERM_MAX_R][TERM_MAX_C];
    signed char bg[TERM_MAX_R][TERM_MAX_C];

    /* What is currently painted on the real screen, so render() can send
     * only the cells that changed. */
    char        sh_ch[TERM_MAX_R][TERM_MAX_C];
    signed char sh_fg[TERM_MAX_R][TERM_MAX_C];
    signed char sh_bg[TERM_MAX_R][TERM_MAX_C];
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
 * size.  argv[0] is the program name; `path` is what to execute.  Returns 0 or
 * a negative error. */
int  term_start(struct term *t, const char *path, char *const argv[],
                int oy, int ox, int rows, int cols);

/* Drain whatever the child has produced into the emulator.  Returns 1 if the
 * child is still running, 0 if it has exited (the window should close). */
int  term_pump(struct term *t);

/* Forward one key (an ordinary character or a W_KEY_* code) to the child. */
void term_input(struct term *t, int key);

/* Repaint the changed cells of the terminal window. */
void term_render(struct term *t);

/* Where the hardware cursor belongs when this window has focus. */
void term_cursor(struct term *t, int *row, int *col);

/* Close the window: stop the child and release the pipes. */
void term_close(struct term *t);

#endif /* WOS_VIM_H */
