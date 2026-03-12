/* GDT and TSS setup. */

#include "gdt.h"

#define GDT_ENTRIES 6

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   /* high nibble = flags, low nibble = limit 19:16 */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;
static struct tss_entry tss;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt[i].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[i].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].access      = access;
    gdt[i].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

void gdt_init(void)
{
    /* access byte: P | DPL | S | type
     *   0x9A = present, ring 0, code, readable
     *   0x92 = present, ring 0, data, writable
     *   0xFA = present, ring 3, code, readable
     *   0xF2 = present, ring 3, data, writable
     *   0x89 = present, ring 0, 32-bit TSS (available)
     * flags nibble 0xC = 4 KiB granularity + 32-bit
     */
    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);

    /* The TSS descriptor uses a byte-granular limit covering the struct. */
    for (size_t i = 0; i < sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;
    tss.ss0 = SEL_KDATA;
    tss.esp0 = 0;               /* filled in per context switch */
    /* No I/O permission bitmap: setting the base past the segment limit tells
     * the CPU that every port access from ring 3 must fault. */
    tss.iomap_base = sizeof(tss);
    gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdtp.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtp.base  = (uint32_t)&gdt;

    /* Load the GDT, reload the data segments, then far-jump to reload CS. */
    __asm__ volatile(
        "lgdt %0\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "ljmp %2, $1f\n"
        "1:\n\t"
        :
        : "m"(gdtp), "i"(SEL_KDATA), "i"(SEL_KCODE)
        : "eax", "memory");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)SEL_TSS));
}
