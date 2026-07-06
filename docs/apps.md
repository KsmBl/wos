# Applications

Every application lives in `/app/<name>/`, with its executable at
`/app/<name>/launch` and its source in `/app/<name>/sourcecode/`. Typing the
bare name in `whell` runs it.

This is where the commands live. The shells contain almost nothing: only `cd`,
`exit` and `help` are builtins, because only those change state belonging to
the shell process. `ls`, `cat`, `free` and the rest are ordinary programs, so
`whell` and `fish` run exactly the same ones.

Each is statically linked against `libwkernel.a`, so a program is around 50 KiB
whatever it does. The couple of dozen here come to a few MiB on a 64 MiB disk,
which is the price of having no shared libraries.

| Command | What it does |
|---|---|
| [`ls`](#ls) | list directory contents |
| [`pwd`](#pwd) | print the working directory |
| [`cat`](#cat) | print files |
| [`free`](#free) | show memory use |
| [`df`](#df) | show disk use |
| [`ps`](#ps) | show processes and their memory |
| [`touch`](#touch) | create files |
| [`mkdir`](#mkdir) | create directories |
| [`rm`](#rm) | remove files and directories |
| [`clear`](#clear) | clear the screen |
| [`time`](#time) | show or set the clock |
| [`ping`](#ping) | send ICMP echo requests to a host |
| [`curl`](#curl) | fetch a URL and print it |
| [`wget`](#wget) | download a URL to a file |
| [`lynx`](#lynx) | browse the web as text |
| [`shutdown`](#shutdown) | power the machine off |
| [`uptime`](#uptime) | how long the machine has been running |
| [`cpufreq`](#cpufreq) | show the processor's clock, and change it |
| [`systemctl`](#systemctl) | list, start, stop, enable and disable services |
| [`battery`](#battery) | say whether the machine has a battery, and what it is |
| [`whoami`](#whoami) | print the current user and what it may do |
| [`passwd`](#passwd) | change a password |
| [`su`](#su) | start a shell as another user |
| [`chsh`](#chsh) | change a user's login shell |
| [`adduser`](#adduser) | create a user, asking for a password |
| [`edituser`](#edituser) | add or remove roles |

The graphical session has its own set:

| Command | What it does |
|---|---|
| [`sway`](#sway) | the tiling Wayland compositor |
| [`wlterm`](#wlterm) | a terminal emulator, as a Wayland client |
| [`swaymsg`](#swaymsg) | ask sway something, or tell it to do something |
| [`waylandd`](#waylandd) | a bare display server, for testing clients against |
| [`wlprobe`](#wlprobe) | list what a display server advertises |

Users, roles and what each may write are covered in
[`docs/users.md`](users.md).

---

# Core commands

## ls

```
ls [-l] [-a] [path...]
```

With no operand, lists the working directory. With several, each directory gets
a `path:` header and a blank line between them; a file operand is listed as
itself rather than being descended into. Entries are sorted by name, and plain
output is laid out in columns with a `/` after directory names.

| Option | Effect |
|---|---|
| `-l` | long listing: type, size in bytes, blocks used, name |
| `-a` | include entries starting with `.`, including `.` and `..` |

```
wos:/home$ ls -l
total 3
-   2     1  boots.txt
-  66     1  notes.txt
- 560     1  readme.txt
```

The first column is `d` for a directory and `-` for a file. `total` is the sum
of the block counts, as in Linux.

WFS stores no owners, permissions or timestamps, so those Linux columns are
absent rather than filled with invented values.

**Exit status:** 0, or 1 if any operand could not be read.

## pwd

```
pwd
```

Prints the working directory as an absolute, normalised path. A child inherits
its parent's directory, which is why this works as a program while `cd` cannot.

**Exit status:** 0, or 1 if the path could not be read.

## cat

```
cat file...
```

Writes each file to standard output in order. Continues past a file it cannot
open, reporting each failure, so one bad operand does not hide the rest.

**Exit status:** 0, or 1 if any file could not be read.

## free

```
free [-b | -k | -m | -h]
```

```
wos:/home$ free
          total        used        free
Mem:     262016        9532      252484
Swap:         0           0           0
```

| Option | Units |
|---|---|
| *(none)* | kibibytes, as Linux does by default |
| `-b` `-k` `-m` | bytes, kibibytes, mebibytes |
| `-h` | human readable, e.g. `246.5M` |

The figures come from the kernel's physical frame allocator, so `used` is
exactly the RAM currently allocated — including the kernel's own image, heap
and page tables — and `used + free == total` always holds.

WOS has no swap. The row is printed as zeroes so the output matches what a
reader of `free` expects to find.

**Exit status:** 0, or 1 on an invalid option.

## df

```
df [-b | -k | -m | -h]
```

```
wos:/home$ df
Filesystem    1K-blocks       Used  Available Use% Mounted on
wfs               65536       1264      64272   1% /

inodes: 83 used, 1965 free, 2048 total
```

Options are the same as `free`. The numbers come from the filesystem's block
bitmap, so `Used` counts blocks that are genuinely allocated, including the
superblock, bitmap and inode table. A disk with anything on it reports at least
1%, never 0%.

**Exit status:** 0, or 1 if no filesystem is mounted or the option is invalid.

## ps

```
ps
```

```
wos:/home$ ps
  PID NAME          RESIDENT     CODE     DATA     HEAP    STACK THR
    6 whell           108.0K    12.0K     8.0K       0B    64.0K   1
   11 ps              104.0K     8.0K     8.0K       0B    64.0K   1
```

`ps` lists itself, which it could not do as a shell builtin — a useful reminder
that these really are separate processes.

`RESIDENT` is what the process actually has mapped, counted from its page
tables, which is why it exceeds the four columns beside it: those do not
include the process's own page tables.

**Exit status:** 0.

## touch

```
touch file...
```

Creates each file if it does not exist, and leaves the contents of one that
does alone.

WFS stores no timestamps, so unlike Linux there is nothing to update on a file
that already exists — `touch` on it simply succeeds.

**Exit status:** 0, or 1 if any file could not be created.

## mkdir

```
mkdir dir...
```

Creates each directory. The parent must already exist, so making a nested path
takes one `mkdir` per level.

**Exit status:** 0, or 1 if any directory could not be created.

## rm

```
rm [-r] [-f] file...
```

| Option | Effect |
|---|---|
| `-r`, `-R` | remove directories and everything inside them |
| `-f` | ignore files that do not exist, and say nothing about them |

Without `-r`, naming a directory is an error:

```
wos:/home$ rm tree
rm: tree: is a directory
wos:/home$ rm -r tree
```

`rm` refuses to remove `.` or `..`, as Linux does — `rm -r .` would delete the
working directory out from under the shell.

Recursive removal collects a directory's children before deleting any of them:
`wreaddir` walks by entry index, so removing an entry mid-iteration shifts the
ones after it and the walk would silently skip files.

**Exit status:** 0, or 1 if anything could not be removed. With `-f`, a missing
file is not a failure.

## clear

```
clear
```

Clears the screen and homes the cursor, by emitting the ANSI escapes described
in [`docs/console.md`](console.md).

**Exit status:** 0.

## shutdown

```
shutdown
```

Powers the machine off. Nothing needs flushing first: WFS writes its
superblock, block bitmap and inodes straight through on every change, so the
disk is consistent at every moment.

The machine is asked to power off the way ACPI says to: the kernel reads the
tables at boot to find the chipset's power management register and the sleep
type that means "off", and writes one to the other. The boot log says what it
found — `acpi   : soft-off through PM1a at 0x604, sleep type 0`.

A few fixed addresses the emulators are known to answer on are tried afterwards,
which is what makes this work under QEMU, VirtualBox and Bochs even when their
tables say nothing useful. If none of it takes, the kernel says so and halts the
CPU, which is as close to off as it can get on its own.

There is no user or permission model in WOS, so any process can do this.

**Exit status:** does not return on success; 1 if the machine could not be
powered off.

## uptime

```
uptime [-p]
```

How long the machine has been running, the clock, and how many users it knows
about.

```
$ uptime
 17:41:53  up 1:23,  1 user
$ uptime -p
up 1:23
```

There is no load average. WOS has no notion of one, and a number that looked
like Linux's without meaning what Linux's means would be worse than leaving it
out.

htop draws the same line in its header, from the same function
(`wuptime_string()`), so the two cannot come to disagree about it.

**Exit status:** 0.

## whoami

```
whoami [-v]
```

Prints the current user's name. `-v` also lists the roles held and the
directories that user may write.

**Exit status:** 0.

## passwd

```
passwd [user]
```

Changes your own password, or another user's if you are root or hold the
`useradmin` role. With no argument it changes your own.

An unprivileged user is asked for the current password first; a privileged one
is not, because the kernel would ignore it anyway. Nothing is echoed while
typing, not even a placeholder — the length of a password is itself worth not
showing.

An empty new password clears it, letting that account be entered without one.
`passwd` says so explicitly when that happens rather than letting it pass
quietly.

The program never sees a hash: the kernel does the checking and the storing,
which is what makes it safe for an ordinary user to run without setuid.

**Exit status:** 0, or 1 — with different messages for "not permitted" and
"the current password is wrong", since those are different problems.

## su

```
su [user]
```

Starts a shell as another user, defaulting to `root`. Asks for that user's
password unless you are already root.

It starts a **new** shell rather than changing the current one, because a
process can drop to another user but never climb back. `exit` returns you to
the shell you came from, still as whoever you were.

**Exit status:** the shell's, or 1 if authentication failed.

## adduser

```
adduser [-a] [-u] <name>
```

| Option | Grants |
|---|---|
| `-a` | `appeditor` — may write under `/app` |
| `-u` | `usereditor` — may write `/userconfig`: add users, set passwords, change roles |

Asks for a password, then creates the user, their home directory under `/home`
and their password file at `/userconfig/<name>/password`.

Only root and holders of `usereditor` may run it; anyone else is refused by the
kernel, not by the program.

Names become part of a path, so `/`, `:`, `.` and newlines are refused. An
empty password is allowed, and `adduser` says so rather than letting it pass
quietly.

**Exit status:** 0, or 1.

## edituser

```
edituser <name>                    show the roles held
edituser <name> +appeditor         grant a role
edituser <name> -usereditor        take one away
```

Several changes can be given at once and are applied in order, so
`edituser bob -appeditor +usereditor` does both.

Root and holders of `usereditor` may run it. Root's own roles cannot be
changed: every permission check short-circuits on uid 0, so they carry no
meaning.

**Exit status:** 0, or 1.

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
     \_/\_/     \___/  |____/   Terminal: VGA 80x50
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
 htop   Intel(R) Core(TM) i7-8665U CPU @ 1.90GHz          up 12 secs

 CPU0    [|||||||                       38.4%  1.90GHz  47C]
 Mem     [|                                    9.3M/255.4M]
 /       [|                                       7.7M/2.0G]
 /ramdisk[                                        0B/246.1M]

  Tasks: 2      Threads: 2      Kernel: 9.1M

   PID COMMAND       RESIDENT     CODE     DATA     HEAP    STACK  THR
     6 whell           100.0K    20.0K     8.0K       0B    64.0K    1
     7 htop             88.0K     8.0K     8.0K       0B    64.0K    1

 up/dn  Select  q  Quit   r  Refresh now
```

Every logical processor the machine has gets a bar: how busy it was over the
last second, what it is clocked at, and how hot it is. WOS runs on the
processor it booted on and never starts the others, so those are drawn grey and
say `not started` — they are part of the machine, and a monitor that listed
only the core it happens to be running on would report a four-core laptop as a
single-core one.

The clock and the temperature are left out rather than shown as zero when the
machine will not report them. Inside a hypervisor that is the usual answer: the
counters the real clock is measured from and the on-die thermal sensor are both
model-specific registers, and KVM faults on the ones it was not told to
emulate. What is shown then is the base clock the CPU quotes.

Each mounted filesystem gets its own bar, labelled by where it is mounted: the
disk at `/`, and the scratch space in memory at `/ramdisk`. They fill up
independently, and one figure covering both would describe neither.

The meters lay themselves out in as many columns as the console is wide enough
for, and the process table takes whatever rows are left.

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
freeze the clock until someone pressed a key. Between polls it sleeps rather
than spinning, so the machine really is idle while htop is on screen — a
monitor that pinned the processor at 100% just by being open would be
measuring itself.

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
| `:term [cmd]` | open a terminal window running `cmd` (default: a shell) |

Errors use vim's own numbering where there is an equivalent, so `:q` with
unsaved changes gives `E37: No write since last change (add ! to override)`.

## `:term` — a terminal in a window

`:term` splits the screen: the editor keeps the left half and a terminal
window opens on the right, running a program with its input and output wired to
that window instead of the console. `:term` on its own runs a shell; `:term
asciiquarium` runs the aquarium; `:term ls -l /app` runs one command.

```
Welcome to WOS.                       |whell -- the WOS shell.
This file lives on a real disk image i|root@wos:/home/root# free
Anything you write here survives a reb|              total    used    free
                                      |Mem:         262016    9964  252052
 ...                                  |root@wos:/home/root# _
 /home/root/readme.txt                |terminal
:                                     (bottom: shared command line)
```

| Key | Effect |
|---|---|
| `Ctrl-W Ctrl-W` | move the keyboard between the two windows (also `Ctrl-W w`) |
| `:q` (with a terminal open) | close the terminal window; a second `:q` quits |

The highlighted status line shows which window has focus. When the editor has
focus, keys edit as normal; when the terminal has focus, keys go to the program
running in it. The terminal closes on its own when its program exits — `exit`
in the shell, `q` in the aquarium — or with `:q`.

This is real preemptive multitasking, not a trick: the program in the window is
a separate process that keeps running while you edit. Run `:term asciiquarium`
and the fish keep swimming whichever window you are typing in. That is the
whole reason it exists.

**How it works.** The kernel gained anonymous pipes and a spawn that wires a
child's stdin and stdout to them (see [the kernel API](wkernel-api.md)). vim
spawns the program that way, reads its output, and interprets it — text, the
control characters, and the ANSI escape sequences a full-screen program uses —
into a grid it paints into the window's corner of the screen. In other words,
vim contains a small terminal emulator. The program is told the window's size
through `wconsize()`, so it lays itself out to fit rather than assuming a size.

## What is missing

No counts (`3dd`), no registers or yank/put, no undo, no visual mode, no
search or `:%s///`, and no syntax highlighting. Undo and search are the two
worth adding next; the rest is a long way down from what the editor is for.

The terminal is one window only, and always the right half — no stacking,
resizing or a second terminal. A window is 40 columns wide, so a program that
insists on 80 wraps inside it.

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

Two names are short for longer ones:

| | |
|---|---|
| `ll` | `ls -l` |
| `la` | `ls -a` |

Anything after them is kept, so `ll /app` is `ls -l /app`. They are not an
alias mechanism — there is no way to define your own, which would mean a
configuration file, a parser for it and somewhere to keep it, for two names.
The editor knows them, so they colour green as you type and complete on Tab.

Anything else is looked for at `/app/<name>/launch`, and failing that is
handed to `whell -c "<line>"`. That is where `ls`, `free`, `df`, `ps`, `cat`,
`rm`, `mkdir`, `touch` and `shutdown` live, so the two shells share one set of
builtins instead of each keeping its own copy.

## What is missing

No functions, no user-defined abbreviations, no `$variables`, no pipes or
redirection, no
job control, and no universal variables. Pipes are the significant one — they
need the kernel to support more than one console-attached descriptor per
process.

**Exit status:** the status of the last command, or whatever `exit` was given.

---

# asciiquarium

```
asciiquarium
```

An animated aquarium: fish drift across the water, bubbles rise, seaweed
sways. A WOS-native take on Kirk Baucom's `asciiquarium` — the original is Perl
over `Term::Animation`, neither of which exists here.

Press `q` (or `Escape`) to leave; it restores the screen and cursor on the way
out.

It exists mainly as a moving thing to watch. Two reasons it earns its place:

- **It proves multitasking.** Run it in one of vim's `:term` windows while you
  edit in the other (`:term asciiquarium`) and both animate at once, driven by
  the preemptive scheduler. That is the whole demonstration.
- **It sizes itself to its window.** `wconsize()` tells it how big its terminal
  is, so it fills the whole 80x50 console or a narrow vim split equally well.

Rendering is double-buffered: each frame is composed into a grid off-screen and
only the cells that changed from the last frame are actually written. That
keeps the byte stream small — important when the frames are travelling through
a pipe into vim's terminal emulator rather than straight to the VGA.

**Exit status:** 0.

---

# chess

```
chess
```

Two-player chess. Two people share the keyboard and enter moves in coordinate
notation; a referee enforces the rules.

```
     a  b  c  d  e  f  g  h
  8  r  n  b  q  k  b  n  r  8
  7  p  p  p  p  p  p  p  p  7
  6                          6
  5                          5
  4              P           4
  3                          3
  2  P  P  P  P     P  P  P  2
  1  R  N  B  Q  K  B  N  R  1
     a  b  c  d  e  f  g  h

Black to move.  Enter a move (e2e4), or 'help'.
```

White is UPPERCASE, black is lowercase, on a coloured checkerboard.

| Input | Meaning |
|---|---|
| `e2e4` | move from e2 to e4 |
| `e1g1` | castle — move the king two squares |
| `e7e8q` | promote (`q` `r` `b` `n`; queen if omitted) |
| `new` | start over |
| `resign` | concede |
| `help` | the move syntax |
| `quit` | leave |

All the real rules are enforced: legal movement for each piece, the ban on
leaving your own king in check, castling (with its rights and the no-passing-
through-check rule), en passant and promotion. The game recognises check,
checkmate and stalemate.

**No computer opponent.** This is two humans and a referee — writing an engine
worth playing is a much larger project than the rules themselves. Move legality
is checked by generating every legal reply and matching yours against it, which
is also how checkmate and stalemate are detected: no legal move and in check is
mate, no legal move and not in check is stalemate.

**Exit status:** 0.

---

# math

```
math <expression>
```

Evaluate an arithmetic expression and print the result, after fish's `math`.

```
root@wos:/home/root# math 2 + 3 * 4
14
root@wos:/home/root# math '(1 + 2) / 4'
0.75
root@wos:/home/root# math 2 ^ 10
1024
root@wos:/home/root# math 'sqrt(2)'
1.414213
```

The expression can be one quoted argument or several bare ones — they are
joined with spaces, so `math 2 + 3` and `math "2 + 3"` are the same. With no
arguments it reads one line from standard input.

| | |
|---|---|
| Operators | `+` `-` `*` `/` `%` `^`, unary `-`, and parentheses |
| Functions | `sqrt(x)`, `abs(x)` |
| Precedence | `^` above `* / %` above `+ -`; `^` is right-associative |

## Fixed point, not floating point

WOS user programs are built with no FPU and no SSE, so there is no hardware
floating point. `math` therefore works in **fixed point**: every value is a
64-bit integer of millionths, which gives six decimal places — the same
default fish uses. Multiply and divide need a 128-bit intermediate, computed by
hand from 64-bit pieces because there is no `libgcc` to supply the compiler's
128-bit divide.

The result is exact for the six decimals shown, `10 / 3` prints `3.333333`, and
a whole-number result prints with no decimal point at all. A fractional
exponent is refused, since fixed point cannot raise to one.

**Exit status:** 0 on success, 1 on a math error (division by zero, `sqrt` of a
negative number, ...), 2 if there is no expression.

---

# split

```
split
```

Run two terminals at once -- a tiny terminal multiplexer, in the spirit of tmux
or screen. Each pane is its own shell: the login shell, so [`chsh`](#chsh)
decides which one appears, the same as a fresh login or `su` gives you. The
status label says which it is.

```
whell -- the WOS shell.      |whell -- the WOS shell.
root@wos:/home/root# free    |root@wos:/home/root# pwd
              total    used  |/home/root
Mem:         262016   10020  |root@wos:/home/root# _
root@wos:/home/root#         |
 left: whell                  right: whell
```

**Which way the screen is cut follows the shape of the display**, so that
neither pane ends up long and thin: side by side when the screen is wider than
it is tall, one above the other when it is not. The panes are then labelled
`top` and `bottom` instead of `left` and `right`.

The comparison is in pixels rather than characters, and the two answers differ
because a character cell is twice as tall as it is wide. The default 80x25 grid
is 640x400 pixels — half again wider than it is tall, so the panes go side by
side. `textmode 80 50` makes the same screen 640x800, taller than it is wide,
and `split` then stacks them:

```
whell -- the WOS shell. Type `help` for an introduction.
root@wos:/home/root# _

--------------------------------------------------------
whell -- the WOS shell. Type `help` for an introduction.
root@wos:/home/root#

 top: whell                    bottom: whell
```

The keyboard talks to one pane at a time; the highlighted status label shows
which.

| Key | Effect |
|---|---|
| `Ctrl-W Ctrl-W` | switch the keyboard to the other pane (also `Ctrl-W w`, `h`, `j`, `k`, `l`) |
| `Ctrl-W q` | quit, closing both shells |

Every direction key moves to the other pane rather than a particular one: there
are only two, and which way they lie depends on the layout.

When one shell exits (`exit`), the split goes away and the surviving terminal
takes over the whole screen — you are left with just that one, full size, and
it keeps running. When it too exits, `split` leaves.

The survivor really is resized, not just re-centred: `split` resizes its
emulator to the whole screen and tells the shell its new size, so a program you
start afterwards (say `ls`, or `asciiquarium`) uses all 80 columns rather than
the old half.

Both panes are genuinely separate processes running at the same time. Start a
long-running program in one -- `asciiquarium`, say -- and type in the other
while it animates: preemptive multitasking you can watch.

Each terminal is a `wterm` from the shared library, the same emulator vim's
`:term` uses. `split` just arranges two of them and routes the keyboard between
them, which is why the two features behave identically. Side by side, a pane is
40 columns wide, so a program that insists on 80 wraps inside it; stacked, each
pane has the full width and about half the rows.

**Exit status:** 0.

---

# cpufreq

```
cpufreq [get | set <MHz> | inc <MHz> | dec <MHz> | auto | tui]
```

Show the processor's clock, and change it.

```
root@wos:/home/root# cpufreq
cpu    : Intel(R) Core(TM) i7-8665U CPU @ 1.90GHz
core 0 : 1.90GHz (measured), 47C of 100C
range  : 400MHz to 4.10GHz, in steps of 100MHz
setting: automatic -- the hardware chooses
root@wos:/home/root# cpufreq set 1200
clock: held at 1.20GHz
root@wos:/home/root# cpufreq inc 400
clock: held at 1.60GHz
root@wos:/home/root# cpufreq auto
clock: back to the hardware's own judgement
```

| Command | Effect |
|---|---|
| `get` (or nothing) | what each core is doing, the range, and what the clock is being held at |
| `set <MHz>` | hold the clock there |
| `inc <MHz>` | ask for that much more than it is doing now |
| `dec <MHz>` | ask for that much less |
| `auto` | hand the decision back to the hardware |
| `tui` | a full-screen slider |

Everything is in megahertz. The hardware moves in steps of its bus reference —
100 MHz on anything recent — so a request lands on the nearest step rather than
exactly where it was pointed, and the reply says where it went. Requests
outside the range the machine admits to are clamped to it.

`tui` is the same thing to watch rather than to type at:

```
 cpufreq   Intel(R) Core(TM) i7-8665U CPU @ 1.90GHz

    400MHz [|||||||||||||||                         ] 4.10GHz

  now    1.90GHz   held at 1.20GHz   temp 47C

 up/dn  One step  home/end  Slowest / fastest  a  Automatic  q  Quit
```

**Changing the clock needs root or the [`editfreq`](users.md#roles) role.**
There is one clock and every process on the machine runs on it: a slow machine
is slow for everybody, and a fast one is hot for everybody. Reading it needs
nothing.

Many machines will not let it be set at all, and say so rather than pretending:

```
root@wos:/home/root# cpufreq set 1200
cpufreq: this machine does not let the clock be set
```

That is the usual answer inside a hypervisor. The request goes to a
model-specific register — `IA32_HWP_REQUEST` on a processor that manages its
own clock, `IA32_PERF_CTL` on an older one — and a hypervisor that does not
emulate them faults on the write, or accepts it and forgets. Both are detected:
the kernel writes back the value that is already there at boot to see whether
writing works at all, and reads a request back afterwards to see whether it
stuck.

**Exit status:** 0, or 1 if the request was refused or impossible.

---

# battery

```
battery [-s]
```

Say whether this machine has a battery, and what it is.

```
root@wos:/home/root# battery
battery  : present
pack     : LGC DELL AB01
chemistry: lithium ion
when new : 45.0 Wh at 11.4 V
fitted   : Rear
charge   : not readable
```

On a machine with no battery, which is every virtual one:

```
root@wos:/home/root# battery
No battery: this machine runs on mains.
```

`-s` prints a single line — `none`, or `present, charge unknown` — for a prompt
or a script.

## Why there is no percentage

Every laptop built this century reports its charge through an ACPI method,
`_BST`, which reads the embedded controller and returns a package. Calling one
means interpreting AML bytecode, and WOS has no interpreter: the one piece of
AML the kernel reads is the sleep type for soft-off, which is a constant
sitting in a fixed shape, not a program.

So the charge is missing rather than guessed at, and everything else is read
without executing anything:

| Fact | Where it comes from |
|---|---|
| there is a battery | a `PNP0C0A` device, or a `_BST` method, named in the DSDT |
| there is a mains adapter | an `ACPI0003` device, or a `_PSR` method |
| maker, name, chemistry, capacity, voltage, where it is fitted | the SMBIOS tables, structure type 22 |

The two sources answer slightly different questions and both are used. SMBIOS
describes the machine as it was built, so a laptop sold with its battery
removed still carries the structure; ACPI describes what the firmware is
willing to talk to. Either saying yes is taken as a battery being present.

Writing the AML interpreter would be the way to finish this, and it is a large
piece of work: a namespace, operation regions, embedded-controller
transactions, and enough of the language to run a method that was written
assuming a complete implementation.

**Exit status:** 0.

---

# systemctl

```
systemctl [list | status [name] | start <name> | stop <name> |
           restart <name> | enable <name> | disable <name>]
```

See what the system is running, and change it.

```
root@wos:/home/root# systemctl
SERVICE      STATE     AT BOOT     PID  DESCRIPTION
wayland      running   enabled       1  the Wayland display server

root@wos:/home/root# systemctl status wayland
wayland -- the Wayland display server
   state: running as pid 1
 at boot: enabled
    runs: /app/waylandd/launch

root@wos:/home/root# systemctl stop wayland
wayland: stopped, enabled at boot
root@wos:/home/root# systemctl restart wayland
wayland: running as pid 4, enabled at boot
root@wos:/home/root# systemctl disable wayland
wayland: running as pid 4, disabled at boot
```

**Enabling is not starting.** "Should this run at the next boot" and "is it
running now" are different questions, and a manager that answered one with the
other would be deciding things nobody asked it to. `enable` edits the unit
file; `start` runs the program.

**Changing anything needs root or the
[`systemctleditor`](users.md#roles) role.** Looking needs nothing.

## Services

A service is described by a file in `/services`, named after the service:

```
root@wos:/home/root# cat /services/wayland
description=the bare Wayland display server, on wayland-1
exec=/app/waylandd/launch
enabled=1
```

Unknown keys and blank lines are skipped rather than refused, so a unit file
written for a later version still starts its service.

The kernel reads `/services` at boot and starts everything enabled, before
anything logs in — which is what makes a service belong to the machine rather
than to whoever started it. A service is spawned with no parent, so closing the
shell you started it from does not take it down.

`stop` asks the process to leave rather than tearing it down where it stands:
there is no signal mechanism here, and no way to unwind another thread's kernel
stack from a distance. The process is marked and woken, and it exits at the
next moment it is holding nothing — returning from a wait, entering a syscall,
or being interrupted in ring 3. For anything that waits, which is every
service, that is the next time it is scheduled.

The kernel waits for it to actually go, up to two seconds, so that `restart`
does not start the replacement while the original still holds the socket. What
`systemctl` prints afterwards is read back from the kernel rather than assumed
from the command succeeding — if a process somehow never reaches a safe moment,
it is still reported running, because it is.

**Exit status:** 0, or 1 if the action was refused or impossible.

---

# The desktop

WOS has a graphical session: a tiling Wayland compositor, a terminal emulator
that runs in it, and the tools to look at both. Nothing here is a mock-up of
the protocol — the bytes on the socket are the bytes libwayland sends, and the
compositor and the client agree on them because neither of them knows the other
is not the usual one.

| Command | What it does |
|---|---|
| [`sway`](#sway) | the tiling Wayland compositor |
| [`wlterm`](#wlterm) | a terminal emulator, as a Wayland client |
| [`swaymsg`](#swaymsg) | ask sway something, or tell it to do something |
| [`waylandd`](#waylandd) | a bare display server, for testing clients against |
| [`wlprobe`](#wlprobe) | list what a display server advertises |

## sway

```
sway
```

A tiling Wayland compositor. It takes the screen and the keyboard, draws
windows on the framebuffer the text console was using, and gives both back when
it exits.

```
root@wos:/home/root# sway
```

The screen goes dark, a bar appears at the bottom, and the middle of the screen
says which keys open a window. **Super+Return** opens a terminal.

Or start it as a service, which is the same thing without a shell holding on to
it:

```
root@wos:/home/root# systemctl start sway
sway: running as pid 7, disabled at boot
```

It is disabled at boot on purpose. A compositor that starts before you log in
takes the console away from you before you have used it, and on a machine where
the console is also how you fix things that is the wrong default. `systemctl
enable sway` changes it if you disagree.

### The keys

These come from `/etc/sway/config` and can all be changed. `$mod` is Super.

| Keys | What happens |
|---|---|
| `$mod+Return` | open a terminal |
| `$mod+Shift+Q` | close the focused window |
| `$mod+Shift+C` | reread the configuration file |
| `$mod+Shift+E` | leave sway |
| `$mod+H` `J` `K` `L` | move the focus left, down, up, right |
| `$mod+Left` `Down` `Up` `Right` | the same, with the arrow keys |
| `$mod+Shift+`*direction* | move the window itself |
| `$mod+B` | the next window opens beside this one |
| `$mod+V` | the next window opens below this one |
| `$mod+E` | turn the current container the other way |
| `$mod+F` | fullscreen the focused window |
| `$mod+1` … `$mod+0` | go to a workspace |
| `$mod+Shift+1` … | move the window to a workspace |

Anything not bound goes to the focused window untouched, which is what `$mod`
is for: it marks a keystroke as being for the compositor, so every other key
belongs to the program.

### The layout

Windows never overlap. A workspace holds a tree of containers, each dividing
its rectangle between its children, and every window's position falls out of
one walk from the root — the same model i3 and sway use.

A new window opens **beside the focused one, inside the focused one's
container**. An empty workspace splits along its longest side, so on a screen
wider than it is tall the second window appears to the right without anybody
having asked for it.

### The configuration file

Read from `~/.config/sway/config`, and failing that `/etc/sway/config` — the
same places, in the same order, as the real sway. It is written in sway's
language, which is not a list of settings but a list of commands:

```
set $mod Mod4
set $term wlterm

output * bg #101820 solid_color

bindsym $mod+Return exec $term
bindsym $mod+Shift+q kill
bindsym $mod+1 workspace 1

default_border normal
client.focused #4c7899 #285577 #ffffff #2e9ef4 #285577
gaps inner 0
```

Those are the same commands `swaymsg` sends at runtime, which is why one parser
reads both and why `reload` works at all: rereading the file is running it
again.

**A directive this compositor cannot honour is accepted and ignored, not
refused** — a configuration written for the real sway should start this one.
Each one that is ignored says so in `/ramdisk/sway.log`:

```
root@wos:/home/root# cat /ramdisk/sway.log
sway on a 640x400 screen
ipc: listening on /ramdisk/sway-ipc.sock
font: this compositor draws one 8x16 font and cannot change it
bar { ... }: read but not acted on
read /home/root/.config/sway/config
```

### What it has not got

Each of these is missing because something underneath it is, rather than
because it was not got to:

- **Floating windows.** There is no mouse driver, so there is no pointer to
  drag a window with. `floating_modifier` is accepted and does nothing.
- **swaybar as a separate process.** The bar is drawn by the compositor. A bar
  as a client would need the layer-shell protocol — a surface that is not a
  window and does not tile — and without it a bar would be a window taking up a
  tile, which is not a bar.
- **XWayland.** There is no X server to run.
- **An XKB keymap.** `wl_keyboard.keymap` is sent with format `no_keymap`,
  honestly, and clients read the evdev key codes directly. `wkeychar()` in
  wkernel is what a client uses instead; see
  [`docs/wkernel-api.md`](wkernel-api.md).
- **Several outputs.** There is one framebuffer, at whatever size the console
  left it. `output * bg` works; nothing else about an output does.

**Exit status:** 1 if it cannot take the screen, the keyboard or the socket;
otherwise it runs until told to exit.

## wlterm

```
wlterm [-e <program> [args...]]
```

A terminal emulator that is an ordinary Wayland client — it has no privileges
and no special arrangement with the compositor. It asks for a surface, is told
how big it is, draws into shared memory and hands over the descriptor.

With no arguments it runs your login shell, so a window opens the same thing a
console login would. `-e` runs something else:

```
$mod+Return                      a shell
wlterm -e vim                    an editor, in its own window
```

The terminal inside it is the same emulator `vim`'s `:term` and `split` use, so
the ANSI sequences, the colours and the key encodings behave identically —
what changed is only that the grid is drawn with pixels instead of being handed
to the console.

The window is redrawn from the grid every time the child prints. It uses two
buffers and alternates between them, so the compositor is never reading a
buffer the terminal is drawing into.

**Exit status:** 0 when the shell exits or the window is closed.

## swaymsg

```
swaymsg [-t <type>] [-r] [message]
```

Talk to the compositor over its IPC socket. With no `-t`, the message is a sway
command:

```
root@wos:/home/root# swaymsg splitv
root@wos:/home/root# swaymsg 'workspace 2'
root@wos:/home/root# swaymsg exit
```

With `-t`, it asks a question:

```
root@wos:/home/root# swaymsg -t get_workspaces
NUM  NAME     WINDOWS
1    1        2        (focused)
2    2        1

root@wos:/home/root# swaymsg -t get_tree
root root
  output WOS-1
    workspace 1 [splith]
      whell
      whell  *

root@wos:/home/root# swaymsg -t get_version
sway version 1.0 (sway for WOS)
configuration: /home/root/.config/sway/config
```

Types: `run_command`, `get_workspaces`, `get_outputs`, `get_tree`, `get_marks`,
`get_version`, `get_binding_modes`, `get_config`, `send_tick`, `subscribe`.
`-r` prints the raw JSON instead of a summary.

The protocol is i3's, unchanged — a magic string, a length, a type and a JSON
payload — so the interesting property is not that this program works but that
it is not the only thing that can. Anything that speaks i3's IPC speaks to this
compositor.

`subscribe` is accepted and no events are ever sent, because this compositor
raises none.

**Exit status:** 0, or 1 if sway is not running or the command failed.

## waylandd

```
waylandd [display-name]
```

A display server with nothing on the screen. It is not the compositor — `sway`
is — and it exists for two reasons: it is what you test a *client* against when
you want to know whether the client is right, with no screen and no window
management in the way; and it is the reference for what the protocol library
alone gives you, since `wl_display` and `wl_registry` are implemented by the
library and the `wl_compositor` it advertises is thirty lines.

It answers on **wayland-1**, not wayland-0. The default display belongs to the
compositor and two servers cannot share a name.

```
root@wos:/home/root# systemctl status wayland
wayland -- the bare Wayland display server, on wayland-1
   state: running as pid 4
 at boot: enabled
    runs: /app/waylandd/launch
```

It logs to `/ramdisk/waylandd.log`, because a service has no terminal to be
rude to.

## wlprobe

```
wlprobe [display-name]
```

An ordinary Wayland client that asks a display server what it can do. Connect,
get the registry, add a listener, roundtrip — the opening of every Wayland
client there has ever been — and then print what came back.

```
root@wos:/home/root# wlprobe
connected to wayland-0
6 globals:
   1  wl_compositor    version 6   makes surfaces
   2  wl_shm           version 1   shared memory buffers
   3  xdg_wm_base      version 6   windows
   4  wl_seat          version 9   keyboard and pointer
   5  wl_output        version 4   a screen
the server is answering

root@wos:/home/root# wlprobe wayland-1
connected to wayland-1
1 global:
   1  wl_compositor    version 6   makes surfaces
the server is answering
```

It is the first thing to run when a client will not start: it says whether the
display server is up and whether it advertises the thing the client needs.

**Exit status:** 0, or 1 if it cannot connect or the connection breaks.

---

# textmode

```
textmode [<cols> <rows>]
```

Show or change the console's character grid. With no arguments it prints the
current size and some presets; with two, it switches.

```
root@wos:/home/root# textmode
Current text mode: 80x25

Some sizes (use: textmode <cols> <rows>; 40x25 up to 240x75):
  80 25  (640x400 px)   (current)
  100 37  (800x592 px)
  120 40  (960x640 px)
  128 48  (1024x768 px)
  160 50  (1280x800 px)
  200 60  (1600x960 px)
root@wos:/home/root# textmode 160 50
Text mode is now 160x50.
```

The console is a linear framebuffer rendering an 8x16 font (see
[the console notes](console.md)), so the grid is not tied to a handful of VGA
text modes — it can be **almost any size**, `cols*8` by `rows*16` pixels, from
40x25 up to 240x75. Switching reprograms the display resolution and clears the
screen. Because the font is drawn at native resolution, high-density grids like
160x50 stay crisp instead of blocky.

Programs that draw to the whole screen — the editor, `split`, `asciiquarium` —
read the size with `wconsize()`, so they lay themselves out to whatever grid is
set. Start them after switching and they fit. (Line-oriented tools like `ls`
still assume 80 columns for their column layout.)

**Exit status:** 0, 2 for a usage error.

---

# chsh

```
chsh                     show the current shell and the choices
chsh <shell>             set your own login shell
chsh -u <user> <shell>   set another user's login shell
```

Change a user's **login shell** — the one started for them when the machine
boots (for root) or when someone `su`s to them. After Unix's `chsh`.

```
root@wos:/home/root# chsh
Current shell: /app/whell/launch

Available shells (chsh <name>):
  whell    /app/whell/launch
  fish     /app/fish/launch
root@wos:/home/root# chsh fish
Shell for root is now /app/fish/launch.
It takes effect the next time a shell starts for root (su, or the next boot).
```

A shell is named by app — `whell`, `fish` — which maps to `/app/<name>/launch`,
or by an explicit path. It must be an existing executable; a bad login shell
would leave the account unusable, so `chsh` refuses one that is not there.

| Who | May change |
|---|---|
| any user | their own shell |
| root, or a `usereditor` | anyone's, with `-u <user>` |

The setting is stored as a fourth field on the user's line in
`/userconfig/users` (`name:uid:roles:shell`) and so survives a reboot. An empty
shell — `chsh ""` — restores the default, `whell`.

The change applies the **next** time a shell starts for that user, not to one
already running: `su` reads it when it launches, and the kernel reads root's
when it starts or restarts the boot shell. So after `chsh fish`, either `su
root` or exiting the boot shell brings up fish.

**Exit status:** 0, 1 on error (unknown user, not permitted, no such
executable), 2 for a usage error.

---

# ping

```
ping <ip>            send four echo requests
ping -c <n> <ip>     send n (0 keeps going until interrupted)
```

Send ICMP echo requests to a host and time the replies, over WOS's small IPv4
stack (see [`docs/networking.md`](networking.md)).

```
root@wos:/home/root# ping -c 2 10.0.2.2
PING 10.0.2.2 32 bytes of data.
32 bytes from 10.0.2.2: icmp_seq=1 time=0.121 ms
32 bytes from 10.0.2.2: icmp_seq=2 time=0.027 ms

--- 10.0.2.2 ping statistics ---
2 packets transmitted, 2 received, 0% packet loss
```

Addresses are **dotted-decimal only** — there is no DNS. On QEMU's user-mode
network the reliable targets are the gateway `10.0.2.2` (answers locally, sub-
millisecond) and, if the host allows unprivileged ICMP, real addresses beyond
it like `8.8.8.8`, which route through the gateway and come back in tens of
milliseconds.

Round-trip time is measured with the CPU's timestamp counter, calibrated
against the timer at boot, so it resolves well below a millisecond.

| Reply | Meaning |
|---|---|
| `32 bytes from ...` | an echo reply came back; `time=` is the round trip |
| `host unreachable (no ARP reply)` | the next hop never answered ARP |
| `Request timed out` | no reply within a second |
| `ping: no network card` | the kernel found no RTL8139 |

**Exit status:** 0 if any reply was received, 1 if none, 2 for a usage error.

---

# curl

```
curl <url>        print the body
curl -i <url>     print headers and body
curl -I <url>     print headers only
```

Fetch a URL over HTTP and print it.

```
root@wos:/home/root# curl http://example.com
<!doctype html><html ...><h1>Example Domain</h1><p>This domain is for use in
documentation examples ...</p></html>
```

HTTP only — `https://` is refused, because WOS has no TLS. The host may be a
name (resolved over DNS) or a dotted address. See
[`docs/networking.md`](networking.md) for the stack underneath.

**Exit status:** 0 for a 2xx/3xx response, 1 otherwise or on a network error,
2 for a usage error.

---

# wget

```
wget <url>            save to the file named in the URL (or index.html)
wget -O <file> <url>  save to <file>
```

Download a URL to a file, following up to a few HTTP redirects.

```
root@wos:/home/root# wget http://example.com
Connecting to http://example.com ...
Saved 559 bytes to index.html
```

The output name is the last path component of the URL, or `index.html` when the
path is empty. HTTP only.

**Exit status:** 0 on success, 1 on error, 2 for a usage error.

---

# lynx

```
lynx <url>
```

Browse the web as text. `lynx` fetches a page, renders the HTML — dropping tags
and scripts, decoding entities, wrapping text and numbering links — and shows
it in a full-screen pager.

```
Example Domain

This domain is for use in documentation examples without needing permission.
Avoid use in operations.

Learn more [1]
 http://example.com  (1 links)  [num]=follow g=go b=back q=quit
```

| Key | Action |
|---|---|
| Up / Down, `j` / `k` | scroll a line |
| Space, PgDn | scroll a page |
| a number then Enter | follow that link |
| `g` | type a new URL to open |
| `b` | back to the previous page |
| `q` | quit |

A WOS-native browser in the spirit of lynx, not a build of it: no CSS, no
JavaScript, no forms, and HTTP only. It renders the *text* of the web, which for
a great many pages is what you came for. A link to an `https://` page reports
that WOS cannot fetch it.

**Exit status:** 0.

---

# time

```
time                        show the current date and time
time HH:MM[:SS]             set the time of day, keeping the date
time YYYY-MM-DD HH:MM[:SS]  set the whole date and time
```

Show or set the wall clock.

```
root@wos:/home/root# time
Monday August 3 2026, 13:26:19
root@wos:/home/root# time 12:34:56
Clock set to: Monday August 3 2026, 12:34:56
```

The clock is the CMOS real-time clock, read and written through the 0x70/0x71
ports. Under QEMU it starts from the host's time, which is why a fresh boot
already knows the date; the weekday is computed from the date rather than read
from the chip, since that register is not always trustworthy.

Setting the clock is **root only** — it is a system-wide setting — so an
ordinary user gets "only root may set the clock". A change lasts until the VM
resets, when QEMU seeds the emulated clock from the host again.

**Exit status:** 0, 1 on error (not root, cannot read the clock), 2 for a bad
argument.
