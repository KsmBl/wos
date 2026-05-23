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
