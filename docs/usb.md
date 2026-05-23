# Booting WOS on real hardware

`tools/flash-usb.sh` writes WOS to a USB stick. It lists the removable disks it
can see, asks which one, and refuses the ones it should — the disk holding `/`,
anything mounted at `/boot` or `/home`. It needs root and re-runs itself under
`sudo` if it has to.

```sh
make
sudo tools/flash-usb.sh
```

The stick boots both ways: BIOS/CSM machines through GRUB, UEFI machines
through WOS's own UEFI loader. Nothing needs to be selected or configured for
either.

## What ends up on the stick

The default (`--mode install`) does not copy an image onto the device; it
installs a bootable system:

| | |
|---|---|
| partition table | MBR, two partitions, the first marked **active**, type `ef` |
| filesystem | FAT32 on partition 1, label `WOS`; the WFS volume raw on partition 2 |
| MBR + gap | GRUB's `i386-pc` boot image, for BIOS/CSM machines |
| `/EFI/BOOT/BOOTX64.EFI` | WOS's UEFI loader, for UEFI machines |
| `/boot/kernel.elf` | the kernel, as GRUB loads it |
| `/boot/wos.img` | a copy of the filesystem, for machines whose USB the kernel cannot drive |
| `/boot/grub/grub.cfg` | GRUB's config — the BIOS path only |

That combination is what firmware actually accepts from a removable disk. The
active flag matters because some BIOSes will not offer a USB disk without one,
and an ESP on an MBR label is what UEFI expects from removable media.

Two other modes exist:

- `--mode iso` writes `build/wos.iso` over the whole device with `dd`. Quicker,
  but weaker in two ways: the hybrid ISO leaves a GPT behind a protective MBR
  with no active partition, which some BIOSes refuse, and on a UEFI machine it
  boots without a filesystem — UEFI firmware reads FAT and nothing else, so
  `/boot/wos.img` on the ISO9660 track is unreachable.
- `--mode img` writes `build/wos.img` to LBA 0 of a device, with no partition
  table — for putting a volume on an ATA disk the kernel reads directly.

## Where the filesystem comes from

`wfs_mount()` tries three devices in order, and the boot log says which it took:

| | |
|---|---|
| an ATA disk | `wfs    : mounted from the disk` |
| a USB disk | `wfs    : mounted from the USB device` |
| the image the loader put in memory | `wfs    : mounted in RAM (changes are not saved)` |

The first two keep what is written to them. The third is a copy: everything
works normally — every application is there, since they live in the image — but
nothing written survives the reboot.

When one of the first two wins, the copy in memory is freed: on a 16 MiB image
that is 16 MiB of RAM holding a second copy of what is already on the disk,
reserved for the whole boot and never read again.  The log says so --
`ramdisk: released 16 MiB; the volume is on a real device`.

The RAM copy exists because it needs no driver at all. It is loaded before the
kernel starts, by GRUB as a Multiboot module on BIOS or by the UEFI loader
reading `\boot\wos.img` off the volume, which is what keeps a machine bootable
when its hardware is something this kernel cannot drive. The frame allocator is
told to keep off it, and off the block the loader used to describe it — see
`find_span()` in `kernel/mm/pmm.c`.

## Why UEFI has its own loader

GRUB cannot start this kernel on UEFI firmware. Not in one way — in every way
it offers:

| how | what happens |
|---|---|
| `multiboot` | page fault inside GRUB at the handover |
| `linux`, fixed load address | GRUB's relocator moves the image → same fault |
| `linux`, relocatable + self-relocating kernel | no move needed → same fault |
| EFI handover protocol | never used; upstream GRUB has no handover support |

The fault is in GRUB, before the kernel runs: its relocator writes to a page
that current EDK2-derived firmware maps read-only. Booting a real `vmlinuz` on
the same firmware shows why that is never hit in practice — GRUB detects that a
Linux kernel is *itself* a PE/COFF EFI application and starts it through the
firmware, skipping the relocator entirely:

```
loader/efi/linux.c: UEFI stub kernel:  PE/COFF header @ 00000040
loader/efi/linux.c: starting image 0x1e503498
```

Anything that is not a UEFI application falls through to the path that faults.
So WOS is one: `uefi/stub.c` and `uefi/blob.S` build `BOOTX64.EFI`, a PE32+
application with the kernel embedded in it, which the firmware loads directly.
It claims the memory at 1 MiB where the kernel is linked to run, copies the
kernel there, finds the framebuffer through the Graphics Output Protocol, loads
`\boot\wos.img`, converts the firmware's memory map into the Multiboot format
the kernel already reads, calls `ExitBootServices` and jumps to
`kernel/uefiboot.S`. GRUB is not involved at all.

No cross-compiler is needed for any of that: the host `gcc` builds position
independent freestanding code, `ld` links it as a shared object and `objcopy`
converts that to PE. Two things about that conversion are load-bearing, and the
build checks both, because getting either wrong produces a loader that works on
one machine and fails unpredictably on the next:

- **No load-time relocations.** Nothing applies them; any that survived the link
  would be wrong addresses at runtime.
- **No `.bss`.** A section with no contents cannot be carried into the PE, so
  the image would end before it, and every write to a zero-initialised static
  would land past the end of the allocation the firmware made from
  `SizeOfImage` — in memory belonging to some other driver. `uefi/stub.lds`
  folds `.bss` into `.data` so those variables live inside the image, the same
  arrangement gnu-efi uses. Without it the loader corrupts whatever the firmware
  put after it, which on one machine is nothing and on another is fatal at
  whatever unrelated moment the firmware next reads what was overwritten.

Secure Boot must be off: this loader is unsigned.

Four more things differ between firmware and firmware, and the loader does not
assume its way past any of them:

- **Which display is the screen.** A machine with switchable or dual graphics
  has a Graphics Output Protocol per device, and the first one is not
  necessarily the one being scanned out. The loader takes the handle the
  firmware marks as its console output device, and falls back to the first
  usable framebuffer only if nothing claims to be the console.
- **What the firmware left in CR4.** `kernel/uefiboot.S` clears SMAP, SMEP,
  UMIP, PKE and CET before the kernel's own page tables load. GRUB leaves all of
  them off, so the rest of the kernel is written as if they cannot be on — SMAP
  alone would fault every system call that reads its arguments.
- **Five-level paging.** If it is on, the kernel's PML4 would be read as a PML5
  and the machine would reset three instructions into the kernel. It cannot be
  turned off without leaving long mode, so the loader refuses on the spot, while
  a console still exists to say why.
- **Where the firmware's stack is.** On a machine with a couple of gigabytes it
  sits near the top of memory, outside the first gigabyte the kernel identity
  maps — so `kernel/uefiboot.S` moves to the kernel's own stack *before* loading
  CR3, not after. Pushing to an unmapped stack faults with no IDT to catch it,
  which is a reset, not a message. GRUB's stack is in low memory and stays
  mapped either way, so the Multiboot path never had to care.

Two of the loader's addresses are chosen by the firmware and land in ordinary
memory: the boot information block and the filesystem image. The frame allocator
used to put its bitmap immediately above the last module, which on a 2 GiB
machine is exactly where the firmware had put the boot information block — so
the bitmap overwrote the memory map it was in the middle of reading. It now
searches for a gap that is free according to that map and clear of the kernel,
the modules and the boot information, and reserves all of it (`find_span()` in
`kernel/mm/pmm.c`). Whether the old placement collided depended on how much RAM
the machine had, which is the kind of bug that boots on the machine you test on.

## Console

The console renders its own 8x16 glyphs. It captures them from the graphics
card at boot (`vga_read_font16()`), which only works while the card is still in
a text mode — plane 2 holds font data in text modes and pixels in graphics
modes. Under UEFI there is no text mode at all, so the kernel also carries a
copy of the same font (`kernel/drivers/font8x16.c`) and falls back to it.
Without that, a UEFI boot draws 256 blank glyphs: a black screen in front of a
system running perfectly well.

Which display it drives:

- On QEMU, the Bochs VBE adapter directly (`fbcon_init()`), the only path that
  can change resolution at runtime, so the `textmode` app keeps working.
- Under UEFI, the framebuffer the firmware set up, described in the Multiboot
  info by the loader (`fbcon_init_boot()`). The resolution is fixed, so the
  character grid is sized to fill it — 160x50 on a 1280x800 screen.
- On a BIOS machine that is not QEMU, plain VGA text mode, 80x25 or 80x50.

Serial output is unconditional either way: 115200 8N1 on COM1.

## The two partitions

The stick is written with two of them:

| | |
|---|---|
| 1 | FAT32, marked active and typed EFI System — the loader, the kernel and a copy of the filesystem image |
| 2 | the WFS volume itself, raw, with no filesystem in front of it |

Partition 2 is the one WOS runs on. The kernel drives the stick itself, through
its own xHCI and USB mass storage drivers, so what is written to it is still
there at the next boot — `/home/boots.txt` counts boots and keeps counting
across power cycles.

It cannot live inside the FAT partition, because there is no FAT driver here.
And the copy in `/boot/wos.img` is not redundant: it is what the loader hands
over as a RAM disk on a machine whose USB the kernel cannot drive, which is what
keeps such a machine bootable at all. The kernel prefers, in order: an ATA disk,
a USB one, then that copy in memory. The boot log says which it took —
`wfs    : mounted from the USB device`.

What the USB support does and does not cover:

- **xHCI only.** Every machine with UEFI firmware has one, and on those machines
  it drives the USB 2 ports as well. A pre-2012 machine with only EHCI or UHCI
  controllers will fall back to the RAM disk.
- **Root ports only**, no hubs. A stick in a port on the machine works; one
  behind an external hub is not found.
- **Every device is looked at, not just the first.** A keyboard on the same
  controller is asked what it is, found not to be a disk, and passed over.
- **What is plugged in at boot is what there is.** Nothing is watched for
  afterwards.

## /ramdisk

One directory is deliberately not on the stick. `/ramdisk` is held in memory and
starts empty at every boot; it is scratch space -- somewhere to put a build, a
download, a file that only matters for the next ten minutes -- and it is faster
than the stick by a wide margin.

It has no size. A file in it takes pages from free memory as it grows and gives
every one of them back when it shrinks or is deleted, so an empty `/ramdisk`
costs a few hundred bytes and a full one costs exactly what is in it. The
self-test at boot checks precisely that: 40 KiB of file takes 40 KiB of memory,
and deleting it returns all of it.

Writes there are lost when the machine stops. That is the point of it.

## Memory

`htop` and `free` report a boot from USB holding some 70 MiB, which is a lot for
a system whose kernel is under half a megabyte. Measured on a 1280x800 UEFI
machine, it is:

| | |
|---|---|
| filesystem image | 64 MiB |
| kernel heap | 8 MiB |
| framebuffer window | 4 MiB (16 MiB before this was sized to the display) |
| kernel, low megabyte, frame bitmap, boot info | ~1.6 MiB |

The filesystem image is nearly all of it, and it is not overhead: there is no
USB driver, so the image the loader read is the disk, held in memory for the
life of the boot. `make DISK_MB=16` builds a smaller one — the installed system
is about 4 MiB — and gets most of it back.

The framebuffer window costs memory for a subtler reason: it lives inside the
identity map (it has to, so the console can print no matter which process is
current), so it hides the RAM at the same physical address, which then has to be
kept from the allocator. It is now sized to the picture rather than to the
largest picture imaginable. Only the one card whose resolution can change at
runtime — QEMU's — still takes the full 16 MiB.

The heap is a fixed arena, `KHEAP_SIZE` in `kernel/include/kheap.h`.

### Scrolling

Two things about a real framebuffer that an emulated one hides, both of which
made scrolling take seconds on hardware and nothing at all under QEMU:

- **It is not memory, it is a device.** Uncached is what firmware leaves it as,
  and an uncached store is a round trip across PCIe that the CPU waits for. The
  console maps the aperture write-combining instead (`PTE_WC`, PAT entry 4), so
  stores are gathered into full-line bursts. Write-combining in the PAT beats
  uncached in the MTRRs, which is what makes this possible without touching the
  MTRRs at all.
- **Reading it is far worse than writing it.** Scrolling used to `memmove` the
  picture up one row, which read the whole screen back — 4 MiB of the slowest
  reads the machine can do, per line of output. The pixels are not moved any
  more: the character grid is shifted in ordinary RAM and only the cells whose
  contents actually changed are drawn again. On the boot log's own output that
  is about a quarter of the screen, so a scroll went from 4 MiB read plus 4 MiB
  written to under 1 MiB written and nothing read.

## When the firmware will not boot the stick

1. Check the stick really was written — `lsblk` should show one FAT32 partition
   labelled `WOS`, and `sfdisk -l /dev/sdX` should show `*` in the Boot column.
   A blank device means the flash never landed; re-run the script and read its
   output, which verifies each step.
2. Enable "USB HDD" (or the stick by name) in the firmware's boot order, and
   try the one-off boot menu rather than the fixed order.
3. Turn Secure Boot off.
4. Some firmware only boots USB devices from particular ports — try the others,
   preferring USB 2.0.

The loader reports each step on the firmware's console, so a boot that stops
says how far it got.  A complete one reads:

```
WOS: starting
WOS: kernel memory at 0x100000
WOS: boot block at 0x2ffe6000
WOS: display 1280x800 at 0xc0000000
WOS: filesystem 64 MiB loaded
WOS: entering the kernel
```

One line per firmware call that returned, so the last line on a stopped screen
names the call that did not.  Anything after the final line comes from the
kernel's own console; the firmware's is gone by then.  Every failure the loader
can detect prints a `WOS:` line of its own instead -- no display, no filesystem
image, nowhere to put the boot block.

`1 MiB is firmware memory; taking it at the handover` in place of the second
line is normal, not an error: the kernel is not relocatable and has to be
written to 1 MiB, so if the firmware is still using that memory the loader waits
until the firmware is gone and takes it then.  The write happens after
`exit_boot_services()` in every case, so it can never damage firmware that is
still running -- which would hang the machine with no console left to report it.

Past that point the kernel's own log is on screen, because under UEFI the
framebuffer console starts before anything else prints (`kmain()` brings it up
from the loader's description, through the boot page tables, before the
allocator or paging exist). A kernel that stops early therefore shows how far
it got rather than a frozen loader message.

Three waits in the kernel are bounded for the same reason -- each spins on
hardware that a modern machine may simply not have, and each ran before there
was any way to say so:

| where | what is missing | what happens now |
|---|---|---|
| `serial.c` transmit wait | no COM1, and a chipset that reads absent ports as 0x00 rather than 0xFF | the byte is dropped after a bounded spin |
| `keyboard.c` buffer drain | no PS/2 controller, so port 0x64 reads 0xFF and always claims a byte is waiting | reports `no PS/2 controller` and moves on |
| `net.c` TSC calibration | timer interrupts not reaching the CPU, e.g. routed through the IOAPIC | reports the timer is not ticking and uses a nominal figure |
