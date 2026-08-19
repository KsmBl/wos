/* A very small IPv4 stack: Ethernet, ARP, IPv4 and ICMP echo -- enough for
 * ping.  It drives the RTL8139 by polling and holds a static configuration
 * matching QEMU's user-mode (SLIRP) network.
 */
#ifndef WOS_NET_H
#define WOS_NET_H

#include "types.h"

/* Compose an IPv4 address as a network-order 32-bit value from its octets. */
#define NET_IP(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Find and start the network card.  Safe to call with no card present -- ping
 * then reports that there is no device. */
void net_init(void);

/* Take the network stack, and give it back.
 *
 * Everything in here is one set of buffers, one address cache and one table of
 * connections, and none of it is written for two threads at once.  The kernel
 * lock used to guarantee that by itself -- one processor inside the kernel at
 * a time -- but a network call now gives that lock up while it waits, so that
 * a ping does not freeze the other processors for as long as it runs.  The
 * moment it does that, two threads can be in here together.
 *
 * So this is the guarantee instead, taken at the syscall boundary where each
 * network operation begins and ends.  A thread waiting its turn gives the
 * kernel lock up while it waits, which is the whole point: waiting for the
 * stack while holding the lock the thread inside needs is how a queue becomes
 * a deadlock. */
void net_claim(void);
void net_release(void);

/* True once a working card has been configured. */
bool net_ready(void);

/* Point the stack at an adapter: make it the one frames go through, take its
 * hardware address, and forget everything cached about the network we were on
 * before.  net_init does this for whichever adapter registered first; the
 * wireless layer does it again when a link comes up.
 *
 * Passing NULL marks the stack as having no adapter at all. */
struct netdev;
void net_bind(struct netdev *dev);

/* This host's configuration, as network-order values, for anything that wants
 * to print it. */
uint32_t net_local_ip(void);
uint32_t net_gateway_ip(void);
uint32_t net_netmask(void);
uint32_t net_dns_ip(void);

/* The netmask as a prefix length -- 24 rather than 255.255.255.0. */
unsigned net_prefix_length(void);

/* Set the addresses by hand, for a network that hands out none. */
void net_set_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);

/* Ask the network for an address: broadcast a DISCOVER, take the first offer,
 * request it, and adopt what the acknowledgement carries.
 *
 * The emulated network the wired card lives on needs none of this -- its
 * addresses are known and set at boot -- but a wireless link has just joined
 * a network it knows nothing about, and this is how it finds out.
 *
 * Returns 0, -W_ENODEV with no adapter, or -W_ETIMEDOUT when nothing
 * answered within `timeout_ms`. */
int net_dhcp(uint32_t timeout_ms);

/* Whether the current addresses came from a server or are the built-in
 * defaults, which is worth being able to tell apart when nothing works. */
bool net_dhcp_configured(void);

/* Send one ICMP echo request to `dst` (network order) and wait up to
 * `timeout_ms` for the matching reply.
 *
 * On success returns 0 and stores the round-trip time in microseconds through
 * `rtt_us`.  Otherwise a negative error: -W_ENODEV if there is no card,
 * -W_EHOSTUNREACH if the next hop's hardware address could not be resolved, or
 * -W_ETIMEDOUT if no reply arrived in time. */
int net_ping(uint32_t dst, uint16_t id, uint16_t seq, uint32_t timeout_ms,
             uint32_t *rtt_us);

/* Resolve a host name (or a dotted-decimal address) to a network-order
 * address.  Returns 0, -W_ENODEV, -W_EHOSTUNREACH or -W_EINVAL. */
int net_resolve(const char *host, uint32_t *ip);

/* A tiny client TCP.  net_tcp_open connects and returns a handle (0..3);
 * send/recv move bytes (recv returns 0 at the peer's end of file); close tears
 * the connection down.  All block, polling the card, up to internal timeouts. */
int  net_tcp_open(uint32_t ip, uint16_t port);
int  net_tcp_send(int handle, const void *data, uint32_t len);
int  net_tcp_recv(int handle, void *buf, uint32_t len);
void net_tcp_close(int handle);

#endif /* WOS_NET_H */
