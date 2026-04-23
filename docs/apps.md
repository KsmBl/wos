# Applications

Every application lives in `/app/<name>/`, with its executable at
`/app/<name>/launch` and its source in `/app/<name>/sourcecode/`. Typing the
bare name in `whell` runs it.

This is where the commands live. The shells contain almost nothing: only `cd`,
`exit` and `help` are builtins, because only those change state belonging to
the shell process. `ls`, `cat`, `free` and the rest are ordinary programs, so
`whell` and `fish` run exactly the same ones.

Each is statically linked against `libwkernel.a`, so a program is around 50 KiB
whatever it does. Seventeen of them come to about 1.2 MiB on a 64 MiB disk,
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
| [`shutdown`](#shutdown) | power the machine off |
| [`whoami`](#whoami) | print the current user and what it may do |
| [`passwd`](#passwd) | change a password |
| [`su`](#su) | start a shell as another user |
| [`adduser`](#adduser) | create a user, asking for a password |
| [`edituser`](#edituser) | add or remove roles |

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

Under QEMU, VirtualBox or Bochs the VM exits. On real hardware the kernel has
no ACPI parser to find the platform's soft-off registers, so it says so and
halts the CPU instead.

There is no user or permission model in WOS, so any process can do this.

**Exit status:** does not return on success; 1 if the machine could not be
powered off.

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
