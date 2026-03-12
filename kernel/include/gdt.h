/* Global Descriptor Table and Task State Segment.
 *
 * WOS uses flat segmentation: every segment spans the whole 4 GiB address
 * space, and protection comes from paging instead.  Segments exist only to
 * carry the privilege level, so there are exactly four of them plus the TSS.
 *
 *   0x00  null
 *   0x08  kernel code   (ring 0)
 *   0x10  kernel data   (ring 0)
 *   0x18  user code     (ring 3, selector used as 0x1B with RPL 3)
 *   0x20  user data     (ring 3, selector used as 0x23 with RPL 3)
 *   0x28  TSS
 */
#ifndef WOS_GDT_H
#define WOS_GDT_H

#include "types.h"

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B      /* 0x18 | RPL 3 */
#define SEL_UDATA 0x23      /* 0x20 | RPL 3 */
#define SEL_TSS   0x28

/* The 32-bit Task State Segment.  WOS does not use hardware task switching;
 * the TSS exists purely so the CPU knows which kernel stack to switch to when
 * an interrupt arrives while running in ring 3. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;          /* kernel stack pointer loaded on ring 3 -> 0 */
    uint32_t ss0;           /* kernel stack segment                       */
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);

/* Point the TSS at the kernel stack to use for the next ring 3 -> ring 0
 * transition.  Called by the scheduler on every context switch. */
void tss_set_kernel_stack(uint32_t esp0);

#endif /* WOS_GDT_H */
