/* fastfetch -- show system information beside an ASCII logo.
 *
 * This is a WOS-native program in the spirit of fastfetch, not a build of the
 * upstream one: that reads /proc, /sys, DRM and PCI databases through a full
 * libc, none of which exists here.  Everything below comes from the wkernel
 * API instead, plus CPUID, which ring 3 is allowed to execute directly.
 */

#include <wkernel.h>

#define LOGO_LINES 8
#define LOGO_WIDTH 32

/* Drawn in ASCII rather than the block characters a real fetch tool uses:
 * the VGA font is code page 437 and the serial port usually reaches a UTF-8
 * terminal, and no single byte sequence looks right on both. */
static const char *logo[LOGO_LINES] = {
    "                                ",
    "  __        __   ___    ____    ",
    "  \\ \\      / /  / _ \\  / ___|   ",
    "   \\ \\ /\\ / /  | | | | \\___ \\   ",
    "    \\ V  V /   | |_| |  ___) |  ",
    "     \\_/\\_/     \\___/  |____/   ",
    "                                ",
    "                                ",
};

/* Read a CPUID leaf.  This is an unprivileged instruction, so a ring 3
 * program can identify the processor without asking the kernel. */
static void cpuid(unsigned leaf, unsigned *a, unsigned *b,
                  unsigned *c, unsigned *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static void append_reg(char *dst, int *at, unsigned reg)
{
    for (int i = 0; i < 4; i++)
        dst[(*at)++] = (char)((reg >> (i * 8)) & 0xFF);
}

/* The processor's brand string if it has one, otherwise its vendor id. */
static void cpu_name(char *out, wsize_t size)
{
    unsigned a, b, c, d;
    char     buf[64];
    int      at = 0;

    cpuid(0x80000000u, &a, &b, &c, &d);

    if (a >= 0x80000004u) {
        for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid(leaf, &a, &b, &c, &d);
            append_reg(buf, &at, a);
            append_reg(buf, &at, b);
            append_reg(buf, &at, c);
            append_reg(buf, &at, d);
        }
        buf[at] = '\0';

        /* Brand strings are padded with leading spaces on many parts. */
        char *start = buf;
        while (*start == ' ')
            start++;
        strlcpy(out, start, size);
        return;
    }

    cpuid(0, &a, &b, &c, &d);
    append_reg(buf, &at, b);
    append_reg(buf, &at, d);
    append_reg(buf, &at, c);
    buf[at] = '\0';
    strlcpy(out, buf, size);
}

static void format_uptime(char *out, wsize_t size)
{
    unsigned total = wuptime_ms() / 1000u;
    unsigned hours = total / 3600u;
    unsigned mins  = (total % 3600u) / 60u;
    unsigned secs  = total % 60u;

    if (hours)
        wsnprintf(out, size, "%u hour%s, %u min%s", hours,
                  hours == 1 ? "" : "s", mins, mins == 1 ? "" : "s");
    else if (mins)
        wsnprintf(out, size, "%u min%s, %u sec%s", mins,
                  mins == 1 ? "" : "s", secs, secs == 1 ? "" : "s");
    else
        wsnprintf(out, size, "%u second%s", secs, secs == 1 ? "" : "s");
}

/* One "Key: value" line, with the key in the accent colour. */
static void info_line(const char *key, const char *value)
{
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("%s", key);
    wcolor_reset();
    wprintf(": %s", value);
}

int main(int argc, char **argv)
{
    wmeminfo_t  mem;
    wdiskinfo_t disk;
    wprocmem_t  procs[32];

    wmeminfo(&mem);
    wdiskinfo(&disk);
    int process_count = wproclist(procs, 32);

    char cpu[64];
    char uptime[48];
    char line[96];

    cpu_name(cpu, sizeof(cpu));
    format_uptime(uptime, sizeof(uptime));

    /* Build the right-hand column first, so the logo and the text can be
     * printed a line at a time side by side. */
    const char *keys[12];
    char        values[12][64];
    int         rows = 0;

    keys[rows] = "OS";
    strlcpy(values[rows++], "WOS 0.1 (i386)", 64);

    keys[rows] = "Kernel";
    strlcpy(values[rows++], "wos-0.1", 64);

    keys[rows] = "Uptime";
    strlcpy(values[rows++], uptime, 64);

    keys[rows] = "Shell";
    strlcpy(values[rows++], "whell", 64);

    keys[rows] = "Terminal";
    wsnprintf(values[rows++], 64, "VGA %dx%d",
              W_CONSOLE_WIDTH, W_CONSOLE_HEIGHT);

    keys[rows] = "CPU";
    strlcpy(values[rows++], cpu, 64);

    keys[rows] = "Memory";
    wsnprintf(values[rows++], 64, "%s / %s",
              whuman(mem.used_bytes), whuman(mem.total_bytes));

    keys[rows] = "Disk (/)";
    wsnprintf(values[rows++], 64, "%s / %s (wfs)",
              whuman(disk.used_bytes), whuman(disk.total_bytes));

    keys[rows] = "Processes";
    wsnprintf(values[rows++], 64, "%d", process_count);

    wprintf("\n");

    /* Header: the "user@host" line every fetch tool opens with. */
    wprintf("%*s", LOGO_WIDTH, "");
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("wos");
    wcolor_reset();
    wprintf("@");
    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("wos\n");
    wcolor_reset();

    wprintf("%*s", LOGO_WIDTH, "");
    for (int i = 0; i < 6; i++)
        wprintf("-");
    wprintf("\n");

    /* Logo and information side by side, each continuing after the other
     * runs out. */
    int lines = (LOGO_LINES > rows + 2) ? LOGO_LINES : rows + 2;

    for (int i = 0; i < lines; i++) {
        if (i < LOGO_LINES) {
            wcolor(W_BLUE | W_BRIGHT, W_DEFAULT);
            wprintf("%s", logo[i]);
            wcolor_reset();
        } else {
            wprintf("%*s", LOGO_WIDTH, "");
        }

        /* The first two info rows sit beside the logo's blank top lines. */
        int row = i - 2;
        if (row >= 0 && row < rows) {
            wsnprintf(line, sizeof(line), "%s", values[row]);
            info_line(keys[row], line);
        }

        wprintf("\n");
    }

    /* The colour bar, so the palette is visible at a glance.  The normal
     * eight go in as backgrounds; the bright eight are drawn as foreground
     * blocks instead, because VGA text mode spends the top attribute bit on
     * blink rather than on a bright background. */
    wprintf("%*s", LOGO_WIDTH, "");
    for (int c = 0; c < 8; c++) {
        wcolor(W_DEFAULT, c);
        wprintf("   ");
    }
    wcolor_reset();
    wprintf("\n");

    wprintf("%*s", LOGO_WIDTH, "");
    for (int c = 0; c < 8; c++) {
        wcolor(c | W_BRIGHT, W_BLACK);
        wprintf("###");
    }
    wcolor_reset();
    wprintf("\n\n");

    return 0;
}
