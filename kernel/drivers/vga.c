/* VGA text-mode console driver.
 *
 * Text mode gives us an 80x25 grid of 16-bit cells at physical 0xB8000; the low
 * byte is the character and the high byte packs the foreground colour in the
 * low nibble and the background colour in the high nibble.  The hardware cursor
 * is moved through the CRT controller's index/data port pair.
 *
 * The driver understands the ANSI escape sequences a full-screen program
 * needs: cursor positioning, erasing, colours and cursor visibility.  Serial
 * output is not filtered, so the same byte stream drives a real terminal on
 * COM1 and this screen identically.
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

/* Escape sequence parser state. */
#define MAX_PARAMS 8

enum ansi_state {
    ANSI_NORMAL = 0,   /* ordinary text                   */
    ANSI_ESC,          /* just saw 0x1B                   */
    ANSI_CSI           /* inside "ESC [", collecting args */
};

static enum ansi_state ansi_state;
static int             ansi_params[MAX_PARAMS];
static int             ansi_param_count;
static bool            ansi_private;    /* the '?' in sequences like ESC[?25l */
static size_t          saved_row, saved_col;

/* Deferred wrap.
 *
 * Filling the last column does not move the cursor off the line; the wrap
 * happens only when another character actually arrives.  Terminals behave
 * this way, and it matters: a status bar drawn across the full width of the
 * bottom row would otherwise scroll the whole screen every time it was
 * painted. */
static bool wrap_pending;

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
    wrap_pending = false;
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

/* Show or hide the hardware cursor.  Bit 5 of CRTC register 0x0A disables it;
 * a full-screen program that redraws constantly wants it off so the cursor
 * does not flicker across the screen mid-repaint. */
static void vga_set_cursor_visible(bool visible)
{
    outb(CRTC_INDEX, 0x0A);
    uint8_t start = inb(CRTC_DATA);

    if (visible)
        start &= (uint8_t)~0x20;
    else
        start |= 0x20;

    outb(CRTC_INDEX, 0x0A);
    outb(CRTC_DATA, start);
}

/* Blank a span of cells with the current colour. */
static void vga_erase(size_t from, size_t to)
{
    for (size_t i = from; i < to && i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = vga_entry(' ', color);
}

/* ANSI colour order is black, red, green, yellow, blue, magenta, cyan, white;
 * the VGA attribute nibble uses a different order, so map between them. */
static const uint8_t ansi_to_vga[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN,
    VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LIGHT_GREY
};

static void ansi_apply_sgr(void)
{
    uint8_t fg = color & 0x0F;
    uint8_t bg = (color >> 4) & 0x0F;

    for (int i = 0; i < ansi_param_count; i++) {
        int p = ansi_params[i];

        if (p == 0) {                       /* reset          */
            fg = VGA_LIGHT_GREY;
            bg = VGA_BLACK;
        } else if (p == 1) {                /* bold => bright */
            fg |= 0x08;
        } else if (p == 22) {
            fg &= (uint8_t)~0x08;
        } else if (p == 7) {                /* reverse video  */
            uint8_t t = fg;
            fg = bg;
            bg = t;
        } else if (p >= 30 && p <= 37) {
            fg = (uint8_t)((fg & 0x08) | ansi_to_vga[p - 30]);
        } else if (p == 39) {
            fg = VGA_LIGHT_GREY;
        } else if (p >= 40 && p <= 47) {
            bg = ansi_to_vga[p - 40];
        } else if (p == 49) {
            bg = VGA_BLACK;
        } else if (p >= 90 && p <= 97) {    /* bright foreground */
            fg = (uint8_t)(ansi_to_vga[p - 90] | 0x08);
        } else if (p >= 100 && p <= 107) {
            bg = ansi_to_vga[p - 100];
        }
    }

    color = (uint8_t)(fg | (bg << 4));
}

/* Act on a complete CSI sequence whose final byte is `final`. */
static void ansi_execute(char final)
{
    /* An omitted parameter means 1 for movement and 0 for erasing, which is
     * what the defaults below encode. */
    int p0 = (ansi_param_count > 0) ? ansi_params[0] : 0;
    int p1 = (ansi_param_count > 1) ? ansi_params[1] : 0;
    size_t pos = cursor_row * VGA_WIDTH + cursor_col;

    /* Any explicit cursor movement or erase settles an owed wrap. */
    wrap_pending = false;

    switch (final) {
    case 'H':                               /* cursor position, 1-based */
    case 'f':
        cursor_row = (p0 > 0) ? (size_t)(p0 - 1) : 0;
        cursor_col = (p1 > 0) ? (size_t)(p1 - 1) : 0;
        if (cursor_row >= VGA_HEIGHT)
            cursor_row = VGA_HEIGHT - 1;
        if (cursor_col >= VGA_WIDTH)
            cursor_col = VGA_WIDTH - 1;
        break;

    case 'A': {                             /* up    */
        size_t n = (size_t)(p0 > 0 ? p0 : 1);
        cursor_row = (n > cursor_row) ? 0 : cursor_row - n;
        break;
    }
    case 'B':                               /* down  */
        cursor_row += (size_t)(p0 > 0 ? p0 : 1);
        if (cursor_row >= VGA_HEIGHT)
            cursor_row = VGA_HEIGHT - 1;
        break;
    case 'C':                               /* right */
        cursor_col += (size_t)(p0 > 0 ? p0 : 1);
        if (cursor_col >= VGA_WIDTH)
            cursor_col = VGA_WIDTH - 1;
        break;
    case 'D': {                             /* left  */
        size_t n = (size_t)(p0 > 0 ? p0 : 1);
        cursor_col = (n > cursor_col) ? 0 : cursor_col - n;
        break;
    }

    case 'J':                               /* erase in display */
        if (p0 == 0)
            vga_erase(pos, VGA_WIDTH * VGA_HEIGHT);
        else if (p0 == 1)
            vga_erase(0, pos + 1);
        else
            vga_erase(0, VGA_WIDTH * VGA_HEIGHT);
        break;

    case 'K':                               /* erase in line */
        if (p0 == 0)
            vga_erase(pos, cursor_row * VGA_WIDTH + VGA_WIDTH);
        else if (p0 == 1)
            vga_erase(cursor_row * VGA_WIDTH, pos + 1);
        else
            vga_erase(cursor_row * VGA_WIDTH,
                      cursor_row * VGA_WIDTH + VGA_WIDTH);
        break;

    case 'm':
        ansi_apply_sgr();
        break;

    case 's':                               /* save cursor    */
        saved_row = cursor_row;
        saved_col = cursor_col;
        break;
    case 'u':                               /* restore cursor */
        cursor_row = saved_row;
        cursor_col = saved_col;
        break;

    case 'h':                               /* set mode   */
        if (ansi_private && p0 == 25)
            vga_set_cursor_visible(true);
        break;
    case 'l':                               /* reset mode */
        if (ansi_private && p0 == 25)
            vga_set_cursor_visible(false);
        break;

    default:
        break;                              /* unknown: ignore it */
    }

    vga_update_cursor();
}

/* Feed one byte to the escape parser.  Returns true if the byte was consumed
 * as part of a sequence and must not be printed. */
static bool ansi_consume(char c)
{
    switch (ansi_state) {
    case ANSI_NORMAL:
        if (c == 0x1B) {
            ansi_state = ANSI_ESC;
            return true;
        }
        return false;

    case ANSI_ESC:
        if (c == '[') {
            ansi_state = ANSI_CSI;
            ansi_param_count = 0;
            ansi_private = false;
            for (int i = 0; i < MAX_PARAMS; i++)
                ansi_params[i] = 0;
        } else {
            /* Not a sequence we handle; drop it rather than printing a
             * stray character. */
            ansi_state = ANSI_NORMAL;
        }
        return true;

    case ANSI_CSI:
        if (c == '?') {
            ansi_private = true;
        } else if (c >= '0' && c <= '9') {
            if (ansi_param_count == 0)
                ansi_param_count = 1;
            int *p = &ansi_params[ansi_param_count - 1];
            *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (ansi_param_count < MAX_PARAMS)
                ansi_param_count++;
        } else {
            ansi_execute(c);
            ansi_state = ANSI_NORMAL;
        }
        return true;
    }

    return false;
}

void vga_putc(char c)
{
    if (ansi_consume(c))
        return;

    switch (c) {
    case '\n':
        wrap_pending = false;
        vga_newline();
        break;
    case '\r':
        wrap_pending = false;
        cursor_col = 0;
        break;
    case '\t':
        /* Advance to the next multiple of 8. */
        do {
            vga_putc(' ');
        } while (cursor_col % 8 != 0);
        return;                       /* vga_putc already moved the cursor */
    case '\b':
        wrap_pending = false;
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
        VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', color);
        break;
    default:
        /* A character owed a wrap from last time takes it now. */
        if (wrap_pending) {
            wrap_pending = false;
            vga_newline();
        }

        VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, color);

        if (cursor_col + 1 >= VGA_WIDTH)
            wrap_pending = true;      /* sit on the last column for now */
        else
            cursor_col++;
        break;
    }
    vga_update_cursor();
}

void vga_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        vga_putc(s[i]);
}
