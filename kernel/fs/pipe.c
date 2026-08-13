/* Anonymous pipes. See pipe.h for the model. */

#include "pipe.h"
#include "sched.h"
#include "kheap.h"
#include "string.h"
#include "wabi.h"

/* Pipes come from the heap, one at a time, the way socket endpoints do.
 *
 * They used to come from a fixed pool of sixteen, on the reasoning that a
 * handful of terminals was the most this system would run at once.  A tiling
 * compositor made that wrong in a way worth recording: every window running a
 * program needs two pipes, so sixteen was a hard ceiling of eight windows --
 * and the failure was a terminal that opened and then could not start a shell,
 * which looks nothing like "the machine is out of pipes".
 *
 * A pipe is four kilobytes of buffer.  Sixteen of those sat in the kernel's
 * BSS whether or not anything ever opened one; now they cost nothing until
 * they exist, and the limit is the heap, which is a limit that says what it
 * is. */
pipe_t *pipe_create(void)
{
    pipe_t *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;

    p->used    = true;
    p->readers = 1;
    p->writers = 1;
    return p;
}

void pipe_ref(pipe_t *p, bool write_end)
{
    if (!p)
        return;
    if (write_end)
        p->writers++;
    else
        p->readers++;
}

void pipe_unref(pipe_t *p, bool write_end)
{
    if (!p)
        return;

    if (write_end)
        p->writers--;
    else
        p->readers--;

    /* A closing end changes what the other end should do: a reader waiting on
     * a pipe whose last writer just left has to wake up to see the end of
     * file, and vice versa. */
    sched_wake(WAIT_PIPE);

    if (p->readers <= 0 && p->writers <= 0) {
        p->used = false;
        kfree(p);
    }
}

int pipe_read(pipe_t *p, void *buf, uint32_t len)
{
    if (!p)
        return -W_EBADF;
    if (len == 0)
        return 0;

    /* Wait for a byte, unless there can never be one: no writer means end of
     * file, which is a real result rather than something to block on. */
    while (p->count == 0 && p->writers > 0)
        sched_block(WAIT_PIPE);

    if (p->count == 0)
        return 0;               /* end of file */

    uint8_t *out = buf;
    uint32_t n = 0;
    while (n < len && p->count > 0) {
        out[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_CAP;
        p->count--;
    }

    /* A writer blocked on a full pipe can now make progress. */
    sched_wake(WAIT_PIPE);
    return (int)n;
}

int pipe_write(pipe_t *p, const void *buf, uint32_t len)
{
    if (!p)
        return -W_EBADF;
    if (len == 0)
        return 0;

    const uint8_t *in = buf;
    uint32_t n = 0;

    while (n < len) {
        /* Nobody left to read it: the classic broken pipe. */
        if (p->readers <= 0)
            return n > 0 ? (int)n : -W_EPIPE;

        while (p->count == PIPE_CAP && p->readers > 0)
            sched_block(WAIT_PIPE);

        if (p->readers <= 0)
            return n > 0 ? (int)n : -W_EPIPE;

        while (n < len && p->count < PIPE_CAP) {
            p->buf[p->head] = in[n++];
            p->head = (p->head + 1) % PIPE_CAP;
            p->count++;
        }

        /* A reader waiting for input can now make progress. */
        sched_wake(WAIT_PIPE);
    }

    return (int)n;
}

bool pipe_pollin(pipe_t *p)
{
    if (!p)
        return true;            /* a bad pipe should not make a poller hang */
    return p->count > 0 || p->writers <= 0;
}

bool pipe_pollout(pipe_t *p)
{
    if (!p)
        return true;
    return p->count < PIPE_CAP || p->readers <= 0;
}
