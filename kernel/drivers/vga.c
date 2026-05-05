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

/* The character grid is chosen at runtime by vga_set_mode(); these name the
 * current size, and cap the largest mode the driver offers. */
static int vga_w = 80;
static int vga_h = 50;
#define VGA_WIDTH  ((size_t)vga_w)
#define VGA_HEIGHT ((size_t)vga_h)
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)

#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

/* The sequencer and graphics-controller port pairs, used only to reach the
 * font memory in plane 2. */
#define SEQ_INDEX  0x3C4
#define SEQ_DATA   0x3C5
#define GC_INDEX   0x3CE
#define GC_DATA    0x3CF

/* Character-generator memory: 256 glyphs, 32 bytes apart, at 0xA0000 once the
 * plane is mapped there. */
#define FONT_MEMORY ((volatile uint8_t *)0xA0000)

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

/* Map plane 2 (the font memory) flat at 0xA0000 so the glyphs can be read and
 * written, saving the registers the mapping disturbs.  This is the standard
 * VGA font-access dance; the values come straight from the hardware manual. */
static void font_access_begin(uint8_t saved[5])
{
    outb(SEQ_INDEX, 2); saved[0] = inb(SEQ_DATA);
    outb(SEQ_INDEX, 4); saved[1] = inb(SEQ_DATA);
    outb(GC_INDEX, 4);  saved[2] = inb(GC_DATA);
    outb(GC_INDEX, 5);  saved[3] = inb(GC_DATA);
    outb(GC_INDEX, 6);  saved[4] = inb(GC_DATA);

    outb(SEQ_INDEX, 2); outb(SEQ_DATA, 0x04);   /* write to plane 2      */
    outb(SEQ_INDEX, 4); outb(SEQ_DATA, 0x06);   /* sequential addressing */
    outb(GC_INDEX, 4);  outb(GC_DATA, 0x02);    /* read from plane 2     */
    outb(GC_INDEX, 5);  outb(GC_DATA, 0x00);    /* flat, no odd/even     */
    outb(GC_INDEX, 6);  outb(GC_DATA, 0x04);    /* map at 0xA0000        */
}

static void font_access_end(const uint8_t saved[5])
{
    outb(SEQ_INDEX, 2); outb(SEQ_DATA, saved[0]);
    outb(SEQ_INDEX, 4); outb(SEQ_DATA, saved[1]);
    outb(GC_INDEX, 4);  outb(GC_DATA, saved[2]);
    outb(GC_INDEX, 5);  outb(GC_DATA, saved[3]);
    outb(GC_INDEX, 6);  outb(GC_DATA, saved[4]);
}

/* Two fonts, captured and derived once at boot: the 8x16 GRUB leaves loaded,
 * and an 8x8 made from it (each row the OR of two, so thin strokes survive the
 * squash).  Keeping both means the driver can switch between the tall and
 * short character cells without shipping a font bitmap. */
static uint8_t font16[256 * 16];
static uint8_t font8[256 * 8];

static void vga_read_font16(void)
{
    uint8_t saved[5];
    font_access_begin(saved);
    for (int g = 0; g < 256; g++)
        for (int r = 0; r < 16; r++)
            font16[g * 16 + r] = FONT_MEMORY[g * 32 + r];
    font_access_end(saved);

    for (int g = 0; g < 256; g++)
        for (int r = 0; r < 8; r++)
            font8[g * 8 + r] = (uint8_t)(font16[g * 16 + 2 * r] |
                                         font16[g * 16 + 2 * r + 1]);
}

static void vga_load_font(const uint8_t *font, int height)
{
    uint8_t saved[5];
    font_access_begin(saved);
    for (int g = 0; g < 256; g++)
        for (int r = 0; r < height; r++)
            FONT_MEMORY[g * 32 + r] = font[g * height + r];
    font_access_end(saved);
}

/* A full register dump for a text mode: misc, then the sequencer, CRTC,
 * graphics and attribute registers in order.  Programming a whole set is far
 * more reliable than nudging individual registers between modes. */
#define REG_MISC 0
#define REG_SEQ  1              /* 5 registers  */
#define REG_CRTC 6             /* 25 registers */
#define REG_GC   31            /* 9 registers  */
#define REG_AC   40            /* 21 registers */
#define REG_TOTAL 61

/* 80x25: the standard colour text mode (mode 3h), 720x400, 8x16 cell. */
static const uint8_t regs_80x25[REG_TOTAL] = {
    0x67,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};

/* 40x25: mode 1h.  The sequencer's clock/2 bit halves the columns, and the
 * CRTC horizontal timing is halved to match. */
static const uint8_t regs_40x25[REG_TOTAL] = {
    0x67,
    0x03, 0x08, 0x03, 0x00, 0x02,
    0x2D, 0x27, 0x28, 0x90, 0x2B, 0xA0, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0xA0,
    0x9C, 0x0E, 0x8F, 0x14, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};

/* 80x30 / 80x60: 720x480, so thirty 8x16 rows or sixty 8x8 rows. */
static const uint8_t regs_80x30[REG_TOTAL] = {
    0xE3,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0x0B, 0x3E,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0xEA, 0x0C, 0xDF, 0x28, 0x1F, 0xE7, 0x04, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};

static void vga_write_regs(const uint8_t *r)
{
    outb(0x3C2, r[REG_MISC]);                       /* miscellaneous output */

    for (int i = 0; i < 5; i++) {                   /* sequencer */
        outb(SEQ_INDEX, (uint8_t)i);
        outb(SEQ_DATA, r[REG_SEQ + i]);
    }

    /* Unlock CRTC registers 0-7 by clearing the protect bit. */
    outb(CRTC_INDEX, 0x11);
    outb(CRTC_DATA, (uint8_t)(inb(CRTC_DATA) & 0x7F));

    for (int i = 0; i < 25; i++) {                  /* CRTC */
        outb(CRTC_INDEX, (uint8_t)i);
        outb(CRTC_DATA, r[REG_CRTC + i]);
    }

    for (int i = 0; i < 9; i++) {                   /* graphics controller */
        outb(GC_INDEX, (uint8_t)i);
        outb(GC_DATA, r[REG_GC + i]);
    }

    for (int i = 0; i < 21; i++) {                  /* attribute controller */
        (void)inb(0x3DA);                           /* reset the flip-flop */
        outb(0x3C0, (uint8_t)i);
        outb(0x3C0, r[REG_AC + i]);
    }

    (void)inb(0x3DA);
    outb(0x3C0, 0x20);                              /* re-enable video output */
}

static void vga_set_cursor_shape(int height)
{
    /* A thin cursor on the last two scan lines of the cell. */
    outb(CRTC_INDEX, 0x0A); outb(CRTC_DATA, (uint8_t)(height - 2));
    outb(CRTC_INDEX, 0x0B); outb(CRTC_DATA, (uint8_t)(height - 1));
}

int vga_set_mode(int cols, int rows)
{
    const uint8_t *base;
    int font_h;

    if (cols == 80 && rows == 25)      { base = regs_80x25; font_h = 16; }
    else if (cols == 80 && rows == 50) { base = regs_80x25; font_h = 8;  }
    else if (cols == 40 && rows == 25) { base = regs_40x25; font_h = 16; }
    else if (cols == 40 && rows == 50) { base = regs_40x25; font_h = 8;  }
    else if (cols == 80 && rows == 30) { base = regs_80x30; font_h = 16; }
    else if (cols == 80 && rows == 60) { base = regs_80x30; font_h = 8;  }
    else
        return -1;

    vga_write_regs(base);

    if (font_h == 8) {
        /* Halve the character cell: Maximum Scan Line to 7, preserving the top
         * three bits the register dump set. */
        outb(CRTC_INDEX, 0x09);
        outb(CRTC_DATA, (uint8_t)((inb(CRTC_DATA) & 0xE0) | 0x07));
        vga_load_font(font8, 8);
    } else {
        vga_load_font(font16, 16);
    }
    vga_set_cursor_shape(font_h);

    vga_w = cols;
    vga_h = rows;
    vga_clear();
    return 0;
}

void vga_size(int *cols, int *rows)
{
    if (cols) *cols = vga_w;
    if (rows) *rows = vga_h;
}

void vga_init(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_read_font16();          /* capture GRUB's font before changing modes */
    vga_set_mode(80, 50);       /* the default: twice the rows of plain text */
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
