# WOS

A small operating system written from scratch in C, bootable under QEMU.

WOS is a 32-bit x86 kernel with paging, preemptive multitasking, ring-3
processes, a persistent filesystem on a real disk, and a documented application
API (`wkernel`). Its first application is a shell called **whell**.

## Quick start

```sh
make          # build the kernel, the apps, the bootable ISO and the disk image
make run      # boot it in QEMU (VGA window, serial log on stdio)
make log      # boot headless for a few seconds and dump the serial log
make clean
```

Requirements: `gcc` with 32-bit multilib, `binutils`, `grub-mkrescue` with the
`i386-pc` platform modules, `xorriso`, `mtools` and `qemu-system-i386`.
**No cross-compiler and no nasm are needed** — the host `gcc -m32` builds the
freestanding kernel and GAS assembles the `.S` files.

## Layout

| Path | Contents |
|---|---|
| `kernel/` | the kernel: boot, arch (GDT/IDT/PIC/PIT), drivers, memory, filesystem, processes |
| `lib/wkernel/` | the application API — `wkernel.h` plus the library applications link against |
| `app/<name>/sourcecode/` | source for each application; the build installs its binary to `/app/<name>/launch` on the disk |
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

- A 32-bit x86 kernel: paging with a per-process address space, a preemptive
  round-robin scheduler, ring-3 processes loaded from ELF binaries, and
  `int 0x80` syscalls with every user pointer validated.
- **WFS**, a filesystem on a real disk image — reads and writes persist across
  reboots, and the disk figures come from its block bitmap.
- **wkernel**, the documented application API: file and directory I/O, memory
  and disk statistics, process control, `printf`, `malloc`.
- **whell**, a shell with `ls`, `free`, `cd` and `pwd` behaving as they do on
  Linux, plus `df`, `ps`, `cat` and `help`.

## Documentation

- [`docs/wkernel-api.md`](docs/wkernel-api.md) — every application-facing function,
  with parameters, return values, errors and examples
- [`docs/whell.md`](docs/whell.md) — the shell and its builtins
- [`docs/architecture.md`](docs/architecture.md) — how the kernel fits together
- [`docs/building.md`](docs/building.md) — building, running, debugging, and
  adding an application

## Testing

The kernel runs self-tests on every boot — frame accounting, heap
split/coalesce, address-space teardown, filesystem read/write/delete, and
spawning ring-3 processes — because an OS has no test runner to fall back on.
`/home/boots.txt` counts boots, which is the standing proof that writes reach
the disk.

Interactive behaviour is tested with `tools/keytest.sh`, which types into QEMU
through the monitor:

```sh
./tools/keytest.sh 'pwd' 'ls -l' 'free -h'
```

## Status

Built in seven stages, each on its own branch and merged into `master` only
once it booted and passed its checks. See `git log --graph` and the `stage*`
tags.
