/* Fixed-width integer types and basic definitions for the WOS kernel.
 *
 * The kernel is freestanding, so it cannot use the host's <stdint.h> contents
 * wholesale; GCC does provide the freestanding headers, but we keep our own
 * short aliases because they read better in kernel code.
 */
#ifndef WOS_TYPES_H
#define WOS_TYPES_H

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef uint32_t           uintptr_t;

#define NULL ((void *)0)

/* <stdbool.h> is one of the freestanding headers GCC always provides, so it
 * is safe to use even without a hosted libc. */
#include <stdbool.h>

/* Round x up / down to the next multiple of a (a must be a power of two). */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

#define PAGE_SIZE 4096u

#endif /* WOS_TYPES_H */
