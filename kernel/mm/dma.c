/* DMA buffers. See dma.h. */

#include "dma.h"
#include "pmm.h"
#include "paging.h"
#include "string.h"

static uint64_t outstanding;

dma_buffer_t dma_alloc(uint64_t bytes, uint64_t align)
{
    dma_buffer_t buf = { NULL, 0, 0 };
    uint64_t     align_frames = 1;

    if (!bytes)
        return buf;

    /* Alignment is expressed in bytes because that is how a data sheet writes
     * it, but the allocator works in frames, so round up to whole pages and
     * then to a power of two -- an alignment that is not a power of two is
     * not something any device asks for, and rounding up only ever gives a
     * stricter buffer than was requested. */
    if (align > PAGE_SIZE) {
        uint64_t pages = ALIGN_UP(align, PAGE_SIZE) / PAGE_SIZE;

        while (align_frames < pages)
            align_frames *= 2;
    }

    uint64_t size   = ALIGN_UP(bytes, PAGE_SIZE);
    uint64_t frames = size / PAGE_SIZE;
    uint64_t phys   = pmm_alloc_contiguous(frames, align_frames);

    if (!phys)
        return buf;

    buf.virt  = (void *)(uintptr_t)phys;   /* identity mapped below 1 GiB */
    buf.phys  = phys;
    buf.bytes = size;

    /* Zero it here rather than leaving it to the caller.  A descriptor ring
     * read by the device before the driver has filled every entry must
     * contain zeroes and not whatever the last owner of these pages left, and
     * that is the kind of mistake that shows up as an adapter walking into
     * memory it was never pointed at. */
    memset(buf.virt, 0, size);

    outstanding += size;
    return buf;
}

void dma_free(dma_buffer_t *buf)
{
    if (!buf || !buf->virt)
        return;

    pmm_free_contiguous(buf->phys, buf->bytes / PAGE_SIZE);
    outstanding -= buf->bytes;

    buf->virt  = NULL;
    buf->phys  = 0;
    buf->bytes = 0;
}

uint64_t dma_used_bytes(void)
{
    return outstanding;
}
