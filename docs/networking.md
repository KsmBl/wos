# Networking

WOS has a small but real IPv4 stack: a driver for one card, then Ethernet, ARP,
IPv4, ICMP (for [`ping`](apps.md#ping)), UDP (for DNS), and a client
**TCP** — enough to resolve a host name and fetch a web page over HTTP, which
[`curl`](apps.md#curl), [`wget`](apps.md#wget) and [`lynx`](apps.md#lynx) do.
There is no TLS, no server side, and no general socket API; it is a client that
reaches out, not a host that listens.

## The card: RTL8139

QEMU's `-device rtl8139` is the card, chosen because it is about the simplest
PCI NIC there is: a handful of I/O registers and two ring buffers.

- **Finding it.** `kernel/drivers/pci.c` walks PCI configuration space through
  the `0xCF8`/`0xCFC` port pair looking for vendor `10EC`, device `8139`, then
  reads its I/O base and turns on bus mastering.
- **DMA buffers.** The card reads and writes packet memory itself, by bus
  master DMA, using 32-bit physical addresses. The buffers come from the kernel
  heap, which is identity-mapped — so a buffer's virtual address *is* its
  physical address, and it is already below 4 GiB. No special allocator needed.
- **Polling, not interrupts.** The driver never takes the card's IRQ. Packets
  arrive in the receive ring by DMA regardless, so when the stack is waiting
  for a reply it just reads the ring. That sidesteps PCI interrupt routing —
  which without ACPI or an I/O APIC would be fiddly — and is perfectly adequate
  for request/reply traffic. `kernel/drivers/rtl8139.c`.

## The stack

`kernel/net/net.c` is the whole of it. Every inbound frame goes through one
dispatcher (`net_poll`), which the blocking calls spin on while they wait:

- **Ethernet** framing — destination and source MAC, an ethertype, the payload.
- **ARP** — a small cache, resolving an IPv4 address to a MAC by broadcasting a
  request and waiting for the reply. Incoming requests for us are answered.
- **IPv4** — header construction with the RFC 1071 checksum, a TTL, and the
  one routing decision that matters: a destination on our subnet is reached
  directly, anything else goes to the gateway.
- **ICMP** — echo request out, echo reply matched back by id and sequence.
- **UDP** — send and receive datagrams; used for DNS.
- **DNS** — build an A-record query, send it to the resolver, parse the answer.
- **TCP** (client) — the real work: a three-way handshake, sequence and
  acknowledgement numbers, a receive ring with an advertised window, in-order
  reassembly (out-of-order segments earn a duplicate ACK so the peer resends),
  retransmission of the SYN and of unacknowledged data, and a FIN teardown.
  The checksum covers the usual pseudo-header. It is a client only — no
  listening — which is all a browser needs.

One subtlety worth recording: these calls busy-poll the card from inside a
syscall, which is entered with interrupts off. They re-enable interrupts
(`net_poll` does an `sti`), or the timer would never tick and every millisecond
deadline would wait forever — the bug that first made TCP hang.

## Configuration

Static, matching QEMU's user-mode (SLIRP) network, which is what `-netdev user`
hands out:

| | |
|---|---|
| address | `10.0.2.15` |
| netmask | `255.255.255.0` |
| gateway | `10.0.2.2` |

SLIRP's virtual gateway at `10.0.2.2` answers ARP and ICMP directly, so it is
the address that always replies. Real addresses beyond it (say `8.8.8.8`) route
through the gateway, and SLIRP proxies the ICMP onto the host's network if the
host permits unprivileged ping.

Running QEMU needs the card attached; the Makefile's `run` target and the test
harness both pass:

```
-netdev user,id=net0 -device rtl8139,netdev=net0
```

## Timing

Round-trip time is measured with the CPU timestamp counter (`rdtsc`), which is
calibrated against the PIT at boot to get ticks-per-microsecond. That resolves
a local round trip of a few tens of microseconds, far finer than the 10 ms
timer alone could.

## The calls applications use

Rather than a full socket API, the kernel exposes a few purpose-built calls:

```c
int rtt_us = wping(ip, seq, timeout_ms);      /* ICMP echo, microseconds   */
int rc     = wresolve(host, &ip);             /* DNS, or a dotted address   */

int h = wtcp_open(ip, port);                  /* connect                    */
    wtcp_send(h, buf, len);                   /* send, blocks until acked   */
    wtcp_recv(h, buf, len);                   /* recv, 0 at end of file     */
    wtcp_close(h);
```

On top of those, `libwkernel` has a tiny HTTP client, `whttp_get(url, &resp)`,
which resolves the host, connects, sends a GET and reads the whole HTTP/1.0
response into one buffer — the shared engine behind `curl`, `wget` and `lynx`.

## What is missing

- **No TLS.** Plain HTTP only, so `https://` is refused. This is the big one —
  a great deal of the web now insists on HTTPS.
- **No server side.** TCP connects out; it does not listen. No sockets API in
  the Unix sense, no `bind`/`accept`.
- **A minimal TCP.** No congestion control, no window scaling, no selective
  ACK, and out-of-order segments are dropped rather than buffered. Correct and
  enough for request/response over a low-loss path; not a stack for a server.
- **No DHCP** (the address is hard-coded to SLIRP's), **no IPv6**, one card.
- The driver **polls**, so nothing happens on the network unless a program is
  actively waiting on it.
