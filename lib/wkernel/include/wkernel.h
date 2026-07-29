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
 * `cpu_ticks` is how many timer ticks the process has run for since it
 * started, at the timer's 100 ticks a second.  It is a total, not a rate:
 * to show a load, read it twice and divide the difference by the ticks that
 * passed in between -- see wticks().
 *
 * @param pid Process to inspect, or 0 for the calling process.
 * @param out Filled in with the pid, name, resident bytes, and the code,
 *            data, heap and stack breakdown, plus the thread count and the
 *            processor time used.
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

/**
 * Report every mounted filesystem, not just the one the system is on.
 *
 * WOS mounts two: the disk at `/`, and the one held in memory at `/ramdisk`
 * that starts empty and is gone at the next boot.  Each entry says where it is
 * mounted, what it is, whether what is written to it survives a reboot, and
 * the same usage figures wdiskinfo() gives for the disk alone.
 *
 * @param out Array of at least @p max entries.
 * @param max Most entries to write; `W_DISK_MAX` is always enough.
 * @return The number of filesystems written, or `-W_EFAULT`.
 */
int wdisklist(wdisk_t *out, int max);

/* ==================================================================== *
 *  Services
 * ==================================================================== */

/**
 * List the system's services: what they are, whether each starts at boot, and
 * whether it is running now.
 *
 * Reading needs no permission. The list comes from the unit files in
 * `/services`, read at boot; the running state comes from the process table,
 * so a service whose process has died is reported stopped rather than
 * running.
 *
 * @param out Array of at least @p max entries.
 * @param max Most to write; `W_SERVICE_MAX` is always enough.
 * @return How many were written, or `-W_EFAULT`.
 */
int wservicelist(wservice_t *out, int max);

/**
 * Start, stop, restart, enable or disable a service.
 *
 * Enabling does not start and starting does not enable: the two answer
 * different questions -- "should this run at the next boot" and "is it running
 * now".
 *
 * Every action needs root or the `systemctleditor` role, because each changes
 * what the machine is running for everybody on it.
 *
 * A stop asks the process to leave rather than tearing it down where it
 * stands, so the service may still be reported running for a moment
 * afterwards. That is the truth about the machine, not a delay in reporting.
 *
 * @param action One of `W_SVC_START`, `W_SVC_STOP`, `W_SVC_RESTART`,
 *               `W_SVC_ENABLE`, `W_SVC_DISABLE`.
 * @param name   The service's name, as in `/services/<name>`.
 * @return 0 on success, `-W_EPERM` without the role, `-W_ENOENT` if there is
 *         no such service, `-W_EBUSY` when starting one already running or
 *         stopping one that is not, or an error from spawning it.
 */
int wservicectl(int action, const char *name);

/* ==================================================================== *
 *  Shared memory
 *
 *  Pages two processes can both see.  A socket copies what it carries, which
 *  is right for messages and wrong for a screenful of pixels; this is how the
 *  pixels stay where they are and only the descriptor travels.
 * ==================================================================== */

/**
 * Create a shared memory object of @p bytes and return a descriptor for it.
 *
 * The size is fixed at creation and rounded up to a whole page. The pages are
 * zeroed, so nothing the previous owner of that memory left behind is visible.
 *
 * The descriptor is an ordinary one: it can be sent over a socket with
 * wsend(), and the receiver ends up naming the same pages. It cannot be read
 * or written -- shared memory is reached by mapping it, and wread() on one
 * fails with `-W_EINVAL` rather than pretending to be a file.
 *
 * The object lives until every descriptor naming it is closed *and* every
 * process that mapped it has unmapped or exited. A client may therefore create
 * a buffer, hand it to a compositor and exit, leaving the compositor holding
 * pixels that are still perfectly good.
 *
 * @param bytes How much to allocate; at most `W_SHM_MAX_BYTES`.
 * @return A descriptor, `-W_EINVAL` for a size of zero or one too large,
 *         `-W_ENOMEM` if the machine has not got the memory, or `-W_EMFILE`.
 */
int wshmopen(unsigned int bytes);

/**
 * Map a shared memory object into this process and return a pointer to it.
 *
 * The whole object is mapped, readable and writable. Mapping the same
 * descriptor twice gives two addresses for the same pages, which is harmless.
 *
 * @param fd A descriptor from wshmopen(), or one that arrived over a socket.
 * @return A pointer to the pages, or NULL if @p fd is not a shared memory
 *         object or there is no room to map it.
 */
void *wshmmap(int fd);

/**
 * Release a mapping made by wshmmap(). The pages survive if anything else
 * still names the object.
 * @param addr Exactly what wshmmap() returned.
 * @return 0, or `-W_EINVAL` if there is no mapping there.
 */
int wshmunmap(void *addr);

/**
 * How large a shared memory object is, in bytes -- what was asked for, rounded
 * up to a whole page.
 *
 * The receiver of a descriptor needs this: it was told a width and a height by
 * the protocol, and this is how it checks that the memory it was handed is
 * actually big enough to hold them, rather than trusting the sender's
 * arithmetic.
 *
 * @param fd A shared memory descriptor.
 * @return The size in bytes, or `-W_EBADF`.
 */
int wshmsize(int fd);

/* ==================================================================== *
 *  The screen
 *
 *  For a program that draws pixels rather than characters.  The text console
 *  has the framebuffer until something takes it; taking it is what a
 *  compositor does, and giving it back is what leaves the machine usable
 *  afterwards.
 * ==================================================================== */

/**
 * Describe the screen: its size, its stride, and who has it.
 *
 * `present` is 0 on a machine whose console is still VGA text mode, where
 * there is no framebuffer to draw on at all. A program that draws should check
 * this and say so rather than blitting into nothing.
 *
 * @param out Filled in; zeroed first, so every field is defined.
 * @return 0, or `-W_EFAULT`.
 */
int wdisplayinfo(wdisplay_t *out);

/**
 * Take the screen from the text console.
 *
 * The console does not stop -- it keeps tracking the cursor and recording
 * everything printed -- it just stops drawing. When the screen is released
 * everything written meanwhile is repainted, so a program that printed behind
 * a compositor has not lost its output.
 *
 * There is one screen, so taking it affects every process on the machine. That
 * makes it root's to do, like setting the clock -- or a session's, when root
 * started it and handed it the seat with wseatgrant().
 *
 * @return 0, `-W_EPERM` without root or a seat, `-W_ENODEV` on a machine with
 *         no framebuffer, or `-W_EBUSY` if another process already has it.
 *
 * @note The screen is released automatically when the holder exits, however it
 *       exits. A compositor that faults does not take the display with it.
 */
int wdisplaygrab(void);

/** Give the screen back to the console, which repaints it. @return 0. */
int wdisplaydrop(void);

/**
 * Put a rectangle of pixels on the screen.
 *
 * Pixels are `0x00RRGGBB`, one per 32-bit word. `stride` is the source's row
 * length in pixels, so a rectangle can be blitted straight out of a larger
 * back buffer without being copied out of it first.
 *
 * The rectangle is clipped to the screen rather than trusted, so a coordinate
 * off the edge draws less rather than writing somewhere it should not.
 *
 * @param b Where the pixels are and where they go.
 * @return 0, `-W_EPERM` if this process does not hold the screen, `-W_EINVAL`
 *         for a rectangle wider than its own stride, or `-W_EFAULT`.
 */
int wdisplayblit(const wblit_t *b);

/**
 * Open the keyboard as a stream of key transitions.
 *
 * The console turns keystrokes into lines of text, which is what a shell wants
 * and the opposite of what a compositor wants: a compositor has to know that a
 * key was *released*, and which physical key it was regardless of the
 * character it would print, so that it can tell one of its own bindings from
 * something to forward to a window.
 *
 * Read the descriptor for whole `winput_t` records -- a short read is never
 * half an event -- and wait on it with wpoll() alongside everything else.
 *
 * While it is open the console reads nothing at all. That is not a limitation
 * being worked around: there is one keyboard, and the holder has it. Closing
 * the descriptor, or exiting, gives it straight back.
 *
 * Key codes are the Linux evdev codes that `wl_keyboard.key` carries, so a
 * program that knows Wayland already knows these numbers.
 *
 * @return A descriptor, `-W_EPERM` without root or a seat, or `-W_EBUSY` if
 *         another process already holds the keyboard.
 */
int winputopen(void);

/**
 * Where the pointer is, and whether the machine has one.
 *
 * A compositor asks this before it advertises a seat: telling clients there is
 * a pointer when there is none leaves them waiting for motion that will never
 * come, and a cursor drawn on a machine with no mouse is a cursor nobody can
 * move. It is also where the first cursor position comes from, so the arrow
 * starts wherever the kernel has been keeping it rather than in a corner.
 *
 * The position is clamped to the screen by the kernel, which is the only place
 * that knows how big the screen is.
 *
 * @param out Filled in; zeroed first, so every field is defined.
 * @return 0, or `-W_EFAULT`.
 */
int wpointer(wpointer_t *out);

/**
 * Arm a seat grant: let the next process this one spawns take the screen and
 * the keyboard, whoever it runs as.
 *
 * The seat is both devices together. A session with a screen and no keys is
 * not one anybody can use, so wdisplaygrab() and winputopen() accept the same
 * grant and there is no way to hand over half of it.
 *
 * This is for writing a login manager, and its shape is that job's shape. Such
 * a program starts as root, checks a password, becomes the user who gave it
 * with wlogin(), and starts their session -- but a process that has dropped to
 * a user can never climb back, so by the time it has someone to hand the seat
 * to, it is no longer anyone who could grant it. Hence arming it in advance:
 *
 * ```c
 * wseatgrant();                 // while still root
 * wlogin(name, password);       // now uid 1, and cannot undo that
 * wspawn("/app/sway/launch", argv);   // takes the seat with it
 * ```
 *
 * The grant is spent by that one spawn and does not descend any further: the
 * session leader has the seat, and the terminals and editors it goes on to
 * start are ordinary processes that cannot take the display from it.
 *
 * @return 0, or `-W_EPERM` for anyone but root.
 */
int wseatgrant(void);

/* ==================================================================== *
 *  Local sockets
 *
 *  A connection-oriented byte stream between two processes, named by a path,
 *  able to carry file descriptors alongside the bytes -- a Unix domain
 *  socket, in the shape WOS needs it.
 *
 *  A pipe reaches another process only by being inherited across a spawn,
 *  which is enough for a shell and a terminal emulator and not enough for a
 *  display server: a client has to find the compositor by name, having never
 *  been its child, talk in both directions, and hand over a descriptor for a
 *  buffer rather than a copy of it.
 * ==================================================================== */

/**
 * Answer to @p path from now on.
 *
 * Nothing is created on the disk. The path is an address, and it is gone when
 * the returned descriptor is closed. Answering to a name counts as writing
 * where it lives, so this needs write permission on the directory -- which for
 * an ordinary user means somewhere under their own home.
 *
 * @param path Address to listen on, e.g. "/ramdisk/wayland-0".
 * @return A descriptor to accept connections on, `-W_EEXIST` if the name is
 *         taken, `-W_EACCES`, `-W_ENFILE` or `-W_EMFILE`.
 */
int wlisten(const char *path);

/**
 * Connect to whoever is listening on @p path.
 *
 * Returns as soon as the connection is queued rather than waiting to be
 * accepted, so a client may start sending immediately.
 *
 * @param path Address to connect to.
 * @return A connected descriptor, `-W_ENOENT` if nothing is listening there,
 *         `-W_EBUSY` if the listener's backlog is full, or `-W_EMFILE`.
 */
int wconnect(const char *path);

/**
 * Take the next connection waiting on a listening descriptor, blocking until
 * one arrives. Use wpoll() first to wait for one without blocking here.
 *
 * @param fd A descriptor from wlisten().
 * @return A connected descriptor, or `-W_EBADF`.
 */
int waccept(int fd);

/**
 * Send bytes, and optionally descriptors, on a connected socket.
 *
 * A descriptor passed this way is copied into the receiver's table with a
 * reference of its own: closing yours afterwards does not close its. It is
 * delivered no earlier than the byte it was sent with, so a receiver that has
 * read the message describing a buffer holds that buffer's descriptor by then
 * and never before.
 *
 * Blocks while the connection's buffer is full, like a write to a pipe.
 *
 * @param fd  A connected descriptor.
 * @param msg `buf`/`len` are the bytes; `fds`/`fd_count` the descriptors to
 *            pass, at most `W_SEND_MAX_FDS` of them.
 * @return Bytes sent, `-W_EPIPE` once the far end has gone, or `-W_EBADF`.
 */
int wsend(int fd, wmsg_t *msg);

/**
 * Receive bytes, and any descriptors that arrived with them.
 *
 * @param fd  A connected descriptor.
 * @param msg `buf`/`len` is where bytes go; `fds`/`fd_count` where arriving
 *            descriptors go, with `fd_count` updated to how many there were.
 * @return Bytes received, 0 once the far end has gone and nothing is left, or
 *         `-W_EBADF`.
 *
 * @code
 *     char     bytes[256];
 *     int      fds[4];
 *     wmsg_t   msg = { bytes, sizeof(bytes), 4, fds };
 *
 *     int n = wrecv(fd, &msg);
 *     // n bytes in `bytes`, msg.fd_count descriptors in `fds`
 * @endcode
 */
int wrecv(int fd, wmsg_t *msg);

/**
 * Wait until one of several descriptors is ready.
 *
 * Sockets, pipes and the console can all be waited on together, which is what
 * a program serving several clients at once needs and what wpollin() -- one
 * descriptor, no waiting -- cannot do.
 *
 * @param fds        Array of descriptors and the `W_POLL*` events wanted;
 *                   `revents` is filled in with what is true now.
 * @param count      How many, at most `W_POLL_MAX`.
 * @param timeout_ms How long to wait: 0 returns at once, negative waits
 *                   forever.
 * @return How many entries came back with a non-zero `revents`, 0 on timeout,
 *         or `-W_EINVAL` / `-W_EFAULT`.
 */
int wpoll(wpollfd_t *fds, int count, int timeout_ms);

/* ==================================================================== *
 *  Processors
 * ==================================================================== */

/**
 * Report what the machine's processor is and what it is capable of.
 *
 * `count` is every logical processor the firmware listed; `online` is how many
 * the kernel is actually running on, which is one.  See wcpulist() for why the
 * difference is worth showing rather than hiding.
 *
 * @param out Filled in with the core counts, the tick rate the usage counters
 *            advance at, the base, minimum and maximum clocks the machine
 *            admits to, and the CPU's own name.  A clock of 0 means this
 *            machine would not say.
 * @return 0 on success, or `-W_EFAULT`.
 */
int wcpuinfo(wcpuinfo_t *out);

/**
 * Read the per-core figures: clock, temperature and how busy each one is.
 *
 * WOS starts only the processor it booted on, so the other cores come back
 * with `online` clear, no clock and no temperature -- a reading has to be
 * taken by the core it describes, and nothing is running there to take it.
 * They are still listed, because they are still part of the machine.
 *
 * `busy_ticks` and `idle_ticks` count since boot.  A load figure is the change
 * in `busy_ticks` over the change in both, between two samples:
 *
 * @code
 *     wcpu_t before[W_CPU_MAX], after[W_CPU_MAX];
 *     int n = wcpulist(before, W_CPU_MAX);
 *     ... wait a second ...
 *     wcpulist(after, W_CPU_MAX);
 *
 *     unsigned busy = after[0].busy_ticks - before[0].busy_ticks;
 *     unsigned idle = after[0].idle_ticks - before[0].idle_ticks;
 *     unsigned percent = (busy + idle) ? busy * 100 / (busy + idle) : 0;
 * @endcode
 *
 * @param out Array of at least @p max entries.
 * @param max Most entries to write; `W_CPU_MAX` is always enough.
 * @return The number of cores written, or `-W_EFAULT`.
 */
int wcpulist(wcpu_t *out, int max);

/**
 * Ask the processor to run at a particular clock.
 *
 * The request is clamped to the range wcpuinfo() reported and rounded to a
 * step the hardware can take -- `step_khz`, usually 100 MHz -- so the clock
 * that comes back is rarely the exact one asked for.
 *
 * There is one clock and every process on the machine runs on it, so this
 * needs root or the `editfreq` role: a slow machine is slow for everybody, and
 * a fast one is hot for everybody.
 *
 * @param khz  The clock to hold, in kilohertz.  Zero or less hands the
 *             decision back to the hardware's own judgement.
 * @return The clock settled on in kHz (0 for automatic), `-W_EPERM` without
 *         the role, or `-W_ENODEV` on a machine whose clock cannot be set --
 *         which is the usual answer inside a hypervisor, where the registers
 *         that carry the request are not emulated.
 */
int wcpufreq(int khz);

/**
 * Report the machine's battery, as far as the firmware describes it.
 *
 * `present` says whether there is one at all -- the difference between a
 * laptop and a desktop, and the part of the question that can always be
 * answered. The pack's maker, name, chemistry, design capacity and nominal
 * voltage come from the SMBIOS tables where the firmware provides them.
 *
 * `charge_percent` is almost always -1, and that is not a failure. Every
 * laptop reports its charge through an ACPI method that reads the embedded
 * controller, and calling one means interpreting AML bytecode, which WOS has
 * no interpreter for. Everything static is read; the one figure that changes
 * minute to minute is the one that needs the interpreter, so it is reported as
 * unknown rather than guessed at. `state` and `ac_online` are unknown for the
 * same reason.
 *
 * @param out Filled in with what was found; every field is zeroed first, so an
 *            absent battery reads as `present == 0` and nothing else.
 * @return 0 on success, or `-W_EFAULT`.
 */
int wbattery(wbattery_t *out);

/**
 * Format a clock rate in kilohertz for people: 1900000 becomes "1.90GHz",
 * 400000 becomes "400MHz", and 0 becomes "-".
 *
 * @param khz Rate to format.
 * @return A pointer into the same rotating set of static buffers whuman()
 *         uses, with the same rules. Not reentrant.
 */
const char *wclock_string(unsigned int khz);

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
 * Reap a child that has already exited, without waiting for one that has not.
 *
 * wwait() blocks, which is right for a shell running a command and wrong for
 * anything that has other work: a compositor that started a program on a
 * keybinding cannot stop serving its clients until that program happens to
 * finish. Without a call like this, every program it ever started would stay
 * in the process table as a zombie.
 *
 * Call it whenever it is convenient -- there is no cost to calling it when
 * nothing has exited.
 *
 * @param status Where to put the exit status, or NULL.
 * @return The pid reaped, or `-W_ECHILD` when nothing has exited (which
 *         includes having no children at all).
 */
int wreap(int *status);

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
 *
 * This is not a way to wait.  A loop that yields is still a process asking to
 * run, and while it goes round the processor is fully occupied doing nothing.
 * Use wsleep() to wait.
 */
void wyield(void);

/**
 * Stop running until @p ms milliseconds have passed.
 *
 * The process is taken off the run queue entirely, so the machine can be
 * genuinely idle while it waits -- which is what makes a CPU usage figure mean
 * something, and on a laptop is the difference between a fan that runs and one
 * that does not.
 *
 * The wait is rounded up to whole timer ticks (10 ms), and returns as soon as
 * possible after the deadline rather than exactly on it: the process has to be
 * scheduled again like any other.
 *
 * @param ms Milliseconds to wait.  Zero or less is a wyield().
 *
 * @code
 *     while (running) {
 *         redraw();
 *         while (!wpollin(W_STDIN) && wticks() < until)
 *             wsleep(20);        // 50 times a second, not as fast as it can
 *     }
 * @endcode
 */
void wsleep(int ms);

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
 * are honoured, so `%-12s` and `%6u` line columns up.  A precision on `%s`
 * truncates it, so `%.20s` and `%.*s` cut text to a column.
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
 * Format text into a buffer from an already-collected argument list, so that a
 * program can build a printf of its own on top -- a logger, say.
 *
 * Include `<stdarg.h>` for `va_list`.
 *
 * @param buf  Destination, always NUL-terminated.
 * @param size Size of @p buf.
 * @param fmt  Format string, as for wprintf().
 * @param ap   The arguments, from va_start().
 * @return The number of characters written, not counting the NUL.
 */
int wvsnprintf(char *buf, wsize_t size, const char *fmt, __builtin_va_list ap)
    __attribute__((format(printf, 3, 0)));

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

/* ==================================================================== *
 *  Drawing text
 *
 *  A program that owns the framebuffer draws its own characters, and there is
 *  no font on the screen to borrow -- the console's glyphs live in the kernel.
 *  So the same font is here: the standard IBM VGA 8x16 set, which is what the
 *  console draws, so a window and the console beneath it are set in one type.
 * ==================================================================== */

/** The whole font: 256 glyphs of 16 rows, one byte per row, high bit leftmost. */
const unsigned char *wfont8x16(void);

/** One glyph: 16 bytes, one per row. @param c The character. */
const unsigned char *wglyph8x16(unsigned int c);

/* ==================================================================== *
 *  Keys
 *
 *  What xkbcommon answers on Linux, for a system that has not got it. The
 *  codes are the evdev codes `wl_keyboard.key` carries and the names are the
 *  X11 keysym names a sway configuration file is written in, so a binding
 *  means here what it means there.
 * ==================================================================== */

/**
 * The key code a name refers to, e.g. "Return" -> 28, "q" -> 16, "Left" -> 105.
 * Case-insensitive. A single shifted character names the key that prints it,
 * which is how `bindsym $mod+Shift+Q` finds the q key.
 * @return The evdev code, or 0 if the name is not a key.
 */
uint32_t wkeycode_from_name(const char *name);

/** What a key is called, or NULL if nothing here knows it. */
const char *wkeyname(uint32_t keycode);

/**
 * The character a key produces with those modifiers held, or 0 for a key that
 * prints nothing. Shift and Caps Lock behave as a keyboard does, and Ctrl
 * turns a letter into its control code, so Ctrl+C arrives as 0x03.
 * @param keycode An evdev key code.
 * @param mods    `W_MOD_*` flags in force.
 */
uint32_t wkeychar(uint32_t keycode, uint32_t mods);

/** The `W_MOD_*` bit a name refers to: "Shift", "Ctrl", "Alt", "Mod1",
 *  "Mod4", "Super". @return The bit, or 0. */
uint32_t wmodifier_from_name(const char *name);

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
