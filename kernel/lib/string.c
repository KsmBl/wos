/* Freestanding string and memory routines. */

#include "string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = dst;
    const uint8_t *s = src;

    /* Copy whole words while both pointers stay aligned, and the word here is
     * the machine's own: this is a 64-bit kernel, and moving four bytes at a
     * time was doing half the work per instruction that the registers allow.
     * It runs on every file read and every message copied to a process, so the
     * width is worth having. */
    if (((uintptr_t)d % 8) == 0 && ((uintptr_t)s % 8) == 0) {
        uint64_t       *d8 = (uint64_t *)d;
        const uint64_t *s8 = (const uint64_t *)s;
        while (n >= 8) {
            *d8++ = *s8++;
            n -= 8;
        }
        d = (uint8_t *)d8;
        s = (const uint8_t *)s8;
    } else if (((uintptr_t)d % 4) == 0 && ((uintptr_t)s % 4) == 0) {
        /* Aligned to four but not to eight: still better than bytes. */
        uint32_t       *d4 = (uint32_t *)d;
        const uint32_t *s4 = (const uint32_t *)s;
        while (n >= 4) {
            *d4++ = *s4++;
            n -= 4;
        }
        d = (uint8_t *)d4;
        s = (const uint8_t *)s4;
    }

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return dst;

    /* Overlapping and moving forward: copy backwards so we read each byte
     * before it is overwritten. */
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t  v = (uint8_t)c;

    if (((uintptr_t)d % 8) == 0) {
        uint64_t  word = (uint64_t)v * 0x0101010101010101ULL;
        uint64_t *d8   = (uint64_t *)d;
        while (n >= 8) {
            *d8++ = word;
            n -= 8;
        }
        d = (uint8_t *)d8;
    } else if (((uintptr_t)d % 4) == 0) {
        uint32_t  word = (uint32_t)v * 0x01010101u;
        uint32_t *d4   = (uint32_t *)d;
        while (n >= 4) {
            *d4++ = word;
            n -= 4;
        }
        d = (uint8_t *)d4;
    }

    while (n--)
        *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(uint8_t)*a - (int)(uint8_t)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);

    if (n != 0) {
        size_t copy = (len < n - 1) ? len : n - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;    /* the length it wanted, so truncation is detectable */
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (; *s; s++)
        if (*s == (char)c)
            last = s;
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}
