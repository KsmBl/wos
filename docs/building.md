# Building and running WOS

## Requirements

| Tool | Why |
|---|---|
| `gcc` | builds the kernel, the library and the applications |
| `binutils` | `ld` must be able to emit `elf_x86_64`; `ar` builds the library |
| `grub-mkrescue` with the `i386-pc` platform | makes the bootable ISO |
| `xorriso`, `mtools` | used by `grub-mkrescue` |
| `qemu-system-x86_64` | runs it |

**No cross-compiler and no nasm are needed.** The host `gcc -m64` builds
freestanding 64-bit code and GAS assembles the `.S` files.

The target CPU must support long mode. Every x86-64 processor does, and so does
QEMU's default model.

Check the toolchain with:

```sh
echo 'int main(void){return 0;}' > /tmp/t.c && gcc -m64 -c /tmp/t.c -o /tmp/t.o
ls /usr/lib/grub/i386-pc >/dev/null && echo "grub i386-pc present"
```

## Targets

```sh
make            # kernel, library, applications, ISO and disk image
make kernel     # just build/kernel.elf
make lib        # just build/lib/libwkernel.a
make apps       # just the application binaries
make iso        # just build/wos.iso
make efi        # just build/BOOTX64.EFI, the UEFI loader
make disk       # just build/wos.img
make run        # boot in QEMU, with a VGA window and the serial log on stdio
make run-nox    # boot headless, serial only
make log        # boot headless for TIMEOUT seconds, capture the serial log
make debug      # boot stopped, waiting for gdb on :1234
make clean
```

To boot it on a real machine instead of QEMU, `sudo tools/flash-usb.sh` writes
a bootable USB stick; see [`usb.md`](usb.md). That needs GRUB's `i386-pc`
modules (which the ISO already requires) and `dosfstools` for `mkfs.vfat`.
Nothing extra is needed for UEFI: WOS carries its own loader, built from
`uefi/` by the host `gcc` like everything else.

`make log` exists because piping `-serial stdio` through a timeout loses
output to buffering; it writes to `build/serial.log` and prints that instead.

```sh
make log TIMEOUT=20
```

## Build options

```sh
tools/configure.sh     # pick them from a menu, then build
```

Ticks the on/off ones, asks for the others, and writes `config.mk` -- which is
the file `make` reads, so a setting changed here and one changed by hand in the
file are the same setting. `tools/configure.sh --show` prints them without
opening the menu.

Or set them directly, in `config.mk` for good or on the command line for one
build:

```sh
make SELFTEST=0        # build without the boot-time self-tests
make DISK_MB=16        # a smaller filesystem image (default 64)
make config            # print what is currently set
```

The self-tests are the four blocks of `[ok  ]` lines the boot prints; they take
a few seconds and spawn a process that faults on purpose, which is alarming to
watch if you did not expect it. `SELFTEST=0` leaves them out of the build
rather than skipping them at runtime, so `kernel/selftest.c` compiles to nothing
and the kernel is about 40 KiB smaller. The boot goes straight from the driver
log to the shell.

`DISK_MB` sizes the image `make run` boots from. It is not the size of a
flashed USB stick — `flash-usb.sh` gives the filesystem the whole stick — and
it is not the size of the fallback copy in `/boot` either, which that script
caps at 64 MiB because the loader has to place it in low memory. See
[`usb.md`](usb.md#memory).

Changing either setting rebuilds what depends on it: every kernel object for
`SELFTEST`, the image for `DISK_MB`. Neither can be noticed from a timestamp,
since no source file changes, so a stamp file named after the current value
stands in for one.

Two things that are easy to expect from this and do not follow: it does not
change what is installed on the disk image (the `hello` program the process
test uses is an ordinary application and stays), and it does not remove the
boot counter in `/home/boots.txt` — that is written by the filesystem test, so
it simply stops advancing.

## Testing interactive behaviour

The PS/2 keyboard cannot be driven from a pipe, so `tools/keytest.sh`
translates ASCII into QEMU `sendkey` commands and feeds them to the monitor:

```sh
./tools/keytest.sh 'pwd' 'ls -l' 'free -h'
./tools/keytest.sh -d 10 -t 90 'cd /app' 'ls' 'hello'
```

Each argument is typed and followed by Enter. `-d` sets how long to wait
before typing (default 6 seconds, enough for the kernel's self-tests) and `-t`
the overall QEMU timeout.

## Debugging

```sh
make debug                                   # in one terminal
gdb build/kernel.elf -ex 'target remote :1234'   # in another
```

The kernel is built with `-g`, so symbols and source line numbers work.

For a fault so early that nothing prints, ask QEMU what the CPU did:

```sh
qemu-system-x86_64 -cdrom build/wos.iso -d int,cpu_reset -no-reboot -display none
```

Otherwise the serial log is the first place to look: everything the kernel
prints goes to both the VGA console and COM1, and the page-fault handler
reports the faulting address, direction, ring and register state.

## Build flags that matter

Three of these are not optional, and getting them wrong fails in ways that are
hard to diagnose:

| Flag | Why |
|---|---|
| `-fno-pie -fno-pic`, `ld -no-pie` | this host's gcc defaults to PIE, and a position-independent kernel triple-faults immediately at 1 MiB |
| `-mno-red-zone` | not optional in a kernel: an interrupt would otherwise land on the 128 bytes below `rsp` that a leaf function assumes are its own |
| `-mcmodel=small` | holds because the whole kernel sits inside the identity-mapped first 1 GiB, so every symbol address fits in 32 bits |
| `-mno-sse -mno-sse2 -mno-mmx -mno-80387` | the kernel never enables those units, so any instruction gcc emits for them faults |
| `-ffreestanding`, plus our own `memcpy`/`memset`/`memmove`/`memcmp` | gcc emits calls to those four for struct assignment even when freestanding, so they must exist under exactly those names |

The link step runs `grub-file --is-x86-multiboot` so a broken multiboot header
fails the build rather than producing an ISO that GRUB silently refuses.

Assembly objects are built to `.asm.o`. A `foo.c` and a `foo.S` in the same
directory would otherwise both produce `foo.o`, and one would silently
overwrite the other.

## The disk image

`make` rebuilds `build/wos.img` from a staging tree: everything under
`rootfs/` verbatim, plus each application installed as `/app/<name>/launch`
with its source copied to `/app/<name>/sourcecode/`.

Rebuilding discards anything WOS wrote to the image — which is what you want
from a build, but it means checking persistence requires rebooting *without*
running `make` in between:

```sh
make            # boot counter resets
make log        # "this is boot number 1"
make log        # "this is boot number 2"
make log        # "this is boot number 3"
```

## Adding an application

1. `mkdir -p app/<name>/sourcecode`
2. Write C there, including `<wkernel.h>` and defining
   `int main(int argc, char **argv)`.
3. `make`

It is compiled as an ELF64 binary, linked against `libwkernel.a` at
`0x40000000`, and installed at
`/app/<name>/launch`. In whell, type `<name>` to run it.

`app/hello/sourcecode/hello.c` is a worked example that exercises most of the
API.
