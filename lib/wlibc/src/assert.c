/* What happens when a program's claim about itself turns out to be false. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void _wc_assert_fail(const char *claim, const char *file, int line,
                     const char *function)
{
    /* The same sentence glibc prints, because it is the sentence people know
     * how to read: where it was, which function, and what was claimed. */
    fprintf(stderr, "%s:%d: %s: Assertion `%s' failed.\n",
            file, line, function, claim);

    abort();
}
