/* Anonymous pipes: a byte stream between two file descriptors.
 *
 * A pipe is a fixed ring buffer with a read end and a write end, each of which
 * is reference counted so the buffer lives exactly as long as some descriptor
 * still names it.  Reads block while the pipe is empty and a writer still
 * exists; once the last writer closes, an empty read returns 0 (end of file).
 * Writes block while the pipe is full and a reader still exists; once the last
 * reader closes, a write fails with -W_EPIPE.
 *
 * WOS has no fork, so a pipe reaches a child through proc_spawn(), which dups
 * the chosen ends into the child's descriptor table.  That is what lets vim's
 * :term run a program with its stdin and stdout wired to the editor instead of
 * the console.
 */
#ifndef WOS_PIPE_H
#define WOS_PIPE_H

#include "types.h"

#define PIPE_CAP 4096

typedef struct pipe {
    bool     used;
    uint8_t  buf[PIPE_CAP];
    uint32_t head;          /* next write position          */
    uint32_t tail;          /* next read position           */
    uint32_t count;         /* bytes currently buffered      */
    int      readers;       /* descriptors holding the read end  */
    int      writers;       /* descriptors holding the write end */
} pipe_t;

/* Allocate a pipe with one reader and one writer already accounted for -- the
 * two descriptors the creator is about to install.  Returns NULL if the pool
 * is exhausted. */
pipe_t *pipe_create(void);

/* Adjust the reference count on one end.  `write_end` selects which. */
void pipe_ref(pipe_t *p, bool write_end);
void pipe_unref(pipe_t *p, bool write_end);

/* Move bytes through the pipe.  Both may block; see the file comment.
 * pipe_read returns the count read (0 at end of file); pipe_write returns the
 * count written or -W_EPIPE. */
int pipe_read(pipe_t *p, void *buf, uint32_t len);
int pipe_write(pipe_t *p, const void *buf, uint32_t len);

/* True if a read would not block: data is waiting, or every writer has gone
 * and the reader should be told about the end of file. */
bool pipe_pollin(pipe_t *p);

/* True if a write would not block: there is room in the buffer, or nobody is
 * reading any more -- a write to a pipe with no readers fails at once rather
 * than waiting for room that will never come. */
bool pipe_pollout(pipe_t *p);

#endif /* WOS_PIPE_H */
