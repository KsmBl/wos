/* Leaving, converting, sorting.
 *
 * malloc and its family are not here: they are wkernel's, and <stdlib.h>
 * declares them under the same names.  One allocator on this machine.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "wkernel.h"

/* Flush every stream. In stdio.c, where the streams are. */
void _wc_flush_all(void);

/* ------------------------------------------------------------------ *
 *  Leaving
 * ------------------------------------------------------------------ */

#define MAX_ATEXIT 32

static void (*at_exit[MAX_ATEXIT])(void);
static int    at_exit_count;

int atexit(void (*fn)(void))
{
    if (!fn || at_exit_count == MAX_ATEXIT)
        return -1;

    at_exit[at_exit_count++] = fn;
    return 0;
}

void exit(int status)
{
    /* Backwards, which is the standard's rule and the useful one: a handler
     * registered later may depend on what an earlier one set up. */
    while (at_exit_count > 0)
        at_exit[--at_exit_count]();

    _wc_flush_all();
    wexit(status);
}

void abort(void)
{
    /* Whatever was buffered still goes out.  A program that died mid-sentence
     * is easier to understand with the sentence than without it, and there is
     * no signal here for a debugger to catch instead. */
    _wc_flush_all();
    wexit(134);              /* 128 + SIGABRT, the shell's convention */
}

/* ------------------------------------------------------------------ *
 *  Numbers out of text
 * ------------------------------------------------------------------ */

/* The shared engine.  Returns the magnitude and reports the sign and where it
 * stopped, so the signed and unsigned versions differ only in how they clamp. */
static unsigned long scan_integer(const char *s, char **end, int base,
                                  int *negative, int *overflowed)
{
    const char *start = s;

    *negative   = 0;
    *overflowed = 0;

    while (isspace((unsigned char)*s))
        s++;

    if (*s == '+' || *s == '-')
        *negative = (*s++ == '-');

    /* Base 0 means "read the prefix": 0x is hexadecimal, a leading 0 is
     * octal, anything else is decimal. */
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }

    if (base < 2 || base > 36) {
        errno = EINVAL;
        if (end)
            *end = (char *)start;
        return 0;
    }

    unsigned long value = 0;
    int           any   = 0;

    for (;; s++) {
        int digit;

        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        /* Checked before the multiply rather than after: an unsigned overflow
         * wraps silently, so there would be nothing to detect afterwards. */
        if (value > (ULONG_MAX - (unsigned long)digit) / (unsigned long)base)
            *overflowed = 1;
        else
            value = value * (unsigned long)base + (unsigned long)digit;

        any = 1;
    }

    /* Nothing was converted: the end pointer goes back to the start, as the
     * standard says, so the caller can tell "no digits" from "the value 0". */
    if (end)
        *end = (char *)(any ? s : start);

    return value;
}

long strtol(const char *s, char **end, int base)
{
    int negative, overflowed;
    unsigned long v = scan_integer(s, end, base, &negative, &overflowed);

    if (overflowed || (!negative && v > (unsigned long)LONG_MAX) ||
        (negative && v > (unsigned long)LONG_MAX + 1)) {
        errno = ERANGE;
        return negative ? LONG_MIN : LONG_MAX;
    }

    return negative ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    int negative, overflowed;
    unsigned long v = scan_integer(s, end, base, &negative, &overflowed);

    if (overflowed) {
        errno = ERANGE;
        return ULONG_MAX;
    }

    /* strtoul("-1") is ULONG_MAX, which surprises everybody once and is what
     * the standard asks for. */
    return negative ? (unsigned long)(-(long)v) : v;
}

long long strtoll(const char *s, char **end, int base)
{
    /* long and long long are both 64-bit here, so this is the same function
     * with a wider-looking name. */
    return (long long)strtol(s, end, base);
}

int  atoi(const char *s);                    /* wkernel's */
long atol(const char *s) { return strtol(s, NULL, 10); }

int  abs(int v)   { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* ------------------------------------------------------------------ *
 *  Sorting and searching
 * ------------------------------------------------------------------ */

static void swap_bytes(char *a, char *b, size_t size)
{
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Quicksort, with two guards that matter more here than speed does.
 *
 * The pivot is the median of the first, middle and last elements, so already
 * sorted input -- which is most of what a compiler sorts -- does not become
 * the quadratic case.  And only the smaller half is recursed into, the larger
 * one being looped on, so the stack depth is bounded by log2(count) however
 * badly the pivots fall.  A user stack here is 64 KiB. */
static void quicksort(char *base, size_t count, size_t size,
                      int (*compare)(const void *, const void *))
{
    while (count > 1) {
        if (count <= 8) {
            /* Insertion sort: fewer comparisons than a partition on a run this
             * short, and no recursion at all. */
            for (size_t i = 1; i < count; i++)
                for (size_t j = i; j > 0 &&
                     compare(base + (j - 1) * size, base + j * size) > 0; j--)
                    swap_bytes(base + (j - 1) * size, base + j * size, size);
            return;
        }

        char *first  = base;
        char *middle = base + (count / 2) * size;
        char *last   = base + (count - 1) * size;

        if (compare(middle, first) < 0)  swap_bytes(middle, first, size);
        if (compare(last, first) < 0)    swap_bytes(last, first, size);
        if (compare(last, middle) < 0)   swap_bytes(last, middle, size);

        /* The median is parked at the front and kept out of the partition. */
        swap_bytes(first, middle, size);

        size_t split = 0;
        for (size_t i = 1; i < count; i++) {
            if (compare(base + i * size, base) < 0) {
                split++;
                swap_bytes(base + split * size, base + i * size, size);
            }
        }
        swap_bytes(base, base + split * size, size);

        size_t left_count  = split;
        size_t right_count = count - split - 1;
        char  *right       = base + (split + 1) * size;

        if (left_count < right_count) {
            quicksort(base, left_count, size, compare);
            base  = right;
            count = right_count;
        } else {
            quicksort(right, right_count, size, compare);
            count = left_count;
        }
    }
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    if (base && count > 1 && size > 0)
        quicksort(base, count, size, compare);
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const char *at = base;

    while (count > 0) {
        size_t      half  = count / 2;
        const char *probe = at + half * size;
        int         order = compare(key, probe);

        if (order == 0)
            return (void *)probe;

        if (order > 0) {
            at    = probe + size;
            count = count - half - 1;
        } else {
            count = half;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ *
 *  The rest
 * ------------------------------------------------------------------ */

/* The linear congruential generator from the C standard's own example.  Not a
 * good source of randomness and not meant to be one -- what it is for is a
 * program that wants the same sequence every run unless it says otherwise. */
static unsigned long rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245 + 12345;
    return (int)((rand_state >> 16) & 0x7FFFFFFF);
}

void srand(unsigned int seed)
{
    rand_state = seed;
}

char *getenv(const char *name)
{
    /* There is no environment on this machine: a program is given its
     * arguments and nothing else.  Answering "not set" is the truth. */
    (void)name;
    return NULL;
}
