/* <stdbool.h> -- the spelling C99 gave to something the language already had.
 *
 * `_Bool` is a real type in the compiler; these three names are all this
 * header has ever been.
 */
#ifndef WCC_STDBOOL_H
#define WCC_STDBOOL_H

#define bool  _Bool
#define true  1
#define false 0

#define __bool_true_false_are_defined 1

#endif /* WCC_STDBOOL_H */
