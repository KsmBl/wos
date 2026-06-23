# WOS

A small operating system written from scratch in C, bootable under QEMU.

WOS is an x86-64 kernel that boots itself into long mode, with 4-level paging,
preemptive multitasking, ring-3 processes, a persistent filesystem on a real
disk, and a documented application API (`wkernel`). Its first application is a
shell called **whell**.

## Quick start

```sh
make          # build the kernel, the apps, the bootable ISO and the disk image
make run      # boot it in QEMU (VGA window, serial log on stdio)
make log      # boot headless for a few seconds and dump the serial log
make clean

sudo tools/flash-usb.sh   # put it on a USB stick and boot it on real hardware
```

Requirements: `gcc`, `binutils`, `grub-mkrescue` with the `i386-pc` platform
modules, `xorriso`, `mtools` and `qemu-system-x86_64`; `dosfstools` to write a
USB stick.
**No cross-compiler and no nasm are needed** — the host `gcc -m64` builds the
freestanding kernel and GAS assembles the `.S` files.

The CPU must support long mode. One that does not gets a legible message rather
than a triple fault; see [`docs/architecture.md`](docs/architecture.md).

## Layout

| Path | Contents |
|---|---|
| `kernel/` | the kernel: boot, arch (GDT/IDT/PIC/PIT), drivers, memory, filesystem, processes |
| `lib/wkernel/` | the application API — `wkernel.h` plus the library applications link against |
| `app/<name>/sourcecode/` | source for each application; the build installs its binary to `/app/<name>/launch` on the disk |
| `uefi/` | the UEFI loader — a PE32+ application with the kernel embedded, for firmware GRUB cannot hand over on |
| `tools/` | host-side tools, notably `mkwfs` which builds the disk image |
| `docs/` | API and shell documentation |

## On-disk layout

Every application owns a directory under `/app`:

```
/app/whell/launch            the executable
/app/whell/sourcecode/*.c    its source
```

A bare command name in `whell` resolves to `/app/<name>/launch` — that rule
*is* the search path, so there is no `PATH` variable to maintain.

## What it does

```
wos:/home$ ls
boots.txt   notes.txt   readme.txt
wos:/home$ free -h
          total        used        free
Mem:     255.8M        5.2M      250.6M
Swap:        0B          0B          0B
wos:/home$ hello
hello: pid 7, argc 1 argv[0]=hello
hello: RAM 250.5M free of 255.8M (kernel holds 5.1M)
hello: I am resident in 88.0K (code 8.0K, data 8.0K, stack 64.0K)
```

- An x86-64 kernel: a 32-bit Multiboot stub checks CPUID for long mode and
  jumps into 64-bit, then four-level paging with a per-process address space, a
  preemptive round-robin scheduler, ring-3 processes loaded from ELF64
  binaries, and `int 0x80` syscalls with every user pointer validated.
- **USB mass storage**: an xHCI driver and the bulk-only transport, so a machine
  that boots from a USB stick reads and writes that stick directly. See
  [`docs/usb.md`](docs/usb.md).
- **`/ramdisk`**, a filesystem held in memory that grows and shrinks with what
  is in it, over a disk that holds everything else.
- **WFS**, a filesystem on a real disk image — reads and writes persist across
  reboots, and the disk figures come from its block bitmap.
- **wkernel**, the documented application API: file and directory I/O, memory
  and disk statistics, process control, `printf`, `malloc`.
- **whell**, a shell with tab completion and line editing. It is deliberately
  tiny: only `cd`, `exit` and `help` are builtins, because only those change
  state belonging to the shell process.
- **Commands as programs**: `ls`, `pwd`, `cat`, `free`, `df`, `ps`, `touch`,
  `mkdir`, `rm`, `clear` and `shutdown` each live in `/app`, behaving as they
  do on Linux. Both shells run the same ones.
- **The hardware, honestly**: `cpufreq` reads the processor's clock and holds
  it at a speed; `battery` says what the firmware knows about the pack. Both
  report what the machine will not tell them as unknown, rather than as a
  number nobody measured.
- **Services**: programs the machine runs rather than a person does, described
  by unit files in `/services` and managed with `systemctl` — list, start,
  stop, enable, disable. The first one is `wayland`.
- **The beginning of Wayland**: local sockets that carry file descriptors, and
  `waylandd`, a display server speaking the real wire format. A client can
  complete a `wl_display` roundtrip today; the registry is empty until there is
  something to put in it.
- **Users and roles**: every process runs as a user, `root` may do anything,
  and everyone else writes only in their own `/home` directory unless a role
  says otherwise — `appeditor` for `/app`, `usereditor` for `/userconfig`,
  `editfreq` for the processor's clock, `systemctleditor` for the services.
  `/kernel` is root-only. Passwords live in `/userconfig/<name>/password`,
  readable and writable by root and the kernel alone, so `passwd` and `su`
  need no setuid.
- **Applications**: `fish` (a shell that colours commands as you type and
  suggests from history), `vim` (a modal editor), `htop` (a live process
  monitor, with a meter per core and per filesystem) and `fastfetch` (system
  information). These are WOS-native programs
  in the spirit of the originals — see [`docs/apps.md`](docs/apps.md) for what
  the upstream versions would need that WOS does not have.

## Documentation

- [`docs/wkernel-api.md`](docs/wkernel-api.md) — every application-facing function,
  with parameters, return values, errors and examples
- [`docs/whell.md`](docs/whell.md) — the shell and its builtins
- [`docs/users.md`](docs/users.md) — users, roles, passwords and who may write where
- [`docs/apps.md`](docs/apps.md) — the commands (`ls`, `free`, `rm`, ...) and
  fish, vim, htop, fastfetch
- [`docs/console.md`](docs/console.md) — raw input mode and the ANSI escape
  sequences full-screen programs use
- [`docs/architecture.md`](docs/architecture.md) — how the kernel fits together
- [`docs/usb.md`](docs/usb.md) — putting WOS on a USB stick and booting it on
  real hardware
- [`docs/building.md`](docs/building.md) — building, running, debugging, and
  adding an application

## Testing

The kernel runs self-tests on every boot — frame accounting, heap
split/coalesce, address-space teardown, filesystem read/write/delete, and
spawning ring-3 processes — because an OS has no test runner to fall back on.
`/home/boots.txt` counts boots, which is the standing proof that writes reach
the disk. `make SELFTEST=0` builds without them, for a boot that goes straight
to the shell.

Interactive behaviour is tested with `tools/keytest.sh`, which types into QEMU
through the monitor:

```sh
./tools/keytest.sh 'pwd' 'ls -l' 'free -h'
```

## Status

Built in seven stages, each on its own branch and merged into `master` only
once it booted and passed its checks. See `git log --graph` and the `stage*`
tags.
