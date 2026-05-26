/**
 * @file wkernel.h
 * @brief The WOS application interface.
 *
 * This is the only header a WOS application needs to include:
 *
 * @code
 *     #include <wkernel.h>
 *
 *     int main(int argc, char **argv)
 *     {
 *         wprintf("hello from pid %d\n", wgetpid());
 *         return 0;
 *     }
 * @endcode
 *
 * Every function below is documented with what it does, what each parameter
 * means, and exactly what it returns.  The conventions are uniform:
 *
 * - Functions that can fail return a negative error code, one of the `W_E*`
 *   constants negated.  So `if (r < 0)` detects failure and `-r` names the
 *   reason.  Functions returning a count or a descriptor return it directly
 *   when they succeed.
 * - Pointers passed to the kernel are validated; passing a bad one fails with
 *   `-W_EFAULT` rather than corrupting anything.
 * - Paths may be absolute or relative to the working directory, and "." and
 *   ".." are resolved before the filesystem sees them.
 *
 * The full reference, with worked examples, is in docs/wkernel-api.md.
 */
#ifndef WKERNEL_H
#define WKERNEL_H

#include "wabi.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* LP64: pointers and sizes are 64-bit. */
typedef unsigned long wsize_t;

/* ==================================================================== *
 *  Files
 * ==================================================================== */

/**
 * Open a file, optionally creating it.
 *
 * @param path  File to open. Absolute, or relative to the working directory.
 * @param flags Exactly one access mode -- #W_O_RDONLY, #W_O_WRONLY or
 *              #W_O_RDWR -- optionally ORed with #W_O_CREAT (create it if it
 *              does not exist), #W_O_TRUNC (discard existing contents) and
 *              #W_O_APPEND (every write goes to the end of the file).
 *
 * @return A file descriptor (always >= 3) on success, or a negative error:
 *         `-W_ENOENT` if the file does not exist and #W_O_CREAT was not given,
 *         `-W_ENOTDIR` if a component of the path is not a directory,
 *         `-W_EISDIR` if the path names a directory and write access was
 *         requested, `-W_EMFILE` if this process has no free descriptors,
 *         `-W_ENOSPC` if the file had to be created and the disk is full.
 *
 * @note Descriptors 0, 1 and 2 are already open on the console.
 *
 * @code
 *     int fd = wopen("/home/notes.txt", W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
 *     if (fd < 0) {
 *         wprintf("cannot open: %s\n", wstrerror(-fd));
 *         return 1;
 *     }
 * @endcode
 */
int wopen(const char *path, int flags);

/**
 * Close a file descriptor.
 *
 * @param fd Descriptor from wopen() or wopendir().
 * @return 0 on success, or `-W_EBADF` if the descriptor is not open.
 */
int wclose(int fd);

/**
 * Read bytes from a file or from the console.
 *
 * On descriptor 0 this reads from the keyboard and blocks until the user
 * presses Enter, then returns the whole line including its trailing newline.
 *
 * @param fd    Descriptor to read from.
 * @param buf   Buffer to fill. Must be writable and at least @p count bytes.
 * @param count Maximum number of bytes to read.
 *
 * @return The number of bytes actually read, which may be less than @p count
 *         near the end of a file; 0 means end of file.  On failure:
 *         `-W_EBADF` for a bad descriptor, `-W_EISDIR` if @p fd is a
 *         directory (use wreaddir()), `-W_EACCES` if it was opened write-only,
 *         `-W_EFAULT` if @p buf is not valid writable memory.
 */
int wread(int fd, void *buf, wsize_t count);

/**
 * Write bytes to a file or to the console.
 *
 * @param fd    Descriptor to write to. 1 is stdout, 2 is stderr.
 * @param buf   Data to write.
 * @param count Number of bytes to write.
 *
 * @return The number of bytes written -- short only if the disk filled up --
 *         or a negative error: `-W_EBADF`, `-W_EISDIR`, `-W_EACCES` if the
 *         file was opened read-only, `-W_EFAULT` for an unreadable buffer,
 *         `-W_ENOSPC` if nothing could be written because the disk is full,
 *         `-W_EFBIG` if the write would push the file past its maximum size.
 */
int wwrite(int fd, const void *buf, wsize_t count);

/**
 * Move the read/write position of an open file.
 *
 * @param fd     Descriptor to reposition.
 * @param offset Byte offset, interpreted according to @p whence. May be
 *               negative for #W_SEEK_CUR and #W_SEEK_END.
 * @param whence #W_SEEK_SET (from the start), #W_SEEK_CUR (from the current
 *               position) or #W_SEEK_END (from the end of the file).
 *
 * @return The new absolute position, or a negative error: `-W_EBADF`,
 *         `-W_EINVAL` if @p whence is not valid or the result would be
 *         negative, `-W_ESPIPE` if @p fd is the console, which cannot seek.
 *
 * @note Seeking past the end is allowed; writing there leaves a hole that
 *       reads back as zeroes.
 */
int wlseek(int fd, int offset, int whence);

/**
 * Look up a file's metadata by path, without opening it.
 *
 * @param path Path to inspect.
 * @param out  Filled in with the inode number, size in bytes, number of disk
 *             blocks used, and type (#W_FT_FILE or #W_FT_DIR).
 *
 * @return 0 on success, or `-W_ENOENT`, `-W_ENOTDIR` or `-W_EFAULT`.
 */
int wstat(const char *path, wstat_t *out);

/**
 * Delete a file.
 *
 * @param path File to remove. Use wrmdir() for directories.
 * @return 0 on success, `-W_ENOENT` if it does not exist, `-W_EISDIR` if it
 *         is a directory.
 */
int wunlink(const char *path);

/* ==================================================================== *
 *  Directories
 * ==================================================================== */

/**
 * Open a directory for reading with wreaddir().
 *
 * @param path Directory to open.
 * @return A descriptor on success, or `-W_ENOENT` if it does not exist,
 *         `-W_ENOTDIR` if @p path is not a directory, `-W_EMFILE` if this
 *         process has no free descriptors.
 */
int wopendir(const char *path);

/**
 * Read the next entry from a directory opened by wopendir().
 *
 * Entries come back one per call, including "." and "..".  The position
 * advances only when an entry is actually produced.
 *
 * @param fd  Descriptor from wopendir().
 * @param out Filled in with the entry's inode number, type and name.
 *
 * @return 1 when an entry was written to @p out, 0 when the end of the
 *         directory has been reached, or a negative error: `-W_EBADF`,
 *         `-W_ENOTDIR` if @p fd is not a directory, `-W_EFAULT`.
 *
 * @code
 *     int d = wopendir("/app");
 *     wdirent_t e;
 *     while (wreaddir(d, &e) == 1)
 *         wprintf("%s%s\n", e.name, e.type == W_FT_DIR ? "/" : "");
 *     wclosedir(d);
 * @endcode
 */
int wreaddir(int fd, wdirent_t *out);

/**
 * Close a directory descriptor. Identical to wclose(); it exists so that
 * directory code reads symmetrically.
 *
 * @param fd Descriptor from wopendir().
 * @return 0 on success, `-W_EBADF` otherwise.
 */
int wclosedir(int fd);

/**
 * Create a directory.
 *
 * @param path Directory to create. Its parent must already exist.
 * @return 0 on success, `-W_EEXIST` if the name is taken, `-W_ENOENT` if the
 *         parent does not exist, `-W_ENOSPC` if the disk is full.
 */
int wmkdir(const char *path);

/**
 * Remove an empty directory.
 *
 * @param path Directory to remove.
 * @return 0 on success, `-W_ENOTDIR` if it is not a directory,
 *         `-W_ENOTEMPTY` if it still contains entries, `-W_ENOENT` if it does
 *         not exist.
 */
int wrmdir(const char *path);

/**
 * Change the working directory of this process.
 *
 * @param path Directory to move to.
 * @return 0 on success, `-W_ENOENT` or `-W_ENOTDIR` on failure.
 */
int wchdir(const char *path);

/**
 * Get the working directory as an absolute, normalised path.
 *
 * @param buf  Buffer to fill.
 * @param size Size of @p buf. #W_PATH_MAX + 1 is always enough.
 * @return The length of the path written (not counting the NUL), or
 *         `-W_ERANGE` if @p buf is too small, or `-W_EFAULT`.
 */
int wgetcwd(char *buf, wsize_t size);

/* ==================================================================== *
 *  Memory statistics
 * ==================================================================== */

/**
 * Report system-wide memory use.
 *
 * The numbers come from the kernel's physical frame allocator, so they are
 * measured rather than estimated: `used_bytes` is exactly the number of 4 KiB
 * frames currently allocated, and `used + free == total`.
 *
 * @param out Filled in with total, used and free bytes of usable RAM, the
 *            bytes held by the kernel itself (its image, heap arena and page
 *            tables), and the page size.
 * @return 0 on success, or `-W_EFAULT` if @p out is not writable.
 *
 * @code
 *     wmeminfo_t m;
 *     wmeminfo(&m);
 *     wprintf("%u KiB free of %u KiB\n", m.free_bytes / 1024,
 *             m.total_bytes / 1024);
 * @endcode
 */
int wmeminfo(wmeminfo_t *out);

/**
 * Report the memory use of one process.
 *
 * `resident_bytes` is the number of frames actually mapped into that
 * process's address space, counted from its page tables -- what the process
 * genuinely occupies, not a reservation.
 *
 * @param pid Process to inspect, or 0 for the calling process.
 * @param out Filled in with the pid, name, resident bytes, and the code,
 *            data, heap and stack breakdown, plus the thread count.
 * @return 0 on success, `-W_ESRCH` if there is no such process, `-W_EFAULT`
 *         if @p out is not writable.
 */
int wprocmem(int pid, wprocmem_t *out);

/**
 * Report the memory use of one thread.
 *
 * @param tid Thread to inspect, or 0 for the calling thread.
 * @param out Filled in with the thread and process ids, its kernel and user
 *            stack sizes, and how many timer ticks it has run for.
 * @return 0 on success, `-W_ESRCH` if there is no such thread, `-W_EFAULT`
 *         if @p out is not writable.
 *
 * @note Every process currently has exactly one thread, so only the calling
 *       thread is addressable.
 */
int wthreadmem(int tid, wthreadmem_t *out);

/**
 * List every running process and its memory use.
 *
 * @param out Array to fill, of at least @p max entries.
 * @param max Maximum number of entries to write.
 * @return The number of processes written, or `-W_EFAULT`.
 *
 * @code
 *     wprocmem_t procs[16];
 *     int n = wproclist(procs, 16);
 *     for (int i = 0; i < n; i++)
 *         wprintf("%-12s %u KiB\n", procs[i].name,
 *                 procs[i].resident_bytes / 1024);
 * @endcode
 */
int wproclist(wprocmem_t *out, int max);

/* ==================================================================== *
 *  Disk statistics
 * ==================================================================== */

/**
 * Report disk space use.
 *
 * The figures come from the filesystem's block bitmap, so `used_bytes` counts
 * blocks that are genuinely allocated, including the metadata the filesystem
 * needs for itself.
 *
 * @param out Filled in with total, used and free bytes, the block size, the
 *            total and free block counts, and the total and free inode counts.
 * @return 0 on success, or `-W_EFAULT`.
 *
 * @note If no disk is mounted every field is zero.
 */
int wdiskinfo(wdiskinfo_t *out);

/* ==================================================================== *
 *  Processes
 * ==================================================================== */

/**
 * Load and run a program as a child process.
 *
 * The child gets a copy of the caller's working directory and its own console
 * descriptors.  It runs concurrently: use wwait() to block until it finishes.
 *
 * @param path Executable to run, e.g. "/app/whell/launch".
 * @param argv NULL-terminated argument array; `argv[0]` should be the program
 *             name.  May be NULL for no arguments.
 *
 * @return The child's process id (always > 0), or a negative error:
 *         `-W_ENOENT` if the file does not exist, `-W_ENOEXEC` if it is not a
 *         valid 32-bit x86 executable, `-W_ENOMEM` if memory or the process
 *         table is exhausted, `-W_EFAULT` for a bad pointer.
 *
 * @code
 *     char *argv[] = { "ls", "-l", NULL };
 *     int pid = wspawn("/app/ls/launch", argv);
 *     int status;
 *     if (pid > 0)
 *         wwait(pid, &status);
 * @endcode
 */
int wspawn(const char *path, char *const argv[]);

/**
 * Create a pipe: a one-way byte stream between two descriptors.
 *
 * `fds[0]` is the read end and `fds[1]` the write end.  Bytes written to the
 * write end are read, in order, from the read end.  A read blocks while the
 * pipe is empty and a writer still exists, and returns 0 once every writer has
 * closed.  A write blocks while the pipe is full, and fails with `-W_EPIPE`
 * once every reader has closed.
 *
 * Together with wspawn_io() this is how a program runs another inside a window
 * of its own, the way vim's :term runs a shell.
 *
 * @param fds Receives the read and write descriptors.
 * @return 0, or a negative error (`-W_EMFILE`, `-W_ENFILE`, `-W_EFAULT`).
 */
int wpipe(int fds[2]);

/**
 * Spawn a program with its standard descriptors wired to pipes.
 *
 * Like wspawn(), but the child's stdin reads from `io->in_fd` (a pipe read end
 * in the caller) and its stdout and stderr write to `io->out_fd` (a pipe write
 * end).  The child reports `io->rows` by `io->cols` from wconsize(), so a
 * full-screen program can size itself to the window it was given.
 *
 * After spawning, close your copies of the child's ends: only then does the
 * read end see end of file when the child exits.
 *
 * @param path Executable to run.
 * @param argv NULL-terminated argument array.
 * @param io   Descriptor wiring and terminal size.
 * @return The child's pid, or a negative error.
 *
 * @code
 *     int in[2], out[2];
 *     wpipe(in); wpipe(out);
 *     wspawnio_t io = { in[0], out[1], rows, cols };
 *     int pid = wspawn_io("/app/whell/launch", argv, &io);
 *     wclose(in[0]); wclose(out[1]);   // keep in[1] to send, out[0] to receive
 * @endcode
 */
int wspawn_io(const char *path, char *const argv[], const wspawnio_t *io);

/**
 * Report the size of the terminal this process draws to.
 *
 * The console is a fixed 80x25; a program started by wspawn_io() is told the
 * size of the window it was given instead.  Both pointers are required.
 *
 * @param rows Receives the number of rows.
 * @param cols Receives the number of columns.
 * @return 0, or `-W_EFAULT` if a pointer is not writable.
 */
int wconsize(int *rows, int *cols);

/**
 * Set the terminal size reported to one of your own child processes.
 *
 * Affects what wconsize() returns for that child and, through inheritance, for
 * anything the child later spawns.  Used when a window is resized -- `split`
 * calls it after collapsing to a single full-screen terminal -- so a program
 * started afterwards lays itself out to the new size.  You may only set a
 * process you are the parent of.
 *
 * @param pid  A child of the calling process.
 * @param rows New row count (ignored if <= 0).
 * @param cols New column count (ignored if <= 0).
 * @return 0, `-W_ESRCH` if there is no such process, or `-W_EPERM` if it is
 *         not your child.
 */
int wsetsize(int pid, int rows, int cols);

/**
 * Change the console's text mode to `cols` by `rows` characters.
 *
 * The supported modes are 80x25, 80x50, 80x30, 80x60, 40x25 and 40x50 -- VGA
 * text modes are not arbitrary, so other sizes are refused.  After the switch
 * the screen is cleared and wconsize() reports the new size, which full-screen
 * programs read to lay themselves out.
 *
 * Only a program attached to the real console may do this; one running in a
 * pipe or a window gets `-W_EPERM`.
 *
 * @param cols Character columns (40 or 80).
 * @param rows Character rows (25, 30, 50 or 60, as the mode allows).
 * @return 0, `-W_EINVAL` for an unsupported size, or `-W_EPERM`.
 */
int wsetmode(int cols, int rows);

/**
 * Wait for a child process to exit and clean it up.
 *
 * Blocks until a matching child has exited.  Until a child is waited for, its
 * process slot is not released.
 *
 * @param pid    Child to wait for, or -1 for any child.
 * @param status If not NULL, receives the child's exit status. A process
 *               killed by a fault reports -1.
 *
 * @return The pid of the child that was reaped, or `-W_ECHILD` if there is no
 *         such child, or `-W_EFAULT` if @p status is not writable.
 */
int wwait(int pid, int *status);

/**
 * Terminate the calling process immediately. Does not return.
 *
 * Open descriptors are closed and the address space is released; the parent
 * sees @p status from wwait().  Returning from main() has the same effect,
 * with main's return value as the status.
 *
 * @param status Exit status to report to the parent.
 */
void wexit(int status) __attribute__((noreturn));

/**
 * @return The calling process's id. Never fails.
 */
int wgetpid(void);

/**
 * Grow or shrink the process heap.
 *
 * This is the primitive malloc() is built on; most programs should use
 * malloc() instead.
 *
 * @param increment Bytes to add to the heap; may be negative to give memory
 *                  back, or 0 to query the current break.
 * @return The address of the *previous* end of the heap, so
 *         `wsbrk(n)` returns a pointer to @p n newly usable bytes.
 *         Returns `(void *)-1` if the request cannot be satisfied.
 *
 * @note New pages are zero-filled.
 */
void *wsbrk(int increment);

/**
 * @return Timer ticks since boot. The timer runs at 100 Hz, so one tick is
 *         10 ms. Never fails.
 */
unsigned int wticks(void);

/**
 * @return Milliseconds since boot. Never fails.
 */
unsigned int wuptime_ms(void);

/**
 * Give up the rest of this timeslice to another runnable process.
 * Purely an optimisation; the scheduler preempts anyway.
 */
void wyield(void);

/**
 * Switch the console between line-buffered and raw input.
 *
 * In the default canonical mode the kernel echoes as you type, handles
 * backspace, and a wread() on descriptor 0 returns one whole line once Enter
 * is pressed.
 *
 * In raw mode every keystroke is readable immediately and nothing is echoed,
 * so a program that wants to react to individual keys -- Tab, arrow keys, a
 * pager waiting for a single letter -- can do so.  In exchange it must echo
 * what it wants seen and implement its own editing.  Ctrl+letter arrives as
 * the corresponding control code, so Ctrl+C is 0x03.
 *
 * Switching modes discards anything typed but not yet submitted.
 *
 * @param mode #W_CONSOLE_CANONICAL or #W_CONSOLE_RAW.
 * @return The mode that was in effect before, or `-W_EINVAL`.
 *
 * @note The mode is a property of the console, not of the process, so a
 *       program that switches to raw should switch back before it exits or
 *       spawns a child that reads input.
 *
 * @code
 *     wconsole_raw(W_CONSOLE_RAW);
 *     char c;
 *     wread(W_STDIN, &c, 1);        // returns as soon as a key is pressed
 *     wconsole_raw(W_CONSOLE_CANONICAL);
 * @endcode
 */
int wconsole_raw(int mode);

/**
 * Ask whether a read would return immediately.
 *
 * This is what lets a program stay responsive while doing something else:
 * a display that refreshes on a timer can check for a keypress between
 * repaints instead of blocking in wread() and freezing until one arrives.
 *
 * @param fd Descriptor to test.
 * @return 1 if a wread() on @p fd would not block, 0 if it would.
 *         Only the console can ever block, so any file reports 1.
 *
 * @code
 *     while (running) {
 *         redraw();
 *         unsigned until = wticks() + 100;      // 1 second
 *         while (wticks() < until) {
 *             if (wpollin(W_STDIN)) { handle_key(); break; }
 *             wyield();
 *         }
 *     }
 * @endcode
 */
int wpollin(int fd);

/* ==================================================================== *
 *  Users and permissions
 *
 *  Every process runs as a user.  Root is uid 0 and bypasses every check;
 *  anyone else needs the matching role, and may write only inside their own
 *  home directory (plus /app, with W_ROLE_APPEDITOR).
 *
 *  Password hashes are never readable from a program: the kernel owns the
 *  database and does the checking, which is why none of these calls needs
 *  anything like setuid.
 * ==================================================================== */

/**
 * @return The uid this process runs as. Cannot fail.
 */
int wgetuid(void);

/**
 * Look a user up by id.
 *
 * @param uid User to look up, or -1 for the calling process's own user.
 * @param out Filled in with the uid, the role bitmask and the name.
 * @return 0, `-W_ENOENT` if there is no such user, or `-W_EFAULT`.
 */
int wuserinfo(int uid, wuser_t *out);

/**
 * List every user on the system.
 *
 * @param out Array to fill, of at least @p max entries.
 * @param max Maximum number of entries to write.
 * @return The number written, or `-W_EFAULT`.
 */
int wuserlist(wuser_t *out, int max);

/**
 * Check a password and, if it matches, become that user.
 *
 * The check happens in the kernel, so a wrong password cannot be told from a
 * right one by anything the caller can observe except the return value.
 *
 * Root may become any user without supplying a password -- that is what being
 * root means. There is no way back: a process that drops to another user
 * cannot return to root, so `su` runs a fresh shell rather than changing the
 * one you are sitting in.
 *
 * @param name     User to become.
 * @param password Their password; ignored when the caller is root.
 * @return The new uid, `-W_ENOENT` if there is no such user, `-W_EACCES` if
 *         the password is wrong, or `-W_EFAULT`.
 */
int wlogin(const char *name, const char *password);

/**
 * Set a user's password.
 *
 * Root and holders of #W_ROLE_USEREDITOR may set anyone's password without
 * knowing the old one. Anyone else may set only their own, and must supply it.
 *
 * @param name         User whose password to change.
 * @param old_password The current password; ignored for a privileged caller,
 *                     and ignored for an account that has none.
 * @param new_password The new password. An empty string clears it, which
 *                     lets that account be entered without one.
 * @return 0, `-W_ENOENT`, `-W_EPERM` if not permitted, `-W_EACCES` if the old
 *         password is wrong, or `-W_EFAULT`.
 */
int wpasswd(const char *name, const char *old_password,
            const char *new_password);

/**
 * Create a user, and their home directory under /home.
 *
 * Only root and holders of #W_ROLE_USEREDITOR may do this.
 *
 * @param name     Name for the new user. May not contain '/', ':', '.' or a
 *                 newline, since it becomes part of a path.
 * @param password Their initial password; empty means none.
 * @param roles    Bitmask of `W_ROLE_*` values.
 * @return The new uid, `-W_EPERM` if not permitted, `-W_EEXIST` if the name is
 *         taken, `-W_EINVAL` for an unusable name, or `-W_ENOSPC`.
 */
int wuseradd(const char *name, const char *password, unsigned int roles);

/**
 * Replace a user's roles outright.
 *
 * Only root and holders of #W_ROLE_USEREDITOR may do this. Root's own roles
 * cannot be changed: every check short-circuits on uid 0, so they carry no
 * meaning and letting them be edited would only suggest otherwise.
 *
 * @param name  User to change.
 * @param roles The complete new bitmask -- this replaces the old set rather
 *              than adding to it, so read the current roles with wuserinfo()
 *              first if you mean to adjust one.
 * @return 0, `-W_EPERM` if not permitted or the target is root, `-W_ENOENT`,
 *         or `-W_EFAULT`.
 */
int wsetroles(const char *name, unsigned int roles);

/**
 * Get a user's login shell -- the program started for them at boot or by su.
 *
 * Always yields a usable path, the system default when the user has set none.
 *
 * @param uid  User to look up, or negative for the calling user.
 * @param buf  Receives the shell path.
 * @param size Size of @p buf.
 * @return 0, or `-W_EFAULT` if @p buf is not writable.
 */
int wgetshell(int uid, char *buf, int size);

/**
 * Set a user's login shell.
 *
 * A user may change their own; root and holders of #W_ROLE_USEREDITOR may
 * change anyone's. The change takes effect the next time a shell is started
 * for that user -- their next su, or the next boot for root. An empty string
 * restores the default shell.
 *
 * @param name  User to change.
 * @param shell Path to the shell executable, e.g. "/app/fish/launch".
 * @return 0, `-W_ENOENT`, `-W_EPERM` if not permitted, `-W_ENAMETOOLONG`, or
 *         `-W_EFAULT`.
 */
int wsetshell(const char *name, const char *shell);

/**
 * Send one ICMP echo request and wait for the reply -- the primitive `ping` is
 * built on.
 *
 * WOS has a small IPv4 stack over an RTL8139 card and QEMU's user-mode
 * network, configured as 10.0.2.15 with the gateway at 10.0.2.2. This resolves
 * the next hop with ARP, sends the echo, and waits up to @p timeout_ms for the
 * matching reply.
 *
 * @param ip         Destination, a network-order address (build it with the
 *                   octets in order: `a | b<<8 | c<<16 | d<<24`).
 * @param seq        Sequence number to put in the echo, echoed back in the
 *                   reply.
 * @param timeout_ms How long to wait for the reply.
 * @return The round-trip time in microseconds (>= 0), or a negative error:
 *         `-W_ENODEV` if there is no network card, `-W_EHOSTUNREACH` if the
 *         next hop could not be resolved, `-W_ETIMEDOUT` if no reply came.
 */
int wping(unsigned int ip, int seq, int timeout_ms);

/**
 * Resolve a host name to a network-order address.
 *
 * A dotted-decimal string is parsed directly; anything else is looked up over
 * DNS. `ip` receives the address in the same network-order form ping and the
 * TCP calls expect.
 *
 * @return 0, `-W_ENODEV`, `-W_EHOSTUNREACH` (no answer) or `-W_EINVAL`.
 */
int wresolve(const char *host, unsigned int *ip);

/**
 * Open a TCP connection to `ip` (network order) on `port`.
 *
 * Blocks through the handshake. WOS's TCP is a small client only -- no
 * listening sockets -- so this is how a program reaches a server.
 *
 * @return A connection handle (>= 0) for the other TCP calls, or a negative
 *         error: `-W_ECONNREFUSED`, `-W_ETIMEDOUT`, `-W_EMFILE`, `-W_ENODEV`.
 */
int wtcp_open(unsigned int ip, int port);

/**
 * Send bytes on a TCP connection. Blocks until they are acknowledged.
 * @return The number sent, or a negative error (`-W_ECONNRESET`, ...).
 */
int wtcp_send(int handle, const void *data, int len);

/**
 * Receive bytes from a TCP connection. Blocks until some arrive.
 * @return The number read, 0 at the peer's end of file, or a negative error.
 */
int wtcp_recv(int handle, void *buf, int len);

/**
 * Close a TCP connection.
 */
void wtcp_close(int handle);

/**
 * Read the wall-clock date and time from the real-time clock.
 * @return 0, or `-W_EFAULT` if @p out is not writable.
 */
int wtime_get(wtime_t *out);

/**
 * Set the wall-clock date and time.  System-wide, so root only.
 * @return 0, `-W_EPERM` if not root, or `-W_EFAULT`.
 */
int wtime_set(const wtime_t *t);

/**
 * The result of an HTTP GET.  `raw` is the whole response and must be freed;
 * `body` and `location` point into it.
 */
typedef struct {
    int   status;              /* HTTP status code, 0 if unparsed        */
    char *raw;                 /* the whole response; free() this        */
    int   raw_len;
    char *body;                /* start of the body, within raw          */
    int   body_len;
    char  location[512];       /* a redirect target, empty if none       */
} whttp_t;

/**
 * Split a URL into host, port and path.  https:// returns -2 (no TLS);
 * malformed returns -1.
 */
int whttp_parse_url(const char *url, char *host, int host_size,
                    int *port, char *path, int path_size);

/**
 * Fetch a URL over HTTP.  Resolves the host, connects, sends a GET and reads
 * the whole response into `out->raw` (which the caller frees).  Returns 0, a
 * negative W_E* code, or -2 for an https URL.
 */
int whttp_get(const char *url, whttp_t *out);

/**
 * Shut the machine down.
 *
 * Does not return when it succeeds -- the machine powers off. Nothing needs
 * to be flushed first: the filesystem writes its metadata straight through,
 * so the disk is consistent at every moment.
 *
 * @return Only on failure, with a negative error code. `-W_ENOSYS` means the
 *         machine offers no soft-off this kernel knows how to drive, in which
 *         case the kernel halts the CPU and says so on the console rather
 *         than returning here.
 *
 * @note There is no user or permission model in WOS, so any process may call
 *       this.
 */
int wshutdown(void);

/* ==================================================================== *
 *  POSIX-style aliases
 *
 *  The same calls under their familiar names, for code that reads more
 *  naturally that way.  They are plain inline forwards, so there is no
 *  cost to using either spelling.
 * ==================================================================== */

static inline int open(const char *path, int flags) { return wopen(path, flags); }
static inline int close(int fd)                     { return wclose(fd); }
static inline int read(int fd, void *buf, wsize_t n)         { return wread(fd, buf, n); }
static inline int write(int fd, const void *buf, wsize_t n)  { return wwrite(fd, buf, n); }
static inline int lseek(int fd, int off, int whence) { return wlseek(fd, off, whence); }

/* ==================================================================== *
 *  Convenience layer
 *
 *  Implemented on top of the calls above; no extra kernel support.
 * ==================================================================== */

/**
 * Print formatted text to stdout.
 *
 * Supports `%d` `%i` (signed), `%u` (unsigned), `%x` `%X` (hex), `%c`, `%s`,
 * `%p` and `%%`.  A width and the flags `-` (left align) and `0` (zero pad)
 * are honoured, so `%-12s` and `%6u` line columns up.
 *
 * @param fmt Format string, followed by its arguments.
 * @return The number of characters written.
 *
 * @note There is no floating point support; WOS never enables the FPU.
 */
int wprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Print formatted text to a file descriptor.
 * @param fd  Descriptor to write to.
 * @param fmt Format string, as for wprintf().
 * @return The number of characters written.
 */
int wfprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * Format text into a buffer.
 * @param buf  Destination, always NUL-terminated.
 * @param size Size of @p buf.
 * @param fmt  Format string, as for wprintf().
 * @return The number of characters written, not counting the NUL.
 */
int wsnprintf(char *buf, wsize_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * Write a string to stdout, with no newline added.
 * @param s String to write.
 * @return The number of characters written.
 */
int wputs(const char *s);

/* --- full-screen output ------------------------------------------- *
 *
 * These emit ANSI escape sequences, which the VGA console understands and
 * which also drive a real terminal attached to the serial port, so the same
 * program looks right on both.
 */

/** Clear the screen and put the cursor at the top left. */
void wcls(void);

/**
 * Move the cursor.
 * @param row Row, 1 at the top, up to #W_CONSOLE_HEIGHT.
 * @param col Column, 1 at the left, up to #W_CONSOLE_WIDTH.
 */
void wgotoxy(int row, int col);

/**
 * Set the drawing colours for text printed from now on.
 * @param fg Foreground: one of the `W_*` colours, optionally + #W_BRIGHT,
 *           or #W_DEFAULT.
 * @param bg Background: one of the `W_*` colours, or #W_DEFAULT.
 */
void wcolor(int fg, int bg);

/** Restore the default colours. */
void wcolor_reset(void);

/** Erase from the cursor to the end of the line. */
void wclear_line(void);

/**
 * Show or hide the hardware cursor.
 * @param visible Non-zero to show it.
 * @note A program that repaints the whole screen should hide it first, or the
 *       cursor flickers across the screen during every repaint.
 */
void wcursor(int visible);

/**
 * Read one keystroke, decoding the escape sequences that special keys send.
 *
 * Requires raw mode (see wconsole_raw()).  Blocks until a key is pressed.
 *
 * @return An ordinary character as itself, or one of the `W_KEY_*` codes for
 *         an arrow, Home, End, Page Up/Down or Delete.  A bare Escape returns
 *         #W_KEY_ESCAPE.
 *
 * @note Escape both introduces sequences and is a key in its own right, so
 *       this distinguishes them by checking whether anything follows it
 *       immediately -- the same guess a terminal program makes.
 */
int wgetkey(void);

/**
 * Prompt for a password and read it without echoing.
 *
 * Switches the console to raw mode for the duration, so nothing appears on
 * screen and nothing is left in the scrollback. Backspace works; Ctrl+C
 * abandons the entry and yields an empty string.
 *
 * @param prompt Text to print before reading, e.g. "Password: ".
 * @param buf    Buffer to fill, always NUL-terminated.
 * @param size   Size of @p buf.
 * @return The length read, or a negative error.
 */
int wgetpass(const char *prompt, char *buf, wsize_t size);

/**
 * Read one line from the console.
 *
 * Blocks until Enter is pressed. The trailing newline is removed.
 *
 * @param buf  Buffer to fill.
 * @param size Size of @p buf.
 * @return The length of the line, or a negative error from wread().
 */
int wgetline(char *buf, wsize_t size);

/**
 * Format a byte count for people, e.g. 268435456 becomes "256.0M".
 *
 * @param bytes Value to format.
 * @return A pointer into a rotating set of eight static buffers, so up to
 *         eight results can be live in one wprintf() call.  Beyond that the
 *         earliest one is overwritten and prints the wrong value.
 *         Not reentrant.
 */
const char *whuman(unsigned long bytes);

/**
 * How long the machine has been running, in words: "3 days, 4:05", "1:23",
 * "7 mins", "42 secs".
 *
 * The `uptime` command and htop's header both print this, so that they cannot
 * disagree about a number a person is likely to read twice.
 *
 * @return A pointer into a rotating set of static buffers, valid until a few
 *         more calls have been made -- the same arrangement whuman() uses, so
 *         several can appear in one printf.
 */
const char *wuptime_string(void);

/**
 * Turn an error code into a readable message.
 * @param err A positive `W_E*` code -- negate what a failing call returned.
 * @return A short description, e.g. "no such file or directory".
 */
const char *wstrerror(int err);

/* --- memory ------------------------------------------------------- */

/**
 * Allocate @p size bytes.
 * @param size Bytes required. Allocating 0 bytes returns NULL.
 * @return A pointer to the block, or NULL if the heap cannot grow.
 */
void *malloc(wsize_t size);

/** Allocate and zero @p count * @p size bytes. @return As malloc(). */
void *calloc(wsize_t count, wsize_t size);

/**
 * Resize a block, preserving its contents up to the smaller of the two sizes.
 * @param ptr  Block from malloc(), or NULL to allocate afresh.
 * @param size New size; 0 frees the block and returns NULL.
 * @return The new pointer, or NULL if it could not be resized (the original
 *         block is then still valid).
 */
void *realloc(void *ptr, wsize_t size);

/** Release a block. Passing NULL does nothing. @param ptr Block to free. */
void free(void *ptr);

/* --- strings ------------------------------------------------------ */

wsize_t strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, wsize_t n);
char   *strcpy(char *dst, const char *src);
char   *strcat(char *dst, const char *src);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *haystack, const char *needle);

/**
 * Copy a string, always NUL-terminating and never overrunning.
 * @param dst  Destination buffer.
 * @param src  String to copy.
 * @param size Size of @p dst.
 * @return The length of @p src, so a result >= @p size means it was truncated.
 */
wsize_t strlcpy(char *dst, const char *src, wsize_t size);

void *memcpy(void *dst, const void *src, wsize_t n);
void *memmove(void *dst, const void *src, wsize_t n);
void *memset(void *dst, int c, wsize_t n);
int   memcmp(const void *a, const void *b, wsize_t n);

/** Parse a decimal integer, skipping leading spaces and honouring a sign.
 *  @param s Text to parse. @return The value, or 0 if there are no digits. */
int atoi(const char *s);

#endif /* WKERNEL_H */
