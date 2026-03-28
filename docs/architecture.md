# How WOS fits together

WOS is a 32-bit x86 kernel with paging, preemptive multitasking, ring-3
processes and a persistent filesystem. This describes how the pieces relate;
`docs/wkernel-api.md` covers the application interface and `docs/whell.md`
the shell.

## Boot

```
GRUB  ──multiboot──▶  kernel/boot.S  ──▶  kmain()
                        stack, EFLAGS       brings up everything below
```

GRUB loads the kernel at physical 1 MiB in 32-bit protected mode with paging
off. `boot.S` sets up a 16 KiB stack, clears EFLAGS (the C ABI wants the
direction flag clear) and calls `kmain` with the multiboot magic and info
pointer.

`kmain` initialises in dependency order, and the order matters:

1. **Console** — serial and VGA, so everything after this can report failure.
2. **GDT and IDT** — nothing can fault safely until the IDT is live.
3. **PIC, PIT, keyboard** — the PIC must be remapped before interrupts are
   enabled, or a plain IRQ0 arrives looking like a double fault.
4. **Memory** — frame allocator, kernel heap, paging. Deliberately after
   interrupts, so a fault here is reported instead of triple-faulting.
5. **Disk** — ATA probe, then mount the filesystem.
6. **Processes** — process table, scheduler, syscall gate.
7. **Self-tests**, then the shell.

## Memory layout

```
 virtual                                    physical
 0x00000000  ┬ unmapped (null page)
 0x00001000  │ identity mapped, supervisor   ┬ same addresses
             │   kernel image @ 1 MiB        │
             │   frame bitmap                │
             │   kernel heap (4 MiB arena)   │
             │   page tables                 │
 0x01000000  ┴ (16 MiB)                      ┘
      ...      unmapped
 0x40000000  ┬ user program                  ─ frames from anywhere
             │   .text, .rodata, .data, .bss
             │   heap, grown by wsbrk
      ...    │
 0xBFFF0000  ┴ user stack (64 KiB), grows down
```

Every address space identity maps the low 16 MiB as supervisor-only and
**shares the kernel's page tables** rather than copying them, so a mapping the
kernel makes later is visible in every address space at once.

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

## Processes and scheduling

A **process** owns an address space, a working directory and a descriptor
table. A **thread** owns a kernel stack and is what the scheduler runs. Every
process has exactly one thread today, but the split is real so threads can be
added without rewriting the scheduler.

Switching is `switch_context` in `kernel/proc/switch.S`: it saves the
callee-saved registers and flags, swaps `esp`, and unwinds the mirror image, so
its `ret` returns into whatever the other thread was doing. A brand new thread
has its stack primed by hand to look exactly like a thread that had switched
out, with the return address pointing at a trampoline that drops to ring 3.

There is no instruction that simply lowers the privilege level, so entering
ring 3 means forging the stack frame an interrupt would have pushed on entry
*from* ring 3 and letting `iret` consume it.

The scheduler is round-robin over a circular run queue, preempting every tick
(10 ms). Blocked and zombie threads stay on the queue but are skipped, so
waking a thread is a single state change. The boot context is adopted as the
idle thread, so there is always something runnable.

## Syscalls

`int 0x80`, with the call number in `eax` and arguments in `ebx`, `ecx`, `edx`.
The result comes back in `eax`; failures are the negated error code.

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
