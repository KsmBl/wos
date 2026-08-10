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
| `W_EXDEV` | 18 | rename across two filesystems |
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

## `int wrename(const char *from, const char *to)`

Give a file a different name, or a different directory, **in one step**.

The one step is the point. A program replacing a file safely writes the new
contents under a temporary name and renames it over the old one:

```c
int fd = wopen("/home/root/.config/sway/config.new", W_O_WRONLY | W_O_CREAT);
/* ... write the whole file ... */
wclose(fd);
wrename("/home/root/.config/sway/config.new",
        "/home/root/.config/sway/config");
```

The swap is a single change to a directory, so a machine that stops in the
middle — out of disk, powered off — has either the whole old file or the whole
new one. Writing over the original instead has a moment where it is neither,
and a configuration lost that way is lost for good. `swaysettings` saves this
way.

**Nothing is copied**: a directory entry is a name and an inode number, and
only the entry moves, so renaming a large file costs the same as renaming an
empty one. An existing destination is replaced when it is a file — that is what makes
the swap atomic — and refused when it is a directory, rather than discarding
whatever is inside it. A directory cannot be moved inside itself.

Both ends must be on **one filesystem**. Moving between the disk and
`/ramdisk` would be a copy and a delete rather than one step, and a rename that
silently copied a gigabyte would not be what anybody meant by rename;
[`mv`](apps.md#mv) says so rather than doing it.

**Returns** 0, `-W_ENOENT`, `-W_EXDEV` across two filesystems, `-W_EISDIR` if
the destination is a directory, `-W_EINVAL` for a directory moved inside
itself, or `-W_EACCES`.

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

Memory and CPU use of one process. Pass `0` for the calling process.

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
    uint32_t cpu_ticks;       /* timer ticks this process has run for   */
} wprocmem_t;
```

`resident_bytes` is counted from the process's page tables — what it actually
occupies, including its page tables themselves, not a reservation.

`cpu_ticks` is a running total since the process started, at the timer's 100
ticks a second, and it counts every thread the process has had rather than
only the ones it still has. It is not a rate: to show a load, read it twice
and divide the difference by the [`wticks()`](#unsigned-int-wticksvoid) that
passed in between.

```c
wprocmem_t before, after;
unsigned   start = wticks();

wprocmem(pid, &before);
wsleep(1000);
wprocmem(pid, &after);

unsigned tenths = (after.cpu_ticks - before.cpu_ticks) * 1000
                / (wticks() - start);
wprintf("%u.%u%%\n", tenths / 10, tenths % 10);
```

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

# Services

A service is a program the system runs rather than a person: described by a
file in `/services`, started by the kernel at boot if it is enabled, and
running with no parent so that closing a shell does not take it down. See
[`systemctl`](apps.md#systemctl).

## `int wservicelist(wservice_t *out, int max)`

```c
typedef struct {
    char     name[28];         /* what it is called, and its file name */
    char     exec[96];         /* the program that is run              */
    char     description[64];
    uint32_t enabled;          /* 1 if it starts at boot               */
    uint32_t running;          /* 1 if it is running now               */
    int32_t  pid;              /* which process, or 0                  */
    int32_t  exit_status;      /* how it last finished, if it has      */
} wservice_t;
```

The list comes from the unit files, read at boot; `running` and `pid` come from
the process table, so a service whose process has died is reported stopped
rather than running. Reading needs no permission. `max` is the size of the
array; `W_SERVICE_MAX` is always enough.

**Returns** how many were written, or `-W_EFAULT`.

## `int wservicectl(int action, const char *name)`

`action` is one of `W_SVC_START`, `W_SVC_STOP`, `W_SVC_RESTART`,
`W_SVC_ENABLE` or `W_SVC_DISABLE`.

Enabling does not start and starting does not enable: the two answer different
questions. Every action needs root or the `systemctleditor` role, because each
changes what the machine is running for everybody on it.

A stop asks the process to leave rather than tearing it down where it stands,
and waits up to two seconds for it to go — long enough that a restart does not
race the thing it just stopped. A process that never reaches a safe moment is
still reported running afterwards, because it is.

**Returns** 0, `-W_EPERM` without the role, `-W_ENOENT` if there is no such
service or its program is missing, `-W_EBUSY` when starting one already running
or stopping one that is not, or an error from spawning it.

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

The socket **belongs to the user who listened on it**, which is what decides
who may connect: see `wconnect()`.

**Returns** a descriptor to accept connections on, `-W_EEXIST` if the name is
taken, `-W_EACCES`, `-W_ENFILE` or `-W_EMFILE`.

## `int wconnect(const char *path)`

Connect to whoever is listening on `path`. Returns as soon as the connection is
queued rather than waiting to be accepted, so a client may start sending
immediately.

**A socket belongs to whoever is listening on it, and another user cannot
connect.** Talking to a socket is talking to the process behind it, and what
that process does with what it hears is its business: a compositor's socket
takes commands and runs programs as the user whose session it is. So the check
is here, where every connection goes past, rather than in each program that
listens. Root is not stopped, because root can already become anybody.

**Returns** a connected descriptor, `-W_ENOENT` if nothing is listening,
`-W_EPERM` if the socket belongs to another user, `-W_EBUSY` if the backlog is
full, or `-W_EMFILE`.

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

`charge_percent` is a percentage of what the pack holds **when full now**, not
of what it held when new — a battery that has aged reports how much of itself
is left, rather than how much of the battery it used to be. It comes from
`_BST`, an ACPI method that reads the embedded controller, which the kernel
runs; `state` and `ac_online` come from the same place and from `_PSR`.

It is -1 when there is nothing to read: no battery, no `_BST` declared for it,
or a `_BST` that used something the interpreter does not implement and stopped.
Reported as unknown rather than guessed at, in every case. Nothing is cached,
so two calls a minute apart give two readings. See
[`battery`](apps.md#battery).

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

## `int wreap(int *status)`

Reap a child that has *already* exited, without waiting for one that has not.

`wwait()` blocks, which is right for a shell running a command and wrong for
anything that has other work to do. A compositor that started a program on a
keybinding cannot stop serving its clients until that program happens to
finish — and without a call like this, every program it ever started would stay
in the process table as a zombie.

There is no cost to calling it when nothing has exited, so the usual shape is
to call it once each time round an event loop.

**Returns** the pid reaped, or `-W_ECHILD` when nothing has exited — which
includes having no children at all.

```c
while (wreap(NULL) >= 0)
    ;                       /* collect whatever finished */
```

## `int wkill(int pid)`

Ask another process to stop. This is the `kill(pid, SIGTERM)` of a system that
has neither signals nor a number to ask with: there is one thing to ask for.

It does not stop anything dead. The kernel cannot unwind another process's
kernel stack from a distance, so the process is marked and woken, and it leaves
at the next point where it holds nothing — returning from a blocking wait,
entering a syscall, or being interrupted while in user code. That is prompt for
anything that waits and prompt for anything that computes, which between them
is everything; but the call returns before the process is gone, so a caller
that needs it *gone* watches for that with `wproclist()`.

What it leaves behind is what an exit leaves behind: descriptors closed, shared
memory unmapped, the screen handed back if it had it, and a zombie until its
parent reaps it. The exit status is -1.

Root may stop anything. Anybody else may stop only processes running as them,
so that the process table is not a way to end another user's session — a shell
is a process like any other, and every service on the machine belongs to root.

**Returns** 0, or `-W_ESRCH` (no such process, or it has already exited), or
`-W_EPERM` (it belongs to somebody else).

```c
if (wkill(pid) == -W_EPERM)
    wprintf("pid %d is not yours to stop\n", pid);
```

[`htop`](apps.md#htop) is what this is for: F9 on a row asks, and yes stops it.

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
| `W_KEY_F1` … `W_KEY_F12` | the function keys |
| `W_KEY_ESCAPE` | a bare Escape |

Escape both introduces sequences and is a key in its own right; they are told
apart by whether anything follows immediately, which is the same guess a
terminal program makes.

The function keys are sent as the sequences a terminal sends — `ESC O P` for
F1 to F4, and the numbered `ESC [ 15 ~` form from F5 up, with the gaps at 16
and 22 that the VT220 left and nothing since has filled in. So a program
decodes them identically whether the keys came from this console or down the
serial port. Like the arrows, they exist only in raw mode: in canonical mode
the driver is assembling a line, and F5 has no meaning within one.

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


---

# Shared memory

Pages two processes can both see. A socket copies what it carries, which is
right for messages and wrong for a screenful of pixels: a 640x400 window is a
megabyte, and a client redrawing it sixty times a second would push sixty
megabytes a second through a 4 KiB buffer for the compositor to copy a second
time.

So the pixels do not move. One process asks for an object and gets a
descriptor; either side maps it and gets a pointer; the descriptor travels over
a socket like any other. That is exactly what `wl_shm` is, and it is why the
socket had to be able to carry descriptors before any of this could work.

The object lives until every descriptor naming it is closed **and** every
process that mapped it has unmapped or exited — so a client may draw a buffer,
hand it over and exit, leaving the compositor holding pixels that are still
perfectly good.

## `int wshmopen(unsigned int bytes)`

Create an object of `bytes`, rounded up to a whole page and zeroed. The size is
fixed at creation: there is no growing one, and a program that needs more room
makes another.

It cannot be read or written. `wread()` on one fails with `-W_EINVAL` rather
than pretending to be a file — shared memory is reached by mapping it.

**Returns** a descriptor, `-W_EINVAL` for zero or more than `W_SHM_MAX_BYTES`,
`-W_ENOMEM`, or `-W_EMFILE`.

## `void *wshmmap(int fd)`

Map the whole object and return a pointer to it, readable and writable. Mapping
the same descriptor twice gives two addresses for the same pages, which is
harmless.

**Returns** the pointer, or NULL if `fd` is not shared memory or there is no
room to map it.

## `int wshmunmap(void *addr)`

Release a mapping. `addr` must be exactly what `wshmmap()` returned. The pages
survive if anything else still names the object.

**Returns** 0, or `-W_EINVAL`.

## `int wshmsize(int fd)`

**Returns** the size in bytes, or `-W_EBADF`.

The receiver of a descriptor needs this. It was told a width and a height by
whatever protocol handed the buffer over; this is how it checks that the memory
it was actually given is big enough to hold them, rather than trusting the
sender's arithmetic. The size is the one fact about a buffer a sender cannot
misreport.

```c
int   fd   = wshmopen(640 * 400 * 4);
void *pool = wshmmap(fd);

draw_into(pool);

int    fds[1] = { fd };
wmsg_t msg    = { "here", 4, 1, fds };
wsend(socket, &msg);        /* the descriptor travels; the pixels do not */
```

---

# The screen

For a program that draws pixels rather than characters. The text console has
the framebuffer until something takes it.

While it is lent out the console does not stop — it keeps tracking the cursor,
keeps scrolling, keeps recording everything printed — it only stops drawing.
When the screen comes back everything written meanwhile is repainted, so a
program that printed from behind a compositor has not lost a line of it.

## `int wdisplayinfo(wdisplay_t *out)`

Describe the screen: `width`, `height`, `stride` in bytes, `bpp` (always 32),
and `owner`, the pid holding it or 0 for the console.

`present` is 0 on a machine whose console is still VGA text mode, where there
is no framebuffer at all. A program that draws should check it and say so
rather than blitting into nothing.

**Returns** 0, or `-W_EFAULT`. Every field is zeroed first.

## `int wdisplaygrab(void)`

Take the screen from the console.

There is one screen, so taking it affects every process on the machine. That
makes it root's to do, in the same way setting the clock is — or a session's,
when root started it and handed it a seat with
[`wseatgrant()`](#int-wseatgrantvoid).

**Returns** 0, `-W_EPERM` without root or a seat, `-W_ENODEV` on a machine with
no framebuffer, or `-W_EBUSY` if another process already has it.

**The screen is released automatically when the holder exits, however it
exits.** A compositor that faults does not take the machine's only output with
it and leave it running with no way to say so.

## `int wdisplaydrop(void)`

Give it back. The console repaints. **Returns** 0.

## `int wdisplayblit(const wblit_t *b)`

Put a rectangle of pixels on the screen. Pixels are `0x00RRGGBB`, one per
32-bit word, and `stride` is the source's row length **in pixels** — so a
rectangle can be blitted straight out of a larger back buffer without being
copied out of it first.

The rectangle is clipped to the screen rather than trusted, so a coordinate off
the edge draws less rather than writing somewhere it should not.

**Returns** 0, `-W_EPERM` if this process does not hold the screen, `-W_EINVAL`
for a rectangle wider than its own stride, or `-W_EFAULT`.

```c
wdisplay_t screen;
wdisplayinfo(&screen);
if (!screen.present || wdisplaygrab() < 0)
    return 1;

uint32_t *back = malloc(screen.width * screen.height * 4);
/* ... compose the whole frame into `back` ... */

wblit_t b = { back, screen.width, 0, 0,
              (int)screen.width, (int)screen.height };
wdisplayblit(&b);

wdisplaydrop();
```

---

# Raw input

## `int winputopen(void)`

Open the keyboard as a stream of key transitions.

The console turns keystrokes into lines of text, which is what a shell wants
and the opposite of what a compositor wants. A compositor has to know that a
key was *released*, and which physical key it was regardless of the character
it would print, so that it can tell one of its own bindings from something to
forward to a window.

Read the descriptor for whole `winput_t` records — a short read is never half
an event — and wait on it with `wpoll()` alongside everything else.

**While it is open the console reads nothing at all.** That is not a limitation
being worked around: there is one keyboard, and the holder has it. Closing the
descriptor, or exiting, gives it straight back.

**Returns** a descriptor, `-W_EPERM` without root or a seat, or `-W_EBUSY` if
another process already holds the keyboard.

## `int wpointer(wpointer_t *out)`

Where the pointer is, and whether the machine has one.

```c
typedef struct {
    uint32_t present;    /* 0 when the machine has no pointing device */
    int32_t  x, y;       /* where it is, in pixels                    */
} wpointer_t;
```

A compositor asks this before it advertises a seat. Telling clients there is a
pointer when there is none leaves them waiting for motion that will never come,
and a cursor drawn on a machine with no mouse is a cursor nobody can move. It
is also where the first cursor position comes from, so the arrow starts
wherever the kernel has been keeping it rather than in a corner.

The position is clamped to the screen by the kernel, which is the only place
that knows how big the screen is — a pointer that could leave it is one nobody
could bring back.

**Returns** 0, or `-W_EFAULT`.

## `int wpointerspeed(int percent)`

How far the pointer moves for how far the mouse does, as a percentage.

```c
#define W_POINTER_SPEED_MIN     10
#define W_POINTER_SPEED_DEFAULT 100
#define W_POINTER_SPEED_MAX     800
```

100 is one count from the mouse to one pixel on the screen, 50 is half as fast
and 200 twice. A value outside the range is clamped rather than refused, and a
negative one reads the speed in force without changing it — a compositor that
had to set the speed to find out what it was could not report the setting it
found.

It is a plain multiplier and nothing else: the same movement always moves the
pointer the same distance, however quickly the hand made it. **Acceleration —
further for a fast movement than for a slow one of the same length — needs the
interval between packets to mean something**, and on a PS/2 mouse sampled at
whatever rate the firmware left it that interval is not a speed.

The kernel applies it, in the same place it turns counts into a position, and
it keeps what the rounding left over so a slow setting is slow rather than
sticky: at 50%, a one-count movement is half a pixel, and a driver that rounded
each packet on its own would throw both halves away.

The speed is the machine's, not the process's — it stays as it was set after
the program that set it exits, which is why [`sway`](apps.md#sway) puts the
default back when it starts.

**Returns** the speed now in force, `-W_EPERM` without root or the seat, or
`-W_ENODEV` on a machine with no pointing device.

## `int wseatgrant(void)`

Arm a seat grant: let the next process this one spawns take the screen and the
keyboard, whoever it runs as.

The **seat** is both devices together. A session with a screen and no keys is
not one anybody could use, so `wdisplaygrab()` and `winputopen()` accept the
same grant and there is no way to hand over half of it.

This exists so a login manager can be written, and its shape is that job's
shape. Such a program starts as root, checks a password, becomes the user who
gave it, and starts their session — but a process that drops to a user can
never climb back, so by the time it has somebody to hand the seat to it is no
longer anybody who could grant one.

```c
wseatgrant();                            /* while still root       */
wlogin(name, password);                  /* now uid 1, no way back */
wspawn("/app/sway/launch", argv);        /* takes the seat with it */
```

Arming before the password is checked reads oddly, and is the point: succeeding
at the check is what stops the program being able to arm. It is not a hole —
only root may arm, the grant is spent by **one** spawn, and it does not descend
past that process. The session leader holds the seat; the terminals and editors
it goes on to start are ordinary processes that cannot take the display from
the compositor drawing them.

**Returns** 0, or `-W_EPERM` for anyone but root.

See [the seat](users.md#the-seat-and-the-one-way-past-that-rule) and
[`login`](apps.md#login).

```c
typedef struct {
    uint32_t type;       /* W_INPUT_*                               */
    uint32_t code;       /* evdev key code, W_BTN_*, or the axis     */
    uint32_t state;      /* 1 pressed, 0 released                   */
    uint32_t mods;       /* W_MOD_* held, as of after this event    */
    uint32_t unicode;    /* the character it would make, or 0       */
    uint32_t time_ms;    /* milliseconds since boot                 */
    int32_t  x, y;       /* where the pointer is, in pixels         */
    int32_t  dx, dy;     /* how far it moved; dy is wheel notches,
                          * counting down when the wheel is turned
                          * away from the user -- the sign the mouse
                          * reports and the one wl_pointer.axis wants */
} winput_t;
```

| `type` | What it is | Which fields mean anything |
|---|---|---|
| `W_INPUT_KEY` | a key went down or came up | `code`, `state`, `unicode` |
| `W_INPUT_POINTER_MOTION` | the pointer moved | `x`, `y`, `dx`, `dy` |
| `W_INPUT_POINTER_BUTTON` | a button went down or came up | `code` (`W_BTN_LEFT`, `W_BTN_RIGHT`, `W_BTN_MIDDLE`), `state`, `x`, `y` |
| `W_INPUT_POINTER_AXIS` | the wheel turned | `code` (the axis), `dy` (steps) |

`mods`, `time_ms`, `x` and `y` are filled in on **every** event, whatever its
type. A key event carrying the pointer position means a program that wants to
know where the mouse was when a key was pressed does not have to track it.

**Both devices arrive in one stream.** That is deliberate: a compositor needs
them interleaved in the order they happened, because a click that arrived
before the motion that led to it would land on whatever used to be under the
pointer. Two streams could only promise that by being merged on timestamps at
the far end.

The pointer position is **absolute and already clamped to the screen**, because
only the kernel knows how big the screen is — and a pointer that could leave it
is one nobody could bring back. The delta is there as well, for a client that
wants a distance rather than a place.

Key codes are the Linux **evdev** codes, which for the main block of a PS/2
keyboard are the AT set 1 scancodes they were originally defined from. They are
what `wl_keyboard.key` carries and what an XKB keymap is written against, so a
client that knows Wayland already knows these numbers. The modifier mask is in
XKB's own bit positions, so it is what `wl_keyboard.modifiers` carries without
translation.

`unicode` is a convenience, so a program that only wants text need not carry a
keymap; a compositor that does carry one ignores it and uses `code`.

---

# Keys, and drawing text

What xkbcommon and a font server answer on Linux, for a system that has
neither.

## `const unsigned char *wfont8x16(void)`
## `const unsigned char *wglyph8x16(unsigned int c)`

The console's font: 256 glyphs of 16 rows, one byte per row, most significant
bit leftmost. `wglyph8x16(c)` is the 16 bytes for one character.

A program that owns the framebuffer draws its own characters, and there is no
font on the screen to borrow — the console's glyphs live in the kernel. Having
the same font in both places is what makes a window and the console beneath it
look like one machine.

```c
const unsigned char *glyph = wglyph8x16('A');
for (int row = 0; row < 16; row++)
    for (int col = 0; col < 8; col++)
        if (glyph[row] & (0x80 >> col))
            put_pixel(x + col, y + row, colour);
```

## `uint32_t wkeycode_from_name(const char *name)`

The evdev code a name refers to: `"Return"` → 28, `"q"` → 16, `"Left"` → 105.
Case-insensitive. The names are the X11 keysym names a sway configuration file
is written in, so `bindsym $mod+Shift+Q` means here what it means there — a
single shifted character names the key that prints it.

**Returns** the code, or 0 if the name is not a key.

## `const char *wkeyname(uint32_t keycode)`

What a key is called, or NULL.

## `uint32_t wkeychar(uint32_t keycode, uint32_t mods)`

The character a key produces with those modifiers held, or 0 for a key that
prints nothing. Shift and Caps Lock behave as a keyboard does, and Ctrl turns a
letter into its control code, so Ctrl+C arrives as `0x03`.

This is what a Wayland client on WOS uses in place of xkbcommon: the compositor
sends `wl_keyboard.keymap` with format `no_keymap` and the evdev codes
directly, and this turns one into a character.

## `uint32_t wmodifier_from_name(const char *name)`

The `W_MOD_*` bit a name refers to: `"Shift"`, `"Ctrl"`, `"Alt"`, `"Mod1"`,
`"Mod4"`, `"Super"`. **Returns** the bit, or 0.

---

# Wayland

WOS carries a libwayland-shaped protocol library, in both halves. It is not
described here — it is large, and it is documented where it is declared:

- `<wayland-client.h>` — `wl_display_connect()`, `wl_display_roundtrip()`,
  `wl_registry_add_listener()`, `wl_proxy_marshal()` and the typed calls
  generated from the protocol.
- `<wayland-server.h>` — `wl_display_create()`, `wl_global_create()`,
  `wl_resource_create()`, `wl_resource_post_event()`.
- `<wayland-protocol.h>` — the interfaces, their opcodes and their enumerations,
  transcribed from `wayland.xml` and `xdg-shell.xml`.

The names, shapes and argument orders are libwayland's, so a client written for
a Linux desktop is written against these. Two things upstream has that this has
not: there is no `wl_event_queue`, because that exists to let several threads
dispatch one connection and a WOS process has one thread; and `wl_fixed_t` has
no floating-point conversions, because the kernel never turns the FPU on.
Neither changes a byte on the wire.

[`app/wlprobe`](../app/wlprobe/sourcecode/wlprobe.c) is the smallest complete
client to read; [`app/wlterm`](../app/wlterm/sourcecode/wlterm.c) is a real one.
