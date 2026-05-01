/* Memory, disk and process syscall wrappers. */

#include "wkernel.h"
#include "syscall.h"

int wmeminfo(wmeminfo_t *out)
{
    return wsyscall1(WSYS_MEMINFO, (long)out);
}

int wprocmem(int pid, wprocmem_t *out)
{
    return wsyscall2(WSYS_PROCMEM, pid, (long)out);
}

int wthreadmem(int tid, wthreadmem_t *out)
{
    return wsyscall2(WSYS_THREADMEM, tid, (long)out);
}

int wproclist(wprocmem_t *out, int max)
{
    return wsyscall2(WSYS_PROCLIST, (long)out, max);
}

int wdiskinfo(wdiskinfo_t *out)
{
    return wsyscall1(WSYS_DISKINFO, (long)out);
}

int wspawn(const char *path, char *const argv[])
{
    return wsyscall2(WSYS_SPAWN, (long)path, (long)argv);
}

int wwait(int pid, int *status)
{
    return wsyscall2(WSYS_WAIT, pid, (long)status);
}

int wpipe(int fds[2])
{
    return wsyscall1(WSYS_PIPE, (long)fds);
}

int wspawn_io(const char *path, char *const argv[], const wspawnio_t *io)
{
    return wsyscall3(WSYS_SPAWN_IO, (long)path, (long)argv, (long)io);
}

int wconsize(int *rows, int *cols)
{
    return wsyscall2(WSYS_CONSIZE, (long)rows, (long)cols);
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

int wconsole_raw(int mode)
{
    return wsyscall1(WSYS_CONSOLE, mode);
}

int wpollin(int fd)
{
    return wsyscall1(WSYS_POLLIN, fd);
}

int wgetuid(void)
{
    return (int)wsyscall0(WSYS_GETUID);
}

int wuserinfo(int uid, wuser_t *out)
{
    return (int)wsyscall2(WSYS_USERINFO, uid, (long)out);
}

int wuserlist(wuser_t *out, int max)
{
    return (int)wsyscall2(WSYS_USERLIST, (long)out, max);
}

int wlogin(const char *name, const char *password)
{
    return (int)wsyscall2(WSYS_LOGIN, (long)name, (long)password);
}

int wpasswd(const char *name, const char *old_password,
            const char *new_password)
{
    return (int)wsyscall3(WSYS_PASSWD, (long)name, (long)old_password,
                          (long)new_password);
}

int wuseradd(const char *name, const char *password, unsigned int roles)
{
    return (int)wsyscall3(WSYS_USERADD, (long)name, (long)password,
                          (long)roles);
}

int wsetroles(const char *name, unsigned int roles)
{
    return (int)wsyscall2(WSYS_SETROLES, (long)name, (long)roles);
}

int wshutdown(void)
{
    /* Only comes back if the kernel could not power the machine off, and even
     * then the kernel halts rather than returning, so this is effectively
     * unreachable. */
    return wsyscall0(WSYS_SHUTDOWN);
}
