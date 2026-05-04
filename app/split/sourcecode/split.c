/* split -- run two terminals side by side, a tiny terminal multiplexer.
 *
 * The screen is divided into a left and a right pane, each running its own
 * shell.  The keyboard talks to one pane at a time; Ctrl-W Ctrl-W moves between
 * them, the way the split in vim's :term does, and Ctrl-W q quits.  A pane
 * whose shell exits freezes with [exited] in its status; when both have gone,
 * split leaves.
 *
 * Both panes are real, separate processes running at once -- the point of the
 * program, and a visible demonstration of preemptive multitasking: type in one
 * while a program runs in the other.
 *
 * Each terminal is a wterm from the shared library, the same emulator vim's
 * :term uses; split just arranges two of them and routes the keyboard.
 */

#include <wkernel.h>
#include <wterm.h>

/* Layout on the 80x25 console: two panes over a status row, a separator
 * column between them. */
#define CONTENT_H   (W_CONSOLE_HEIGHT - 1)          /* rows 1..24        */
#define STATUS_ROW  W_CONSOLE_HEIGHT                 /* row 25            */
#define LEFT_W      39                               /* cols 1..39        */
#define SEP_COL     40
#define RIGHT_X     41                               /* cols 41..80       */
#define RIGHT_W     (W_CONSOLE_WIDTH - RIGHT_X + 1)  /* 40                */

static struct wterm pane[2];
static int  focus;                 /* 0 = left, 1 = right       */
static int  chrome_dirty = 1;

static int start_pane(int i, int ox, int cols)
{
    char *const argv[] = { "whell", 0 };
    return wterm_start(&pane[i], "/app/whell/launch", argv,
                       1, ox, CONTENT_H, cols);
}

static void draw_field(int row, int col, int width, int fg, int bg,
                       const char *text)
{
    wgotoxy(row, col);
    wcolor(fg, bg);

    char out[W_CONSOLE_WIDTH + 1];
    int  n = 0;
    for (; text[n] && n < width; n++)
        out[n] = text[n];
    for (int i = n; i < width; i++)
        out[i] = ' ';
    out[width] = '\0';
    wprintf("%s", out);
    wcolor_reset();
}

static void draw_chrome(void)
{
    /* Separator column. */
    wcolor(W_BLUE | W_BRIGHT, W_DEFAULT);
    for (int row = 1; row <= CONTENT_H; row++) {
        wgotoxy(row, SEP_COL);
        wprintf("|");
    }
    wcolor_reset();

    /* Status labels, the focused one highlighted. */
    for (int i = 0; i < 2; i++) {
        char label[42];
        wsnprintf(label, sizeof(label), " %s: whell%s",
                  i == 0 ? "left" : "right",
                  pane[i].open ? "" : " [exited]");

        int fg = (focus == i) ? W_BLACK : W_WHITE;
        int bg = (focus == i) ? W_CYAN  : W_BLUE;
        int col   = (i == 0) ? 1 : RIGHT_X;
        int width = (i == 0) ? LEFT_W : RIGHT_W;
        draw_field(STATUS_ROW, col, width, fg, bg, label);
    }
}

/* Move focus to the other pane if it is still alive. */
static void refocus_if_dead(void)
{
    if (!pane[focus].open && pane[!focus].open) {
        focus = !focus;
        chrome_dirty = 1;
    }
}

int main(int argc, char **argv)
{
    int prev = wconsole_raw(W_CONSOLE_RAW);
    wcursor(1);
    wcolor_reset();
    wcls();

    if (start_pane(0, 1, LEFT_W) < 0 ||
        start_pane(1, RIGHT_X, RIGHT_W) < 0) {
        wconsole_raw(prev);
        wcls();
        wfprintf(W_STDERR, "split: cannot start a shell\n");
        return 1;
    }

    int ctrl_w_pending = 0;

    for (;;) {
        if (chrome_dirty) {
            draw_chrome();
            chrome_dirty = 0;
        }

        /* Keep both shells moving and drain their output. */
        for (int i = 0; i < 2; i++) {
            if (pane[i].open) {
                int alive = wterm_pump(&pane[i]);
                if (!alive)
                    chrome_dirty = 1;          /* status now says [exited] */
            }
        }

        if (!pane[0].open && !pane[1].open)
            break;                              /* both shells gone */

        refocus_if_dead();

        for (int i = 0; i < 2; i++)
            wterm_render(&pane[i]);

        /* Park the cursor in the focused pane. */
        int r, c;
        if (pane[focus].open) {
            wterm_cursor(&pane[focus], &r, &c);
            wgotoxy(r, c);
        }

        if (!wpollin(W_STDIN)) {
            wyield();                           /* let the shells run */
            continue;
        }

        int key = wgetkey();
        if (key < 0)
            break;

        /* Ctrl-W is the window-command prefix. */
        if (ctrl_w_pending) {
            ctrl_w_pending = 0;
            if (key == 'q' || key == 'Q')
                break;                          /* Ctrl-W q quits */
            if (key == 0x17 || key == 'w' || key == 'h' || key == 'l') {
                if (pane[!focus].open) {        /* only switch to a live pane */
                    focus = !focus;
                    chrome_dirty = 1;
                }
            }
            continue;
        }
        if (key == 0x17) {                      /* Ctrl-W */
            ctrl_w_pending = 1;
            continue;
        }

        if (pane[focus].open)
            wterm_input(&pane[focus], key);
    }

    wterm_close(&pane[0]);
    wterm_close(&pane[1]);

    wcolor_reset();
    wcursor(1);
    wcls();
    wconsole_raw(prev);
    return 0;
}
