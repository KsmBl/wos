/* VGA text-mode console driver.
 *
 * Text mode gives us an 80x25 grid of 16-bit cells at physical 0xB8000; the low
 * byte is the character and the high byte packs the foreground colour in the
 * low nibble and the background colour in the high nibble.  The hardware cursor
 * is moved through the CRT controller's index/data port pair.
 */

#include "vga.h"
#include "io.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)

#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

static size_t   cursor_row;
static size_t   cursor_col;
static uint8_t  color;

static inline uint16_t vga_entry(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

/* Tell the CRT controller where to draw the blinking cursor. */
static void vga_update_cursor(void)
{
    uint16_t pos = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);

    outb(CRTC_INDEX, 0x0F);
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(CRTC_INDEX, 0x0E);
    outb(CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    color = (uint8_t)(fg | (bg << 4));
}

void vga_clear(void)
{
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = vga_entry(' ', color);
    cursor_row = 0;
    cursor_col = 0;
    vga_update_cursor();
}

void vga_init(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_get_cursor(size_t *row, size_t *col)
{
    if (row) *row = cursor_row;
    if (col) *col = cursor_col;
}

/* Move every line up by one and blank the last one. */
static void vga_scroll(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];

    for (size_t x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', color);

    cursor_row = VGA_HEIGHT - 1;
}

static void vga_newline(void)
{
    cursor_col = 0;
    if (++cursor_row >= VGA_HEIGHT)
        vga_scroll();
}

void vga_putc(char c)
{
    switch (c) {
    case '\n':
        vga_newline();
        break;
    case '\r':
        cursor_col = 0;
        break;
    case '\t':
        /* Advance to the next multiple of 8. */
        do {
            vga_putc(' ');
        } while (cursor_col % 8 != 0);
        return;                       /* vga_putc already moved the cursor */
    case '\b':
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
        VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', color);
        break;
    default:
        VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, color);
        if (++cursor_col >= VGA_WIDTH)
            vga_newline();
        break;
    }
    vga_update_cursor();
}

void vga_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        vga_putc(s[i]);
}
