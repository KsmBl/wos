# The console

WOS draws on an 80x25 VGA text console and mirrors everything to COM1, so a
program's output is identical on screen and on a serial terminal.

Two things make full-screen programs possible: **raw input mode**, so a program
sees individual keystrokes, and **ANSI escape sequences**, so it can position
the cursor and paint in colour.

## Input modes

| Mode | Behaviour |
|---|---|
| canonical (default) | the kernel echoes as you type and handles backspace; a `wread()` on descriptor 0 returns a whole line once Enter is pressed |
| raw | every keystroke is readable at once, nothing is echoed, and Ctrl+letter arrives as its control code |

Switch with [`wconsole_raw()`](wkernel-api.md). Changing mode discards anything
typed but not yet submitted.

The mode belongs to the console, not to a process, so a program that switches
to raw must switch back before it exits or spawns something that reads input.
`whell` does this around each line it reads.

## Escape sequences

The VGA driver parses these. Anything it does not recognise is dropped rather
than printed, so an unsupported sequence never leaves debris on the screen.

| Sequence | Effect |
|---|---|
| `ESC[<row>;<col>H` | move the cursor; 1-based, top-left is `1;1` |
| `ESC[<n>A` `B` `C` `D` | move up / down / right / left by `n` (default 1) |
| `ESC[0J` | erase from the cursor to the end of the screen |
| `ESC[1J` | erase from the start of the screen to the cursor |
| `ESC[2J` | erase the whole screen |
| `ESC[0K` | erase from the cursor to the end of the line |
| `ESC[1K` | erase from the start of the line to the cursor |
| `ESC[2K` | erase the whole line |
| `ESC[<n>m` | set colours and attributes, see below |
| `ESC[s` / `ESC[u` | save / restore the cursor position |
| `ESC[?25h` / `ESC[?25l` | show / hide the hardware cursor |

### Colours

| Parameter | Meaning |
|---|---|
| `0` | reset to the default colours |
| `1` | bold, which shows as a bright foreground |
| `7` | reverse video: swap foreground and background |
| `30`–`37` | foreground: black, red, green, yellow, blue, magenta, cyan, white |
| `90`–`97` | the same eight as bright foregrounds |
| `40`–`47` | background, in the same order |
| `39` / `49` | default foreground / background |

Several parameters can be combined: `ESC[1;32;40m` is bright green on black.

ANSI's colour order and the VGA attribute nibble's order differ — ANSI's third
colour is yellow where VGA's is brown — so the driver maps between them rather
than passing the number through.

## Using it from a program

`wkernel` wraps the common operations so applications need not write escapes by
hand:

```c
#include <wkernel.h>

int main(void)
{
    wconsole_raw(W_CONSOLE_RAW);
    wcursor(0);                       /* stop the cursor flickering */
    wcls();

    wgotoxy(1, 1);
    wcolor(W_GREEN | W_BRIGHT, W_BLACK);
    wprintf("top left, in bright green");
    wcolor_reset();

    char c;
    wread(W_STDIN, &c, 1);            /* wait for any key */

    wcursor(1);
    wconsole_raw(W_CONSOLE_CANONICAL);
    return 0;
}
```

A program that repaints continuously should hide the cursor first, or it
flickers across the screen during every repaint. It must also restore the
cursor and the input mode before exiting, since both outlive the process.

`W_CONSOLE_WIDTH` and `W_CONSOLE_HEIGHT` give the console size. There is no way
to ask the hardware, so they are fixed at 80 and 25.
