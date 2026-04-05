/* Full-screen console control.
 *
 * All of this is ANSI escape sequences.  The VGA console parses them, and so
 * does any real terminal on the serial port, so a full-screen program renders
 * correctly on both without knowing which it is talking to.
 */

#include "wkernel.h"

/* Map a W_* colour onto its ANSI code. The two orderings agree except that
 * ANSI calls 3 "yellow" and puts bright colours in the 90s. */
static int ansi_foreground(int c)
{
    static const int base[8] = { 30, 31, 32, 33, 34, 35, 36, 37 };

    if (c == W_DEFAULT)
        return 39;

    int bright = (c & W_BRIGHT) != 0;
    int index  = c & 7;

    return bright ? base[index] + 60 : base[index];
}

static int ansi_background(int c)
{
    static const int base[8] = { 40, 41, 42, 43, 44, 45, 46, 47 };

    if (c == W_DEFAULT)
        return 49;

    /* Bright backgrounds are the 100s. Note that VGA text mode uses the top
     * attribute bit for blink unless that is turned off, so a bright
     * background may blink on the console even though it is correct here. */
    int bright = (c & W_BRIGHT) != 0;
    int index  = c & 7;

    return bright ? base[index] + 60 : base[index];
}

void wcls(void)
{
    wputs("\033[2J\033[H");
}

void wgotoxy(int row, int col)
{
    if (row < 1)
        row = 1;
    if (col < 1)
        col = 1;

    wprintf("\033[%d;%dH", row, col);
}

void wcolor(int fg, int bg)
{
    wprintf("\033[%d;%dm", ansi_foreground(fg), ansi_background(bg));
}

void wcolor_reset(void)
{
    wputs("\033[0m");
}

void wclear_line(void)
{
    wputs("\033[K");
}

void wcursor(int visible)
{
    wputs(visible ? "\033[?25h" : "\033[?25l");
}

int wgetkey(void)
{
    char c;

    if (wread(W_STDIN, &c, 1) != 1)
        return -1;

    if (c != 0x1B)
        return (unsigned char)c;

    /* Escape starts a sequence, but it is also a key. Nothing waiting behind
     * it means the user pressed Escape itself -- the same guess any terminal
     * program has to make. */
    if (!wpollin(W_STDIN))
        return W_KEY_ESCAPE;

    if (wread(W_STDIN, &c, 1) != 1)
        return W_KEY_ESCAPE;
    if (c != '[')
        return W_KEY_ESCAPE;

    if (wread(W_STDIN, &c, 1) != 1)
        return W_KEY_ESCAPE;

    switch (c) {
    case 'A': return W_KEY_UP;
    case 'B': return W_KEY_DOWN;
    case 'C': return W_KEY_RIGHT;
    case 'D': return W_KEY_LEFT;
    case 'H': return W_KEY_HOME;
    case 'F': return W_KEY_END;
    default:  break;
    }

    /* The numeric forms end with '~', e.g. ESC[3~ for Delete. */
    if (c >= '0' && c <= '9') {
        int value = c - '0';
        for (;;) {
            if (wread(W_STDIN, &c, 1) != 1)
                break;
            if (c == '~')
                break;
            if (c >= '0' && c <= '9')
                value = value * 10 + (c - '0');
            else
                break;
        }

        switch (value) {
        case 3: return W_KEY_DELETE;
        case 5: return W_KEY_PGUP;
        case 6: return W_KEY_PGDN;
        default: break;
        }
    }

    return W_KEY_ESCAPE;
}
