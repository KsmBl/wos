/* VGA text-mode console (80x25 at 0xB8000). */
#ifndef WOS_VGA_H
#define WOS_VGA_H

#include "types.h"

/* Hardware colour codes for the text-mode attribute byte. */
enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE
};

void vga_init(void);
void vga_putc(char c);
void vga_write(const char *s, size_t len);
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_get_cursor(size_t *row, size_t *col);

/* Switch the text mode to `cols` x `rows` characters.  Returns 0, or -1 if
 * that combination is not one of the supported modes. */
int  vga_set_mode(int cols, int rows);

/* Report the current text-mode size. */
void vga_size(int *cols, int *rows);

/* The 8x16 font captured from the card, 256 glyphs of 16 bytes. */
const uint8_t *vga_font16(void);

#endif /* WOS_VGA_H */
