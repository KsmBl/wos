/* Paging: per-process address spaces over a shared kernel mapping.
 *
 * Every address space identity maps the low 16 MiB as supervisor-only, so the
 * kernel keeps working no matter which process is current, and kernel pointers
 * survive a context switch.  User pages live at 0x40000000 and above.
 *
 * Because the kernel is mapped everywhere, loading a program is done by
 * switching to the new address space first and then writing through ordinary
 * user virtual addresses -- no temporary mapping window is needed.
 */
#ifndef WOS_PAGING_H
#define WOS_PAGING_H

#include "types.h"

/* Page table / directory entry flags. */
#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_USER    0x004

/* Number of page directory entries covering the shared kernel identity map:
 * each entry spans 4 MiB, so four of them cover the low 16 MiB. */
#define KERNEL_PDE_COUNT 4

/* Where user programs are loaded and where their stacks start. */
#define USER_BASE        0x40000000u
#define USER_STACK_TOP   0xBFFF0000u
#define USER_STACK_SIZE  (64u * 1024u)

typedef struct addrspace {
    uint32_t *pd;             /* page directory; identity mapped, so virt==phys */
    uint32_t  user_frames;    /* frames mapped for user pages -- the RSS figure */
    uint32_t  table_frames;   /* frames spent on this space's own page tables   */
} addrspace_t;

void paging_init(void);

addrspace_t *paging_kernel_space(void);
addrspace_t *paging_current(void);
void         paging_switch(addrspace_t *as);

/* Create an address space containing only the shared kernel mapping.
 * Returns NULL if memory is exhausted. */
addrspace_t *paging_new_addrspace(void);

/* Tear down an address space: unmap and free every user frame, the page
 * tables and the directory itself.  Must not be the current space. */
void paging_free_addrspace(addrspace_t *as);

/* Map one page. `flags` takes PTE_WRITE and PTE_USER; PTE_PRESENT is implied.
 * Returns false only if a page table could not be allocated. */
bool paging_map(addrspace_t *as, uint32_t virt, uint32_t phys, uint32_t flags);

/* Map one page onto a freshly allocated frame.  If `zero` is set the page is
 * cleared, which requires `as` to be the current address space. */
bool paging_map_alloc(addrspace_t *as, uint32_t virt, uint32_t flags, bool zero);

/* Unmap a page and release its frame. */
void paging_unmap(addrspace_t *as, uint32_t virt);

/* Physical address backing `virt`, or 0 if it is not mapped. */
uint32_t paging_translate(addrspace_t *as, uint32_t virt);

/* True if `virt` is mapped and reachable from ring 3 (and writable if asked).
 * Used to validate pointers that arrive from user space. */
bool paging_user_can_access(addrspace_t *as, uint32_t virt, bool need_write);

/* Resident bytes: user frames plus the page tables this space owns. */
uint32_t paging_user_bytes(const addrspace_t *as);

#endif /* WOS_PAGING_H */
