# The wkernel API

`wkernel` is the interface between WOS applications and the kernel. An
application includes one header and links against one library:

```c
#include <wkernel.h>

int main(int argc, char **argv)
{
    wprintf("hello from pid %d\n", wgetpid());
    return 0;
}
```

The build does the rest: put your source in `app/<name>/sourcecode/`, run
`make`, and it is installed on the disk as `/app/<name>/launch` with its source
beside it in `/app/<name>/sourcecode/`.

## Conventions

These hold for every function in the API, so they are stated once here rather
than repeated in each entry.

**Errors.** Any function that can fail returns a negative number on failure:
the negation of one of the `W_E*` constants. Success returns 0, or a count, or
a descriptor. So the shape of every call is:

```c
int r = wopen("/home/notes.txt", W_O_RDONLY);
if (r < 0) {
    wprintf("open failed: %s\n", wstrerror(-r));
    return 1;
}
```

`wstrerror()` turns the positive code into a readable message.

**Paths** may be absolute (`/home/notes.txt`) or relative to the process's
working directory (`notes.txt`, `../app`). `.` and `..` are resolved before the
filesystem sees the path. The longest path is `W_PATH_MAX` (255) characters and
the longest single name component is `W_NAME_MAX` (27).

**Pointers** handed to the kernel are checked against the calling process's
page tables first. A bad pointer fails with `-W_EFAULT`; it cannot corrupt the
kernel or another process.

**Blocking.** Only three calls ever block: `wread()` on the console, `wwait()`,
and `wyield()`. Everything else returns promptly.

## Error codes

| Code | Value | Meaning |
|---|---|---|
| `W_EPERM` | 1 | operation not permitted |
| `W_ENOENT` | 2 | no such file or directory |
| `W_ESRCH` | 3 | no such process |
| `W_EIO` | 5 | disk or device error |
| `W_E2BIG` | 7 | argument list too long |
| `W_ENOEXEC` | 8 | not a valid executable |
| `W_EBADF` | 9 | bad file descriptor |
| `W_ECHILD` | 10 | no child processes |
| `W_ENOMEM` | 12 | out of memory |
| `W_EACCES` | 13 | permission denied |
| `W_EFAULT` | 14 | bad address passed to the kernel |
| `W_EBUSY` | 16 | resource busy |
| `W_EEXIST` | 17 | already exists |
| `W_ENOTDIR` | 20 | a path component is not a directory |
| `W_EISDIR` | 21 | is a directory |
| `W_EINVAL` | 22 | invalid argument |
| `W_ENFILE` | 23 | system file table full |
| `W_EMFILE` | 24 | too many open files in this process |
| `W_EFBIG` | 27 | file too large |
| `W_ENOSPC` | 28 | no space left on the disk |
| `W_ESPIPE` | 29 | seek on something that cannot seek |
| `W_ERANGE` | 34 | result does not fit in the buffer |
| `W_ENAMETOOLONG` | 36 | path or name too long |
| `W_ENOSYS` | 38 | no such syscall |
| `W_ENOTEMPTY` | 39 | directory not empty |

---

# Files

Descriptors 0, 1 and 2 are open on the console in every process
(`W_STDIN`, `W_STDOUT`, `W_STDERR`). Files opened with `wopen()` get
descriptor 3 upwards. A process may hold 32 descriptors at once.

## `int wopen(const char *path, int flags)`

Open a file, optionally creating it.

| Parameter | Meaning |
|---|---|
| `path` | file to open |
| `flags` | exactly one of `W_O_RDONLY`, `W_O_WRONLY`, `W_O_RDWR`, ORed with any of `W_O_CREAT`, `W_O_TRUNC`, `W_O_APPEND` |

| Flag | Effect |
|---|---|
| `W_O_CREAT` | create the file if it does not exist |
| `W_O_TRUNC` | discard any existing contents |
| `W_O_APPEND` | every write goes to the current end of the file |

**Returns** a descriptor (>= 3), or `-W_ENOENT`, `-W_ENOTDIR`, `-W_EISDIR`
(directory opened for writing), `-W_EMFILE`, `-W_ENOSPC`.

```c
int fd = wopen("/home/log.txt", W_O_WRONLY | W_O_CREAT | W_O_APPEND);
if (fd >= 0) {
    wwrite(fd, "another line\n", 13);
    wclose(fd);
}
```

## `int wclose(int fd)`

Close a descriptor. **Returns** 0, or `-W_EBADF`.

## `int wread(int fd, void *buf, wsize_t count)`

Read up to `count` bytes into `buf`.

On descriptor 0 this reads the console: it blocks until Enter is pressed and
then returns the whole line, including its trailing `\n`.

**Returns** the number of bytes read — possibly fewer than asked for near the
end of a file, and 0 at end of file — or `-W_EBADF`, `-W_EISDIR`, `-W_EACCES`
(opened write-only), `-W_EFAULT`.

## `int wwrite(int fd, const void *buf, wsize_t count)`

Write `count` bytes. Descriptor 1 and 2 go to the console.

**Returns** the number of bytes written, short only when the disk fills up, or
`-W_EBADF`, `-W_EISDIR`, `-W_EACCES` (opened read-only), `-W_EFAULT`,
`-W_ENOSPC`, `-W_EFBIG`.

## `int wlseek(int fd, int offset, int whence)`

Move the read/write position.

| `whence` | `offset` is measured from |
|---|---|
| `W_SEEK_SET` | the start of the file |
| `W_SEEK_CUR` | the current position |
| `W_SEEK_END` | the end of the file |

**Returns** the new absolute position, or `-W_EBADF`, `-W_EINVAL` (bad
`whence`, or a negative result), `-W_ESPIPE` (the console cannot seek).

Seeking past the end is allowed; writing there leaves a hole that reads back
as zeroes.

```c
int size = wlseek(fd, 0, W_SEEK_END);   /* file size without stat */
wlseek(fd, 0, W_SEEK_SET);              /* rewind */
```

## `int wstat(const char *path, wstat_t *out)`

Look up a file's metadata without opening it.

```c
typedef struct {
    uint32_t ino;      /* inode number, unique on the volume */
    uint32_t size;     /* length in bytes                    */
    uint32_t blocks;   /* disk blocks occupied               */
    uint32_t type;     /* W_FT_FILE or W_FT_DIR              */
} wstat_t;
```

**Returns** 0, or `-W_ENOENT`, `-W_ENOTDIR`, `-W_EFAULT`.

## `int wunlink(const char *path)`

Delete a file. **Returns** 0, or `-W_ENOENT`, `-W_EISDIR` (use `wrmdir()`).

---

# Directories

## `int wopendir(const char *path)`

Open a directory for reading. **Returns** a descriptor, or `-W_ENOENT`,
`-W_ENOTDIR`, `-W_EMFILE`.

## `int wreaddir(int fd, wdirent_t *out)`

Read the next entry. `.` and `..` are included, as they are real entries.

```c
typedef struct {
    uint32_t ino;
    uint32_t type;                 /* W_FT_FILE or W_FT_DIR */
    char     name[W_NAME_MAX + 1];
} wdirent_t;
```

**Returns** 1 when an entry was written, **0 at the end of the directory**, or
`-W_EBADF`, `-W_ENOTDIR`, `-W_EFAULT`. Note that the end of a directory is 0,
not an error — the loop condition is `== 1`.

```c
int d = wopendir("/app");
if (d >= 0) {
    wdirent_t e;
    while (wreaddir(d, &e) == 1)
        wprintf("%s%s\n", e.name, e.type == W_FT_DIR ? "/" : "");
    wclosedir(d);
}
```

## `int wclosedir(int fd)`

Close a directory descriptor. Identical to `wclose()`; it exists so directory
code reads symmetrically. **Returns** 0, or `-W_EBADF`.

## `int wmkdir(const char *path)`

Create a directory; its parent must exist. **Returns** 0, or `-W_EEXIST`,
`-W_ENOENT`, `-W_ENOSPC`.

## `int wrmdir(const char *path)`

Remove an empty directory. **Returns** 0, or `-W_ENOTDIR`, `-W_ENOTEMPTY`,
`-W_ENOENT`.

## `int wchdir(const char *path)`

Change this process's working directory. **Returns** 0, or `-W_ENOENT`,
`-W_ENOTDIR`.

## `int wgetcwd(char *buf, wsize_t size)`

Get the working directory as an absolute, normalised path. `W_PATH_MAX + 1`
bytes is always enough.

**Returns** the length written (excluding the NUL), or `-W_ERANGE`,
`-W_EFAULT`.

---

# Memory statistics

## `int wmeminfo(wmeminfo_t *out)`

System-wide RAM use, taken from the kernel's physical frame allocator. These
are measured numbers, not estimates: `used_bytes` is exactly the number of
4 KiB frames currently allocated, and `used + free == total` always holds.

```c
typedef struct {
    uint32_t total_bytes;    /* usable RAM the machine reported          */
    uint32_t used_bytes;     /* frames currently allocated               */
    uint32_t free_bytes;     /* total - used                             */
    uint32_t kernel_bytes;   /* kernel image, heap arena and page tables */
    uint32_t page_size;      /* always 4096                              */
} wmeminfo_t;
```

**Returns** 0, or `-W_EFAULT`.

```c
wmeminfo_t m;
wmeminfo(&m);
wprintf("%s free of %s\n", whuman(m.free_bytes), whuman(m.total_bytes));
```

## `int wprocmem(int pid, wprocmem_t *out)`

Memory use of one process. Pass `0` for the calling process.

```c
typedef struct {
    int32_t  pid;
    char     name[32];
    uint32_t resident_bytes;  /* frames mapped in its address space     */
    uint32_t code_bytes;      /* executable segments                    */
    uint32_t data_bytes;      /* initialised data and bss               */
    uint32_t heap_bytes;      /* grown through wsbrk()                  */
    uint32_t stack_bytes;     /* user stack                             */
    int32_t  thread_count;
} wprocmem_t;
```

`resident_bytes` is counted from the process's page tables — what it actually
occupies, including its page tables themselves, not a reservation.

**Returns** 0, or `-W_ESRCH`, `-W_EFAULT`.

## `int wthreadmem(int tid, wthreadmem_t *out)`

Memory and CPU use of one thread. Pass `0` for the calling thread.

```c
typedef struct {
    int32_t  tid;
    int32_t  pid;
    uint32_t kernel_stack_bytes;
    uint32_t user_stack_bytes;
    uint32_t cpu_ticks;       /* timer ticks this thread has run for */
} wthreadmem_t;
```

Every process currently has exactly one thread, so only the calling thread is
addressable. **Returns** 0, or `-W_ESRCH`, `-W_EFAULT`.

## `int wproclist(wprocmem_t *out, int max)`

Fill `out` with up to `max` process records. **Returns** the number written,
or `-W_EFAULT`.

```c
wprocmem_t procs[16];
int n = wproclist(procs, 16);
for (int i = 0; i < n; i++)
    wprintf("%5d  %-12s %8s\n", procs[i].pid, procs[i].name,
            whuman(procs[i].resident_bytes));
```

---

# Disk statistics

## `int wdiskinfo(wdiskinfo_t *out)`

Disk space and inode use, taken from the filesystem's block bitmap, so
`used_bytes` counts blocks that are genuinely allocated — including the
metadata the filesystem needs for itself.

```c
typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t block_size;      /* 1024 */
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
} wdiskinfo_t;
```

**Returns** 0, or `-W_EFAULT`. If no disk is mounted every field is zero.

---

# Processes

## `int wspawn(const char *path, char *const argv[])`

Load and run a program as a child. It runs concurrently with the caller and
inherits the caller's working directory. `argv` is NULL-terminated and
`argv[0]` should be the program name; `argv` itself may be NULL.

**Returns** the child's pid (> 0), or `-W_ENOENT`, `-W_ENOEXEC` (not a valid
32-bit x86 executable), `-W_ENOMEM`, `-W_EFAULT`.

```c
char *argv[] = { "hello", "spin", NULL };
int pid = wspawn("/app/hello/launch", argv);
if (pid > 0) {
    int status;
    wwait(pid, &status);
    wprintf("child exited with %d\n", status);
}
```

## `int wwait(int pid, int *status)`

Block until a child exits, then release its process slot. Pass `-1` to wait
for any child. `status` may be NULL. A process killed by a fault reports a
status of `-1`.

**Returns** the pid reaped, or `-W_ECHILD` (no such child), `-W_EFAULT`.

Until a child is waited for it keeps its slot in the process table, so a
long-running parent should reap the children it spawns.

## `void wexit(int status)`

End the calling process. Does not return. Descriptors are closed and all
memory is released; the parent sees `status` from `wwait()`.

Returning from `main()` does the same thing, using main's return value.

## `int wgetpid(void)`

**Returns** the calling process's id. Cannot fail.

## `void *wsbrk(int increment)`

Grow or shrink the process heap. This is what `malloc()` is built on; prefer
`malloc()` unless you are writing an allocator.

**Returns** the *previous* end of the heap — so `wsbrk(n)` gives you a pointer
to `n` newly usable, zero-filled bytes — or `(void *)-1` if the request cannot
be met. A negative `increment` returns memory to the system.

## `unsigned int wticks(void)`

**Returns** timer ticks since boot. The timer runs at 100 Hz, so one tick is
10 ms. Cannot fail.

## `unsigned int wuptime_ms(void)`

**Returns** milliseconds since boot. Cannot fail.

## `void wyield(void)`

Give up the rest of this timeslice. Purely an optimisation — the scheduler
preempts anyway.

## `int wconsole_raw(int mode)`

Switch the console between line-buffered and raw input.

| `mode` | Behaviour |
|---|---|
| `W_CONSOLE_CANONICAL` | the default: the kernel echoes as you type, handles backspace, and a `wread()` on descriptor 0 returns one whole line once Enter is pressed |
| `W_CONSOLE_RAW` | every keystroke is readable immediately and nothing is echoed |

Raw mode is what a program needs to react to individual keys — Tab, a pager
waiting for a single letter, a full-screen editor. In exchange it must echo
whatever it wants seen and do its own editing. Ctrl+letter arrives as the
corresponding control code, so Ctrl+C is `0x03`.

Switching modes discards anything typed but not yet submitted, so a change
never delivers half a line under the other discipline's rules.

**Returns** the mode that was in effect before, or `-W_EINVAL`.

The mode belongs to the console, not to the process, so a program that
switches to raw should switch back before it exits or before it spawns a child
that reads input.

```c
wconsole_raw(W_CONSOLE_RAW);
char c;
wread(W_STDIN, &c, 1);            /* returns as soon as a key is pressed */
wconsole_raw(W_CONSOLE_CANONICAL);
```

## `int wshutdown(void)`

Power the machine off. **Does not return on success.**

Nothing needs flushing first — the filesystem writes its metadata straight
through, so the disk is consistent at every moment.

**Returns** only on failure. `-W_ENOSYS` means the machine offers no soft-off
this kernel knows how to drive; in that case the kernel says so on the console
and halts the CPU rather than returning here, so in practice this call never
comes back.

There is no user or permission model in WOS, so any process may call it. On a
system with several users this would need a privilege check in the kernel.

```c
wprintf("goodbye\n");
wshutdown();
```

---

# POSIX-style aliases

The same calls under their familiar names, as inline forwards with no
overhead. Use whichever spelling reads better:

```c
int open (const char *path, int flags);
int close(int fd);
int read (int fd, void *buf, wsize_t n);
int write(int fd, const void *buf, wsize_t n);
int lseek(int fd, int offset, int whence);
```

```c
int fd = open("/home/readme.txt", W_O_RDONLY);
char buf[256];
int n = read(fd, buf, sizeof(buf));
write(W_STDOUT, buf, n);
close(fd);
```

---

# Convenience layer

Everything here is built on the calls above and needs no extra kernel support.

## Output

### `int wprintf(const char *fmt, ...)`

Print formatted text to stdout. **Returns** the number of characters written.

| Conversion | Prints |
|---|---|
| `%d`, `%i` | signed decimal |
| `%u` | unsigned decimal |
| `%x`, `%X` | hexadecimal, lower or upper case |
| `%c` | one character |
| `%s` | NUL-terminated string |
| `%p` | pointer, as `0x0040f120` |
| `%%` | a literal `%` |

A field width and the flags `-` (left align) and `0` (zero pad) are honoured,
and `*` takes the width from the argument list, so columns line up:

```c
wprintf("%-12s %6u %s\n", name, size, whuman(bytes));
```

There is no floating point: WOS never enables the FPU.

### `int wfprintf(int fd, const char *fmt, ...)`

As `wprintf()`, to a specific descriptor. Use `W_STDERR` for diagnostics.

### `int wsnprintf(char *buf, wsize_t size, const char *fmt, ...)`

As `wprintf()`, into a buffer. Always NUL-terminates. **Returns** the number
of characters written, not counting the NUL.

### `int wputs(const char *s)`

Write a string to stdout with no newline added. **Returns** the count written.

## Input

### `int wgetline(char *buf, wsize_t size)`

Read one line from the console, blocking until Enter is pressed. The trailing
newline is removed. **Returns** the line length, or a negative error.

```c
char line[256];
wprintf("> ");
if (wgetline(line, sizeof(line)) >= 0)
    wprintf("you typed: %s\n", line);
```

## Formatting helpers

### `const char *whuman(unsigned int bytes)`

Format a byte count for people: `268435456` becomes `256.0M`.

**Returns** a pointer into a rotating set of eight static buffers, so up to
eight results can be live in a single `wprintf()` call. Past that the earliest
buffer is reused and that argument prints the wrong value. Not reentrant.

### `const char *wstrerror(int err)`

Turn a positive `W_E*` code into a readable message. Negate what the failing
call returned.

## Heap

| Function | Behaviour |
|---|---|
| `void *malloc(wsize_t size)` | allocate `size` bytes; NULL on failure or for `size == 0` |
| `void *calloc(wsize_t n, wsize_t size)` | allocate and zero `n * size` bytes; NULL if the multiplication would overflow |
| `void *realloc(void *ptr, wsize_t size)` | resize, preserving contents; NULL on failure, and the original block stays valid |
| `void free(void *ptr)` | release a block; NULL is ignored |

The heap grows through `wsbrk()` in 16 KiB steps and never shrinks; everything
returns to the system when the process exits.

## Strings

```c
wsize_t strlen (const char *s);
int     strcmp (const char *a, const char *b);
int     strncmp(const char *a, const char *b, wsize_t n);
char   *strcpy (char *dst, const char *src);
char   *strcat (char *dst, const char *src);
char   *strchr (const char *s, int c);
char   *strrchr(const char *s, int c);
wsize_t strlcpy(char *dst, const char *src, wsize_t size);

void *memcpy (void *dst, const void *src, wsize_t n);
void *memmove(void *dst, const void *src, wsize_t n);
void *memset (void *dst, int c, wsize_t n);
int   memcmp (const void *a, const void *b, wsize_t n);

int atoi(const char *s);
```

`strlcpy()` always NUL-terminates and never overruns. It **returns the length
of `src`**, so a result `>= size` means the copy was truncated — which is what
makes truncation detectable, unlike `strncpy`.

---

# Writing an application

1. Create `app/<name>/sourcecode/` and put your `.c` files there.
2. `#include <wkernel.h>` and write a `main(int argc, char **argv)`.
3. Run `make`. The program is linked against `libwkernel.a` at `0x40000000`
   and installed on the disk as `/app/<name>/launch`, with its source copied
   to `/app/<name>/sourcecode/`.
4. In `whell`, type `<name>` to run it — a bare command name resolves to
   `/app/<name>/launch`.

A complete example lives in `app/hello/sourcecode/hello.c`; it exercises most
of this API and is what the kernel's boot-time self-test runs.

## `int wpollin(int fd)`

Ask whether a read would return immediately.

This is what lets a program stay responsive while doing something else: a
display that refreshes on a timer can check for a keypress between repaints
instead of blocking in `wread()` and freezing until one arrives.

**Returns** 1 if a `wread()` on `fd` would not block, 0 if it would. Only the
console can ever block, so any file reports 1.

```c
while (running) {
    redraw();
    unsigned until = wticks() + 100;          /* one second */
    while (wticks() < until) {
        if (wpollin(W_STDIN)) { handle_key(); break; }
        wyield();
    }
}
```

## `int wgetkey(void)`

Read one keystroke, decoding the escape sequences that special keys send.
Requires raw mode, and blocks until a key is pressed.

**Returns** an ordinary character as itself, or one of these for a special key:

| Constant | Key |
|---|---|
| `W_KEY_UP` `W_KEY_DOWN` `W_KEY_LEFT` `W_KEY_RIGHT` | the arrows |
| `W_KEY_HOME` `W_KEY_END` | Home, End |
| `W_KEY_PGUP` `W_KEY_PGDN` | Page Up, Page Down |
| `W_KEY_DELETE` | Delete |
| `W_KEY_ESCAPE` | a bare Escape |

Escape both introduces sequences and is a key in its own right; they are told
apart by whether anything follows immediately, which is the same guess a
terminal program makes.

```c
wconsole_raw(W_CONSOLE_RAW);
int key = wgetkey();
if (key == W_KEY_UP)
    move_up();
wconsole_raw(W_CONSOLE_CANONICAL);
```
