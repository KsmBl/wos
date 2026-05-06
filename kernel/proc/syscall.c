/* Syscall dispatch.
 *
 * Entered through int 0x80 with the call number in eax and up to five
 * arguments in ebx, ecx, edx, esi, edi.  The result goes back in eax; failures
 * are the negated W_E* code, so user space checks `if (r < 0)`.
 *
 * Every pointer that arrives from ring 3 is checked against the calling
 * process's page tables before the kernel touches it.  Ring 3 cannot reach
 * kernel memory through the MMU, but it can happily pass a kernel address as
 * an argument, and the kernel would dereference it with full privilege.
 */

#include "proc.h"
#include "sched.h"
#include "vfs.h"
#include "pipe.h"
#include "wfs_kernel.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "isr.h"
#include "pit.h"
#include "power.h"
#include "keyboard.h"
#include "vga.h"
#include "net.h"
#include "user.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* Largest single read or write we will service, so a bogus length cannot make
 * the kernel loop over 4 GiB of page checks. */
#define MAX_IO_SIZE (1u * 1024u * 1024u)

/* Check that [addr, addr+len) is entirely mapped and reachable from ring 3. */
static bool user_range_ok(const void *addr, uint64_t len, bool need_write)
{
    uint64_t start = (uint64_t)addr;

    if (len == 0)
        return true;
    if (start < USER_BASE || start + len < start)   /* NULL or wraparound */
        return false;
    if (start + len > USER_STACK_TOP)
        return false;

    process_t *p = proc_current();
    for (uint64_t page = ALIGN_DOWN(start, PAGE_SIZE);
         page < start + len; page += PAGE_SIZE) {
        if (!paging_user_can_access(p->space, page, need_write))
            return false;
    }
    return true;
}

/* Copy a NUL-terminated string in from user space, checking each page as we
 * reach it so a string running off the end of a mapping is caught. */
static int copy_string_from_user(const char *user, char *dst, uint64_t max)
{
    uint64_t addr = (uint64_t)user;

    if (addr < USER_BASE || addr >= USER_STACK_TOP)
        return -W_EFAULT;

    process_t *p = proc_current();

    for (uint64_t i = 0; i < max; i++) {
        uint64_t at = addr + i;

        if (at >= USER_STACK_TOP)
            return -W_EFAULT;
        if (i == 0 || (at % PAGE_SIZE) == 0) {
            if (!paging_user_can_access(p->space, at, false))
                return -W_EFAULT;
        }

        dst[i] = ((const char *)user)[i];
        if (dst[i] == '\0')
            return (int)i;
    }
    return -W_ENAMETOOLONG;
}

/* ------------------------------------------------------------------ *
 *  The calls
 * ------------------------------------------------------------------ */

static int64_t sys_exit(uint64_t status)
{
    proc_exit((int32_t)status);
    return 0;   /* not reached */
}

static int64_t sys_open(uint64_t path, uint64_t flags)
{
    char buf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, buf, sizeof(buf));
    if (r < 0)
        return r;

    return vfs_open(proc_current(), buf, flags);
}

static int64_t sys_close(uint64_t fd)
{
    return vfs_close(proc_current(), (int)fd);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint32_t len)
{
    if (len > MAX_IO_SIZE)
        len = MAX_IO_SIZE;
    if (!user_range_ok((void *)buf, len, true))
        return -W_EFAULT;

    return vfs_read(proc_current(), (int)fd, (void *)buf, len);
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint32_t len)
{
    if (len > MAX_IO_SIZE)
        len = MAX_IO_SIZE;
    if (!user_range_ok((const void *)buf, len, false))
        return -W_EFAULT;

    return vfs_write(proc_current(), (int)fd, (const void *)buf, len);
}

static int64_t sys_lseek(uint64_t fd, uint64_t offset, uint32_t whence)
{
    return vfs_lseek(proc_current(), (int)fd, (int32_t)offset, (int)whence);
}

static int64_t sys_stat(uint64_t path, uint64_t out)
{
    char buf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, buf, sizeof(buf));
    if (r < 0)
        return r;
    if (!user_range_ok((void *)out, sizeof(wstat_t), true))
        return -W_EFAULT;

    return vfs_stat(proc_current(), buf, (wstat_t *)out);
}

static int64_t sys_path_only(uint64_t path,
                             int (*op)(struct process *, const char *))
{
    char buf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, buf, sizeof(buf));
    if (r < 0)
        return r;

    return op(proc_current(), buf);
}

static int64_t sys_unlink(uint64_t path)  { return sys_path_only(path, vfs_unlink); }
static int64_t sys_mkdir(uint64_t path)   { return sys_path_only(path, vfs_mkdir); }
static int64_t sys_rmdir(uint64_t path)   { return sys_path_only(path, vfs_rmdir); }
static int64_t sys_chdir(uint64_t path)   { return sys_path_only(path, vfs_chdir); }
static int64_t sys_opendir(uint64_t path) { return sys_path_only(path, vfs_opendir); }

static int64_t sys_readdir(uint64_t fd, uint64_t out)
{
    if (!user_range_ok((void *)out, sizeof(wdirent_t), true))
        return -W_EFAULT;

    return vfs_readdir(proc_current(), (int)fd, (wdirent_t *)out);
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size)
{
    if (size > W_PATH_MAX + 1)
        size = W_PATH_MAX + 1;
    if (!user_range_ok((void *)buf, size, true))
        return -W_EFAULT;

    return vfs_getcwd(proc_current(), (char *)buf, size);
}

static int64_t sys_meminfo(uint64_t out)
{
    if (!user_range_ok((void *)out, sizeof(wmeminfo_t), true))
        return -W_EFAULT;

    wmeminfo_t *info = (wmeminfo_t *)out;
    info->total_bytes  = pmm_total_bytes();
    info->used_bytes   = pmm_used_bytes();
    info->free_bytes   = pmm_free_bytes();
    info->kernel_bytes = pmm_kernel_bytes();
    info->page_size    = PAGE_SIZE;
    return 0;
}

static int64_t sys_procmem(uint64_t pid, uint64_t out)
{
    if (!user_range_ok((void *)out, sizeof(wprocmem_t), true))
        return -W_EFAULT;

    process_t *p = ((int32_t)pid <= 0) ? proc_current()
                                       : proc_by_pid((int32_t)pid);
    if (!p)
        return -W_ESRCH;

    proc_meminfo(p, (wprocmem_t *)out);
    return 0;
}

static int64_t sys_threadmem(uint64_t tid, uint64_t out)
{
    if (!user_range_ok((void *)out, sizeof(wthreadmem_t), true))
        return -W_EFAULT;

    /* Only the calling thread is addressable for now; there is no API to
     * create others, so anything else would be unreachable by definition. */
    thread_t *t = thread_current();
    if ((int32_t)tid > 0 && t->tid != (int32_t)tid)
        return -W_ESRCH;

    thread_meminfo(t, (wthreadmem_t *)out);
    return 0;
}

static int64_t sys_proclist(uint64_t out, uint64_t max)
{
    if (max > MAX_PROCESSES)
        max = MAX_PROCESSES;
    if (!user_range_ok((void *)out, max * sizeof(wprocmem_t), true))
        return -W_EFAULT;

    return proc_list((wprocmem_t *)out, (int32_t)max);
}

static int64_t sys_diskinfo(uint64_t out)
{
    if (!user_range_ok((void *)out, sizeof(wdiskinfo_t), true))
        return -W_EFAULT;

    wfs_statfs((wdiskinfo_t *)out);
    return 0;
}

/* Copy a user argv array into the kernel.  Fills `args` (NULL-terminated) with
 * pointers into a single heap allocation returned through `storage_out`, which
 * the caller must kfree.  Returns argc, or a negative W_E* code.
 *
 * The copies come from the heap, not the stack: MAX_ARGS full-length arguments
 * are several times the size of a kernel stack.  Everything is captured before
 * the address space is switched out from under the user pointers. */
static int copy_user_argv(uint64_t argv, char *args[], char **storage_out)
{
    *storage_out = NULL;
    int argc = 0;

    if (!argv) {
        args[0] = NULL;
        return 0;
    }

    if (!user_range_ok((void *)argv, sizeof(uint64_t), false))
        return -W_EFAULT;

    char *storage = kmalloc(MAX_ARGS * (W_PATH_MAX + 1));
    if (!storage)
        return -W_ENOMEM;

    const uint64_t *user_argv = (const uint64_t *)argv;
    while (argc < MAX_ARGS) {
        if (!user_range_ok(&user_argv[argc], sizeof(uint64_t), false)) {
            kfree(storage);
            return -W_EFAULT;
        }
        if (user_argv[argc] == 0)
            break;

        char *slot = storage + (uint64_t)argc * (W_PATH_MAX + 1);
        int r = copy_string_from_user((const char *)user_argv[argc],
                                      slot, W_PATH_MAX + 1);
        if (r < 0) {
            kfree(storage);
            return r;
        }

        args[argc] = slot;
        argc++;
    }
    args[argc] = NULL;

    *storage_out = storage;
    return argc;
}

static int64_t sys_spawn(uint64_t path, uint64_t argv)
{
    char pathbuf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, pathbuf, sizeof(pathbuf));
    if (r < 0)
        return r;

    char *args[MAX_ARGS + 1];
    char *storage = NULL;
    int   argc = copy_user_argv(argv, args, &storage);
    if (argc < 0)
        return argc;

    int32_t pid = proc_spawn(pathbuf, args, proc_current());

    if (storage)
        kfree(storage);

    return pid;
}

/* Resolve a descriptor that must be a pipe end of the given direction, and
 * hand back the pipe object behind it. */
static int pipe_from_fd(struct process *p, int fd, bool want_write,
                        struct pipe **out)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES)
        return -W_EBADF;

    file_t *f = &p->fds[fd];
    if (f->type != FD_PIPE || f->write_end != want_write)
        return -W_EBADF;

    *out = f->pipe;
    return 0;
}

static int64_t sys_spawn_io(uint64_t path, uint64_t argv, uint64_t io_ptr)
{
    char pathbuf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, pathbuf, sizeof(pathbuf));
    if (r < 0)
        return r;

    if (!user_range_ok((void *)io_ptr, sizeof(wspawnio_t), false))
        return -W_EFAULT;

    wspawnio_t req = *(const wspawnio_t *)io_ptr;
    process_t *me  = proc_current();

    struct spawn_io io = {
        .piped = true,
        .rows  = (req.rows > 0) ? (uint32_t)req.rows : 0,
        .cols  = (req.cols > 0) ? (uint32_t)req.cols : 0,
    };

    r = pipe_from_fd(me, req.in_fd, false, &io.in);
    if (r < 0)
        return r;
    r = pipe_from_fd(me, req.out_fd, true, &io.out);
    if (r < 0)
        return r;

    char *args[MAX_ARGS + 1];
    char *storage = NULL;
    int   argc = copy_user_argv(argv, args, &storage);
    if (argc < 0)
        return argc;

    int32_t pid = proc_spawn_io(pathbuf, args, me, &io);

    if (storage)
        kfree(storage);

    return pid;
}

static int64_t sys_pipe(uint64_t user_fds)
{
    if (!user_range_ok((void *)user_fds, 2 * sizeof(int32_t), true))
        return -W_EFAULT;

    int fds[2];
    int r = vfs_pipe(proc_current(), fds);
    if (r < 0)
        return r;

    ((int32_t *)user_fds)[0] = fds[0];
    ((int32_t *)user_fds)[1] = fds[1];
    return 0;
}

static int64_t sys_consize(uint64_t rows_ptr, uint64_t cols_ptr)
{
    if (!user_range_ok((void *)rows_ptr, sizeof(int32_t), true) ||
        !user_range_ok((void *)cols_ptr, sizeof(int32_t), true))
        return -W_EFAULT;

    process_t *p = proc_current();

    /* A process drawing to the real console gets the live text-mode size, so
     * it follows wsetmode() without anyone having to update a stored value.  A
     * process in a window reports the size its parent gave it. */
    if (vfs_stdin_is_console(p)) {
        int cols, rows;
        vga_size(&cols, &rows);
        *(int32_t *)rows_ptr = (int32_t)rows;
        *(int32_t *)cols_ptr = (int32_t)cols;
    } else {
        *(int32_t *)rows_ptr = (int32_t)p->term_rows;
        *(int32_t *)cols_ptr = (int32_t)p->term_cols;
    }
    return 0;
}

/* Change the physical text mode.  Only a process actually attached to the
 * console may do this -- a program running in a pipe or a window must not
 * reprogram the hardware out from under whoever owns the screen. */
static int64_t sys_setmode(uint64_t cols, uint64_t rows)
{
    if (!vfs_stdin_is_console(proc_current()))
        return -W_EPERM;

    return vga_set_mode((int)cols, (int)rows) < 0 ? -W_EINVAL : 0;
}

/* Change the terminal size reported to one of your own children, so a program
 * it later spawns lays itself out to a resized window.  A caller may only set
 * a process it is the parent of. */
static int64_t sys_setsize(uint64_t pid, uint64_t rows, uint64_t cols)
{
    process_t *p = proc_by_pid((int32_t)pid);
    if (!p)
        return -W_ESRCH;
    if (p->parent != proc_current())
        return -W_EPERM;

    if ((int32_t)rows > 0) p->term_rows = (uint32_t)rows;
    if ((int32_t)cols > 0) p->term_cols = (uint32_t)cols;
    return 0;
}

static int64_t sys_wait(uint64_t pid, uint64_t status_out)
{
    int64_t status = 0;

    if (status_out && !user_range_ok((void *)status_out, sizeof(int32_t), true))
        return -W_EFAULT;

    int32_t st32 = 0;
    int32_t r = proc_wait((int32_t)pid, &st32);
    status = st32;

    if (r >= 0 && status_out)
        *(int32_t *)status_out = (int32_t)status;

    return r;
}

static int64_t sys_getpid(void)
{
    return proc_current()->pid;
}

static int64_t sys_sbrk(uint64_t increment)
{
    return (int64_t)proc_sbrk((int64_t)increment);
}

static int64_t sys_ticks(void)
{
    return (int64_t)pit_ticks();
}

static int64_t sys_yield(void)
{
    sched_yield();
    return 0;
}

static int64_t sys_console(uint64_t mode)
{
    if (mode != W_CONSOLE_CANONICAL && mode != W_CONSOLE_RAW)
        return -W_EINVAL;

    /* The raw/canonical setting is a property of the physical keyboard.  A
     * program reading from a pipe -- one running inside vim's :term, say --
     * must not reach through and change it for whoever owns the real console.
     * Its own stdin is a byte stream that is always effectively raw, so report
     * that and do nothing. */
    if (!vfs_stdin_is_console(proc_current()))
        return W_CONSOLE_RAW;

    bool was_raw = keyboard_raw();
    keyboard_set_raw(mode == W_CONSOLE_RAW);

    return was_raw ? W_CONSOLE_RAW : W_CONSOLE_CANONICAL;
}

static int64_t sys_pollin(uint64_t fd)
{
    process_t *p = proc_current();

    if ((int)fd >= 0 && (int)fd < MAX_OPEN_FILES &&
        p->fds[fd].type == FD_PIPE && !p->fds[fd].write_end)
        return pipe_pollin(p->fds[fd].pipe) ? 1 : 0;

    /* Only the console can otherwise make a reader wait; a file read always
     * has something to return, even if that something is end of file. */
    if (fd == W_STDIN && p->fds[W_STDIN].type == FD_CONSOLE)
        return keyboard_has_data() ? 1 : 0;

    return 1;
}

/* ------------------------------------------------------------------ *
 *  Users
 * ------------------------------------------------------------------ */

static int64_t sys_getuid(void)
{
    return proc_current()->uid;
}

static int64_t sys_userinfo(uint64_t uid, uint64_t out)
{
    if (!user_range_ok((void *)out, sizeof(wuser_t), true))
        return -W_EFAULT;

    /* A negative uid means "whoever is asking". */
    uint32_t want = ((int64_t)uid < 0) ? proc_current()->uid : (uint32_t)uid;

    return user_by_uid(want, (wuser_t *)out);
}

static int64_t sys_userlist(uint64_t out, uint64_t max)
{
    if (max > W_MAX_USERS)
        max = W_MAX_USERS;
    if (!user_range_ok((void *)out, max * sizeof(wuser_t), true))
        return -W_EFAULT;

    return user_list((wuser_t *)out, (int)max);
}

/* Authenticate and, if it works, become that user.
 *
 * Doing the check in the kernel is what lets this exist at all: the password
 * file is unreadable from ring 3, so there is no way to verify a password in
 * user space and no need for setuid to work around it. */
static int64_t sys_login(uint64_t name, uint64_t password)
{
    char namebuf[W_NAME_LEN + 1];
    char passbuf[128];

    int r = copy_string_from_user((const char *)name, namebuf, sizeof(namebuf));
    if (r < 0)
        return r;
    r = copy_string_from_user((const char *)password, passbuf, sizeof(passbuf));
    if (r < 0)
        return r;

    process_t *p = proc_current();
    wuser_t    who;

    /* Root may become anyone without proving anything; that is what being
     * root means. */
    if (p->uid == W_ROOT_UID) {
        r = user_by_name(namebuf, &who);
    } else {
        r = user_authenticate(namebuf, passbuf, &who);
    }
    if (r < 0)
        return r;

    p->uid = who.uid;
    return (int64_t)who.uid;
}

static int64_t sys_passwd(uint64_t name, uint64_t old_password,
                          uint64_t new_password)
{
    char namebuf[W_NAME_LEN + 1];
    char oldbuf[128];
    char newbuf[128];

    int r = copy_string_from_user((const char *)name, namebuf, sizeof(namebuf));
    if (r < 0)
        return r;
    r = copy_string_from_user((const char *)old_password, oldbuf, sizeof(oldbuf));
    if (r < 0)
        return r;
    r = copy_string_from_user((const char *)new_password, newbuf, sizeof(newbuf));
    if (r < 0)
        return r;

    return user_set_password(proc_current()->uid, namebuf, oldbuf, newbuf);
}

static int64_t sys_useradd(uint64_t name, uint64_t password, uint64_t roles)
{
    char namebuf[W_NAME_LEN + 1];
    char passbuf[128];

    int r = copy_string_from_user((const char *)name, namebuf, sizeof(namebuf));
    if (r < 0)
        return r;
    r = copy_string_from_user((const char *)password, passbuf, sizeof(passbuf));
    if (r < 0)
        return r;

    return user_add(proc_current()->uid, namebuf, passbuf, (uint32_t)roles);
}

static int64_t sys_setroles(uint64_t name, uint64_t roles)
{
    char namebuf[W_NAME_LEN + 1];

    int r = copy_string_from_user((const char *)name, namebuf, sizeof(namebuf));
    if (r < 0)
        return r;

    return user_set_roles(proc_current()->uid, namebuf, (uint32_t)roles);
}

/* Send one ICMP echo to `dst` (network order) and wait for the reply.  The
 * echo id is the caller's pid, so replies to different processes do not get
 * confused.  Returns the round-trip time in microseconds, or a negative
 * error. */
static int64_t sys_ping(uint64_t dst, uint64_t seq, uint64_t timeout_ms)
{
    uint16_t id = (uint16_t)proc_current()->pid;
    uint32_t rtt = 0;

    int r = net_ping((uint32_t)dst, id, (uint16_t)seq, (uint32_t)timeout_ms, &rtt);
    if (r < 0)
        return r;
    return (int64_t)rtt;
}

static int64_t sys_getshell(uint64_t uid, uint64_t buf, uint32_t size)
{
    if (size == 0 || !user_range_ok((void *)buf, size, true))
        return -W_EFAULT;

    /* A negative uid means "whoever is asking". */
    uint32_t want = ((int64_t)uid < 0) ? proc_current()->uid : (uint32_t)uid;

    char path[W_SHELL_MAX + 1];
    int  r = user_shell(want, path, sizeof(path));
    if (r < 0)
        return r;

    strlcpy((char *)buf, path, size);
    return 0;
}

static int64_t sys_setshell(uint64_t name, uint64_t shell)
{
    char namebuf[W_NAME_LEN + 1];
    char shellbuf[W_SHELL_MAX + 1];

    int r = copy_string_from_user((const char *)name, namebuf, sizeof(namebuf));
    if (r < 0)
        return r;
    r = copy_string_from_user((const char *)shell, shellbuf, sizeof(shellbuf));
    if (r < 0)
        return r;

    return user_set_shell(proc_current()->uid, namebuf, shellbuf);
}

static int64_t sys_shutdown(void)
{
    /* There is no user or permission model in WOS, so any process may do
     * this. On a system with several users that would need a check here. */
    power_off();
    return 0;   /* not reached */
}

/* ------------------------------------------------------------------ *
 *  Dispatch
 * ------------------------------------------------------------------ */

static void syscall_handler(regs_t *regs)
{
    uint64_t n = regs->rax;
    int64_t  r;

    switch (n) {
    case WSYS_EXIT:      r = sys_exit(regs->rdi); break;
    case WSYS_OPEN:      r = sys_open(regs->rdi, regs->rsi); break;
    case WSYS_CLOSE:     r = sys_close(regs->rdi); break;
    case WSYS_READ:      r = sys_read(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_WRITE:     r = sys_write(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_LSEEK:     r = sys_lseek(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_STAT:      r = sys_stat(regs->rdi, regs->rsi); break;
    case WSYS_UNLINK:    r = sys_unlink(regs->rdi); break;
    case WSYS_MKDIR:     r = sys_mkdir(regs->rdi); break;
    case WSYS_RMDIR:     r = sys_rmdir(regs->rdi); break;
    case WSYS_OPENDIR:   r = sys_opendir(regs->rdi); break;
    case WSYS_READDIR:   r = sys_readdir(regs->rdi, regs->rsi); break;
    case WSYS_CHDIR:     r = sys_chdir(regs->rdi); break;
    case WSYS_GETCWD:    r = sys_getcwd(regs->rdi, regs->rsi); break;
    case WSYS_MEMINFO:   r = sys_meminfo(regs->rdi); break;
    case WSYS_PROCMEM:   r = sys_procmem(regs->rdi, regs->rsi); break;
    case WSYS_THREADMEM: r = sys_threadmem(regs->rdi, regs->rsi); break;
    case WSYS_PROCLIST:  r = sys_proclist(regs->rdi, regs->rsi); break;
    case WSYS_DISKINFO:  r = sys_diskinfo(regs->rdi); break;
    case WSYS_SPAWN:     r = sys_spawn(regs->rdi, regs->rsi); break;
    case WSYS_WAIT:      r = sys_wait(regs->rdi, regs->rsi); break;
    case WSYS_GETPID:    r = sys_getpid(); break;
    case WSYS_SBRK:      r = sys_sbrk(regs->rdi); break;
    case WSYS_TICKS:     r = sys_ticks(); break;
    case WSYS_YIELD:     r = sys_yield(); break;
    case WSYS_CONSOLE:   r = sys_console(regs->rdi); break;
    case WSYS_POLLIN:    r = sys_pollin(regs->rdi); break;
    case WSYS_GETUID:    r = sys_getuid(); break;
    case WSYS_USERINFO:  r = sys_userinfo(regs->rdi, regs->rsi); break;
    case WSYS_USERLIST:  r = sys_userlist(regs->rdi, regs->rsi); break;
    case WSYS_LOGIN:     r = sys_login(regs->rdi, regs->rsi); break;
    case WSYS_PASSWD:    r = sys_passwd(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_USERADD:   r = sys_useradd(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_SETROLES:  r = sys_setroles(regs->rdi, regs->rsi); break;
    case WSYS_PIPE:      r = sys_pipe(regs->rdi); break;
    case WSYS_SPAWN_IO:  r = sys_spawn_io(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_CONSIZE:   r = sys_consize(regs->rdi, regs->rsi); break;
    case WSYS_SETSIZE:   r = sys_setsize(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_SETMODE:   r = sys_setmode(regs->rdi, regs->rsi); break;
    case WSYS_GETSHELL:  r = sys_getshell(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_SETSHELL:  r = sys_setshell(regs->rdi, regs->rsi); break;
    case WSYS_PING:      r = sys_ping(regs->rdi, regs->rsi, regs->rdx); break;
    case WSYS_SHUTDOWN:  r = sys_shutdown(); break;
    default:             r = -W_ENOSYS; break;
    }

    regs->rax = (uint64_t)r;
}

void syscall_init(void)
{
    register_interrupt_handler(INT_SYSCALL, syscall_handler);
}
