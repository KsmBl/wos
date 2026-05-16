/* Multiboot 1 structures, as passed by GRUB in EBX.
 * See the Multiboot 0.6.96 specification for field meanings.
 */
#ifndef WOS_MULTIBOOT_H
#define WOS_MULTIBOOT_H

#include "types.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MB_FLAG_MEM     (1 << 0)   /* mem_lower / mem_upper valid */
#define MB_FLAG_BOOTDEV (1 << 1)
#define MB_FLAG_CMDLINE (1 << 2)
#define MB_FLAG_MODS    (1 << 3)
#define MB_FLAG_MMAP    (1 << 6)   /* mmap_addr / mmap_length valid */
#define MB_FLAG_VBE     (1 << 11)
#define MB_FLAG_FRAMEBUFFER (1 << 12)  /* framebuffer_* valid */

/* Not Multiboot: WOS's own UEFI loader sets this to say it filled in `rsdp`.
 * The firmware hands the ACPI tables to a UEFI application through its
 * configuration table and leaves nothing to find in low memory, so a UEFI boot
 * cannot locate them the way a BIOS boot does.  GRUB never sets this bit, and
 * the specification leaves the high bits free. */
#define MB_FLAG_WOS_RSDP (1u << 31)

/* Values for multiboot_info.framebuffer_type */
#define MB_FRAMEBUFFER_INDEXED 0
#define MB_FRAMEBUFFER_RGB     1
#define MB_FRAMEBUFFER_EGA     2

/* Values for multiboot_mmap_entry.type */
#define MB_MEMORY_AVAILABLE 1
#define MB_MEMORY_RESERVED  2
#define MB_MEMORY_ACPI      3
#define MB_MEMORY_NVS       4
#define MB_MEMORY_BADRAM    5

struct multiboot_mmap_entry {
    uint32_t size;      /* size of this entry, not counting this field */
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
} __attribute__((packed));

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;      /* KiB below 1 MiB   */
    uint32_t mem_upper;      /* KiB above 1 MiB   */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    /* Present when MB_FLAG_FRAMEBUFFER is set, which the kernel asks for
     * through the video fields of its Multiboot header.  On real hardware this
     * is the only description of the display it gets. */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;      /* bytes per scan line */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];

    /* Past the end of the Multiboot structure, and only there when
     * MB_FLAG_WOS_RSDP says so: the physical address of the ACPI root pointer.
     * See the flag. */
    uint64_t rsdp;
} __attribute__((packed));

#endif /* WOS_MULTIBOOT_H */
