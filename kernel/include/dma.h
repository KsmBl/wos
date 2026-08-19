/* Buffers a device can read and write directly.
 *
 * The kernel heap is no good for this.  kmalloc returns 8-byte-aligned
 * pointers out of one arena, with no promise that two adjacent bytes of a
 * large allocation are adjacent in physical memory -- true today because the
 * arena happens to be one contiguous span, but not something a driver should
 * lean on -- and no way to ask for the page alignment that a descriptor ring
 * needs.  The RTL8139 gets away with kmalloc because its buffers are small
 * and its ring has no alignment requirement worth the name.  An adapter with
 * real descriptor rings does not get away with it.
 *
 * So DMA memory comes straight from the frame allocator, in runs of whole
 * pages below the identity map, where the virtual address a driver holds and
 * the physical address it programs into the device are the same number.  That
 * identity is the reason this is as thin as it is: there is no translation to
 * do, only an allocation with stronger promises than the heap makes.
 */
#ifndef WOS_DMA_H
#define WOS_DMA_H

#include "types.h"

typedef struct {
    void     *virt;      /* what the kernel dereferences                   */
    uint64_t  phys;      /* what the device is told; equal to virt today   */
    uint64_t  bytes;     /* rounded up to a whole number of pages          */
} dma_buffer_t;

/* Claim `bytes` of zeroed, page-aligned, physically contiguous memory.
 *
 * `align` is a byte alignment for the start of the buffer, rounded up to a
 * power-of-two number of pages; pass 0 or PAGE_SIZE for plain page alignment.
 * Descriptor rings are the reason it exists -- an adapter that keeps a ring
 * index in the low bits of a register requires the ring to begin on a
 * multiple of its own size.
 *
 * Returns a buffer whose `virt` is NULL if the memory could not be found. */
dma_buffer_t dma_alloc(uint64_t bytes, uint64_t align);

/* Release a buffer.  Safe on one whose `virt` is NULL, so an allocation
 * failure part-way through a driver's setup can be unwound by freeing
 * everything it might have taken. */
void dma_free(dma_buffer_t *buf);

/* Total DMA memory outstanding, for the memory report to account for it --
 * these pages are gone from the free figure and it should be possible to see
 * where they went. */
uint64_t dma_used_bytes(void);

#endif /* WOS_DMA_H */
