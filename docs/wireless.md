# Wireless

WOS can scan for wireless networks and join one, including a WPA2 network with
a passphrase. The 802.11 layer, the WPA2 supplicant and the `wifi` command are
finished and tested. **The driver for the adapter is not** — see
[Where this stands](#where-this-stands) at the bottom, which is the first thing
to read if you were hoping to get online.

## The shape of it

Four pieces, bottom to top:

| | |
|---|---|
| `kernel/drivers/iwlwifi/` | the adapter: registers, DMA rings, firmware |
| `kernel/net/wifi.c` | 802.11 — scanning, joining, the WPA2 handshake |
| `kernel/lib/crypto.c` | SHA-1, HMAC, PBKDF2, AES, key unwrap, CCM |
| `app/wifi/` | the command |

The seam between the driver and the layer above it is `wifi_ops` in
`kernel/include/wifi.h`: tune to a channel, send an 802.11 frame, take a
received one, install a key, start a scan. A driver implements those seven
functions and calls `wifi_attach()`. Nothing above that seam is specific to any
adapter.

The seam *above* `wifi.c` is an ordinary [netdev](networking.md#adapters), so
the IP stack sees an Ethernet adapter and never learns there is a radio
involved.

## How much 802.11 is in the firmware

More than you would expect, which changes what the host has to do.

Acknowledgements, retries, inter-frame timing, rate selection, and the
encryption of individual data frames are all done by the adapter. The driver
never sees them and could not keep up if it had to — an acknowledgement is due
microseconds after a frame arrives.

What the host must still do is everything involving a **decision**: which
networks exist, which one to join, proving we know the passphrase, and telling
the hardware what key to use once that is proved. That is what `wifi.c` is, and
it is why it is a thousand lines rather than ten thousand.

## Joining a network

```
scan        the adapter sweeps the channels; every beacon it hears becomes an
            entry in a table — the network's name, its access point, its
            channel, its signal strength and what security it wants

join        tune to that channel, then two exchanges with the access point:
            authentication (a formality, unless the network still uses the
            ancient shared-key scheme) and association, where we say which
            cipher we intend to use and are given an identifier for the session

handshake   four EAPOL messages that prove both ends derived the same key from
            the passphrase, without either of them sending it

configure   the key goes into the adapter, and DHCP asks the network for an
            address
```

### The four-way handshake

This is the interesting part, and the part where a wrong byte produces silence
rather than an error.

Both ends already hold the **pairwise master key**. It never crosses the air:
it is `PBKDF2-HMAC-SHA1(passphrase, ssid, 4096 iterations, 32 bytes)`, and both
sides compute it from things they already have. The 4096 iterations are the
point — they make guessing passphrases from a captured handshake slow.

What is exchanged is two nonces:

1. **AP → us.** The access point's nonce. Nothing is signed yet.
2. **us → AP.** Our nonce, and the RSN element saying which cipher we want,
   signed with a key derived from both nonces. The access point derives the
   same key and checks the signature; if the passphrase was wrong it cannot,
   and **it simply does not reply**. There is no "wrong password" message in
   this protocol.
3. **AP → us.** Signed the same way, which is what proves the access point
   knows the passphrase too — without this check, anything on the channel
   could hand us a group key of its choosing. It also carries the group key,
   wrapped with AES under a second derived key.
4. **us → AP.** We are satisfied.

The session keys come from
`PRF-384(PMK, "Pairwise key expansion", addresses ‖ nonces)`, with both the
addresses and the nonces in sorted order — smaller first — which is what lets
each end compute the same thing without agreeing who is who. Of the 48 bytes
out, the first 16 sign handshake messages, the next 16 unwrap the group key,
and the last 16 are what the hardware encrypts data with.

Two details that are easy to get wrong and invisible when you do:

- The label in the PRF is hashed **with its terminating zero byte**.
- The signature covers exactly what the EAPOL header says the message is, not
  everything that arrived — frames can carry padding past the end of the
  payload.

The keys go into the hardware only after the fourth message is sent. Installing
them earlier makes the adapter encrypt that message, which the access point is
not yet expecting to be encrypted.

## The cryptography

`kernel/lib/crypto.c`, written for this and used by nothing else yet: SHA-1,
HMAC-SHA1, PBKDF2, the 802.11 PRF, AES (128/192/256), RFC 3394 key unwrap and
AES-CCM.

Every one of them is checked against the vectors published with it — FIPS
180-2, RFC 2202, RFC 6070, FIPS-197, RFC 3394, RFC 3610, and the two
passphrase-to-key vectors from the 802.11i annex — by the boot self-test, so
`make SELFTEST=1` and watch the console:

```
-- cryptography self-test --
  [ok  ] SHA-1 of "abc" (FIPS 180-2)
  [ok  ] HMAC-SHA1 case 1 (RFC 2202)
  [ok  ] the 802.11 pseudo-random function
  [ok  ] WPA2 master key from a passphrase (802.11i)
  [ok  ] AES-128 of the FIPS-197 example block
  [ok  ] AES-128 decryption undoes it
  [ok  ] AES key unwrap (RFC 3394)
  [ok  ] key unwrap refuses a wrong key rather than returning rubbish
  [ok  ] AES-CCM ciphertext and tag (RFC 3610)
  [ok  ] AES-CCM decryption recovers the payload
  [ok  ] AES-CCM rejects a frame whose bits were changed
-- cryptography self-test passed --
```

A failure there panics rather than continuing. That is worth a boot's time
more than usual here, because **a wrong digest does not fail loudly**. It
produces a handshake the access point declines without ever saying why, which
is close to undebuggable from the far end.

Tag and signature comparisons go through `crypto_equal`, which looks at every
byte regardless of where the first difference is, so the time taken carries no
information about how much of a guess was right.

## The command

```sh
wifi                          # what the adapter is doing
wifi scan                     # what is in range
wifi connect <name>           # an open network
wifi connect <name> <key>     # a protected one
wifi connect <name> -         # ask for the passphrase without showing it
wifi disconnect
```

`scan` lists networks strongest first, one line per name — several access
points broadcasting one name are one network to anyone choosing which to join —
and marks the ones WOS cannot join, so nobody spends a minute finding out the
hard way. [`docs/apps.md`](apps.md#wifi) shows the intended output; note that
it is the intended output and not a transcript, for the reason at the bottom of
this page.

Scanning is open to any account: seeing what networks exist tells you nothing
you could not learn by standing in the room with a phone. **Connecting is
root's**, because it changes where every program's traffic goes and the
passphrase is a credential.

A passphrase of `-` is read with the console in raw mode and never echoed, so
it stays out of the shell's history. It is wiped from memory in both the
command and the kernel as soon as the master key is derived from it.

## The firmware

The adapter does almost nothing until it is given firmware — about 1.45 MiB of
it, signed by Intel. On a cold machine its registers answer and that is all:
no MAC, no radio control, no notion of a channel.

The firmware is **not in this repository**: it is Intel's, redistributable but
not ours to keep in a source tree. `tools/copyfw.sh` takes it from the machine
doing the build, where Linux has already installed it, and puts it at
`/lib/firmware/iwlwifi-9000.ucode`.

It briefly shipped in six numbered pieces, because WFS held at most 267 KiB in
one file and this is more than five times that. That limit is gone — WFS grew a
double-indirect block and now holds 64 MiB in a file — so the firmware is one
file and the driver reads it in one go. A limit that forces the things being
stored to be cut up is a limit in the wrong place, and this was the case that
made that obvious.

A machine without the firmware builds an image without it, and the driver says
so at boot rather than failing.

The file is a small header and then a stream of tagged blocks. Most describe
the firmware; a handful *are* it, each carrying the address inside the device
that it loads at. This machine's copy holds 208 blocks, of which ten are
runtime sections — four for the adapter's first processor, four for its second,
and two that are paged in on demand. The parser in `iwl-fw.c` is checked
against the real file.

## What the wireless layer does not do

Separately from the driver, which is its own story below:

- **WPA2-PSK with CCMP only.** WEP and the original WPA with TKIP are
  recognised and refused — both are broken, and neither is worth implementing.
  WPA3's handshake (SAE) is not implemented, though most WPA3 access points
  also offer WPA2 in a transitional mode and those are joined as WPA2.
  Enterprise networks, which authenticate against a server rather than a
  passphrase, are not supported at all.
- **Hidden networks are listed, not joinable.** A network that suppresses its
  name in beacons shows up keyed by its access point's address. Joining one
  means sending a directed probe carrying the name, which is not implemented.
- **The group key is never renewed.** An access point that rekeys sends a
  two-message group handshake; it is ignored, so broadcast traffic stops being
  readable until the next connection. Unicast is unaffected.
- **One adapter, one network.** No roaming between access points, no band
  steering, no background scanning while connected.
- **The nonce source is weak.** There is no entropy pool in this kernel, so
  the nonce for each handshake is derived from the timestamp counter and the
  adapter's address. A nonce must be unique rather than secret, and that
  gives uniqueness — but it is not what a system with a real random source
  would use, and `random_bytes()` in `wifi.c` is the first place a real one
  should go.

## Where this stands

Honestly, because a driver that looks finished and is not wastes somebody's
week.

**Finished and tested:**

- the cryptography, against published vectors, in the boot self-test
- the 802.11 layer: beacon and element parsing, the scan table, authentication
  and association, the four-way handshake, 802.11 ⇄ Ethernet conversion
- the netdev layer, the DHCP client, the DMA allocator
- the syscalls and the `wifi` command, checked end to end by
  `python3 tools/check.py wifi`
- the firmware parser, against the real 1.45 MiB file

**Run against the real adapter, over VFIO passthrough, and working:**

- finding the adapter and mapping its registers — `hardware revision 0x312,
  radio 0x105110`, real values from real silicon
- claiming the device from the platform, and powering it up:
  `GP_CNTRL 0x8040005` is clock-ready, init-done, radio not killed
- reading and writing the device's own memory through the address/data window
- **loading the complete firmware** — all ten runtime sections, both
  processors, each verified by reading back what was written
- **starting it**: `the firmware raised its alive interrupt`. The firmware
  runs.
- **reading the hardware address** — `14:f6:d8:fa:f4:9c`, which is exactly
  what Linux reports for the same adapter. `wlan0` registers in WOS.

**Run, and not working: the adapter does no DMA at all.**

This is one finding, not several, and it is what everything now rests on.

- The firmware-loading DMA engine is programmed exactly as the vendor's
  driver programs it — the registers read back holding the right source,
  destination and length — and it transfers nothing.
- The receive engine is configured exactly as the vendor's driver configures
  it — `rfh dma cfg 0x87940000, active 0x10001, free base 0xb16000`, every
  value as written — and delivers nothing.
- The host has recorded **no IOMMU fault of any kind, ever**. A device whose
  DMA was being refused would fault. This one is not being refused; it is not
  asking.

Two independent engines, both correctly programmed, both silent, nothing
denied. Meanwhile memory-mapped access works perfectly in both directions --
which is how the firmware got loaded at all: the driver falls back to copying
each section in through the memory window, a word at a time.

**The likeliest explanation is the passthrough, not the driver.** This is a
CNVi part: the radio lives in the chipset and only the controller half sits on
the PCI bus, and its DMA does not take the path a discrete card's would. The
warning at the top of `tools/wifi-passthrough.sh` -- written before any of
this was tried -- was that CNVi parts pass through badly. That appears to be
exactly what happened.

**The way to find out is bare metal.** Booted from a USB stick on the machine
itself there is no IOMMU, no VFIO and no emulator between the adapter and
memory: it would write to real physical addresses, which is the case the
driver is actually written for. Everything above the DMA is now known to work
on this silicon, so that is a genuinely open question rather than a hope.

```sh
make && sudo tools/flash-usb.sh    # then boot the laptop from the stick
```

**Not written, and blocked behind the DMA:**

Commands reach the firmware through a transmit ring and are answered through
the receive ring — both of which are DMA. So until the adapter moves bytes,
none of the following can be tested even if it were written, which is why the
work stopped here rather than continuing:

- the firmware command layouts. Several of them — the station command, the MAC
  context, the scan request — are large structures versioned by the firmware's
  own capability flags, and they could not be reproduced faithfully without the
  hardware or its documentation to check against. The functions that need them
  say so and return an error rather than sending a malformed command and
  wedging the firmware. `iwl-mvm.c` lists what each one would need.

So on the laptop this was written for, `wifi scan` will find the adapter, load
and parse its firmware, and then tell you it cannot command it. Everything
above that point is waiting and works.

**Why it was not tested:** QEMU emulates no wireless device of any kind, so
there is no emulator to develop against.

## Testing it against the real adapter

The one way to run this code against something that answers like the hardware
is to give it the hardware. `tools/wifi-passthrough.sh` does that: it takes the
adapter away from Linux, binds it to `vfio-pci`, and boots WOS in QEMU with the
device mapped in, so the driver sees the real registers.

```sh
sudo tools/wifi-passthrough.sh            # run it
sudo tools/wifi-passthrough.sh --return   # give the adapter back
```

It needs an **IOMMU**, which is off unless the kernel is told at boot to use
one. `tools/enable-iommu.sh` arranges that:

```sh
tools/enable-iommu.sh --check      # what the state is; needs no root
sudo tools/enable-iommu.sh         # add intel_iommu=on iommu=pt, rebuild grub.cfg
# ... restart ...
sudo tools/enable-iommu.sh --undo  # put it back
```

It is the only thing in this repository that changes the machine it is built
on rather than the machine being built. It edits one line of
`/etc/default/grub`, keeps a timestamped backup, verifies that the regenerated
`grub.cfg` really contains the parameters, and restores the backup if anything
went wrong.

**A restart is unavoidable** — the kernel reads its command line at boot.
Afterwards `ls /sys/kernel/iommu_groups` should list something; while it is
empty, VFIO has nothing to isolate the device with, and QEMU does not support
VFIO's no-IOMMU mode.

Check first that the firmware is offering VT-d at all: `--check` reports
whether the DMAR ACPI table is present. If it is missing, the parameters will
do nothing and VT-d needs turning on in the firmware setup — usually called
"VT-d" or "Intel Virtualization Technology for Directed I/O".

Two things to know before spending an evening on it:

- **Linux has no wireless while the adapter is bound to `vfio-pci`.** Use a
  cable or a USB adapter, and do not run it from a session you are reaching
  over wifi. `--return` puts it back without a reboot.
- **CNVi parts pass through badly.** The radio is in the chipset and only the
  controller half is on the bus; there is no function-level reset, so a guest
  that touches the adapter and dies can leave it wedged until the machine is
  power-cycled. It may simply not work. Better to know that going in than to
  conclude it at two in the morning.

Whatever happens, the serial log is the thing to read: every line the driver
prints goes there, and on a run that fails the last of them says how far it
got.

**If you are picking this up**, the order of operations is written out in full
at the bottom of `iwl-mvm.c`. Steps one to four are implemented and proven on
hardware. The immediate blocker is getting the processors out of reset; after
that comes the alive handshake, and only then step five and the command layer.

Some things this cost time to learn, recorded so they cost nobody else any:

- **`NIC_READY` is a bit you write, not one you wait for.** You assert it to
  claim the device and read it back to see whether the claim was allowed.
  Polling for it without writing it waits forever.
- **`MAC_ACCESS_REQ` is for older families and is actively harmful here.**
  Asserting it drops `MAC_CLOCK_READY` while the request is renegotiated, so
  a driver that sets it and then waits for the clock has broken the thing it
  is waiting for. Once `INIT_DONE` is set this generation is simply awake.
- **The APMG power-management block does not exist on the 9000 series.**
  Those peripheral writes go into a hole.
- **Ask the device rather than guessing.** Nearly every step above was found
  by printing a register and reading it, not by reasoning about what ought to
  be true. The register dumps in this driver are deliberate and worth keeping.
- **Check the host's log too.** That there were no IOMMU faults is what
  established the DMA engine never attempts a transfer, rather than attempting
  one that is refused — two very different problems that look identical from
  inside the guest.

Two things above the driver are also worth a look once frames are moving, both
marked in the source: whether the protected bit should be set in the frame
header when the hardware is also being asked to encrypt (`wifi_netdev_send`),
and the descriptor layout in front of a received frame (`iwl_service`), which
is versioned like the command bodies and is written here as a shape rather
than a layout known to be right.

The most useful thing to have while doing any of it is a second machine
capturing on the same channel in monitor mode. Most of what goes wrong at this
level is a frame that was sent and looked fine, and the only way to see what
actually left the antenna is to listen for it.
