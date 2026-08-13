/* <string.h> -- the string functions, under their standard names.
 *
 * Most of these are wkernel's own, declared here rather than reimplemented:
 * there is one strlen on this machine and both libraries name it the same
 * thing.  What this library adds is the rest of the standard set -- the ones
 * wkernel never needed and a ported program does.
 */
#ifndef WLIBC_STRING_H
#define WLIBC_STRING_H

#include <stddef.h>

/* Implemented in libwkernel.a. */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
size_t strlcpy(char *dst, const char *src, size_t size);

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* Implemented here. */
char  *strncpy(char *dst, const char *src, size_t n);
char  *strncat(char *dst, const char *src, size_t n);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strtok(char *s, const char *sep);
char  *strtok_r(char *s, const char *sep, char **state);
void  *memchr(const void *s, int c, size_t n);

/* The message for an errno value.  Positive, as errno holds it -- unlike
 * wstrerror(), which takes the same numbers but is reached by negating what a
 * wkernel call returned. */
char *strerror(int err);

#endif /* WLIBC_STRING_H */
