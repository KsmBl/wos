/* Interrupt Descriptor Table setup. */

#include "isr.h"
#include "gdt.h"

struct idt_entry {
    uint16_t base_lo;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Gate type/attribute bytes: present, 32-bit interrupt gate, given DPL.
 * An interrupt gate (as opposed to a trap gate) clears IF on entry, so a
 * handler never runs with interrupts unexpectedly enabled. */
#define GATE_RING0 0x8E
#define GATE_RING3 0xEE

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* Filled in by isr.S: 48 vectors followed by the syscall stub. */
extern uint32_t isr_stub_table[];
#define STUB_COUNT 48
#define STUB_SYSCALL_INDEX 48

static void idt_set_gate(uint8_t n, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[n].base_lo  = (uint16_t)(base & 0xFFFF);
    idt[n].base_hi  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[n].selector = sel;
    idt[n].zero     = 0;
    idt[n].flags    = flags;
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

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint32_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtp));
}
