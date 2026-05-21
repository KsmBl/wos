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
#include "fbcon.h"
#include "serial.h"
#include "gdt.h"
#include "isr.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "kheap.h"
#include "paging.h"
#include "ata.h"
#include "ramdisk.h"
#include "usbdisk.h"
#include "wfs_kernel.h"
#include "ramfs.h"
#include "net.h"
#include "rtc.h"
#include "acpi.h"
#include "user.h"
#include "proc.h"
#include "sched.h"
#include "selftest.h"
#include "string.h"
#include "io.h"

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static void run_shell(void) __attribute__((noreturn));

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

/* Dump the E820 memory map GRUB collected, and return the total usable
 * bytes. */
static uint64_t print_memory_map(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MB_FLAG_MMAP)) {
        kprintf("  (no memory map provided by the bootloader)\n");
        return 0;
    }

    uint64_t usable = 0;
    uintptr_t cur = mbi->mmap_addr;
    uintptr_t end = mbi->mmap_addr + mbi->mmap_length;

    while (cur < end) {
        const struct multiboot_mmap_entry *e =
            (const struct multiboot_mmap_entry *)cur;

        /* A 64-bit kernel can address every region the firmware reports, so
         * unlike the 32-bit version there is nothing to clamp away. */
        kprintf("  %p - %p  %s\n", (void *)(uintptr_t)e->addr,
                (void *)(uintptr_t)(e->addr + e->len), mmap_type_name(e->type));

        if (e->type == MB_MEMORY_AVAILABLE)
            usable += e->len;

        cur += e->size + sizeof(uint32_t);   /* size excludes its own field */
    }

    return usable;
}

/* Hand the first Multiboot module to the RAM disk driver.  grub.cfg loads the
 * filesystem image as the only module, so there is no need to match on the
 * command line string. */
static bool mount_boot_module(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MB_FLAG_MODS) || mbi->mods_count == 0)
        return false;

    const struct multiboot_module *mod =
        (const struct multiboot_module *)(uintptr_t)mbi->mods_addr;

    if (mod->mod_end <= mod->mod_start)
        return false;

    if (!ramdisk_init(mod->mod_start, mod->mod_end - mod->mod_start)) {
        kprintf("ramdisk: unusable boot module at %p - %p\n",
                (void *)(uintptr_t)mod->mod_start,
                (void *)(uintptr_t)mod->mod_end);
        return false;
    }
    return true;
}

void kmain(uint32_t magic, struct multiboot_info *mbi)
{
    serial_init();

    /* Before vga_init(), deliberately.  On a UEFI boot this is the only
     * console there will ever be -- VGA text mode does not exist, everything
     * printed before the framebuffer is running would go nowhere, and a kernel
     * that stopped early would look like a machine that never started.  Coming
     * up first also stops vga_init() from touching the legacy hardware, which
     * on a UEFI machine can take the display away from the mode the firmware
     * is scanning out.
     *
     * It needs no allocator and no paging: the loader described the
     * framebuffer, and the boot page tables can reach it. */
    if (magic == MULTIBOOT_BOOTLOADER_MAGIC && (mbi->flags & MB_FLAG_FRAMEBUFFER))
        fbcon_init_boot(mbi, 0, 0);

    vga_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs("WOS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kputs("a small operating system\n\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("bad multiboot magic %08x (expected %08x)",
              magic, MULTIBOOT_BOOTLOADER_MAGIC);

    uint64_t ksize = (uint64_t)(__kernel_end - __kernel_start);
    kprintf("kernel : %p - %p (%s)\n",
            (void *)__kernel_start, (void *)__kernel_end, fmt_bytes(ksize));

    if (mbi->flags & MB_FLAG_MEM)
        kprintf("memory : %s low, %s high\n",
                fmt_bytes((uint64_t)mbi->mem_lower * 1024),
                fmt_bytes((uint64_t)mbi->mem_upper * 1024));

    kputs("memory map:\n");
    uint64_t usable = print_memory_map(mbi);
    kprintf("usable : %s\n", fmt_bytes(usable));

    /* Descriptor tables first: nothing else can fault safely until the IDT
     * is live, and the PIC must be remapped before interrupts are enabled. */
    gdt_init();
    kputs("gdt    : flat segments + TSS loaded\n");

    idt_init();
    traps_init();
    kputs("idt    : 48 vectors + syscall gate installed\n");

    pic_mask_all();
    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    kputs("pic    : IRQs remapped to vectors 32-47\n");

    pit_init(PIT_HZ);
    keyboard_init();
    kprintf("pit    : %u Hz\nkbd    : ready\n", PIT_HZ);

    sti();

    /* Memory comes up after interrupts so a fault during initialisation is
     * reported rather than silently rebooting the machine. */
    pmm_init(mbi);
    fbcon_reserve_aperture();
    kprintf("pmm    : %s usable in %lu frames\n",
            fmt_bytes(pmm_total_bytes()), pmm_total_bytes() / PAGE_SIZE);

    kheap_init(pmm_heap_base(), KHEAP_SIZE);
    kprintf("kheap  : %s arena at %p\n",
            fmt_bytes(KHEAP_SIZE), (void *)pmm_heap_base());

    paging_init();
    kprintf("paging : enabled, low %s identity mapped\n",
            fmt_bytes(LOW_MEMORY_LIMIT));

    /* Needs the heap (tables are copied out of physical memory) and paging (on
     * most machines they live above the identity map). */
    acpi_init(mbi);
    {
        uint16_t port, sleep_type;
        acpi_power_info(&port, &sleep_type);

        if (port)
            kprintf("acpi   : soft-off through PM1a at 0x%x, sleep type %u\n",
                    port, sleep_type);
        else
            kputs("acpi   : no soft-off found; the machine can only halt\n");
    }

    /* Move the console onto a linear framebuffer for crisp text at real
     * resolutions.  Needs paging (to map the aperture) and PCI, both up now.
     *
     * QEMU's card comes first because its resolution can be changed at
     * runtime, which is what the textmode app does.  On anything else the
     * display is the one GRUB set up and described in the Multiboot info: a
     * fixed mode, but the only one real hardware offers -- and after a UEFI
     * boot the only console at all, since VGA text mode is gone. */
    int fbw, fbh;
    if (fbcon_active()) {
        /* Already running: the bootloader's framebuffer was taken before the
         * first line of this log was printed. */
        int c, r;
        fbcon_size(&c, &r);
        fbcon_resolution(&fbw, &fbh);
        kprintf("video  : bootloader framebuffer, %dx%d (%dx%d), 8x16 font\n",
                c, r, fbw, fbh);
    } else if (fbcon_init(80, 25)) {
        fbcon_resolution(&fbw, &fbh);
        kprintf("video  : framebuffer console, 80x25 (%dx%d), 8x16 font\n",
                fbw, fbh);
    } else if (fbcon_init_boot(mbi, 0, 0)) {   /* 0: fill the screen */
        int c, r;
        fbcon_size(&c, &r);
        fbcon_resolution(&fbw, &fbh);
        kprintf("video  : bootloader framebuffer, %dx%d (%dx%d), 8x16 font\n",
                c, r, fbw, fbh);
    } else {
        kputs("video  : no framebuffer; staying in VGA text mode\n");
    }

    if (ata_init()) {
        uint32_t sectors = ata_sector_count();
        kprintf("ata    : primary master, %u sectors (%s)\n",
                sectors, fmt_bytes((uint64_t)sectors * 512));
    } else {
        kputs("ata    : no drive on the primary bus\n");
    }

    if (usbdisk_init()) {
        uint32_t count = usbdisk_sector_count();
        kprintf("usb    : %s, %u sectors (%s)\n",
                usbdisk_name(), count, fmt_bytes((uint64_t)count * 512));
    } else {
        kputs("usb    : no mass storage device\n");
    }

    /* A filesystem the bootloader loaded into memory, for machines whose boot
     * device the kernel has no driver for.  pmm_init() has already fenced the
     * module off, so it is safe to keep using it in place. */
    if (mount_boot_module(mbi)) {
        uint32_t sectors = ramdisk_sector_count();
        kprintf("ramdisk: boot module, %u sectors (%s)\n",
                sectors, fmt_bytes((uint64_t)sectors * 512));
    }

    if (wfs_mount()) {
        wdiskinfo_t info;
        wfs_statfs(&info);
        const char *where = "from the disk";
        switch (wfs_source()) {
        case WFS_SOURCE_USB:     where = "from the USB device"; break;
        case WFS_SOURCE_RAMDISK: where = "in RAM (changes are not saved)"; break;
        default: break;
        }

        kprintf("wfs    : mounted %s, %s of %s free\n", where,
                fmt_bytes(info.free_bytes), fmt_bytes(info.total_bytes));
    }

    /* Scratch space in memory, mounted over the empty directory of the same
     * name on the disk.  It starts with nothing in it and takes only what is
     * put there. */
    ramfs_init();
    kprintf("ramfs  : %s in memory, growing as it is used\n", RAMFS_MOUNT);

    net_init();

    {
        wtime_t now;
        rtc_read(&now);
        kprintf("clock  : %d-%02d-%02d %02d:%02d:%02d (from the RTC)\n",
                now.year, now.month, now.day, now.hour, now.minute, now.second);
    }

    user_init();

    proc_init();
    syscall_init();
    kputs("proc   : scheduler running, syscall gate open\n");

    selftest_interrupts();
    selftest_memory();
    selftest_filesystem();
    selftest_ramdisk();
    selftest_processes();

    run_shell();
}

/* Launch the shell and keep it running.
 *
 * This runs on the boot context, which the scheduler has adopted as the idle
 * thread, so it must never block: if it did, and every other thread were
 * waiting too, there would be nothing left to switch to.  Hence the poll and
 * halt rather than a blocking wait. */
static void run_shell(void)
{
    /* The boot shell is root's login shell, so `chsh` changes what starts
     * here.  It is re-read on each (re)launch, and falls back to whell if the
     * configured one has gone missing. */
    char     shell[W_SHELL_MAX + 1];
    char    *argv[] = { "shell", NULL };
    uint32_t ino;

    user_shell(W_ROOT_UID, shell, sizeof(shell));
    if (wfs_lookup(shell, &ino) != 0)
        strlcpy(shell, "/app/whell/launch", sizeof(shell));

    if (wfs_lookup(shell, &ino) != 0) {
        kprintf("\n[kernel] %s is missing; nothing to run.\n", shell);
        for (;;)
            hlt();
    }

    int32_t pid = proc_spawn(shell, argv, NULL);
    if (pid < 0) {
        panic("cannot start the shell (error %d)", -pid);
    }

    for (;;) {
        process_t *p = proc_by_pid(pid);

        if (p && p->exited) {
            int32_t status = 0;
            proc_wait(pid, &status);     /* already exited, so returns at once */

            user_shell(W_ROOT_UID, shell, sizeof(shell));
            if (wfs_lookup(shell, &ino) != 0)
                strlcpy(shell, "/app/whell/launch", sizeof(shell));

            kprintf("\n[kernel] the shell exited with status %d; restarting %s\n",
                    status, shell);
            pid = proc_spawn(shell, argv, NULL);
            if (pid < 0)
                panic("cannot restart the shell (error %d)", -pid);
        }

        sti();
        hlt();
    }
}
