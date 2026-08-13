/* <stddef.h> -- the types the language itself needs names for.
 *
 * This header belongs to the compiler rather than to a C library: `size_t` is
 * whatever `sizeof` produces, and only the compiler knows that.  wcc installs
 * it at /lib/wcc/include and looks there before /include.
 */
#ifndef WCC_STDDEF_H
#define WCC_STDDEF_H

typedef unsigned long size_t;
typedef long          ptrdiff_t;
typedef int           wchar_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* The distance from the start of a structure to one of its members.  The
 * usual definition: pretend there is one at address zero and ask where the
 * member is. */
#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#endif /* WCC_STDDEF_H */
