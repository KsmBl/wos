# whell — the WOS shell

`whell` is the first WOS application and the one the kernel starts at boot. It
reads a line, splits it into arguments, and either runs a builtin or launches a
program.

```
whell -- the WOS shell. Type `help` for the builtins.

wos:/home$ ls
boots.txt   notes.txt   readme.txt
wos:/home$
```

The prompt is `wos:<working directory>$ `.

If whell ever exits, the kernel restarts it, so the machine always has a shell.

## How commands are found

WOS has no `PATH` variable. Every application lives in its own directory under
`/app`, with its executable at `/app/<name>/launch` and its source in
`/app/<name>/sourcecode/`. A bare command name maps straight onto that layout:

| What you type | What runs |
|---|---|
| `hello` | `/app/hello/launch` |
| `whell` | `/app/whell/launch` |
| `/app/hello/launch` | that exact path |
| `./launch` | that path, relative to the working directory |

Anything containing a `/` is treated as a path and run directly. Anything else
is looked up in `/app`. A name that resolves to nothing gives:

```
whell: nosuchcmd: command not found
```

Programs run as child processes and whell waits for each one to finish. Its
exit status follows the usual shell convention: the program's own status, or
`127` if the command was not found, or `126` if it was found but could not be
started.

## Line editing

Editing is handled by the kernel's console, which is line buffered exactly like
a Linux terminal:

| Key | Effect |
|---|---|
| Backspace | delete the character before the cursor |
| Ctrl+C | discard the line being typed |
| Enter | submit the line |

Arguments are separated by spaces or tabs. `"double quotes"` and
`'single quotes'` group text containing spaces into one argument, and the
quotes themselves are removed.

---

# Builtins

## `ls` — list directory contents

```
ls [-l] [-a] [path...]
```

With no operand, lists the working directory. With several, each directory gets
a `path:` header and a blank line between them; a file operand is listed as
itself rather than being descended into. Entries are sorted by name, and plain
output is laid out in columns that fit an 80-column terminal, with a `/` after
directory names.

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

WFS stores no owners, permissions or timestamps, so the corresponding Linux
columns are absent rather than filled with invented values.

**Exit status:** 0, or 1 if any operand could not be read.

```
wos:/home$ ls /nope
ls: /nope: no such file or directory
```

## `cd` — change directory

```
cd [path | -]
```

| Form | Effect |
|---|---|
| `cd` | go to `/home` |
| `cd <path>` | go to that directory |
| `cd -` | go back to the previous directory, and print it |

`.` and `..` work as expected and are resolved by the kernel, so `cd /app/../home`
lands in `/home`.

**Exit status:** 0, or 1 if the directory does not exist, is not a directory,
or `cd -` was used before any other `cd`.

## `pwd` — print working directory

```
pwd
```

Prints the working directory as an absolute, normalised path.

**Exit status:** 0, or 1 if the path could not be read.

## `free` — show memory use

```
free [-b | -k | -m | -h]
```

```
wos:/home$ free
          total        used        free
Mem:     262016        5360      256656
Swap:         0           0           0
```

| Option | Units |
|---|---|
| *(none)* | kibibytes, as Linux does by default |
| `-b` | bytes |
| `-k` | kibibytes |
| `-m` | mebibytes |
| `-h` | human readable, e.g. `250.6M` |

The figures come from the kernel's physical frame allocator, so `used` is
exactly the RAM currently allocated — including the kernel's own image, heap
and page tables — and `used + free == total` always holds.

WOS has no swap. The `Swap:` row is printed as zeroes so the output matches
what a reader of `free` expects to find.

**Exit status:** 0, or 1 on an invalid option.

## `df` — show disk use

```
df [-b | -k | -m | -h]
```

```
wos:/home$ df
Filesystem    1K-blocks       Used  Available Use% Mounted on
wfs               65536        302      65234   1% /

inodes: 20 used, 2028 free, 2048 total
```

Options are the same as `free`. The numbers come from the filesystem's block
bitmap, so `Used` counts blocks that are genuinely allocated, including the
superblock, bitmap and inode table. A disk with anything at all on it reports
at least 1%, never 0%.

**Exit status:** 0, or 1 if no filesystem is mounted or the option is invalid.

## `ps` — show processes

```
ps
```

```
wos:/home$ ps
  PID NAME          RESIDENT     CODE     DATA     HEAP    STACK THR
    6 whell            92.0K    12.0K     8.0K       0B    64.0K   1
```

`RESIDENT` is counted from the process's page tables: it is what the process
actually occupies, including its own page tables, so it is slightly more than
the four columns beside it add up to.

**Exit status:** 0.

## `cat` — print files

```
cat file...
```

Writes each file to standard output in order. Continues after a file it cannot
open, reporting each failure.

**Exit status:** 0, or 1 if any file could not be read.

## `shutdown` — power the machine off

```
shutdown
```

Powers the machine off. Nothing needs flushing first: WFS writes its
superblock, block bitmap and inodes straight through on every change, so the
disk is consistent at every moment.

```
wos:/home$ shutdown
shutting down

[kernel] shutting down
```

Under QEMU, VirtualBox or Bochs the VM exits. On real hardware the kernel has
no ACPI parser to find the platform's soft-off registers, so it reports that
and halts the CPU instead:

```
[kernel] no ACPI soft-off available on this machine
[kernel] it is now safe to turn off the power
```

There is no user or permission model in WOS, so any process can do this.

**Exit status:** does not return on success; 1 if the machine could not be
powered off.

## `help` — list the builtins

```
help
```

**Exit status:** 0.

## `exit` — leave the shell

```
exit [status]
```

Exits with `status`, or 0. Since whell is what the kernel runs, exiting simply
causes the kernel to start a fresh one:

```
wos:/home$ exit
whell: goodbye

[kernel] whell exited with status 0; restarting it
```

---

# Source

whell lives in `app/whell/sourcecode/`, and the same source is on the disk at
`/app/whell/sourcecode/`, readable with `cat`:

| File | Contents |
|---|---|
| `whell.c` | the main loop, prompt, dispatch and program launching |
| `parse.c` | splitting a line into arguments, with quoting |
| `cmd_ls.c` | `ls` |
| `cmd_mem.c` | `free`, `df`, `ps` |
| `cmd_nav.c` | `cd`, `pwd`, `cat`, `help` |
| `whell.h` | shared declarations |

Every builtin has the same shape as `main()` — it takes `argc`/`argv` with the
command name in `argv[0]` and returns an exit status — so adding one means
writing that function and adding a line to the table in `whell.c`.
