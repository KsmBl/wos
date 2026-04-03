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

    return base[c & 7];
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
