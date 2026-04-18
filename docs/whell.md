# whell — the WOS shell

`whell` is the first WOS application and the one the kernel starts at boot. It
reads a line, splits it into arguments, and either runs one of its three
builtins or launches a program.

The shell is deliberately small. `ls`, `cat`, `free` and the rest are not part
of it: they are ordinary programs in `/app`, documented in
[`docs/apps.md`](apps.md). Only the things that change state belonging to the
shell process itself are built in.

```
whell -- the WOS shell. Type `help` for an introduction.

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

whell puts the console into raw mode while reading a line, so it sees every
keystroke and does its own echoing and editing:

| Key | Effect |
|---|---|
| Backspace | delete the character before the cursor |
| Tab | complete the word being typed |
| Ctrl+C | abandon the line and start a fresh one |
| Enter | submit the line |

Raw mode is entered and left around each line rather than held for the shell's
lifetime. The mode belongs to the console rather than to a process, so leaving
it set would change how a program whell launches reads its own input.

Arguments are separated by spaces or tabs. `"double quotes"` and
`'single quotes'` group text containing spaces into one argument, and the
quotes themselves are removed.

## Tab completion

Tab completes the word at the end of the line. What it completes against
depends on the position:

- **The first word** is a command, so it is matched against the builtins and
  against `/app`. A directory under `/app` only counts if it really holds a
  `launch` binary — completing to one that does not would just produce
  `command not found`.
- **Any later word** is a path, matched against the directory it names.

A word containing a `/` is always treated as a path, even in first position.

The behaviour follows bash. One match completes fully and adds a trailing
space, or a `/` if it is a directory and you are likely to keep typing:

```
wos:/home$ hell<Tab>
wos:/home$ hello
wos:/home$ ls /ap<Tab>
wos:/home$ ls /app/
```

Several matches extend as far as they agree, and stop there:

```
wos:/home$ he<Tab>
wos:/home$ hel            (help and hello both start with "hel")
```

Pressing Tab again when there is nothing left to add lists the candidates and
redraws the line:

```
wos:/home$ hel<Tab>
help  hello
wos:/home$ hel
```

Hidden entries are only offered once you have typed the leading dot, which is
how shells keep dotfiles from burying the useful names.

---

# Builtins

There are three, and there only need to be three.

## `cd` — change directory

```
cd [path | -]
```

| Form | Effect |
|---|---|
| `cd` | go to `/home` |
| `cd <path>` | go to that directory |
| `cd -` | go back to the previous directory, and print it |

**This is the one command that cannot be a separate program.** A child process
gets a *copy* of its parent's working directory; changing it changes only that
copy, and when the child exits the shell is exactly where it started. So while
`pwd` works perfectly well as an application, `cd` has to run inside the shell.
Every Unix shell is built this way, for this reason.

`.` and `..` work as expected and are resolved by the kernel, so
`cd /app/../home` lands in `/home`.

**Exit status:** 0, or 1 if the directory does not exist, is not a directory,
or `cd -` was used before any other `cd`.

## `help` — an introduction

```
help
```

Lists the builtins and points at `/app` for everything else.

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
| `cmd_nav.c` | `cd` and `help` |
| `complete.c` | tab completion |
| `whell.h` | shared declarations |

Every builtin has the same shape as `main()` — it takes `argc`/`argv` with the
command name in `argv[0]` and returns an exit status — so adding one means
writing that function and adding a line to the table in `whell.c`. Before you
do, check whether it needs to be a builtin at all: unless it changes the
shell's own state, it belongs in `/app` instead.
