# The console

WOS draws on a **linear-framebuffer console** and mirrors everything to COM1,
so a program's output is identical on screen and on a serial terminal. It boots
at 80x25 and the grid can be changed at runtime with the
[`textmode`](apps.md#textmode) command or `wsetmode()` — almost any size from
40x25 up to 240x75, `160x50` included.

## Why a framebuffer, not VGA text mode

VGA text mode tops out near 80 columns, and its taller modes (80x50, 80x60)
squash an 8x8 font into a small area that the display then stretches — blocky
and ugly. So after boot the console moves onto a linear framebuffer instead:

- The card is QEMU's standard VGA in its **Bochs VBE** mode, set through the
  `0x1CE`/`0x1CF` dispi registers to a 32-bit-per-pixel resolution. No BIOS
  call, no GRUB framebuffer request.
- The kernel renders the **8x16 font itself**, glyph by glyph. The font is the
  authentic one, captured from VGA plane 2 at boot while still in text mode,
  before the switch — so no bitmap is shipped.
- Each grid is `cols*8` by `rows*16` pixels: 80x25 is 640x400, **160x50 is
  1280x800**. The display shows exactly that resolution, so text is crisp at
  any density rather than scaled.

The framebuffer aperture (the card's PCI BAR, at `0xFD000000`) is mapped into
an unused virtual hole in the low gigabyte, which lives in the kernel page
directory *every* address space shares — so the kernel can draw to the screen
whichever process is currently scheduled.

Early boot, before paging and PCI are up, still uses VGA text mode for its
first few lines; `kputc` switches to the framebuffer once it is ready. Serial
gets every byte throughout.

A full-screen program should read the size in force with `wconsize()` rather
than assume one, so it follows a mode change; `W_CONSOLE_MAX_WIDTH` and
`W_CONSOLE_MAX_HEIGHT` bound the largest grid, for sizing fixed buffers.

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

## Special keys

In raw mode the driver sends the escape sequences a terminal would, so a
program decodes arrows and friends the same way whether it is reading from
this console or from a serial terminal:

| Key | Sent as |
|---|---|
| Up / Down / Right / Left | `ESC[A` / `ESC[B` / `ESC[C` / `ESC[D` |
| Home / End | `ESC[H` / `ESC[F` |
| Page Up / Page Down | `ESC[5~` / `ESC[6~` |
| Delete | `ESC[3~` |

[`wgetkey()`](wkernel-api.md) decodes these into `W_KEY_*` constants, so a
program does not have to parse them. Special keys are only delivered in raw
mode — in canonical mode the driver is assembling a line, and an arrow key has
no meaning within one.

Escape both introduces these sequences and is a key in its own right.
`wgetkey()` tells them apart by checking whether anything follows immediately,
which is the same guess any terminal program has to make.

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

`W_CONSOLE_WIDTH` and `W_CONSOLE_HEIGHT` are the size the console *boots* in
(80x50), not a promise it stays there — the mode is changeable, so a program
that lays itself out should call `wconsize()`, which reports the size actually
in force. `wconsize()` is also what a program running in a window rather than
on the whole screen — under vim's `:term` or `split` — must use, since there it
reports the window's size rather than the console's.
