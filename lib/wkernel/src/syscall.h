/* Raw syscall entry, internal to lib/wkernel.
 *
 * The kernel takes the call number in rax and up to three arguments in the
 * System V argument registers rdi, rsi and rdx, and returns the result in rax.
 *
 * The way in is the syscall instruction, which the kernel answers in
 * sysentry.S.  It was int 0x80 before, and the kernel still accepts that on
 * the same vector; the difference is what the processor does before any kernel
 * code runs.  A software interrupt has it walk the descriptor table, check
 * privileges and load a stack from the TSS.  syscall does none of that -- two
 * registers from MSRs and a jump -- and a program that reads a file a line at
 * a time pays that difference on every line.
 *
 * rcx and r11 really are destroyed now: the instruction puts the return
 * address in one and the flags in the other.  They were already declared, in
 * anticipation of exactly this.
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
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long wsyscall1(long n, long a)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long wsyscall2(long n, long a, long b)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long wsyscall3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

#endif /* WKERNEL_SYSCALL_H */
