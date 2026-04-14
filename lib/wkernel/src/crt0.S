/* C runtime startup for WOS applications, x86-64.
 *
 * The kernel enters a new process with the stack laid out as
 *
 *     [argc][argv[0]][argv[1]]...[NULL]  <- rsp
 *
 * so all this has to do is move those into the System V argument registers
 * and hand main's return value to wexit.  Placing this in .text.start makes
 * the linker script put _start at the very front of the image, where the ELF
 * entry point points.
 */

.section .text.start, "ax"
.global _start
.type _start, @function
_start:
    /* A zeroed frame pointer terminates any stack walk cleanly. */
    xorq  %rbp, %rbp

    movq  (%rsp), %rdi          /* argc */
    leaq  8(%rsp), %rsi         /* argv */

    /* System V wants rsp 16-byte aligned at the call instruction.  The kernel
     * aligned the argv block, and popping nothing since means rsp is already
     * where it should be; force it regardless so a future change to the entry
     * layout cannot silently misalign a callee. */
    andq  $-16, %rsp

    call  main

    /* main returned, so exit with its value. wexit never comes back. */
    movl  %eax, %edi
    call  wexit

    /* Unreachable: stop the CPU rather than run into whatever follows. */
1:  hlt
    jmp 1b
.size _start, . - _start
