/* What the IP stack needs from a network adapter, and no more.
 *
 * Until there was one card this was not worth having: net.c called the
 * RTL8139 by name and that was the whole of it.  A second adapter changes
 * that, and a wireless one changes it more than a second wired one would,
 * because the two are configured through completely different machinery --
 * an Ethernet card is either plugged in or it is not, where a wireless one
 * has to be told which network to join before it carries anything.
 *
 * The seam is deliberately placed at Ethernet frames.  A wireless adapter
 * does not send Ethernet frames; it sends 802.11 frames with a different
 * header, three or four addresses instead of two, and its own encryption.
 * All of that is the wireless driver's business, and it presents Ethernet to
 * the layer above -- so the IP stack, ARP and everything over them work
 * across a wireless link without knowing one is there.  That is the same
 * bargain every other system makes, and it is what keeps net.c from growing
 * a second set of everything.
 */
#ifndef WOS_NETDEV_H
#define WOS_NETDEV_H

#include "types.h"

#define NETDEV_MAX      4
#define NETDEV_NAME_MAX 16

/* The largest Ethernet frame this stack will carry, header included.  1514 is
 * the real limit; the slack lets a driver hand over a frame with padding
 * without a bounds check at every call site. */
#define NETDEV_MTU_FRAME 1600

typedef struct netdev {
    char    name[NETDEV_NAME_MAX];  /* "eth0", "wlan0"                     */
    uint8_t mac[6];
    bool    wireless;

    /* Whatever the driver wants to find itself by.  The stack never looks
     * inside it. */
    void *priv;

    /* Send one complete Ethernet frame, header and all.  Returns 0 on
     * success and negative if the adapter would not take it.  Padding a
     * short frame to the medium's minimum is the driver's job, because what
     * that minimum is depends on the medium. */
    int (*send)(struct netdev *dev, const void *frame, uint32_t len);

    /* Take one received Ethernet frame if there is one, copying at most
     * `cap` bytes into `out` and returning how many.  Zero means nothing was
     * waiting -- this never blocks, because every caller in the stack is
     * inside a polling loop with a deadline of its own.
     *
     * A driver whose hardware needs periodic attention regardless of traffic
     * does that work here too: this is the one function the stack promises to
     * call often. */
    uint32_t (*poll)(struct netdev *dev, void *out, uint32_t cap);

    /* Whether the adapter can carry traffic right now.  For a wired card
     * that is whether the cable is in; for a wireless one it is whether it
     * has joined a network and finished its handshake.  The IP stack refuses
     * to send when this is false rather than dropping frames into a link
     * that is not there. */
    bool (*link_up)(struct netdev *dev);
} netdev_t;

/* Add an adapter.  The first one registered becomes the default, so the
 * order drivers are started in decides what the stack uses when nothing has
 * said otherwise.  The netdev_t must stay valid for the life of the system;
 * drivers keep theirs in static storage. */
void netdev_register(netdev_t *dev);

/* The adapter the IP stack sends through, or NULL if none is registered. */
netdev_t *netdev_default(void);

/* Choose it.  Passing an adapter that was never registered does nothing. */
void netdev_set_default(netdev_t *dev);

/* Walk the registered adapters, so a status command can list them. */
int       netdev_count(void);
netdev_t *netdev_at(int index);

/* Find one by name, or NULL. */
netdev_t *netdev_find(const char *name);

/* Convenience wrappers that do nothing gracefully when there is no default
 * adapter, which is the state the stack is in on a machine with no card at
 * all and the state every one of these has to survive. */
int      netdev_send(const void *frame, uint32_t len);
uint32_t netdev_poll(void *out, uint32_t cap);
bool     netdev_link_up(void);

#endif /* WOS_NETDEV_H */
