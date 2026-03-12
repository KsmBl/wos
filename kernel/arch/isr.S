/* Interrupt stubs.
 *
 * The CPU pushes a different amount of state depending on the vector and on
 * whether a privilege change happened.  These stubs normalise that: after the
 * common tail runs, the stack always holds a complete `regs_t`.
 *
 * For vectors that push an error code the CPU has already done it; for the
 * rest we push a zero so the layout matches.
 */

/* Exception/interrupt without a CPU-pushed error code. */
.macro ISR_NOERR n
.global isr\n
.type isr\n, @function
isr\n:
    pushl $0
    pushl $\n
    jmp   int_common
.endm

/* Exception with a CPU-pushed error code. */
.macro ISR_ERR n
.global isr\n
.type isr\n, @function
isr\n:
    pushl $\n
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
    pusha                        /* eax ecx edx ebx esp ebp esi edi */
    movl  %ds, %eax
    pushl %eax                   /* save the caller's data segment  */

    movw  $0x10, %ax             /* SEL_KDATA: kernel data segments */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    pushl %esp                   /* regs_t * argument               */
    call  interrupt_dispatch
    addl  $4, %esp

    popl  %eax                   /* restore the caller's segments   */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    popa
    addl  $8, %esp               /* discard int_no and err_code     */
    iret

/* A table of every stub, indexed by vector, so idt_init() can install them
 * in a loop instead of naming 49 externs. Index 48 is the syscall stub. */
.section .rodata
.global isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .long isr\n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long isr\n
.endr
    .long isr128
