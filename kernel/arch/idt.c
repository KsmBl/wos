/* Interrupt Descriptor Table setup, x86-64. */

#include "isr.h"
#include "gdt.h"
#include "lapic.h"

/* Long mode gates are 16 bytes: the handler address is 64-bit and there is an
 * IST field selecting an alternative stack. */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;               /* 0 = keep using the current stack */
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Gate type/attribute bytes: present, 64-bit interrupt gate, given DPL.
 * An interrupt gate (as opposed to a trap gate) clears IF on entry, so a
 * handler never runs with interrupts unexpectedly enabled. */
#define GATE_RING0 0x8E
#define GATE_RING3 0xEE

static struct idt_entry  idt[256];
static struct idt_pointer idtp;

/* Filled in by isr.S: 48 vectors followed by the syscall stub. */
extern uint64_t isr_stub_table[];
#define STUB_COUNT 48
#define STUB_SYSCALL_INDEX 48
#define STUB_LAPIC_TIMER_INDEX 49
#define STUB_LAPIC_SPURIOUS_INDEX 50

static void idt_set_gate(uint8_t n, uint64_t handler, uint16_t sel,
                         uint8_t flags)
{
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[n].selector    = sel;
    idt[n].ist         = 0;
    idt[n].flags       = flags;
    idt[n].reserved    = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, 0, 0, 0);

    /* Exceptions and hardware IRQs: kernel only. */
    for (int i = 0; i < STUB_COUNT; i++)
        idt_set_gate((uint8_t)i, isr_stub_table[i], SEL_KCODE, GATE_RING0);

    /* The syscall gate must be reachable from ring 3, hence DPL 3. */
    idt_set_gate(INT_SYSCALL, isr_stub_table[STUB_SYSCALL_INDEX],
                 SEL_KCODE, GATE_RING3);

    /* The local APIC's own two: the timer that preempts whichever processor
     * it belongs to, and the spurious vector the controller raises when it
     * could not take an interrupt back in time. */
    idt_set_gate(LAPIC_VECTOR_TIMER, isr_stub_table[STUB_LAPIC_TIMER_INDEX],
                 SEL_KCODE, GATE_RING0);
    idt_set_gate(LAPIC_VECTOR_SPURIOUS,
                 isr_stub_table[STUB_LAPIC_SPURIOUS_INDEX],
                 SEL_KCODE, GATE_RING0);

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint64_t)&idt;

    idt_load();
}

/* Point this processor at the table.  It is built once and shared: a gate says
 * what code to run, which is the same answer on every core. */
void idt_load(void)
{
    __asm__ volatile("lidt %0" : : "m"(idtp));
}
