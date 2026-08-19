/* Physical memory manager: a bitmap frame allocator.
 *
 * One bit per 4 KiB frame, 0 = free, 1 = used.  This is the authority for the
 * system-wide RAM figures that applications read through wmeminfo(), so the
 * counters here are maintained exactly rather than estimated.
 */
#ifndef WOS_PMM_H
#define WOS_PMM_H

#include "types.h"
#include "multiboot.h"

/* Frames below this address are identity mapped into every address space, so
 * the kernel can always dereference them directly.  Page tables and the kernel
 * heap must live here; user pages need not. */
/* Defined in paging.h, which owns the identity-map size. */
#include "paging.h"

void pmm_init(const struct multiboot_info *mbi);

/* Allocate one frame. Returns its physical address, or 0 if memory is full.
 * The frame's contents are undefined. */
uint64_t pmm_alloc_frame(void);

/* Allocate one frame below LOW_MEMORY_LIMIT, for structures the kernel must
 * be able to touch through the identity map (page directories and tables). */
uint64_t pmm_alloc_frame_low(void);

/* Allocate `count` frames next to each other in physical memory, starting on
 * a multiple of `align_frames` frames, below LOW_MEMORY_LIMIT so the result is
 * reachable through the identity map.  Returns the physical address of the
 * first, or 0 if no such run is free.
 *
 * This is what a device doing DMA needs: it is given one address and walks
 * forward from it with no notion of page tables, so the frames behind that
 * address have to really be adjacent. */
uint64_t pmm_alloc_contiguous(uint64_t count, uint64_t align_frames);

/* Hand back a run from pmm_alloc_contiguous.  `count` must be what was asked
 * for -- the allocator keeps no record of run lengths. */
void pmm_free_contiguous(uint64_t phys, uint64_t count);

void pmm_free_frame(uint64_t phys);

/* Reserve an explicit physical range, e.g. a module the bootloader loaded. */
void pmm_reserve_range(uint64_t start, uint64_t end);

/* Hand a reserved range back.  Only whole frames wholly inside it are freed,
 * and the boot-time reservation figure comes down with them, so a region that
 * turned out not to be needed stops being counted as the kernel's. */
void pmm_release_range(uint64_t start, uint64_t end);

uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
uint64_t pmm_free_bytes(void);

/* Bytes that were already in use when the allocator finished initialising:
 * the kernel image, the frame bitmap, the heap arena and low memory. */
uint64_t pmm_kernel_bytes(void);

/* First address the heap may use; the bitmap ends just below it. */
uint64_t pmm_heap_base(void);

#endif /* WOS_PMM_H */
