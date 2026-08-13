# Compiling WOS on WOS

The goal: open `/app/ls/sourcecode/ls.c` in `vim` on the running machine, change
it, type `make`, and run the result. Nothing about that is exotic on Linux and
none of it is possible here yet, for three specific reasons — each of which is a
piece of work rather than a difficulty.

## What is actually missing

**There is no assembler and no linker.** Every application in this tree is
compiled on a host and installed as a finished ELF64 executable at
`/app/<name>/launch`. A compiler that emits assembly — chibicc, 8cc, cproc —
would need `as` and `ld` ported beside it before it could produce anything this
machine can run. **TCC** is the choice because it does not: it compiles and
links in one process and writes the executable itself.

**The library programs link against is built on the host.** `libwkernel.a` and
the headers in `include/` and `lib/wkernel/include/` exist only in the build
tree. `wprintf` cannot be resolved on the machine because nothing on the machine
has ever heard of it.

**The filesystem stores no times.** `wstat_t` is `ino`, `size`, `blocks`,
`type`. A `make` decides what to rebuild by comparing when things changed, and
there is nothing here to compare.

And two smaller facts worth having in mind: applications are not all one file
(`app/vim/sourcecode` holds `vim.c`, `buffer.c` and `vim.h`), so the per-app
Makefiles have to handle several objects and a link step; and this machine has
256 MB, which TCC compiling a large translation unit will notice.

## The order to build it in

Each step is useful on its own and testable before the next one starts, which
is the point of the order: nothing here has to wait for the compiler to be
finished before it can be known to work.

### 1. Times in WFS

Add a modification time to the on-disk inode, set it wherever an inode is
written — create, write, truncate, rename — and carry it out through `wstat()`
into `wstat_t`. The clock already exists (`kernel/arch/rtc.c`), and the disk
format is this project's own, so this is an added field rather than a
negotiation.

It changes the on-disk layout: `tools/mkwfs.c` writes it, and an image built by
an older `mkwfs` has to be either rebuilt or read with the field treated as
zero. Decide which and say so in `docs/architecture.md`.

Falls out for free: `ls -l` can show dates instead of leaving the column out.

**Done when:** a file written on the machine reports a time from `wstat()` that
matches the clock, and the time changes when the file is written again.

### 2. `make`

A real subset, in `app/make`: variables and `$(VAR)`, targets with
prerequisites, tab-indented recipes, `$@` and `$<`, and staleness decided by the
times from step 1. Recipes are run the way `whell` runs a command line, because
that is the only way to run anything here.

It needs nothing from the compiler and can be checked against the commands
already on the machine — a Makefile whose recipe is `touch` and whose
prerequisite is a file that `vim` just saved.

**Done when:** a target rebuilds after its prerequisite changes and does not
rebuild when nothing has.

### 3. The toolchain on the disk

Install `libwkernel.a` and every header it needs into the image — `/lib` and
`/include` are the obvious homes — and have the build put them there the same
way it installs each app's source beside its binary. No new code; a change to
the `rootfs` rules in the `Makefile`.

**Done when:** the files are on the machine and `ls /include` shows
`wkernel.h`.

### 4. The C library TCC expects

TCC is written in C and uses a hosted library: `FILE *` and `fopen`, `fread`,
`fwrite`, `fseek`, `ftell`, `fprintf`, `vsnprintf`, `qsort`, `strtol`,
`realloc`, `setjmp`/`longjmp`. Some of that is already in `wkernel` under its
own names and some of it is not there at all.

This is the largest quiet piece of the work, and every function in it is
testable on its own the day it is written — which is the only reason it is not
also the riskiest.

**Done when:** a program written against the shim rather than against `wkernel`
compiles on the host and runs on the machine.

### 5. TCC

Its sources under `app/tcc/sourcecode`, its ELF writer taught what this loader
expects, and its memory use watched. Milestones, in the order they will happen:

1. it compiles and runs `hello.c`;
2. it compiles a real command out of this tree — `pwd`, then `ls`;
3. it compiles something with several files in it — `vim`.

**Done when:** a binary it produced replaces one in `/app` and the machine keeps
working.

### 6. A Makefile in every `sourcecode` directory

Generated, once `make` and the compiler agree on flags, so that all fifty-one of
them say the same thing and none of them drifts. The scenario that proves the
whole chain: boot the machine, edit a source file in `vim`, run `make`, run the
program, and see the change — which is the sentence this document started with.

## Why this order

The compiler is the largest piece and the last one, deliberately. Every step
before it is independently useful: times make `ls -l` honest, `make` is worth
having even when the only recipes are shell commands, and the library shim is
what any port would need. If the compiler turns out to be a longer fight than it
looks, everything under it still stands on its own.
