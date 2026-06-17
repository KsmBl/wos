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

## `int wdisklist(wdisk_t *out, int max)`

Every mounted filesystem, not just the one the system is on. WOS mounts two:
the disk at `/`, and the one held in memory at `/ramdisk` that starts empty and
is gone at the next boot.

```c
typedef struct {
    char        mount[28];    /* where it appears: "/" or "/ramdisk"      */
    char        device[24];   /* what it is: "ATA disk", "memory", ...    */
    uint32_t    persistent;   /* 0 when what is written is gone at reboot */
    uint32_t    pad;
    wdiskinfo_t usage;
} wdisk_t;
```

`max` is the size of the array; `W_DISK_MAX` is always enough.

**Returns** the number of filesystems written, or `-W_EFAULT`.

---

# Local sockets

A connection-oriented byte stream between two processes, named by a path, able
to carry file descriptors alongside the bytes — a Unix domain socket, in the
shape WOS needs it.

A pipe reaches another process only by being inherited across a spawn. That is
enough for a shell and a terminal emulator, and not enough for a display
server: a client has to find the compositor by name having never been its
child, talk in both directions, and hand over a descriptor for a buffer rather
than a copy of it.

## `int wlisten(const char *path)`

Answer to `path` from now on. Nothing is created on the disk — the path is an
address, and it is gone when the returned descriptor closes. Answering to a
name counts as writing where it lives, so this needs write permission on the
directory, which for an ordinary user means somewhere under their own home.

**Returns** a descriptor to accept connections on, `-W_EEXIST` if the name is
taken, `-W_EACCES`, `-W_ENFILE` or `-W_EMFILE`.

## `int wconnect(const char *path)`

Connect to whoever is listening on `path`. Returns as soon as the connection is
queued rather than waiting to be accepted, so a client may start sending
immediately.

**Returns** a connected descriptor, `-W_ENOENT` if nothing is listening,
`-W_EBUSY` if the backlog is full, or `-W_EMFILE`.

## `int waccept(int fd)`

Take the next connection waiting on a listening descriptor, blocking until one
arrives. Poll first to wait for one without blocking here.

**Returns** a connected descriptor, or `-W_EBADF`.

## `int wsend(int fd, wmsg_t *msg)` / `int wrecv(int fd, wmsg_t *msg)`

```c
typedef struct {
    void    *buf;
    uint32_t len;
    int32_t  fd_count;   /* in: how many to pass; out: how many arrived */
    int32_t *fds;
} wmsg_t;
```

```c
char     bytes[256];
int      fds[4];
wmsg_t   msg = { bytes, sizeof(bytes), 4, fds };

int n = wrecv(fd, &msg);
/* n bytes in `bytes`, msg.fd_count descriptors in `fds` */
```

A descriptor passed this way is copied into the receiver's table with a
reference of its own: closing yours afterwards does not close its. At most
`W_SEND_MAX_FDS` (28) travel with one message — the same limit libwayland
uses, and for the same reason.

**Delivery order matters and is guaranteed.** A descriptor is delivered no
earlier than the byte it was sent with, so a receiver that has read the message
describing a buffer is holding that buffer's descriptor by then, and never
before. Reading in small pieces cannot make one arrive early.

Both block the way a pipe does: a send waits for room, a receive waits for
bytes. `wsend` returns `-W_EPIPE` once the far end has gone; `wrecv` returns 0.

An ordinary `wread()`/`wwrite()` on a socket descriptor works too, and carries
no descriptors.

## `int wpoll(wpollfd_t *fds, int count, int timeout_ms)`

Wait until one of several descriptors is ready. Sockets, pipes and the console
can be waited on together, which is what a program serving several clients at
once needs and what `wpollin()` — one descriptor, no waiting — cannot do.

```c
typedef struct {
    int32_t fd;
    int16_t events;    /* W_POLLIN | W_POLLOUT */
    int16_t revents;   /* what is true now */
} wpollfd_t;
```

| Flag | Meaning |
|---|---|
| `W_POLLIN` | reading would not block |
| `W_POLLOUT` | writing would not block |
| `W_POLLHUP` | the other end has gone (reported whether asked for or not) |
| `W_POLLERR` | not a descriptor that can be waited on (likewise) |

`timeout_ms` of 0 returns at once; negative waits forever. At most `W_POLL_MAX`
(32) descriptors.

**Returns** how many entries came back with a non-zero `revents`, 0 on timeout,
or `-W_EINVAL` / `-W_EFAULT`.

A socket becoming ready wakes the poller immediately. Other descriptors are
noticed within one timer tick, because nothing announces their readiness to the
scheduler.

---

# Processors

## `int wcpuinfo(wcpuinfo_t *out)`

What the machine's processor is, and what it says it can do.

```c
typedef struct {
    int32_t  count;      /* logical processors the machine has     */
    int32_t  online;     /* how many of them the kernel runs on    */
    uint32_t tick_hz;    /* rate the usage counters advance at     */
    uint32_t base_khz;   /* the clock the part is specified at     */
    uint32_t min_khz;    /* slowest the machine says it will go, 0 */
    uint32_t max_khz;    /* fastest, including turbo, 0 if unknown */
    char     brand[52];  /* what the CPU calls itself, or empty    */
} wcpuinfo_t;
```

A clock of 0 means the machine would not say. That is common inside a
hypervisor, which answers CPUID but not the model-specific registers the rest
of the figures live in.

**Returns** 0, or `-W_EFAULT`.

## `int wcpulist(wcpu_t *out, int max)`

The per-core figures. `max` is the size of the array; `W_CPU_MAX` is always
enough.

```c
typedef struct {
    int32_t  id;           /* 0-based, and the position in the list       */
    uint32_t apic_id;      /* what the firmware calls it                  */
    uint32_t online;       /* 1 if the kernel executes on this core       */
    uint32_t clock_khz;    /* current clock, 0 when unknown               */
    uint32_t clock_source; /* W_CLOCK_*: how clock_khz was arrived at     */
    int32_t  temp_c;       /* degrees Celsius, or W_TEMP_UNKNOWN          */
    int32_t  temp_max_c;   /* the temperature the CPU throttles itself at */
    uint32_t busy_ticks;   /* timer ticks spent running something         */
    uint32_t idle_ticks;   /* timer ticks spent with nothing to run       */
} wcpu_t;
```

WOS starts only the processor it booted on, so every other core comes back with
`online` clear, no clock and no temperature: a reading has to be taken by the
core it describes, and nothing is running there to take it. They are listed
anyway, because they are part of the machine.

`clock_source` says how much the clock is worth:

| Value | Meaning |
|---|---|
| `W_CLOCK_APERF` | measured over the last tick — the real clock |
| `W_CLOCK_TSC` | the timestamp counter's rate, i.e. the base clock, timed at boot |
| `W_CLOCK_CPUID` | the base clock CPUID quoted, never measured |
| `W_CLOCK_NONE` | nothing could be established |

`busy_ticks` and `idle_ticks` are cumulative since boot, and wrap the way the
timer does. A load figure is the change in `busy_ticks` over the change in
both, between two samples — a single sample gives the average since boot, which
is a different and much less interesting number.

```c
wcpu_t before[W_CPU_MAX], after[W_CPU_MAX];
int n = wcpulist(before, W_CPU_MAX);
/* ... wait a second ... */
wcpulist(after, W_CPU_MAX);

unsigned busy = after[0].busy_ticks - before[0].busy_ticks;
unsigned idle = after[0].idle_ticks - before[0].idle_ticks;
unsigned percent = (busy + idle) ? busy * 100 / (busy + idle) : 0;
```

**Returns** the number of cores written, or `-W_EFAULT`.

## `int wcpufreq(int khz)`

Ask the processor to run at a particular clock. Zero or less hands the decision
back to the hardware.

The request is clamped to the range `wcpuinfo()` reported and rounded to a step
the hardware can take — `step_khz`, usually 100 MHz — so the clock that comes
back is rarely the exact one asked for.

There is one clock and every process on the machine runs on it, so this needs
root or the `editfreq` role: a slow machine is slow for everybody, and a fast
one is hot for everybody.

**Returns** the clock settled on in kHz (0 for automatic), `-W_EPERM` without
the role, or `-W_ENODEV` on a machine whose clock cannot be set — the usual
answer inside a hypervisor, where the registers carrying the request are not
emulated.

## `int wbattery(wbattery_t *out)`

The machine's battery, as far as the firmware describes it.

```c
typedef struct {
    uint32_t present;        /* 1 if this machine has a battery at all   */
    uint32_t state;          /* W_BATTERY_*                              */
    int32_t  charge_percent; /* -1 when it cannot be read                */
    int32_t  ac_online;      /* 1 on mains, 0 on battery, -1 not known   */
    uint32_t design_mwh;     /* what it holds when new, 0 unknown        */
    uint32_t design_mv;      /* nominal voltage in millivolts, 0 unknown */
    uint32_t chemistry;      /* W_BATTERY_CHEM_*                         */
    uint32_t pad;
    char     name[32];       /* the pack's device name, or empty         */
    char     maker[32];      /* who made it, or empty                    */
    char     location[32];   /* where it sits, e.g. "Rear"               */
} wbattery_t;
```

`charge_percent` is almost always -1, and that is not a failure. Every laptop
reports its charge through an ACPI method that reads the embedded controller,
and calling one means interpreting AML bytecode, which WOS has no interpreter
for. Everything static is read out of the firmware's tables; the one figure
that changes minute to minute is the one that needs the interpreter, so it is
reported as unknown rather than guessed at. `state` and `ac_online` are unknown
for the same reason. See [`battery`](apps.md#battery).

**Returns** 0, or `-W_EFAULT`. Every field is zeroed first, so an absent battery
reads as `present == 0` and nothing else.

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

This is not a way to wait. A loop that yields is still a process asking to run,
and the processor is fully occupied going round it.

## `void wsleep(int ms)`

Stop running for `ms` milliseconds. The process leaves the run queue entirely,
so the machine can be genuinely idle while it waits — which is what makes a CPU
usage figure mean anything, and on a laptop is the difference between a fan
that runs and one that does not.

Rounded up to whole timer ticks (10 ms), and it returns as soon as possible
after the deadline rather than exactly on it: the process has to be scheduled
again like any other. Zero or less is a `wyield()`.

A full-screen program that must stay responsive sleeps in short slices rather
than one long one:

```c
while (!wpollin(W_STDIN) && wticks() < until)
    wsleep(20);        /* 50 times a second, not as fast as it can go */
```

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

A precision truncates `%s`, which is how a full-screen program cuts text at a
column without copying it into a buffer first:

```c
wprintf("%.*s", width, title);      /* at most `width` characters */
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

### `const char *wclock_string(unsigned int khz)`

Format a clock rate for people: `1900000` becomes `1.90GHz`, `400000` becomes
`400MHz`, and `0` becomes `-`. Two decimal places in gigahertz, because one is
not enough to tell neighbouring steps of a processor's clock apart.

Same rotating buffers, and the same rules, as `whuman()`.

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

---

# Users and permissions

Every process runs as a user. Root is uid 0 and bypasses every check; anyone
else needs the matching role, and may write only inside their own home
directory (plus `/app`, with `W_ROLE_APPEDITOR`).

Password hashes are never readable from a program: the kernel owns the database
and does the checking, which is why none of these calls needs anything like
setuid. See [`docs/users.md`](users.md) for the whole picture.

```c
typedef struct {
    uint32_t uid;
    uint32_t roles;                /* W_ROLE_* bitmask */
    char     name[W_NAME_LEN + 1];
} wuser_t;
```

| Role | Grants |
|---|---|
| `W_ROLE_APPEDITOR` | write access under `/app` |
| `W_ROLE_USEREDITOR` | writing `/userconfig`: creating users, setting anyone's password, changing roles |

## `int wgetuid(void)`

**Returns** the uid this process runs as. Cannot fail.

## `int wuserinfo(int uid, wuser_t *out)`

Look a user up by id. Pass `-1` for the calling process's own user.

**Returns** 0, `-W_ENOENT`, or `-W_EFAULT`.

## `int wuserlist(wuser_t *out, int max)`

Fill `out` with up to `max` user records. **Returns** the number written, or
`-W_EFAULT`.

## `int wlogin(const char *name, const char *password)`

Check a password and, if it matches, become that user. Root may become anyone
without supplying one.

**Returns** the new uid, `-W_ENOENT` if there is no such user, `-W_EACCES` if
the password is wrong, or `-W_EFAULT`.

There is no way back — a process that drops to another user cannot return to
root — which is why `su` runs a fresh shell rather than changing the current
one.

## `int wpasswd(const char *name, const char *old, const char *new)`

Set a user's password. Root and holders of `W_ROLE_USEREDITOR` may set anyone's
without knowing the old one; anyone else may set only their own and must supply
it. An empty `new` clears the password.

**Returns** 0, `-W_ENOENT`, `-W_EPERM` (not permitted), `-W_EACCES` (the old
password is wrong), or `-W_EFAULT`.

## `int wuseradd(const char *name, const char *password, unsigned roles)`

Create a user and their home directory under `/home`. Only root and holders of
`W_ROLE_USEREDITOR` may do this.

**Returns** the new uid, `-W_EPERM`, `-W_EEXIST`, `-W_EINVAL` for a name that
cannot be part of a path, or `-W_ENOSPC`.

## `int wsetroles(const char *name, unsigned roles)`

Replace a user's roles outright. Only root and holders of `W_ROLE_USEREDITOR`
may do this, and root's own roles cannot be changed.

`roles` is the complete new bitmask rather than a delta, so read the current
set with `wuserinfo()` first if you mean to adjust one.

**Returns** 0, `-W_EPERM`, `-W_ENOENT`, or `-W_EFAULT`.

## `int wgetpass(const char *prompt, char *buf, wsize_t size)`

Prompt for a password and read it without echoing. Switches the console to raw
mode for the duration and restores whatever mode was in effect. Backspace
works; Ctrl+C abandons the entry.

**Returns** the length read, or a negative error.

Note that switching console modes discards anything typed but not yet
submitted, so text typed ahead of the prompt is dropped rather than being
captured into the password.
