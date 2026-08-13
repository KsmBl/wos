/* Memory, buffers and the two ways this compiler gives up.
 *
 * The allocator never frees.  A compiler reads one program, writes one object
 * and exits; what it holds is bounded by what it read, and an arena with no
 * free() is a whole class of lifetime bug that cannot happen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "wcc.h"

void *wcc_alloc(size_t bytes)
{
    void *p = calloc(1, bytes ? bytes : 1);

    if (!p)
        fatal("out of memory");

    return p;
}

char *wcc_strndup(const char *s, size_t n)
{
    char *copy = wcc_alloc(n + 1);

    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

char *wcc_strdup(const char *s)
{
    return wcc_strndup(s, strlen(s));
}

void error_at(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s:%d: ", file ? file : "wcc", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    exit(1);
}

void fatal(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "wcc: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    exit(1);
}

/* ------------------------------------------------------------------ *
 *  Buffers
 * ------------------------------------------------------------------ */

void buf_init(buffer_t *b)
{
    b->data     = NULL;
    b->len      = 0;
    b->capacity = 0;
}

static void buf_reserve(buffer_t *b, int extra)
{
    if (b->len + extra <= b->capacity)
        return;

    int want = b->capacity ? b->capacity : 256;
    while (want < b->len + extra)
        want *= 2;

    unsigned char *bigger = realloc(b->data, (size_t)want);
    if (!bigger)
        fatal("out of memory growing a buffer to %d bytes", want);

    b->data     = bigger;
    b->capacity = want;
}

void buf_put(buffer_t *b, const void *bytes, int len)
{
    if (len <= 0)
        return;

    buf_reserve(b, len);
    memcpy(b->data + b->len, bytes, (size_t)len);
    b->len += len;
}

void buf_byte(buffer_t *b, int byte)
{
    unsigned char c = (unsigned char)byte;
    buf_put(b, &c, 1);
}

/* Little-endian, which is what x86-64 reads and what ELF says on it. */
void buf_u16(buffer_t *b, unsigned value)
{
    buf_byte(b, (int)(value & 0xFF));
    buf_byte(b, (int)((value >> 8) & 0xFF));
}

void buf_u32(buffer_t *b, unsigned value)
{
    for (int i = 0; i < 4; i++)
        buf_byte(b, (int)((value >> (i * 8)) & 0xFF));
}

void buf_u64(buffer_t *b, unsigned long value)
{
    for (int i = 0; i < 8; i++)
        buf_byte(b, (int)((value >> (i * 8)) & 0xFF));
}

void buf_zero(buffer_t *b, int count)
{
    for (int i = 0; i < count; i++)
        buf_byte(b, 0);
}

void buf_align(buffer_t *b, int alignment)
{
    while (alignment > 1 && b->len % alignment)
        buf_byte(b, 0);
}

int buf_string(buffer_t *b, const char *s)
{
    int at = b->len;

    buf_put(b, s, (int)strlen(s) + 1);
    return at;
}
