# The hosted C library

`lib/wlibc` is the half of the C standard library that a program written for
Unix expects to find and `wkernel` deliberately does not have: `FILE *` and
`fopen`, `qsort` and `strtol`, `setjmp`, `errno`. It builds to `libc.a`, which
is the name a compiler looks for, and it is installed on the machine at
`/lib/libc.a` with its headers in `/include`.

It exists for the compiler. [`docs/self-hosting.md`](self-hosting.md) put it in
as the step before one, because a compiler is an ordinary C program that reads
its input with `fopen` and reports its errors with a `longjmp` — and none of
that had anywhere to land here. [`wcc`](wcc.md) is written against this library
and nothing else, and compiles itself with it.

## It is a layer, not a second system

There is one allocator on this machine, one `strlen` and one `printf`. They are
wkernel's, and this library declares them rather than writing them again:

| What | Where it comes from |
|---|---|
| `malloc`, `calloc`, `realloc`, `free` | `libwkernel.a` |
| `strlen`, `strcmp`, `strcpy`, `strchr`, `memcpy`, … | `libwkernel.a` |
| the format-string rules behind `printf` | `wvsnprintf`, in `libwkernel.a` |
| everything else below | `libc.a` |

So the two archives are always linked together, `libc.a` first:

```sh
ld -T lib/wkernel/user.ld -o launch prog.o build/lib/libc.a build/lib/libwkernel.a
```

A static archive is searched once, for whatever is undefined when the linker
reaches it, so the one that needs things goes before the one that has them. An
application that uses neither takes nothing from either — `hello` is still 20
KiB.

## What is in it

| Header | Contents |
|---|---|
| `<stdio.h>` | `FILE`, `fopen`/`fdopen`/`freopen`/`fclose`, `fread`/`fwrite`, `fgetc`/`fgets`/`ungetc`, `fputc`/`fputs`/`puts`, `fseek`/`ftell`/`rewind`, `feof`/`ferror`/`clearerr`, `fflush`, `setvbuf`, `remove`/`rename`, the whole `printf` family |
| `<stdlib.h>` | `exit`/`abort`/`atexit`, `qsort`/`bsearch`, `strtol`/`strtoul`/`strtoll`, `atoi`/`atol`, `abs`/`labs`, `rand`/`srand`, `getenv`, and the heap |
| `<string.h>` | the wkernel ones by declaration, plus `strncpy`, `strncat`, `strdup`, `strndup`, `strspn`, `strcspn`, `strpbrk`, `strtok`, `strtok_r`, `memchr`, `strerror` |
| `<ctype.h>` | the character classes, ASCII only |
| `<errno.h>` | `errno` and the `E*` names |
| `<setjmp.h>` | `setjmp`, `longjmp` |
| `<time.h>` | `time`, `clock`, `localtime`, `gmtime`, `mktime` |
| `<assert.h>` | `assert` |
| `<limits.h>` | the integer limits |

`<stddef.h>`, `<stdarg.h>`, `<stdint.h>` and `<stdbool.h>` are the compiler's
rather than a library's, and are not here. `<limits.h>` is, because gcc's
version hands the actual numbers over to the host's C library, which is the one
thing a WOS program must not touch.

## Decisions worth knowing about

**Buffering is the reason this exists.** WFS writes a whole 1 KiB block per
write call, so a program emitting an object file a byte at a time through
`wwrite()` would write a thousand blocks per kilobyte. A `FILE` takes a
1 KiB buffer the first time it needs one. `stdout` is line buffered, `stderr` is
not buffered at all, and a read or write larger than the buffer bypasses it
rather than being copied through it in pieces.

**A stream is either reading or writing, never both at once.** Standard C
already requires a seek between the two on an update stream, so nothing is
given up — and a buffer that is a read-ahead and a write-behind at the same
time is where every subtle stdio bug lives.

**Positions are the kernel's.** `ftell()` asks the descriptor where it is and
corrects for what this layer is holding: bytes read ahead of the program, or
bytes written by it and not yet handed over. Two records of the same number are
two records that can disagree.

**`errno` is set at the boundary.** Every wkernel call returns its own negated
error code, which is the better arrangement; `_wc_errno()` turns that into the
global a ported program reads, in one place, so no wrapper has to remember to
negate. A call that succeeds leaves `errno` alone, as the standard requires.

**There are no floating-point conversions.** No `%f`, no `strtod`, no `math.h`.
This is not an omission that can be filled in later by writing more of this
library: WOS user code is compiled `-mno-sse`, and the x86-64 calling
convention passes a `double` to a variadic function in an SSE register. The
machine would have to start saving that state per process first.

**There is no `<unistd.h>` and no `<fcntl.h>`.** `open`, `read`, `write`,
`close` and `lseek` are already in `<wkernel.h>` as one-line aliases for the
`w`-prefixed calls. A program that wants descriptors uses those; this library
is the part above them.

## Checking it

`ctest` is a program that includes no WOS header at all — `<stdio.h>` and its
neighbours are the whole of what it knows about the machine. It runs 70 checks
across streams, `stdlib`, strings, `setjmp`, formatted output and the clock:

```
wos:/home/root# ctest
ctest -- the hosted C library, in /ramdisk

streams
  [ok  ] a file can be opened for writing
  ...

70 checks, 0 failed
ctest: all passed
```

It takes a directory to work in, so it can be pointed at the disk rather than
at `/ramdisk`:

```
wos:/home/root# ctest /home/root
```

`make check ARGS=toolchain` runs both. `make check ARGS=selfhost` runs them
again against a binary the machine's own compiler produced, which is the
stronger claim: every one of those 70 checks passes on code generated by
[`wcc`](wcc.md) on WOS.
