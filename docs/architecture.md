# How WOS fits together

WOS is an x86-64 kernel with four-level paging, preemptive multitasking, ring-3
processes and a persistent filesystem. This describes how the pieces relate;
`docs/wkernel-api.md` covers the application interface and `docs/whell.md`
the shell.

## Boot, and getting into long mode

```
GRUB ──multiboot──▶ boot.S (32-bit) ──▶ boot.S (64-bit) ──▶ kmain()
                     check CPUID        reload segments
                     build PML4         set up the stack
                     PAE + LME + PG
```

GRUB loads the kernel at physical 1 MiB in **32-bit** protected mode with
paging off — that is all Multiboot 1 can do, and it is true even though the
kernel image is ELF64.

Long mode cannot be entered directly from there, because it requires paging to
be *already* enabled and paging in long mode requires a 4-level table. So the
stub runs in 32 bits just long enough to:

1. check that the processor has CPUID at all, by toggling `EFLAGS.ID`;
2. check extended leaf `0x80000001`, `EDX` bit 29 — the long mode feature bit;
3. identity map the first 1 GiB with 2 MiB pages, which fits in one page
   directory instead of the 512 page tables 4 KiB pages would need;
4. set `CR4.PAE`, set `EFER.LME`, then `CR0.PG` — at which point the CPU is in
   compatibility mode;
5. far-jump through a 64-bit code segment, which is what actually switches to
   64-bit execution.

**A CPU without long mode gets a legible message on both the VGA screen and
COM1, then halts.** There is no 32-bit fallback: the rest of the kernel is
compiled for x86-64, so there would be nothing to fall back *into*. A single
boot image cannot be both; systems that support both ship two kernels and
choose at install time.

The boot page tables are not thrown away — `paging.c` adopts them as the
kernel's own address space.

`kmain` initialises in dependency order, and the order matters:

1. **Console** — serial and VGA text mode, so the first lines can report
   failure. The authentic 8x16 font is captured from VGA plane 2 here, before
   later steps switch the console to a framebuffer.
2. **GDT and IDT** — nothing can fault safely until the IDT is live.
3. **PIC, PIT, keyboard** — the PIC must be remapped before interrupts are
   enabled, or a plain IRQ0 arrives looking like a double fault.
4. **Memory** — frame allocator, kernel heap, paging. Deliberately after
   interrupts, so a fault here is reported instead of triple-faulting. Once
   paging is up the console moves onto a linear framebuffer (see
   [`docs/console.md`](console.md)) for crisp text at real resolutions.
5. **Disk** — ATA probe, then mount the filesystem.
6. **Network** — find the RTL8139 and configure the IPv4 stack, if a card is
   present (see [`docs/networking.md`](networking.md)).
7. **Processes** — process table, scheduler, syscall gate.
8. **Self-tests**, then the shell.

## Memory layout

```
 virtual                                    physical
 0x00000000  ┬ unmapped (null page)
 0x00001000  │ identity mapped, supervisor   ┬ same addresses
             │   kernel image @ 1 MiB        │
             │   frame bitmap                │
             │   kernel heap (8 MiB arena)   │
             │   page tables                 │
 0x40000000  ┴ (1 GiB)                       ┘
 0x40000000  ┬ user program                  ─ frames from anywhere
             │   .text, .rodata, .data, .bss
             │   heap, grown by wsbrk
 0x80000000  ┼ shared memory window          ─ frames shared with another
             │   mapped by wshmmap             process
 0xB0000000  ┤
      ...    │
 0xBFFF0000  ┴ user stack (64 KiB), grows down
```

The walk is PML4 → PDPT → PD → PT, nine address bits each, 4 KiB at the bottom.

Kernel and user both live under PML4 entry 0: the identity map is PDPT entry 0
and user space begins at the second gigabyte, PDPT entry 1. So each process
gets its own PML4 **and its own PDPT**, whose first entry points at the same
page directory the kernel uses. The user bit is set on the PML4 entry, because
user pages hang beneath it, and left clear on the entry covering the identity
map — which is what keeps kernel memory out of reach of ring 3, since the CPU
ANDs the user bit across all four levels.

The identity map uses 2 MiB pages, except for the very first one: that is split
into 4 KiB pages so page zero can be left unmapped. Without that a null
dereference would quietly write to physical address 0 instead of faulting,
because a huge page covers the whole region in one entry and cannot exclude
part of it.

Two consequences fall out of that, and both simplify the rest of the system:

- Kernel pointers stay valid across a context switch, so a kernel stack can
  live in the heap.
- Loading a program can switch CR3 to the new address space first and then
  write through ordinary user addresses. No temporary mapping window is needed.

Page tables are allocated from a low-memory pool so the kernel can always reach
them through the identity map. The null page is left unmapped so a null
dereference faults instead of quietly reading the interrupt vector table.

## Interrupts

Every vector — exception, IRQ or syscall — funnels through one stub in
`kernel/arch/isr.S` that normalises the CPU's varying stack frame into a
`regs_t`, so handlers see identical state whether or not the exception carried
an error code and whether or not a privilege change happened.

The EOI is sent **before** the handler runs. The timer handler switches
threads and does not return until that thread is scheduled again, so
acknowledging afterwards would leave the PIC waiting on an interrupt that never
completes and no further IRQs would arrive.

A fault in ring 3 kills that process and says which one; a fault in ring 0
panics, because there is nothing sensible to continue with.

One general protection fault is not a fault at all. Reading a model-specific
register the processor does not implement is the only way to find out that it
does not implement it, so `kernel/arch/msr.S` puts each such instruction at an
exported address and the handler resumes at the fixup just past it. That is
what lets the kernel ask about the clock and the temperature on a machine that
has them without dying on one that has not — most often a hypervisor, which
faults on every register it was not told to emulate.

## Processors

`kernel/arch/cpu.c` answers three separate questions from three different
places.

**How many there are** comes from the ACPI processor list, and from CPUID on a
machine that has no such list. They are all started (see [more than one
processor](#more-than-one-processor)), and each is marked online as it reports
in — a core the kernel failed to start is still listed, as present and offline,
because a monitor that showed one core on an eight-core machine would simply be
wrong.

**How fast one is going** comes from APERF and MPERF, sampled on every timer
tick. MPERF counts at the base clock and APERF at whatever the core is really
being clocked at, so their ratio scales the one known frequency into the real
one. The timestamp counter cannot do this: it deliberately runs at a fixed rate
however fast the core goes, which is what makes it a good clock and useless as
a speedometer. Where the counters are missing, the base clock CPUID quotes is
reported instead, tagged as such.

**How hot it is** comes from the on-die sensor, which reports not a temperature
but a distance: how many degrees below the point at which this part throttles
itself the core currently is.

Timer ticks are counted as busy or idle depending on whether the scheduler had
anything to run on **that** core — every processor has a timer of its own, so
the answer differs between them, which is what makes a meter per core mean
something rather than repeat one number four times.

## Processes and scheduling

A **process** owns an address space, a working directory and a descriptor
table. A **thread** owns a kernel stack and is what the scheduler runs. Every
process has exactly one thread today, but the split is real so threads can be
added without rewriting the scheduler.

Switching is `switch_context` in `kernel/proc/switch.S`: it saves the
callee-saved registers and flags, swaps `rsp`, and unwinds the mirror image, so
its `ret` returns into whatever the other thread was doing. System V passes its
two arguments in `rdi` and `rsi`, so unlike the 32-bit version there is no
stack arithmetic to get wrong. A brand new thread
has its stack primed by hand to look exactly like a thread that had switched
out, with the return address pointing at a trampoline that drops to ring 3.

There is no instruction that simply lowers the privilege level, so entering
ring 3 means forging the stack frame an interrupt would have pushed on entry
*from* ring 3 and letting `iret` consume it.

The scheduler is round-robin over a circular run queue, preempting every tick
(10 ms). Blocked and zombie threads stay on the queue but are skipped, so
waking a thread is a single state change. The boot context is adopted as the
idle thread, so there is always something runnable.

The idle thread is skipped too, and reached only as the fallback when nothing
else can run. It is a member of the queue like any other thread, so taking it
in turn gave it a full timeslice between every two slices of real work — which
it spends halted until the next timer interrupt. A machine with one process to
run was idle half the time with something ready to go throughout.

That leaves a program with nothing to do no way to stand down, since yielding
in a loop is still asking to run. `wsleep()` is the answer: the thread blocks
on `WAIT_TIME` with a deadline, and the timer wakes it. Everything that waits —
a frame interval, a poll for a keystroke, a gap between pings — waits that way.

## Services

`kernel/proc/service.c` reads `/services` at boot and starts what is enabled,
before anything logs in. A unit file is `key=value` lines; unknown keys are
skipped rather than refused, so one written for a later version still starts
its service.

The description lives on the disk and the running state lives in the kernel,
which is the only place that knows whether a process is still there. A service
is spawned with no parent, so it outlives whoever asked for it.

Stopping is the interesting half. There are no signals here and no way to
unwind another thread's kernel stack from a distance, so `proc_kill()` does not
kill anything: it marks the process and wakes it, and the process leaves
through `proc_exit()` at the next moment it holds nothing — returning from a
blocking wait, entering a syscall, or being interrupted while in ring 3. The
first covers every service, since a service waits; the last covers a runaway
loop. A process interrupted *in the kernel* is left alone, because it could be
holding anything and there is nothing to unwind it with.

The service manager then waits, bounded, for the process to actually go.
Without that, `restart` would start the replacement while the original still
held the socket — the first bug this arrangement produced, and the reason the
wait is there.

The same mechanism is what [`wkill()`](wkernel-api.md#int-wkillint-pid) exposes
to programs, and what F9 in [`htop`](apps.md#htop) uses — with a check the
service manager does not need, because it has one of its own: root may stop
anything, and anybody else only what is running as them.

Something has to collect what a service leaves behind. Nothing in the kernel
calls `proc_wait()` for one, and a process that has exited keeps its slot in
the process table and its whole address space until somebody does — so a
stopped service went on being listed by `ps`, holding its memory, until the
machine went down, and thirty-two stops would have filled the table. The
kernel's idle loop reaps them, which is the part of an init's job that has to
be done even by a kernel that is its own init. The login shell is the one child
it skips: that loop watches for its exit itself, to start another.

## More than one processor

The machine boots on one processor and starts the rest at the end of the boot,
once the page tables, the heap and the process table they immediately touch all
exist. A started core repeats `boot.S` in miniature from a page copied into low
memory — real mode, a GDT, PAE, the long mode bit, paging — with one difference:
it does not build page tables, it loads the CR3 the boot processor is already
using, so the kernel is mapped the instant paging comes on. See
`kernel/arch/trampoline.S`.

Three things had to exist first, and each is small:

- **The local APIC** (`kernel/arch/lapic.c`), because a processor is started by
  sending it an interrupt, and because a core with no timer of its own would
  run the first thread it picked up forever. The 8259 and the PIT stay where
  they are, wired to the boot processor: they are the machine's clock and its
  keyboard, and one of each is enough. The one trap worth naming is that
  **enabling the local APIC puts it in front of the legacy interrupt line** —
  the 8259 then arrives on LINT0, and a masked LINT0 is a machine that halts
  with interrupts enabled and never wakes up.
- **Per-processor state** (`kernel/include/smp.h`): which thread this core is
  running, which idle thread it falls back to, which address space it has in
  CR3, and a GDT with a TSS of its own — `rsp0` is the stack the core takes its
  next ring 3 interrupt on, and two cores sharing that would take an interrupt
  onto the same stack at the same moment.
- **A lock** (`kernel/include/spinlock.h`). Until now the kernel's mutual
  exclusion was `cli`, which says nothing at all about what another processor
  is doing.

### One lock around the kernel

Every way into the kernel — a syscall, an interrupt, a fault — funnels through
`interrupt_dispatch()`, and that is where the lock is taken and released. So
kernel code still runs one processor at a time and every assumption it was
written with still holds; what runs in parallel is **user code**, which is where
the time goes. Four cores running four programs now run them at once.

It is deliberately the coarse answer. The alternative is a lock per structure —
the frame allocator, the heap, the filesystem, the console, every driver — and
getting one of them wrong is not a crash but a corruption that surfaces
somewhere else an hour later.

The lock is **handed over rather than carried across** a context switch. A core
that switched while holding it would give the kernel to a thread that never
asked for it, and a core with nothing to run would hold it while it slept, which
is the whole machine stopped behind one idle processor. So `schedule()` releases
it, switches, and takes it again on the other side — and a core with no work
releases it before it halts.

Two things make this smaller than it sounds. **Every process here has exactly
one thread**, so no address space is ever loaded on two cores at once and there
is no TLB shootdown to do. And the kernel's own mappings are shared by every
core's tables and effectively never change after boot.

What was already per-processor in spirit and had to become so in fact: the
running thread, the idle thread, the current address space, the GDT and TSS, and
the busy/idle tick counters — which is what makes [`htop`](apps.md#htop)'s meter
per core mean anything.

## Local sockets

`kernel/fs/socket.c` adds a second kind of channel beside the pipe: named,
bidirectional, and able to carry descriptors.

A pipe is one direction, anonymous, and reaches another process only by being
inherited across a spawn. That covers a shell wiring a child's output into a
terminal emulator. It does not cover a display server, where a client has to
find the compositor by name having never been its child, talk both ways, and
hand over a descriptor for a buffer of pixels rather than a copy of them. So:
Unix domain sockets, in the shape WOS needs them.

The address is a path but not a file. Nothing is created on the disk; the path
is the name a listener answers to, and it disappears when the listener closes.
Binding one needs write permission where it lives, which is a rule that already
existed: answering to a name in a directory is a kind of writing there.

**A socket has an owner**, and another user cannot connect to it. That rule was
missing at first, on the reasoning that the write permission needed to bind one
was permission enough. It was not, and the difference is what a socket *is*: a
file is bytes, and a socket is a way into the process behind it. Sockets on
this machine live in `/ramdisk`, which everybody can write, and the compositor
listens on one that takes commands — so any user's program could have told
another user's sway to run a program, and it would have run it as them. The
Wayland socket beside it is worse: a client that binds `wl_seat` is told every
keystroke.

So `socket_connect()` compares the caller's uid with the uid that listened and
refuses anyone else with `-W_EPERM`; root is not stopped, because root can
already become anybody. The check is in the one place every connection goes
past rather than in each program that listens, because a program that forgot it
would be a hole in the machine rather than a bug in itself.

**Descriptor passing** is the part worth being careful about. A `file_t` is a
reference to something — a pipe end, an open file, a socket endpoint — so
passing one is copying the struct and taking a reference, which is what
`vfs_fd_retain()` and `vfs_fd_drop()` exist for. The copy that lands in the
receiver is an ordinary descriptor: either process may close its own without
disturbing the other.

Each queued descriptor records the sender's byte position when it was queued,
and is released to the receiver only once that byte has been read. That is what
keeps a descriptor from arriving before the message that explains it, however
small the pieces the receiver reads in.

`W_POLLOUT` promises room for `W_SEND_CHUNK` bytes rather than for one. "A
write would not block" is only true of a write that fits, and without a size in
the promise two programs that each poll before writing can still deadlock on
each other, both having been told to go ahead.

## Shared memory

A socket copies what it carries. That is right for messages and wrong for a
window: 640x400 is a megabyte, and a client redrawing it sixty times a second
would push sixty megabytes a second through a 4 KiB buffer for the compositor
to copy a second time.

`kernel/mm/shm.c` is the answer, and it is the reason descriptor passing was
built first. An object is a fixed set of frames with a descriptor naming it;
either side maps it, and the descriptor travels over the socket while the pages
stay where they are. That is `wl_shm` exactly.

Two things it has to be careful about:

- **The frames are not the address space's to free.** `paging_unmap_keep()`
  takes a page out without giving the frame back, and a process drops its
  mappings in `proc_exit()` before its space is torn down — the teardown frees
  every frame it finds mapped, and half of these belong to somebody else.
- **The window is its own range.** A pool is mapped and unmapped in the middle
  of a program's life while the heap only grows from one end, so they cannot
  share a range; `wsbrk()` refuses to grow past `USER_MMAP_BASE`.

An object outlives its makers: it goes when the last descriptor closes *and*
the last mapping is dropped. A client may draw a buffer, hand it over and exit,
leaving the compositor holding pixels that are still good.

## The screen, the keyboard and the mouse

All three are single devices that the console has until something takes them,
and the first two are lent to one process at a time — which is what a compositor needs and
the reason either mechanism exists.

`kernel/drivers/display.c` holds the whole of the screen's ownership model in
one pid. While the screen is lent the console keeps running and stops drawing:
it still tracks the cursor, still scrolls, still records every character, so
when it takes the screen back it repaints and nothing printed behind the
compositor was lost. Pixels go out through `wdisplayblit()` rather than by
mapping the framebuffer into the process — the copy is the one that was going
to happen anyway, and the rectangle gets clipped by somebody who knows where
the aperture ends.

The keyboard grows a third discipline above canonical and raw. Both of those
turn keys into text, which throws away the two things a compositor needs: that
a key was *released*, and which physical key it was regardless of what it
prints. Event mode reports every transition with the Linux evdev code — which
for the main block of a PS/2 keyboard is the AT set 1 scancode it was defined
from, so no translation table is needed — and the modifier mask in XKB's bit
positions. Those are the numbers `wl_keyboard.key` and `wl_keyboard.modifiers`
carry, unchanged.

The mouse goes into that same stream rather than a stream of its own. A
compositor wants the two devices interleaved in the order they happened: a
click that arrived before the motion that led to it would land on whatever used
to be under the pointer, and two rings could only promise that by being merged
on timestamps at the far end. `winput_t` carries a position and a delta
alongside the key fields, and the event type says which of them mean anything.

The pointer's absolute position is kept by the kernel, because only the kernel
knows how big the screen is — and a pointer that can leave the screen is one
nobody can bring back. `kernel/drivers/mouse.c` clamps it and reports both the
new position and the movement that caused it, so the compositor can draw a
cursor without tracking anything and a client can be told a distance without
knowing where the screen ends.

That is also why the pointer's *speed* is the kernel's — `wpointerspeed()`,
which sway drives from `input * pointer_accel`. The counts a mouse reports
become pixels in exactly one place, and a compositor that scaled them itself
would draw its cursor where the kernel's pointer was not. The scaling keeps
what its rounding left over, because half of a pixel dropped on every packet is
a pointer that will not move at all below half speed. It is a multiplier and
not a curve: acceleration would need the interval between packets to mean
something, and on a mouse sampled at whatever rate the firmware left it that
interval is not a speed.

Taking the screen or the keyboard affects every process on the machine, so
both need root or a seat granted by root — see
[`docs/users.md`](users.md). Both come back when the holder exits, however it
exits: a compositor that faults must not take the machine's only screen with
it.

## The firmware's bytecode

Some of what the firmware knows is data and some of it is a program. The S5
sleep type is data — a package of constants in a fixed shape, which is why
`acpi.c` reads it with a byte scan. A battery's charge is not: it comes back
from `_BST`, a method that reads the embedded controller, does arithmetic on
what it finds, and returns a package. There is no way to that number except to
run it.

`kernel/acpi/` is an AML interpreter for exactly that. A load pass builds a
namespace out of the DSDT and every SSDT; an evaluator runs methods against it,
with integers, buffers, strings and packages, the arithmetic and the logic,
`If`, `While` and `Return`, and fields cut out of operation regions at bit
granularity.

The embedded controller is what makes it worth having. Its command interface is
architectural — port `0x62` for data, `0x66` for command, `0x80` to read, on
every machine ever built — while the offset the charge lives at is
firmware-specific and comes out of the DSDT. The standard describes the
transport and the firmware describes the layout, and between them nothing is
left to guess.

A method body is located at load time and not read, so nothing touches hardware
while the machine is still being described. An unimplemented construct is
stepped over and counted rather than failing the table: a table abandoned at
the first one loses every name after it, including — on a laptop whose battery
is declared late — the battery.

## The display server

Everything above the kernel is in user space, and none of it is privileged
beyond taking the two devices.

```
   sway                       wlterm
   ├─ the screen (root)       ├─ a wl_surface with an xdg_toplevel
   ├─ the keyboard (root)     ├─ pixels in shared memory
   ├─ wl_compositor, wl_shm,  └─ struct wterm: a shell on two pipes
   │  xdg_wm_base, wl_seat,          │
   │  wl_output                      │
   └─ i3 IPC on a socket             ▼
              ▲                   whell
              │
           swaymsg
```

`lib/wkernel` carries a libwayland-shaped protocol library — proxies and
listeners on the client side, globals and resources on the server side, with
one signature-driven marshaller underneath both. The interface tables are
transcribed from `wayland.xml` and `xdg-shell.xml`, because an opcode is a
position in a list and a signature is a message's wire format: a list in the
wrong order produces a client that asks for a region when it meant a surface.

The one thing upstream needs that is not available here is libffi, which
libwayland uses to call a listener whose shape is known only to the client. It
turns out not to be needed: every Wayland argument is a 32-bit integer or a
pointer, x86-64 passes both identically, so a handler can be called through one
function type wide enough for the longest event with the callee ignoring the
slots it did not declare. A floating-point argument would break it, and the
protocol has none — which is exactly why `wl_fixed_t` is an integer.

## Syscalls

`int 0x80`, with the call number in `rax` and arguments in the System V
registers `rdi`, `rsi` and `rdx`. The result comes back in `rax`; failures are
the negated error code.

There is no `pusha` in long mode, so the interrupt stub pushes all fifteen
general registers by hand. In exchange the CPU always pushes `rsp` and `ss` on
a 64-bit interrupt, whether or not the privilege level changed, so the saved
frame is one fixed shape rather than two.

Every pointer arriving from ring 3 is validated against the calling process's
page tables before the kernel touches it. The MMU stops ring 3 *reaching*
kernel memory, but nothing stops it *passing a kernel address as an argument*,
which the kernel would then dereference with full privilege.

## Storage

```
 wfs_* ── the filesystem: inodes, directories, block allocation
   │
 ata_* ── polled PIO reads and writes of 512-byte sectors
   │
 qemu -drive file=build/wos.img
```

The on-disk format (`include/wfs.h`, shared verbatim with the host `mkwfs`
tool, so the two cannot drift apart):

| Region | Contents |
|---|---|
| block 0 | superblock: magic, geometry, free counts |
| bitmap | one bit per block — the authority for used and free disk space |
| inode table | 64-byte inodes, 16 per block |
| data | file and directory contents |

Blocks are 1 KiB. A file reaches 268 KiB through 12 direct blocks and one
indirect block. Directories are files holding fixed 32-byte records, so
reading one needs no variable-length parsing, and `.` and `..` are real entries
so path resolution has no special cases.

Bitmap and superblock changes are written straight back rather than cached:
there is no fsck here, so a volume that loses its free list on a hard reset is
simply corrupt. Writes are followed by FLUSH CACHE, without which data can sit
in the drive's write cache and be lost.

Above WFS, `kernel/fs/vfs.c` owns per-process descriptor tables, routes
descriptors 0–2 to the console, and resolves relative paths and `.`/`..`
against the working directory — so WFS itself only ever sees normalised
absolute paths.

## Self-tests

The kernel runs its self-tests on every boot rather than in a separate
harness. An OS has no test runner to fall back on, and a broken allocator or a
missing timer tick is far cheaper to find at the layer that introduced it than
three subsystems later. They cover frame accounting, heap split and coalesce,
address-space create/map/write/teardown, filesystem read/write/delete, and
spawning ring-3 processes.

`/home/boots.txt` is a boot counter: it climbs by one on every boot, which is
the standing proof that writes really reach the disk.
