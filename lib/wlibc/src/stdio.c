/* Streams: buffered file I/O over the WOS descriptors.
 *
 * The rule that keeps this honest: a stream is either reading or writing at
 * any moment, never both.  Standard C already requires a program to seek
 * between the two on a "r+" stream, so nothing is given up by enforcing it --
 * and a buffer that has to be a read-ahead and a write-behind at the same time
 * is where every subtle bug in a stdio lives.
 *
 * Positions are kept by the kernel, not here.  ftell() asks it where the
 * descriptor is and corrects by what this layer is holding: bytes read ahead
 * of the program, or bytes written by the program and not yet handed over.
 * Two records of the same number are two records that can disagree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "wkernel.h"

/* The three that are open before main() runs.
 *
 * stdout is line buffered when it is the console, so a prompt printed without
 * a newline still appears; stderr is never buffered, because a message about
 * something going wrong is worth a syscall.  A program writing to a file
 * through stdout gets full buffering, which is what makes redirected output
 * cost one write per kilobyte rather than one per line. */
static FILE std_files[3] = {
    { 0, NULL, 0, 0, 0, _WC_F_READ  | _WC_F_OPEN,               EOF },
    { 1, NULL, 0, 0, 0, _WC_F_WRITE | _WC_F_OPEN | _WC_F_LINE,  EOF },
    { 2, NULL, 0, 0, 0, _WC_F_WRITE | _WC_F_OPEN | _WC_F_NOBUF, EOF },
};

FILE *stdin  = &std_files[0];
FILE *stdout = &std_files[1];
FILE *stderr = &std_files[2];

/* Every stream that has ever been opened, so exit() can flush them.  A fixed
 * table rather than a list: FOPEN_MAX is a promise about how many streams can
 * be open at once, and a table is the promise written down. */
static FILE *open_streams[FOPEN_MAX];
static int   open_count;

static void remember(FILE *f)
{
    if (open_count < FOPEN_MAX)
        open_streams[open_count++] = f;
}

static void forget(FILE *f)
{
    for (int i = 0; i < open_count; i++)
        if (open_streams[i] == f) {
            open_streams[i] = open_streams[--open_count];
            return;
        }
}

/* Called from exit(). Anything still in a write buffer goes out now. */
void _wc_flush_all(void)
{
    fflush(stdout);
    fflush(stderr);

    for (int i = 0; i < open_count; i++)
        fflush(open_streams[i]);
}

/* Give the stream a buffer if it should have one and has not got one yet.
 * Failing to allocate is not an error: the stream simply stays unbuffered,
 * which is slower and correct. */
static void want_buffer(FILE *f)
{
    if (f->buf || (f->flags & _WC_F_NOBUF))
        return;

    f->buf = malloc(BUFSIZ);
    if (!f->buf) {
        f->flags |= _WC_F_NOBUF;
        return;
    }

    f->size   = BUFSIZ;
    f->flags |= _WC_F_OWNBUF;
}

/* Hand the buffered bytes to the kernel.  Returns 0, or EOF with the error
 * flag set. */
static int flush_write(FILE *f)
{
    if (!(f->flags & _WC_F_WRITING) || f->pos == 0)
        return 0;

    int done = 0;
    while (done < f->pos) {
        int n = wwrite(f->fd, f->buf + done, (wsize_t)(f->pos - done));
        if (n <= 0) {
            /* What could not be written stays in the buffer, so a caller that
             * retries after making room does not lose it. */
            if (done > 0) {
                memmove(f->buf, f->buf + done, (wsize_t)(f->pos - done));
                f->pos -= done;
            }
            _wc_errno(n < 0 ? n : -W_EIO);
            f->flags |= _WC_F_ERR;
            return EOF;
        }
        done += n;
    }

    f->pos = 0;
    return 0;
}

/* Fill the buffer from the file. Returns the number of bytes now available,
 * 0 at end of file, or -1 on an error. */
static int fill_read(FILE *f)
{
    want_buffer(f);

    if (!f->buf) {
        /* An unbuffered read stream still has to produce a byte, and one byte
         * is what it does. */
        return 0;
    }

    f->pos = 0;
    f->len = 0;

    int n = wread(f->fd, f->buf, (wsize_t)f->size);
    if (n < 0) {
        _wc_errno(n);
        f->flags |= _WC_F_ERR;
        return -1;
    }
    if (n == 0) {
        f->flags |= _WC_F_EOF;
        return 0;
    }

    f->len = n;
    return n;
}

/* Leave whichever mode the stream is in, so it can enter the other one. */
static int leave_write_mode(FILE *f)
{
    if (f->flags & _WC_F_WRITING) {
        if (flush_write(f) != 0)
            return EOF;
        f->flags &= ~_WC_F_WRITING;
    }
    return 0;
}

/* Bytes read ahead of the program, which ftell() has to subtract. */
static int read_ahead(FILE *f)
{
    int held = f->len - f->pos;

    if (f->ungot != EOF)
        held++;
    return held;
}

/* Give back what was read ahead: the descriptor is moved back to where the
 * program thinks it is. */
static void drop_read_ahead(FILE *f)
{
    int held = read_ahead(f);

    if (held > 0)
        wlseek(f->fd, -held, W_SEEK_CUR);

    f->pos   = 0;
    f->len   = 0;
    f->ungot = EOF;
}

/* ------------------------------------------------------------------ *
 *  Opening and closing
 * ------------------------------------------------------------------ */

/* "r", "w", "a", any of them with "+", and a "b" that is ignored because
 * there is no text mode here to be the other thing. */
static int mode_flags(const char *mode, int *stream_flags)
{
    int update = 0;

    for (const char *p = mode; *p; p++)
        if (*p == '+')
            update = 1;

    switch (mode[0]) {
    case 'r':
        *stream_flags = update ? (_WC_F_READ | _WC_F_WRITE) : _WC_F_READ;
        return update ? W_O_RDWR : W_O_RDONLY;
    case 'w':
        *stream_flags = update ? (_WC_F_READ | _WC_F_WRITE) : _WC_F_WRITE;
        return (update ? W_O_RDWR : W_O_WRONLY) | W_O_CREAT | W_O_TRUNC;
    case 'a':
        *stream_flags = update ? (_WC_F_READ | _WC_F_WRITE) : _WC_F_WRITE;
        return (update ? W_O_RDWR : W_O_WRONLY) | W_O_CREAT | W_O_APPEND;
    default:
        return -1;
    }
}

FILE *fdopen(int fd, const char *mode)
{
    int stream_flags = 0;

    if (mode_flags(mode, &stream_flags) < 0) {
        errno = EINVAL;
        return NULL;
    }

    FILE *f = calloc(1, sizeof(*f));
    if (!f) {
        errno = ENOMEM;
        return NULL;
    }

    f->fd    = fd;
    f->flags = stream_flags | _WC_F_OPEN;
    f->ungot = EOF;

    remember(f);
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int stream_flags = 0;
    int open_flags   = mode_flags(mode, &stream_flags);

    if (open_flags < 0) {
        errno = EINVAL;
        return NULL;
    }

    int fd = wopen(path, open_flags);
    if (fd < 0) {
        _wc_errno(fd);
        return NULL;
    }

    FILE *f = fdopen(fd, mode);
    if (!f) {
        wclose(fd);
        return NULL;
    }

    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    if (!f)
        return NULL;

    fflush(f);
    if (f->fd > 2)
        wclose(f->fd);

    int stream_flags = 0;
    int open_flags   = mode_flags(mode, &stream_flags);

    if (open_flags < 0) {
        errno = EINVAL;
        return NULL;
    }

    int fd = wopen(path, open_flags);
    if (fd < 0) {
        _wc_errno(fd);
        return NULL;
    }

    f->fd    = fd;
    f->flags = stream_flags | _WC_F_OPEN | (f->flags & (_WC_F_OWNBUF));
    f->pos   = 0;
    f->len   = 0;
    f->ungot = EOF;
    return f;
}

int fclose(FILE *f)
{
    if (!f)
        return EOF;

    int status = fflush(f);

    /* The three standard streams share their descriptors with the console and
     * with anything the shell wired them to, so closing one closes what the
     * program was given rather than something it opened. */
    if (wclose(f->fd) < 0)
        status = EOF;

    if (f->flags & _WC_F_OWNBUF)
        free(f->buf);

    f->flags = 0;
    f->buf   = NULL;

    if (f != stdin && f != stdout && f != stderr) {
        forget(f);
        free(f);
    }

    return status;
}

int fflush(FILE *f)
{
    /* fflush(NULL) means every output stream, which is what exit() wants. */
    if (!f) {
        _wc_flush_all();
        return 0;
    }

    return leave_write_mode(f);
}

int fileno(FILE *f)
{
    return f ? f->fd : -1;
}

void setbuf(FILE *f, char *buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if (!f)
        return EOF;

    fflush(f);

    if (f->flags & _WC_F_OWNBUF)
        free(f->buf);

    f->buf    = NULL;
    f->size   = 0;
    f->pos    = 0;
    f->len    = 0;
    f->flags &= ~(_WC_F_OWNBUF | _WC_F_NOBUF | _WC_F_LINE);

    switch (mode) {
    case _IONBF:
        f->flags |= _WC_F_NOBUF;
        return 0;
    case _IOLBF:
        f->flags |= _WC_F_LINE;
        break;
    case _IOFBF:
        break;
    default:
        errno = EINVAL;
        return EOF;
    }

    if (buf && size > 0) {
        f->buf  = buf;
        f->size = (int)size;
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Reading
 * ------------------------------------------------------------------ */

int fgetc(FILE *f)
{
    if (!f || !(f->flags & _WC_F_READ)) {
        errno = EBADF;
        return EOF;
    }

    if (leave_write_mode(f) != 0)
        return EOF;

    if (f->ungot != EOF) {
        int c = f->ungot;
        f->ungot = EOF;
        return c;
    }

    if (f->pos == f->len) {
        if (f->flags & _WC_F_EOF)
            return EOF;

        int n = fill_read(f);
        if (n <= 0) {
            /* Unbuffered, or a stream whose buffer could not be allocated:
             * read the one byte directly. */
            if (n == 0 && !f->buf) {
                unsigned char c;
                int got = wread(f->fd, &c, 1);

                if (got < 0) {
                    _wc_errno(got);
                    f->flags |= _WC_F_ERR;
                    return EOF;
                }
                if (got == 0) {
                    f->flags |= _WC_F_EOF;
                    return EOF;
                }
                return c;
            }
            return EOF;
        }
    }

    return (unsigned char)f->buf[f->pos++];
}

int getc(FILE *f)      { return fgetc(f); }
int getchar(void)      { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (!f || c == EOF || f->ungot != EOF)
        return EOF;

    /* Pushing back into the buffer where it came from would be faster and
     * would also let a program push back a character that was never there and
     * corrupt what follows.  One slot, always. */
    f->ungot  = (unsigned char)c;
    f->flags &= ~_WC_F_EOF;
    return (unsigned char)c;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *f)
{
    if (size == 0 || count == 0)
        return 0;
    if (!f || !(f->flags & _WC_F_READ)) {
        errno = EBADF;
        return 0;
    }
    if (leave_write_mode(f) != 0)
        return 0;

    unsigned char *out   = ptr;
    size_t         want  = size * count;
    size_t         done  = 0;

    while (done < want) {
        if (f->ungot != EOF) {
            out[done++] = (unsigned char)f->ungot;
            f->ungot = EOF;
            continue;
        }

        int held = f->len - f->pos;

        if (held > 0) {
            size_t take = (size_t)held;
            if (take > want - done)
                take = want - done;

            memcpy(out + done, f->buf + f->pos, take);
            f->pos += (int)take;
            done   += take;
            continue;
        }

        if (f->flags & _WC_F_EOF)
            break;

        /* A read larger than the buffer goes straight into the caller's
         * memory: copying a megabyte through a kilobyte buffer is a thousand
         * copies of the same bytes, and a compiler reading a source file asks
         * for the whole thing at once. */
        if (want - done >= BUFSIZ) {
            int n = wread(f->fd, out + done, want - done);

            if (n < 0) {
                _wc_errno(n);
                f->flags |= _WC_F_ERR;
                break;
            }
            if (n == 0) {
                f->flags |= _WC_F_EOF;
                break;
            }
            done += (size_t)n;
            continue;
        }

        if (fill_read(f) <= 0) {
            if (!f->buf) {
                /* Unbuffered: read what is left directly. */
                int n = wread(f->fd, out + done, want - done);
                if (n <= 0) {
                    if (n < 0) {
                        _wc_errno(n);
                        f->flags |= _WC_F_ERR;
                    } else {
                        f->flags |= _WC_F_EOF;
                    }
                    break;
                }
                done += (size_t)n;
                continue;
            }
            break;
        }
    }

    return done / size;
}

char *fgets(char *s, int size, FILE *f)
{
    if (size <= 0)
        return NULL;

    int i = 0;

    while (i < size - 1) {
        int c = fgetc(f);

        if (c == EOF)
            break;

        s[i++] = (char)c;
        if (c == '\n')
            break;
    }

    if (i == 0)
        return NULL;

    s[i] = '\0';
    return s;
}

/* ------------------------------------------------------------------ *
 *  Writing
 * ------------------------------------------------------------------ */

/* One byte into the stream, buffering it or not as the stream says. */
static int put_byte(FILE *f, unsigned char c)
{
    if (f->flags & _WC_F_NOBUF) {
        if (wwrite(f->fd, &c, 1) != 1) {
            f->flags |= _WC_F_ERR;
            return EOF;
        }
        return c;
    }

    want_buffer(f);
    if (!f->buf) {
        if (wwrite(f->fd, &c, 1) != 1) {
            f->flags |= _WC_F_ERR;
            return EOF;
        }
        return c;
    }

    f->flags |= _WC_F_WRITING;
    f->buf[f->pos++] = (char)c;

    if (f->pos == f->size || ((f->flags & _WC_F_LINE) && c == '\n')) {
        if (flush_write(f) != 0)
            return EOF;
    }

    return c;
}

int fputc(int c, FILE *f)
{
    if (!f || !(f->flags & _WC_F_WRITE)) {
        errno = EBADF;
        return EOF;
    }

    /* Coming from reading: give back the read-ahead first, or the bytes would
     * be written where the program has already been shown past. */
    if (!(f->flags & _WC_F_WRITING) && read_ahead(f) > 0)
        drop_read_ahead(f);

    return put_byte(f, (unsigned char)c);
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    while (*s)
        if (fputc((unsigned char)*s++, f) == EOF)
            return EOF;

    return 0;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;

    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *f)
{
    if (size == 0 || count == 0)
        return 0;
    if (!f || !(f->flags & _WC_F_WRITE)) {
        errno = EBADF;
        return 0;
    }

    if (!(f->flags & _WC_F_WRITING) && read_ahead(f) > 0)
        drop_read_ahead(f);

    const unsigned char *in   = ptr;
    size_t               want = size * count;

    /* Big writes go straight out, after what is buffered: putting a hundred
     * kilobytes through a one-kilobyte buffer is a hundred round trips for no
     * gain, and a linker writing an executable does exactly that. */
    if (want >= BUFSIZ && !(f->flags & _WC_F_LINE)) {
        if (leave_write_mode(f) != 0)
            return 0;

        size_t done = 0;
        while (done < want) {
            int n = wwrite(f->fd, in + done, want - done);
            if (n <= 0) {
                _wc_errno(n < 0 ? n : -W_EIO);
                f->flags |= _WC_F_ERR;
                break;
            }
            done += (size_t)n;
        }
        return done / size;
    }

    size_t done = 0;
    while (done < want) {
        if (put_byte(f, in[done]) == EOF)
            break;
        done++;
    }

    return done / size;
}

/* ------------------------------------------------------------------ *
 *  Position
 * ------------------------------------------------------------------ */

int fseek(FILE *f, long offset, int whence)
{
    if (!f) {
        errno = EBADF;
        return -1;
    }

    if (leave_write_mode(f) != 0)
        return -1;

    /* A seek relative to the current position means the program's position,
     * not the descriptor's -- and those differ by whatever was read ahead. */
    if (whence == SEEK_CUR)
        offset -= read_ahead(f);

    f->pos   = 0;
    f->len   = 0;
    f->ungot = EOF;

    int r = wlseek(f->fd, (int)offset, whence);
    if (r < 0) {
        _wc_errno(r);
        return -1;
    }

    f->flags &= ~_WC_F_EOF;
    return 0;
}

long ftell(FILE *f)
{
    if (!f) {
        errno = EBADF;
        return -1;
    }

    int at = wlseek(f->fd, 0, W_SEEK_CUR);
    if (at < 0) {
        _wc_errno(at);
        return -1;
    }

    /* The descriptor is ahead of the program by what was read into the buffer
     * and not yet handed out, and behind it by what the program has written
     * and this layer has not yet passed on. */
    if (f->flags & _WC_F_WRITING)
        return (long)at + f->pos;

    return (long)at - read_ahead(f);
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    if (f)
        f->flags &= ~_WC_F_ERR;
}

/* ------------------------------------------------------------------ *
 *  State, and the two file operations that live in this header
 * ------------------------------------------------------------------ */

int feof(FILE *f)   { return f && (f->flags & _WC_F_EOF) ? 1 : 0; }
int ferror(FILE *f) { return f && (f->flags & _WC_F_ERR) ? 1 : 0; }

void clearerr(FILE *f)
{
    if (f)
        f->flags &= ~(_WC_F_EOF | _WC_F_ERR);
}

int remove(const char *path)
{
    int r = wunlink(path);

    /* The standard's remove() takes a directory too, where unlink() does not;
     * the second try is what makes it. */
    if (r == -W_EISDIR)
        r = wrmdir(path);

    return _wc_errno(r) < 0 ? -1 : 0;
}

int rename(const char *from, const char *to)
{
    return _wc_errno(wrename(from, to)) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ *
 *  Formatted output
 *
 *  All of it goes through wkernel's formatter.  One implementation of the
 *  format-string rules on this machine, not two that drift apart.
 * ------------------------------------------------------------------ */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    return wvsnprintf(buf, size, fmt, ap);
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    /* No size to respect, which is why sprintf() is the function every
     * security advisory is about.  The bound here is the largest thing the
     * formatter will produce into a buffer, not a promise about the caller's. */
    return wvsnprintf(buf, (wsize_t)-1, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char stack_buf[512];

    /* Formatted into memory first, then written as one run: a format string
     * that produced its output character by character through this layer would
     * flush a line-buffered stream in the middle of a line. */
    va_list count_ap;
    va_copy(count_ap, ap);
    int need = wvsnprintf(stack_buf, sizeof(stack_buf), fmt, count_ap);
    va_end(count_ap);

    if (need < 0)
        return -1;

    if ((size_t)need < sizeof(stack_buf))
        return (int)fwrite(stack_buf, 1, (size_t)need, f) == need ? need : -1;

    char *big = malloc((size_t)need + 1);
    if (!big)
        return -1;

    wvsnprintf(big, (wsize_t)need + 1, fmt, ap);
    int wrote = (int)fwrite(big, 1, (size_t)need, f);
    free(big);

    return wrote == need ? need : -1;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = wvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
