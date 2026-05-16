/* The entry point WOS's UEFI loader jumps to.
 *
 * Under UEFI the kernel is started by uefi/stub.c rather than by GRUB, because
 * GRUB cannot start it there at all: every handover it offers ends in a page
 * fault inside GRUB, on firmware that maps its pages read-only.  The stub is a
 * UEFI application that loads this kernel to 1 MiB, finds the framebuffer and
 * the filesystem, shuts the firmware down and lands here.
 *
 * It arrives in a very different machine from the Multiboot entry in boot.S:
 * long mode is already on, with the firmware's paging and GDT, and the
 * Multiboot info block the rest of the kernel expects has already been built
 * -- RDI points at it.  So all that is left is to take ownership of the
 * machine: the kernel's own page tables, its own descriptors, its own stack.
 *
 * The entry sits at a fixed offset (0x200) into the image, which is how the
 * stub finds it without sharing a symbol table with the kernel build.
 */

.set MB_MAGIC, 0x2BADB002             /* what kmain checks for */

.section .uefi_entry, "ax"
.code64
.global uefi_entry
.type uefi_entry, @function
uefi_entry:
    jmp uefi_entry_body

.section .text
.code64
.type uefi_entry_body, @function
uefi_entry_body:
    cli
    cld

    /* The info block pointer is the only argument, and none of what follows
     * preserves RDI. */
    movq %rdi, %r15

    /* The stub copies the image as it is on disk and nothing more, so .bss is
     * whatever the firmware left there.  The page tables built below live in
     * it, so this comes first. */
    leaq __bss_start(%rip), %rdi
    leaq __kernel_end(%rip), %rcx
    subq %rdi, %rcx
    xorl %eax, %eax
    rep stosb

    /* Off the firmware's stack and onto the kernel's, before anything below
     * pushes to it.  On a machine with a couple of gigabytes the firmware's
     * stack is near the top of memory, well outside the first gigabyte the
     * tables built below identity map -- so the first push after CR3 is loaded
     * would fault, with no IDT to take the fault, and the machine would reset.
     *
     * This is why it is done here rather than after the descriptors are
     * loaded, where the Multiboot path does it: there GRUB's stack is in low
     * memory and stays mapped, and the bug cannot happen.  The .bss zeroing
     * above has to come first, because that stack is in .bss. */
    movq $stack_top, %rsp
    xorq %rbp, %rbp

    call build_page_tables64

    /* The firmware configured the machine to suit itself, and some of what it
     * may have turned on the kernel would not survive:
     *
     *   SMAP  faults the kernel for reading the user memory a system call was
     *         handed, which is most of what system calls do,
     *   SMEP  faults it for running a user page, which signal delivery does,
     *   UMIP  faults sgdt and friends,
     *   PKE   adds protection keys to page table entries that do not set any,
     *   CET   demands shadow stacks the kernel never builds.
     *
     * GRUB leaves all five off, so the Multiboot path has never met them and
     * the rest of the kernel is written as if they cannot be on.  Clearing them
     * here is what makes that true after a UEFI boot as well. */
    movq %cr4, %rax
    movq $((1 << 11) | (1 << 20) | (1 << 21) | (1 << 22) | (1 << 23)), %rcx
    notq %rcx
    andq %rcx, %rax
    movq %rax, %cr4

    /* A swap, not an enable: from the firmware's tables to the kernel's, which
     * identity map the same first gigabyte, so the next instruction is still
     * mapped where it is executing. */
    leaq boot_pml4(%rip), %rax
    movq %rax, %cr3

    lgdt gdt64_pointer(%rip)

    /* CS cannot be loaded by a jump in long mode; a far return does it. */
    leaq 1f(%rip), %rax
    pushq $0x08
    pushq %rax
    lretq

1:  movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movq %r15, %rsi
    movl $MB_MAGIC, %edi
    call kmain

2:  cli
    hlt
    jmp 2b

/* Identity map the first 1 GiB with 2 MiB pages -- the same mapping, in the
 * same tables, that build_page_tables sets up for the Multiboot path.  Written
 * again here because that one is .code32 and cannot be called from long
 * mode. */
.type build_page_tables64, @function
build_page_tables64:
    leaq boot_pdpt(%rip), %rax
    orq  $0x03, %rax                  /* present | writable */
    leaq boot_pml4(%rip), %rdi
    movq %rax, (%rdi)

    leaq boot_pd(%rip), %rax
    orq  $0x03, %rax
    leaq boot_pdpt(%rip), %rdi
    movq %rax, (%rdi)

    leaq boot_pd(%rip), %rdi
    xorq %rcx, %rcx
    movq $0x83, %rax                  /* present | writable | huge, address 0 */
1:
    movq %rax, (%rdi, %rcx, 8)
    addq $0x200000, %rax
    incq %rcx
    cmpq $512, %rcx
    jb 1b
    ret
