/* Framebuffer text console.
 *
 * Once the machine has paging and PCI, the console moves off VGA text mode onto
 * a linear framebuffer (QEMU's Bochs VBE display) and renders the 8x16 font
 * itself.  That buys crisp text at real resolutions -- 80x25 is 640x400,
 * 160x50 is 1280x800 -- instead of the blocky, stretched high-density text
 * modes, and lets the grid be any size the screen allows.
 */
#ifndef WOS_FBCON_H
#define WOS_FBCON_H

#include "types.h"

/* Bring up the framebuffer and switch the console onto it.  Returns false and
 * leaves VGA text mode in place if there is no usable display. */
bool fbcon_init(int cols, int rows);

/* True once the framebuffer console is the active output. */
bool fbcon_active(void);

/* Write one character, interpreting the same ANSI escapes the VGA console
 * does. */
void fbcon_putc(char c);

/* Resize the grid to `cols` x `rows`, changing the framebuffer resolution to
 * match (cols*8 by rows*16).  Returns 0, or -1 if it will not fit. */
int  fbcon_set_mode(int cols, int rows);

/* Report the current grid size. */
void fbcon_size(int *cols, int *rows);

#endif /* WOS_FBCON_H */
