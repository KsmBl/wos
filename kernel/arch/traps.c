/* CPU exception handlers.
 *
 * With no user processes yet, any fault is a kernel bug, so most handlers dump
 * state and panic.  The page-fault handler decodes the error code because it
 * is by far the most common thing to go wrong once paging is enabled.
 */

#include "isr.h"
#include "kprintf.h"

/* Page-fault error code bits. */
#define PF_PRESENT  (1 << 0)   /* 0 = page not present, 1 = protection violation */
#define PF_WRITE    (1 << 1)   /* 0 = read, 1 = write                            */
#define PF_USER     (1 << 2)   /* 1 = fault happened in ring 3                   */
#define PF_RESERVED (1 << 3)   /* a reserved bit was set in a page table entry    */
#define PF_FETCH    (1 << 4)   /* instruction fetch                              */

/* Overridable by the memory manager once demand paging exists; until then a
 * fault is always fatal. Returns true if the fault was handled. */
__attribute__((weak)) bool paging_handle_fault(uint32_t addr, uint32_t err)
{
    (void)addr;
    (void)err;
    return false;
}

static void page_fault_handler(regs_t *regs)
{
    uint32_t addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(addr));

    if (paging_handle_fault(addr, regs->err_code))
        return;

    kprintf("\npage fault at %p while %s (%s, ring %u)%s\n",
            (void *)addr,
            (regs->err_code & PF_WRITE) ? "writing" : "reading",
            (regs->err_code & PF_PRESENT) ? "protection violation"
                                          : "page not present",
            (regs->err_code & PF_USER) ? 3u : 0u,
            (regs->err_code & PF_RESERVED) ? " [reserved bit set]" : "");
    dump_regs(regs);
    panic("page fault");
}

/* Breakpoints are informational: print and carry on. This is what makes
 * `int $3` usable as a lightweight kernel tracepoint. */
static void breakpoint_handler(regs_t *regs)
{
    kprintf("breakpoint at %p\n", (void *)regs->eip);
    dump_regs(regs);
}

void traps_init(void)
{
    register_interrupt_handler(3, breakpoint_handler);
    register_interrupt_handler(14, page_fault_handler);
    /* Every other exception falls through to the default path in
     * interrupt_dispatch(), which dumps registers and panics. */
}
