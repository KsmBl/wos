/* Interrupt stubs, x86-64.
 *
 * The CPU pushes a different amount of state depending on the vector.  These
 * stubs normalise that: after the common tail runs, the stack always holds a
 * complete `regs_t`.
 *
 * For vectors that push an error code the CPU has already done it; for the
 * rest we push a zero so the layout matches.
 */

/* Exception/interrupt without a CPU-pushed error code. */
.macro ISR_NOERR n
.global isr\n
.type isr\n, @function
isr\n:
    pushq $0
    pushq $\n
    jmp   int_common
.endm

/* Exception with a CPU-pushed error code. */
.macro ISR_ERR n
.global isr\n
.type isr\n, @function
isr\n:
    pushq $\n
    jmp   int_common
.endm

.section .text

/* Exceptions 0-31: 8, 10-14, 17, 21, 29 and 30 carry an error code. */
.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
    ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
    ISR_ERR \n
.endr

/* Hardware IRQs, remapped by the PIC to vectors 32-47. */
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR_NOERR \n
.endr

/* The syscall gate. */
ISR_NOERR 128

int_common:
    /* No pusha in long mode: push the fifteen registers by hand, in the
     * order regs_t expects to find them. */
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

    /* Segment registers are vestigial in long mode and the CPU has already
     * loaded the kernel's on entry, so there is nothing to switch here. */

    movq %rsp, %rdi                  /* regs_t * as the first argument */
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

    addq $16, %rsp                   /* discard int_no and err_code */
    iretq

/* A table of every stub, indexed by vector, so idt_init() can install them
 * in a loop instead of naming 49 externs. Index 48 is the syscall stub. */
.section .rodata
.align 8
.global isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .quad isr\n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr\n
.endr
    .quad isr128
