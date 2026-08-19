/* GDT and TSS setup for x86-64. */

#include "gdt.h"
#include "sysentry.h"
#include "smp.h"
#include "string.h"

/* Five 8-byte slots, the last two forming the 16-byte TSS descriptor. */
#define GDT_SLOTS 7

/* One of each per processor.  The table itself could have been shared -- the
 * four segments say the same thing on every core -- but the TSS in it cannot:
 * it holds rsp0, the stack this processor takes its next ring 3 interrupt on,
 * and two processors sharing that would take an interrupt onto the same stack
 * at the same moment. */
static uint64_t     gdt[MAX_CPUS][GDT_SLOTS];
static struct tss64 tss[MAX_CPUS];

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Build a code or data descriptor.  Base and limit are ignored in long mode,
 * so only the access and flag bits carry meaning. */
static uint64_t segment(uint8_t access, uint8_t flags)
{
    uint64_t d = 0;

    d |= (uint64_t)0xFFFF;                  /* limit 15:0, ignored  */
    d |= (uint64_t)0xF << 48;               /* limit 19:16, ignored */
    d |= (uint64_t)access << 40;
    d |= (uint64_t)(flags & 0xF0) << 48;

    return d;
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    tss[smp_cpu_index()].rsp0 = rsp0;

    /* The same stack, told to the other way in.  An interrupt gate takes it
     * from the TSS; SYSCALL does not switch stacks at all and has to be handed
     * it separately.  Both are set here so they cannot drift apart. */
    sysentry_set_kernel_stack(rsp0);
}

void gdt_init(void)
{
    gdt_init_cpu(0);
}

/* Build this processor's table and load it.  Every core runs this, the boot
 * one from gdt_init() and the rest from the trampoline's far side. */
void gdt_init_cpu(int cpu)
{
    /* access byte: P | DPL | S | type
     *   0x9A = present, ring 0, code, readable
     *   0x92 = present, ring 0, data, writable
     *   0xF2 = present, ring 3, data, writable
     *   0xFA = present, ring 3, code, readable
     * flags nibble 0xA = long mode (L) + 4 KiB granularity; the D bit must be
     * clear whenever L is set. */
    gdt[cpu][0] = 0;
    gdt[cpu][1] = segment(0x9A, 0xA0);           /* kernel code */
    gdt[cpu][2] = segment(0x92, 0xC0);           /* kernel data */
    gdt[cpu][3] = segment(0xF2, 0xC0);           /* user data   */
    gdt[cpu][4] = segment(0xFA, 0xA0);           /* user code   */

    memset(&tss[cpu], 0, sizeof(tss[cpu]));
    tss[cpu].rsp0 = 0;                           /* filled in per context switch */
    /* No I/O permission bitmap: pointing the base past the segment limit
     * tells the CPU that every port access from ring 3 must fault. */
    tss[cpu].iomap_base = sizeof(tss[cpu]);

    /* The TSS descriptor is 16 bytes and spans two slots. */
    uint64_t base  = (uint64_t)&tss[cpu];
    uint64_t limit = sizeof(tss[cpu]) - 1;

    gdt[cpu][5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | ((uint64_t)0x89 << 40)         /* present, 64-bit TSS available */
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[cpu][6] = (base >> 32) & 0xFFFFFFFF;

    struct gdt_pointer gdtp;

    gdtp.limit = (uint16_t)(sizeof(gdt[cpu]) - 1);
    gdtp.base  = (uint64_t)&gdt[cpu];

    /* CS cannot be loaded with a plain move, and long mode has no far jump
     * to an immediate.  Pushing the selector and return address and using a
     * far return is the usual way to reload it. */
    __asm__ volatile(
        "lgdt %0\n\t"
        "movw %w1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "pushq %q2\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        :
        : "m"(gdtp), "i"(SEL_KDATA), "i"((uint64_t)SEL_KCODE)
        : "rax", "memory");

    __asm__ volatile("ltr %w0" : : "r"((uint16_t)SEL_TSS));
}
