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

/* How much of low memory boot.S identity maps, and therefore the region the
 * kernel can always reach directly: page tables and the heap must live here. */
#define LOW_MEMORY_LIMIT (1024UL * 1024UL * 1024UL)

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
