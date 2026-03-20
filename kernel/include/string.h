/* Freestanding string and memory routines.
 *
 * These carry their standard names on purpose: even with -ffreestanding, GCC
 * is allowed to emit calls to memcpy, memset, memmove and memcmp for things
 * like struct assignment, so the kernel must provide them under exactly those
 * symbols or it will fail to link (or worse, link against nothing).
 */
#ifndef WOS_STRING_H
#define WOS_STRING_H

#include "types.h"

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);

/* Copy at most n-1 bytes and always NUL-terminate.  Unlike strncpy this
 * cannot leave the destination unterminated, which is the whole point. */
size_t strlcpy(char *dst, const char *src, size_t n);

char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

#endif /* WOS_STRING_H */
