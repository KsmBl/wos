/* The global that says what went wrong last.
 *
 * Every wkernel call reports its own failure in its own return value, which is
 * the better arrangement and not the one C programs are written against.  This
 * is the bridge: one place where a negative wkernel result becomes a positive
 * errno, so no wrapper in this library has to remember to negate.
 */

#include <errno.h>

int errno;

int _wc_errno(int r)
{
    /* Success leaves errno alone.  The standard is explicit that a call which
     * succeeds may not clear it, because a program is allowed to set it to
     * zero, make a call, and look afterwards. */
    if (r < 0)
        errno = -r;

    return r;
}
