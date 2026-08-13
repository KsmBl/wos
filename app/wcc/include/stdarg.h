/* <stdarg.h> -- reading the arguments a function was not declared to have.
 *
 * On x86-64 the first six integer arguments arrive in registers and the rest
 * on the stack, so walking them is not a matter of stepping a pointer: it has
 * to read the registers first and then change over.  wcc handles that by
 * having a variadic function spill all six registers into a save area in its
 * own frame, so `va_list` is a cursor over that area and then over the stack.
 *
 * The structure is the one the ABI describes, so a `va_list` this compiler
 * produces and one the host's compiler produced have the same shape.
 */
#ifndef WCC_STDARG_H
#define WCC_STDARG_H

/* `__builtin_va_list` is the compiler's own type -- an array of one record
 * holding the cursor, so that passing a va_list passes its address without
 * every caller having to write the &. */
typedef __builtin_va_list va_list;

/* Only va_start needs the compiler: it is the one that knows where the
 * registers were spilled.  Stepping through the arguments afterwards is
 * ordinary code, and lives in the C library. */
void  __va_start(va_list ap, void *save_area, void *overflow,
                 unsigned int gp_offset);
void *__va_arg(va_list ap, unsigned long size);

#define va_start(ap, last) __builtin_va_start(ap)
#define va_arg(ap, type)   (*(type *)__va_arg(ap, sizeof(type)))
#define va_end(ap)         ((void)0)
#define va_copy(to, from)  ((to)[0] = (from)[0])

#endif /* WCC_STDARG_H */
