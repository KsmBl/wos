/* <limits.h> -- how far each integer type reaches.
 *
 * This header belongs to a C library rather than to a compiler, which is why
 * it is here: gcc ships one, but its version hands over to the host's
 * (`#include_next <limits.h>`) for exactly these values, and the host's C
 * library is the one thing a WOS program must not be built against.
 *
 * The numbers are for LP64, which is what this machine is: int is 32 bits,
 * long and every pointer are 64.  The minima are written as (-MAX - 1) rather
 * than as a literal, because the literal -9223372036854775808 is parsed as a
 * negation applied to a number too large to be a signed long.
 */
#ifndef WLIBC_LIMITS_H
#define WLIBC_LIMITS_H

#define CHAR_BIT    8
#define MB_LEN_MAX  1

/* Plain char is signed on x86-64. */
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX

#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535

#define INT_MIN     (-INT_MAX - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_MIN    (-LONG_MAX - 1L)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL

#define LLONG_MIN   (-LLONG_MAX - 1LL)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

/* The longest path this system will accept, which is what a program sizing a
 * buffer for one wants.  W_PATH_MAX in <wabi.h> is the same number. */
#define PATH_MAX    255
#define NAME_MAX    27

#endif /* WLIBC_LIMITS_H */
