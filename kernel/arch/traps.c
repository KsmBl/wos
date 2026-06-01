/* CPU exception handlers.
 *
 * A fault in ring 3 is the program's problem: it is reported and that process
 * is killed, while the rest of the system carries on.  A fault in ring 0 is a
 * kernel bug and there is nothing sensible to continue with, so it panics.
 *
 * The page-fault handler decodes the error code because it is by far the most
 * common thing to go wrong once paging is enabled.
 */

#include "isr.h"
#include "kprintf.h"
#include "proc.h"
#include "sched.h"

/* Page-fault error code bits. */
#define PF_PRESENT  (1 << 0)   /* 0 = page not present, 1 = protection violation */
#define PF_WRITE    (1 << 1)   /* 0 = read, 1 = write                            */
#define PF_USER     (1 << 2)   /* 1 = fault happened in ring 3                   */
#define PF_RESERVED (1 << 3)   /* a reserved bit was set in a page table entry    */
#define PF_FETCH    (1 << 4)   /* instruction fetch                              */

/* Overridable by the memory manager once demand paging exists; until then a
 * fault is always fatal. Returns true if the fault was handled. */
__attribute__((weak)) bool paging_handle_fault(uint64_t addr, uint64_t err)
{
    (void)addr;
    (void)err;
    return false;
}

/* Kill the current process if the fault came from ring 3; otherwise panic.
 * `what` names the exception for the message. */
static void fault_in_process(regs_t *regs, const char *what)
{
    if (!(regs->cs & 3) || !sched_active()) {
        dump_regs(regs);
        panic("%s in the kernel", what);
    }

    process_t *p = proc_current();
    kprintf("[kernel] killing %s (pid %d): %s\n",
            p->name[0] ? p->name : "process", p->pid, what);
    dump_regs(regs);

    proc_exit(-1);
}

static void page_fault_handler(regs_t *regs)
{
    uint64_t addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(addr));

    if (paging_handle_fault(addr, regs->err_code))
        return;

    kprintf("\npage fault at %p while %s (%s, ring %lu)%s\n",
            (void *)addr,
            (regs->err_code & PF_WRITE) ? "writing" : "reading",
            (regs->err_code & PF_PRESENT) ? "protection violation"
                                          : "page not present",
            (regs->err_code & PF_USER) ? 3UL : 0UL,
            (regs->err_code & PF_RESERVED) ? " [reserved bit set]" : "");

    fault_in_process(regs, "page fault");
}

/* The two MSR instructions in msr.S, and where to resume when one of them
 * faults because this machine has not got that register. */
extern const char msr_read_insn[], msr_read_fixup[];
extern const char msr_write_insn[], msr_write_fixup[];

static bool resume_from_msr_fault(regs_t *regs)
{
    if (regs->rip == (uint64_t)msr_read_insn) {
        regs->rip = (uint64_t)msr_read_fixup;
        return true;
    }
    if (regs->rip == (uint64_t)msr_write_insn) {
        regs->rip = (uint64_t)msr_write_fixup;
        return true;
    }
    return false;
}

static void gpf_handler(regs_t *regs)
{
    /* Reading a model-specific register the CPU does not implement is a
     * question that was asked and answered with "no", not a bug: msr.S puts
     * each such instruction at a known address and this resumes just past it.
     * See the comment there. */
    if (resume_from_msr_fault(regs))
        return;

    kprintf("\ngeneral protection fault (selector %04lx)\n",
            regs->err_code & 0xFFFF);
    fault_in_process(regs, "general protection fault");
}

static void opcode_handler(regs_t *regs)
{
    kprintf("\ninvalid opcode at %p\n", (void *)regs->rip);
    fault_in_process(regs, "invalid opcode");
}

static void divide_handler(regs_t *regs)
{
    kprintf("\ndivide by zero at %p\n", (void *)regs->rip);
    fault_in_process(regs, "divide by zero");
}

/* Breakpoints are informational: print and carry on. This is what makes
 * `int $3` usable as a lightweight kernel tracepoint. */
static void breakpoint_handler(regs_t *regs)
{
    kprintf("breakpoint at %p\n", (void *)regs->rip);
    dump_regs(regs);
}

void traps_init(void)
{
    register_interrupt_handler(0, divide_handler);
    register_interrupt_handler(3, breakpoint_handler);
    register_interrupt_handler(6, opcode_handler);
    register_interrupt_handler(13, gpf_handler);
    register_interrupt_handler(14, page_fault_handler);
    /* Every other exception falls through to the default path in
     * interrupt_dispatch(), which dumps registers and panics. */
}
