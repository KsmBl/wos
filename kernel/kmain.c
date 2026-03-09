/* WOS kernel entry point.
 *
 * Called from boot.S with the multiboot magic and info pointer that GRUB left
 * in EAX and EBX.  Brings up the subsystems in dependency order and then hands
 * the machine to the scheduler.
 */

#include "types.h"
#include "multiboot.h"
#include "kprintf.h"
#include "vga.h"
#include "serial.h"
#include "io.h"

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static const char *mmap_type_name(uint32_t type)
{
    switch (type) {
    case MB_MEMORY_AVAILABLE: return "available";
    case MB_MEMORY_RESERVED:  return "reserved";
    case MB_MEMORY_ACPI:      return "ACPI reclaimable";
    case MB_MEMORY_NVS:       return "ACPI NVS";
    case MB_MEMORY_BADRAM:    return "bad RAM";
    default:                  return "unknown";
    }
}

/* Dump the E820 memory map GRUB collected, and return the total usable bytes.
 * Regions above 4 GiB cannot be addressed by this 32-bit kernel and are
 * clamped away here rather than silently overflowing. */
static uint32_t print_memory_map(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MB_FLAG_MMAP)) {
        kprintf("  (no memory map provided by the bootloader)\n");
        return 0;
    }

    uint32_t usable = 0;
    uintptr_t cur = mbi->mmap_addr;
    uintptr_t end = mbi->mmap_addr + mbi->mmap_length;

    while (cur < end) {
        const struct multiboot_mmap_entry *e =
            (const struct multiboot_mmap_entry *)cur;

        /* Anything starting at or above 4 GiB is unreachable for us. */
        bool addressable = (e->addr >> 32) == 0;
        uint32_t base = addressable ? (uint32_t)e->addr : 0xFFFFFFFFu;
        uint32_t len;

        if (!addressable) {
            len = 0;
        } else if ((e->len >> 32) != 0 || base + (uint32_t)e->len < base) {
            len = 0xFFFFFFFFu - base;   /* clamp to the top of the address space */
        } else {
            len = (uint32_t)e->len;
        }

        kprintf("  %p - %p  %s\n", (void *)base, (void *)(base + len),
                mmap_type_name(e->type));

        if (e->type == MB_MEMORY_AVAILABLE)
            usable += len;

        cur += e->size + sizeof(uint32_t);   /* size excludes its own field */
    }

    return usable;
}

void kmain(uint32_t magic, struct multiboot_info *mbi)
{
    serial_init();
    vga_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("WOS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("a small operating system\n\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic %08x (expected %08x)",
              magic, MULTIBOOT_BOOTLOADER_MAGIC);

    uint32_t ksize = (uint32_t)(__kernel_end - __kernel_start);
    kprintf("kernel : %p - %p (%s)\n",
            (void *)__kernel_start, (void *)__kernel_end, fmt_bytes(ksize));

    if (mbi->flags & MB_FLAG_MEM)
        kprintf("memory : %s low, %s high\n",
                fmt_bytes(mbi->mem_lower * 1024u),
                fmt_bytes(mbi->mem_upper * 1024u));

    kputs("memory map:\n");
    uint32_t usable = print_memory_map(mbi);
    kprintf("usable : %s\n", fmt_bytes(usable));

    kputs("\nboot complete; nothing left to do yet.\n");

    for (;;)
        hlt();
}
