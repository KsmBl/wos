/* The SYSCALL entry point.
 *
 * int 0x80 still works and still goes through the ordinary interrupt path;
 * this is the same journey by a shorter road.  A software interrupt makes the
 * CPU walk the IDT, read a gate descriptor, check its privilege and load a
 * stack from the TSS.  SYSCALL does none of that: it loads CS and SS from
 * fixed values in an MSR, jumps to another, and leaves everything else to us.
 * That is a few hundred cycles saved on every call into the kernel, on a
 * system that makes one for every character it reads.
 *
 * What "everything else" means is the stack.  SYSCALL does not switch it, so
 * on entry RSP still points into the calling process's memory and cannot be
 * trusted or used.  The kernel stack for this processor is reached through GS,
 * which is what SWAPGS is for: the per-processor block set up in sysentry.c is
 * in the shadow GS base until that instruction, and in GS afterwards.
 *
 * The frame built below is deliberately identical to the one the interrupt
 * stubs build, so interrupt_dispatch and everything downstream cannot tell the
 * difference and needs no special case.
 */

#include "gdt_sel.h"

.section .text
.global syscall_entry
.type syscall_entry, @function

syscall_entry:
    swapgs                          /* GS -> this processor's block */
    movq %rsp, %gs:8                /* stash the user stack         */
    movq %gs:0, %rsp                /* and take the kernel's        */

    /* Align it.  The C this eventually calls is compiled to the System V ABI,
     * which promises the compiler a 16-byte aligned stack -- and the compiler
     * spends that promise on instructions like MOVAPS, which fault rather than
     * run slowly when it turns out to be false.
     *
     * The interrupt path never had to think about this: delivering an
     * interrupt in 64-bit mode, the processor aligns the stack pointer itself
     * before pushing anything.  SYSCALL does not touch RSP at all, so what
     * arrives here is exactly what sysentry_set_kernel_stack() was given, and
     * that came from kmalloc, which aligns to eight.  Half of all kernel
     * stacks would be wrong, and wrong in a way that shows up only when the
     * compiler happens to choose an aligned instruction. */
    andq $-16, %rsp

    /* The five words the CPU would have pushed for an interrupt, in the order
     * regs_t expects.  RCX holds the return address and R11 the flags, which
     * is where SYSCALL put them. */
    pushq $SEL_UDATA                /* ss     */
    pushq %gs:8                     /* rsp    */
    pushq %r11                      /* rflags */
    pushq $SEL_UCODE                /* cs     */
    pushq %rcx                      /* rip    */

    /* And straight back, before anything else runs.
     *
     * Leaving GS swapped for the length of the call would mean every other way
     * into the kernel had to swap it too, because the kernel can switch
     * threads in the middle of a syscall and resume one that arrived by
     * interrupt instead -- and GS belongs to the processor, not the thread, so
     * whichever state was left loaded is the state that thread would return to
     * ring 3 with.  Getting that consistently right costs a test and a branch
     * on every interrupt and exception in the system, and gets it wrong
     * silently.
     *
     * Everything GS was needed for has already happened: the stack has been
     * switched and the user's stack pointer is on it.  Nothing else in this
     * kernel reads GS at all.  So the window where it is swapped is these five
     * instructions, with interrupts off because SYSCALL masked them, and there
     * is no context switch to be caught by. */
    swapgs

    pushq $0                        /* err_code: nothing produced one */
    pushq $0x80                     /* int_no: the number it used to arrive on */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    call interrupt_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp                  /* discard int_no and err_code */

    /* SYSRET can only return to ring 3 with the flags and the address it is
     * handed, and it is the caller's business whether that is still what the
     * frame says.  The kernel is free to have changed any of it -- a signal
     * delivered onto the user stack, an exec that replaced the program, a
     * scheduler that resumed a thread which entered some other way -- and in
     * any of those cases the honest instruction is IRETQ, which reloads
     * everything from the frame rather than assuming it.
     *
     * So the fast return is taken only when the frame is still exactly what
     * SYSCALL left: returning to user code, on the user stack, with a
     * canonical address.  Otherwise fall through to the general path, which is
     * always correct. */
    cmpq $SEL_UCODE, 8(%rsp)        /* cs  */
    jne  .Lslow
    cmpq $SEL_UDATA, 32(%rsp)       /* ss  */
    jne  .Lslow

    movq (%rsp), %rcx               /* rip -> where SYSRET wants it */

    /* Canonical means the top seventeen bits all agree.  A non-canonical
     * address in RCX makes SYSRET fault in ring 0 still on the user's stack,
     * which is the one way this instruction is genuinely dangerous.
     *
     * R11 is the scratch for the test because SYSCALL destroyed it on the way
     * in -- the caller already knows it is gone, and it is about to be loaded
     * with the flags anyway.  Using a callee-saved register here would hand
     * the process back a register the kernel had quietly scribbled on. */
    movq %rcx, %r11
    shlq $16, %r11
    sarq $16, %r11
    cmpq %rcx, %r11
    jne  .Lslow

    movq 16(%rsp), %r11             /* rflags */
    movq 24(%rsp), %rsp             /* back to the user stack */
    sysretq

.Lslow:
    iretq
