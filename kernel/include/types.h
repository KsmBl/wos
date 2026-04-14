/* Fixed-width integer types and basic definitions for the WOS kernel.
 *
 * WOS is an x86-64 kernel, so this is an LP64 target: int is 32 bits, long and
 * pointers are 64.  Sizes and addresses are 64-bit throughout; only the
 * on-disk filesystem format pins itself to fixed widths, because that has to
 * match what the host tool writes.
 */
#ifndef WOS_TYPES_H
#define WOS_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long      uint64_t;
typedef signed long        int64_t;

typedef uint64_t           size_t;
typedef int64_t            ssize_t;
typedef uint64_t           uintptr_t;

#define NULL ((void *)0)

/* <stdbool.h> is one of the freestanding headers GCC always provides, so it
 * is safe to use even without a hosted libc. */
#include <stdbool.h>

/* Round x up / down to the next multiple of a (a must be a power of two). */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

#define PAGE_SIZE 4096UL

#endif /* WOS_TYPES_H */
