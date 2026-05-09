/* Kernel formatted output: a small printf that writes to VGA and COM1.
 *
 * On x86-64 a 64-bit divide is a single instruction, so unlike the 32-bit
 * kernel this needs no help from libgcc and can print 64-bit quantities
 * directly.
 */

#include "kprintf.h"
#include "vga.h"
#include "fbcon.h"
#include "serial.h"
#include "io.h"

void kputc(char c)
{
    /* Early boot draws to VGA text mode; once the framebuffer console is up it
     * takes over.  Serial gets every byte throughout. */
    if (fbcon_active())
        fbcon_putc(c);
    else
        vga_putc(c);
    serial_putc(c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

/* Emit `s`, padded to `width` with `pad` on the left. */
static void emit_padded(const char *s, size_t len, int width, char pad)
{
    for (int i = (int)len; i < width; i++)
        kputc(pad);
    for (size_t i = 0; i < len; i++)
        kputc(s[i]);
}

static void print_uint(uint64_t value, uint64_t base, bool upper,
                       int width, char pad)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;

    char buf[65];
    size_t len = 0;

    if (value == 0) {
        buf[len++] = '0';
    } else {
        while (value != 0) {
            buf[len++] = digits[value % base];
            value /= base;
        }
    }

    /* buf holds the digits reversed; flip it in place. */
    for (size_t i = 0; i < len / 2; i++) {
        char t = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = t;
    }

    emit_padded(buf, len, width, pad);
}

static void print_int(int64_t value, int width, char pad)
{
    if (value < 0) {
        kputc('-');
        /* Negating the most negative value overflows, so widen through
         * unsigned before taking the magnitude. */
        print_uint(-(uint64_t)value, 10, false,
                   width > 0 ? width - 1 : 0, pad);
    } else {
        print_uint((uint64_t)value, 10, false, width, pad);
    }
}

/* Append the decimal form of `v` to buf[] at *pos. */
static void append_uint(char *buf, size_t *pos, size_t cap, uint64_t v)
{
    char tmp[21];
    size_t n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v != 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && *pos + 1 < cap)
        buf[(*pos)++] = tmp[--n];
}

/* Format a byte count with the largest unit that keeps the value >= 1,
 * e.g. 268435456 -> "256 MiB".  One decimal place, no floating point. */
const char *fmt_bytes(uint64_t bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    static char slots[4][24];
    static int  next_slot;

    char *buf = slots[next_slot];
    next_slot = (next_slot + 1) % 4;

    uint64_t whole = bytes;
    uint64_t frac  = 0;
    int      unit  = 0;

    while (whole >= 1024 && unit < 4) {
        frac  = ((whole % 1024) * 10) / 1024;
        whole /= 1024;
        unit++;
    }

    size_t pos = 0;
    append_uint(buf, &pos, sizeof(slots[0]), whole);
    if (unit > 0 && frac > 0) {
        buf[pos++] = '.';
        append_uint(buf, &pos, sizeof(slots[0]), frac);
    }
    buf[pos++] = ' ';
    for (const char *u = units[unit]; *u && pos + 1 < sizeof(slots[0]); u++)
        buf[pos++] = *u;
    buf[pos] = '\0';

    return buf;
}

void kvprintf(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            kputc(*fmt++);
            continue;
        }
        fmt++;                                  /* skip '%' */

        char pad   = ' ';
        int  width = 0;
        int  is_long = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* Length modifiers. `z` is here because size_t is 64-bit. */
        while (*fmt == 'l' || *fmt == 'z') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt++) {
        case 'd':
            print_int(is_long ? va_arg(ap, int64_t) : va_arg(ap, int32_t),
                      width, pad);
            break;
        case 'u':
            print_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t),
                       10, false, width, pad);
            break;
        case 'x':
            print_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t),
                       16, false, width, pad);
            break;
        case 'X':
            print_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t),
                       16, true, width, pad);
            break;
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 'p':
            kputs("0x");
            print_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, false,
                       16, '0');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t len = 0;
            while (s[len])
                len++;
            emit_padded(s, len, width, pad);
            break;
        }
        case '%': kputc('%'); break;
        default:  kputc('?'); break;            /* unknown conversion */
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...)
{
    va_list ap;

    cli();
    vga_set_color(VGA_WHITE, VGA_RED);
    kputs("\n*** KERNEL PANIC: ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputs(" ***\n");

    for (;;)
        hlt();
}
