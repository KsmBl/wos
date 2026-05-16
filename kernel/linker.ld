/* WOS kernel linker script.
 *
 * The kernel is a flat, non-relocatable ELF64 binary loaded by GRUB at the
 * physical 1 MiB mark.  GRUB's Multiboot 1 loader accepts an ELF64 image and
 * enters it in 32-bit protected mode; boot.S makes the jump to long mode.
 *
 * Everything stays inside the first 1 GiB, which boot.S identity maps, so
 * virtual equals physical for the whole kernel and -mcmodel=small holds.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    /* .multiboot must be within the first 8 KiB of the file for GRUB to
     * find the header, so it leads the first section.
     *
     * The Linux boot protocol, which is how this kernel is loaded under UEFI,
     * fixes its 64-bit entry point at offset 0x200 of the image instead of
     * naming it, so .linux_entry is padded out to exactly there. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        /* Section-relative: the UEFI loader jumps to image + 0x200, a fixed
         * offset agreed with uefi/stub.c so the two builds need share no
         * symbols. */
        . = 0x200;
        *(.uefi_entry)
        *(.text)
        *(.text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
        /* The end of what a loader actually reads from the file, and so the
         * length linuxboot.S copies when it moves the image. */
        __data_end = .;
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
