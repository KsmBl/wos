/* <stdlib.h> -- memory, conversion, sorting and leaving.
 *
 * The heap is wkernel's: malloc, calloc, realloc and free are the same
 * functions the rest of the system uses, declared here under the names a
 * ported program looks for.  There is one allocator on this machine, not two.
 */
#ifndef WLIBC_STDLIB_H
#define WLIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* rand() is a 31-bit linear congruential generator; this is its ceiling. */
#define RAND_MAX 0x7FFFFFFF

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

void  exit(int status) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   atexit(void (*fn)(void));

int   atoi(const char *s);
long  atol(const char *s);

long          strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long     strtoll(const char *s, char **end, int base);

int  abs(int v);
long labs(long v);

void  qsort(void *base, size_t count, size_t size,
            int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

int  rand(void);
void srand(unsigned int seed);

/* There are no environment variables on this machine, so this always answers
 * that the variable is not set.  It is here because ported code asks. */
char *getenv(const char *name);

#endif /* WLIBC_STDLIB_H */
