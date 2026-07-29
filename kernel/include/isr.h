/* Interrupt dispatch, x86-64.
 *
 * Every interrupt, exception and syscall funnels through a single assembly
 * stub that builds a `regs_t` on the stack and calls interrupt_dispatch().
 */
#ifndef WOS_ISR_H
#define WOS_ISR_H

#include "types.h"

/* The saved machine state, laid out exactly as the stub pushes it.
 * Fields appear in increasing address order: the last thing pushed is first.
 *
 * There is no `pusha` in long mode, so the stub pushes all fifteen general
 * registers by hand.  The CPU always pushes rsp and ss on a 64-bit interrupt,
 * whether or not the privilege level changed, which makes the frame one fixed
 * shape rather than two. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;              /* stub, and maybe the CPU  */
    uint64_t rip, cs, rflags, rsp, ss;      /* pushed by the CPU        */
} regs_t;

typedef void (*interrupt_handler_t)(regs_t *regs);

/* Vector numbers. IRQs are remapped to sit just above the exceptions. */
#define IRQ_BASE      32
#define IRQ_TIMER     (IRQ_BASE + 0)
#define IRQ_KEYBOARD  (IRQ_BASE + 1)
#define IRQ_MOUSE     (IRQ_BASE + 12)
#define INT_SYSCALL   0x80

void idt_init(void);

/* Install the CPU exception handlers (page fault, breakpoint, ...). */
void traps_init(void);

/* Install `handler` for vector `n`, replacing any previous one. */
void register_interrupt_handler(uint8_t n, interrupt_handler_t handler);

/* Print a full register dump; used by the exception handlers and by panics. */
void dump_regs(const regs_t *regs);

#endif /* WOS_ISR_H */
