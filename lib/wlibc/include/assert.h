/* <assert.h> -- a claim the program makes about itself.
 *
 * A failed assertion prints the file, the line and the text of the claim, and
 * stops the process with abort().  Deliberately not a no-op when NDEBUG is
 * absent and deliberately compiled out when it is present, which is the one
 * thing every C program assumes about this header.
 */
#ifndef WLIBC_ASSERT_H
#define WLIBC_ASSERT_H

/* No include guard on the macro itself: <assert.h> is the one header the
 * standard says may be included repeatedly with a different NDEBUG each time,
 * so the definition below is undefined first every time. */
#undef assert

#ifdef NDEBUG
#define assert(claim) ((void)0)
#else
#define assert(claim) \
    ((claim) ? (void)0 : _wc_assert_fail(#claim, __FILE__, __LINE__, __func__))
#endif

void _wc_assert_fail(const char *claim, const char *file, int line,
                     const char *function) __attribute__((__noreturn__));

#endif /* WLIBC_ASSERT_H */
