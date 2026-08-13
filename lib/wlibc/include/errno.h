/* <errno.h> -- the last failure, as a number a ported program can read.
 *
 * wkernel does not have one: every call returns its own negated error code, so
 * there is nothing global to get out of step with the call that set it.  This
 * is the other convention, and the library keeps it up to date at the boundary
 * -- a stdio call that fails sets `errno` from what the wkernel call underneath
 * it returned.
 *
 * The numbers are the W_E* ones from <wabi.h>, which are the traditional Linux
 * values, so a program that compares against ENOENT gets the 2 it expects.
 */
#ifndef WLIBC_ERRNO_H
#define WLIBC_ERRNO_H

extern int errno;

#define EPERM          1
#define ENOENT         2
#define ESRCH          3
#define EINTR          4
#define EIO            5
#define E2BIG          7
#define ENOEXEC        8
#define EBADF          9
#define ECHILD        10
#define EAGAIN        11
#define ENOMEM        12
#define EACCES        13
#define EFAULT        14
#define EBUSY         16
#define EEXIST        17
#define EXDEV         18
#define ENODEV        19
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define ENFILE        23
#define EMFILE        24
#define EFBIG         27
#define ENOSPC        28
#define ESPIPE        29
#define EROFS         30
#define EPIPE         32
#define ERANGE        34
#define ENAMETOOLONG  36
#define ENOSYS        38
#define ENOTEMPTY     39
#define ECONNRESET    104
#define ETIMEDOUT     110
#define ECONNREFUSED  111
#define EHOSTUNREACH  113

/* Set errno from a wkernel return value and pass the value back, so a wrapper
 * can say `return _wc_errno(wopen(...))`.  A non-negative value is a success
 * and leaves errno alone, as the standard requires. */
int _wc_errno(int r);

#endif /* WLIBC_ERRNO_H */
