/* Anonymous pipes. See pipe.h for the model. */

#include "pipe.h"
#include "sched.h"
#include "string.h"
#include "wabi.h"

/* A small fixed pool: a handful of terminals is the most this system will run
 * at once, and each needs two pipes. */
#define MAX_PIPES 16

static pipe_t pipes[MAX_PIPES];

pipe_t *pipe_create(void)
{
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            pipe_t *p = &pipes[i];
            memset(p, 0, sizeof(*p));
            p->used    = true;
            p->readers = 1;
            p->writers = 1;
            return p;
        }
    }
    return NULL;
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

    if (p->readers <= 0 && p->writers <= 0)
        p->used = false;
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
