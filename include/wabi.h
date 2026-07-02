/* The WOS kernel/application contract.
 *
 * Everything in this file is shared verbatim between the kernel and user
 * space: syscall numbers, error codes, flags and the structures passed across
 * the boundary.  Keeping it in one place means the two sides cannot drift.
 *
 * Applications do not include this header directly -- they include
 * <wkernel.h>, which pulls this in and documents every call.
 */
#ifndef WOS_WABI_H
#define WOS_WABI_H

#ifdef WOS_KERNEL
#include "types.h"
#else
#include <stdint.h>
#endif

/* ------------------------------------------------------------------ *
 *  Error codes
 *
 *  Syscalls report failure by returning the negated code, so a caller
 *  checks `if (r < 0)` and reads the reason from `-r`.  The numbers match
 *  the traditional Linux values to keep them unsurprising.
 * ------------------------------------------------------------------ */
#define W_EPERM          1   /* operation not permitted            */
#define W_ENOENT         2   /* no such file or directory          */
#define W_ESRCH          3   /* no such process                    */
#define W_EIO            5   /* disk or device error               */
#define W_ENODEV        19   /* no such device (e.g. no network card) */
#define W_E2BIG          7   /* argument list too long             */
#define W_ENOEXEC        8   /* not a valid executable             */
#define W_EBADF          9   /* bad file descriptor                */
#define W_ECHILD        10   /* no such child process              */
#define W_ENOMEM        12   /* out of memory                      */
#define W_EACCES        13   /* permission denied                  */
#define W_EFAULT        14   /* bad address passed from user space */
#define W_EBUSY         16   /* resource busy                      */
#define W_EEXIST        17   /* already exists                     */
#define W_ENOTDIR       20   /* a path component is not a directory */
#define W_EISDIR        21   /* is a directory                     */
#define W_EINVAL        22   /* invalid argument                   */
#define W_ENFILE        23   /* system file table full             */
#define W_EMFILE        24   /* too many open files in this process */
#define W_ENOSPC        28   /* no space left on the disk          */
#define W_ESPIPE        29   /* seek on a stream that cannot seek  */
#define W_EPIPE         32   /* write to a pipe with no reader     */
#define W_ERANGE        34   /* result does not fit in the buffer  */
#define W_ENAMETOOLONG  36   /* path or name too long              */
#define W_ENOSYS        38   /* no such syscall                    */
#define W_ENOTEMPTY     39   /* directory not empty                */
#define W_EFBIG         27   /* file too large for this filesystem */
#define W_ECONNRESET   104   /* connection reset by the peer       */
#define W_ETIMEDOUT    110   /* operation timed out                */
#define W_ECONNREFUSED 111   /* connection refused                 */
#define W_EHOSTUNREACH 113   /* no route/ARP to the host           */

/* ------------------------------------------------------------------ *
 *  Files and directories
 * ------------------------------------------------------------------ */

/* File types, as reported in wstat_t.type and wdirent_t.type. */
#define W_FT_FILE 1
#define W_FT_DIR  2

/* Flags for open(). Exactly one access mode, ORed with any of the rest. */
#define W_O_RDONLY 0x0000
#define W_O_WRONLY 0x0001
#define W_O_RDWR   0x0002
#define W_O_ACCMODE 0x0003
#define W_O_CREAT  0x0100   /* create the file if it does not exist  */
#define W_O_TRUNC  0x0200   /* truncate to zero length when opening  */
#define W_O_APPEND 0x0400   /* every write goes to the end           */

/* `whence` values for lseek(). */
#define W_SEEK_SET 0
#define W_SEEK_CUR 1
#define W_SEEK_END 2

/* Standard file descriptors, open in every process at startup. */
#define W_STDIN  0
#define W_STDOUT 1
#define W_STDERR 2

/* Longest single name component, and longest full path. */
#define W_NAME_MAX 27
#define W_PATH_MAX 255

typedef struct {
    uint32_t ino;        /* inode number, unique within the filesystem */
    uint32_t size;       /* size in bytes                              */
    uint32_t blocks;     /* disk blocks the file occupies              */
    uint32_t type;       /* W_FT_FILE or W_FT_DIR                      */
} wstat_t;

typedef struct {
    uint32_t ino;
    uint32_t type;                    /* W_FT_FILE or W_FT_DIR */
    char     name[W_NAME_MAX + 1];
} wdirent_t;

/* ------------------------------------------------------------------ *
 *  Memory and disk statistics
 * ------------------------------------------------------------------ */

/* 64-bit, because a PC puts most of its memory above the 4 GiB line and a
 * 32-bit count of bytes cannot reach it: a machine with 8 GiB would report 3.
 * The disk figures are the same size for the same reason. */
typedef struct {
    uint64_t total_bytes;    /* usable RAM the machine reported          */
    uint64_t used_bytes;     /* frames currently allocated               */
    uint64_t free_bytes;     /* total_bytes - used_bytes                 */
    uint64_t kernel_bytes;   /* kernel image, heap arena and page tables */
    uint32_t page_size;      /* always 4096                              */
    uint32_t pad;
} wmeminfo_t;

typedef struct {
    int32_t  pid;
    char     name[32];
    uint32_t resident_bytes; /* frames mapped in this address space   */
    uint32_t code_bytes;     /* loaded program text and read-only data */
    uint32_t data_bytes;     /* initialised data and bss               */
    uint32_t heap_bytes;     /* grown through wsbrk()                  */
    uint32_t stack_bytes;    /* user stack                             */
    int32_t  thread_count;
} wprocmem_t;

typedef struct {
    int32_t  tid;
    int32_t  pid;
    uint32_t kernel_stack_bytes;
    uint32_t user_stack_bytes;
    uint32_t cpu_ticks;      /* timer ticks this thread has run for */
} wthreadmem_t;

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
} wdiskinfo_t;

/* One mounted filesystem.  wdiskinfo() reports the disk the system is on;
 * this is every one of them, which on a running WOS means that disk and the
 * one in memory over /ramdisk. */
#define W_DISK_MAX 8

typedef struct {
    char        mount[28];    /* where it appears: "/" or "/ramdisk"      */
    char        device[24];   /* what it is: "ATA disk", "memory", ...    */
    uint32_t    persistent;   /* 0 when what is written is gone at reboot */
    uint32_t    pad;
    wdiskinfo_t usage;
} wdisk_t;

/* ------------------------------------------------------------------ *
 *  Processors
 *
 *  One wcpu_t per logical processor the machine reported, whether or not the
 *  kernel is running anything on it.  WOS starts only the processor it booted
 *  on, so the rest are listed with `online` clear and nothing to say about
 *  them: a reading has to be taken by the core it describes, and no code runs
 *  there to take it.
 * ------------------------------------------------------------------ */

#define W_CPU_MAX 64        /* longest processor list the kernel will report */

/* Where a clock reading came from, so a program can say what it is showing
 * rather than implying a precision it has not got. */
#define W_CLOCK_NONE  0     /* nothing could be measured                     */
#define W_CLOCK_APERF 1     /* measured over the last tick: the real clock   */
#define W_CLOCK_TSC   2     /* the timestamp counter's rate: the base clock, */
                            /* which is what the core runs at only when it   */
                            /* is neither throttled nor in turbo             */
#define W_CLOCK_CPUID 3     /* the base clock CPUID quoted, never measured   */

/* temp_c when the machine has no thermal sensor this core can read. */
#define W_TEMP_UNKNOWN (-1000)

typedef struct {
    int32_t  id;             /* 0-based index, and the position in the list */
    uint32_t apic_id;        /* what the firmware calls it                  */
    uint32_t online;         /* 1 if the kernel executes on this core       */
    uint32_t clock_khz;      /* current clock, 0 when unknown               */
    uint32_t clock_source;   /* W_CLOCK_*: how clock_khz was arrived at     */
    int32_t  temp_c;         /* degrees Celsius, or W_TEMP_UNKNOWN          */
    int32_t  temp_max_c;     /* the temperature the CPU throttles itself at */
    uint32_t busy_ticks;     /* timer ticks spent running something         */
    uint32_t idle_ticks;     /* timer ticks spent with nothing to run       */
} wcpu_t;

/* Both tick counts are cumulative since boot and wrap the way the timer does;
 * a program shows a load by taking two samples and dividing the difference in
 * busy_ticks by the difference in both.  A single sample is the average since
 * boot, which is a different and much less interesting number. */

typedef struct {
    int32_t  count;          /* logical processors the machine has     */
    int32_t  online;         /* how many of them the kernel runs on    */
    uint32_t tick_hz;        /* rate the tick counts above advance at  */
    uint32_t base_khz;       /* the clock the part is specified at     */
    uint32_t min_khz;        /* slowest the machine says it will go, 0 */
    uint32_t max_khz;        /* fastest, including turbo, 0 if unknown */
    uint32_t settable;       /* 1 if the clock can be asked to change  */
    uint32_t step_khz;       /* the smallest step it moves in, 0 if it
                              * cannot be set at all                   */
    uint32_t pinned_khz;     /* what it was last pinned to, 0 for the
                              * hardware's own choice                  */
    uint32_t pad;
    char     brand[52];      /* what the CPU calls itself, or empty    */
} wcpuinfo_t;

/* ------------------------------------------------------------------ *
 *  Services
 *
 *  A service is a program the system runs rather than a person does: it is
 *  described by a file in /services, may be enabled so that it starts at
 *  boot, and may be started and stopped while the machine is up.
 *
 *  The description lives on the disk and the running state lives in the
 *  kernel, which is the only place that knows whether a process is still
 *  there.  Enabling a service does not start it and starting one does not
 *  enable it -- the two questions are "should this run at the next boot" and
 *  "is it running now", and answering one with the other is how a machine
 *  ends up in a state nobody asked for.
 * ------------------------------------------------------------------ */

#define W_SERVICE_MAX 16          /* services the kernel will track */

/* What to do to one.  Every action needs root or W_ROLE_SYSCTLEDIT. */
#define W_SVC_START   0
#define W_SVC_STOP    1
#define W_SVC_RESTART 2
#define W_SVC_ENABLE  3           /* start at boot from now on */
#define W_SVC_DISABLE 4           /* do not                    */

typedef struct {
    char     name[28];            /* what it is called, and its file name  */
    char     exec[96];            /* the program that is run               */
    char     description[64];
    uint32_t enabled;             /* 1 if it starts at boot                */
    uint32_t running;             /* 1 if it is running now                */
    int32_t  pid;                 /* which process, or 0                   */
    int32_t  exit_status;         /* how it last finished, if it has       */
} wservice_t;

/* ------------------------------------------------------------------ *
 *  Local sockets
 *
 *  A connection-oriented byte stream between two processes, named by a path
 *  in the filesystem, carrying file descriptors alongside the bytes.  That is
 *  the shape of a Unix domain socket, and it is the shape because it is what
 *  a display protocol needs: a client connects to a compositor by name, and
 *  hands it a descriptor for a buffer of pixels rather than a copy of them.
 *
 *  The name is a path but not a file.  Nothing is created on the disk; the
 *  path is the address a listener answers to, and it disappears when the
 *  listener closes.
 * ------------------------------------------------------------------ */

/* Most descriptors one call can carry.  The same number libwayland uses, and
 * for the same reason: it bounds the queue a peer can make you hold. */
#define W_SEND_MAX_FDS 28

/* How much a positive W_POLLOUT promises room for.
 *
 * "Writing would not block" is only true of a write small enough to fit in
 * what is free, so the promise has to name a size.  A protocol writes whole
 * messages: without a figure here, two programs that each poll before writing
 * could still both block on each other, each having been told to go ahead.
 *
 * A writer that flushes no more than this after a positive poll cannot
 * block. */
#define W_SEND_CHUNK 1024

/* One message: bytes, and the descriptors travelling with them.
 *
 * On wsend(), `fds` names `fd_count` open descriptors to pass; the receiver
 * gets copies of them in its own table, and closing yours afterwards does not
 * close its.  On wrecv(), `fds` is where up to `fd_count` arriving descriptors
 * are written, and `fd_count` is updated to how many actually arrived.
 *
 * A descriptor is delivered no earlier than the byte it was sent with, so a
 * receiver that has read the message describing a buffer is holding the
 * buffer's descriptor by then, and never before. */
typedef struct {
    void    *buf;
    uint32_t len;
    int32_t  fd_count;
    int32_t *fds;
} wmsg_t;

/* What a descriptor is waited on for, and what happened. */
#define W_POLLIN   0x0001   /* reading would not block                     */
#define W_POLLOUT  0x0002   /* a write of up to W_SEND_CHUNK would not block */
#define W_POLLHUP  0x0004   /* the other end has gone (always reported)    */
#define W_POLLERR  0x0008   /* the descriptor is not one that can be waited
                             * on (always reported)                        */

typedef struct {
    int32_t fd;
    int16_t events;         /* what to wait for: W_POLLIN | W_POLLOUT */
    int16_t revents;        /* what is true now                       */
} wpollfd_t;

#define W_POLL_MAX 32       /* most descriptors one wpoll() can watch */

/* ------------------------------------------------------------------ *
 *  Shared memory
 *
 *  Pages two processes can both see.  One asks for an object and gets a
 *  descriptor; either side maps it and gets a pointer; the descriptor travels
 *  over a socket like any other.  That is how a window's pixels reach the
 *  compositor without being copied -- the client draws into the mapping and
 *  sends the descriptor, not the megabyte.
 *
 *  The object is a fixed size, fixed when it is created.  It exists as long as
 *  any descriptor names it or any process has it mapped, so a client may
 *  create a buffer, hand it over and exit without the pixels going away.
 * ------------------------------------------------------------------ */

/* Largest object wshmopen() will create: two screens' worth at 1920x1200. */
#define W_SHM_MAX_BYTES (32u * 1024u * 1024u)

/* ------------------------------------------------------------------ *
 *  The screen
 *
 *  The framebuffer console owns the display until something asks for it.  A
 *  compositor asks: it draws windows, and windows and a text console cannot
 *  share a screen.
 *
 *  While it is lent out the console keeps running and stops drawing, so
 *  everything printed behind the compositor is still there when the screen
 *  comes back.  It comes back when the holder gives it up or exits -- the
 *  second being the one that matters, because a compositor that crashes must
 *  not take the machine's only output with it.
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t present;   /* 0 when there is no framebuffer: VGA text mode */
    uint32_t width;     /* pixels                                        */
    uint32_t height;
    uint32_t stride;    /* bytes per row, which is not always width * 4  */
    uint32_t bpp;       /* always 32: 0x00RRGGBB, one pixel per uint32_t */
    uint32_t owner;     /* pid holding the screen, 0 for the console     */
} wdisplay_t;

/* One rectangle to put on the screen.  `pixels` is 0x00RRGGBB, `stride` is the
 * source's row length in *pixels*, so a compositor can send part of a larger
 * image without copying it out first. */
typedef struct {
    const void *pixels;
    uint32_t    stride;
    int32_t     x, y;
    int32_t     width, height;
} wblit_t;

/* ------------------------------------------------------------------ *
 *  Raw input
 *
 *  The console turns keystrokes into lines of text, which is what a shell
 *  wants and the opposite of what a compositor wants.  A compositor needs to
 *  know that a key went down and later came up, which key it was regardless of
 *  what character it would produce, and what modifiers were held -- because it
 *  has to decide whether the keystroke is a binding of its own or something to
 *  forward to whichever window has the focus.
 *
 *  So there is a second way to read the keyboard.  While it is open the line
 *  discipline is bypassed entirely: nothing is echoed, nothing is buffered
 *  into lines, and the console gets no input at all.  That is not a
 *  shortcoming -- the keyboard, like the screen, is one device, and the
 *  compositor has it.
 *
 *  Key codes are the Linux evdev codes, which for the main block of a PS/2
 *  keyboard are the AT set 1 scancodes they were originally defined from.
 *  They are what wl_keyboard.key carries and what an XKB keymap is written
 *  against, so a client that knows Wayland already knows these numbers.
 * ------------------------------------------------------------------ */

#define W_INPUT_KEY 1        /* a key went down or came up */

/* Modifiers, in the bit positions XKB gives them -- so this value is the one
 * wl_keyboard.modifiers carries, without translation. */
#define W_MOD_SHIFT (1u << 0)
#define W_MOD_CAPS  (1u << 1)
#define W_MOD_CTRL  (1u << 2)
#define W_MOD_ALT   (1u << 3)   /* Mod1 */
#define W_MOD_LOGO  (1u << 6)   /* Mod4: the Super key, sway's default $mod */

typedef struct {
    uint32_t type;       /* W_INPUT_KEY                                   */
    uint32_t code;       /* evdev key code                                */
    uint32_t state;      /* 1 pressed, 0 released                         */
    uint32_t mods;       /* W_MOD_* held, as of after this event          */
    uint32_t unicode;    /* the character it would produce, or 0 for none
                          * -- a convenience, so a program that only wants
                          * text need not carry a keymap of its own       */
    uint32_t time_ms;    /* milliseconds since boot, for wl_keyboard.key  */
} winput_t;

/* A few evdev codes worth naming, because a compositor's default bindings are
 * written in terms of them. */
#define W_KEYCODE_ESC       1
#define W_KEYCODE_ENTER    28
#define W_KEYCODE_LEFTCTRL 29
#define W_KEYCODE_LEFTSHIFT 42
#define W_KEYCODE_LEFTALT  56
#define W_KEYCODE_LEFTMETA 125

/* ------------------------------------------------------------------ *
 *  The battery
 * ------------------------------------------------------------------ */

/* What the battery is doing.  UNKNOWN is the usual answer and not a failure:
 * see the note on `charge_percent` below. */
#define W_BATTERY_UNKNOWN     0
#define W_BATTERY_CHARGING    1
#define W_BATTERY_DISCHARGING 2
#define W_BATTERY_FULL        3

/* Chemistries SMBIOS distinguishes. */
#define W_BATTERY_CHEM_UNKNOWN  0
#define W_BATTERY_CHEM_LEAD     1
#define W_BATTERY_CHEM_NICD     2
#define W_BATTERY_CHEM_NIMH     3
#define W_BATTERY_CHEM_LION     4
#define W_BATTERY_CHEM_ZINCAIR  5
#define W_BATTERY_CHEM_LIPOLY   6

typedef struct {
    uint32_t present;        /* 1 if this machine has a battery at all      */
    uint32_t state;          /* W_BATTERY_*                                 */
    int32_t  charge_percent; /* -1 when it cannot be read -- see below      */
    int32_t  ac_online;      /* 1 on mains, 0 on battery, -1 not known      */
    uint32_t design_mwh;     /* what it holds when new, 0 unknown           */
    uint32_t design_mv;      /* nominal voltage in millivolts, 0 unknown    */
    uint32_t chemistry;      /* W_BATTERY_CHEM_*                            */
    uint32_t pad;
    char     name[32];       /* the pack's device name, or empty            */
    char     maker[32];      /* who made it, or empty                       */
    char     location[32];   /* where it sits, e.g. "Rear"                  */
} wbattery_t;

/* Why charge_percent is usually -1:
 *
 * On every laptop built this century the charge is reported by an ACPI method
 * that reads the embedded controller and returns a package.  Calling it means
 * interpreting AML bytecode, and WOS has no interpreter -- the one piece of
 * AML it reads is a constant in a fixed shape, not a program.  Everything in
 * this structure that is static is read out of the firmware's tables; the one
 * thing that changes minute to minute is the one thing that needs the
 * interpreter.  It is reported as unknown rather than guessed at.
 */

/* ------------------------------------------------------------------ *
 *  Users, roles and permissions
 *
 *  Every process runs as a user, identified by a uid.  Root is uid 0 and
 *  bypasses every check.  Any other user needs the matching role for a
 *  privileged action, and may write only inside its own home directory.
 * ------------------------------------------------------------------ */

#define W_ROOT_UID 0

#define W_NAME_LEN 31              /* longest user name */
#define W_MAX_USERS 32
#define W_SHELL_MAX 63             /* longest login-shell path */

/* Roles are a bitmask, so a user can hold several. */
#define W_ROLE_APPEDITOR  (1u << 0)  /* may write under /app                  */
#define W_ROLE_USEREDITOR (1u << 1)  /* may write /userconfig: add users, set
                                      * their passwords and change their roles.
                                      * NOT the password files themselves --
                                      * those stay root-only, for reading as
                                      * well as writing.                      */
#define W_ROLE_SYSCTLEDIT (1u << 3)  /* may start, stop, enable and disable the
                                      * system's services.  Like editfreq and
                                      * unlike the first two, this is not write
                                      * access to a place but permission to
                                      * change what the machine is running.   */
#define W_ROLE_EDITFREQ   (1u << 2)  /* may change the processor's clock.  Not
                                      * a file permission like the two above:
                                      * the clock is one setting the whole
                                      * machine shares, so changing it affects
                                      * everybody's programs and the heat the
                                      * hardware makes.                       */

typedef struct {
    uint32_t uid;
    uint32_t roles;
    char     name[W_NAME_LEN + 1];
} wuser_t;

/* Wall-clock date and time, from the CMOS real-time clock. */
typedef struct {
    int32_t year;      /* full year, e.g. 2026 */
    int32_t month;     /* 1-12                 */
    int32_t day;       /* 1-31                 */
    int32_t hour;      /* 0-23                 */
    int32_t minute;    /* 0-59                 */
    int32_t second;    /* 0-59                 */
} wtime_t;

/* Passed to wspawn_io(): the descriptors a child's stdin and stdout should be
 * wired to, and the terminal size it should report from wconsize().  `in_fd`
 * must be the read end of a pipe in the caller and `out_fd` the write end. */
typedef struct {
    int32_t in_fd;
    int32_t out_fd;
    int32_t rows;
    int32_t cols;
} wspawnio_t;

/* ------------------------------------------------------------------ *
 *  Syscall numbers
 * ------------------------------------------------------------------ */
#define WSYS_EXIT        0
#define WSYS_OPEN        1
#define WSYS_CLOSE       2
#define WSYS_READ        3
#define WSYS_WRITE       4
#define WSYS_LSEEK       5
#define WSYS_STAT        6
#define WSYS_UNLINK      7
#define WSYS_MKDIR       8
#define WSYS_RMDIR       9
#define WSYS_OPENDIR    10
#define WSYS_READDIR    11
#define WSYS_CHDIR      12
#define WSYS_GETCWD     13
#define WSYS_MEMINFO    14
#define WSYS_PROCMEM    15
#define WSYS_THREADMEM  16
#define WSYS_PROCLIST   17
#define WSYS_DISKINFO   18
#define WSYS_SPAWN      19
#define WSYS_WAIT       20
#define WSYS_GETPID     21
#define WSYS_SBRK       22
#define WSYS_TICKS      23
#define WSYS_YIELD      24
#define WSYS_SHUTDOWN   25
#define WSYS_CONSOLE    26
#define WSYS_POLLIN     27
#define WSYS_GETUID     28
#define WSYS_USERINFO   29
#define WSYS_USERLIST   30
#define WSYS_LOGIN      31
#define WSYS_PASSWD     32
#define WSYS_USERADD    33
#define WSYS_SETROLES   34
#define WSYS_PIPE       35
#define WSYS_SPAWN_IO   36
#define WSYS_CONSIZE    37
#define WSYS_SETSIZE    38
#define WSYS_SETMODE    39
#define WSYS_GETSHELL   40
#define WSYS_SETSHELL   41
#define WSYS_PING       42
#define WSYS_RESOLVE    43
#define WSYS_TCP_OPEN   44
#define WSYS_TCP_SEND   45
#define WSYS_TCP_RECV   46
#define WSYS_TCP_CLOSE  47
#define WSYS_TIME_GET   48
#define WSYS_TIME_SET   49
#define WSYS_CPUINFO    50
#define WSYS_CPULIST    51
#define WSYS_DISKLIST   52
#define WSYS_SLEEP      53
#define WSYS_CPUFREQ    54
#define WSYS_BATTERY    55
#define WSYS_LISTEN     56
#define WSYS_CONNECT    57
#define WSYS_ACCEPT     58
#define WSYS_SEND       59
#define WSYS_RECV       60
#define WSYS_POLL       61
#define WSYS_SVCLIST    62
#define WSYS_SVCCTL     63
#define WSYS_SHM_OPEN   64
#define WSYS_SHM_MAP    65
#define WSYS_SHM_UNMAP  66
#define WSYS_SHM_SIZE   67
#define WSYS_DISPINFO   68
#define WSYS_DISPGRAB   69
#define WSYS_DISPDROP   70
#define WSYS_DISPBLIT   71
#define WSYS_INPUTOPEN  72
#define WSYS_REAP       73
#define WSYS_MAX        74

/* Console modes for wconsole_raw() / WSYS_CONSOLE. */
#define W_CONSOLE_CANONICAL 0
#define W_CONSOLE_RAW       1

/* Console geometry.  The text mode can be changed at runtime with wsetmode();
 * W_CONSOLE_WIDTH/HEIGHT are the size the console boots in (80x50 -- an 8x8
 * cell, for twice the rows of plain 80x25), and a full-screen program should
 * call wconsize() to learn the size in force rather than assume these.
 *
 * The MAX values bound the largest grid the kernel will ever build, and are
 * what a fixed buffer has to be sized by.  They are not a guess: the
 * framebuffer console is bounded by these same two numbers, so a program that
 * sizes a line buffer by W_CONSOLE_MAX_WIDTH and fills it to the width
 * wconsize() reported cannot overrun it.  A machine with a 1280x800 display
 * gives a 160x50 console, and 1920x1200 gives 240x75, which is where these
 * come from. */
#define W_CONSOLE_WIDTH      80
#define W_CONSOLE_HEIGHT     50
#define W_CONSOLE_MAX_WIDTH  240
#define W_CONSOLE_MAX_HEIGHT 75

/* The character cell, in pixels.  The console draws an 8x16 font, so a cell is
 * twice as tall as it is wide -- which a program dividing the screen has to
 * know, because a grid of 80x25 characters is a display that is wider than it
 * is tall, and one of 80x50 is a display that is taller than it is wide. */
#define W_CELL_WIDTH   8
#define W_CELL_HEIGHT 16

/* Special keys returned by wgetkey().  They start above 0xFF so they cannot
 * collide with an ordinary character. */
#define W_KEY_UP     0x100
#define W_KEY_DOWN   0x101
#define W_KEY_RIGHT  0x102
#define W_KEY_LEFT   0x103
#define W_KEY_HOME   0x104
#define W_KEY_END    0x105
#define W_KEY_PGUP   0x106
#define W_KEY_PGDN   0x107
#define W_KEY_DELETE 0x108
#define W_KEY_ESCAPE 0x1B

/* Colours for wcolor(). Add W_BRIGHT to a foreground for the bright variant. */
#define W_BLACK   0
#define W_RED     1
#define W_GREEN   2
#define W_YELLOW  3
#define W_BLUE    4
#define W_MAGENTA 5
#define W_CYAN    6
#define W_WHITE   7
#define W_BRIGHT  8
#define W_DEFAULT (-1)

#endif /* WOS_WABI_H */
