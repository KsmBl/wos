/* Paging: per-process address spaces over a shared kernel mapping, x86-64.
 *
 * Long mode uses four levels: PML4 -> PDPT -> PD -> PT, nine address bits
 * each, with a 4 KiB page at the bottom.
 *
 * boot.S identity maps the first 1 GiB with 2 MiB pages, and every address
 * space keeps that mapping, supervisor-only.  So the kernel keeps working no
 * matter which process is current, and kernel pointers survive a context
 * switch.  User pages live at 0x40000000 and above, which is the second
 * gigabyte -- just past the identity map, sharing its PML4 entry but not its
 * page directory pointer entry.
 *
 * Because the kernel is mapped everywhere, loading a program is done by
 * switching to the new address space first and then writing through ordinary
 * user virtual addresses -- no temporary mapping window is needed.
 */
#ifndef WOS_PAGING_H
#define WOS_PAGING_H

#include "types.h"

/* Page table entry flags. */
#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004
#define PTE_HUGE    0x080
#define PTE_NX      (1UL << 63)

/* Write-combining, for a device aperture that is written and never read.
 *
 * Only valid on a huge page, where bit 12 is the PAT bit -- in a 4 KiB entry
 * the same bit is the bottom of the physical address, and setting it there
 * would move the mapping rather than change its caching.  Means nothing until
 * paging_enable_write_combining() has redefined the entry it selects. */
#define PTE_WC      (1UL << 12)

/* Uncached, for device registers: a write to one has to reach the device in
 * the order it was written, and a read has to come from the device rather than
 * from whatever the last read left in the cache. */
#define PTE_UNCACHED 0x010

/* How much of low memory boot.S identity maps, and therefore the region the
 * kernel can always reach directly: page tables and the heap must live here. */
#define LOW_MEMORY_LIMIT (1024UL * 1024UL * 1024UL)

/* Where device registers are mapped.
 *
 * The fourth gigabyte, which is the one part of the lower four that nothing
 * else claims: the kernel's identity map is the first, and user programs and
 * their stacks are the second and third.  Putting device registers here rather
 * than inside the identity map means they cost no memory -- a mapping in the
 * identity map hides the RAM at the same address, which then has to be kept
 * from the allocator.
 *
 * It lives in its own page directory pointer entry, which every address space
 * copies, so the kernel can reach a device no matter whose process is
 * current. */
#define DEVICE_WINDOW 0xC0000000UL

/* Where user programs are loaded and where their stacks start. */
#define USER_BASE        0x40000000UL
#define USER_STACK_TOP   0xBFFF0000UL
#define USER_STACK_SIZE  (64UL * 1024UL)

typedef struct addrspace {
    uint64_t *pml4;           /* identity mapped, so virtual == physical */
    uint64_t  user_frames;    /* frames mapped for user pages -- the RSS figure */
    uint64_t  table_frames;   /* frames spent on this space's own page tables   */
} addrspace_t;

void paging_init(void);

addrspace_t *paging_kernel_space(void);
addrspace_t *paging_current(void);
void         paging_switch(addrspace_t *as);

/* Create an address space containing only the shared kernel mapping.
 * Returns NULL if memory is exhausted. */
addrspace_t *paging_new_addrspace(void);

/* Tear down an address space: unmap and free every user frame, the page
 * tables and the top-level table itself.  Must not be the current space. */
void paging_free_addrspace(addrspace_t *as);

/* Map one page. `flags` takes PTE_WRITE and PTE_USER; PTE_PRESENT is implied.
 * Returns false only if a page table could not be allocated. */
bool paging_map(addrspace_t *as, uint64_t virt, uint64_t phys, uint64_t flags);

/* Map one page onto a freshly allocated frame.  If `zero` is set the page is
 * cleared, which requires `as` to be the current address space. */
bool paging_map_alloc(addrspace_t *as, uint64_t virt, uint64_t flags, bool zero);

/* Identity-map one 2 MiB huge page into the kernel space (for the framebuffer
 * aperture above the boot identity map).  `virt`/`phys` must be 2 MiB aligned. */
bool paging_map_huge(uint64_t virt, uint64_t phys, uint64_t flags);

/* The same, usable before paging_init() -- and so before the frame allocator
 * exists -- by writing straight into the boot page tables, which paging_init()
 * later adopts as the kernel's own.  It is how the console reaches a
 * firmware-provided framebuffer early enough to show the boot itself, on a
 * machine where there is no other console to fall back to.  Page directories
 * come from a small static pool, so only a few calls are possible. */
bool paging_map_huge_early(uint64_t virt, uint64_t phys, uint64_t flags);

/* Map `size` bytes of device registers at physical `phys` into the device
 * window and return a pointer to them, or NULL if the window is full.  The
 * mapping is uncached and never released. */
void *paging_map_device(uint64_t phys, uint64_t size);

/* Teach the CPU that PTE_WC means write-combining, by pointing one PAT entry at
 * it.  Returns false on a CPU with no PAT, where PTE_WC has to be left alone.
 * Safe to call before paging_init(); calling it twice does nothing. */
bool paging_enable_write_combining(void);

/* False until paging_init() has run. */
bool paging_ready(void);

/* Unmap a page and release its frame. */
void paging_unmap(addrspace_t *as, uint64_t virt);

/* Physical address backing `virt`, or 0 if it is not mapped. */
uint64_t paging_translate(addrspace_t *as, uint64_t virt);

/* True if `virt` is mapped and reachable from ring 3 (and writable if asked).
 * Used to validate pointers that arrive from user space. */
bool paging_user_can_access(addrspace_t *as, uint64_t virt, bool need_write);

/* Resident bytes: user frames plus the page tables this space owns. */
uint64_t paging_user_bytes(const addrspace_t *as);

#endif /* WOS_PAGING_H */
