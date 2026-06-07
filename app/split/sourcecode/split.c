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
 *
 * The panes run the login shell, so `chsh` decides what appears in them -- the
 * same shell a fresh login or `su` would give, rather than a second opinion
 * about which shell the user meant.
 */

#include <wkernel.h>
#include <wterm.h>

/* Layout: two panes over a status row, a separator column between them.  The
 * sizes are computed from the console size, read at startup, so split follows
 * whatever text mode is in force. */
static int con_w = W_CONSOLE_WIDTH;
static int con_h = W_CONSOLE_HEIGHT;
static int CONTENT_H;    /* pane height, above the status row */
static int STATUS_ROW;   /* the bottom status line            */
static int LEFT_W;       /* left pane width                   */
static int SEP_COL;      /* separator column                  */
static int RIGHT_X;      /* first column of the right pane    */
static int RIGHT_W;      /* right pane width                  */

static void layout_init(void)
{
    int rows = 0, cols = 0;
    if (wconsize(&rows, &cols) == 0 && rows > 0 && cols > 0) {
        con_h = rows;
        con_w = cols;
    }
    CONTENT_H  = con_h - 1;
    STATUS_ROW = con_h;
    LEFT_W     = (con_w - 1) / 2;
    SEP_COL    = LEFT_W + 1;
    RIGHT_X    = SEP_COL + 1;
    RIGHT_W    = con_w - RIGHT_X + 1;
}

static struct wterm pane[2];
static int  focus;                 /* 0 = left, 1 = right       */
static int  chrome_dirty = 1;
static int  single;                /* one pane left, filling the screen */

/* The shell the panes run, and the bare name of it for the status line. */
static char shell_path[W_SHELL_MAX + 1];
static char shell_name[W_NAME_MAX + 1] = "shell";

/* Ask what our own login shell is, and fall back to whell if the answer is
 * unusable -- an empty setting, or one naming a program that has since been
 * removed, either of which would otherwise leave two empty panes. */
static void find_shell(void)
{
    if (wgetshell(-1, shell_path, sizeof(shell_path)) < 0 || !shell_path[0])
        strlcpy(shell_path, "/app/whell/launch", sizeof(shell_path));

    wstat_t st;
    if (wstat(shell_path, &st) < 0)
        strlcpy(shell_path, "/app/whell/launch", sizeof(shell_path));

    /* /app/<name>/launch names the shell; anything else is shown by its last
     * component. */
    const char *start = shell_path;
    const char *end   = shell_path + strlen(shell_path);

    if (strncmp(shell_path, "/app/", 5) == 0) {
        start = shell_path + 5;
        const char *slash = strchr(start, '/');
        if (slash)
            end = slash;
    } else {
        const char *slash = strrchr(shell_path, '/');
        if (slash)
            start = slash + 1;
    }

    int len = (int)(end - start);
    if (len > 0 && len <= (int)sizeof(shell_name) - 1) {
        memcpy(shell_name, start, (wsize_t)len);
        shell_name[len] = '\0';
    }
}

static int start_pane(int i, int ox, int cols)
{
    char *const argv[] = { shell_name, 0 };
    return wterm_start(&pane[i], shell_path, argv, 1, ox, CONTENT_H, cols);
}

static void draw_field(int row, int col, int width, int fg, int bg,
                       const char *text)
{
    wgotoxy(row, col);
    wcolor(fg, bg);

    char out[W_CONSOLE_MAX_WIDTH + 1];
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
    /* Collapsed to one terminal: a single full-width status line, no
     * separator. */
    if (single) {
        char label[64];
        wsnprintf(label, sizeof(label), " %s    (Ctrl-W q to quit)", shell_name);
        draw_field(STATUS_ROW, 1, con_w, W_BLACK, W_CYAN, label);
        return;
    }

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
        wsnprintf(label, sizeof(label), " %s: %s%s",
                  i == 0 ? "left" : "right", shell_name,
                  pane[i].open ? "" : " [exited]");

        int fg = (focus == i) ? W_BLACK : W_WHITE;
        int bg = (focus == i) ? W_CYAN  : W_BLUE;
        int col   = (i == 0) ? 1 : RIGHT_X;
        int width = (i == 0) ? LEFT_W : RIGHT_W;
        draw_field(STATUS_ROW, col, width, fg, bg, label);
    }
}

/* When one shell exits, the survivor takes over the whole screen: the split
 * goes away and the last terminal is shown full size. */
static void collapse_to_survivor(void)
{
    int s = pane[0].open ? 0 : 1;

    wcls();
    wterm_resize(&pane[s], CONTENT_H, con_w, 1, 1);

    /* Tell the shell its new size so a program it launches next fills the
     * widened window rather than the old half. */
    wsetsize(pane[s].pid, CONTENT_H, con_w);

    focus = s;
    single = 1;
    chrome_dirty = 1;
}

int main(int argc, char **argv)
{
    layout_init();              /* size the panes to the current text mode */
    find_shell();               /* and run whatever chsh says a shell is    */

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
                    chrome_dirty = 1;
            }
        }

        if (!pane[0].open && !pane[1].open)
            break;                              /* both shells gone */

        /* One shell just exited: give the survivor the whole screen. */
        if (!single && (pane[0].open ^ pane[1].open))
            collapse_to_survivor();

        for (int i = 0; i < 2; i++)
            wterm_render(&pane[i]);

        /* Park the cursor in the focused pane. */
        int r, c;
        if (pane[focus].open) {
            wterm_cursor(&pane[focus], &r, &c);
            wgotoxy(r, c);
        }

        if (!wpollin(W_STDIN)) {
            wsleep(5);                          /* let the shells run */
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
