/* GDT and TSS setup for x86-64. */

#include "gdt.h"
#include "string.h"

/* Five 8-byte slots, the last two forming the 16-byte TSS descriptor. */
#define GDT_SLOTS 7

static uint64_t     gdt[GDT_SLOTS];
static struct tss64 tss;

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_pointer gdtp;

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
    tss.rsp0 = rsp0;
}

void gdt_init(void)
{
    /* access byte: P | DPL | S | type
     *   0x9A = present, ring 0, code, readable
     *   0x92 = present, ring 0, data, writable
     *   0xF2 = present, ring 3, data, writable
     *   0xFA = present, ring 3, code, readable
     * flags nibble 0xA = long mode (L) + 4 KiB granularity; the D bit must be
     * clear whenever L is set. */
    gdt[0] = 0;
    gdt[1] = segment(0x9A, 0xA0);           /* kernel code */
    gdt[2] = segment(0x92, 0xC0);           /* kernel data */
    gdt[3] = segment(0xF2, 0xC0);           /* user data   */
    gdt[4] = segment(0xFA, 0xA0);           /* user code   */

    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = 0;                           /* filled in per context switch */
    /* No I/O permission bitmap: pointing the base past the segment limit
     * tells the CPU that every port access from ring 3 must fault. */
    tss.iomap_base = sizeof(tss);

    /* The TSS descriptor is 16 bytes and spans two slots. */
    uint64_t base  = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;

    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | ((uint64_t)0x89 << 40)         /* present, 64-bit TSS available */
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = (base >> 32) & 0xFFFFFFFF;

    gdtp.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtp.base  = (uint64_t)&gdt;

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
