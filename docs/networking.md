# Networking

WOS has just enough of a network to run [`ping`](apps.md#ping): a driver for
one card and a small IPv4 stack — Ethernet, ARP, IPv4 and ICMP echo. There is
no TCP, no UDP, no sockets and no DNS. It exists to answer one question — is the
machine on the network and can it reach a host — and to be a foundation to
build the rest on.

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

`kernel/net/net.c` is the whole of it, under 350 lines:

- **Ethernet** framing — destination and source MAC, an ethertype, the payload.
- **ARP** — a small cache, resolving an IPv4 address to a MAC by broadcasting a
  request and waiting for the reply. Incoming requests for us are answered.
- **IPv4** — header construction with the RFC 1071 checksum, a TTL, and the
  one routing decision that matters: a destination on our subnet is reached
  directly, anything else goes to the gateway.
- **ICMP** — echo request out, echo reply matched back by id and sequence.

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

## The one call

Applications reach all of this through a single syscall, `wping()`:

```c
int rtt_us = wping(ip, seq, timeout_ms);   /* >= 0 microseconds, or -errno */
```

The kernel resolves the next hop, sends the echo, waits for the reply and
returns the round-trip time. There is no general socket API yet — `ping` is the
only thing that needs the network, so the network exposes exactly `ping`. A
sockets layer, and UDP and TCP beneath it, is the direction to grow.

## What is missing

No TCP or UDP, no sockets, no DNS, no DHCP (the address is hard-coded), no
IPv6, and no second card. The driver polls, so nothing happens on the network
unless a program is actively waiting on it. Each is a deliberate stop short of a
real stack — the point here was a working `ping`, and the smallest thing that
makes it real.
