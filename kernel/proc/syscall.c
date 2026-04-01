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
#include "wfs_kernel.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "isr.h"
#include "pit.h"
#include "power.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* Largest single read or write we will service, so a bogus length cannot make
 * the kernel loop over 4 GiB of page checks. */
#define MAX_IO_SIZE (1u * 1024u * 1024u)

/* Check that [addr, addr+len) is entirely mapped and reachable from ring 3. */
static bool user_range_ok(const void *addr, uint32_t len, bool need_write)
{
    uint32_t start = (uint32_t)addr;

    if (len == 0)
        return true;
    if (start < USER_BASE || start + len < start)   /* NULL or wraparound */
        return false;
    if (start + len > USER_STACK_TOP)
        return false;

    process_t *p = proc_current();
    for (uint32_t page = ALIGN_DOWN(start, PAGE_SIZE);
         page < start + len; page += PAGE_SIZE) {
        if (!paging_user_can_access(p->space, page, need_write))
            return false;
    }
    return true;
}

/* Copy a NUL-terminated string in from user space, checking each page as we
 * reach it so a string running off the end of a mapping is caught. */
static int copy_string_from_user(const char *user, char *dst, uint32_t max)
{
    uint32_t addr = (uint32_t)user;

    if (addr < USER_BASE || addr >= USER_STACK_TOP)
        return -W_EFAULT;

    process_t *p = proc_current();

    for (uint32_t i = 0; i < max; i++) {
        uint32_t at = addr + i;

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

static int32_t sys_exit(uint32_t status)
{
    proc_exit((int32_t)status);
    return 0;   /* not reached */
}

static int32_t sys_open(uint32_t path, uint32_t flags)
{
    char buf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, buf, sizeof(buf));
    if (r < 0)
        return r;

    return vfs_open(proc_current(), buf, flags);
}

static int32_t sys_close(uint32_t fd)
{
    return vfs_close(proc_current(), (int)fd);
}

static int32_t sys_read(uint32_t fd, uint32_t buf, uint32_t len)
{
    if (len > MAX_IO_SIZE)
        len = MAX_IO_SIZE;
    if (!user_range_ok((void *)buf, len, true))
        return -W_EFAULT;

    return vfs_read(proc_current(), (int)fd, (void *)buf, len);
}

static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t len)
{
    if (len > MAX_IO_SIZE)
        len = MAX_IO_SIZE;
    if (!user_range_ok((const void *)buf, len, false))
        return -W_EFAULT;

    return vfs_write(proc_current(), (int)fd, (const void *)buf, len);
}

static int32_t sys_lseek(uint32_t fd, uint32_t offset, uint32_t whence)
{
    return vfs_lseek(proc_current(), (int)fd, (int32_t)offset, (int)whence);
}

static int32_t sys_stat(uint32_t path, uint32_t out)
{
    char buf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, buf, sizeof(buf));
    if (r < 0)
        return r;
    if (!user_range_ok((void *)out, sizeof(wstat_t), true))
        return -W_EFAULT;

    return vfs_stat(proc_current(), buf, (wstat_t *)out);
}

static int32_t sys_path_only(uint32_t path,
                             int (*op)(struct process *, const char *))
{
    char buf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, buf, sizeof(buf));
    if (r < 0)
        return r;

    return op(proc_current(), buf);
}

static int32_t sys_unlink(uint32_t path)  { return sys_path_only(path, vfs_unlink); }
static int32_t sys_mkdir(uint32_t path)   { return sys_path_only(path, vfs_mkdir); }
static int32_t sys_rmdir(uint32_t path)   { return sys_path_only(path, vfs_rmdir); }
static int32_t sys_chdir(uint32_t path)   { return sys_path_only(path, vfs_chdir); }
static int32_t sys_opendir(uint32_t path) { return sys_path_only(path, vfs_opendir); }

static int32_t sys_readdir(uint32_t fd, uint32_t out)
{
    if (!user_range_ok((void *)out, sizeof(wdirent_t), true))
        return -W_EFAULT;

    return vfs_readdir(proc_current(), (int)fd, (wdirent_t *)out);
}

static int32_t sys_getcwd(uint32_t buf, uint32_t size)
{
    if (size > W_PATH_MAX + 1)
        size = W_PATH_MAX + 1;
    if (!user_range_ok((void *)buf, size, true))
        return -W_EFAULT;

    return vfs_getcwd(proc_current(), (char *)buf, size);
}

static int32_t sys_meminfo(uint32_t out)
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

static int32_t sys_procmem(uint32_t pid, uint32_t out)
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

static int32_t sys_threadmem(uint32_t tid, uint32_t out)
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

static int32_t sys_proclist(uint32_t out, uint32_t max)
{
    if (max > MAX_PROCESSES)
        max = MAX_PROCESSES;
    if (!user_range_ok((void *)out, max * sizeof(wprocmem_t), true))
        return -W_EFAULT;

    return proc_list((wprocmem_t *)out, (int32_t)max);
}

static int32_t sys_diskinfo(uint32_t out)
{
    if (!user_range_ok((void *)out, sizeof(wdiskinfo_t), true))
        return -W_EFAULT;

    wfs_statfs((wdiskinfo_t *)out);
    return 0;
}

static int32_t sys_spawn(uint32_t path, uint32_t argv)
{
    char pathbuf[W_PATH_MAX + 1];
    int  r = copy_string_from_user((const char *)path, pathbuf, sizeof(pathbuf));
    if (r < 0)
        return r;

    /* argv is an array of user pointers; both the array and every string it
     * names have to be copied into the kernel before the address space is
     * switched out from under them.
     *
     * The copies come from the heap, not the stack: MAX_ARGS full-length
     * arguments are several times the size of a kernel stack. */
    char *args[MAX_ARGS + 1];
    char *storage = NULL;
    int   argc = 0;

    if (argv) {
        if (!user_range_ok((void *)argv, sizeof(uint32_t), false))
            return -W_EFAULT;

        storage = kmalloc(MAX_ARGS * (W_PATH_MAX + 1));
        if (!storage)
            return -W_ENOMEM;

        const uint32_t *user_argv = (const uint32_t *)argv;
        while (argc < MAX_ARGS) {
            if (!user_range_ok(&user_argv[argc], sizeof(uint32_t), false)) {
                kfree(storage);
                return -W_EFAULT;
            }
            if (user_argv[argc] == 0)
                break;

            char *slot = storage + (uint32_t)argc * (W_PATH_MAX + 1);
            r = copy_string_from_user((const char *)user_argv[argc],
                                      slot, W_PATH_MAX + 1);
            if (r < 0) {
                kfree(storage);
                return r;
            }

            args[argc] = slot;
            argc++;
        }
    }
    args[argc] = NULL;

    int32_t pid = proc_spawn(pathbuf, args, proc_current());

    if (storage)
        kfree(storage);

    return pid;
}

static int32_t sys_wait(uint32_t pid, uint32_t status_out)
{
    int32_t status = 0;

    if (status_out && !user_range_ok((void *)status_out, sizeof(int32_t), true))
        return -W_EFAULT;

    int32_t r = proc_wait((int32_t)pid, &status);

    if (r >= 0 && status_out)
        *(int32_t *)status_out = status;

    return r;
}

static int32_t sys_getpid(void)
{
    return proc_current()->pid;
}

static int32_t sys_sbrk(uint32_t increment)
{
    return (int32_t)proc_sbrk((int32_t)increment);
}

static int32_t sys_ticks(void)
{
    return (int32_t)pit_ticks();
}

static int32_t sys_yield(void)
{
    sched_yield();
    return 0;
}

static int32_t sys_shutdown(void)
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
    uint32_t n = regs->eax;
    int32_t  r;

    switch (n) {
    case WSYS_EXIT:      r = sys_exit(regs->ebx); break;
    case WSYS_OPEN:      r = sys_open(regs->ebx, regs->ecx); break;
    case WSYS_CLOSE:     r = sys_close(regs->ebx); break;
    case WSYS_READ:      r = sys_read(regs->ebx, regs->ecx, regs->edx); break;
    case WSYS_WRITE:     r = sys_write(regs->ebx, regs->ecx, regs->edx); break;
    case WSYS_LSEEK:     r = sys_lseek(regs->ebx, regs->ecx, regs->edx); break;
    case WSYS_STAT:      r = sys_stat(regs->ebx, regs->ecx); break;
    case WSYS_UNLINK:    r = sys_unlink(regs->ebx); break;
    case WSYS_MKDIR:     r = sys_mkdir(regs->ebx); break;
    case WSYS_RMDIR:     r = sys_rmdir(regs->ebx); break;
    case WSYS_OPENDIR:   r = sys_opendir(regs->ebx); break;
    case WSYS_READDIR:   r = sys_readdir(regs->ebx, regs->ecx); break;
    case WSYS_CHDIR:     r = sys_chdir(regs->ebx); break;
    case WSYS_GETCWD:    r = sys_getcwd(regs->ebx, regs->ecx); break;
    case WSYS_MEMINFO:   r = sys_meminfo(regs->ebx); break;
    case WSYS_PROCMEM:   r = sys_procmem(regs->ebx, regs->ecx); break;
    case WSYS_THREADMEM: r = sys_threadmem(regs->ebx, regs->ecx); break;
    case WSYS_PROCLIST:  r = sys_proclist(regs->ebx, regs->ecx); break;
    case WSYS_DISKINFO:  r = sys_diskinfo(regs->ebx); break;
    case WSYS_SPAWN:     r = sys_spawn(regs->ebx, regs->ecx); break;
    case WSYS_WAIT:      r = sys_wait(regs->ebx, regs->ecx); break;
    case WSYS_GETPID:    r = sys_getpid(); break;
    case WSYS_SBRK:      r = sys_sbrk(regs->ebx); break;
    case WSYS_TICKS:     r = sys_ticks(); break;
    case WSYS_YIELD:     r = sys_yield(); break;
    case WSYS_SHUTDOWN:  r = sys_shutdown(); break;
    default:             r = -W_ENOSYS; break;
    }

    regs->eax = (uint32_t)r;
}

void syscall_init(void)
{
    register_interrupt_handler(INT_SYSCALL, syscall_handler);
}
