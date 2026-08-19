/* Framebuffer text console.
 *
 * Once the machine has paging and PCI, the console moves off VGA text mode onto
 * a linear framebuffer (QEMU's Bochs VBE display) and renders the 8x16 font
 * itself.  That buys crisp text at real resolutions -- 80x25 is 640x400,
 * 160x50 is 1280x800 -- instead of the blocky, stretched high-density text
 * modes, and lets the grid be any size the screen allows.
 */
#ifndef WOS_FBCON_H
#define WOS_FBCON_H

#include "types.h"
#include "multiboot.h"

/* Where the framebuffer is mapped, and how much room the mapping takes.
 *
 * The window lives inside the identity map, so it shadows whatever physical
 * memory sits at the same address on a machine with more than 768 MiB: memory
 * up here can be mapped, but it cannot be read back.  Everything the kernel
 * keeps for itself has to stay below it, which is why the frame allocator (and
 * uefi/stub.c, at the other end of the boot) both know this address.  */
#define FBCON_APERTURE      0x30000000UL
#define FBCON_APERTURE_SIZE (16UL * 1024 * 1024)   /* 1920x1200x32 needs 9 MiB */

/* Bring up QEMU's Bochs VBE display and switch the console onto it.  Returns
 * false and leaves VGA text mode in place if there is no usable display. */
bool fbcon_init(int cols, int rows);

/* Same, for the linear framebuffer the bootloader set up and described in the
 * Multiboot info.  This is the path real hardware takes -- there is no Bochs
 * VBE there, and after a UEFI boot no VGA text mode either.  The resolution is
 * fixed at whatever GRUB chose, so fbcon_set_mode() can only change the size
 * of the character grid drawn inside it. */
bool fbcon_init_boot(const struct multiboot_info *mbi, int cols, int rows);

/* The framebuffer size in pixels, 0 if the console is not on one. */
void fbcon_resolution(int *w, int *h);

/* Keep the frame allocator off the physical addresses the aperture window
 * shadows.  Call once the allocator exists: fbcon_init_boot() runs before it
 * does, so the mapping and the reservation cannot happen together. */
void fbcon_reserve_aperture(void);

/* True once the framebuffer console is the active output. */
bool fbcon_active(void);

/* Write one character, interpreting the same ANSI escapes the VGA console
 * does. */
void fbcon_putc(char c);

/* Resize the grid to `cols` x `rows`, changing the framebuffer resolution to
 * match (cols*8 by rows*16).  Returns 0, or -1 if it will not fit. */
int  fbcon_set_mode(int cols, int rows);

/* Report the current grid size. */
void fbcon_size(int *cols, int *rows);

/* ------------------------------------------------------------------ *
 *  Lending the screen out
 *
 *  A compositor and a text console cannot both own the framebuffer.  These are
 *  how the console gives it up and takes it back; who is allowed to ask, and
 *  who has it at the moment, is display.c's business rather than the console's.
 * ------------------------------------------------------------------ */

/* The framebuffer in pixels, and its stride in pixels (which is not always the
 * width).  False when the console is not on a framebuffer at all, which is
 * what a machine in VGA text mode reports. */
bool fbcon_geometry(int *w, int *h, uint32_t *stride_px);

/* Stop drawing to the glass, without stopping.  Everything written while
 * suspended still goes into the backing store, so the console does not lose
 * the output of a program that carried on printing behind the compositor. */
void fbcon_suspend(void);

/* Take the screen back and repaint every cell of it. */
void fbcon_resume(void);

bool fbcon_suspended(void);

/* Copy a rectangle of 32-bit pixels onto the screen.  `src_stride_px` is the
 * source's row length in pixels, which lets a caller blit part of a larger
 * image.  The rectangle is clipped to the screen here, because it arrives from
 * a process and a compositor's arithmetic mistake must not become a write past
 * the aperture. */
void fbcon_blit(const uint32_t *src, uint32_t src_stride_px,
                int x, int y, int w, int h);

/* True when the console scrolls by moving the card's view of a taller
 * framebuffer rather than by repainting.  Reported at boot because the
 * difference is large and silent otherwise. */
bool fbcon_panning(void);

#endif /* WOS_FBCON_H */
