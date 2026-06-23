/* Formatted output, line input, and the small helpers built on them. */

#include "wkernel.h"
#include <stdarg.h>

/* Where a formatting run is sending its output: either straight to a
 * descriptor, or into a caller's buffer. */
struct sink {
    int      fd;          /* used when buf is NULL */
    char    *buf;
    wsize_t  size;        /* capacity of buf, including room for the NUL */
    wsize_t  written;     /* characters produced, whether stored or not  */
};

/* Buffer descriptor output so a format string does not turn into one syscall
 * per character. */
#define FLUSH_AT 128

static char    out_buf[FLUSH_AT];
static wsize_t out_len;

static void sink_flush(struct sink *s)
{
    if (!s->buf && out_len) {
        wwrite(s->fd, out_buf, out_len);
        out_len = 0;
    }
}

static void sink_putc(struct sink *s, char c)
{
    if (s->buf) {
        if (s->written + 1 < s->size)
            s->buf[s->written] = c;
    } else {
        out_buf[out_len++] = c;
        if (out_len == FLUSH_AT)
            sink_flush(s);
    }
    s->written++;
}

static void sink_pad(struct sink *s, int count, char pad)
{
    while (count-- > 0)
        sink_putc(s, pad);
}

/* Render an unsigned value into `buf` (written backwards) and return its
 * length; the caller emits it with whatever padding applies. */
static int render_uint(char *buf, unsigned long value, unsigned long base,
                       int upper)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = upper ? upper_digits : lower_digits;
    int len = 0;

    if (value == 0)
        buf[len++] = '0';
    while (value) {
        buf[len++] = digits[value % base];
        value /= base;
    }
    return len;
}

static void emit_number(struct sink *s, const char *rev, int len,
                        const char *prefix, int width, int left, char pad)
{
    int prefix_len = 0;
    while (prefix && prefix[prefix_len])
        prefix_len++;

    int total = len + prefix_len;

    /* Zero padding goes after the sign, spaces go before it. */
    if (!left && pad == ' ')
        sink_pad(s, width - total, ' ');

    for (int i = 0; i < prefix_len; i++)
        sink_putc(s, prefix[i]);

    if (!left && pad == '0')
        sink_pad(s, width - total, '0');

    while (len)
        sink_putc(s, rev[--len]);

    if (left)
        sink_pad(s, width - total, ' ');
}

static void format(struct sink *s, const char *fmt, va_list ap)
{
    char rev[72];

    while (*fmt) {
        if (*fmt != '%') {
            sink_putc(s, *fmt++);
            continue;
        }
        fmt++;

        int  left  = 0;
        char pad   = ' ';
        int  width = 0;

        for (;;) {
            if (*fmt == '-')      { left = 1; fmt++; }
            else if (*fmt == '0') { pad = '0'; fmt++; }
            else                  break;
        }

        int is_long = 0;

        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* A precision, which for %s is the most characters to take from the
         * string.  Full-screen programs need it: text that has to stop at a
         * column has to be cut somewhere, and cutting it here beats every
         * caller copying into a buffer to do the same thing by hand. */
        int precision = -1;

        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
            if (precision < 0)
                precision = -1;       /* a negative one is no precision */
        }

        /* Length modifiers. 'z' is here because wsize_t is 64-bit. */
        while (*fmt == 'l' || *fmt == 'z') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt++) {
        case 'd':
        case 'i': {
            long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long mag = (v < 0) ? -(unsigned long)v : (unsigned long)v;
            int len = render_uint(rev, mag, 10, 0);
            emit_number(s, rev, len, (v < 0) ? "-" : NULL, width, left, pad);
            break;
        }
        case 'u': {
            unsigned long v = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
            int len = render_uint(rev, v, 10, 0);
            emit_number(s, rev, len, NULL, width, left, pad);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long v = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
            int len = render_uint(rev, v, 16, fmt[-1] == 'X');
            emit_number(s, rev, len, NULL, width, left, pad);
            break;
        }
        case 'p': {
            int len = render_uint(rev, (unsigned long)va_arg(ap, void *), 16, 0);
            while (len < 16)
                rev[len++] = '0';
            emit_number(s, rev, len, "0x", width, left, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left)
                sink_pad(s, width - 1, ' ');
            sink_putc(s, c);
            if (left)
                sink_pad(s, width - 1, ' ');
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            int len = (int)strlen(str);

            if (precision >= 0 && len > precision)
                len = precision;

            if (!left)
                sink_pad(s, width - len, ' ');
            for (int i = 0; i < len; i++)
                sink_putc(s, str[i]);
            if (left)
                sink_pad(s, width - len, ' ');
            break;
        }
        case '%':
            sink_putc(s, '%');
            break;
        default:
            /* Unknown conversion: show it rather than swallowing it, so the
             * mistake is visible in the output. */
            sink_putc(s, '%');
            sink_putc(s, fmt[-1]);
            break;
        }
    }
}

int wprintf(const char *fmt, ...)
{
    struct sink s = { W_STDOUT, NULL, 0, 0 };
    va_list ap;

    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);

    sink_flush(&s);
    return (int)s.written;
}

int wfprintf(int fd, const char *fmt, ...)
{
    struct sink s = { fd, NULL, 0, 0 };
    va_list ap;

    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);

    sink_flush(&s);
    return (int)s.written;
}

int wsnprintf(char *buf, wsize_t size, const char *fmt, ...)
{
    struct sink s = { -1, buf, size, 0 };
    va_list ap;

    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);

    if (size) {
        wsize_t at = (s.written < size - 1) ? s.written : size - 1;
        buf[at] = '\0';
    }
    return (int)s.written;
}

int wvsnprintf(char *buf, wsize_t size, const char *fmt, va_list ap)
{
    struct sink s = { -1, buf, size, 0 };

    format(&s, fmt, ap);

    if (size) {
        wsize_t at = (s.written < size - 1) ? s.written : size - 1;
        buf[at] = '\0';
    }
    return (int)s.written;
}

int wputs(const char *s)
{
    return wwrite(W_STDOUT, s, strlen(s));
}

int wgetline(char *buf, wsize_t size)
{
    if (size == 0)
        return 0;

    int n = wread(W_STDIN, buf, size - 1);
    if (n < 0)
        return n;

    /* The console is line buffered, so a read ends with the newline the user
     * pressed; the caller wants the text without it. */
    if (n > 0 && buf[n - 1] == '\n')
        n--;

    buf[n] = '\0';
    return n;
}

/* Number of results that can be live at once. Eight, because `ps` prints five
 * figures in a single wprintf and a caller has no way to notice the buffers
 * wrapping -- it just silently prints the wrong number. */
#define WHUMAN_SLOTS 8

const char *whuman(unsigned long bytes)
{
    static const char units[] = { 'B', 'K', 'M', 'G' };
    static char slots[WHUMAN_SLOTS][16];
    static int  next;

    char *buf = slots[next];
    next = (next + 1) % WHUMAN_SLOTS;

    unsigned long whole = bytes;
    unsigned long frac  = 0;
    int unit = 0;

    while (whole >= 1024 && unit < 3) {
        frac  = ((whole % 1024) * 10) / 1024;
        whole /= 1024;
        unit++;
    }

    if (unit == 0)
        wsnprintf(buf, sizeof(slots[0]), "%luB", whole);
    else
        wsnprintf(buf, sizeof(slots[0]), "%lu.%lu%c", whole, frac, units[unit]);

    return buf;
}

const char *wclock_string(unsigned int khz)
{
    static char slots[WHUMAN_SLOTS][16];
    static int  next;

    char *buf = slots[next];
    next = (next + 1) % WHUMAN_SLOTS;

    if (khz == 0)
        return "-";

    /* Two decimals in gigahertz: one is not enough to tell neighbouring steps
     * of a processor's clock apart, and 1.9 GHz next to 1.9 GHz reads as a
     * meter that is not working. */
    if (khz >= 1000000)
        wsnprintf(buf, sizeof(slots[0]), "%u.%02uGHz",
                  khz / 1000000, (khz % 1000000) / 10000);
    else
        wsnprintf(buf, sizeof(slots[0]), "%uMHz", khz / 1000);

    return buf;
}

const char *wstrerror(int err)
{
    switch (err) {
    case 0:              return "success";
    case W_EPERM:        return "operation not permitted";
    case W_ENOENT:       return "no such file or directory";
    case W_ESRCH:        return "no such process";
    case W_EIO:          return "input/output error";
    case W_E2BIG:        return "argument list too long";
    case W_ENOEXEC:      return "not a valid executable";
    case W_EBADF:        return "bad file descriptor";
    case W_ECHILD:       return "no child processes";
    case W_ENOMEM:       return "out of memory";
    case W_EACCES:       return "permission denied";
    case W_EFAULT:       return "bad address";
    case W_EBUSY:        return "resource busy";
    case W_EEXIST:       return "file exists";
    case W_ENOTDIR:      return "not a directory";
    case W_EISDIR:       return "is a directory";
    case W_EINVAL:       return "invalid argument";
    case W_ENFILE:       return "file table overflow";
    case W_EMFILE:       return "too many open files";
    case W_ENOSPC:       return "no space left on device";
    case W_ESPIPE:       return "illegal seek";
    case W_ERANGE:       return "result too large";
    case W_ENAMETOOLONG: return "name too long";
    case W_ENOSYS:       return "function not implemented";
    case W_ENOTEMPTY:    return "directory not empty";
    case W_EFBIG:        return "file too large";
    default:             return "unknown error";
    }
}

/* How long the machine has been up, in words.
 *
 * Shared so that the uptime command and the header htop draws cannot disagree
 * about it -- one of them would be quietly wrong, and there is no way to tell
 * which from looking.  Returns a pointer into a rotating set of buffers, like
 * whuman() above, so a few can be in one printf.
 */
const char *wuptime_string(void)
{
    static char slots[4][48];
    static int  next;

    char *buf = slots[next];
    next = (next + 1) % 4;

    unsigned seconds = wuptime_ms() / 1000u;
    unsigned days    = seconds / 86400u;
    unsigned hours   = (seconds % 86400u) / 3600u;
    unsigned minutes = (seconds % 3600u) / 60u;

    /* Days matter or they do not; below an hour, minutes are what a person
     * wants, and below a minute, seconds are the only thing moving. */
    if (days)
        wsnprintf(buf, sizeof(slots[0]), "%u day%s, %u:%02u",
                  days, days == 1 ? "" : "s", hours, minutes);
    else if (hours)
        wsnprintf(buf, sizeof(slots[0]), "%u:%02u", hours, minutes);
    else if (minutes)
        wsnprintf(buf, sizeof(slots[0]), "%u min%s", minutes,
                  minutes == 1 ? "" : "s");
    else
        wsnprintf(buf, sizeof(slots[0]), "%u sec%s", seconds,
                  seconds == 1 ? "" : "s");

    return buf;
}
