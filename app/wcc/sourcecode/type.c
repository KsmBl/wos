/* What a type is.
 *
 * Sizes and alignments are LP64 and x86-64: char 1, short 2, int 4, long and
 * every pointer 8, each aligned to its own size.  A structure is aligned to
 * its widest member and padded up to a multiple of that, which is what makes
 * an array of them work.
 */

#include <stdlib.h>
#include <string.h>

#include "wcc.h"

static type_t base_types[] = {
    { TY_VOID,  1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_BOOL,  1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_CHAR,  1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_SHORT, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_INT,   4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_LONG,  8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_CHAR,  1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_SHORT, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_INT,   4, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { TY_LONG,  8, 8, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
};

type_t *ty_void   = &base_types[0];
type_t *ty_bool   = &base_types[1];
type_t *ty_char   = &base_types[2];
type_t *ty_short  = &base_types[3];
type_t *ty_int    = &base_types[4];
type_t *ty_long   = &base_types[5];
type_t *ty_uchar  = &base_types[6];
type_t *ty_ushort = &base_types[7];
type_t *ty_uint   = &base_types[8];
type_t *ty_ulong  = &base_types[9];

type_t *new_type(type_kind_t kind, int size, int align)
{
    type_t *t = wcc_alloc(sizeof(*t));

    t->kind  = kind;
    t->size  = size;
    t->align = align;
    return t;
}

type_t *pointer_to(type_t *base)
{
    type_t *t = new_type(TY_PTR, 8, 8);

    t->base = base;

    /* A pointer is an address, and an address is not a negative number: the
     * comparison and the division of one are unsigned. */
    t->is_unsigned = 1;
    return t;
}

type_t *array_of(type_t *base, int len)
{
    type_t *t = new_type(TY_ARRAY, base->size * (len < 0 ? 0 : len),
                         base->align);

    t->base      = base;
    t->array_len = len;
    return t;
}

type_t *func_type(type_t *return_type)
{
    /* A function has no size of its own; sizeof on one is not allowed, and
     * what programs actually take is the size of a pointer to it. */
    type_t *t = new_type(TY_FUNC, 1, 1);

    t->return_type = return_type;
    return t;
}

int is_integer(const type_t *t)
{
    return t->kind == TY_BOOL || t->kind == TY_CHAR || t->kind == TY_SHORT ||
           t->kind == TY_INT  || t->kind == TY_LONG || t->kind == TY_ENUM;
}

int is_pointer_like(const type_t *t)
{
    return t->kind == TY_PTR || t->kind == TY_ARRAY;
}
