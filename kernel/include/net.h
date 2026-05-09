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

/* True once a working card has been configured. */
bool net_ready(void);

/* This host's address and its gateway, as network-order values, for anything
 * that wants to print the configuration. */
uint32_t net_local_ip(void);
uint32_t net_gateway_ip(void);

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
