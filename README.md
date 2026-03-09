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

## Documentation

- [`docs/wkernel-api.md`](docs/wkernel-api.md) — every application-facing function
- [`docs/whell.md`](docs/whell.md) — the shell and its builtins
- [`docs/architecture.md`](docs/architecture.md) — how the kernel fits together

## Status

Built in stages, each on its own branch and merged into `master` only once it
boots and passes its checks. See `git log --graph` and the `stage*` tags.
