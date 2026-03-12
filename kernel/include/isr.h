/* Interrupt dispatch.
 *
 * Every interrupt, exception and syscall funnels through a single assembly
 * stub that builds a `regs_t` on the stack and calls interrupt_dispatch().
 */
#ifndef WOS_ISR_H
#define WOS_ISR_H

#include "types.h"

/* The saved machine state, laid out exactly as the stub pushes it.
 * Fields appear in increasing address order: the last thing pushed is first. */
typedef struct {
    uint32_t ds;                                    /* pushed by the stub    */
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* pusha         */
    uint32_t int_no, err_code;                      /* stub + maybe the CPU  */
    uint32_t eip, cs, eflags;                       /* pushed by the CPU     */
    uint32_t useresp, ss;                           /* only on a ring change */
} regs_t;

typedef void (*interrupt_handler_t)(regs_t *regs);

/* Vector numbers. IRQs are remapped to sit just above the exceptions. */
#define IRQ_BASE      32
#define IRQ_TIMER     (IRQ_BASE + 0)
#define IRQ_KEYBOARD  (IRQ_BASE + 1)
#define INT_SYSCALL   0x80

void idt_init(void);

/* Install the CPU exception handlers (page fault, breakpoint, ...). */
void traps_init(void);

/* Install `handler` for vector `n`, replacing any previous one. */
void register_interrupt_handler(uint8_t n, interrupt_handler_t handler);

/* Print a full register dump; used by the exception handlers and by panics. */
void dump_regs(const regs_t *regs);

#endif /* WOS_ISR_H */
