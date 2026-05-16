/* Link script for BOOTX64.EFI.
 *
 * It exists for one reason: .bss must not be a section of its own.
 *
 * The firmware reads the PE headers, allocates SizeOfImage bytes, and copies
 * each section into that allocation.  A .bss section has no contents to copy,
 * and the objcopy that turns this into a PE cannot carry it over -- so the
 * image ends where the initialised data ends, and every write to a
 * zero-initialised static lands past the end of the allocation, in memory the
 * firmware handed to somebody else.
 *
 * The damage is invisible on a machine whose memory happens to be free there
 * and fatal on one whose is not, at whatever point the firmware next touches
 * what was overwritten -- which is to say it looks like a loader that stops in
 * a different place every boot.
 *
 * Folding .bss into .data gives those variables room inside the image, at the
 * cost of carrying their zeroes in the file.  There are three of them.
 *
 * This is the same arrangement gnu-efi uses, for the same reason.
 */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
ENTRY(efi_main)

SECTIONS
{
    . = 0;

    /* -shared produces these whether they are wanted or not; keeping them at
     * the front leaves the sections that matter page aligned.  They are not
     * copied into the PE. */
    .hash   : { *(.hash) *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }

    . = ALIGN(4096);
    .text : { *(.text .text.*) }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    /* Everything writable, in one section with contents. */
    . = ALIGN(4096);
    .data : {
        *(.got .got.plt)
        *(.data .data.* .sdata)
        *(.bss .bss.* .dynbss .sbss)
        *(COMMON)
    }

    . = ALIGN(4096);
    .reloc : { *(.reloc) }

    .dynamic : { *(.dynamic) }

    /DISCARD/ : { *(.rela*) *(.eh_frame) *(.note*) *(.comment) }
}
