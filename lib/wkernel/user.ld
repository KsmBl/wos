/* Linker script for WOS applications, x86-64.
 *
 * Programs are statically linked ELF64 ET_EXEC binaries based at 0x40000000,
 * which
 * is where the kernel's ELF loader expects user code to live.  Everything
 * below that belongs to the kernel's identity map, and the region above
 * 0xBFFF0000 is reserved for the user stack.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : ALIGN(4K)
    {
        *(.text.start)      /* crt0 first, so _start leads the image */
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(4K);
    __end = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
