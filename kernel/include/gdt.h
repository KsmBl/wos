/* Global Descriptor Table and Task State Segment, x86-64.
 *
 * Long mode almost does away with segmentation: the base and limit of a code
 * or data segment are ignored entirely.  What is left is the privilege level
 * and, for code, the L bit that says "this is 64-bit code".  So there are four
 * segments plus the TSS, and they exist only to carry those two facts.
 *
 *   0x00  null
 *   0x08  kernel code   (ring 0, L set)
 *   0x10  kernel data   (ring 0)
 *   0x18  user data     (ring 3, selector used as 0x1B with RPL 3)
 *   0x20  user code     (ring 3, L set, selector used as 0x23 with RPL 3)
 *   0x28  TSS           (16 bytes in long mode, so it takes two slots)
 *
 * The selector constants below must agree with that order.  Getting them out
 * of step is not a subtle bug: a CS naming the TSS descriptor faults on the
 * iretq into ring 3, with the TSS selector in the error code.
 */
#ifndef WOS_GDT_H
#define WOS_GDT_H

#include "types.h"

/* The selector values live in their own header so that sysentry.S can have
 * them too. */
#include "gdt_sel.h"

/* The 64-bit TSS.  Hardware task switching is gone in long mode; what remains
 * is rsp0, the stack the CPU switches to when an interrupt arrives while
 * running in ring 3, and the IST table for stacks that must always be valid. */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);

/* The same, for a processor other than the one that booted: each has its own
 * table because each needs its own TSS. */
void gdt_init_cpu(int cpu);

/* Point the TSS at the kernel stack to use for the next ring 3 -> ring 0
 * transition.  Called by the scheduler on every context switch. */
void tss_set_kernel_stack(uint64_t rsp0);

#endif /* WOS_GDT_H */
