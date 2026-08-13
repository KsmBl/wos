/* setjmp and longjmp, x86-64.
 *
 * The jmp_buf holds the six registers the System V calling convention says a
 * function must give back to its caller unchanged, plus the stack pointer and
 * the address to continue at:
 *
 *     0   rbx        24  r13        48  rsp (as it will be after ret)
 *     8   rbp        32  r14        56  rip (the return address)
 *     16  r12        40  r15
 *
 * Nothing else is saved because nothing else is promised.  Every other
 * register belongs to the caller, which means the code around the setjmp has
 * either spilled it already or does not care what is in it -- and the local
 * variables that survive a longjmp are the ones the program declared volatile,
 * which is the standard's rule and not something this file can fix.
 */

.section .text, "ax"

.global setjmp
.type setjmp, @function
setjmp:
    movq  %rbx,  0(%rdi)
    movq  %rbp,  8(%rdi)
    movq  %r12, 16(%rdi)
    movq  %r13, 24(%rdi)
    movq  %r14, 32(%rdi)
    movq  %r15, 40(%rdi)

    /* The stack pointer to restore is the one this function will leave behind,
     * which is eight bytes above the return address sitting at the top. */
    leaq  8(%rsp), %rax
    movq  %rax, 48(%rdi)

    movq  (%rsp), %rax          /* where to come back to */
    movq  %rax, 56(%rdi)

    xorl  %eax, %eax            /* the first return is always 0 */
    ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
    movl  %esi, %eax

    /* longjmp(env, 0) has to make setjmp return 1: zero is how the program
     * tells the two returns apart, and giving it back would make the jump
     * indistinguishable from the call. */
    testl %eax, %eax
    jnz   1f
    movl  $1, %eax
1:
    movq   0(%rdi), %rbx
    movq   8(%rdi), %rbp
    movq  16(%rdi), %r12
    movq  24(%rdi), %r13
    movq  32(%rdi), %r14
    movq  40(%rdi), %r15

    /* The stack goes back first, then the jump.  rdi is still the buffer: it
     * is an argument register, so nothing above cares what happens to it. */
    movq  48(%rdi), %rsp
    jmp   *56(%rdi)
.size longjmp, . - longjmp

/* Nothing here needs an executable stack; say so, or the linker marks the
 * whole program as wanting one. */
.section .note.GNU-stack, "", @progbits
