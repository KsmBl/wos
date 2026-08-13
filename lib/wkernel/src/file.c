/* File, directory and process syscall wrappers. */

#include "wkernel.h"
#include "syscall.h"

int wopen(const char *path, int flags)
{
    return wsyscall2(WSYS_OPEN, (long)path, flags);
}

int wclose(int fd)
{
    return wsyscall1(WSYS_CLOSE, fd);
}

int wread(int fd, void *buf, wsize_t count)
{
    return wsyscall3(WSYS_READ, fd, (long)buf, (long)count);
}

int wwrite(int fd, const void *buf, wsize_t count)
{
    return wsyscall3(WSYS_WRITE, fd, (long)buf, (long)count);
}

int wlseek(int fd, int offset, int whence)
{
    return wsyscall3(WSYS_LSEEK, fd, offset, whence);
}

int wstat(const char *path, wstat_t *out)
{
    return wsyscall2(WSYS_STAT, (long)path, (long)out);
}

int wutime(const char *path)
{
    return wsyscall1(WSYS_UTIME, (long)path);
}

int wunlink(const char *path)
{
    return wsyscall1(WSYS_UNLINK, (long)path);
}

int wrename(const char *from, const char *to)
{
    return wsyscall2(WSYS_RENAME, (long)from, (long)to);
}

int wopendir(const char *path)
{
    return wsyscall1(WSYS_OPENDIR, (long)path);
}

int wreaddir(int fd, wdirent_t *out)
{
    return wsyscall2(WSYS_READDIR, fd, (long)out);
}

int wclosedir(int fd)
{
    return wsyscall1(WSYS_CLOSE, fd);
}

int wmkdir(const char *path)
{
    return wsyscall1(WSYS_MKDIR, (long)path);
}

int wrmdir(const char *path)
{
    return wsyscall1(WSYS_RMDIR, (long)path);
}

int wchdir(const char *path)
{
    return wsyscall1(WSYS_CHDIR, (long)path);
}

int wgetcwd(char *buf, wsize_t size)
{
    return wsyscall2(WSYS_GETCWD, (long)buf, (long)size);
}
