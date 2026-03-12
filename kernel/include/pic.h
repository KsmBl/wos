/* 8259A Programmable Interrupt Controller.
 *
 * The two cascaded PICs power up mapped onto vectors 0x08-0x0F and 0x70-0x77,
 * which collides with the CPU's own exception vectors.  They must be remapped
 * before interrupts are enabled or a plain IRQ0 looks like a #DF.
 */
#ifndef WOS_PIC_H
#define WOS_PIC_H

#include "types.h"

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);     /* stop delivering this IRQ  */
void pic_clear_mask(uint8_t irq);   /* start delivering this IRQ */
void pic_mask_all(void);

#endif /* WOS_PIC_H */
