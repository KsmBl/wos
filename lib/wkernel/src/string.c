/* String and memory routines for applications.
 *
 * These carry their standard names because GCC emits calls to memcpy, memset,
 * memmove and memcmp on its own -- for struct assignment and array
 * initialisation -- even with -ffreestanding.
 */

#include "wkernel.h"

wsize_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (wsize_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, wsize_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dst;
}

wsize_t strlcpy(char *dst, const char *src, wsize_t size)
{
    wsize_t len = strlen(src);

    if (size != 0) {
        wsize_t copy = (len < size - 1) ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
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

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

void *memcpy(void *dst, const void *src, wsize_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;

    if (((unsigned long)d % 4) == 0 && ((unsigned long)s % 4) == 0) {
        unsigned int       *d4 = (unsigned int *)d;
        const unsigned int *s4 = (const unsigned int *)s;
        while (n >= 4) {
            *d4++ = *s4++;
            n -= 4;
        }
        d = (unsigned char *)d4;
        s = (const unsigned char *)s4;
    }

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, wsize_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0)
        return dst;

    /* Overlapping and moving up: copy backwards so each byte is read before
     * it gets overwritten. */
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

void *memset(void *dst, int c, wsize_t n)
{
    unsigned char *d = dst;
    unsigned char  v = (unsigned char)c;

    if (((unsigned long)d % 4) == 0) {
        unsigned int  word = (unsigned int)v * 0x01010101u;
        unsigned int *d4   = (unsigned int *)d;
        while (n >= 4) {
            *d4++ = word;
            n -= 4;
        }
        d = (unsigned char *)d4;
    }

    while (n--)
        *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, wsize_t n)
{
    const unsigned char *x = a, *y = b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

int atoi(const char *s)
{
    int sign = 1;
    int value = 0;

    while (*s == ' ' || *s == '\t')
        s++;

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (*s >= '0' && *s <= '9')
        value = value * 10 + (*s++ - '0');

    return sign * value;
}
