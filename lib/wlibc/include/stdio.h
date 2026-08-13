/* <stdio.h> -- streams, over the WOS file descriptors.
 *
 * Part of the hosted C library described in docs/libc.md: the half of the C
 * standard that a program ported from Unix expects to find, written on top of
 * wkernel rather than instead of it.
 *
 * A FILE here is a descriptor, a buffer and a position.  Buffering is the
 * whole reason this exists rather than a set of one-line wrappers: WFS writes a
 * whole 1 KiB block per write call, so a compiler emitting an object file one
 * byte at a time through wwrite() would write a thousand blocks per kilobyte.
 */
#ifndef WLIBC_STDIO_H
#define WLIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

/* The buffer a stream takes when it first needs one.  A WFS block, because
 * that is the unit the filesystem reads and writes underneath. */
#define BUFSIZ 1024

/* fseek() and friends. Same values as W_SEEK_*, which is not a coincidence:
 * they go straight through to wlseek(). */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* setvbuf() modes. */
#define _IOFBF 0    /* fully buffered   */
#define _IOLBF 1    /* line buffered    */
#define _IONBF 2    /* unbuffered       */

#define FOPEN_MAX 16
#define FILENAME_MAX 256
#define L_tmpnam 32

typedef struct _wc_file FILE;

struct _wc_file {
    int   fd;
    char *buf;
    int   size;        /* capacity of buf                                  */
    int   pos;         /* reading: next byte to hand out; writing: next gap */
    int   len;         /* reading: how many bytes in buf are valid          */
    int   flags;       /* _WC_F_* below                                     */
    int   ungot;       /* a character pushed back, or EOF                   */
};

/* Flags. Public only because the FILE structure is, and the FILE structure is
 * public because a program is allowed to declare one. */
#define _WC_F_READ   0x0001
#define _WC_F_WRITE  0x0002
#define _WC_F_EOF    0x0004
#define _WC_F_ERR    0x0008
#define _WC_F_WRITING 0x0010   /* the buffer holds bytes not yet written    */
#define _WC_F_OWNBUF 0x0020    /* the buffer came from malloc               */
#define _WC_F_LINE   0x0040    /* flush at every newline                    */
#define _WC_F_NOBUF  0x0080    /* write straight through                    */
#define _WC_F_OPEN   0x0100

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *fdopen(int fd, const char *mode);
int   fclose(FILE *f);
int   fflush(FILE *f);
int   fileno(FILE *f);
void  setbuf(FILE *f, char *buf);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);

size_t fread(void *ptr, size_t size, size_t count, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *f);

int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);

int   fgetc(FILE *f);
int   getc(FILE *f);
int   getchar(void);
char *fgets(char *s, int size, FILE *f);
int   ungetc(int c, FILE *f);

int   fputc(int c, FILE *f);
int   putc(int c, FILE *f);
int   putchar(int c);
int   fputs(const char *s, FILE *f);
int   puts(const char *s);

int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);

int   remove(const char *path);
int   rename(const char *from, const char *to);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

void perror(const char *s);

#endif /* WLIBC_STDIO_H */
