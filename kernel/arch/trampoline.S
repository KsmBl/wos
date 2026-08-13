/* Where another processor wakes up.
 *
 * A processor that has just been sent a startup interrupt begins in **real
 * mode**, sixteen bits wide, with paging off and nothing set up -- the same
 * state the boot processor was in when the firmware handed it to GRUB, and a
 * long way from the machine the rest of this kernel is written for.  So this
 * is boot.S again, in miniature: a GDT, protected mode, PAE, the long mode
 * enable bit, paging, and a far jump into 64-bit code.
 *
 * It differs from boot.S in the one way that matters: it does not build page
 * tables.  The boot processor already did, and every processor here shares
 * them, so this loads the CR3 that is already in use and the kernel is mapped
 * the instant paging comes on.
 *
 * The startup interrupt carries a page number rather than an address, so this
 * has to be copied into the first megabyte on a page boundary before it can be
 * used.  It is written to run from wherever TRAMPOLINE_BASE says, and every
 * address inside it is computed against that -- there is no relocation here
 * and nothing to do it.
 */

#define TRAMPOLINE_BASE 0x8000
#define AT(label)       ((label) - trampoline_start + TRAMPOLINE_BASE)

.section .rodata
.align 4096
.code16
.global trampoline_start
trampoline_start:
    cli
    cld

    /* Segments to zero, so every address is the linear address it looks
     * like.  A processor started this way has no promises about them. */
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    lgdtl   AT(gdt_pointer)

    /* Protected mode, and a far jump to load a 32-bit code segment: the
     * processor keeps decoding as 16-bit until CS says otherwise. */
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    ljmpl   $0x08, $AT(protected_mode)

.code32
protected_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* The boot processor's page tables, whole and already populated. */
    movl    AT(trampoline_cr3), %eax
    movl    %eax, %cr3

    /* PAE, which four-level paging requires. */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    /* EFER.LME: long mode enabled, though not yet entered. */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $(1 << 8), %eax
    wrmsr

    /* Paging on.  The processor is now in compatibility mode -- 64-bit
     * paging, 32-bit code -- and one far jump from where it should be. */
    movl    %cr0, %eax
    orl     $(1 << 31), %eax
    movl    %eax, %cr0

    ljmpl   $0x18, $AT(long_mode)

.code64
long_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    /* A stack of this processor's own, allocated by the one that started it.
     * Nothing before this point touched memory, which is why there did not
     * have to be one. */
    movq    AT(trampoline_stack), %rsp
    xorq    %rbp, %rbp

    /* And into C, at the kernel's own address: the identity map covers it,
     * so the same address works from here as from anywhere else. */
    movq    AT(trampoline_entry), %rax
    callq   *%rax

    /* smp_ap_entry() does not return.  If it ever did, stopping is the only
     * honest thing left to do. */
1:  cli
    hlt
    jmp     1b

/* The descriptor tables this needs before the kernel's own can be loaded.
 * Three entries: 32-bit code to leave real mode with, data, and 64-bit code to
 * enter long mode with. */
.align 8
gdt:
    .quad 0x0000000000000000        /* null */
    .quad 0x00CF9A000000FFFF        /* 0x08: 32-bit code, ring 0 */
    .quad 0x00CF92000000FFFF        /* 0x10: data, ring 0 */
    .quad 0x00AF9A000000FFFF        /* 0x18: 64-bit code (L set) */
gdt_end:

.align 8
gdt_pointer:
    .word gdt_end - gdt - 1
    .long AT(gdt)

/* Filled in by smp.c before each processor is started.  They are read from the
 * copy in low memory, so it is the copy that gets patched. */
.align 8
.global trampoline_cr3, trampoline_stack, trampoline_entry
trampoline_cr3:
    .quad 0
trampoline_stack:
    .quad 0
trampoline_entry:
    .quad 0

.global trampoline_end
trampoline_end:
