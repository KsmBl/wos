/* Raw syscall entry, internal to lib/wkernel.
 *
 * The kernel takes the call number in rax and up to three arguments in the
 * System V argument registers rdi, rsi and rdx, and returns the result in rax.
 *
 * rcx and r11 are listed as clobbered because that is what the syscall
 * instruction would destroy; int 0x80 does not, but declaring it costs
 * nothing and keeps the wrappers correct if the entry mechanism ever changes.
 *
 * "memory" clobbers keep the compiler from caching anything the kernel may
 * have written through a pointer we passed it.
 */
#ifndef WKERNEL_SYSCALL_H
#define WKERNEL_SYSCALL_H

#include "wabi.h"

static inline long wsyscall0(long n)
{
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long wsyscall1(long n, long a)
{
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long wsyscall2(long n, long a, long b)
{
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long wsyscall3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

#endif /* WKERNEL_SYSCALL_H */
