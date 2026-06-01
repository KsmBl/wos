/* A minimal IPv4 stack for ping. See net.h. */

#include "net.h"
#include "rtl8139.h"
#include "cpu.h"
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

#define DNS_SERVER NET_IP(10, 0, 2, 3)   /* SLIRP's forwarding resolver */

#define ETH_ARP    0x0806
#define ETH_IPV4   0x0800
#define IP_ICMP    1
#define IP_TCP     6
#define IP_UDP     17
#define ICMP_ECHO      8
#define ICMP_ECHOREPLY 0

/* TCP flags. */
#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10

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

#define TSC_PER_US_GUESS 1000u         /* 1 GHz, if nothing could be timed */

/* How many timestamp counter cycles go by in a microsecond, so a round trip
 * can be timed finely.
 *
 * cpu_init() has already worked the rate out -- by asking the processor, or by
 * timing it against the interval timer when the processor would not say -- and
 * ran long before this.  A machine where even that failed leaves the figure at
 * zero, and a guess is better here than nothing: the calibration only paces
 * network timeouts. */
static void calibrate_tsc(void)
{
    uint32_t khz = cpu_tsc_khz();

    if (!khz) {
        tsc_per_us = TSC_PER_US_GUESS;
        kputs("net    : the timestamp counter's rate is unknown; "
              "timeouts are approximate\n");
        return;
    }

    tsc_per_us = khz / 1000;
    if (!tsc_per_us)
        tsc_per_us = 1;
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

struct __attribute__((packed)) udp_hdr {
    uint16_t sport, dport, length, checksum;
};

struct __attribute__((packed)) tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  data_off;      /* high nibble: header length in 32-bit words */
    uint8_t  flags;
    uint16_t window, checksum, urg;
};

/* The pseudo-header TCP and UDP checksums are computed over. */
struct __attribute__((packed)) pseudo_hdr {
    uint32_t src, dst;
    uint8_t  zero, proto;
    uint16_t length;
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

/* These call one another, so forward declare them. */
static bool resolve(uint32_t ip, uint8_t *mac);
static void net_poll(void);

static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00)
         | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* Parse "a.b.c.d" into a network-order address; false if it is not one. */
static bool parse_dotted(const char *s, uint32_t *out)
{
    uint32_t part[4]; int n = 0, digits = 0; uint32_t cur = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { cur = cur * 10 + (uint32_t)(*p - '0'); if (cur > 255) return false; digits++; }
        else if (*p == '.' || *p == '\0') {
            if (digits == 0 || n > 3) return false;
            part[n++] = cur; cur = 0; digits = 0;
            if (*p == '\0') break;
        } else return false;
    }
    if (n != 4) return false;
    *out = part[0] | (part[1] << 8) | (part[2] << 16) | (part[3] << 24);
    return true;
}

/* ------------------------------------------------------------------ *
 *  IPv4 and UDP transmit
 * ------------------------------------------------------------------ */

static bool ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len)
{
    uint32_t nexthop = ((dst & NETMASK) == (OUR_IP & NETMASK)) ? dst : GATEWAY;
    uint8_t mac[6];
    if (!resolve(nexthop, mac))
        return false;

    static uint16_t ip_id;
    uint8_t pkt[1500];
    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    uint32_t total = sizeof(*ip) + len;
    if (total > sizeof(pkt))
        return false;

    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl   = 0x45;
    ip->total_len = htons((uint16_t)total);
    ip->id        = htons(++ip_id);
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->src       = OUR_IP;
    ip->dst       = dst;
    ip->checksum  = checksum(ip, sizeof(*ip));
    memcpy(pkt + sizeof(*ip), payload, len);

    eth_send(mac, ETH_IPV4, pkt, total);
    return true;
}

static bool udp_send(uint32_t dst, uint16_t sport, uint16_t dport,
                     const void *data, uint32_t len)
{
    uint8_t buf[1480];
    struct udp_hdr *u = (struct udp_hdr *)buf;
    if (sizeof(*u) + len > sizeof(buf))
        return false;

    u->sport = htons(sport);
    u->dport = htons(dport);
    u->length = htons((uint16_t)(sizeof(*u) + len));
    u->checksum = 0;                            /* optional under IPv4 */
    memcpy(buf + sizeof(*u), data, len);

    return ip_send(dst, IP_UDP, buf, sizeof(*u) + len);
}

/* ------------------------------------------------------------------ *
 *  DNS
 * ------------------------------------------------------------------ */

static volatile bool dns_done;
static uint16_t dns_query_id;
static uint32_t dns_answer;

static int dns_encode_name(const char *host, uint8_t *out)
{
    int at = 0;
    for (const char *p = host; *p; ) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int label = (int)(dot - p);
        if (label <= 0 || label > 63) return -1;
        out[at++] = (uint8_t)label;
        for (int i = 0; i < label; i++) out[at++] = (uint8_t)p[i];
        p = (*dot == '.') ? dot + 1 : dot;
    }
    out[at++] = 0;
    return at;
}

static int dns_skip_name(const uint8_t *msg, int len, int at)
{
    while (at < len) {
        uint8_t b = msg[at];
        if ((b & 0xC0) == 0xC0) return at + 2;
        if (b == 0) return at + 1;
        at += b + 1;
    }
    return len;
}

static void dns_input(const uint8_t *msg, int len)
{
    if (len < 12 || ((msg[0] << 8) | msg[1]) != dns_query_id)
        return;

    int qd = (msg[4] << 8) | msg[5];
    int an = (msg[6] << 8) | msg[7];
    int at = 12;

    for (int i = 0; i < qd; i++)
        at = dns_skip_name(msg, len, at) + 4;

    for (int i = 0; i < an && at + 10 <= len; i++) {
        at = dns_skip_name(msg, len, at);
        int type  = (msg[at] << 8) | msg[at + 1];
        int rdlen = (msg[at + 8] << 8) | msg[at + 9];
        if (type == 1 && rdlen == 4 && at + 10 + 4 <= len) {
            memcpy(&dns_answer, msg + at + 10, 4);
            dns_done = true;
            return;
        }
        at += 10 + rdlen;
    }
}

/* ------------------------------------------------------------------ *
 *  TCP (client only)
 * ------------------------------------------------------------------ */

enum { S_CLOSED = 0, S_SYN_SENT, S_ESTABLISHED, S_FIN_SENT, S_DONE };

#define TCP_RX 32768
#define TCP_MSS 1400

struct tcp_conn {
    bool     used;
    int      state;
    uint32_t rip;
    uint16_t rport, lport;
    uint32_t snd_nxt, snd_una;      /* sent / acked                 */
    uint32_t rcv_nxt;               /* next byte we expect          */
    uint8_t  rx[TCP_RX];
    uint32_t rx_head, rx_tail;      /* ring counters; count = head-tail */
    bool     rfin, reset;
};

static struct tcp_conn conns[4];

static uint32_t rx_count(struct tcp_conn *c) { return c->rx_head - c->rx_tail; }
static uint32_t rx_free(struct tcp_conn *c)  { return TCP_RX - rx_count(c); }

static uint16_t tcp_checksum(uint32_t src, uint32_t dst,
                             const void *seg, uint32_t len)
{
    /* Checksum the pseudo-header and the segment as one contiguous block,
     * reusing the proven checksum routine. */
    uint8_t tmp[sizeof(struct pseudo_hdr) + sizeof(struct tcp_hdr) + TCP_MSS];
    struct pseudo_hdr *ph = (struct pseudo_hdr *)tmp;

    ph->src = src;
    ph->dst = dst;
    ph->zero = 0;
    ph->proto = IP_TCP;
    ph->length = htons((uint16_t)len);
    memcpy(tmp + sizeof(*ph), seg, len);

    return checksum(tmp, sizeof(*ph) + len);
}

static void tcp_send(struct tcp_conn *c, uint8_t flags,
                     const void *data, uint32_t dlen)
{
    uint8_t seg[sizeof(struct tcp_hdr) + TCP_MSS];
    struct tcp_hdr *t = (struct tcp_hdr *)seg;

    memset(t, 0, sizeof(*t));
    t->sport = htons(c->lport);
    t->dport = htons(c->rport);
    t->seq   = htonl(c->snd_nxt);
    t->ack   = htonl(c->rcv_nxt);
    t->data_off = 5 << 4;
    t->flags = flags;
    uint32_t win = rx_free(c);
    t->window = htons((uint16_t)(win > 0xFFFF ? 0xFFFF : win));
    if (dlen) memcpy(seg + sizeof(*t), data, dlen);
    t->checksum = tcp_checksum(OUR_IP, c->rip, seg, sizeof(*t) + dlen);

    ip_send(c->rip, IP_TCP, seg, sizeof(*t) + dlen);
}

static void tcp_input(uint32_t src, const struct tcp_hdr *t,
                      const uint8_t *data, uint32_t dlen)
{
    struct tcp_conn *c = NULL;
    for (int i = 0; i < 4; i++)
        if (conns[i].used && conns[i].rip == src &&
            conns[i].rport == ntohs(t->sport) &&
            conns[i].lport == ntohs(t->dport)) {
            c = &conns[i]; break;
        }
    if (!c)
        return;

    uint32_t seq = ntohl(t->seq);
    uint32_t ack = ntohl(t->ack);

    if (t->flags & TF_RST) { c->reset = true; c->state = S_DONE; return; }

    if (t->flags & TF_ACK && (int32_t)(ack - c->snd_una) > 0)
        c->snd_una = ack;

    if (c->state == S_SYN_SENT) {
        if ((t->flags & TF_SYN) && (t->flags & TF_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;
            c->state = S_ESTABLISHED;
            tcp_send(c, TF_ACK, NULL, 0);
        }
        return;
    }

    /* In-order data goes into the ring; anything else earns a duplicate ACK. */
    if (dlen > 0 && seq == c->rcv_nxt && dlen <= rx_free(c)) {
        for (uint32_t i = 0; i < dlen; i++)
            c->rx[(c->rx_head++) % TCP_RX] = data[i];
        c->rcv_nxt += dlen;
        tcp_send(c, TF_ACK, NULL, 0);
    } else if (dlen > 0) {
        tcp_send(c, TF_ACK, NULL, 0);
    }

    if ((t->flags & TF_FIN) && seq + dlen == c->rcv_nxt) {
        c->rcv_nxt += 1;
        c->rfin = true;
        tcp_send(c, TF_ACK, NULL, 0);
    }

    if (c->state == S_FIN_SENT && ack == c->snd_nxt)
        c->state = S_DONE;
}

/* ------------------------------------------------------------------ *
 *  ICMP echo reply, for ping
 * ------------------------------------------------------------------ */

static volatile bool ping_got;
static uint32_t ping_src;
static uint16_t ping_id, ping_seq;

static void icmp_input(const struct ip_hdr *ip, const struct icmp_hdr *ic)
{
    if (ic->type == ICMP_ECHOREPLY && ip->src == ping_src &&
        ic->id == htons(ping_id) && ic->seq == htons(ping_seq))
        ping_got = true;
}

/* ------------------------------------------------------------------ *
 *  Receive dispatch
 * ------------------------------------------------------------------ */

static void net_poll(void)
{
    /* The card has no interrupt wired, so the network calls busy-poll it.  They
     * run inside a syscall, which is entered through an interrupt gate with
     * interrupts off -- leave them off and the timer never ticks, so every
     * millisecond deadline in these loops would wait forever.  Enable them here
     * (the timer only schedules; nothing here holds a lock). */
    sti();

    uint8_t buf[1600];
    uint32_t len = rtl8139_poll(buf, sizeof(buf));
    if (len < sizeof(struct eth_hdr))
        return;

    struct eth_hdr *eh = (struct eth_hdr *)buf;

    if (eh->type == htons(ETH_ARP)) {
        if (len >= sizeof(*eh) + sizeof(struct arp_pkt))
            arp_input((struct arp_pkt *)(buf + sizeof(*eh)));
        return;
    }
    if (eh->type != htons(ETH_IPV4) || len < sizeof(*eh) + sizeof(struct ip_hdr))
        return;

    struct ip_hdr *ip = (struct ip_hdr *)(buf + sizeof(*eh));
    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4u;
    uint32_t off = sizeof(*eh) + ihl;
    if (off > len)
        return;

    uint32_t iplen = ntohs(ip->total_len);
    uint32_t plen  = (iplen > ihl) ? iplen - ihl : 0;
    if (off + plen > len)
        plen = len - off;

    if (ip->proto == IP_ICMP && plen >= sizeof(struct icmp_hdr)) {
        icmp_input(ip, (struct icmp_hdr *)(buf + off));
    } else if (ip->proto == IP_UDP && plen >= sizeof(struct udp_hdr)) {
        struct udp_hdr *u = (struct udp_hdr *)(buf + off);
        if (ntohs(u->sport) == 53)
            dns_input(buf + off + sizeof(*u), (int)(plen - sizeof(*u)));
    } else if (ip->proto == IP_TCP && plen >= sizeof(struct tcp_hdr)) {
        struct tcp_hdr *t = (struct tcp_hdr *)(buf + off);
        uint32_t thl = (uint32_t)(t->data_off >> 4) * 4u;
        if (thl >= sizeof(*t) && plen >= thl)
            tcp_input(ip->src, t, buf + off + thl, plen - thl);
    }
}

static bool resolve(uint32_t ip, uint8_t *mac)
{
    if (arp_lookup(ip, mac))
        return true;

    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(1, broadcast, ip);
        uint32_t deadline = pit_uptime_ms() + 250;
        while (pit_uptime_ms() < deadline) {
            net_poll();
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

    uint8_t icmp[sizeof(struct icmp_hdr) + 32];
    struct icmp_hdr *ic = (struct icmp_hdr *)icmp;

    for (int i = 0; i < 32; i++)
        icmp[sizeof(*ic) + i] = (uint8_t)('a' + i % 26);

    memset(ic, 0, sizeof(*ic));
    ic->type = ICMP_ECHO;
    ic->id   = htons(id);
    ic->seq  = htons(seq);
    ic->checksum = checksum(ic, sizeof(icmp));

    ping_src = dst; ping_id = id; ping_seq = seq;
    ping_got = false;

    uint64_t t0 = rdtsc();
    if (!ip_send(dst, IP_ICMP, icmp, sizeof(icmp)))
        return -W_EHOSTUNREACH;

    uint32_t deadline = pit_uptime_ms() + timeout_ms;
    while (pit_uptime_ms() < deadline) {
        net_poll();
        if (ping_got) {
            *rtt_us = (uint32_t)((rdtsc() - t0) / tsc_per_us);
            return 0;
        }
        io_wait();
    }
    return -W_ETIMEDOUT;
}

/* ------------------------------------------------------------------ *
 *  DNS and TCP, exposed to user space
 * ------------------------------------------------------------------ */

int net_resolve(const char *host, uint32_t *ip)
{
    if (!ready)
        return -W_ENODEV;

    if (parse_dotted(host, ip))
        return 0;                           /* already an address */

    uint8_t query[512];
    static uint16_t qid = 0x1000;
    dns_query_id = ++qid;

    query[0] = (uint8_t)(dns_query_id >> 8);
    query[1] = (uint8_t)dns_query_id;
    query[2] = 0x01; query[3] = 0x00;       /* standard query, recursion */
    query[4] = 0; query[5] = 1;             /* one question */
    query[6] = query[7] = query[8] = query[9] = query[10] = query[11] = 0;

    int nl = dns_encode_name(host, query + 12);
    if (nl < 0)
        return -W_EINVAL;
    int at = 12 + nl;
    query[at++] = 0; query[at++] = 1;       /* type A  */
    query[at++] = 0; query[at++] = 1;       /* class IN */

    dns_done = false;
    for (int attempt = 0; attempt < 4; attempt++) {
        udp_send(DNS_SERVER, 5353, 53, query, (uint32_t)at);
        uint32_t deadline = pit_uptime_ms() + 500;
        while (pit_uptime_ms() < deadline) {
            net_poll();
            if (dns_done) {
                *ip = dns_answer;
                return 0;
            }
            io_wait();
        }
    }
    return -W_EHOSTUNREACH;
}

int net_tcp_open(uint32_t ip, uint16_t port)
{
    if (!ready)
        return -W_ENODEV;

    int h = -1;
    for (int i = 0; i < 4; i++)
        if (!conns[i].used) { h = i; break; }
    if (h < 0)
        return -W_EMFILE;

    struct tcp_conn *c = &conns[h];
    memset(c, 0, sizeof(*c));
    c->used  = true;
    c->rip   = ip;
    c->rport = port;
    static uint16_t lp = 49152;
    c->lport = ++lp;

    uint32_t isn = (uint32_t)rdtsc();
    c->snd_una = isn;
    c->state   = S_SYN_SENT;

    for (int attempt = 0; attempt < 5; attempt++) {
        c->snd_nxt = isn;
        tcp_send(c, TF_SYN, NULL, 0);
        c->snd_nxt = isn + 1;               /* SYN consumes a sequence */

        uint32_t deadline = pit_uptime_ms() + 500;
        while (pit_uptime_ms() < deadline) {
            net_poll();
            if (c->state == S_ESTABLISHED) {
                return h;
            }
            io_wait();
        }
    }

    c->used = false;
    return -W_ETIMEDOUT;
}

int net_tcp_send(int h, const void *data, uint32_t len)
{
    if (h < 0 || h >= 4 || !conns[h].used)
        return -W_EBADF;
    struct tcp_conn *c = &conns[h];
    if (c->reset)
        return -W_ECONNRESET;
    if (c->state != S_ESTABLISHED)
        return -W_EPIPE;

    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        uint32_t seg_seq = c->snd_nxt;
        bool acked = false;
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            c->snd_nxt = seg_seq;
            tcp_send(c, TF_PSH | TF_ACK, (const uint8_t *)data + sent, chunk);
            c->snd_nxt = seg_seq + chunk;

            uint32_t deadline = pit_uptime_ms() + 500;
            while (pit_uptime_ms() < deadline) {
                net_poll();
                if (c->reset) return -W_ECONNRESET;
                if ((int32_t)(c->snd_una - (seg_seq + chunk)) >= 0) { acked = true; break; }
                io_wait();
            }
        }
        if (!acked)
            return sent > 0 ? (int)sent : -W_ETIMEDOUT;
        sent += chunk;
    }
    return (int)sent;
}

int net_tcp_recv(int h, void *buf, uint32_t len)
{
    if (h < 0 || h >= 4 || !conns[h].used)
        return -W_EBADF;
    struct tcp_conn *c = &conns[h];

    /* Wait for data, the peer's FIN (end of file) or a reset. */
    uint32_t deadline = pit_uptime_ms() + 10000;
    for (;;) {
        if (rx_count(c) > 0) {
            uint32_t n = rx_count(c);
            if (n > len) n = len;
            for (uint32_t i = 0; i < n; i++)
                ((uint8_t *)buf)[i] = c->rx[(c->rx_tail++) % TCP_RX];
            /* We freed ring space; nudge the peer with the new window. */
            if (c->state == S_ESTABLISHED)
                tcp_send(c, TF_ACK, NULL, 0);
            return (int)n;
        }
        if (c->rfin)   return 0;            /* end of file */
        if (c->reset)  return -W_ECONNRESET;
        if (pit_uptime_ms() > deadline) return -W_ETIMEDOUT;
        net_poll();
        io_wait();
    }
}

void net_tcp_close(int h)
{
    if (h < 0 || h >= 4 || !conns[h].used)
        return;
    struct tcp_conn *c = &conns[h];

    if (c->state == S_ESTABLISHED) {
        tcp_send(c, TF_FIN | TF_ACK, NULL, 0);
        c->snd_nxt += 1;
        c->state = S_FIN_SENT;

        uint32_t deadline = pit_uptime_ms() + 1000;
        while (pit_uptime_ms() < deadline && c->state != S_DONE) {
            net_poll();
            io_wait();
        }
    }
    c->used = false;
}
