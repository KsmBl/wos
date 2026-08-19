# Compiling WOS on WOS

The goal: open `/app/ls/sourcecode/ls.c` in `vim` on the running machine, change
it, type `make`, and run the result. Nothing about that is exotic on Linux and
none of it was possible here, for three specific reasons — each of which is a
piece of work rather than a difficulty.

**All six steps below are done, and the sentence above is now something you can
do.** The filesystem keeps times, `make` is on the machine, the library and its
headers are on the disk, there is a hosted C library, every application's source
has the Makefile that rebuilds it — and there is a C compiler, which compiles
itself.

## What was actually missing

~~**There is no assembler and no linker.**~~ There is still neither, and there
does not need to be: `wcc` writes machine code rather than assembly text and
links the objects itself, which is the same shape TCC has and the reason TCC was
the plan. Step 5 is what changed — it is written rather than ported, and
[`docs/wcc.md`](wcc.md) is the compiler.

~~**The library programs link against is built on the host.**~~ It is on the
machine now, in `/lib` and `/include`, with a hosted C library beside it — steps
3 and 4.

~~**The filesystem stores no times.**~~ It does now — step 1 — and `make` reads
them, which is step 2.

And two smaller facts that turned out to matter exactly as expected:
applications are not all one file (`app/vim/sourcecode` holds `vim.c`,
`buffer.c` and `vim.h`), so the per-app Makefiles handle several objects and a
link step — step 6 generates them; and this machine has 256 MB, which the
compiler notices only in that it never frees anything and never needs to.

## The order to build it in

Each step is useful on its own and testable before the next one starts, which
is the point of the order: nothing here has to wait for the compiler to be
finished before it can be known to work.

### 1. Times in WFS — **done**

`struct wfs_inode` carries `mtime`, seconds since 1970, stamped from the CMOS
clock (`rtc_epoch()`, in `kernel/drivers/rtc.c`) wherever an inode is written:
created, written, truncated. A rename stamps the two directories rather than the
file, because a file that moved is not a file that changed. `/ramdisk` keeps the
same field in its nodes, so the two filesystems answer the same question the
same way, and `vfs_stat()` carries it out into `wstat_t.mtime`.

The field had to come out of the direct block pointers — the inode is 64 bytes
so that sixteen fit in a block, and it was exactly full — so a file reached
267 KiB rather than 268. That moved everything after it, which is why the
volume magic went from `WFS1` to `WFS2`: an older image is refused at mount
with a line saying to rebuild it, rather than read with its block numbers one
field out of place. `docs/architecture.md` says so.

(The same trade happened again later, for a double-indirect block that took the
largest file from 267 KiB to 64 MiB, and the magic to `WFS3`.)

Two things fell out. `ls -l` shows dates. And `touch` on a file that already
exists does what its name says, through a new `wutime()` — a file with no
contents to write still has to be able to say it changed, or nothing could tell
`make` that something did.

**Done:** `make check ARGS=times` sets the clock to a date nothing else could
produce, writes a file, and finds that date in `ls -l`; then writes it again at
a different time and finds the new one. The boot-time self-test checks the same
thing one layer down.

### 2. `make` — **done**

`app/make`, in three files and about eight hundred lines: variables (`=`, `:=`,
`+=`) and `$(VAR)`, rules with several targets and prerequisites, tab-indented
recipes, `$@`, `$<` and `$^`, `@` and `-` prefixes, `.PHONY`, `-f`, `-C`, `-n`,
`-s`, `-B`, and `VAR=value` on the command line. Staleness is the times from
step 1, and a target with no time at all counts as needing a rebuild — the safe
direction to be wrong in.

Recipes run through `whell -c`, one process per line, whatever the user's login
shell is: a recipe is written against one syntax and has to get the one it was
written for.

There is a worked example on the machine at `/home/root/example`, whose recipes
are `cat` and `touch` because that is what there is to build with so far. When
there is a compiler those lines become a compile and a link, and nothing else
about the Makefile changes.

**Done:** `make check ARGS=make` builds a target that is not there, gets told
there is nothing to do the second time, touches what it is built from and
watches it rebuild — and the same again with the change made in `vim` rather
than by `touch`, which is the sentence this document opens with, minus the
compiler.

### 3. The toolchain on the disk — **done**

`/lib` holds `libwkernel.a`, `libc.a` and `user.ld`; `/include` holds
`wkernel.h`, `wabi.h`, the Wayland and drawing headers, and the hosted C
library's. The `rootfs` rules in the `Makefile` put them there, and no new code
was needed.

One thing that had to be learnt rather than assumed: `libwkernel.a` is 347 KiB
with its debug information, and WFS held 267 KiB in one file at the time, so
the archive is stripped on the way in exactly as the binaries are. It is
117 KiB after that. WFS holds 64 MiB in a file now, but the archive is still
stripped — a third of a megabyte of debug information is dead weight on the
disk whether or not it would fit.

**Done:** `ls /include` and `ls /lib` on the machine, in
`make check ARGS=toolchain`.

### 4. The hosted C library — **done**

Written for TCC and used by the compiler that replaced it.
`lib/wlibc`, built as `libc.a` because that is the name a compiler looks for,
and documented in [`docs/libc.md`](libc.md): `FILE` and the whole of stdio with
real buffering, `qsort`, `bsearch`, `strtol`, `setjmp`/`longjmp` in assembly,
`errno`, `ctype`, the string functions wkernel never needed, and `time`.

It is a layer rather than a second system. `malloc`, `strlen` and the
format-string rules stay wkernel's and are declared rather than rewritten;
there is one allocator and one `printf` on this machine. The two archives are
linked together with `libc.a` first, and an application that uses neither takes
nothing from either.

Two things came out of doing it. The first is that WOS programs now compile
with `-nostdinc`: the host's `/usr/include` cannot reach a WOS binary at all, so
"it compiled" means it compiled against what is on the machine. The second is a
limit worth stating plainly — there are no floating-point conversions, no `%f`
and no `strtod`, because user code is built `-mno-sse` and the calling
convention passes a `double` to a variadic function in an SSE register. That is
a decision about the machine, not a gap in this library.

**Done:** `ctest`, a program that includes no WOS header at all, runs 70 checks
on the machine against both filesystems and passes them.

### 5. A C compiler — **done**, and not the one that was planned

The plan said TCC. What is here instead is **`wcc`**, about 5,000 lines in
`app/wcc/sourcecode`, written for this machine: a preprocessor, a parser, an
x86-64 code generator that writes machine code rather than assembly text, an
ELF64 object writer, and a linker that reads both its own objects and the ones
the host's gcc left in `libwkernel.a`.

The reason for the change is the line at the top of the README: everything here
is written from scratch. Vendoring sixty thousand lines of somebody else's
compiler would have been the faster route and would have made this the one
directory in the tree nobody could read. It is also the one place where "port
it" and "write it" are closer together than they look — TCC would have needed
its ELF writer taught this loader's rules, its libc gaps filled and its build
arranged, and what it would have given back is a compiler far better than this
one at everything except being understandable.

The milestones the document listed, in the order they happened:

1. **it compiles and runs `hello.c`** — on the host first, then on the machine;
2. **it compiles a real command out of this tree** — `pwd`, then `ls`, then
   most of the rest: 51 of the 54 applications compile;
3. **it compiles something with several files in it** — `whell`, four files
   and a header;
4. **it compiles itself** — `cd /app/wcc/sourcecode && make`, on WOS, in about
   two minutes, and the compiler that comes out compiles working programs.

What it will not compile, each said by name when it is met rather than as a
parse error further on: inline assembly (`fastfetch` reads CPUID),
variable-length arrays (`vim` sizes a buffer by the console width), and
compound literals (`swaysettings`). Those three are still built on the host.
Floating point is absent for a reason that is not the compiler's: WOS user code
is built `-mno-sse`, so there is no `double` to generate code for.

Two bugs from writing it are worth keeping, because both assembled quietly and
neither showed up until the machine ran the result. A byte store through `%dil`
needs a REX prefix, without which register field 7 means `%bh` -- `char s[] =
"..."` came out empty. And an assignment has to evaluate the value before the
destination, or `x = setjmp(env)` reads a stack slot that was overwritten
between the two returns. [`docs/wcc.md`](wcc.md) has the rest.

**Done:** `make check ARGS=selfhost` compiles and links a program on the
machine, runs the C library's 70 checks against a binary wcc produced, and
rebuilds one of the machine's own commands with `make`.

### 6. A Makefile in every `sourcecode` directory — **done**

Every `/app/<name>/sourcecode` holds the Makefile that rebuilds that
application, written by `tools/appmakefile.sh` at every build rather than by
hand — fifty-four hand-written ones are fifty-four chances for one to drift, and
a source file added to an application appears in its Makefile without anybody
remembering to put it there. Generating them is also what makes the `make` on
this machine enough: it has no pattern rules, and a rule per object is exactly
the sort of repetition a generator should be writing.

They name `wcc` as the compiler, and `make CC=something` tries another one.
`make -n` shows what would be run without running it:

```
wos:/app/whell/sourcecode# make -n
wcc -c -I/include -o cmd_nav.o cmd_nav.c
wcc -c -I/include -o complete.o complete.c
wcc -c -I/include -o parse.o parse.c
wcc -c -I/include -o whell.o whell.c
wcc -T /lib/user.ld -o /app/whell/launch cmd_nav.o complete.o parse.o whell.o /lib/libc.a /lib/libwkernel.a
```

and without `-n` it builds the shell.

**Done:** `make check ARGS=toolchain` reads those Makefiles on the machine and
checks that they compile every source and link every object.

## Why this order

The compiler was the largest piece and the last one, deliberately. Every step
before it was independently useful, and each proved it: times made `ls -l`
honest and `touch` truthful before anything was compiled, `make` was worth
having with `cat` and `touch` for recipes, and the C library is what any port to
this machine would need — not only this one. When the compiler was finally
written, every one of those was already there and already checked, which is why
the first program it produced could be run the same afternoon.

The scenario that proves the whole chain is the sentence this document started
with, and it now runs as a check:

```
root@wos:/app/pwd/sourcecode# vim pwd.c
root@wos:/app/pwd/sourcecode# make
wcc -c -I/include -o pwd.o pwd.c
wcc -T /lib/user.ld -o /app/pwd/launch pwd.o /lib/libc.a /lib/libwkernel.a
root@wos:/app/pwd/sourcecode# pwd
/app/pwd/sourcecode
built on WOS by wcc
```

## What is next

The machine builds its own programs; it does not yet build its own kernel. That
is the next thing of this size, and it is a different fight: the kernel is
freestanding, uses inline assembly in a dozen places, and is linked at
`0x100000` by a script `wcc` does not read. Between here and there are the three
constructs in the table above, an `ar` so archives can be made on the machine
rather than only read, and something that can write a boot image.
