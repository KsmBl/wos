/* Paging implementation. */

#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"

#define PD_INDEX(v) ((v) >> 22)
#define PT_INDEX(v) (((v) >> 12) & 0x3FF)
#define FRAME_OF(e) ((e) & ~0xFFFu)

static addrspace_t  kernel_space;
static addrspace_t *current_space;

static inline void invlpg(uint32_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void load_cr3(uint32_t phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

static uint32_t *zeroed_low_frame(void)
{
    uint32_t phys = pmm_alloc_frame_low();
    if (!phys)
        return NULL;

    /* Low frames are identity mapped, so the physical address is usable
     * directly even before (and after) paging is enabled. */
    uint32_t *p = (uint32_t *)phys;
    for (int i = 0; i < 1024; i++)
        p[i] = 0;
    return p;
}

/* Fetch the page table for `virt`, optionally creating it. */
static uint32_t *get_page_table(addrspace_t *as, uint32_t virt, bool create,
                                uint32_t flags)
{
    uint32_t pdi = PD_INDEX(virt);
    uint32_t pde = as->pd[pdi];

    if (!(pde & PTE_PRESENT)) {
        if (!create)
            return NULL;

        uint32_t *pt = zeroed_low_frame();
        if (!pt)
            return NULL;

        as->pd[pdi] = (uint32_t)pt | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
        as->table_frames++;
        return pt;
    }

    /* A page directory entry gates every page beneath it, so a user mapping
     * needs the user bit set at both levels. */
    if ((flags & PTE_USER) && !(pde & PTE_USER))
        as->pd[pdi] |= PTE_USER;

    return (uint32_t *)FRAME_OF(pde);
}

bool paging_map(addrspace_t *as, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t *pt = get_page_table(as, virt, true, flags);
    if (!pt)
        return false;

    uint32_t pti = PT_INDEX(virt);

    /* Replacing a live mapping would leak its frame; callers must unmap. */
    if (pt[pti] & PTE_PRESENT)
        panic("paging_map: %p is already mapped in space %p",
              (void *)virt, (void *)as);

    pt[pti] = FRAME_OF(phys) | (flags & (PTE_WRITE | PTE_USER)) | PTE_PRESENT;

    if (flags & PTE_USER)
        as->user_frames++;

    if (as == current_space)
        invlpg(virt);

    return true;
}

bool paging_map_alloc(addrspace_t *as, uint32_t virt, uint32_t flags, bool zero)
{
    uint32_t phys = pmm_alloc_frame();
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
        uint8_t *p = (uint8_t *)virt;
        for (uint32_t i = 0; i < PAGE_SIZE; i++)
            p[i] = 0;
    }

    return true;
}

void paging_unmap(addrspace_t *as, uint32_t virt)
{
    uint32_t *pt = get_page_table(as, virt, false, 0);
    if (!pt)
        return;

    uint32_t pti = PT_INDEX(virt);
    uint32_t pte = pt[pti];
    if (!(pte & PTE_PRESENT))
        return;

    pmm_free_frame(FRAME_OF(pte));
    pt[pti] = 0;

    if (pte & PTE_USER)
        as->user_frames--;

    if (as == current_space)
        invlpg(virt);
}

uint32_t paging_translate(addrspace_t *as, uint32_t virt)
{
    uint32_t *pt = get_page_table(as, virt, false, 0);
    if (!pt)
        return 0;

    uint32_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PTE_PRESENT))
        return 0;

    return FRAME_OF(pte) | (virt & 0xFFF);
}

bool paging_user_can_access(addrspace_t *as, uint32_t virt, bool need_write)
{
    uint32_t pde = as->pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT) || !(pde & PTE_USER))
        return false;

    uint32_t *pt  = (uint32_t *)FRAME_OF(pde);
    uint32_t  pte = pt[PT_INDEX(virt)];

    if (!(pte & PTE_PRESENT) || !(pte & PTE_USER))
        return false;

    return !need_write || (pte & PTE_WRITE);
}

addrspace_t *paging_kernel_space(void) { return &kernel_space; }
addrspace_t *paging_current(void)      { return current_space; }

void paging_switch(addrspace_t *as)
{
    current_space = as;
    load_cr3((uint32_t)as->pd);
}

addrspace_t *paging_new_addrspace(void)
{
    addrspace_t *as = kzalloc(sizeof(addrspace_t));
    if (!as)
        return NULL;

    as->pd = zeroed_low_frame();
    if (!as->pd) {
        kfree(as);
        return NULL;
    }

    /* Share the kernel's page tables rather than copying them, so a mapping
     * the kernel makes later is visible in every address space at once. */
    for (uint32_t i = 0; i < KERNEL_PDE_COUNT; i++)
        as->pd[i] = kernel_space.pd[i];

    return as;
}

void paging_free_addrspace(addrspace_t *as)
{
    if (!as || as == &kernel_space)
        return;
    if (as == current_space)
        panic("paging_free_addrspace: refusing to free the current space");

    for (uint32_t pdi = KERNEL_PDE_COUNT; pdi < 1024; pdi++) {
        uint32_t pde = as->pd[pdi];
        if (!(pde & PTE_PRESENT))
            continue;

        uint32_t *pt = (uint32_t *)FRAME_OF(pde);
        for (uint32_t pti = 0; pti < 1024; pti++) {
            if (pt[pti] & PTE_PRESENT) {
                pmm_free_frame(FRAME_OF(pt[pti]));
                if (pt[pti] & PTE_USER)
                    as->user_frames--;
            }
        }

        pmm_free_frame((uint32_t)pt);
        as->table_frames--;
        as->pd[pdi] = 0;
    }

    pmm_free_frame((uint32_t)as->pd);
    kfree(as);
}

uint32_t paging_user_bytes(const addrspace_t *as)
{
    return (as->user_frames + as->table_frames) * PAGE_SIZE;
}

void paging_init(void)
{
    kernel_space.pd = zeroed_low_frame();
    if (!kernel_space.pd)
        panic("paging_init: cannot allocate the kernel page directory");

    /* Identity map the low 16 MiB, supervisor-only and writable.  This covers
     * the kernel image, the frame bitmap, the heap arena, the VGA text buffer
     * and every page table we will ever allocate. */
    for (uint32_t pdi = 0; pdi < KERNEL_PDE_COUNT; pdi++) {
        uint32_t *pt = zeroed_low_frame();
        if (!pt)
            panic("paging_init: cannot allocate kernel page tables");

        for (uint32_t pti = 0; pti < 1024; pti++) {
            uint32_t phys = (pdi << 22) | (pti << 12);
            pt[pti] = phys | PTE_PRESENT | PTE_WRITE;
        }

        kernel_space.pd[pdi] = (uint32_t)pt | PTE_PRESENT | PTE_WRITE;
        kernel_space.table_frames++;
    }

    /* Leave the null page unmapped so a null dereference faults instead of
     * quietly reading the interrupt vector table. */
    uint32_t *pt0 = (uint32_t *)FRAME_OF(kernel_space.pd[0]);
    pt0[0] = 0;

    paging_switch(&kernel_space);

    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u;             /* PG (paging) | WP (honour read-only in ring 0) */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}
