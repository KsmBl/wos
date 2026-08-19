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

/* Find and initialise the card, registering it as `eth0` on success.
 * Returns true if a card was found, false if none is present. */
bool rtl8139_init(void);

/* The card's six-byte MAC address, or NULL before it has been found.  The
 * stack reads this through the netdev it registered; this stays for anything
 * that wants the card specifically. */
const uint8_t *rtl8139_mac(void);

#endif /* WOS_RTL8139_H */
