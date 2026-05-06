/* Realtek RTL8139 driver -- QEMU's `-device rtl8139`.
 *
 * The card is driven by polling rather than its interrupt: packets arrive by
 * bus-master DMA into a ring buffer in RAM whether or not we take the IRQ, and
 * for request/reply traffic like ping, reading the ring when we are waiting
 * for a reply is simpler and needs no PCI interrupt routing.
 */
#ifndef WOS_RTL8139_H
#define WOS_RTL8139_H

#include "types.h"

/* Find and initialise the card.  Returns true on success and fills in the MAC
 * address; false if no RTL8139 is present. */
bool rtl8139_init(void);

/* The card's six-byte MAC address. */
const uint8_t *rtl8139_mac(void);

/* Send one Ethernet frame.  Returns 0 or a negative error. */
int rtl8139_send(const void *frame, uint32_t len);

/* Take one received frame from the ring, if any.  Returns its length (already
 * stripped of the CRC), or 0 when the ring is empty. */
uint32_t rtl8139_poll(void *out, uint32_t cap);

#endif /* WOS_RTL8139_H */
