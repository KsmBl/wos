/* BOOTX64.EFI -- WOS's own UEFI loader.
 *
 * Under UEFI there is no bootloader between the firmware and this file: the
 * firmware loads it from the EFI system partition and calls efi_main().  That
 * is deliberate.  GRUB cannot start this kernel on UEFI at all -- Multiboot,
 * the Linux protocol and the EFI handover all end in a page fault inside GRUB,
 * whose relocator writes to a page current firmware maps read-only -- and the
 * one path it does support wants the kernel to be a UEFI application anyway.
 * If the kernel has to be one, it may as well be its own loader and drop GRUB
 * from the UEFI path entirely.
 *
 * What a bootloader owes this kernel is short:
 *
 *   * the kernel image in memory at 1 MiB, where it is linked to run,
 *   * the filesystem image loaded somewhere it can be reached,
 *   * a description of memory and of the display,
 *   * the firmware shut down, and
 *   * a jump to the kernel's entry point.
 *
 * All of which is below, in that order.  The kernel image itself is built into
 * this binary (uefi/blob.S), so booting needs one file, not two.
 *
 * The code must be position independent: it is compiled -fpic and linked
 * without relocations, so the firmware can load it anywhere.  That rules out
 * pointer initialisers in static storage, which would need relocating; the
 * build checks for any that slip in.
 */

#include "efi.h"
#include "multiboot.h"

/* From uefi/blob.S: the kernel image, verbatim. */
extern const unsigned char kernel_blob[];
extern const unsigned char kernel_blob_end[];

/* Where the kernel is linked to run, and how much memory it needs including
 * .bss.  KERNEL_MEM_END comes from the build, which reads it off the linked
 * kernel, so the two cannot disagree. */
#define KERNEL_LOAD_ADDRESS 0x100000UL
#ifndef KERNEL_MEM_END
#error "KERNEL_MEM_END must be defined by the build"
#endif

/* Everything the kernel is handed has to sit inside the gigabyte it identity
 * maps, because it reads all of it before paging is its own -- and below the
 * window its console maps the framebuffer into, at 768 MiB, which overwrites
 * the identity mapping of whatever physical memory happens to be there.  Left
 * to itself the firmware is free to put the filesystem image exactly in that
 * range, and the kernel would then read the framebuffer where it expected its
 * own filesystem.  The two must not be able to collide, and the kernel is the
 * one with the fixed address, so the loader gives way.
 *
 * Kept in step with FB_VIRT in kernel/drivers/fbcon.c. */
#define LOW_LIMIT 0x30000000UL

#define MMAP_BUFFER_BYTES (32 * 1024)
#define MAX_MMAP_ENTRIES  128

/* The block handed to the kernel: a Multiboot info structure and the arrays it
 * points at, allocated as one piece of memory. */
struct handoff {
    struct multiboot_info       mbi;
    struct multiboot_module     module;
    struct multiboot_mmap_entry mmap[MAX_MMAP_ENTRIES];
    uint8_t                     efi_map[MMAP_BUFFER_BYTES];
};

static struct efi_system_table   *systab;
static struct efi_boot_services  *bs;
static efi_handle_t               self_image;

/* -----------------------------------------------------------------------
 * Small helpers.  Freestanding and position independent, so no library.
 * ----------------------------------------------------------------------- */

static void copy_bytes(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
}

static void zero_bytes(void *dst, uint64_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = 0;
}

static void print(const uint16_t *s)
{
    if (systab && systab->con_out)
        systab->con_out->output_string(systab->con_out, s);
}

/* Report a number.  The loader's messages are the only diagnostics a UEFI
 * machine gives up before the kernel's own console exists, so they are worth
 * being specific: a screen that stops has to say what it stopped after. */
static void print_dec(uint64_t v)
{
    uint16_t buf[24];
    int i = 23;

    buf[i--] = 0;
    if (!v)
        buf[i--] = u'0';
    while (v && i >= 0) {
        buf[i--] = (uint16_t)(u'0' + (v % 10));
        v /= 10;
    }
    print(&buf[i + 1]);
}

static void print_hex(uint64_t v)
{
    uint16_t buf[19];
    int i = 18;

    buf[i--] = 0;
    if (!v)
        buf[i--] = u'0';
    while (v && i >= 2) {
        uint64_t digit = v & 0xF;
        buf[i--] = (uint16_t)(digit < 10 ? u'0' + digit : u'a' + digit - 10);
        v >>= 4;
    }
    buf[i--] = u'x';
    buf[i] = u'0';
    print(&buf[i]);
}

/* Report and stop.  Only usable before the firmware is shut down; afterwards
 * there is no console and nothing to return to. */
static void fatal(const uint16_t *msg)
{
    print(u"WOS: ");
    print(msg);
    print(u"\r\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* -----------------------------------------------------------------------
 * The display
 * ----------------------------------------------------------------------- */

/* The display the firmware is drawing on.
 *
 * A laptop with switchable graphics, or anything with more than one output,
 * offers a graphics output protocol per device, and locate_protocol() returns
 * whichever comes first -- which may be a framebuffer nothing is scanning out,
 * leaving the kernel writing text to a screen that stays dark.  The handle the
 * firmware puts its own console on is marked, so ask for that one and fall back
 * to the first usable device only if nothing claims to be the console. */
static struct efi_graphics_output_protocol *find_gop(void)
{
    struct efi_guid gop_guid = EFI_GOP_GUID;
    struct efi_guid con_guid = EFI_CONSOLE_OUT_DEVICE_GUID;
    struct efi_graphics_output_protocol *fallback = 0;
    efi_handle_t *handles = 0;
    uint64_t count = 0;

    if (bs->locate_handle_buffer(BY_PROTOCOL, &gop_guid, 0, &count, &handles)
        == EFI_SUCCESS) {
        for (uint64_t i = 0; i < count; i++) {
            struct efi_graphics_output_protocol *gop = 0;
            void *console = 0;

            if (bs->handle_protocol(handles[i], &gop_guid, (void **)&gop)
                    != EFI_SUCCESS ||
                !gop || !gop->mode || !gop->mode->info ||
                !gop->mode->framebuffer_base)
                continue;

            if (bs->handle_protocol(handles[i], &con_guid, &console) == EFI_SUCCESS)
                return gop;

            if (!fallback)
                fallback = gop;
        }
    }

    if (!fallback) {
        struct efi_graphics_output_protocol *gop = 0;
        if (bs->locate_protocol(&gop_guid, 0, (void **)&gop) == EFI_SUCCESS &&
            gop && gop->mode && gop->mode->info)
            fallback = gop;
    }

    return fallback;
}

static void describe_framebuffer(struct multiboot_info *mbi)
{
    struct efi_graphics_output_protocol *gop = find_gop();

    if (!gop) {
        print(u"WOS: the firmware offers no graphics output protocol\r\n");
        return;
    }

    const struct efi_gop_mode_info *info = gop->mode->info;

    /* The console draws a pixel as one 32-bit store.  Both 32-bit layouts are
     * fine -- the palette is symmetric enough that swapped red and blue is not
     * worth a second drawing path -- and so is a bit-masked format whose masks
     * describe 32-bit pixels, which is how some firmware reports exactly the
     * same thing.  Anything else means the framebuffer is not addressable as
     * an array of 32-bit pixels, and is refused rather than scribbled on. */
    int usable = info->pixel_format == EFI_PIXEL_RGB_RESERVED ||
                 info->pixel_format == EFI_PIXEL_BGR_RESERVED;

    if (info->pixel_format == EFI_PIXEL_BIT_MASK) {
        uint32_t red = info->pixel_information[0];
        uint32_t green = info->pixel_information[1];
        uint32_t blue = info->pixel_information[2];
        /* Eight bits each, somewhere inside 32: that is a 32-bit pixel. */
        usable = (red | green | blue) != 0 &&
                 ((red | green | blue) & 0xFF000000u) == 0;
    }

    if (!usable) {
        print(u"WOS: the display is not a 32-bit framebuffer; "
              u"no console will be available\r\n");
        return;
    }

    if (!gop->mode->framebuffer_base) {
        print(u"WOS: the display has no linear framebuffer\r\n");
        return;
    }

    mbi->framebuffer_addr   = gop->mode->framebuffer_base;
    mbi->framebuffer_pitch  = info->pixels_per_scan_line * 4;
    mbi->framebuffer_width  = info->horizontal_resolution;
    mbi->framebuffer_height = info->vertical_resolution;
    mbi->framebuffer_bpp    = 32;
    mbi->framebuffer_type   = MB_FRAMEBUFFER_RGB;
    mbi->flags             |= MB_FLAG_FRAMEBUFFER;

    print(u"WOS: display ");
    print_dec(info->horizontal_resolution);
    print(u"x");
    print_dec(info->vertical_resolution);
    print(u" at ");
    print_hex(gop->mode->framebuffer_base);
    print(u"\r\n");
}

/* -----------------------------------------------------------------------
 * ACPI
 * ----------------------------------------------------------------------- */

static int same_guid(const struct efi_guid *a, const struct efi_guid *b)
{
    if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->data4[i] != b->data4[i])
            return 0;
    return 1;
}

/* Pass on where the firmware put the ACPI tables.
 *
 * A BIOS machine leaves the root pointer lying in the memory below 1 MiB, where
 * the kernel finds it by looking.  A UEFI machine does not: it is in the
 * configuration table and nowhere else, and a kernel that cannot find it cannot
 * read the tables that say how to turn the machine off.
 *
 * Version 2 is preferred where both are present, which is what the
 * specification asks for. */
static void describe_acpi(struct multiboot_info *mbi)
{
    struct efi_guid v2 = EFI_ACPI_20_TABLE_GUID;
    struct efi_guid v1 = EFI_ACPI_10_TABLE_GUID;
    void *found = 0;

    for (uint64_t i = 0; i < systab->number_of_table_entries; i++) {
        struct efi_configuration_table *e = &systab->configuration_table[i];

        if (same_guid(&e->vendor_guid, &v2)) {
            found = e->vendor_table;
            break;
        }
        if (same_guid(&e->vendor_guid, &v1) && !found)
            found = e->vendor_table;
    }

    if (!found) {
        print(u"WOS: the firmware reports no ACPI tables\r\n");
        return;
    }

    mbi->rsdp   = (uint64_t)found;
    mbi->flags |= MB_FLAG_WOS_RSDP;
}

/* -----------------------------------------------------------------------
 * The filesystem image
 * ----------------------------------------------------------------------- */

/* Open \boot\wos.img on one volume, or nothing. */
static struct efi_file_protocol *open_on_volume(efi_handle_t device)
{
    struct efi_guid fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;
    struct efi_simple_file_system_protocol *fs = 0;
    struct efi_file_protocol *root = 0, *file = 0;

    if (!device)
        return 0;
    if (bs->handle_protocol(device, &fs_guid, (void **)&fs) != EFI_SUCCESS)
        return 0;
    if (fs->open_volume(fs, &root) != EFI_SUCCESS)
        return 0;
    if (root->open(root, &file, u"\\boot\\wos.img", EFI_FILE_MODE_READ, 0)
        != EFI_SUCCESS)
        return 0;

    return file;
}

/* Load \boot\wos.img: from the volume this binary came from if it is there,
 * otherwise from any volume the firmware can see.  The second case is what a
 * chainloaded boot needs -- started from an EFI system partition, the
 * filesystem image lives on the volume beside it, not on the one the firmware
 * loaded this from.
 *
 * Missing altogether is not fatal: the kernel boots without a filesystem and
 * says so. */
static void load_filesystem(struct handoff *h)
{
    struct efi_guid li_guid   = EFI_LOADED_IMAGE_GUID;
    struct efi_guid fs_guid   = EFI_SIMPLE_FILE_SYSTEM_GUID;
    struct efi_guid info_guid = EFI_FILE_INFO_GUID;

    struct efi_loaded_image *li = 0;
    struct efi_file_protocol *file = 0;

    if (bs->handle_protocol(self_image, &li_guid, (void **)&li) == EFI_SUCCESS)
        file = open_on_volume(li->device_handle);

    if (!file) {
        efi_handle_t *handles = 0;
        uint64_t count = 0;

        if (bs->locate_handle_buffer(BY_PROTOCOL, &fs_guid, 0, &count,
                                     &handles) == EFI_SUCCESS) {
            for (uint64_t i = 0; i < count && !file; i++)
                file = open_on_volume(handles[i]);
        }
    }

    if (!file) {
        print(u"WOS: no \\boot\\wos.img on any volume; "
              u"booting without a filesystem\r\n");
        return;
    }

    /* EFI_FILE_INFO is followed by the file's name, and the firmware refuses
     * to fill in a buffer too small to hold both. */
    uint8_t info_buffer[512];
    struct efi_file_info *info = (struct efi_file_info *)info_buffer;
    uint64_t info_size = sizeof(info_buffer);

    if (file->get_info(file, &info_guid, &info_size, info_buffer) != EFI_SUCCESS ||
        info->file_size == 0) {
        print(u"WOS: cannot measure \\boot\\wos.img\r\n");
        file->close(file);
        return;
    }

    uint64_t pages = (info->file_size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
    uint64_t addr  = LOW_LIMIT - 1;      /* the ceiling, not the address */

    if (bs->allocate_pages(ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, pages, &addr)
        != EFI_SUCCESS) {
        print(u"WOS: not enough low memory for the filesystem image\r\n");
        file->close(file);
        return;
    }

    uint64_t size = info->file_size;
    if (file->read(file, &size, (void *)addr) != EFI_SUCCESS || size == 0) {
        print(u"WOS: could not read \\boot\\wos.img\r\n");
        file->close(file);
        return;
    }
    file->close(file);

    /* Presented exactly as GRUB presents it on a BIOS machine, so the kernel
     * has one way of finding its filesystem rather than two. */
    h->module.mod_start = (uint32_t)addr;
    h->module.mod_end   = (uint32_t)(addr + size);
    h->module.cmdline   = 0;
    h->module.pad       = 0;

    h->mbi.mods_count = 1;
    h->mbi.mods_addr  = (uint32_t)(uint64_t)&h->module;
    h->mbi.flags     |= MB_FLAG_MODS;

    print(u"WOS: filesystem ");
    print_dec(size / (1024 * 1024));
    print(u" MiB loaded\r\n");
}

/* -----------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------- */

static uint32_t e820_type_of(uint32_t efi_type)
{
    switch (efi_type) {
    /* Loader and boot-services memory is free once the firmware is gone --
     * that is what those types mean.  The regions inside it that must survive,
     * the kernel image and the filesystem, are marked reserved by the caller. */
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
    case EFI_CONVENTIONAL_MEMORY:
        return MB_MEMORY_AVAILABLE;
    case EFI_ACPI_RECLAIM_MEMORY:
        return MB_MEMORY_ACPI;
    case EFI_ACPI_MEMORY_NVS:
        return MB_MEMORY_NVS;
    case EFI_UNUSABLE_MEMORY:
        return MB_MEMORY_BADRAM;
    default:
        /* Runtime services memory included: the firmware still lives there. */
        return MB_MEMORY_RESERVED;
    }
}

/* True if [start, start+len) touches [lo, hi). */
static int overlaps(uint64_t start, uint64_t len, uint64_t lo, uint64_t hi)
{
    return start < hi && lo < start + len;
}

/* Turn the firmware's map into the Multiboot one, marking as reserved the two
 * regions that look like ordinary loader memory but must outlive the loader:
 * the kernel image and whatever was loaded for it. */
static void convert_memory_map(struct handoff *h, uint64_t map_size,
                               uint64_t descriptor_size)
{
    uint64_t keep_lo = KERNEL_LOAD_ADDRESS, keep_hi = KERNEL_MEM_END;
    uint64_t mod_lo = 0, mod_hi = 0;
    uint64_t self_lo = (uint64_t)h, self_hi = (uint64_t)h + sizeof(*h);
    uint32_t count = 0;

    if (h->mbi.flags & MB_FLAG_MODS) {
        mod_lo = h->module.mod_start;
        mod_hi = h->module.mod_end;
    }

    for (uint64_t off = 0; off + descriptor_size <= map_size && count < MAX_MMAP_ENTRIES;
         off += descriptor_size) {
        const struct efi_memory_descriptor *d =
            (const struct efi_memory_descriptor *)(h->efi_map + off);

        uint64_t start = d->physical_start;
        uint64_t len   = d->number_of_pages * EFI_PAGE_SIZE;
        uint32_t type  = e820_type_of(d->type);

        if (!len)
            continue;

        if (type == MB_MEMORY_AVAILABLE &&
            (overlaps(start, len, keep_lo, keep_hi) ||
             overlaps(start, len, self_lo, self_hi) ||
             (mod_hi && overlaps(start, len, mod_lo, mod_hi))))
            type = MB_MEMORY_RESERVED;

        /* The firmware hands out dozens of small regions and the kernel reads
         * a bounded number, so runs of the same kind are merged. */
        if (count && h->mmap[count - 1].type == type &&
            h->mmap[count - 1].addr + h->mmap[count - 1].len == start) {
            h->mmap[count - 1].len += len;
            continue;
        }

        h->mmap[count].size = sizeof(struct multiboot_mmap_entry) - sizeof(uint32_t);
        h->mmap[count].addr = start;
        h->mmap[count].len  = len;
        h->mmap[count].type = type;
        count++;
    }

    h->mbi.mmap_addr   = (uint32_t)(uint64_t)h->mmap;
    h->mbi.mmap_length = count * sizeof(struct multiboot_mmap_entry);
    h->mbi.flags      |= MB_FLAG_MMAP;
}

/* -----------------------------------------------------------------------
 * Entry
 * ----------------------------------------------------------------------- */

efi_status_t EFI_ABI efi_main(efi_handle_t image_handle,
                              struct efi_system_table *st)
{
    systab     = st;
    bs         = st->boot_services;
    self_image = image_handle;

    print(u"WOS: starting\r\n");

    /* Five-level paging would make the kernel's page tables mean something
     * other than what they say: the table it loads into CR3 would be read as a
     * level higher than the one it built.  The bit cannot be turned off without
     * leaving long mode, so a machine booted with it on is one this kernel
     * cannot run on -- and the time to say so is while there is still a console
     * to say it on, rather than as a reset three instructions into the kernel. */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1UL << 12))
        fatal(u"this machine uses 5-level paging, which WOS cannot run under");

    /* Claim the address the kernel is linked to run at.  Nothing in this stub
     * is position dependent, but the kernel is: it has no relocations at all,
     * so 1 MiB is the one address in the machine that is not negotiable.
     *
     * The firmware refusing it is not fatal, and on a real machine it is not
     * even unusual: what a refusal means is that the memory at 1 MiB is the
     * firmware's own, and everything of the firmware's stops mattering at
     * exit_boot_services().  The image is written there afterwards either way
     * -- see the end of this function -- so this call is a reservation, made
     * early to keep the allocations below out of the kernel's way. */
    uint64_t kernel_size  = (uint64_t)(kernel_blob_end - kernel_blob);
    uint64_t kernel_pages = (KERNEL_MEM_END - KERNEL_LOAD_ADDRESS + EFI_PAGE_SIZE - 1)
                          / EFI_PAGE_SIZE;
    uint64_t kernel_addr  = KERNEL_LOAD_ADDRESS;

    if (bs->allocate_pages(ALLOCATE_ADDRESS, EFI_LOADER_DATA, kernel_pages,
                           &kernel_addr) == EFI_SUCCESS) {
        print(u"WOS: kernel memory at ");
        print_hex(KERNEL_LOAD_ADDRESS);
        print(u"\r\n");
    } else {
        print(u"WOS: 1 MiB is firmware memory; taking it at the handover\r\n");
    }

    /* The handoff block has to be addressable by the kernel, which means below
     * the gigabyte it identity maps, and it has to be memory the firmware is
     * not using -- so it is allocated rather than taken from this image. */
    uint64_t handoff_addr = LOW_LIMIT - 1;
    if (bs->allocate_pages(ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                           (sizeof(struct handoff) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE,
                           &handoff_addr) != EFI_SUCCESS)
        fatal(u"cannot allocate the boot information block");

    struct handoff *h = (struct handoff *)handoff_addr;
    zero_bytes(h, sizeof(*h));

    print(u"WOS: boot block at ");
    print_hex(handoff_addr);
    print(u"\r\n");

    /* Whatever is at 1 MiB gets overwritten once the firmware is gone, and by
     * then there is no console left to say that something of ours was there.
     * The three things that must not be: this loader, which is doing the
     * copying; the block being handed over; and the stack it all runs on.  The
     * firmware picked every one of those addresses, and if it picked badly the
     * time to say so is now, while saying so still reaches a screen. */
    uint64_t stack;
    __asm__ volatile("mov %%rsp, %0" : "=r"(stack));

    if (overlaps((uint64_t)kernel_blob, kernel_size,
                 KERNEL_LOAD_ADDRESS, KERNEL_MEM_END))
        fatal(u"this loader sits where the kernel has to go");
    if (overlaps(handoff_addr, sizeof(*h), KERNEL_LOAD_ADDRESS, KERNEL_MEM_END))
        fatal(u"the boot information block sits where the kernel has to go");
    if (overlaps(stack - 65536, 65536, KERNEL_LOAD_ADDRESS, KERNEL_MEM_END))
        fatal(u"the firmware stack sits where the kernel has to go");

    describe_framebuffer(&h->mbi);
    describe_acpi(&h->mbi);
    load_filesystem(h);

    if ((h->mbi.flags & MB_FLAG_MODS) &&
        overlaps(h->module.mod_start, h->module.mod_end - h->module.mod_start,
                 KERNEL_LOAD_ADDRESS, KERNEL_MEM_END))
        fatal(u"the filesystem image sits where the kernel has to go");

    print(u"WOS: entering the kernel\r\n");

    /* Shutting the firmware down needs the key of the current memory map, and
     * it refuses once if anything changed the map in between.  Reading it
     * again and retrying is what the specification asks for. */
    uint64_t map_size = 0, map_key = 0, descriptor_size = 0;
    uint32_t descriptor_version = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        map_size = MMAP_BUFFER_BYTES;
        if (bs->get_memory_map(&map_size,
                               (struct efi_memory_descriptor *)h->efi_map,
                               &map_key, &descriptor_size,
                               &descriptor_version) != EFI_SUCCESS)
            fatal(u"cannot read the firmware memory map");

        if (bs->exit_boot_services(self_image, map_key) == EFI_SUCCESS)
            break;
        if (attempt)
            fatal(u"the firmware refused to hand over the machine");
    }

    /* No firmware from here: no console, no services, no going back.  Whatever
     * is on the screen when this line runs is the last thing the firmware will
     * ever show; anything after it comes from the kernel's own console.
     *
     * Which is exactly why the kernel is written to 1 MiB now rather than
     * earlier.  Firmware is entitled to keep its own things in low memory while
     * it is running -- its stack, its page tables, whatever a vendor put there
     * -- and writing over any of that while it is still live kills the machine
     * on the spot, with no console to say what happened.  Once boot services
     * have exited there is nothing down there left to break. */
    copy_bytes((void *)KERNEL_LOAD_ADDRESS, kernel_blob, kernel_size);

    convert_memory_map(h, map_size, descriptor_size);

    /* The kernel's UEFI entry point sits at a fixed offset into the image, so
     * that this jump needs no symbol from the other side of the build. */
    void (*entry)(struct multiboot_info *) =
        (void (*)(struct multiboot_info *))(KERNEL_LOAD_ADDRESS + 0x200);
    entry(&h->mbi);

    for (;;)
        __asm__ volatile("cli; hlt");
}
