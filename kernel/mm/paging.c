/* Four-level paging for x86-64. */

#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* Index into each level of the walk. */
#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

/* The physical address inside an entry, with the flag bits and the NX bit
 * masked away. */
#define FRAME_OF(e) ((e) & 0x000FFFFFFFFFF000UL)

/* Built by boot.S and adopted as the kernel's address space. */
extern uint64_t boot_pml4[];
extern uint64_t boot_pdpt[];
extern uint64_t boot_pd[];

static addrspace_t  kernel_space;
static addrspace_t *current_space;

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void load_cr3(uint64_t phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

static uint64_t *zeroed_low_frame(void)
{
    uint64_t phys = pmm_alloc_frame_low();
    if (!phys)
        return NULL;

    /* Low frames are identity mapped, so the physical address is usable
     * directly as a pointer. */
    uint64_t *p = (uint64_t *)phys;
    memset(p, 0, PAGE_SIZE);
    return p;
}

/* Walk one level down, optionally creating the table beneath `entry`.
 *
 * A parent entry has to carry the union of the permissions of everything
 * below it: the CPU ANDs the user and write bits across all four levels, so a
 * user page under a supervisor-only directory is unreachable. */
static uint64_t *next_level(uint64_t *table, uint64_t index, bool create,
                            uint64_t flags, addrspace_t *as)
{
    uint64_t entry = table[index];

    if (!(entry & PTE_PRESENT)) {
        if (!create)
            return NULL;

        uint64_t *fresh = zeroed_low_frame();
        if (!fresh)
            return NULL;

        table[index] = (uint64_t)fresh | PTE_PRESENT | PTE_WRITE
                     | (flags & PTE_USER);
        if (as)
            as->table_frames++;

        return fresh;
    }

    if ((flags & PTE_USER) && !(entry & PTE_USER))
        table[index] |= PTE_USER;

    return (uint64_t *)FRAME_OF(entry);
}

/* Find the page table holding `virt`, optionally creating the path to it. */
static uint64_t *walk_to_pt(addrspace_t *as, uint64_t virt, bool create,
                            uint64_t flags)
{
    uint64_t *pdpt = next_level(as->pml4, PML4_INDEX(virt), create, flags, as);
    if (!pdpt)
        return NULL;

    uint64_t *pd = next_level(pdpt, PDPT_INDEX(virt), create, flags, as);
    if (!pd)
        return NULL;

    /* The kernel's identity map uses 2 MiB pages, which end the walk one
     * level early.  Nothing should ever try to map a 4 KiB page inside it. */
    if (pd[PD_INDEX(virt)] & PTE_HUGE)
        return NULL;

    return next_level(pd, PD_INDEX(virt), create, flags, as);
}

bool paging_map(addrspace_t *as, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pt = walk_to_pt(as, virt, true, flags);
    if (!pt)
        return false;

    uint64_t index = PT_INDEX(virt);

    /* Replacing a live mapping would leak its frame; callers must unmap. */
    if (pt[index] & PTE_PRESENT)
        panic("paging_map: %p is already mapped in space %p",
              (void *)virt, (void *)as);

    pt[index] = FRAME_OF(phys) | (flags & (PTE_WRITE | PTE_USER)) | PTE_PRESENT;

    if (flags & PTE_USER)
        as->user_frames++;

    if (as == current_space)
        invlpg(virt);

    return true;
}

bool paging_map_alloc(addrspace_t *as, uint64_t virt, uint64_t flags, bool zero)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys)
        return false;

    if (!paging_map(as, virt, phys, flags)) {
        pmm_free_frame(phys);
        return false;
    }

    if (zero) {
        if (as != current_space)
            panic("paging_map_alloc: cannot zero %p in a non-current space",
                  (void *)virt);
        memset((void *)virt, 0, PAGE_SIZE);
    }

    return true;
}

void paging_unmap(addrspace_t *as, uint64_t virt)
{
    uint64_t *pt = walk_to_pt(as, virt, false, 0);
    if (!pt)
        return;

    uint64_t index = PT_INDEX(virt);
    uint64_t entry = pt[index];

    if (!(entry & PTE_PRESENT))
        return;

    pmm_free_frame(FRAME_OF(entry));
    pt[index] = 0;

    if (entry & PTE_USER)
        as->user_frames--;

    if (as == current_space)
        invlpg(virt);
}

uint64_t paging_translate(addrspace_t *as, uint64_t virt)
{
    uint64_t *pdpt = next_level(as->pml4, PML4_INDEX(virt), false, 0, NULL);
    if (!pdpt)
        return 0;

    uint64_t *pd = next_level(pdpt, PDPT_INDEX(virt), false, 0, NULL);
    if (!pd)
        return 0;

    uint64_t pde = pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT))
        return 0;

    /* A 2 MiB page ends the walk here. */
    if (pde & PTE_HUGE)
        return FRAME_OF(pde) | (virt & 0x1FFFFF);

    uint64_t *pt = (uint64_t *)FRAME_OF(pde);
    uint64_t  pte = pt[PT_INDEX(virt)];

    if (!(pte & PTE_PRESENT))
        return 0;

    return FRAME_OF(pte) | (virt & 0xFFF);
}

bool paging_user_can_access(addrspace_t *as, uint64_t virt, bool need_write)
{
    uint64_t entry = as->pml4[PML4_INDEX(virt)];
    if (!(entry & PTE_PRESENT) || !(entry & PTE_USER))
        return false;

    uint64_t *pdpt = (uint64_t *)FRAME_OF(entry);
    entry = pdpt[PDPT_INDEX(virt)];
    if (!(entry & PTE_PRESENT) || !(entry & PTE_USER))
        return false;

    uint64_t *pd = (uint64_t *)FRAME_OF(entry);
    entry = pd[PD_INDEX(virt)];
    if (!(entry & PTE_PRESENT) || !(entry & PTE_USER))
        return false;
    if (entry & PTE_HUGE)
        return !need_write || (entry & PTE_WRITE);

    uint64_t *pt = (uint64_t *)FRAME_OF(entry);
    entry = pt[PT_INDEX(virt)];
    if (!(entry & PTE_PRESENT) || !(entry & PTE_USER))
        return false;

    return !need_write || (entry & PTE_WRITE);
}

addrspace_t *paging_kernel_space(void) { return &kernel_space; }
addrspace_t *paging_current(void)      { return current_space; }

void paging_switch(addrspace_t *as)
{
    current_space = as;
    load_cr3((uint64_t)as->pml4);
}

addrspace_t *paging_new_addrspace(void)
{
    addrspace_t *as = kzalloc(sizeof(addrspace_t));
    if (!as)
        return NULL;

    as->pml4 = zeroed_low_frame();
    if (!as->pml4) {
        kfree(as);
        return NULL;
    }

    /* User space sits in the second gigabyte, which shares PML4 entry 0 with
     * the kernel's identity map.  So the process needs its own PDPT, whose
     * first entry points at the same page directory the kernel uses. */
    uint64_t *pdpt = zeroed_low_frame();
    if (!pdpt) {
        pmm_free_frame((uint64_t)as->pml4);
        kfree(as);
        return NULL;
    }

    /* The user bit is set on the PML4 entry because user pages live beneath
     * it; it stays clear on the entry covering the identity map, which is
     * what keeps kernel memory out of reach of ring 3. */
    as->pml4[0] = (uint64_t)pdpt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    pdpt[0]     = boot_pdpt[0];
    as->table_frames = 2;

    return as;
}

void paging_free_addrspace(addrspace_t *as)
{
    if (!as || as == &kernel_space)
        return;
    if (as == current_space)
        panic("paging_free_addrspace: refusing to free the current space");

    uint64_t *pdpt = (uint64_t *)FRAME_OF(as->pml4[0]);

    /* Entry 0 is the shared kernel identity map and is not ours to free. */
    for (uint64_t i = 1; i < 512; i++) {
        if (!(pdpt[i] & PTE_PRESENT))
            continue;

        uint64_t *pd = (uint64_t *)FRAME_OF(pdpt[i]);

        for (uint64_t j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_PRESENT) || (pd[j] & PTE_HUGE))
                continue;

            uint64_t *pt = (uint64_t *)FRAME_OF(pd[j]);

            for (uint64_t k = 0; k < 512; k++) {
                if (pt[k] & PTE_PRESENT) {
                    pmm_free_frame(FRAME_OF(pt[k]));
                    if (pt[k] & PTE_USER)
                        as->user_frames--;
                }
            }

            pmm_free_frame((uint64_t)pt);
            as->table_frames--;
        }

        pmm_free_frame((uint64_t)pd);
        as->table_frames--;
    }

    pmm_free_frame((uint64_t)pdpt);
    pmm_free_frame((uint64_t)as->pml4);
    kfree(as);
}

uint64_t paging_user_bytes(const addrspace_t *as)
{
    return (as->user_frames + as->table_frames) * PAGE_SIZE;
}

void paging_init(void)
{
    /* boot.S already built and loaded a working four-level table, so the
     * kernel address space is simply adopted rather than rebuilt. */
    kernel_space.pml4         = boot_pml4;
    kernel_space.user_frames  = 0;
    kernel_space.table_frames = 3;      /* PML4, PDPT and the identity PD */

    current_space = &kernel_space;

    /* Split the first 2 MiB huge page into 4 KiB pages so that page zero can
     * be left unmapped.  Without this a null dereference would quietly write
     * to physical address 0 instead of faulting, because a huge page covers
     * the whole region in one entry and cannot exclude part of it.  One page
     * table is a cheap price for keeping that bug loud. */
    uint64_t *pt = zeroed_low_frame();
    if (!pt)
        panic("paging_init: cannot allocate the guard page table");

    for (uint64_t i = 1; i < 512; i++)
        pt[i] = (i * PAGE_SIZE) | PTE_PRESENT | PTE_WRITE;

    boot_pd[0] = (uint64_t)pt | PTE_PRESENT | PTE_WRITE;
    kernel_space.table_frames++;

    /* Reload CR3 to flush the stale huge-page translation. */
    load_cr3((uint64_t)boot_pml4);
}
