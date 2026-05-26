/* xHCI: the USB host controller every machine built this decade has.
 *
 * Enough of it to talk to one device: bring the controller up, find a device on
 * a root port, address it, and move data over its control and bulk endpoints.
 * There is no hub support, no interrupt or isochronous transfers, and no
 * hot-plug -- what is plugged in at boot is what there is.
 *
 * Polled rather than interrupt driven, like the ATA driver and for the same
 * reason: every caller is synchronous anyway, and polling removes the races
 * between an interrupt and the request that raised it.
 */
#ifndef WOS_XHCI_H
#define WOS_XHCI_H

#include "types.h"

/* An endpoint on the addressed device, as the transfer calls take it. */
typedef struct {
    uint8_t  number;        /* 1-15, as it appears in the descriptor       */
    bool     in;            /* direction: true for device-to-host          */
    uint16_t max_packet;
} usb_endpoint_t;

/* Bring up the controller.  False if the machine has no xHCI controller, or it
 * would not start. */
bool xhci_init(void);

/* Address the next device on a root port, so the transfer calls below talk to
 * it.  False when there are no more ports to try.
 *
 * Devices are taken one at a time because a machine has more than one: the
 * caller looks at each in turn and stops when it finds the one it wants, which
 * is how a USB disk is told apart from the keyboard next to it. */
bool xhci_next_device(void);

/* True once a device has been addressed and configured. */
bool xhci_device_ready(void);

/* Why the controller or the device got no further, in a few words, for the
 * boot log.  Meaningless once something has been found. */
const char *xhci_error(void);

/* A control transfer on endpoint 0.  `data` may be NULL for a request with no
 * data stage; `in` gives the direction of that stage.  Returns false if the
 * controller reported an error or the transfer did not complete. */
bool xhci_control(uint8_t request_type, uint8_t request, uint16_t value,
                  uint16_t index, void *data, uint16_t length, bool in);

/* Configure the device: select `configuration` and make `in`/`out` usable for
 * bulk transfers.  Must be called before xhci_bulk(). */
bool xhci_configure(uint8_t configuration, const usb_endpoint_t *in,
                    const usb_endpoint_t *out);

/* Move `length` bytes over a bulk endpoint.  Returns false on error or
 * timeout; short transfers count as success and are not reported. */
bool xhci_bulk(bool in, void *data, uint32_t length);

/* Clear a halted bulk endpoint, which is how a device reports a command it
 * refused.  The endpoint is unusable until this is done. */
bool xhci_clear_stall(bool in);

#endif /* WOS_XHCI_H */
