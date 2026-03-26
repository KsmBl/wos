/* Raw syscall entry, internal to lib/wkernel.
 *
 * The kernel takes the call number in eax and up to three arguments in ebx,
 * ecx and edx, and returns the result in eax.  "memory" clobbers keep the
 * compiler from caching anything the kernel may have written through a
 * pointer we passed it.
 */
#ifndef WKERNEL_SYSCALL_H
#define WKERNEL_SYSCALL_H

#include "wabi.h"

static inline int wsyscall0(int n)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

static inline int wsyscall1(int n, int a)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static inline int wsyscall2(int n, int a, int b)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static inline int wsyscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

#endif /* WKERNEL_SYSCALL_H */
