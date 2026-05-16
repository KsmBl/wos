/* The kernel image, built into the UEFI loader, plus the relocation directory
 * the firmware insists on.
 *
 * Carrying the kernel inside BOOTX64.EFI means a UEFI boot needs one file on
 * the EFI system partition rather than two that could drift apart.  The
 * filesystem image is still a separate file: it is 64 MiB, and the loader
 * reads it from the same volume at boot.
 */

	.section .rodata
	.balign 16
	.global kernel_blob
kernel_blob:
	.incbin "build/kernel.bin"
	.global kernel_blob_end
kernel_blob_end:

/* A base relocation directory holding one no-op entry.  The stub is position
 * independent and has no relocations to apply, but a PE image without the
 * directory is one the firmware's loader will not load at all. */
	.section .reloc, "a"
	.long 0                           /* page RVA                        */
	.long 10                          /* block size, including this header */
	.word 0                           /* IMAGE_REL_BASED_ABSOLUTE: ignored */
