/* Context switching and the ring 3 entry gate. */

.section .text

/* void switch_context(uint32_t *save_esp, uint32_t new_esp);
 *
 * Saves the callee-saved registers and the flags on the current kernel stack,
 * records the resulting esp through `save_esp`, then adopts `new_esp` and
 * unwinds the mirror image of that state.  The `ret` at the end therefore
 * returns into whatever the *other* thread was doing when it switched out.
 *
 * A brand new thread has its stack primed by hand to look exactly like a
 * thread that had switched out, with the return address pointing at its
 * entry trampoline -- see thread_prime_stack() in proc.c.
 */
.global switch_context
.type switch_context, @function
switch_context:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushfl

    /* Five dwords pushed, so the arguments sit 20 bytes further up than
     * they did on entry. */
    movl  24(%esp), %eax        /* save_esp */
    movl  %esp, (%eax)
    movl  28(%esp), %esp        /* new_esp  */

    popfl
    popl  %edi
    popl  %esi
    popl  %ebx
    popl  %ebp
    ret
.size switch_context, . - switch_context

/* void enter_user_mode(uint32_t entry, uint32_t user_stack);
 *
 * Drops to ring 3.  There is no instruction that just lowers the privilege
 * level, so we forge the stack frame an interrupt would have pushed on entry
 * from ring 3 and let iret consume it on the way back out.
 */
.global enter_user_mode
.type enter_user_mode, @function
enter_user_mode:
    movl  4(%esp), %ecx         /* entry point      */
    movl  8(%esp), %edx         /* user stack top   */

    /* Load the ring 3 data selector into every data segment. */
    movw  $0x23, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    pushl $0x23                 /* ss     : user data, RPL 3            */
    pushl %edx                  /* esp    : top of the user stack       */
    pushl $0x202                /* eflags : reserved bit + IF, so the
                                 *          process can be preempted    */
    pushl $0x1B                 /* cs     : user code, RPL 3            */
    pushl %ecx                  /* eip    : the program's entry point   */
    iret
.size enter_user_mode, . - enter_user_mode
