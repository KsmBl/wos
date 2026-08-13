/* <stdint.h> -- integers of a size you can count on.
 *
 * LP64, which is what this machine is: int is 32 bits, long and every pointer
 * are 64.  The compiler's header rather than the library's, for the same
 * reason <stddef.h> is: what a `long` is, is the compiler's answer.
 */
#ifndef WCC_STDINT_H
#define WCC_STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef long               intptr_t;
typedef unsigned long      uintptr_t;
typedef long               intmax_t;
typedef unsigned long      uintmax_t;

#define INT8_MIN    (-128)
#define INT16_MIN   (-32768)
#define INT32_MIN   (-2147483647 - 1)
#define INT64_MIN   (-9223372036854775807L - 1)

#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define INT64_MAX   9223372036854775807L

#define UINT8_MAX   255
#define UINT16_MAX  65535
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615UL

#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define SIZE_MAX    UINT64_MAX

#endif /* WCC_STDINT_H */
