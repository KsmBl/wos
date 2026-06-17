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

int wdisklist(wdisk_t *out, int max)
{
    return wsyscall2(WSYS_DISKLIST, (long)out, max);
}

int wcpuinfo(wcpuinfo_t *out)
{
    return wsyscall1(WSYS_CPUINFO, (long)out);
}

int wcpulist(wcpu_t *out, int max)
{
    return wsyscall2(WSYS_CPULIST, (long)out, max);
}

int wservicelist(wservice_t *out, int max)
{
    return (int)wsyscall2(WSYS_SVCLIST, (long)out, max);
}

int wservicectl(int action, const char *name)
{
    return (int)wsyscall2(WSYS_SVCCTL, action, (long)name);
}

int wlisten(const char *path)
{
    return (int)wsyscall1(WSYS_LISTEN, (long)path);
}

int wconnect(const char *path)
{
    return (int)wsyscall1(WSYS_CONNECT, (long)path);
}

int waccept(int fd)
{
    return (int)wsyscall1(WSYS_ACCEPT, fd);
}

int wsend(int fd, wmsg_t *msg)
{
    return (int)wsyscall2(WSYS_SEND, fd, (long)msg);
}

int wrecv(int fd, wmsg_t *msg)
{
    return (int)wsyscall2(WSYS_RECV, fd, (long)msg);
}

int wpoll(wpollfd_t *fds, int count, int timeout_ms)
{
    return (int)wsyscall3(WSYS_POLL, (long)fds, count, timeout_ms);
}

int wbattery(wbattery_t *out)
{
    return wsyscall1(WSYS_BATTERY, (long)out);
}

int wcpufreq(int khz)
{
    return wsyscall1(WSYS_CPUFREQ, khz);
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

int wsetsize(int pid, int rows, int cols)
{
    return wsyscall3(WSYS_SETSIZE, pid, rows, cols);
}

int wsetmode(int cols, int rows)
{
    return wsyscall2(WSYS_SETMODE, cols, rows);
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

void wsleep(int ms)
{
    wsyscall1(WSYS_SLEEP, ms);
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

int wgetshell(int uid, char *buf, int size)
{
    return (int)wsyscall3(WSYS_GETSHELL, uid, (long)buf, size);
}

int wsetshell(const char *name, const char *shell)
{
    return (int)wsyscall2(WSYS_SETSHELL, (long)name, (long)shell);
}

int wping(unsigned int ip, int seq, int timeout_ms)
{
    return (int)wsyscall3(WSYS_PING, (long)ip, seq, timeout_ms);
}

int wresolve(const char *host, unsigned int *ip)
{
    return (int)wsyscall2(WSYS_RESOLVE, (long)host, (long)ip);
}

int wtcp_open(unsigned int ip, int port)
{
    return (int)wsyscall2(WSYS_TCP_OPEN, (long)ip, port);
}

int wtcp_send(int handle, const void *data, int len)
{
    return (int)wsyscall3(WSYS_TCP_SEND, handle, (long)data, len);
}

int wtcp_recv(int handle, void *buf, int len)
{
    return (int)wsyscall3(WSYS_TCP_RECV, handle, (long)buf, len);
}

void wtcp_close(int handle)
{
    wsyscall1(WSYS_TCP_CLOSE, handle);
}

int wtime_get(wtime_t *out)
{
    return (int)wsyscall1(WSYS_TIME_GET, (long)out);
}

int wtime_set(const wtime_t *t)
{
    return (int)wsyscall1(WSYS_TIME_SET, (long)t);
}

int wshutdown(void)
{
    /* Only comes back if the kernel could not power the machine off, and even
     * then the kernel halts rather than returning, so this is effectively
     * unreachable. */
    return wsyscall0(WSYS_SHUTDOWN);
}
