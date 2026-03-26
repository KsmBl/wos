/* File, directory and process syscall wrappers. */

#include "wkernel.h"
#include "syscall.h"

int wopen(const char *path, int flags)
{
    return wsyscall2(WSYS_OPEN, (int)path, flags);
}

int wclose(int fd)
{
    return wsyscall1(WSYS_CLOSE, fd);
}

int wread(int fd, void *buf, wsize_t count)
{
    return wsyscall3(WSYS_READ, fd, (int)buf, (int)count);
}

int wwrite(int fd, const void *buf, wsize_t count)
{
    return wsyscall3(WSYS_WRITE, fd, (int)buf, (int)count);
}

int wlseek(int fd, int offset, int whence)
{
    return wsyscall3(WSYS_LSEEK, fd, offset, whence);
}

int wstat(const char *path, wstat_t *out)
{
    return wsyscall2(WSYS_STAT, (int)path, (int)out);
}

int wunlink(const char *path)
{
    return wsyscall1(WSYS_UNLINK, (int)path);
}

int wopendir(const char *path)
{
    return wsyscall1(WSYS_OPENDIR, (int)path);
}

int wreaddir(int fd, wdirent_t *out)
{
    return wsyscall2(WSYS_READDIR, fd, (int)out);
}

int wclosedir(int fd)
{
    return wsyscall1(WSYS_CLOSE, fd);
}

int wmkdir(const char *path)
{
    return wsyscall1(WSYS_MKDIR, (int)path);
}

int wrmdir(const char *path)
{
    return wsyscall1(WSYS_RMDIR, (int)path);
}

int wchdir(const char *path)
{
    return wsyscall1(WSYS_CHDIR, (int)path);
}

int wgetcwd(char *buf, wsize_t size)
{
    return wsyscall2(WSYS_GETCWD, (int)buf, (int)size);
}
