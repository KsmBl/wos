/* 8259A PIC driver. */

#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11   /* initialise, expect ICW4 */
#define ICW4_8086 0x01   /* 8086/88 mode            */
#define PIC_EOI   0x20

void pic_remap(uint8_t master_offset, uint8_t slave_offset)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT);          io_wait();
    outb(PIC2_CMD, ICW1_INIT);          io_wait();
    outb(PIC1_DATA, master_offset);     io_wait();   /* ICW2: vector offset  */
    outb(PIC2_DATA, slave_offset);      io_wait();
    outb(PIC1_DATA, 0x04);              io_wait();   /* ICW3: slave on IRQ2  */
    outb(PIC2_DATA, 0x02);              io_wait();   /* ICW3: cascade identity */
    outb(PIC1_DATA, ICW4_8086);         io_wait();
    outb(PIC2_DATA, ICW4_8086);         io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq)
{
    /* An IRQ from the slave was also signalled through the master, so both
     * controllers need the end-of-interrupt. */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(irq < 8 ? irq : irq - 8);
    outb(port, (uint8_t)(inb(port) | (1 << bit)));
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(irq < 8 ? irq : irq - 8);
    outb(port, (uint8_t)(inb(port) & ~(1 << bit)));

    /* Unmasking anything on the slave also needs the cascade line open. */
    if (irq >= 8)
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1 << 2)));
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
