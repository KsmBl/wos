/* WOS boot entry point.
 *
 * GRUB loads us in 32-bit protected mode with paging off, A20 enabled and an
 * unknown GDT.  All we do here is give the CPU a stack and hand control to the
 * C kernel, passing along the multiboot magic value and info pointer.
 */

.set MB_ALIGN,    1 << 0              /* align loaded modules on page boundaries */
.set MB_MEMINFO,  1 << 1              /* ask GRUB for the memory map            */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

/* 16 KiB boot stack.  The kernel switches to per-thread stacks once the
 * scheduler is up; this one becomes the idle thread's stack. */
.section .bss, "aw", @nobits
.align 16
stack_bottom:
    .skip 16384
.global __boot_stack_top
__boot_stack_top:
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    movl $stack_top, %esp
    movl %esp, %ebp

    /* Clear EFLAGS (notably the direction flag: the C ABI wants DF = 0). */
    pushl $0
    popfl

    pushl %ebx                        /* struct multiboot_info *              */
    pushl %eax                        /* multiboot magic                      */
    call  kmain
    addl  $8, %esp

    /* kmain must not return, but if it does, park the CPU. */
.global halt_forever
halt_forever:
1:  cli
    hlt
    jmp 1b
.size _start, . - _start
