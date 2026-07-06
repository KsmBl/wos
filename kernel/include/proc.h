/* Processes and threads.
 *
 * A process owns an address space, a working directory and a file descriptor
 * table.  A thread owns a kernel stack and is what the scheduler actually
 * runs.  Every process currently has exactly one thread, but the split is real
 * so that adding threads later does not mean rewriting the scheduler.
 *
 * Per-process memory figures come from the page tables (see
 * paging_user_bytes), not from an estimate, so wprocmem() reports what the
 * process genuinely has mapped.
 */
#ifndef WOS_PROC_H
#define WOS_PROC_H

#include "types.h"
#include "paging.h"
#include "vfs.h"
#include "shm.h"
#include "wabi.h"

#define MAX_PROCESSES     32
#define MAX_THREADS       64

/* Generous, because the filesystem path uses whole 1 KiB block buffers as
 * locals and a spawn nests several of them: syscall -> spawn -> read file ->
 * wfs_read -> bmap. */
#define KERNEL_STACK_SIZE 16384

/* Thread scheduling states. */
typedef enum {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_ZOMBIE
} thread_state_t;

/* Why a blocked thread is waiting, and what will wake it. */
typedef enum {
    WAIT_NONE = 0,
    WAIT_INPUT,      /* console input; woken by the keyboard IRQ    */
    WAIT_CHILD,      /* a child to exit; woken by process teardown  */
    WAIT_PIPE,       /* pipe space or data; woken by the other end  */
    WAIT_TIME,       /* a deadline; woken by the timer              */
    WAIT_SOCKET      /* a socket to become readable, writable or to
                      * accept a connection; woken by its peer      */
} wait_reason_t;

struct process;

typedef struct thread {
    int32_t         tid;
    struct process *proc;
    thread_state_t  state;
    wait_reason_t   wait_reason;

    uint64_t        kernel_stack;      /* base of the allocation      */
    uint64_t        kernel_stack_size;
    uint64_t        rsp;               /* saved kernel rsp while off-CPU */

    /* Where this thread should start in ring 3.  Held per thread rather than
     * in a global, because the thread does not run until the scheduler picks
     * it and another spawn could happen in between. */
    uint64_t        user_entry;
    uint64_t        user_stack;

    uint64_t        cpu_ticks;
    uint32_t        wake_at;           /* tick to wake on, for WAIT_TIME */
    struct thread  *next;              /* run queue link */
} thread_t;

/* Most arguments a spawned program can be given. */
#define MAX_ARGS 32

typedef struct process {
    int32_t      pid;
    char         name[32];
    bool         used;

    addrspace_t *space;
    char         cwd[W_PATH_MAX + 1];
    file_t       fds[MAX_OPEN_FILES];

    /* Who this process runs as.  Inherited by every child, and changed only
     * by a successful login. */
    uint32_t     uid;

    /* The size of the terminal this process draws to, reported by wconsize().
     * The console is a fixed 80x25; a program started into a pipe by vim's
     * :term is told the size of the window it was given instead. */
    uint32_t     term_rows;
    uint32_t     term_cols;

    /* Memory accounting, in bytes. */
    uint64_t     code_bytes;
    uint64_t     data_bytes;
    uint64_t     stack_bytes;
    uint64_t     heap_start;           /* page-aligned, just past the image */
    uint64_t     heap_break;           /* current top of the heap           */

    /* Shared memory this process has mapped.  Held here rather than with the
     * objects because a mapping is a property of an address space, and because
     * these have to come out before the address space is torn down. */
    shm_mapping_t maps[SHM_MAX_MAPPINGS];

    struct process *parent;
    bool         exited;
    bool         killed;       /* asked to stop; unwinds at the next
                               * safe moment -- see proc_kill()      */
    int32_t      exit_status;

    thread_t    *thread;               /* the one thread, for now */
    int32_t      thread_count;
} process_t;

/* Set up the process table and adopt the current boot context as the idle
 * thread, so there is always something runnable. */
void proc_init(void);

process_t *proc_current(void);
thread_t  *thread_current(void);

/* Look a process up by pid. Returns NULL if there is no such process. */
process_t *proc_by_pid(int32_t pid);

/* How a spawned process's standard descriptors should be wired.  When `piped`
 * is false the child gets the console, exactly as an ordinary launch does.
 * When true, fd 0 reads from `in` and fds 1 and 2 write to `out`, and the
 * child reports `rows`x`cols` from wconsize(). */
struct pipe;
struct spawn_io {
    bool         piped;
    struct pipe *in;      /* child stdin  (read end)         */
    struct pipe *out;     /* child stdout/stderr (write end) */
    uint32_t     rows;
    uint32_t     cols;
};

/* Load `path` as a new ring 3 process and make it runnable.
 * `argv` is a NULL-terminated array; argv[0] should be the program name.
 * `io` may be NULL for the ordinary console wiring.
 * Returns the new pid, or a negative W_E* code. */
int32_t proc_spawn(const char *path, char *const argv[], process_t *parent);
int32_t proc_spawn_io(const char *path, char *const argv[], process_t *parent,
                      const struct spawn_io *io);

/* Terminate the calling process. Does not return. */
void proc_exit(int32_t status) __attribute__((noreturn));

/* Wait for a child to exit.  `pid` may be -1 for "any child".  Stores the
 * exit status in `status` if it is non-NULL.  Returns the reaped pid, or a
 * negative W_E* code. */
int32_t proc_wait(int32_t pid, int32_t *status);

/* Reap a child that has already exited, without waiting for one that has not.
 *
 * A program that spawns and then goes back to waiting on something else -- a
 * compositor, which starts what a keybinding asks for and then returns to its
 * event loop -- cannot call proc_wait(): it would stop serving everything else
 * until that child happened to finish.  Without this, every program it started
 * would stay in the process table as a zombie.
 *
 * Returns the pid reaped, or -W_ECHILD when no child has exited (which
 * includes having no children at all). */
int32_t proc_reap(int32_t *status);

/* Ask a process to stop, from outside it.
 *
 * There is no signal mechanism here and no way to unwind another thread's
 * kernel stack from a distance, so this does not kill anything directly: it
 * marks the process and wakes it, and the process leaves through proc_exit()
 * at the next point where it holds nothing -- returning from a blocking wait,
 * entering a syscall, or being interrupted while in ring 3.
 *
 * The effect is prompt for anything that waits, which is every service, and
 * for anything that computes, which is caught by the timer.  Returns 0, or
 * -W_ESRCH. */
int32_t proc_kill(int32_t pid);

/* True when the current process has been asked to stop.  The blocking paths
 * check this after every wake. */
bool proc_should_exit(void);

/* Grow or shrink the calling process's heap by `increment` bytes.
 * Returns the previous break, or (uint32_t)-1 on failure. */
uint64_t proc_sbrk(int64_t increment);

/* Fill in the memory statistics for a process / thread. */
void proc_meminfo(const process_t *p, wprocmem_t *out);
void thread_meminfo(const thread_t *t, wthreadmem_t *out);

/* Copy up to `max` process records into `out`. Returns how many were written. */
int32_t proc_list(wprocmem_t *out, int32_t max);

/* Install the int 0x80 handler. */
void syscall_init(void);

#endif /* WOS_PROC_H */
