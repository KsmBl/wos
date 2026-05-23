/* Framebuffer text console. See fbcon.h. */

#include "fbcon.h"
#include "vga.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "io.h"

/* ------------------------------------------------------------------ *
 *  The display: QEMU's Bochs VBE, programmed through the dispi ports
 * ------------------------------------------------------------------ */

#define DISPI_INDEX  0x01CE
#define DISPI_DATA   0x01CF

#define DISPI_XRES        1
#define DISPI_YRES        2
#define DISPI_BPP         3
#define DISPI_ENABLE      4
#define DISPI_VIRT_WIDTH  6
#define DISPI_VIRT_HEIGHT 7
#define DISPI_X_OFFSET    8
#define DISPI_Y_OFFSET    9

#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

#define FONT_W 8
#define FONT_H 16

/* Where the framebuffer aperture is mapped.  It has to live in the low
 * gigabyte: that is the one page directory every address space shares, so a
 * mapping there is reachable no matter which process's CR3 is loaded when the
 * kernel prints.  Mapping it higher would leave it invisible inside every
 * process.
 *
 * Being inside the identity map means the window shadows whatever RAM sits at
 * the same physical address on a machine with more than 768 MiB, so
 * map_aperture() reserves that range from the frame allocator.  QEMU with the
 * usual 256 MiB has no RAM up there at all and the reservation costs nothing.
 */
/* Declared in fbcon.h, where the frame allocator can see them too: it has to
 * keep its bitmap and the kernel heap out of the window for the same reason
 * uefi/stub.c keeps everything it hands over below it.  Anything left inside
 * would be shadowed by the framebuffer and read back as pixels. */
#define FB_VIRT   FBCON_APERTURE
#define FB_WINDOW FBCON_APERTURE_SIZE

/* The largest grid we will build, bounding the fixed backing store.  240x75 at
 * 8x16 is 1920x1200, which fits QEMU's default 16 MiB of video memory. */
#define MAX_COLS 240
#define MAX_ROWS 75

static volatile uint32_t *fb;        /* the linear framebuffer          */
static uint64_t fb_phys;
static uint64_t fb_window;           /* bytes of it mapped, and reserved */
static uint32_t fb_stride;           /* pixels per row                  */
static int      px_w, px_h;          /* framebuffer size in pixels      */

static int cols, rows;               /* grid size in characters         */
static bool active;

/* Backing store, so a cell can be repainted (to erase the cursor) and the
 * screen can scroll. */
static char    cell_ch[MAX_ROWS][MAX_COLS];
static uint8_t cell_at[MAX_ROWS][MAX_COLS];   /* VGA attribute byte      */

/* What is currently on the glass, which after a scroll is not what the backing
 * store says any more.  Keeping it lets the repaint touch only the cells that
 * actually differ, instead of writing every pixel of the screen back to a
 * device that is slow to write and slower to read. */
static char    shown_ch[MAX_ROWS][MAX_COLS];
static uint8_t shown_at[MAX_ROWS][MAX_COLS];

static int     cur_row, cur_col;
static uint8_t attr = 0x07;          /* light grey on black             */
static bool    wrap_pending;
static bool    cursor_visible = true;
static int     drawn_row = -1, drawn_col = -1;

/* True when the bootloader handed us the framebuffer.  Then the resolution is
 * whatever it set and cannot be changed from here: there is no hardware
 * interface behind it, only the mode the firmware left the card in. */
static bool    fixed_mode;

/* The 16 VGA colours as 0x00RRGGBB. */
static const uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static void dispi_write(uint16_t index, uint16_t value)
{
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

static void dispi_set_res(int w, int h)
{
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)w);
    dispi_write(DISPI_YRES, (uint16_t)h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)w);
    dispi_write(DISPI_VIRT_HEIGHT, (uint16_t)h);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    px_w = w;
    px_h = h;
    fb_stride = (uint32_t)w;          /* 32bpp, so pixels == virt width */
}

/* ------------------------------------------------------------------ *
 *  Pixel and glyph drawing
 * ------------------------------------------------------------------ */

/* Push out whatever is sitting in the write-combining buffers.
 *
 * Combined writes reach the card when a buffer fills or something evicts it,
 * which for a console means the last few characters typed could sit in the CPU
 * and not on the screen until the next output came along.  One fence at the end
 * of a character costs nothing next to the five hundred stores that drew it. */
static inline void flush_writes(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

static void fill_rect(int x, int y, int w, int h, uint32_t rgb)
{
    for (int yy = y; yy < y + h && yy < px_h; yy++) {
        volatile uint32_t *line = fb + (uint32_t)yy * fb_stride + x;
        for (int xx = 0; xx < w && x + xx < px_w; xx++)
            line[xx] = rgb;
    }
}

static void draw_cell(int row, int col)
{
    char c = cell_ch[row][col];
    uint8_t a = cell_at[row][col];
    uint32_t fg = palette[a & 0x0F];
    uint32_t bg = palette[(a >> 4) & 0x0F];

    const uint8_t *glyph = vga_font16() + (uint8_t)c * FONT_H;
    int px = col * FONT_W;
    int py = row * FONT_H;

    for (int gy = 0; gy < FONT_H; gy++) {
        volatile uint32_t *line = fb + (uint32_t)(py + gy) * fb_stride + px;
        uint8_t bits = glyph[gy];
        for (int gx = 0; gx < FONT_W; gx++)
            line[gx] = (bits & (0x80 >> gx)) ? fg : bg;
    }

    shown_ch[row][col] = c;
    shown_at[row][col] = a;
}

/* Bring the glass back into agreement with the backing store. */
static void redraw_changed(void)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (cell_ch[r][c] != shown_ch[r][c] ||
                cell_at[r][c] != shown_at[r][c])
                draw_cell(r, c);
}

static void put_cell(int row, int col, char c, uint8_t a)
{
    cell_ch[row][col] = c;
    cell_at[row][col] = a;
    draw_cell(row, col);
}

/* Draw or erase the block cursor at the current position. */
static void erase_cursor(void)
{
    if (drawn_row >= 0)
        draw_cell(drawn_row, drawn_col);
    drawn_row = drawn_col = -1;
}

static void draw_cursor(void)
{
    if (!cursor_visible)
        return;
    int px = cur_col * FONT_W;
    int py = cur_row * FONT_H;
    /* An underline on the last two scan lines, in the foreground colour. */
    fill_rect(px, py + FONT_H - 2, FONT_W, 2, palette[cell_at[cur_row][cur_col] & 0x0F]);
    drawn_row = cur_row;
    drawn_col = cur_col;
}

static void clear_all(void)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            cell_ch[r][c] = shown_ch[r][c] = ' ';
            cell_at[r][c] = shown_at[r][c] = attr;
        }
    /* One sweep of the whole screen in the background colour, which is what
     * every one of those cells now looks like. */
    fill_rect(0, 0, px_w, px_h, palette[(attr >> 4) & 0x0F]);
    cur_row = cur_col = 0;
    wrap_pending = false;
    drawn_row = drawn_col = -1;
}

/* Scroll by one row.
 *
 * The pixels are not moved.  Moving them means reading the framebuffer back,
 * and a framebuffer is memory that is fast to write and desperately slow to
 * read -- writes are posted and combined, reads are round trips to a device
 * across PCIe, one per access, with nothing to hide the latency.  Copying a
 * 1280x800 screen up a line that way took seconds on real hardware and repainted
 * visibly while it did.
 *
 * So the move happens in the backing store, which is ordinary cached memory,
 * and the glass is brought back into agreement with it afterwards -- a cell at
 * a time, skipping the ones that already show what they should.  Scrolling text
 * leaves most of the screen's right-hand side blank in both pictures, so a
 * good half of the work usually disappears there. */
static void scroll(void)
{
    for (int r = 1; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            cell_ch[r - 1][c] = cell_ch[r][c];
            cell_at[r - 1][c] = cell_at[r][c];
        }
    for (int c = 0; c < cols; c++) {
        cell_ch[rows - 1][c] = ' ';
        cell_at[rows - 1][c] = attr;
    }

    redraw_changed();
}

static void newline(void)
{
    cur_col = 0;
    if (++cur_row >= rows) {
        cur_row = rows - 1;
        scroll();
    }
}

/* ------------------------------------------------------------------ *
 *  ANSI escape parsing (mirrors the VGA console)
 * ------------------------------------------------------------------ */

#define MAX_PARAMS 8
enum { A_NORMAL = 0, A_ESC, A_CSI };

static int  a_state;
static int  a_params[MAX_PARAMS];
static int  a_count;
static bool a_priv;
static int  saved_row, saved_col;

/* ANSI colour order -> VGA attribute nibble. */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void apply_sgr(void)
{
    uint8_t fg = attr & 0x0F, bg = (attr >> 4) & 0x0F;

    for (int i = 0; i < a_count; i++) {
        int p = a_params[i];
        if (p == 0)               { fg = 7; bg = 0; }
        else if (p == 1)          { fg |= 0x08; }
        else if (p == 22)         { fg &= ~0x08; }
        else if (p == 7)          { uint8_t t = fg; fg = bg; bg = t; }
        else if (p >= 30 && p <= 37)  fg = (fg & 0x08) | ansi_to_vga[p - 30];
        else if (p == 39)         fg = 7;
        else if (p >= 40 && p <= 47)  bg = ansi_to_vga[p - 40];
        else if (p == 49)         bg = 0;
        else if (p >= 90 && p <= 97)  fg = 0x08 | ansi_to_vga[p - 90];
        else if (p >= 100 && p <= 107) bg = ansi_to_vga[p - 100];
    }
    attr = (uint8_t)(fg | (bg << 4));
}

static void erase_cells(int from, int to)
{
    for (int i = from; i < to && i < rows * cols; i++)
        put_cell(i / cols, i % cols, ' ', attr);
}

static void csi_execute(char final)
{
    int p0 = (a_count > 0) ? a_params[0] : 0;
    int p1 = (a_count > 1) ? a_params[1] : 0;
    int pos = cur_row * cols + cur_col;

    wrap_pending = false;

    switch (final) {
    case 'H': case 'f':
        cur_row = (p0 > 0) ? p0 - 1 : 0;
        cur_col = (p1 > 0) ? p1 - 1 : 0;
        if (cur_row >= rows) cur_row = rows - 1;
        if (cur_col >= cols) cur_col = cols - 1;
        break;
    case 'A': cur_row -= (p0 > 0) ? p0 : 1; if (cur_row < 0) cur_row = 0; break;
    case 'B': cur_row += (p0 > 0) ? p0 : 1; if (cur_row >= rows) cur_row = rows - 1; break;
    case 'C': cur_col += (p0 > 0) ? p0 : 1; if (cur_col >= cols) cur_col = cols - 1; break;
    case 'D': cur_col -= (p0 > 0) ? p0 : 1; if (cur_col < 0) cur_col = 0; break;
    case 'J':
        if (p0 == 0)      erase_cells(pos, rows * cols);
        else if (p0 == 1) erase_cells(0, pos + 1);
        else              erase_cells(0, rows * cols);
        break;
    case 'K':
        if (p0 == 0)      erase_cells(pos, cur_row * cols + cols);
        else if (p0 == 1) erase_cells(cur_row * cols, pos + 1);
        else              erase_cells(cur_row * cols, cur_row * cols + cols);
        break;
    case 'm': apply_sgr(); break;
    case 's': saved_row = cur_row; saved_col = cur_col; break;
    case 'u': cur_row = saved_row; cur_col = saved_col; break;
    case 'h': if (a_priv && p0 == 25) cursor_visible = true; break;
    case 'l': if (a_priv && p0 == 25) cursor_visible = false; break;
    default: break;
    }
}

static bool consume_escape(char c)
{
    switch (a_state) {
    case A_NORMAL:
        if (c == 0x1B) { a_state = A_ESC; return true; }
        return false;
    case A_ESC:
        if (c == '[') {
            a_state = A_CSI;
            a_count = 0;
            a_priv = false;
            for (int i = 0; i < MAX_PARAMS; i++) a_params[i] = 0;
        } else {
            a_state = A_NORMAL;
        }
        return true;
    case A_CSI:
        if (c == '?') a_priv = true;
        else if (c >= '0' && c <= '9') {
            if (a_count == 0) a_count = 1;
            a_params[a_count - 1] = a_params[a_count - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (a_count < MAX_PARAMS) a_count++;
        } else {
            csi_execute(c);
            a_state = A_NORMAL;
        }
        return true;
    }
    return false;
}

void fbcon_putc(char c)
{
    if (!active)
        return;

    erase_cursor();

    if (consume_escape(c)) {
        draw_cursor();
        flush_writes();
        return;
    }

    switch (c) {
    case '\n': wrap_pending = false; newline(); break;
    case '\r': wrap_pending = false; cur_col = 0; break;
    case '\t':
        do { fbcon_putc(' '); } while (cur_col % 8 != 0);
        return;
    case '\b':
        wrap_pending = false;
        if (cur_col > 0) cur_col--;
        else if (cur_row > 0) { cur_row--; cur_col = cols - 1; }
        put_cell(cur_row, cur_col, ' ', attr);
        break;
    default:
        if ((unsigned char)c < 32) break;
        if (wrap_pending) { wrap_pending = false; newline(); }
        put_cell(cur_row, cur_col, c, attr);
        if (cur_col + 1 >= cols) wrap_pending = true;
        else cur_col++;
        break;
    }

    draw_cursor();
    flush_writes();
}

/* ------------------------------------------------------------------ *
 *  Setup
 * ------------------------------------------------------------------ */

bool fbcon_active(void) { return active; }

void fbcon_size(int *c, int *r)
{
    if (c) *c = cols;
    if (r) *r = rows;
}

int fbcon_set_mode(int c, int r)
{
    /* On a framebuffer whose size is fixed, asking for nothing in particular
     * means filling the screen: there is no resolution to change to match a
     * grid, so the grid is made to match the resolution. */
    if (fixed_mode && c <= 0 && r <= 0) {
        c = px_w / FONT_W;
        r = px_h / FONT_H;
    }

    if (c < 40)  c = 40;
    if (r < 25)  r = 25;
    if (c > MAX_COLS) c = MAX_COLS;
    if (r > MAX_ROWS) r = MAX_ROWS;

    if (fixed_mode) {
        /* Never more than the resolution the bootloader chose can hold. */
        if (c > px_w / FONT_W) c = px_w / FONT_W;
        if (r > px_h / FONT_H) r = px_h / FONT_H;
    }

    cols = c;
    rows = r;
    if (!fixed_mode)
        dispi_set_res(c * FONT_W, r * FONT_H);
    attr = 0x07;
    clear_all();
    return 0;
}

/* Map `bytes` of the framebuffer into the shared low-gigabyte window, rounded
 * up to whole 2 MiB pages.
 *
 * Every byte mapped costs a byte of RAM, because the window sits inside the
 * identity map and hides the memory at the same physical address, which then
 * has to be kept from the frame allocator.  So the window is sized to the
 * display: a fixed mode gets exactly what it needs, and only the one card whose
 * resolution can change at runtime gets the full 16 MiB that the largest mode
 * would want.
 *
 * This runs before paging_init() on a UEFI boot -- the console has to be up
 * before the rest of the kernel starts printing, because there is no text mode
 * behind it to catch the output -- so it goes through the boot page tables
 * when the real ones are not built yet. */
static void map_aperture(uint64_t phys, uint64_t bytes)
{
    if (bytes > FB_WINDOW)
        bytes = FB_WINDOW;
    fb_window = (bytes + 0x1FFFFF) & ~0x1FFFFFUL;

    /* Write-combining, or the console is unusable on real hardware: an uncached
     * store to a PCIe device is a round trip the CPU waits for, and a screen is
     * a million of them.  On the emulators the framebuffer is host memory and
     * the difference is invisible, which is why this went unnoticed for so
     * long.  A CPU without PAT keeps the uncached mapping and still works. */
    uint64_t flags = PTE_WRITE;
    if (paging_enable_write_combining())
        flags |= PTE_WC;

    for (uint64_t off = 0; off < fb_window; off += 0x200000) {
        if (paging_ready())
            paging_map_huge(FB_VIRT + off, phys + off, flags);
        else
            paging_map_huge_early(FB_VIRT + off, phys + off, flags);
    }

    fb_phys = phys;
    fb = (volatile uint32_t *)FB_VIRT;
}

/* The window overwrites the identity mapping of the physical addresses it
 * covers, so those frames must never also be handed out as ordinary RAM.  The
 * allocator does not exist when the mapping is made on the UEFI path, so this
 * is a separate step the caller makes once it does. */
void fbcon_reserve_aperture(void)
{
    if (active)
        pmm_reserve_range(FB_VIRT, FB_VIRT + fb_window);
}

bool fbcon_init(int c, int r)
{
    if (!vga_font16_valid())
        return false;

    pci_device_t dev = pci_find(0x1234, 0x1111);   /* QEMU standard VGA */
    if (!dev.found)
        return false;

    uint64_t phys = dev.bar0 & ~0xFUL;
    if (!phys)
        return false;

    /* The whole window: this card's resolution changes at runtime, and the
     * largest mode the console will set has to be mapped before it is asked
     * for. */
    map_aperture(phys, FB_WINDOW);

    active = true;
    fixed_mode = false;
    fbcon_set_mode(c, r);
    return true;
}

bool fbcon_init_boot(const struct multiboot_info *mbi, int c, int r)
{
    /* Nothing to draw with.  This is why the Multiboot header asks for a text
     * mode rather than a linear one: the font has to survive the handover. */
    if (!vga_font16_valid())
        return false;

    if (!(mbi->flags & MB_FLAG_FRAMEBUFFER))
        return false;

    /* Type 1 is a linear RGB framebuffer; type 2 means the bootloader left the
     * card in EGA text mode, which vga.c already drives. */
    if (mbi->framebuffer_type != MB_FRAMEBUFFER_RGB)
        return false;

    /* Only 32 bits per pixel: every glyph here is drawn as uint32_t writes,
     * and a 24- or 16-bit mode would need its own pixel path.  GRUB is asked
     * for 32bpp in grub.cfg and in the Multiboot header, so this is the mode
     * we get in practice. */
    if (mbi->framebuffer_bpp != 32 || !mbi->framebuffer_addr)
        return false;

    uint64_t addr   = mbi->framebuffer_addr;
    uint32_t pitch  = mbi->framebuffer_pitch;
    uint32_t width  = mbi->framebuffer_width;
    uint32_t height = mbi->framebuffer_height;

    if (width < FONT_W || height < FONT_H || pitch < width * 4)
        return false;

    /* Only what the aperture window can cover may be drawn on; a taller mode
     * is used down to the rows that fit rather than faulting past the end. */
    if ((uint64_t)pitch * height > FB_WINDOW)
        height = (uint32_t)(FB_WINDOW / pitch);

    /* This resolution is the one the firmware set and the only one there will
     * be, so the window need be no larger than the picture. */
    map_aperture(addr, (uint64_t)pitch * height);

    px_w      = (int)width;
    px_h      = (int)height;
    fb_stride = pitch / 4;            /* 32bpp, so 4 bytes per pixel */

    active     = true;
    fixed_mode = true;
    fbcon_set_mode(c, r);
    return true;
}

void fbcon_resolution(int *w, int *h)
{
    if (w) *w = px_w;
    if (h) *h = px_h;
}
