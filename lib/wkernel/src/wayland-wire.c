/* The Wayland wire format.  See wayland-wire.h. */

#include "wayland-wire.h"

struct wl_connection {
    int      fd;
    int      broken;

    uint8_t  in[WL_BUFFER_SIZE];
    uint32_t in_len;

    uint8_t  out[WL_BUFFER_SIZE];
    uint32_t out_len;

    /* Descriptors that arrived, oldest first.  They are handed out by the 'h'
     * arguments of the messages that unpack them, in the order they came --
     * which is the order they were sent, because the kernel delivers a
     * descriptor no earlier than the byte it travelled with. */
    int in_fds[W_SEND_MAX_FDS];
    int in_fd_count;

    /* Descriptors queued to go with whatever is in the output buffer. */
    int out_fds[W_SEND_MAX_FDS];
    int out_fd_count;
};

/* ------------------------------------------------------------------ *
 *  Words
 * ------------------------------------------------------------------ */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A signature may start with the version the message appeared in, and a '?'
 * marks the argument after it as allowed to be null.  Neither is an argument. */
static const char *signature_start(const char *s)
{
    while (*s >= '0' && *s <= '9')
        s++;
    return s;
}

int wl_signature_count(const char *signature)
{
    int n = 0;
    for (const char *s = signature_start(signature); *s; s++)
        if (*s != '?')
            n++;
    return n;
}

/* ------------------------------------------------------------------ *
 *  The connection
 * ------------------------------------------------------------------ */

struct wl_connection *wl_connection_create(int fd)
{
    struct wl_connection *c = malloc(sizeof(*c));
    if (!c)
        return NULL;

    memset(c, 0, sizeof(*c));
    c->fd = fd;
    return c;
}

void wl_connection_destroy(struct wl_connection *c)
{
    if (!c)
        return;

    /* Descriptors that arrived and were never unpacked are ours, and nobody
     * else is going to close them. */
    for (int i = 0; i < c->in_fd_count; i++)
        wclose(c->in_fds[i]);
    for (int i = 0; i < c->out_fd_count; i++)
        wclose(c->out_fds[i]);

    wclose(c->fd);
    free(c);
}

int wl_connection_fd(const struct wl_connection *c)  { return c->fd; }
int wl_connection_broken(const struct wl_connection *c) { return c->broken; }
void wl_connection_break(struct wl_connection *c)    { c->broken = 1; }

uint32_t wl_connection_pending(const struct wl_connection *c)
{
    return c->out_len;
}

/* ------------------------------------------------------------------ *
 *  Packing
 * ------------------------------------------------------------------ */

/* A string on the wire is its length *including* the terminator, then the
 * bytes, then padding to a multiple of four.  A null string is a length of
 * zero and nothing else, which is why the length includes the terminator: it
 * is what distinguishes "" from nothing at all. */
static uint32_t packed_string_size(const char *s)
{
    if (!s)
        return 4;
    return 4 + (uint32_t)((strlen(s) + 1 + 3) & ~(wsize_t)3);
}

static uint32_t packed_array_size(const struct wl_array *a)
{
    if (!a)
        return 4;
    return 4 + (uint32_t)((a->size + 3) & ~(wsize_t)3);
}

/* Close the descriptors a message was carrying when the message is not going
 * to be sent.
 *
 * Queueing takes ownership of every 'h' argument, and it has to do so whether
 * it succeeds or not: a caller that had to work out which of the two happened
 * before deciding whether to close its descriptor would get it wrong
 * eventually, and a descriptor leaked that way is a number that stays in use
 * forever. */
static int give_up(const char *sig, const union wl_argument *args)
{
    int n = 0;

    for (const char *s = sig; *s; s++) {
        if (*s == '?')
            continue;
        if (*s == 'h')
            wclose(args[n].h);
        n++;
    }

    return -1;
}

int wl_connection_queue(struct wl_connection *c, uint32_t id, uint32_t opcode,
                        const struct wl_message *msg,
                        const union wl_argument *args)
{
    const char *drop = signature_start(msg->signature);

    if (c->broken)
        return give_up(drop, args);

    const char *sig = drop;

    /* Work out the size first: the header carries it, and it goes in front. */
    uint32_t size = 8;
    int      n    = 0;
    int      fds  = 0;

    for (const char *s = sig; *s; s++) {
        if (*s == '?')
            continue;

        switch (*s) {
        case 's': size += packed_string_size(args[n].s); break;
        case 'a': size += packed_array_size(args[n].a);  break;
        case 'h': fds++;                                 break;  /* no bytes */
        default:  size += 4;                             break;
        }
        n++;
    }

    if (size > WL_BUFFER_SIZE || c->out_len + size > WL_BUFFER_SIZE) {
        /* Flushing first is worth a try: the queue may simply be full of
         * messages the peer has already made room for. */
        wl_connection_flush(c);
        if (size > WL_BUFFER_SIZE || c->out_len + size > WL_BUFFER_SIZE)
            return give_up(sig, args);
    }
    if (c->out_fd_count + fds > W_SEND_MAX_FDS)
        return give_up(sig, args);

    uint8_t *p = c->out + c->out_len;

    put32(p, id);
    put32(p + 4, (size << 16) | (opcode & 0xFFFF));
    p += 8;

    n = 0;
    for (const char *s = sig; *s; s++) {
        if (*s == '?')
            continue;

        switch (*s) {
        case 'i': put32(p, (uint32_t)args[n].i); p += 4; break;
        case 'u': put32(p, args[n].u);           p += 4; break;
        case 'f': put32(p, (uint32_t)args[n].f); p += 4; break;
        case 'n': put32(p, args[n].n);           p += 4; break;

        case 'o': {
            /* An object argument is its id.  The caller has already reduced
             * the object to one, because what an object *is* differs between
             * the two sides and the wire does not care. */
            put32(p, args[n].u);
            p += 4;
            break;
        }

        case 's': {
            const char *str = args[n].s;
            if (!str) {
                put32(p, 0);
                p += 4;
            } else {
                uint32_t len = (uint32_t)strlen(str) + 1;
                put32(p, len);
                p += 4;
                memcpy(p, str, len);
                p += len;
                while ((len & 3) != 0) { *p++ = 0; len++; }
            }
            break;
        }

        case 'a': {
            const struct wl_array *a = args[n].a;
            uint32_t len = a ? (uint32_t)a->size : 0;
            put32(p, len);
            p += 4;
            if (len) {
                memcpy(p, a->data, len);
                p += len;
                while ((len & 3) != 0) { *p++ = 0; len++; }
            }
            break;
        }

        case 'h':
            /* Descriptors travel beside the bytes rather than in them. */
            c->out_fds[c->out_fd_count++] = args[n].h;
            break;

        default:
            break;
        }
        n++;
    }

    c->out_len += size;
    return 0;
}

int wl_connection_flush(struct wl_connection *c)
{
    if (c->broken)
        return -1;
    if (c->out_len == 0 && c->out_fd_count == 0)
        return 0;

    /* Only as much as a poll has promised room for.  Sending more than that
     * can block, and a compositor blocked writing to a client that is blocked
     * writing to it is a machine that has stopped. */
    while (c->out_len > 0 || c->out_fd_count > 0) {
        wpollfd_t ready = { c->fd, W_POLLOUT, 0 };
        if (wpoll(&ready, 1, 0) <= 0 || !(ready.revents & W_POLLOUT))
            return 0;                      /* try again next time round */

        uint32_t take = c->out_len;
        if (take > W_SEND_CHUNK)
            take = W_SEND_CHUNK;

        /* Descriptors go with the first chunk.  They are delivered with the
         * byte they were queued behind, so sending them ahead of the bytes
         * that describe them would be wrong; sending them with the first
         * chunk of those bytes is not. */
        wmsg_t m = { c->out, take, c->out_fd_count, c->out_fds };

        int sent = wsend(c->fd, &m);
        if (sent < 0) {
            c->broken = 1;
            return -1;
        }

        /* Sending a descriptor gives it away: the kernel took its own
         * reference for the receiver, and the sender's copy is done with.
         * Holding one back to send twice would close a number that something
         * else has been given in the meantime. */
        for (int i = 0; i < c->out_fd_count; i++)
            wclose(c->out_fds[i]);
        c->out_fd_count = 0;

        if ((uint32_t)sent < c->out_len)
            memmove(c->out, c->out + sent, c->out_len - (uint32_t)sent);
        c->out_len -= (uint32_t)sent;

        if (sent == 0)
            break;
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Unpacking
 * ------------------------------------------------------------------ */

int wl_connection_read(struct wl_connection *c)
{
    if (c->broken)
        return -1;
    if (c->in_len >= WL_BUFFER_SIZE) {
        /* A full buffer with no whole message in it is not a slow peer, it is
         * a peer talking a different protocol. */
        c->broken = 1;
        return -1;
    }

    int    fds[W_SEND_MAX_FDS];
    int    room = W_SEND_MAX_FDS - c->in_fd_count;
    wmsg_t m    = { c->in + c->in_len, WL_BUFFER_SIZE - c->in_len, room, fds };

    int n = wrecv(c->fd, &m);
    if (n <= 0) {
        c->broken = 1;
        return n;
    }

    for (int i = 0; i < m.fd_count; i++)
        c->in_fds[c->in_fd_count++] = fds[i];

    c->in_len += (uint32_t)n;
    return n;
}

int wl_connection_next(struct wl_connection *c, struct wl_msg_view *out)
{
    if (c->in_len < 8)
        return 0;

    uint32_t id   = get32(c->in);
    uint32_t word = get32(c->in + 4);
    uint32_t size = word >> 16;

    if (size < 8 || size > WL_BUFFER_SIZE) {
        c->broken = 1;
        return -1;
    }
    if (c->in_len < size)
        return 0;

    out->id       = id;
    out->opcode   = word & 0xFFFF;
    out->size     = size;
    out->body     = c->in + 8;
    out->body_len = size - 8;
    return 1;
}

void wl_connection_consume(struct wl_connection *c, uint32_t size)
{
    if (size > c->in_len)
        size = c->in_len;

    if (size < c->in_len)
        memmove(c->in, c->in + size, c->in_len - size);
    c->in_len -= size;
}

int wl_connection_unpack(struct wl_connection *c, const char *signature,
                         const uint8_t *body, uint32_t body_len,
                         union wl_argument *args, int max)
{
    const char *sig = signature_start(signature);
    uint32_t    at  = 0;
    int         n   = 0;

    for (const char *s = sig; *s; s++) {
        if (*s == '?')
            continue;
        if (n >= max)
            return -1;

        /* Every argument but a descriptor starts with a word. */
        if (*s != 'h') {
            if (at + 4 > body_len)
                return -1;
        }

        switch (*s) {
        case 'i': args[n].i = (int32_t)get32(body + at);    at += 4; break;
        case 'u': args[n].u = get32(body + at);             at += 4; break;
        case 'f': args[n].f = (wl_fixed_t)get32(body + at); at += 4; break;
        case 'n': args[n].n = get32(body + at);             at += 4; break;
        case 'o': args[n].u = get32(body + at);             at += 4; break;

        case 's': {
            uint32_t len = get32(body + at);
            at += 4;

            if (len == 0) {
                args[n].s = NULL;         /* a null string, not an empty one */
                break;
            }
            if (at + len > body_len)
                return -1;
            /* Trusting the terminator would let a peer hand us a string that
             * runs into the next message. */
            if (body[at + len - 1] != '\0')
                return -1;

            args[n].s = (const char *)(body + at);
            at += (len + 3) & ~3u;
            break;
        }

        case 'a': {
            static struct wl_array scratch;    /* one array argument at a time */
            uint32_t len = get32(body + at);
            at += 4;

            if (at + len > body_len)
                return -1;

            scratch.data  = (void *)(body + at);
            scratch.size  = len;
            scratch.alloc = 0;                 /* not ours to grow or free */
            args[n].a     = &scratch;
            at += (len + 3) & ~3u;
            break;
        }

        case 'h': {
            if (c->in_fd_count == 0)
                return -1;                     /* promised one, none arrived */

            args[n].h = c->in_fds[0];
            for (int i = 1; i < c->in_fd_count; i++)
                c->in_fds[i - 1] = c->in_fds[i];
            c->in_fd_count--;
            break;
        }

        default:
            return -1;
        }

        n++;
    }

    return n;
}

/* ------------------------------------------------------------------ *
 *  wl_array
 * ------------------------------------------------------------------ */

void wl_array_init(struct wl_array *array)
{
    array->size  = 0;
    array->alloc = 0;
    array->data  = NULL;
}

void wl_array_release(struct wl_array *array)
{
    if (array->alloc)
        free(array->data);
    wl_array_init(array);
}

void *wl_array_add(struct wl_array *array, wsize_t size)
{
    if (array->size + size > array->alloc) {
        wsize_t want = array->alloc ? array->alloc * 2 : 16;
        while (want < array->size + size)
            want *= 2;

        void *grown = realloc(array->alloc ? array->data : NULL, want);
        if (!grown)
            return NULL;

        /* An array that was pointing into somebody else's memory has to be
         * copied rather than reallocated. */
        if (!array->alloc && array->size)
            memcpy(grown, array->data, array->size);

        array->data  = grown;
        array->alloc = want;
    }

    void *at = (char *)array->data + array->size;
    array->size += size;
    return at;
}
