/* WOS kernel linker script.
 *
 * The kernel is a flat, non-relocatable ELF32 binary loaded by GRUB at the
 * physical 1 MiB mark.  The low 16 MiB of physical memory is identity mapped
 * into every address space, so virtual == physical for everything here.
 */

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
