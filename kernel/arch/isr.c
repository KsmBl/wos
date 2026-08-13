/* Interrupt dispatch: the C side of the stubs in isr.S. */

#include "isr.h"
#include "pic.h"
#include "lapic.h"
#include "smp.h"
#include "kprintf.h"

static interrupt_handler_t handlers[256];

static const char *exception_name(uint32_t n)
{
    static const char *names[32] = {
        "divide by zero", "debug", "non-maskable interrupt", "breakpoint",
        "overflow", "bound range exceeded", "invalid opcode",
        "device not available", "double fault", "coprocessor segment overrun",
        "invalid TSS", "segment not present", "stack-segment fault",
        "general protection fault", "page fault", "reserved",
        "x87 floating-point exception", "alignment check", "machine check",
        "SIMD floating-point exception", "virtualization exception",
        "control protection exception", "reserved", "reserved",
        "reserved", "reserved", "reserved", "reserved",
        "hypervisor injection", "VMM communication", "security exception",
        "reserved"
    };
    return (n < 32) ? names[n] : "unknown";
}

void register_interrupt_handler(uint8_t n, interrupt_handler_t handler)
{
    handlers[n] = handler;
}

void dump_regs(const regs_t *regs)
{
    uint64_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    kprintf("  rax=%016lx rbx=%016lx rcx=%016lx\n",
            regs->rax, regs->rbx, regs->rcx);
    kprintf("  rdx=%016lx rsi=%016lx rdi=%016lx\n",
            regs->rdx, regs->rsi, regs->rdi);
    kprintf("  rbp=%016lx  r8=%016lx  r9=%016lx\n",
            regs->rbp, regs->r8, regs->r9);
    kprintf("  r10=%016lx r11=%016lx r12=%016lx\n",
            regs->r10, regs->r11, regs->r12);
    kprintf("  r13=%016lx r14=%016lx r15=%016lx\n",
            regs->r13, regs->r14, regs->r15);
    kprintf("  rip=%016lx  cs=%04lx rflags=%016lx\n",
            regs->rip, regs->cs, regs->rflags);
    kprintf("  rsp=%016lx  ss=%04lx err=%lx (ring %lu)\n",
            regs->rsp, regs->ss, regs->err_code, regs->cs & 3);
    kprintf("  cr2=%016lx cr3=%016lx\n", cr2, cr3);
}

/* Called from int_common in isr.S with a pointer to the saved state.
 *
 * This is every way into the kernel there is -- a fault, a hardware interrupt,
 * or a program's syscall -- which is what makes it the place to take the
 * kernel lock.  One processor runs kernel code at a time; the rest run user
 * code, or wait here.  See smp.h.
 *
 * The lock is not taken again if this processor already holds it: a page fault
 * inside a syscall arrives here on a processor that is already inside the
 * kernel, and waiting for itself would stop the machine. */
void interrupt_dispatch(regs_t *regs)
{
    bool took = klock_enter();

    interrupt_handler_t handler = handlers[regs->int_no & 0xFF];

    /* Acknowledge the controller *before* running the handler.  The timer
     * handler switches threads and does not return until this thread is
     * scheduled again; sending the EOI afterwards would leave the PIC waiting
     * on an interrupt that never completes, and no further IRQs would arrive.
     * Re-entry is not a concern: this is an interrupt gate, so IF stays clear
     * until the iret. */
    if (regs->int_no >= IRQ_BASE && regs->int_no < IRQ_BASE + 16)
        pic_send_eoi((uint8_t)(regs->int_no - IRQ_BASE));

    /* The local APIC has an acknowledgement of its own, for the interrupts it
     * raised rather than the 8259.  The spurious vector is the exception the
     * manual is explicit about: it must not be acknowledged, because nothing
     * was ever really delivered. */
    if (regs->int_no == LAPIC_VECTOR_TIMER)
        lapic_eoi();

    if (regs->int_no == LAPIC_VECTOR_SPURIOUS) {
        klock_leave(took);
        return;
    }

    if (handler) {
        handler(regs);
    } else if (regs->int_no < 32) {
        /* No handler for a CPU exception: nothing can sensibly continue. */
        kprintf("\nunhandled exception %lu: %s\n",
                regs->int_no, exception_name(regs->int_no));
        dump_regs(regs);
        panic("unhandled CPU exception");
    }
    /* Unhandled hardware IRQs are simply ignored; they were acknowledged
     * above, so the controller keeps working. */

    klock_leave(took);
}
