/* Context switching and the ring 3 entry gate, x86-64. */

.section .text

/* void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
 *
 * Saves the callee-saved registers and the flags on the current kernel stack,
 * records the resulting rsp through `save_rsp`, then adopts `new_rsp` and
 * unwinds the mirror image of that state.  The `ret` at the end therefore
 * returns into whatever the *other* thread was doing when it switched out.
 *
 * System V passes the arguments in rdi and rsi, so unlike the 32-bit version
 * there is no stack arithmetic to get wrong.
 *
 * A brand new thread has its stack primed by hand to look exactly like a
 * thread that had switched out, with the return address pointing at its entry
 * trampoline -- see thread_prime_stack() in proc.c.
 */
.global switch_context
.type switch_context, @function
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq

    movq %rsp, (%rdi)           /* save_rsp */
    movq %rsi, %rsp             /* new_rsp  */

    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size switch_context, . - switch_context

/* void enter_user_mode(uint64_t entry, uint64_t user_stack);
 *
 * Drops to ring 3.  There is no instruction that just lowers the privilege
 * level, so we forge the stack frame an interrupt would have pushed on entry
 * from ring 3 and let iretq consume it on the way back out.
 */
.global enter_user_mode
.type enter_user_mode, @function
enter_user_mode:
    movq %rdi, %rcx             /* entry point    */
    movq %rsi, %rdx             /* user stack top */

    /* Long mode ignores the data segment bases, but the selectors still have
     * to name a ring 3 descriptor rather than whatever was loaded before. */
    movw $0x1B, %ax             /* SEL_UDATA */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushq $0x1B                 /* ss     : user data, RPL 3            */
    pushq %rdx                  /* rsp    : top of the user stack       */
    pushq $0x202                /* rflags : reserved bit + IF, so the
                                 *          process can be preempted    */
    pushq $0x23                 /* cs     : user code, RPL 3            */
    pushq %rcx                  /* rip    : the program's entry point   */
    iretq
.size enter_user_mode, . - enter_user_mode
