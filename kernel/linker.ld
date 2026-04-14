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
     * find the header, so it leads the first section. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
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
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
