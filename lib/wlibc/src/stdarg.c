/* Walking the arguments a function was not declared to have.
 *
 * On x86-64 the first six integer arguments arrive in registers and the rest
 * on the stack, so a va_list is two cursors and a rule for changing over.  The
 * compiler spills the six registers into the function's own frame and calls
 * __va_start() to record where; everything after that is this file, which is
 * ordinary C and needs nothing from the compiler.
 *
 * The structure is the one the System V ABI describes, so a va_list built here
 * and one built by gcc are the same thing -- which is what lets a program
 * compiled by wcc pass a va_list to wvsnprintf() in a library compiled by gcc.
 */

#include <stdarg.h>

/* The compiler calls this from va_start(). `save_area` is where it put the six
 * registers, `overflow` is the first argument that went on the stack, and
 * `gp_offset` is how far into the save area the named parameters already
 * reached -- eight bytes each. */
void __va_start(va_list ap, void *save_area, void *overflow,
                unsigned int gp_offset)
{
    ap[0].gp_offset         = gp_offset;
    ap[0].fp_offset         = 48;      /* no floating point here, ever */
    ap[0].reg_save_area     = save_area;
    ap[0].overflow_arg_area = overflow;
}

/* Where the next argument is.  va_arg() is a macro that reads through the
 * pointer this returns, which is what lets one function serve every type.
 *
 * Anything larger than eight bytes -- a structure passed by value -- is not
 * handled: it is passed in pieces the ABI describes at length, and nothing on
 * this machine does it. */
void *__va_arg(va_list ap, unsigned long size)
{
    (void)size;

    if (ap[0].gp_offset < 48) {
        void *at = (char *)ap[0].reg_save_area + ap[0].gp_offset;

        ap[0].gp_offset += 8;
        return at;
    }

    void *at = ap[0].overflow_arg_area;

    ap[0].overflow_arg_area = (char *)ap[0].overflow_arg_area + 8;
    return at;
}
