/* WOS boot entry point.
 *
 * GRUB hands us a 32-bit protected mode machine with paging off, which is all
 * Multiboot 1 can do.  Long mode cannot be entered directly from there: paging
 * must already be enabled, and paging in long mode needs a 4-level table.  So
 * this stub runs in 32 bits just long enough to
 *
 *   1. check that the processor actually has long mode,
 *   2. build a page table identity mapping the first 1 GiB,
 *   3. turn on PAE, set EFER.LME and enable paging,
 *   4. far-jump through a 64-bit code segment into kmain.
 *
 * A processor without long mode gets a legible message rather than a triple
 * fault.  There is no 32-bit fallback: the rest of the kernel is compiled for
 * x86-64, so there would be nothing to fall back into.  Systems that support
 * both ship two kernels and choose at install time.
 */

.set MB_ALIGN,    1 << 0              /* align loaded modules on page boundaries */
.set MB_MEMINFO,  1 << 1              /* ask GRUB for the memory map            */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

/* Note what is *not* requested here: the video flag (bit 2), which would ask
 * GRUB to set a mode and report the framebuffer's address.  Asking makes the
 * mode switch mandatory -- GRUB aborts the boot with
 *
 *     error: video/video.c:grub_video_set_mode:782:no suitable video mode found
 *
 * on any machine where it cannot deliver one, which includes plain BIOS
 * machines whose GRUB has no video driver loaded for the text mode it is
 * already displaying.  Not asking leaves the console exactly as the firmware
 * set it up, which is what this kernel wants anyway: it captures its 8x16 font
 * from the card's text-mode font memory, and that memory holds pixels rather
 * than glyphs once anything has switched to graphics.
 *
 * GRUB still fills the framebuffer fields in on its own where it has no text
 * mode to leave us in (a UEFI boot), and fbcon_init_boot() uses them if it
 * finds them. */
.section .multiboot, "a"
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

.section .bss, "aw", @nobits
.align 4096
/* The boot page tables.  These stay in use as the kernel's own address space,
 * so paging.c refers to them by name. */
.global boot_pml4
.global boot_pdpt
.global boot_pd
boot_pml4: .skip 4096
boot_pdpt: .skip 4096
boot_pd:   .skip 4096

.align 16
stack_bottom:
    .skip 16384
.global __boot_stack_top
__boot_stack_top:
.global stack_top
stack_top:

saved_magic: .skip 4
saved_info:  .skip 4

.section .rodata
.align 8
/* A flat 64-bit GDT, enough to reach long mode.  gdt.c replaces it with the
 * real one, complete with user segments and a TSS, once C code is running. */
gdt64:
    .quad 0                           /* null                              */
    .quad 0x00AF9A000000FFFF          /* 0x08 kernel code, 64-bit (L bit)  */
    .quad 0x00CF92000000FFFF          /* 0x10 kernel data                  */
gdt64_end:
/* Exported: linuxboot.S loads the same GDT from its own 64-bit entry. */
.global gdt64_pointer
gdt64_pointer:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

msg_no_cpuid: .asciz "WOS: this processor does not support CPUID, so it cannot be 64-bit"
msg_no_long:  .asciz "WOS: this processor does not support 64-bit long mode"

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    movl $stack_top, %esp

    /* Clear EFLAGS; the C ABI wants the direction flag clear. */
    pushl $0
    popfl

    /* Stash what GRUB gave us; the registers will not survive what follows. */
    movl %eax, saved_magic
    movl %ebx, saved_info

    call check_cpuid
    call check_long_mode
    call build_page_tables
    call enter_long_mode

    lgdt gdt64_pointer
    ljmp $0x08, $long_mode_entry

/* CPUID exists if bit 21 of EFLAGS can be flipped.
 *
 * The three routines below are shared with linuxboot.S, which needs the same
 * machine brought up the same way after its own entry path. */
.global check_cpuid
check_cpuid:
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $(1 << 21), %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx                        /* put the original flags back */
    popfl
    cmpl %ecx, %eax
    je no_cpuid
    ret

/* Extended leaf 0x80000001, EDX bit 29, is the long mode feature bit. */
.global check_long_mode
check_long_mode:
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode

    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz no_long_mode
    ret

/* Identity map the first 1 GiB using 2 MiB pages.
 *
 * Huge pages keep this to a single page directory: 512 entries instead of the
 * 512 page tables 4 KiB pages would need.  The kernel, the frame bitmap, the
 * heap and every page table it later allocates all live inside this window,
 * so the kernel can always reach them through the identity map. */
.global build_page_tables
build_page_tables:
    movl $boot_pdpt, %eax
    orl  $0x03, %eax                  /* present | writable */
    movl %eax, boot_pml4
    movl $0, boot_pml4 + 4

    movl $boot_pd, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt
    movl $0, boot_pdpt + 4

    xorl %ecx, %ecx
1:  movl $0x200000, %eax
    mull %ecx                         /* edx:eax = index * 2 MiB */
    orl  $0x83, %eax                  /* present | writable | huge */
    movl %eax, boot_pd(, %ecx, 8)
    movl %edx, boot_pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b
    ret

.global enter_long_mode
enter_long_mode:
    movl $boot_pml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl  $(1 << 5), %eax              /* PAE: required by long mode */
    movl %eax, %cr4

    movl $0xC0000080, %ecx            /* EFER */
    rdmsr
    orl  $(1 << 8), %eax              /* LME: long mode enable */
    wrmsr

    movl %cr0, %eax
    orl  $0x80010001, %eax            /* PG | WP | PE */
    movl %eax, %cr0
    ret

no_cpuid:
    movl $msg_no_cpuid, %esi
    jmp  fatal32
no_long_mode:
    movl $msg_no_long, %esi
    jmp  fatal32

/* Report a fatal problem before any driver exists, on the VGA text screen and
 * on COM1, then stop.  Both are set up from scratch here because nothing else
 * has run yet. */
fatal32:
    /* COM1: 115200 8N1, no interrupts. */
    movw $0x3F9, %dx
    movb $0x00, %al
    outb %al, %dx
    movw $0x3FB, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3F8, %dx
    movb $0x01, %al
    outb %al, %dx
    movw $0x3F9, %dx
    movb $0x00, %al
    outb %al, %dx
    movw $0x3FB, %dx
    movb $0x03, %al
    outb %al, %dx

    movl $0xB8000, %edi
1:  movb (%esi), %al
    testb %al, %al
    jz 3f

    movb %al, (%edi)                  /* VGA: white on red */
    movb $0x4F, 1(%edi)
    addl $2, %edi

2:  movw $0x3FD, %dx                  /* wait for the transmitter */
    inb  %dx, %al
    testb $0x20, %al
    jz 2b
    movb (%esi), %al
    movw $0x3F8, %dx
    outb %al, %dx

    incl %esi
    jmp 1b

3:  movb $'\n', %al
    movw $0x3F8, %dx
    outb %al, %dx
4:  cli
    hlt
    jmp 4b
.size _start, . - _start

.code64
long_mode_entry:
    /* Reload the data segments. In long mode these are mostly ignored, but
     * they must hold a valid selector rather than whatever GRUB left. */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp
    xorq %rbp, %rbp                   /* terminate any stack walk cleanly */

    /* System V puts the first two arguments in rdi and rsi.  Both values are
     * 32-bit physical addresses, and a 32-bit move zero-extends. */
    movl saved_magic, %edi
    movl saved_info, %esi
    call kmain

.global halt_forever
halt_forever:
1:  cli
    hlt
    jmp 1b
