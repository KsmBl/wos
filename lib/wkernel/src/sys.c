/* Memory, disk and process syscall wrappers. */

#include "wkernel.h"
#include "syscall.h"

int wmeminfo(wmeminfo_t *out)
{
    return wsyscall1(WSYS_MEMINFO, (int)out);
}

int wprocmem(int pid, wprocmem_t *out)
{
    return wsyscall2(WSYS_PROCMEM, pid, (int)out);
}

int wthreadmem(int tid, wthreadmem_t *out)
{
    return wsyscall2(WSYS_THREADMEM, tid, (int)out);
}

int wproclist(wprocmem_t *out, int max)
{
    return wsyscall2(WSYS_PROCLIST, (int)out, max);
}

int wdiskinfo(wdiskinfo_t *out)
{
    return wsyscall1(WSYS_DISKINFO, (int)out);
}

int wspawn(const char *path, char *const argv[])
{
    return wsyscall2(WSYS_SPAWN, (int)path, (int)argv);
}

int wwait(int pid, int *status)
{
    return wsyscall2(WSYS_WAIT, pid, (int)status);
}

void wexit(int status)
{
    wsyscall1(WSYS_EXIT, status);

    /* The kernel never returns from exit, but the compiler needs to see that
     * this function cannot fall through. */
    for (;;)
        ;
}

int wgetpid(void)
{
    return wsyscall0(WSYS_GETPID);
}

void *wsbrk(int increment)
{
    return (void *)wsyscall1(WSYS_SBRK, increment);
}

unsigned int wticks(void)
{
    return (unsigned int)wsyscall0(WSYS_TICKS);
}

unsigned int wuptime_ms(void)
{
    /* The timer runs at 100 Hz. */
    return wticks() * 10u;
}

void wyield(void)
{
    wsyscall0(WSYS_YIELD);
}
