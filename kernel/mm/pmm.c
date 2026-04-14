/* Bitmap physical frame allocator. */

#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static uint64_t *bitmap;            /* one bit per frame */
static uint64_t  bitmap_frames;     /* number of frames the bitmap covers */
static uint64_t  bitmap_words;
static uint64_t  total_frames;
static uint64_t  used_frames;
static uint64_t  reserved_frames;   /* used_frames once init finished */
static uint64_t  heap_base;

/* Search hints: allocations almost always succeed near where the last one did,
 * so restarting the scan from zero every time is pure waste. */
static uint64_t  next_hint;
static uint64_t  next_hint_low;

static inline void bitmap_set(uint64_t frame)
{
    bitmap[frame / 64] |= (1UL << (frame % 64));
}

static inline void bitmap_clear(uint64_t frame)
{
    bitmap[frame / 64] &= ~(1UL << (frame % 64));
}

static inline bool bitmap_test(uint64_t frame)
{
    return (bitmap[frame / 64] & (1UL << (frame % 64))) != 0;
}

static void mark_used(uint64_t frame)
{
    if (frame >= total_frames || bitmap_test(frame))
        return;
    bitmap_set(frame);
    used_frames++;
}

static void mark_free(uint64_t frame)
{
    if (frame >= total_frames || !bitmap_test(frame))
        return;
    bitmap_clear(frame);
    used_frames--;
}

void pmm_reserve_range(uint64_t start, uint64_t end)
{
    for (uint64_t a = ALIGN_DOWN(start, PAGE_SIZE); a < end; a += PAGE_SIZE)
        mark_used(a / PAGE_SIZE);
}

static void free_range(uint64_t start, uint64_t end)
{
    /* Only whole frames fully inside the region are usable. */
    for (uint64_t a = ALIGN_UP(start, PAGE_SIZE); a + PAGE_SIZE <= end;
         a += PAGE_SIZE)
        mark_free(a / PAGE_SIZE);
}

/* Find the highest address the memory map reports, so the bitmap covers
 * everything the machine actually has. */
static uint64_t highest_address(const struct multiboot_info *mbi)
{
    uint64_t highest = 0;

    if (mbi->flags & MB_FLAG_MMAP) {
        uintptr_t cur = mbi->mmap_addr;
        uintptr_t end = mbi->mmap_addr + mbi->mmap_length;
        while (cur < end) {
            const struct multiboot_mmap_entry *e =
                (const struct multiboot_mmap_entry *)cur;
            if (e->type == MB_MEMORY_AVAILABLE && (e->addr >> 32) == 0) {
                uint64_t top = e->addr + e->len;
                uint32_t top32 = (top >> 32) ? 0xFFFFF000u : (uint32_t)top;
                if (top32 > highest)
                    highest = top32;
            }
            cur += e->size + sizeof(uint32_t);
        }
    }

    /* Fall back to the simple mem_upper figure if there was no map. */
    if (highest == 0 && (mbi->flags & MB_FLAG_MEM))
        highest = 0x100000u + mbi->mem_upper * 1024u;

    return highest;
}

void pmm_init(const struct multiboot_info *mbi)
{
    uint64_t highest = highest_address(mbi);

    total_frames  = highest / PAGE_SIZE;
    bitmap_frames = total_frames;
    bitmap_words  = ALIGN_UP(total_frames, 64) / 64;

    /* The bitmap lives immediately after the kernel image. */
    bitmap = (uint64_t *)ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE);

    /* Start with everything used, then punch out the regions the firmware
     * reported as available. Anything the map does not mention stays used,
     * which is the safe default for memory-mapped hardware. */
    for (uint64_t i = 0; i < bitmap_words; i++)
        bitmap[i] = ~0UL;
    used_frames = total_frames;

    if (mbi->flags & MB_FLAG_MMAP) {
        uintptr_t cur = mbi->mmap_addr;
        uintptr_t end = mbi->mmap_addr + mbi->mmap_length;
        while (cur < end) {
            const struct multiboot_mmap_entry *e =
                (const struct multiboot_mmap_entry *)cur;
            if (e->type == MB_MEMORY_AVAILABLE && (e->addr >> 32) == 0) {
                uint64_t top = e->addr + e->len;
                uint32_t top32 = (top >> 32) ? 0xFFFFF000u : (uint32_t)top;
                free_range((uint32_t)e->addr, top32);
            }
            cur += e->size + sizeof(uint32_t);
        }
    }

    /* Now take back everything that must never be handed out. */
    uint64_t bitmap_end = (uint64_t)bitmap + bitmap_words * sizeof(uint64_t);

    pmm_reserve_range(0, 0x100000);                     /* BIOS, VGA, IVT   */
    pmm_reserve_range((uint64_t)__kernel_start, bitmap_end);

    /* Carve the kernel heap arena out of the identity-mapped region. */
    heap_base = ALIGN_UP(bitmap_end, PAGE_SIZE);
    pmm_reserve_range(heap_base, heap_base + KHEAP_SIZE);

    reserved_frames = used_frames;

    if (heap_base + KHEAP_SIZE > LOW_MEMORY_LIMIT)
        panic("kernel heap does not fit in the identity-mapped region");
}

static uint64_t alloc_scan(uint64_t start_frame, uint64_t end_frame,
                           uint64_t *hint)
{
    uint64_t begin = (*hint >= start_frame && *hint < end_frame)
                       ? *hint : start_frame;

    /* Two passes: from the hint to the end, then from the start to the hint,
     * so a wrap does not miss frames freed behind us. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t from = (pass == 0) ? begin : start_frame;
        uint64_t to   = (pass == 0) ? end_frame : begin;

        for (uint64_t f = from; f < to; f++) {
            if (!bitmap_test(f)) {
                bitmap_set(f);
                used_frames++;
                *hint = f + 1;
                return f * PAGE_SIZE;
            }
        }
    }
    return 0;
}

uint64_t pmm_alloc_frame(void)
{
    return alloc_scan(0, total_frames, &next_hint);
}

uint64_t pmm_alloc_frame_low(void)
{
    uint64_t limit = LOW_MEMORY_LIMIT / PAGE_SIZE;
    if (limit > total_frames)
        limit = total_frames;
    return alloc_scan(0, limit, &next_hint_low);
}

void pmm_free_frame(uint64_t phys)
{
    uint64_t frame = phys / PAGE_SIZE;

    if (frame >= total_frames)
        return;
    if (!bitmap_test(frame))
        panic("pmm: double free of frame at %p", (void *)phys);

    bitmap_clear(frame);
    used_frames--;

    /* Freed frames are good candidates for the next allocation. */
    if (frame < next_hint)
        next_hint = frame;
    if (phys < LOW_MEMORY_LIMIT && frame < next_hint_low)
        next_hint_low = frame;
}

uint64_t pmm_total_bytes(void)  { return total_frames * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)   { return used_frames * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)   { return (total_frames - used_frames) * PAGE_SIZE; }
uint64_t pmm_kernel_bytes(void) { return reserved_frames * PAGE_SIZE; }
uint64_t pmm_heap_base(void)    { return heap_base; }
