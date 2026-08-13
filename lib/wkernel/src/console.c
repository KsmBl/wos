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

    /* ESC O introduces the short forms, which are only F1 to F4. */
    if (c == 'O') {
        if (wread(W_STDIN, &c, 1) != 1)
            return W_KEY_ESCAPE;

        switch (c) {
        case 'P': return W_KEY_F1;
        case 'Q': return W_KEY_F2;
        case 'R': return W_KEY_F3;
        case 'S': return W_KEY_F4;
        default:  return W_KEY_ESCAPE;
        }
    }

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

    /* The numeric forms end with '~', e.g. ESC[3~ for Delete.  One of them
     * ends with 't' instead: `ESC[8;rows;cols t` is how a terminal reports the
     * size of its window, and it is what arrives here when the window changed
     * size while this program was waiting for a key.  The numbers are read and
     * thrown away -- wconsize() is the authority, and it is already right by
     * the time this is delivered. */
    if (c >= '0' && c <= '9') {
        int value = c - '0';
        for (;;) {
            if (wread(W_STDIN, &c, 1) != 1)
                break;
            if (c == '~')
                break;
            if (c == 't')
                return W_KEY_RESIZE;
            if (c >= '0' && c <= '9')
                value = value * 10 + (c - '0');
            else if (c != ';')
                break;
        }

        switch (value) {
        case 3:  return W_KEY_DELETE;
        case 5:  return W_KEY_PGUP;
        case 6:  return W_KEY_PGDN;
        /* F5 up. The numbering skips 16, and 22, a gap the VT220 left and
         * nothing since has filled in. */
        case 15: return W_KEY_F5;
        case 17: return W_KEY_F6;
        case 18: return W_KEY_F7;
        case 19: return W_KEY_F8;
        case 20: return W_KEY_F9;
        case 21: return W_KEY_F10;
        case 23: return W_KEY_F11;
        case 24: return W_KEY_F12;
        default: break;
        }
    }

    return W_KEY_ESCAPE;
}

void wresize_reports(int on)
{
    /* A private mode, like the one that hides the cursor.  The emulator this
     * program is running in reads it and starts sending #W_KEY_RESIZE when
     * the window changes size; on the real console nothing is listening and
     * nothing is printed, since an unknown private mode is ignored there.
     *
     * Opt-in because the report arrives in this program's own input: a shell
     * that had not asked would echo the escape sequence, and a program that
     * quits on Escape would quit. */
    wputs(on ? "\033[?2048h" : "\033[?2048l");
}

int wgetpass(const char *prompt, char *buf, wsize_t size)
{
    if (size == 0)
        return 0;

    wputs(prompt);

    int previous = wconsole_raw(W_CONSOLE_RAW);
    wsize_t len = 0;

    for (;;) {
        char c;
        int  n = wread(W_STDIN, &c, 1);

        if (n < 0) {
            wconsole_raw(previous);
            return n;
        }
        if (n == 0)
            continue;

        if (c == '\n' || c == '\r')
            break;

        if (c == 0x03) {                 /* Ctrl+C abandons the entry */
            len = 0;
            break;
        }

        if (c == '\b' || c == 0x7F) {
            if (len > 0)
                len--;
            continue;
        }

        /* Nothing is echoed: not even a placeholder, since the length of a
         * password is itself worth not showing. */
        if (c >= 32 && c < 127 && len + 1 < size)
            buf[len++] = c;
    }

    buf[len] = '\0';

    wconsole_raw(previous);
    wputs("\n");

    return (int)len;
}
