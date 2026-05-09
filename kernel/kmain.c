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
#include "wfs_kernel.h"
#include "net.h"
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
    kprintf("pmm    : %s usable in %lu frames\n",
            fmt_bytes(pmm_total_bytes()), pmm_total_bytes() / PAGE_SIZE);

    kheap_init(pmm_heap_base(), KHEAP_SIZE);
    kprintf("kheap  : %s arena at %p\n",
            fmt_bytes(KHEAP_SIZE), (void *)pmm_heap_base());

    paging_init();
    kprintf("paging : enabled, low %s identity mapped\n",
            fmt_bytes(LOW_MEMORY_LIMIT));

    /* Move the console onto a linear framebuffer for crisp text at real
     * resolutions.  Needs paging (to map the aperture) and PCI, both up now.
     * Falls back to VGA text mode if there is no framebuffer. */
    if (fbcon_init(80, 25))
        kprintf("video  : framebuffer console, 80x25 (640x400), 8x16 font\n");
    else
        kputs("video  : no framebuffer; staying in VGA text mode\n");

    if (ata_init()) {
        uint32_t sectors = ata_sector_count();
        kprintf("ata    : primary master, %u sectors (%s)\n",
                sectors, fmt_bytes((uint64_t)sectors * 512));
    } else {
        kputs("ata    : no drive on the primary bus\n");
    }

    if (wfs_mount()) {
        wdiskinfo_t info;
        wfs_statfs(&info);
        kprintf("wfs    : mounted, %s of %s free\n",
                fmt_bytes(info.free_bytes), fmt_bytes(info.total_bytes));
    }

    net_init();

    user_init();

    proc_init();
    syscall_init();
    kputs("proc   : scheduler running, syscall gate open\n");

    selftest_interrupts();
    selftest_memory();
    selftest_filesystem();
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
