/* The file descriptor layer.
 *
 * Sits between the syscalls and WFS: it owns per-process descriptor tables,
 * resolves relative paths against the working directory, and routes
 * descriptors 0, 1 and 2 to the console instead of the disk.
 */
#ifndef WOS_VFS_H
#define WOS_VFS_H

#include "types.h"
#include "wabi.h"

#define MAX_OPEN_FILES 32

typedef enum {
    FD_NONE = 0,
    FD_FILE,
    FD_DIR,
    FD_CONSOLE,
    FD_PIPE
} fd_type_t;

struct pipe;

typedef struct {
    fd_type_t type;
    uint32_t  ino;
    bool      ram;         /* the inode is /ramdisk's, not the disk's      */
    uint32_t  offset;      /* byte offset for files, entry index for dirs */
    uint32_t  flags;       /* the W_O_* flags it was opened with          */
    struct pipe *pipe;     /* the pipe object, when type == FD_PIPE       */
    bool      write_end;   /* which end of that pipe this descriptor is   */
} file_t;

struct process;

/* Create a pipe and install its two ends in `p`, returning the read descriptor
 * in out[0] and the write descriptor in out[1].  Returns 0 or a negative
 * W_E* code. */
int vfs_pipe(struct process *p, int out[2]);

/* Give a new process the standard three console descriptors. */
void vfs_init_fds(struct process *p);

/* Release every descriptor a process holds. */
void vfs_close_all(struct process *p);

/* True if the process reads standard input from the console, not a pipe. */
bool vfs_stdin_is_console(struct process *p);

/* Copy the parent's stdin/stdout/stderr into a child, so redirected output is
 * inherited across a spawn.  Refs any pipe ends it copies. */
void vfs_inherit_stdio(struct process *child, struct process *parent);

/* Turn `path` into a normalised absolute path, resolving it against the
 * process's working directory and collapsing "." and "..".
 * Returns 0 or a negative W_E* code. */
int vfs_resolve(struct process *p, const char *path, char *out, size_t out_size);

/* The syscall implementations. All operate on the calling process and return
 * either a count / descriptor, or a negative W_E* code. */
int vfs_open(struct process *p, const char *path, uint32_t flags);
int vfs_close(struct process *p, int fd);
int vfs_read(struct process *p, int fd, void *buf, uint32_t len);
int vfs_write(struct process *p, int fd, const void *buf, uint32_t len);
int vfs_lseek(struct process *p, int fd, int32_t offset, int whence);
int vfs_stat(struct process *p, const char *path, wstat_t *out);

/* Describe every mounted filesystem, up to `max` of them.  Returns how many
 * were written.  Needs no process: what is mounted is the same for everyone. */
int vfs_disklist(wdisk_t *out, int max);
int vfs_unlink(struct process *p, const char *path);
int vfs_mkdir(struct process *p, const char *path);
int vfs_rmdir(struct process *p, const char *path);
int vfs_opendir(struct process *p, const char *path);
int vfs_readdir(struct process *p, int fd, wdirent_t *out);
int vfs_chdir(struct process *p, const char *path);
int vfs_getcwd(struct process *p, char *buf, uint32_t size);

/* Read a whole file into a freshly kmalloc'd buffer.  The caller owns the
 * buffer and must kfree it.  Used by the program loader. */
int vfs_read_file(struct process *p, const char *path, void **data_out,
                  uint64_t *size_out);

#endif /* WOS_VFS_H */
