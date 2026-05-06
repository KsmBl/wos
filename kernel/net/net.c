/* A minimal IPv4 stack for ping. See net.h. */

#include "net.h"
#include "rtl8139.h"
#include "pit.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* SLIRP's defaults: QEMU hands the guest 10.0.2.15, with the virtual gateway
 * at 10.0.2.2, which answers ARP and ICMP.  These match `-netdev user`. */
#define OUR_IP     NET_IP(10, 0, 2, 15)
#define NETMASK    NET_IP(255, 255, 255, 0)
#define GATEWAY    NET_IP(10, 0, 2, 2)

#define ETH_ARP    0x0806
#define ETH_IPV4   0x0800
#define IP_ICMP    1
#define ICMP_ECHO      8
#define ICMP_ECHOREPLY 0

static bool    ready;
static uint8_t our_mac[6];
static uint64_t tsc_per_us = 1;

/* ---- ARP cache ---- */
struct arp_entry { uint32_t ip; uint8_t mac[6]; bool used; };
static struct arp_entry arp_cache[8];

/* ------------------------------------------------------------------ *
 *  Byte order, timing, checksums
 * ------------------------------------------------------------------ */

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate the timestamp counter against the PIT, so a round trip can be
 * timed finely.  Interrupts are already on by the time net_init runs. */
static void calibrate_tsc(void)
{
    uint32_t t = pit_ticks();
    while (pit_ticks() == t)            /* align to a tick edge */
        ;

    uint64_t c0 = rdtsc();
    uint32_t start = pit_ticks();
    while (pit_ticks() - start < 20)    /* 20 ticks == 200 ms */
        ;
    uint64_t c1 = rdtsc();

    uint64_t per_us = (c1 - c0) / (20u * 10u * 1000u);
    tsc_per_us = per_us ? per_us : 1;
}

/* The Internet checksum (RFC 1071).  Summing native 16-bit words and storing
 * the result native is correct in either byte order. */
static uint16_t checksum(const void *data, uint32_t len)
{
    const uint16_t *w = data;
    uint32_t sum = 0;

    while (len > 1) { sum += *w++; len -= 2; }
    if (len) sum += *(const uint8_t *)w;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ------------------------------------------------------------------ *
 *  Packet layouts
 * ------------------------------------------------------------------ */

struct __attribute__((packed)) eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
};

struct __attribute__((packed)) arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6]; uint32_t spa;
    uint8_t  tha[6]; uint32_t tpa;
};

struct __attribute__((packed)) ip_hdr {
    uint8_t  ver_ihl, tos;
    uint16_t total_len, id, flags_frag;
    uint8_t  ttl, proto;
    uint16_t checksum;
    uint32_t src, dst;
};

struct __attribute__((packed)) icmp_hdr {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
};

static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ------------------------------------------------------------------ *
 *  ARP
 * ------------------------------------------------------------------ */

static void arp_store(uint32_t ip, const uint8_t *mac)
{
    for (int i = 0; i < 8; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < 8; i++)
        if (!arp_cache[i].used) {
            arp_cache[i].used = true;
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    /* Full: overwrite the first slot rather than fail. */
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
}

static bool arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < 8; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    return false;
}

static void eth_send(const uint8_t *dst, uint16_t type,
                     const void *payload, uint32_t len)
{
    uint8_t frame[1600];
    struct eth_hdr *eh = (struct eth_hdr *)frame;

    memcpy(eh->dst, dst, 6);
    memcpy(eh->src, our_mac, 6);
    eh->type = htons(type);

    if (len > sizeof(frame) - sizeof(*eh))
        len = sizeof(frame) - sizeof(*eh);
    memcpy(frame + sizeof(*eh), payload, len);

    rtl8139_send(frame, sizeof(*eh) + len);
}

static void arp_send(uint16_t oper, const uint8_t *target_mac, uint32_t target_ip)
{
    struct arp_pkt a;
    memset(&a, 0, sizeof(a));

    a.htype = htons(1);
    a.ptype = htons(ETH_IPV4);
    a.hlen = 6;
    a.plen = 4;
    a.oper = htons(oper);
    memcpy(a.sha, our_mac, 6);
    a.spa = OUR_IP;
    memcpy(a.tha, target_mac, 6);
    a.tpa = target_ip;

    eth_send(oper == 2 ? target_mac : broadcast, ETH_ARP, &a, sizeof(a));
}

/* Handle an inbound ARP: reply to a request for us, cache a reply to us. */
static void arp_input(const struct arp_pkt *a)
{
    if (a->oper == htons(2))
        arp_store(a->spa, a->sha);
    else if (a->oper == htons(1) && a->tpa == OUR_IP) {
        arp_store(a->spa, a->sha);          /* learn while we are at it */
        arp_send(2, a->sha, a->spa);
    }
}

/* Resolve an address to a MAC, sending a request and polling briefly if it is
 * not already known. */
static bool resolve(uint32_t ip, uint8_t *mac);   /* forward decl */

/* ------------------------------------------------------------------ *
 *  Receive dispatch used while waiting
 * ------------------------------------------------------------------ */

/* Drain and process one frame if present.  When it is the ICMP echo reply we
 * are waiting for (matching id/seq, from `expect_src`), returns 1. */
static int poll_frame(uint32_t expect_src, uint16_t id, uint16_t seq)
{
    uint8_t buf[1600];
    uint32_t len = rtl8139_poll(buf, sizeof(buf));
    if (len < sizeof(struct eth_hdr))
        return 0;

    struct eth_hdr *eh = (struct eth_hdr *)buf;

    if (eh->type == htons(ETH_ARP) &&
        len >= sizeof(struct eth_hdr) + sizeof(struct arp_pkt)) {
        arp_input((struct arp_pkt *)(buf + sizeof(*eh)));
        return 0;
    }

    if (eh->type != htons(ETH_IPV4) ||
        len < sizeof(struct eth_hdr) + sizeof(struct ip_hdr))
        return 0;

    struct ip_hdr *ip = (struct ip_hdr *)(buf + sizeof(*eh));
    if (ip->proto != IP_ICMP)
        return 0;

    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4u;
    if (sizeof(*eh) + ihl + sizeof(struct icmp_hdr) > len)
        return 0;

    struct icmp_hdr *ic = (struct icmp_hdr *)(buf + sizeof(*eh) + ihl);

    if (ic->type == ICMP_ECHOREPLY && ip->src == expect_src &&
        ic->id == htons(id) && ic->seq == htons(seq))
        return 1;

    return 0;
}

static bool resolve(uint32_t ip, uint8_t *mac)
{
    if (arp_lookup(ip, mac))
        return true;

    /* Ask a few times over up to a second. */
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(1, broadcast, ip);

        uint32_t deadline = pit_uptime_ms() + 250;
        while (pit_uptime_ms() < deadline) {
            poll_frame(0, 0, 0);            /* feeds arp_input via the cache */
            if (arp_lookup(ip, mac))
                return true;
            io_wait();
        }
    }
    return false;
}

/* ------------------------------------------------------------------ *
 *  Public
 * ------------------------------------------------------------------ */

void net_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));

    if (!rtl8139_init()) {
        kputs("net    : no rtl8139 card found\n");
        return;
    }

    memcpy(our_mac, rtl8139_mac(), 6);
    calibrate_tsc();
    ready = true;

    kprintf("net    : ip 10.0.2.15, gateway 10.0.2.2 (%lu MHz tsc)\n",
            (unsigned long)tsc_per_us);
}

bool net_ready(void) { return ready; }
uint32_t net_local_ip(void)   { return OUR_IP; }
uint32_t net_gateway_ip(void) { return GATEWAY; }

int net_ping(uint32_t dst, uint16_t id, uint16_t seq, uint32_t timeout_ms,
             uint32_t *rtt_us)
{
    if (!ready)
        return -W_ENODEV;

    /* On-subnet destinations are reached directly; anything else goes via the
     * gateway. */
    uint32_t nexthop = ((dst & NETMASK) == (OUR_IP & NETMASK)) ? dst : GATEWAY;

    uint8_t mac[6];
    if (!resolve(nexthop, mac))
        return -W_EHOSTUNREACH;

    /* Build ICMP echo + IP + Ethernet. */
    uint8_t packet[sizeof(struct ip_hdr) + sizeof(struct icmp_hdr) + 32];
    struct ip_hdr   *ip = (struct ip_hdr *)packet;
    struct icmp_hdr *ic = (struct icmp_hdr *)(packet + sizeof(*ip));
    uint8_t         *payload = packet + sizeof(*ip) + sizeof(*ic);

    for (int i = 0; i < 32; i++)
        payload[i] = (uint8_t)('a' + i % 26);

    memset(ic, 0, sizeof(*ic));
    ic->type = ICMP_ECHO;
    ic->id   = htons(id);
    ic->seq  = htons(seq);
    ic->checksum = checksum(ic, sizeof(*ic) + 32);

    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = htons(sizeof(*ip) + sizeof(*ic) + 32);
    ip->id        = htons(id);
    ip->ttl       = 64;
    ip->proto     = IP_ICMP;
    ip->src       = OUR_IP;
    ip->dst       = dst;
    ip->checksum  = checksum(ip, sizeof(*ip));

    uint64_t t0 = rdtsc();
    eth_send(mac, ETH_IPV4, packet, sizeof(packet));

    uint32_t deadline = pit_uptime_ms() + timeout_ms;
    while (pit_uptime_ms() < deadline) {
        if (poll_frame(dst, id, seq)) {
            *rtt_us = (uint32_t)((rdtsc() - t0) / tsc_per_us);
            return 0;
        }
        io_wait();
    }
    return -W_ETIMEDOUT;
}
