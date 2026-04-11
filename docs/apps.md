# Applications

Every application lives in `/app/<name>/`, with its executable at
`/app/<name>/launch` and its source in `/app/<name>/sourcecode/`. Typing the
bare name in `whell` runs it.

## A note on the "ports"

`fastfetch`, `htop`, `vim` and `fish` here are **WOS-native programs written in
the spirit of the originals, not builds of the upstream source.** That is not a
shortcut taken to save effort — the upstream programs cannot run here:

| Program | What it needs that WOS does not have |
|---|---|
| fastfetch | `/proc`, `/sys`, DRM and PCI ID databases, a full libc |
| htop | ncurses, `/proc`, signals, `ioctl` |
| vim | ~400k lines of C over a full libc, `termios`, `fork`, signals, regex |
| fish | a C++ runtime, `fork`/`exec` job control, `termios`, PCRE |

WOS has a 30-call kernel API, no `fork`, no signals, no `termios`, no dynamic
linking, and a 268 KiB limit on any single file. Building the real thing would
mean writing a POSIX layer several times the size of the whole system.

What is here instead: each program does what the original is *for*, using the
same key bindings and the same output shapes where they apply, so the muscle
memory transfers. Where a feature depends on something WOS lacks, it is absent
and said so rather than faked.

---

# fastfetch

```
fastfetch
```

Prints an ASCII WOS logo beside a summary of the machine.

```
                                wos@wos
                                ------
  __        __   ___    ____    OS: WOS 0.1 (i386)
  \ \      / /  / _ \  / ___|   Kernel: wos-0.1
   \ \ /\ / /  | | | | \___ \   Uptime: 8 seconds
    \ V  V /   | |_| |  ___) |  Shell: whell
     \_/\_/     \___/  |____/   Terminal: VGA 80x25
                                CPU: QEMU Virtual CPU version 2.5+
                                Memory: 5.3M / 255.8M
                                Disk (/): 406.0K / 64.0M (wfs)
                                Processes: 2
```

| Field | Where it comes from |
|---|---|
| Uptime | `wuptime_ms()` |
| CPU | the `CPUID` instruction, executed directly — it is unprivileged, so a ring 3 program can identify the processor without asking the kernel |
| Memory | `wmeminfo()` |
| Disk | `wdiskinfo()` |
| Processes | `wproclist()` |

The logo is ASCII rather than the block characters a real fetch tool uses. The
VGA font is code page 437 and the serial port usually reaches a UTF-8 terminal,
and no single byte sequence looks right on both.

The colour bar shows the normal eight colours as backgrounds and the bright
eight as foreground blocks — VGA text mode spends the top attribute bit on
blink rather than on a bright background.

**Exit status:** 0.

---

# htop

```
htop
```

A full-screen process and resource monitor. It refreshes once a second and
redraws immediately when a key is pressed.

```
 htop   a process monitor for WOS                      Uptime 00:00:08

  Mem [|                              5.3M/255.8M]
  Dsk [|                             476.0K/64.0M]

  Tasks: 2      Threads: 2      Kernel: 5.1M

   PID COMMAND       RESIDENT     CODE     DATA     HEAP    STACK  THR
     6 whell           100.0K    20.0K     8.0K       0B    64.0K    1
     7 htop             88.0K     8.0K     8.0K       0B    64.0K    1

 up/dn  Select  q  Quit   r  Refresh now
```

| Key | Effect |
|---|---|
| Up / Down, or `k` / `j` | move the selection |
| Home / End | first / last process |
| `q`, or Ctrl+C | quit |
| any other key | refresh immediately |

The meters change colour with load, as htop's do: green below 70%, yellow to
90%, red above. A non-zero amount always shows at least one bar, so a small
but real allocation does not read as nothing.

`RESIDENT` is what the process actually has mapped, counted from its page
tables, which is why it exceeds the four columns beside it — those do not
include the process's own page tables.

The display stays responsive because it never blocks: it polls with
`wpollin()` between repaints rather than waiting inside `wread()`, which would
freeze the clock until someone pressed a key.

**Exit status:** 0.

---

# vim

```
vim [file]
```

A modal editor. Opening a file that does not exist starts a new buffer under
that name, as vim does.

```
Welcome to WOS.

This file lives on a real disk image in the WFS filesystem, not in RAM.
Anything you write here survives a reboot.
~
~
 NORMAL  /home/readme.txt [+]                        4,4   17 lines
```

The status bar shows the mode, the file, `[+]` when there are unsaved changes,
the cursor position and the line count. Lines past the end of the buffer are
marked with a `~` column.

## Normal mode

| Key | Effect |
|---|---|
| `h` `j` `k` `l`, arrows | move by character and line |
| `0`, Home | start of line |
| `^` | first non-blank character |
| `$`, End | end of line |
| `w` / `b` | forward / back one word |
| `gg` / `G` | first / last line |
| Ctrl+D / Ctrl+U | down / up half a screen |
| Page Down / Page Up | down / up a full screen |
| `i` `a` | insert before / after the cursor |
| `I` `A` | insert at the start / end of the line |
| `o` `O` | open a line below / above and insert |
| `x`, Delete | delete the character under the cursor |
| `dd` | delete the line |
| `:` | enter a command |

## Insert mode

Typing inserts. Enter splits the line, Backspace deletes back and joins onto
the previous line at column 0, and Escape returns to normal mode — stepping
one character left, as vim does.

Tab inserts four spaces. A literal tab would need the console to expand it
identically on the VGA screen and over serial, and it does not.

## Commands

| Command | Effect |
|---|---|
| `:w` | write the file |
| `:w name` | write to `name` and adopt it as the file name |
| `:q` | quit, refusing if there are unsaved changes |
| `:q!` | quit, discarding changes |
| `:wq`, `:x` | write and quit |
| `:w!` | write regardless |

Errors use vim's own numbering where there is an equivalent, so `:q` with
unsaved changes gives `E37: No write since last change (add ! to override)`.

## What is missing

No counts (`3dd`), no registers or yank/put, no undo, no visual mode, no
search or `:%s///`, and no syntax highlighting. Undo and search are the two
worth adding next; the rest is a long way down from what the editor is for.

**Exit status:** 0.

---

# fish

```
fish
```

A friendly interactive shell. What it adds over `whell` is all in the typing:

```
Welcome to fish, the friendly interactive shell
Type help for instructions on how to use fish

wos /home> ls -l          <- "ls" green: it exists
wos /home> xyzzy abc      <- "xyzzy" red: it does not
wos /home> ls -l          <- " -l" grey: suggested from history
```

| While typing | Effect |
|---|---|
| the first word turns **green** | that command can actually be run |
| the first word turns **red** | it cannot — the typo is visible before Enter |
| **grey** text ahead of the cursor | the rest of the most recent matching command |
| Right arrow, End, Ctrl+E, Ctrl+F | accept the suggestion |
| Up / Down | walk the history |
| Tab | complete commands and paths |
| Left / Right, Home / End, Ctrl+A | move within the line |
| Ctrl+U / Ctrl+K | clear the line / to end of line |
| Ctrl+C | abandon the line |
| Ctrl+D on an empty line | exit |

Completion inserts at the cursor rather than appending, so completing in the
middle of a line does not scramble the rest of it.

## Builtins, and how it borrows the rest

fish implements `cd`, `pwd`, `history`, `clear`, `help` and `exit` itself —
`cd` has to be a builtin because a child process cannot change its parent's
working directory.

Anything else is looked for at `/app/<name>/launch`, and failing that is
handed to `whell -c "<line>"`. That is where `ls`, `free`, `df`, `ps`, `cat`,
`rm`, `mkdir`, `touch` and `shutdown` live, so the two shells share one set of
builtins instead of each keeping its own copy.

## What is missing

No functions, no abbreviations, no `$variables`, no pipes or redirection, no
job control, and no universal variables. Pipes are the significant one — they
need the kernel to support more than one console-attached descriptor per
process.

**Exit status:** the status of the last command, or whatever `exit` was given.
