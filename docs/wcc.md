# wcc — the C compiler

`wcc` reads C, writes ELF64 objects, and links them into the executables this
machine runs. It is about 6,800 lines in `app/wcc/sourcecode`, it runs on WOS,
and it is what makes the sentence [`docs/self-hosting.md`](self-hosting.md)
opens with true:

```
root@wos:/app/pwd/sourcecode# vim pwd.c
root@wos:/app/pwd/sourcecode# make
wcc -c -I/include -o pwd.o pwd.c
wcc -T /lib/user.ld -o /app/pwd/launch pwd.o /lib/libc.a /lib/libwkernel.a
root@wos:/app/pwd/sourcecode# pwd
/app/pwd/sourcecode
built on WOS by wcc
```

## Using it

```
wcc [-c] [-o out] [-I dir] [-D name[=value]] [-E] input...
```

| Option | Effect |
|---|---|
| `-c` | compile each input to an object; do not link |
| `-o out` | where to put the result |
| `-I dir` | look here for `#include <...>` |
| `-D n[=v]` | define a macro before reading anything |
| `-E` | preprocess only, and print the result |
| `-T file` | accepted and ignored: the layout is fixed |

Inputs ending in `.c` are compiled; `.o` and `.a` are for the linker. Options
it does not know — `-g`, `-O2`, `-Wall` — are reported and ignored, because
this compiler has one setting for each of those and it is not negotiable.

The generated Makefile in every `/app/<name>/sourcecode` already says the
right thing, so on the machine the usual way to run it is `make`.

## What it is made of

| File | What it does |
|---|---|
| `lex.c` | text into tokens |
| `pp.c` | `#include`, `#define`, `#if`, macro expansion with hide sets |
| `type.c` | what a type is, and how big |
| `parse.c` | tokens into a tree, with every name resolved |
| `gen.c` | the tree into x86-64 machine code |
| `elf.c` | the machine code into an ELF64 relocatable object |
| `link.c` | objects and archives into an executable |

There is no assembler in between and no assembly text: the code generator
writes machine code into a buffer, and the linker resolves the symbols and
applies the relocations itself. That is a decision about what would otherwise
have to be ported — an assembler and a linker are two more programs that would
each have to run here first.

It is written against the hosted C library ([`docs/libc.md`](libc.md)) and
includes no WOS header at all, so it builds for this machine and for the host
that builds this machine. The second is what makes it testable: a compiler that
can only be run by booting an operating system is a compiler nobody runs often
enough.

## The code it generates

Deliberately simple, and slower and larger than gcc's by a wide margin:

- every expression leaves its value in `rax`;
- a binary operator evaluates its right side, pushes it, evaluates its left
  side, and pops;
- there is no register allocation and nothing is kept in a register between
  statements;
- addresses are 32-bit absolute, because a WOS program lives between
  `0x40000000` and `0xBFFE0000` — so no position-independent code and no GOT.

Two places where the simple thing would have been wrong, and are not:

**Assignment evaluates the value before the destination.** `x = setjmp(env)`
has to work when `longjmp` comes back to it, and execution then resumes just
after the call with anything pushed before it long since overwritten. Pushing
the destination address first — the obvious order — makes that case fail in a
way that looks like a miscompiled `setjmp`.

**A byte store through `%dil` needs a REX prefix.** Without one, register field
7 means `%bh`, and the instruction assembles quietly into a store to the wrong
half of the wrong register. It cost an afternoon: `char s[] = "..."` came out
empty.

## The C it accepts

Everything the applications in this tree are written in: the full expression
grammar, `struct`, `union`, `enum`, `typedef`, pointers and arrays and function
pointers, all the statements including `switch` and `goto`, `static` at both
scopes, variadic functions, designated initialisers, and nested brace
initialisers.

51 of the 54 applications compile. What is missing, and what the message says
when it is met:

| Missing | Where it matters |
|---|---|
| inline assembly (`__asm__`) | `fastfetch` reads CPUID |
| variable-length arrays | `vim` sizes a buffer by the console width |
| compound literals (`(struct x){...}`) | `swaysettings` |
| floating point | nothing: WOS user code is built `-mno-sse`, so there is no `double` to compile |
| bitfields, K&R declarations | nothing in this tree |

`const` and `volatile` are parsed and ignored: neither changes the code this
compiler generates, because it generates the naive thing every time.

## The linker

It reads objects this compiler wrote and objects the host's gcc wrote — both
have to work, because `libwkernel.a` is built on the host and everything links
against it. Five relocation types appear in practice and all five are handled
(`R_X86_64_64`, `PC32`, `PLT32`, `32`, `32S`); anything else is refused by
name, because a relocation silently left unapplied is a program that runs until
it reaches that address.

The layout is `lib/wkernel/user.ld` expressed in code: text at `0x40000000`,
then read-only data, data and bss, each starting on a page. The result has two
`PT_LOAD` segments — one read-and-execute, one read-and-write — because the
kernel counts code and data separately.

Archives are read the plain way: the members are scanned for symbols still
missing and pulled in until a pass adds nothing, so an archive whose members
depend on each other works whatever order they are stored in.

## Checking it

The C library's own test program is the best evidence there is:
`app/ctest/sourcecode/ctest.c` makes 70 checks across streams, `stdlib`,
strings, `setjmp`, formatted output and the clock — and passes all 70 when
compiled by wcc and linked by wcc, on the machine.

`make check ARGS=selfhost` compiles a program on WOS, links it against the
libraries on the disk, runs it, and then rebuilds one of the machine's own
commands from its source with `make`.

## It compiles itself

On the machine:

```
root@wos:/app/wcc/sourcecode# make
wcc -c -I/include -o elf.o elf.c
wcc -c -I/include -o gen.o gen.c
wcc -c -I/include -o lex.o lex.c
wcc -c -I/include -o link.o link.c
wcc -c -I/include -o main.o main.c
wcc -c -I/include -o parse.o parse.c
wcc -c -I/include -o pp.o pp.c
wcc -c -I/include -o type.o type.c
wcc -c -I/include -o util.o util.c
wcc -T /lib/user.ld -o /app/wcc/launch elf.o gen.o lex.o link.o main.o parse.o pp.o type.o util.o /lib/libc.a /lib/libwkernel.a
```

That is WOS rebuilding the tool that builds WOS, using its own `make`, its own
C library and its own linker — and the compiler that comes out of it compiles
and links working programs. It takes about two minutes on an emulated machine.

## What it is not

It is not fast and it does not optimise. The code it generates is several times
larger than gcc's, because every value goes through `rax` and the stack. That
is the trade it was written to make: a compiler small enough to read in an
afternoon, that this machine can run.
