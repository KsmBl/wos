/* The string functions wkernel has no use for.
 *
 * What is here is what a ported program reaches for and the system itself
 * never needed: the bounded copies, the span functions, the tokeniser.  The
 * ones wkernel already has -- strlen, strcmp, memcpy and the rest -- are not
 * repeated; <string.h> declares them and the linker finds them in
 * libwkernel.a.  Two implementations of strlen on one machine would be one
 * too many.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "wkernel.h"

/* strncpy's contract is the strange one and worth stating: it does not
 * terminate the destination when the source fills it, and it pads with zeroes
 * when the source is shorter.  It is not a safe strcpy -- strlcpy, which
 * wkernel has, is the one that is. */
char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < n)
        dst[i++] = '\0';

    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *at = dst + strlen(dst);

    while (n-- && *src)
        *at++ = *src++;

    *at = '\0';
    return dst;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *copy = malloc(n);

    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(copy, s, n);
    return copy;
}

char *strndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;

    char *copy = malloc(len + 1);
    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static int in_set(char c, const char *set)
{
    for (const char *p = set; *p; p++)
        if (*p == c)
            return 1;
    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && in_set(s[n], accept))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !in_set(s[n], reject))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        if (in_set(*s, accept))
            return (char *)s;
    return NULL;
}

char *strtok_r(char *s, const char *sep, char **state)
{
    if (!s)
        s = *state;
    if (!s)
        return NULL;

    s += strspn(s, sep);
    if (!*s) {
        *state = NULL;
        return NULL;
    }

    char *end = s + strcspn(s, sep);

    if (*end) {
        *end   = '\0';
        *state = end + 1;
    } else {
        *state = NULL;
    }

    return s;
}

/* The one with the static state, kept because ported code calls it.  Anything
 * new should use strtok_r: one buffer being tokenised at a time is a limit
 * that is invisible until two loops are nested. */
static char *strtok_state;

char *strtok(char *s, const char *sep)
{
    return strtok_r(s, sep, &strtok_state);
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }

    return NULL;
}

char *strerror(int err)
{
    /* wstrerror() takes the same numbers, since errno holds the W_E* values
     * unchanged.  The cast is because the standard says char * and wkernel,
     * which returns a string nobody may write to, says const. */
    return (char *)wstrerror(err);
}
