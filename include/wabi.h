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
#define W_ERANGE        34   /* result does not fit in the buffer  */
#define W_ENAMETOOLONG  36   /* path or name too long              */
#define W_ENOSYS        38   /* no such syscall                    */
#define W_ENOTEMPTY     39   /* directory not empty                */
#define W_EFBIG         27   /* file too large for this filesystem */

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

typedef struct {
    uint32_t total_bytes;    /* usable RAM the machine reported          */
    uint32_t used_bytes;     /* frames currently allocated               */
    uint32_t free_bytes;     /* total_bytes - used_bytes                 */
    uint32_t kernel_bytes;   /* kernel image, heap arena and page tables */
    uint32_t page_size;      /* always 4096                              */
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
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
} wdiskinfo_t;

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
#define WSYS_MAX        25

#endif /* WOS_WABI_H */
