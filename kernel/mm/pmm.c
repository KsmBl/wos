/* Bitmap physical frame allocator. */

#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"
#include "fbcon.h"

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static uint64_t *bitmap;            /* one bit per frame */
static uint64_t  bitmap_frames;     /* number of frames the bitmap covers */
static uint64_t  bitmap_words;
static uint64_t  total_frames;    /* frames the bitmap covers, holes included */
static uint64_t  usable_frames;   /* of those, the ones that are really RAM   */
static uint64_t  hole_frames;     /* and the ones that are not: total - usable */
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

void pmm_release_range(uint64_t start, uint64_t end)
{
    uint64_t before = used_frames;

    for (uint64_t a = ALIGN_UP(start, PAGE_SIZE); a + PAGE_SIZE <= end;
         a += PAGE_SIZE)
        mark_free(a / PAGE_SIZE);

    uint64_t freed = before - used_frames;

    reserved_frames = reserved_frames > freed ? reserved_frames - freed : 0;
}

static void free_range(uint64_t start, uint64_t end)
{
    /* Only whole frames fully inside the region are usable. */
    for (uint64_t a = ALIGN_UP(start, PAGE_SIZE); a + PAGE_SIZE <= end;
         a += PAGE_SIZE)
        mark_free(a / PAGE_SIZE);
}

/* The top of the memory the machine reports, so the bitmap covers all of it.
 *
 * All of it means all of it: a PC puts a few gigabytes below the 4 GiB line and
 * everything else above it, so a machine with 8 GiB has more than half of its
 * memory up there.  Ignoring that -- which this did, by looking only at regions
 * whose address fits in 32 bits -- is how a machine with 8 GiB came to run in
 * 3.  Nothing below needs the addresses to be small: the bitmap is indexed by
 * frame number, and a frame is only ever touched through a mapping. */
static uint64_t highest_address(const struct multiboot_info *mbi)
{
    uint64_t highest = 0;

    if (mbi->flags & MB_FLAG_MMAP) {
        uintptr_t cur = mbi->mmap_addr;
        uintptr_t end = mbi->mmap_addr + mbi->mmap_length;
        while (cur < end) {
            const struct multiboot_mmap_entry *e =
                (const struct multiboot_mmap_entry *)cur;
            if (e->type == MB_MEMORY_AVAILABLE) {
                uint64_t top = e->addr + e->len;
                if (top > highest)
                    highest = top;
            }
            cur += e->size + sizeof(uint32_t);
        }
    }

    /* Fall back to the simple mem_upper figure if there was no map. */
    if (highest == 0 && (mbi->flags & MB_FLAG_MEM))
        highest = 0x100000u + mbi->mem_upper * 1024u;

    return highest;
}

/* True if [start, start+len) touches [lo, hi). */
static bool overlaps(uint64_t start, uint64_t len, uint64_t lo, uint64_t hi)
{
    return hi > lo && start < hi && lo < start + len;
}

/* The next address at or after `addr` that is clear of everything which must
 * survive being booted: the kernel, the modules, and the structures the
 * bootloader built to describe them.  Returns `addr` itself when it is already
 * clear.
 *
 * The boot information is the reason this exists.  GRUB builds it below the
 * kernel, out of the way; the UEFI loader allocates it from the firmware, which
 * puts it wherever it likes -- in practice directly above the filesystem image,
 * which is exactly where a bitmap placed "after the last module" would go.  The
 * bitmap would then overwrite the memory map it is about to read. */
static uint64_t clear_of_boot_data(const struct multiboot_info *mbi,
                                   uint64_t addr, uint64_t len)
{
    for (;;) {
        uint64_t moved = addr;

        if (overlaps(addr, len, (uint64_t)__kernel_start, (uint64_t)__kernel_end))
            moved = (uint64_t)__kernel_end;

        if (overlaps(addr, len, (uint64_t)mbi, (uint64_t)mbi + sizeof(*mbi)))
            moved = (uint64_t)mbi + sizeof(*mbi);

        if (mbi->flags & MB_FLAG_MMAP)
            if (overlaps(addr, len, mbi->mmap_addr,
                         (uint64_t)mbi->mmap_addr + mbi->mmap_length))
                moved = (uint64_t)mbi->mmap_addr + mbi->mmap_length;

        if (mbi->flags & MB_FLAG_MODS) {
            const struct multiboot_module *mods =
                (const struct multiboot_module *)(uintptr_t)mbi->mods_addr;
            uint64_t mods_bytes = (uint64_t)mbi->mods_count * sizeof(*mods);

            if (overlaps(addr, len, mbi->mods_addr, mbi->mods_addr + mods_bytes))
                moved = (uint64_t)mbi->mods_addr + mods_bytes;

            for (uint32_t i = 0; i < mbi->mods_count; i++)
                if (overlaps(addr, len, mods[i].mod_start, mods[i].mod_end))
                    moved = mods[i].mod_end;
        }

        if (moved == addr)
            return addr;

        /* Something moved it, so start again: the new position may run into
         * the next thing along. */
        addr = ALIGN_UP(moved, PAGE_SIZE);
    }
}

/* Somewhere to put `len` bytes of the kernel's own bookkeeping: inside one
 * region the firmware calls available, clear of everything already there, and
 * below the framebuffer window, which shadows the identity map above it.
 * Returns 0 when the machine has nowhere that fits. */
static uint64_t find_span(const struct multiboot_info *mbi, uint64_t len)
{
    uint64_t ceiling = FBCON_APERTURE;

    if (!(mbi->flags & MB_FLAG_MMAP))
        return clear_of_boot_data(mbi, ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE),
                                  len);

    uintptr_t cur = mbi->mmap_addr;
    uintptr_t end = mbi->mmap_addr + mbi->mmap_length;

    while (cur < end) {
        const struct multiboot_mmap_entry *e =
            (const struct multiboot_mmap_entry *)cur;
        cur += e->size + sizeof(uint32_t);

        if (e->type != MB_MEMORY_AVAILABLE || (e->addr >> 32))
            continue;

        uint64_t region_end = e->addr + e->len;
        if (region_end > ceiling)
            region_end = ceiling;

        /* Never below the kernel: the low megabyte and the space the kernel
         * occupies are spoken for. */
        uint64_t addr = e->addr;
        if (addr < (uint64_t)__kernel_end)
            addr = (uint64_t)__kernel_end;
        addr = ALIGN_UP(addr, PAGE_SIZE);

        /* Stepping past one obstruction can land on another, or push the span
         * out of this region entirely, in which case the region is finished. */
        while (addr + len <= region_end) {
            uint64_t clear = clear_of_boot_data(mbi, addr, len);
            if (clear == addr)
                return addr;
            addr = clear;
        }
    }

    return 0;
}

void pmm_init(const struct multiboot_info *mbi)
{
    uint64_t highest = highest_address(mbi);

    total_frames  = highest / PAGE_SIZE;
    bitmap_frames = total_frames;
    bitmap_words  = ALIGN_UP(total_frames, 64) / 64;

    /* The bitmap and the heap go together into the first gap large enough to
     * hold both, which is a search rather than an address because everything
     * around them was placed by the bootloader: the kernel image, a 64 MiB
     * filesystem module, and the block describing them.  Writing the bitmap
     * over any of those destroys what the rest of this function is about to
     * read, or what the shell is about to mount. */
    uint64_t bitmap_bytes = ALIGN_UP(bitmap_words * sizeof(uint64_t), PAGE_SIZE);
    uint64_t span = find_span(mbi, bitmap_bytes + KHEAP_SIZE);

    if (!span)
        panic("no room for a %s frame bitmap and a %s heap below %p",
              fmt_bytes(bitmap_bytes), fmt_bytes(KHEAP_SIZE),
              (void *)FBCON_APERTURE);

    bitmap = (uint64_t *)span;

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
            if (e->type == MB_MEMORY_AVAILABLE)
                free_range(e->addr, e->addr + e->len);
            cur += e->size + sizeof(uint32_t);
        }
    }

    /* What is left free at this point is the machine's memory; everything else
     * the bitmap covers is a hole between one bank and the next, or hardware
     * with an address but no RAM behind it.  A PC with 8 GiB spreads it either
     * side of a two-gigabyte hole below 4 GiB, and counting that hole as memory
     * in use would report a machine two thirds full at boot. */
    hole_frames   = used_frames;
    usable_frames = total_frames - hole_frames;

    /* Now take back everything that must never be handed out.  The heap follows
     * the bitmap, both inside the span found for them. */
    heap_base = (uint64_t)bitmap + bitmap_bytes;

    pmm_reserve_range(0, 0x100000);                     /* BIOS, VGA, IVT   */
    pmm_reserve_range((uint64_t)__kernel_start, (uint64_t)__kernel_end);
    pmm_reserve_range(span, heap_base + KHEAP_SIZE);

    /* The modules, and the block describing them.  GRUB leaves all of it below
     * 1 MiB, already reserved above; the UEFI loader gets it from the firmware,
     * which puts it in ordinary memory that would otherwise be handed straight
     * back out -- and the filesystem is still mounted from a module long after
     * this runs. */
    pmm_reserve_range((uint64_t)mbi, (uint64_t)mbi + sizeof(*mbi));

    if (mbi->flags & MB_FLAG_MMAP)
        pmm_reserve_range(mbi->mmap_addr, mbi->mmap_addr + mbi->mmap_length);

    if (mbi->flags & MB_FLAG_MODS) {
        const struct multiboot_module *mods =
            (const struct multiboot_module *)(uintptr_t)mbi->mods_addr;

        pmm_reserve_range(mbi->mods_addr,
                          mbi->mods_addr + mbi->mods_count * sizeof(*mods));

        for (uint32_t i = 0; i < mbi->mods_count; i++)
            pmm_reserve_range(mods[i].mod_start, mods[i].mod_end);
    }

    /* What the kernel took, which is not what is marked used: everything that
     * is not memory at all was marked used by the sweep above, and a machine
     * with two gigabytes of hardware addresses between its memory banks would
     * otherwise report the kernel as holding two gigabytes. */
    reserved_frames = used_frames - hole_frames;

    if (heap_base + KHEAP_SIZE > FBCON_APERTURE)
        panic("kernel heap does not fit below the framebuffer window");
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

/* A run of frames next to each other in physical memory, which is what a
 * device doing DMA needs: the adapter is handed one address and walks forward
 * from it, knowing nothing of page tables.
 *
 * The scan is deliberately simple -- walk from the bottom, and on hitting a
 * used frame start again after it.  Nothing here allocates often enough for
 * that to matter: rings and firmware buffers are claimed once when a driver
 * starts and held until it stops.
 *
 * `align_frames` is a power-of-two frame count the run must start on; the
 * adapter's rings must be aligned to their own size, which the bare allocator
 * has no way to promise. */
uint64_t pmm_alloc_contiguous(uint64_t count, uint64_t align_frames)
{
    uint64_t limit = LOW_MEMORY_LIMIT / PAGE_SIZE;

    if (!count)
        return 0;
    if (align_frames < 1)
        align_frames = 1;
    if (limit > total_frames)
        limit = total_frames;

    /* Frame 0 is never handed out -- a null return means failure, and an
     * allocation at physical zero would be indistinguishable from it. */
    for (uint64_t start = ALIGN_UP(1, align_frames);
         start + count <= limit;
         start = ALIGN_UP(start + 1, align_frames)) {
        uint64_t i;

        for (i = 0; i < count; i++)
            if (bitmap_test(start + i))
                break;

        if (i < count) {
            /* Resume past the frame that blocked us rather than one frame
             * on, which turns a long occupied stretch into one step. */
            start = start + i;
            continue;
        }

        for (i = 0; i < count; i++)
            bitmap_set(start + i);
        used_frames += count;

        return start * PAGE_SIZE;
    }

    return 0;
}

void pmm_free_contiguous(uint64_t phys, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
        pmm_free_frame(phys + i * PAGE_SIZE);
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

/* The figures are about memory, not about addresses: `total` is the RAM the
 * machine reported, and used + free adds up to it. */
uint64_t pmm_total_bytes(void)  { return usable_frames * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)   { return (total_frames - used_frames) * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)   { return pmm_total_bytes() - pmm_free_bytes(); }
uint64_t pmm_kernel_bytes(void) { return reserved_frames * PAGE_SIZE; }
uint64_t pmm_heap_base(void)    { return heap_base; }
