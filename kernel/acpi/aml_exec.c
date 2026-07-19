/* Running AML.
 *
 * A method body is a TermList, and a term is either something with a value --
 * an addition, a field read, a call -- or something without one, like a Store
 * or an If.  Evaluating is therefore one recursive walk in which most opcodes
 * take a fixed number of operands, each of which is itself a term.  There is
 * no precedence and no parsing to speak of: the bytecode is already a tree.
 *
 * Two things about the language are worth stating before the code, because
 * they are where an interpreter written from the opcode table alone goes
 * wrong.
 *
 * Most operators end with a Target -- `Add(A, B, C)` means C = A + B, and the
 * result is *also* the value of the expression, so `Store(Add(A, B, C), D)`
 * puts the sum in both.  The target may be the null name, which means discard,
 * and that is by far the commonest case.
 *
 * And a name used as an operand is not a name, it is whatever the name holds:
 * a field read reaches the hardware, and a method is *called*, taking its
 * arguments from the terms that follow.  So the same four characters may be a
 * value or a call depending only on what the namespace says they are, which is
 * why nothing here can be decided without the load pass having run first.
 */

#include "aml_internal.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

/* Values, arguments and locals. */
#define OP_ZERO        0x00
#define OP_ONE         0x01
#define OP_BYTE        0x0A
#define OP_WORD        0x0B
#define OP_DWORD       0x0C
#define OP_STRING      0x0D
#define OP_QWORD       0x0E
#define OP_BUFFER      0x11
#define OP_PACKAGE     0x12
#define OP_VAR_PACKAGE 0x13
#define OP_METHOD      0x14
#define OP_NAME        0x08
#define OP_SCOPE       0x10
#define OP_ALIAS       0x06
#define OP_EXTERNAL    0x15
#define OP_LOCAL0      0x60
#define OP_ARG0        0x68
#define OP_STORE       0x70
#define OP_REF_OF      0x71
#define OP_ADD         0x72
#define OP_CONCAT      0x73
#define OP_SUBTRACT    0x74
#define OP_INCREMENT   0x75
#define OP_DECREMENT   0x76
#define OP_MULTIPLY    0x77
#define OP_DIVIDE      0x78
#define OP_SHIFT_LEFT  0x79
#define OP_SHIFT_RIGHT 0x7A
#define OP_AND         0x7B
#define OP_NAND        0x7C
#define OP_OR          0x7D
#define OP_NOR         0x7E
#define OP_XOR         0x7F
#define OP_NOT         0x80
#define OP_FIND_LEFT   0x81
#define OP_FIND_RIGHT  0x82
#define OP_DEREF_OF    0x83
#define OP_CONCAT_RES  0x84
#define OP_MOD         0x85
#define OP_NOTIFY      0x86
#define OP_SIZE_OF     0x87
#define OP_INDEX       0x88
#define OP_MATCH       0x89
#define OP_CREATE_DW   0x8A
#define OP_CREATE_W    0x8B
#define OP_CREATE_B    0x8C
#define OP_CREATE_BIT  0x8D
#define OP_OBJECT_TYPE 0x8E
#define OP_CREATE_QW   0x8F
#define OP_LAND        0x90
#define OP_LOR         0x91
#define OP_LNOT        0x92
#define OP_LEQUAL      0x93
#define OP_LGREATER    0x94
#define OP_LLESS       0x95
#define OP_TO_BUFFER   0x96
#define OP_TO_DEC_STR  0x97
#define OP_TO_HEX_STR  0x98
#define OP_TO_INTEGER  0x99
#define OP_TO_STRING   0x9C
#define OP_COPY_OBJECT 0x9D
#define OP_MID         0x9E
#define OP_CONTINUE    0x9F
#define OP_IF          0xA0
#define OP_ELSE        0xA1
#define OP_WHILE       0xA2
#define OP_NOOP        0xA3
#define OP_RETURN      0xA4
#define OP_BREAK       0xA5
#define OP_BREAKPOINT  0xCC
#define OP_ONES        0xFF
#define OP_EXT         0x5B

#define EXT_MUTEX      0x01
#define EXT_EVENT      0x02
#define EXT_COND_REF   0x12
#define EXT_CREATE_FLD 0x13
#define EXT_LOAD       0x20
#define EXT_STALL      0x21
#define EXT_SLEEP      0x22
#define EXT_ACQUIRE    0x23
#define EXT_SIGNAL     0x24
#define EXT_WAIT       0x25
#define EXT_RESET      0x26
#define EXT_RELEASE    0x27
#define EXT_FROM_BCD   0x28
#define EXT_TO_BCD     0x29
#define EXT_REVISION   0x30
#define EXT_DEBUG      0x31
#define EXT_FATAL      0x32
#define EXT_TIMER      0x33
#define EXT_REGION     0x80
#define EXT_FIELD      0x81
#define EXT_DEVICE     0x82
#define EXT_PROCESSOR  0x83
#define EXT_POWER_RES  0x84
#define EXT_THERMAL    0x85
#define EXT_INDEX_FLD  0x86
#define EXT_BANK_FLD   0x87

/* A method that calls itself deeper than this is a firmware bug, and following
 * it would take the kernel stack with it. */
#define MAX_DEPTH 24

/* ------------------------------------------------------------------ *
 *  Where a result can be put
 * ------------------------------------------------------------------ */

typedef struct {
    enum { TGT_NONE, TGT_NODE, TGT_LOCAL, TGT_ARG, TGT_ELEMENT } kind;
    aml_node_t   *node;
    int           index;
    aml_object_t *container;     /* for TGT_ELEMENT: the buffer or package */
} target_t;

static aml_object_t *eval_operand(aml_stream_t *s, aml_ctx_t *ctx, bool *failed);
static bool parse_target(aml_stream_t *s, aml_ctx_t *ctx, target_t *out);
static bool store_value(target_t *t, aml_object_t *value);

/* A copy that a second owner can hold without the first one's changes showing
 * through.  AML's Store is a copy, not an aliasing assignment. */
static aml_object_t *clone(aml_object_t *o)
{
    if (!o)
        return NULL;

    switch (o->type) {
    case AML_INTEGER:
        return aml_integer(o->integer);

    case AML_STRING: {
        aml_object_t *c = aml_buffer(o->buffer.bytes, o->buffer.len);
        if (c)
            c->type = AML_STRING;
        return c;
    }

    case AML_BUFFER:
        return aml_buffer(o->buffer.bytes, o->buffer.len);

    default:
        /* Packages, fields and references are shared rather than copied.  A
         * package copy would have to be deep, and nothing in the methods this
         * runs depends on one. */
        return aml_ref(o);
    }
}

/* ------------------------------------------------------------------ *
 *  Names as operands
 * ------------------------------------------------------------------ */

static aml_object_t *call_method(aml_node_t *node, aml_object_t **args,
                                 int argc, int depth);

/* What a node is worth when its name appears in an expression. */
static aml_object_t *value_of(aml_node_t *n)
{
    if (!n)
        return NULL;

    if (n->kind == AML_NODE_OBJECT && n->value) {
        if (n->value->type == AML_FIELD)
            return aml_field_read(n->value);      /* reaches the hardware */

        return aml_ref(n->value);
    }

    /* A device, a scope or a region used as a value is not worth anything,
     * but it is not an error either -- CondRefOf and ObjectType both ask. */
    return aml_integer(0);
}

/* A name in an operand position: look it up, and if it is a method, call it
 * with the terms that follow as its arguments. */
static aml_object_t *eval_name(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    char name[128];

    if (!aml_name_string(s, name, sizeof(name))) {
        *failed = true;
        return NULL;
    }

    aml_node_t *n = aml_lookup(ctx->scope, name);
    if (!n) {
        /* Firmware refers to names that other tables were supposed to define
         * and sometimes nothing does.  Zero is what an uninitialised object
         * is worth, and going on is better than abandoning the method. */
        return aml_integer(0);
    }

    if (n->kind != AML_NODE_METHOD)
        return value_of(n);

    aml_object_t *args[AML_MAX_ARGS];
    int           argc = n->arg_count > AML_MAX_ARGS ? AML_MAX_ARGS
                                                     : n->arg_count;

    for (int i = 0; i < argc; i++)
        args[i] = eval_operand(s, ctx, failed);

    aml_object_t *r = *failed ? NULL
                              : call_method(n, args, argc, ctx->depth + 1);

    for (int i = 0; i < argc; i++)
        aml_unref(args[i]);

    return r;
}

/* ------------------------------------------------------------------ *
 *  Operands
 * ------------------------------------------------------------------ */

static uint64_t integer_operand(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    aml_object_t *o = eval_operand(s, ctx, failed);
    uint64_t      v = 0;

    if (o && !aml_to_integer(o, &v))
        v = 0;

    aml_unref(o);
    return v;
}

/* An operand is any term with a value. */
static aml_object_t *eval_operand(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    if (*failed || !aml_left(s, 1))
        return NULL;

    return aml_eval_term(s, ctx, failed);
}

/* ------------------------------------------------------------------ *
 *  Targets
 * ------------------------------------------------------------------ */

static bool parse_target(aml_stream_t *s, aml_ctx_t *ctx, target_t *out)
{
    uint8_t op = aml_peek(s);

    memset(out, 0, sizeof(*out));

    if (op == 0x00) {                       /* the null name: discard */
        aml_byte(s);
        out->kind = TGT_NONE;
        return true;
    }

    if (op >= OP_LOCAL0 && op < OP_LOCAL0 + AML_MAX_LOCALS) {
        aml_byte(s);
        out->kind  = TGT_LOCAL;
        out->index = op - OP_LOCAL0;
        return true;
    }

    if (op >= OP_ARG0 && op < OP_ARG0 + AML_MAX_ARGS) {
        aml_byte(s);
        out->kind  = TGT_ARG;
        out->index = op - OP_ARG0;
        return true;
    }

    /* Index() as a target: `Store(x, Index(buf, 3))` writes one element. */
    if (op == OP_INDEX) {
        bool failed = false;

        aml_byte(s);

        aml_object_t *container = eval_operand(s, ctx, &failed);
        uint64_t      index     = integer_operand(s, ctx, &failed);

        target_t inner;
        parse_target(s, ctx, &inner);       /* Index takes a target too */

        if (failed || !container) {
            aml_unref(container);
            return false;
        }

        out->kind      = TGT_ELEMENT;
        out->container = container;         /* the target owns this reference */
        out->index     = (int)index;
        return true;
    }

    if (op == OP_DEREF_OF) {
        bool failed = false;

        aml_byte(s);

        aml_object_t *r = eval_operand(s, ctx, &failed);
        if (!failed && r && r->type == AML_REFERENCE && r->reference.is_index) {
            out->kind      = TGT_ELEMENT;
            out->container = aml_ref(r->reference.target);
            out->index     = (int)r->reference.index;
            aml_unref(r);
            return true;
        }

        aml_unref(r);
        out->kind = TGT_NONE;
        return true;
    }

    /* Anything else is a name. */
    char name[128];
    if (!aml_name_string(s, name, sizeof(name)))
        return false;

    out->kind = TGT_NODE;
    out->node = aml_lookup(ctx->scope, name);

    /* Storing to a name that does not exist creates it in the current scope,
     * which is what firmware expects when a method stores to a name it never
     * declared. */
    if (!out->node)
        out->node = aml_ns_create_path(ctx->scope, name, AML_NODE_OBJECT);

    return true;
}

static bool store_value(target_t *t, aml_object_t *value)
{
    if (!t)
        return false;

    switch (t->kind) {
    case TGT_NONE:
        return true;

    case TGT_LOCAL:
    case TGT_ARG:
        return true;      /* filled in by the caller, which has the context */

    case TGT_NODE: {
        if (!t->node)
            return false;

        /* A store into a field is a write to hardware, and takes the integer
         * value rather than replacing the field with the object. */
        if (t->node->kind == AML_NODE_OBJECT && t->node->value &&
            t->node->value->type == AML_FIELD) {
            uint64_t v = 0;
            aml_to_integer(value, &v);
            return aml_field_write(t->node->value, v);
        }

        aml_object_t *copy = clone(value);
        if (!copy)
            return false;

        aml_unref(t->node->value);
        t->node->value = copy;
        t->node->kind  = AML_NODE_OBJECT;
        return true;
    }

    case TGT_ELEMENT: {
        aml_object_t *c = t->container;

        if (!c)
            return false;

        if (c->type == AML_PACKAGE) {
            if ((uint32_t)t->index >= c->package.count)
                return false;

            aml_object_t *copy = clone(value);
            if (!copy)
                return false;

            aml_unref(c->package.items[t->index]);
            c->package.items[t->index] = copy;
            return true;
        }

        if (c->type == AML_BUFFER || c->type == AML_STRING) {
            uint64_t v = 0;

            if ((uint32_t)t->index >= c->buffer.len)
                return false;

            aml_to_integer(value, &v);
            c->buffer.bytes[t->index] = (uint8_t)v;
            return true;
        }

        return false;
    }
    }

    return false;
}

/* Storing needs the context for locals and arguments, and the context is not
 * something a target carries, so this wraps the two cases the target cannot
 * do by itself. */
static bool store_with_ctx(target_t *t, aml_object_t *value, aml_ctx_t *ctx)
{
    if (t->kind == TGT_LOCAL) {
        aml_object_t *copy = clone(value);
        if (!copy)
            return false;
        aml_unref(ctx->locals[t->index]);
        ctx->locals[t->index] = copy;
        return true;
    }

    if (t->kind == TGT_ARG) {
        aml_object_t *copy = clone(value);
        if (!copy)
            return false;
        aml_unref(ctx->args[t->index]);
        ctx->args[t->index] = copy;
        return true;
    }

    return store_value(t, value);
}

static void target_done(target_t *t)
{
    if (t->kind == TGT_ELEMENT)
        aml_unref(t->container);
}

/* The shape almost every operator has: operands, then a target that also
 * receives the result. */
static aml_object_t *finish(aml_stream_t *s, aml_ctx_t *ctx, bool *failed,
                            aml_object_t *result)
{
    target_t t;

    if (!parse_target(s, ctx, &t)) {
        *failed = true;
        aml_unref(result);
        return NULL;
    }

    if (result)
        store_with_ctx(&t, result, ctx);

    target_done(&t);
    return result;
}

static aml_object_t *binary(aml_stream_t *s, aml_ctx_t *ctx, bool *failed,
                            uint8_t op)
{
    uint64_t a = integer_operand(s, ctx, failed);
    uint64_t b = integer_operand(s, ctx, failed);
    uint64_t r = 0;

    if (*failed)
        return NULL;

    switch (op) {
    case OP_ADD:         r = a + b; break;
    case OP_SUBTRACT:    r = a - b; break;
    case OP_MULTIPLY:    r = a * b; break;
    case OP_SHIFT_LEFT:  r = (b < 64) ? a << b : 0; break;
    case OP_SHIFT_RIGHT: r = (b < 64) ? a >> b : 0; break;
    case OP_AND:         r = a & b; break;
    case OP_NAND:        r = ~(a & b); break;
    case OP_OR:          r = a | b; break;
    case OP_NOR:         r = ~(a | b); break;
    case OP_XOR:         r = a ^ b; break;
    case OP_MOD:         r = b ? a % b : 0; break;
    default:             break;
    }

    return finish(s, ctx, failed, aml_integer(r));
}

static aml_object_t *logical(aml_stream_t *s, aml_ctx_t *ctx, bool *failed,
                             uint8_t op)
{
    aml_object_t *a = eval_operand(s, ctx, failed);
    aml_object_t *b = eval_operand(s, ctx, failed);
    bool          r = false;

    if (*failed) {
        aml_unref(a);
        aml_unref(b);
        return NULL;
    }

    /* Strings compare as text and everything else as a number.  Firmware
     * compares _HID strings this way, and comparing them as integers would
     * make every string equal to every other. */
    if (a && b && a->type == AML_STRING && b->type == AML_STRING) {
        int c = strcmp((const char *)a->buffer.bytes,
                       (const char *)b->buffer.bytes);

        r = (op == OP_LEQUAL)   ? (c == 0)
          : (op == OP_LGREATER) ? (c > 0)
                                : (c < 0);
    } else {
        uint64_t x = 0, y = 0;

        aml_to_integer(a, &x);
        aml_to_integer(b, &y);

        r = (op == OP_LEQUAL)   ? (x == y)
          : (op == OP_LGREATER) ? (x > y)
                                : (x < y);
    }

    aml_unref(a);
    aml_unref(b);
    return aml_integer(r ? ~0ULL : 0);
}

/* ------------------------------------------------------------------ *
 *  Buffers and packages built at run time
 * ------------------------------------------------------------------ */

static aml_object_t *run_buffer(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    uint32_t       len  = aml_pkg_length(s);
    const uint8_t *stop = s->p + len;

    if (stop > s->end)
        stop = s->end;

    aml_stream_t sub  = { s->p, stop };
    uint64_t     size = 0;

    aml_object_t *n = eval_operand(&sub, ctx, failed);
    if (n)
        aml_to_integer(n, &size);
    aml_unref(n);

    uint32_t initial = (uint32_t)(stop - sub.p);
    if (size > 0x10000)
        size = 0x10000;
    if (size < initial)
        size = initial;

    aml_object_t *buf = aml_buffer(NULL, (uint32_t)size);
    if (buf && initial)
        memcpy(buf->buffer.bytes, sub.p, initial);

    s->p = stop;
    return buf;
}

static aml_object_t *run_package(aml_stream_t *s, aml_ctx_t *ctx, bool *failed,
                                 bool variable)
{
    uint32_t       len  = aml_pkg_length(s);
    const uint8_t *stop = s->p + len;

    if (stop > s->end)
        stop = s->end;

    uint32_t count;

    if (variable) {
        aml_stream_t  sub = { s->p, stop };
        uint64_t      v   = 0;
        aml_object_t *n   = eval_operand(&sub, ctx, failed);

        if (n)
            aml_to_integer(n, &v);
        aml_unref(n);

        s->p  = sub.p;
        count = (uint32_t)v;
    } else {
        count = aml_byte(s);
    }

    if (count > 512)
        count = 512;

    aml_object_t *pkg = kmalloc(sizeof(*pkg));
    if (!pkg) {
        s->p = stop;
        return NULL;
    }

    memset(pkg, 0, sizeof(*pkg));
    pkg->type = AML_PACKAGE;
    pkg->refs = 1;
    pkg->package.items = kmalloc(sizeof(aml_object_t *) * (count ? count : 1));

    if (!pkg->package.items) {
        kfree(pkg);
        s->p = stop;
        return NULL;
    }
    memset(pkg->package.items, 0, sizeof(aml_object_t *) * (count ? count : 1));
    pkg->package.count = count;

    for (uint32_t i = 0; i < count && s->p < stop; i++) {
        aml_stream_t sub = { s->p, stop };

        pkg->package.items[i] = eval_operand(&sub, ctx, failed);
        s->p = sub.p;
    }

    for (uint32_t i = 0; i < count; i++)
        if (!pkg->package.items[i])
            pkg->package.items[i] = aml_integer(0);

    s->p = stop;
    return pkg;
}

/* ------------------------------------------------------------------ *
 *  Control flow
 * ------------------------------------------------------------------ */

/* If, and the Else that may follow it.  The Else is part of the same decision,
 * so it is consumed here whether or not it runs -- leaving it for the term
 * loop would run it after the If had already run. */
static bool run_if(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    uint32_t       len  = aml_pkg_length(s);
    const uint8_t *stop = s->p + len;

    if (stop > s->end)
        stop = s->end;

    aml_stream_t sub  = { s->p, stop };
    uint64_t     cond = 0;

    aml_object_t *c = eval_operand(&sub, ctx, failed);
    if (c)
        aml_to_integer(c, &cond);
    aml_unref(c);

    if (cond && !*failed)
        aml_run_terms(&sub, ctx);

    s->p = stop;

    if (aml_peek(s) == OP_ELSE) {
        aml_byte(s);

        uint32_t       elen  = aml_pkg_length(s);
        const uint8_t *estop = s->p + elen;

        if (estop > s->end)
            estop = s->end;

        if (!cond && !*failed) {
            aml_stream_t esub = { s->p, estop };
            aml_run_terms(&esub, ctx);
        }

        s->p = estop;
    }

    return true;
}

static bool run_while(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    uint32_t       len  = aml_pkg_length(s);
    const uint8_t *stop = s->p + len;

    if (stop > s->end)
        stop = s->end;

    const uint8_t *body = s->p;

    /* A firmware loop that never ends would hang the boot with no way to say
     * why, so there is a ceiling on the turns.  It is far above anything a
     * real method does and far below forever. */
    for (uint32_t turn = 0; turn < 100000; turn++) {
        aml_stream_t sub  = { body, stop };
        uint64_t     cond = 0;

        aml_object_t *c = eval_operand(&sub, ctx, failed);
        if (c)
            aml_to_integer(c, &cond);
        aml_unref(c);

        if (!cond || *failed)
            break;

        aml_run_terms(&sub, ctx);

        if (ctx->returned || *failed)
            break;

        if (ctx->broke) {
            ctx->broke = false;
            break;
        }
        ctx->continued = false;
    }

    s->p = stop;
    return true;
}

/* ------------------------------------------------------------------ *
 *  One term
 * ------------------------------------------------------------------ */

static aml_object_t *eval_extended(aml_stream_t *s, aml_ctx_t *ctx,
                                   bool *failed)
{
    uint8_t op = aml_byte(s);

    switch (op) {
    case EXT_REVISION:
        return aml_integer(2);

    case EXT_TIMER:
        /* Hundred-nanosecond units since something.  The timer ticks at 100 Hz,
         * so this is coarse -- but the only thing firmware does with it is
         * measure elapsed time, and a coarse answer beats a constant one. */
        return aml_integer((uint64_t)pit_ticks() * 100000ULL);

    case EXT_DEBUG:
        return aml_integer(0);

    case EXT_COND_REF: {
        char name[128];

        if (!aml_name_string(s, name, sizeof(name))) {
            *failed = true;
            return NULL;
        }

        aml_node_t *n = aml_lookup(ctx->scope, name);
        target_t    t;

        if (!parse_target(s, ctx, &t)) {
            *failed = true;
            return NULL;
        }

        if (n) {
            aml_object_t *v = value_of(n);
            store_with_ctx(&t, v, ctx);
            aml_unref(v);
        }

        target_done(&t);
        return aml_integer(n ? ~0ULL : 0);
    }

    case EXT_ACQUIRE: {
        target_t t;

        /* One thing runs at a time here, so a mutex is always free.  The
         * timeout word is consumed and ignored; zero is "acquired". */
        parse_target(s, ctx, &t);
        target_done(&t);
        aml_byte(s);
        aml_byte(s);
        return aml_integer(0);
    }

    case EXT_RELEASE:
    case EXT_RESET:
    case EXT_SIGNAL: {
        target_t t;
        parse_target(s, ctx, &t);
        target_done(&t);
        return NULL;
    }

    case EXT_WAIT: {
        target_t t;
        parse_target(s, ctx, &t);
        target_done(&t);
        integer_operand(s, ctx, failed);
        return aml_integer(0);
    }

    case EXT_SLEEP:
    case EXT_STALL: {
        /* Both are firmware asking for time to pass.  Sleep is milliseconds
         * and Stall is microseconds; neither can be honoured properly this
         * early, and busy-waiting on the tick counter is enough for the
         * hundred-microsecond settling a controller asks for. */
        uint64_t amount = integer_operand(s, ctx, failed);
        uint32_t ticks  = (op == EXT_SLEEP) ? (uint32_t)(amount / 10)
                                            : 0;
        uint32_t until  = pit_ticks() + ticks + 1;

        while (pit_ticks() < until)
            __asm__ volatile("pause");
        return NULL;
    }

    case EXT_FROM_BCD: {
        uint64_t v = integer_operand(s, ctx, failed);
        uint64_t r = 0, scale = 1;

        while (v) {
            r += (v & 0x0F) * scale;
            scale *= 10;
            v >>= 4;
        }
        return finish(s, ctx, failed, aml_integer(r));
    }

    case EXT_TO_BCD: {
        uint64_t v = integer_operand(s, ctx, failed);
        uint64_t r = 0;
        int      shift = 0;

        while (v && shift < 64) {
            r |= (v % 10) << shift;
            v /= 10;
            shift += 4;
        }
        return finish(s, ctx, failed, aml_integer(r));
    }

    case EXT_FATAL:
        aml_byte(s);
        for (int i = 0; i < 4; i++)
            aml_byte(s);
        integer_operand(s, ctx, failed);
        return NULL;

    case EXT_CREATE_FLD: {
        /* CreateField makes a named window onto a buffer.  Buffers here are
         * ordinary objects rather than regions, and nothing the battery needs
         * uses one, so the operands are consumed and the name is left
         * uninitialised rather than pointing somewhere wrong. */
        char name[128];

        eval_operand(s, ctx, failed);
        integer_operand(s, ctx, failed);
        integer_operand(s, ctx, failed);
        aml_name_string(s, name, sizeof(name));
        aml_ns_create_path(ctx->scope, name, AML_NODE_OBJECT);
        return NULL;
    }

    /* Declarations can appear inside a method body.  They mean the same as at
     * the top level, so the load parser handles them -- one implementation,
     * and a Device() declared inside a method still lands in the namespace. */
    case EXT_REGION:
    case EXT_FIELD:
    case EXT_DEVICE:
    case EXT_PROCESSOR:
    case EXT_POWER_RES:
    case EXT_THERMAL:
    case EXT_INDEX_FLD:
    case EXT_BANK_FLD:
    case EXT_MUTEX:
    case EXT_EVENT: {
        aml_stream_t back = { s->p - 2, s->end };
        aml_parse_terms(&back, ctx->scope);
        s->p = back.p;
        return NULL;
    }

    default:
        *failed = true;
        return NULL;
    }
}

aml_object_t *aml_eval_term(aml_stream_t *s, aml_ctx_t *ctx, bool *failed)
{
    if (*failed || !aml_left(s, 1))
        return NULL;

    uint8_t op = aml_peek(s);

    /* Constants and literals. */
    switch (op) {
    case OP_ZERO: aml_byte(s); return aml_integer(0);
    case OP_ONE:  aml_byte(s); return aml_integer(1);
    case OP_ONES: aml_byte(s); return aml_integer(~0ULL);

    case OP_BYTE:
        aml_byte(s);
        return aml_integer(aml_byte(s));

    case OP_WORD: {
        aml_byte(s);
        uint64_t v = aml_byte(s);
        v |= (uint64_t)aml_byte(s) << 8;
        return aml_integer(v);
    }

    case OP_DWORD: {
        aml_byte(s);
        uint64_t v = 0;
        for (int i = 0; i < 4; i++)
            v |= (uint64_t)aml_byte(s) << (i * 8);
        return aml_integer(v);
    }

    case OP_QWORD: {
        aml_byte(s);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v |= (uint64_t)aml_byte(s) << (i * 8);
        return aml_integer(v);
    }

    case OP_STRING: {
        aml_byte(s);

        const char *text = (const char *)s->p;
        uint32_t    n    = 0;

        while (aml_left(s, n + 1) && s->p[n])
            n++;

        aml_object_t *o = aml_buffer((const uint8_t *)text, n);
        if (o)
            o->type = AML_STRING;

        s->p += n;
        if (aml_left(s, 1))
            aml_byte(s);
        return o;
    }

    case OP_BUFFER:
        aml_byte(s);
        return run_buffer(s, ctx, failed);

    case OP_PACKAGE:
        aml_byte(s);
        return run_package(s, ctx, failed, false);

    case OP_VAR_PACKAGE:
        aml_byte(s);
        return run_package(s, ctx, failed, true);

    default:
        break;
    }

    if (op >= OP_LOCAL0 && op < OP_LOCAL0 + AML_MAX_LOCALS) {
        aml_byte(s);
        aml_object_t *v = ctx->locals[op - OP_LOCAL0];
        return v ? aml_ref(v) : aml_integer(0);
    }

    if (op >= OP_ARG0 && op < OP_ARG0 + AML_MAX_ARGS) {
        aml_byte(s);
        aml_object_t *v = ctx->args[op - OP_ARG0];
        return v ? aml_ref(v) : aml_integer(0);
    }

    switch (op) {
    case OP_EXT:
        aml_byte(s);
        return eval_extended(s, ctx, failed);

    case OP_STORE: {
        aml_byte(s);

        aml_object_t *v = eval_operand(s, ctx, failed);
        target_t      t;

        if (*failed || !parse_target(s, ctx, &t)) {
            *failed = true;
            aml_unref(v);
            return NULL;
        }

        store_with_ctx(&t, v, ctx);
        target_done(&t);
        return v;
    }

    case OP_COPY_OBJECT: {
        aml_byte(s);

        aml_object_t *v = eval_operand(s, ctx, failed);
        target_t      t;

        if (*failed || !parse_target(s, ctx, &t)) {
            *failed = true;
            aml_unref(v);
            return NULL;
        }

        store_with_ctx(&t, v, ctx);
        target_done(&t);
        return v;
    }

    case OP_ADD: case OP_SUBTRACT: case OP_MULTIPLY:
    case OP_SHIFT_LEFT: case OP_SHIFT_RIGHT:
    case OP_AND: case OP_NAND: case OP_OR: case OP_NOR: case OP_XOR:
    case OP_MOD:
        aml_byte(s);
        return binary(s, ctx, failed, op);

    case OP_DIVIDE: {
        aml_byte(s);

        uint64_t a = integer_operand(s, ctx, failed);
        uint64_t b = integer_operand(s, ctx, failed);

        /* Divide has two targets: the remainder first, then the quotient.
         * Getting them the wrong way round is a classic, and it produces
         * numbers that look plausible. */
        target_t rem, quo;

        if (*failed || !parse_target(s, ctx, &rem) ||
                       !parse_target(s, ctx, &quo)) {
            *failed = true;
            return NULL;
        }

        aml_object_t *r = aml_integer(b ? a % b : 0);
        aml_object_t *q = aml_integer(b ? a / b : 0);

        store_with_ctx(&rem, r, ctx);
        store_with_ctx(&quo, q, ctx);
        target_done(&rem);
        target_done(&quo);
        aml_unref(r);

        return q;
    }

    case OP_NOT: {
        aml_byte(s);
        uint64_t v = integer_operand(s, ctx, failed);
        return finish(s, ctx, failed, aml_integer(~v));
    }

    case OP_FIND_LEFT: {
        aml_byte(s);
        uint64_t v = integer_operand(s, ctx, failed);
        uint64_t r = 0;

        for (int i = 63; i >= 0; i--)
            if (v & (1ULL << i)) { r = (uint64_t)i + 1; break; }

        return finish(s, ctx, failed, aml_integer(r));
    }

    case OP_FIND_RIGHT: {
        aml_byte(s);
        uint64_t v = integer_operand(s, ctx, failed);
        uint64_t r = 0;

        for (int i = 0; i < 64; i++)
            if (v & (1ULL << i)) { r = (uint64_t)i + 1; break; }

        return finish(s, ctx, failed, aml_integer(r));
    }

    case OP_INCREMENT:
    case OP_DECREMENT: {
        aml_byte(s);

        const uint8_t *at = s->p;
        target_t       t;

        if (!parse_target(s, ctx, &t)) {
            *failed = true;
            return NULL;
        }

        /* Read it back through the same path an operand would, so a field
         * that counts is read from the hardware and written to it. */
        aml_stream_t  reread = { at, s->p };
        aml_object_t *old    = eval_operand(&reread, ctx, failed);
        uint64_t      v      = 0;

        if (old)
            aml_to_integer(old, &v);
        aml_unref(old);

        v += (op == OP_INCREMENT) ? 1 : (uint64_t)-1;

        aml_object_t *r = aml_integer(v);
        store_with_ctx(&t, r, ctx);
        target_done(&t);
        return r;
    }

    case OP_LAND: case OP_LOR: {
        aml_byte(s);

        uint64_t a = integer_operand(s, ctx, failed);
        uint64_t b = integer_operand(s, ctx, failed);
        bool     r = (op == OP_LAND) ? (a && b) : (a || b);

        return aml_integer(r ? ~0ULL : 0);
    }

    case OP_LNOT: {
        aml_byte(s);
        uint64_t v = integer_operand(s, ctx, failed);
        return aml_integer(v ? 0 : ~0ULL);
    }

    case OP_LEQUAL: case OP_LGREATER: case OP_LLESS:
        aml_byte(s);
        return logical(s, ctx, failed, op);

    case OP_SIZE_OF: {
        aml_byte(s);

        aml_object_t *o = eval_operand(s, ctx, failed);
        uint64_t      n = 0;

        if (o) {
            if (o->type == AML_PACKAGE)                       n = o->package.count;
            else if (o->type == AML_BUFFER || o->type == AML_STRING)
                                                              n = o->buffer.len;
        }

        aml_unref(o);
        return aml_integer(n);
    }

    case OP_OBJECT_TYPE: {
        aml_byte(s);

        aml_object_t *o = eval_operand(s, ctx, failed);
        uint64_t      t = 0;

        /* The numbering in the specification: 1 integer, 2 string, 3 buffer,
         * 4 package, 5 field unit. */
        if (o) {
            switch (o->type) {
            case AML_INTEGER: t = 1; break;
            case AML_STRING:  t = 2; break;
            case AML_BUFFER:  t = 3; break;
            case AML_PACKAGE: t = 4; break;
            case AML_FIELD:   t = 5; break;
            default:          t = 0; break;
            }
        }

        aml_unref(o);
        return aml_integer(t);
    }

    case OP_INDEX: {
        aml_byte(s);

        aml_object_t *c = eval_operand(s, ctx, failed);
        uint64_t      i = integer_operand(s, ctx, failed);

        target_t t;
        if (*failed || !parse_target(s, ctx, &t)) {
            *failed = true;
            aml_unref(c);
            return NULL;
        }

        aml_object_t *r = kmalloc(sizeof(*r));
        if (r) {
            memset(r, 0, sizeof(*r));
            r->type                 = AML_REFERENCE;
            r->refs                 = 1;
            r->reference.target     = c;       /* the reference owns it */
            r->reference.index      = (uint32_t)i;
            r->reference.is_index   = true;
        } else {
            aml_unref(c);
        }

        if (r)
            store_with_ctx(&t, r, ctx);
        target_done(&t);
        return r;
    }

    case OP_DEREF_OF: {
        aml_byte(s);

        aml_object_t *r = eval_operand(s, ctx, failed);
        aml_object_t *v = NULL;

        if (r && r->type == AML_REFERENCE && r->reference.target) {
            aml_object_t *c = r->reference.target;

            if (r->reference.is_index && c->type == AML_PACKAGE &&
                r->reference.index < c->package.count)
                v = aml_ref(c->package.items[r->reference.index]);
            else if (r->reference.is_index &&
                     (c->type == AML_BUFFER || c->type == AML_STRING) &&
                     r->reference.index < c->buffer.len)
                v = aml_integer(c->buffer.bytes[r->reference.index]);
            else
                v = aml_ref(c);
        } else if (r) {
            v = aml_ref(r);
        }

        aml_unref(r);
        return v ? v : aml_integer(0);
    }

    case OP_REF_OF: {
        aml_byte(s);

        /* A reference to a name.  Nothing here needs to store through one, so
         * it yields the value: DerefOf of it then gives the same answer the
         * name would have. */
        return eval_operand(s, ctx, failed);
    }

    case OP_TO_INTEGER: {
        aml_byte(s);
        uint64_t v = integer_operand(s, ctx, failed);
        return finish(s, ctx, failed, aml_integer(v));
    }

    case OP_TO_BUFFER: {
        aml_byte(s);

        aml_object_t *o = eval_operand(s, ctx, failed);
        aml_object_t *b = NULL;

        if (o && (o->type == AML_BUFFER || o->type == AML_STRING))
            b = aml_buffer(o->buffer.bytes, o->buffer.len);
        else {
            uint64_t v = 0;
            aml_to_integer(o, &v);
            b = aml_buffer((const uint8_t *)&v, 8);
        }

        aml_unref(o);
        return finish(s, ctx, failed, b);
    }

    case OP_TO_HEX_STR: case OP_TO_DEC_STR: {
        aml_byte(s);

        uint64_t v = integer_operand(s, ctx, failed);
        char     text[24];
        int      at = 0;

        if (op == OP_TO_HEX_STR) {
            static const char digits[] = "0123456789ABCDEF";
            char tmp[16];
            int  n = 0;

            if (!v) tmp[n++] = '0';
            while (v) { tmp[n++] = digits[v & 0x0F]; v >>= 4; }
            while (n) text[at++] = tmp[--n];
        } else {
            char tmp[24];
            int  n = 0;

            if (!v) tmp[n++] = '0';
            while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
            while (n) text[at++] = tmp[--n];
        }

        text[at] = '\0';
        return finish(s, ctx, failed, aml_string(text));
    }

    case OP_TO_STRING: {
        aml_byte(s);

        aml_object_t *o = eval_operand(s, ctx, failed);
        uint64_t      n = integer_operand(s, ctx, failed);
        aml_object_t *r;

        if (o && (o->type == AML_BUFFER || o->type == AML_STRING)) {
            uint32_t len = o->buffer.len;
            if (n < len)
                len = (uint32_t)n;
            r = aml_buffer(o->buffer.bytes, len);
            if (r)
                r->type = AML_STRING;
        } else {
            r = aml_string("");
        }

        aml_unref(o);
        return finish(s, ctx, failed, r);
    }

    case OP_CONCAT: {
        aml_byte(s);

        aml_object_t *a = eval_operand(s, ctx, failed);
        aml_object_t *b = eval_operand(s, ctx, failed);

        uint32_t la = (a && (a->type == AML_BUFFER || a->type == AML_STRING))
                      ? a->buffer.len : 0;
        uint32_t lb = (b && (b->type == AML_BUFFER || b->type == AML_STRING))
                      ? b->buffer.len : 0;

        aml_object_t *r = aml_buffer(NULL, la + lb);

        if (r) {
            if (la) memcpy(r->buffer.bytes, a->buffer.bytes, la);
            if (lb) memcpy(r->buffer.bytes + la, b->buffer.bytes, lb);
            if (a && a->type == AML_STRING)
                r->type = AML_STRING;
        }

        aml_unref(a);
        aml_unref(b);
        return finish(s, ctx, failed, r);
    }

    case OP_MID: {
        aml_byte(s);

        aml_object_t *src   = eval_operand(s, ctx, failed);
        uint64_t      start = integer_operand(s, ctx, failed);
        uint64_t      len   = integer_operand(s, ctx, failed);
        aml_object_t *r;

        if (src && (src->type == AML_BUFFER || src->type == AML_STRING) &&
            start < src->buffer.len) {
            uint32_t have = src->buffer.len - (uint32_t)start;
            if (len > have)
                len = have;

            r = aml_buffer(src->buffer.bytes + start, (uint32_t)len);
            if (r)
                r->type = src->type;
        } else {
            r = aml_buffer(NULL, 0);
            if (r && src)
                r->type = src->type;
        }

        aml_unref(src);
        return finish(s, ctx, failed, r);
    }

    case OP_MATCH: {
        aml_byte(s);

        /* Search a package for an element matching two conditions.  Nothing
         * the battery uses calls it; the operands are consumed properly so
         * the stream stays aligned, and the answer is Ones -- not found. */
        aml_object_t *pkg = eval_operand(s, ctx, failed);
        aml_byte(s);
        integer_operand(s, ctx, failed);
        aml_byte(s);
        integer_operand(s, ctx, failed);
        integer_operand(s, ctx, failed);
        aml_unref(pkg);
        return aml_integer(~0ULL);
    }

    case OP_CREATE_B: case OP_CREATE_W: case OP_CREATE_DW:
    case OP_CREATE_QW: case OP_CREATE_BIT: {
        aml_byte(s);

        /* A named window onto a buffer object.  Consumed so the stream stays
         * aligned; the name is created uninitialised rather than pointing at
         * a place this does not model. */
        char name[128];

        eval_operand(s, ctx, failed);
        integer_operand(s, ctx, failed);
        aml_name_string(s, name, sizeof(name));
        aml_ns_create_path(ctx->scope, name, AML_NODE_OBJECT);
        return NULL;
    }

    case OP_NOTIFY: {
        aml_byte(s);

        target_t t;
        parse_target(s, ctx, &t);
        target_done(&t);
        integer_operand(s, ctx, failed);
        return NULL;
    }

    case OP_IF:
        aml_byte(s);
        run_if(s, ctx, failed);
        return NULL;

    case OP_ELSE: {
        /* An Else with no If in front of it: skip its body.  Reached when the
         * If was inside a construct that was stepped over. */
        aml_byte(s);
        uint32_t len = aml_pkg_length(s);
        s->p += len;
        if (s->p > s->end)
            s->p = s->end;
        return NULL;
    }

    case OP_WHILE:
        aml_byte(s);
        run_while(s, ctx, failed);
        return NULL;

    case OP_RETURN: {
        aml_byte(s);

        aml_object_t *v = eval_operand(s, ctx, failed);

        aml_unref(ctx->result);
        ctx->result   = v;
        ctx->returned = true;
        return NULL;
    }

    case OP_BREAK:
        aml_byte(s);
        ctx->broke = true;
        return NULL;

    case OP_CONTINUE:
        aml_byte(s);
        ctx->continued = true;
        return NULL;

    case OP_NOOP:
    case OP_BREAKPOINT:
        aml_byte(s);
        return NULL;

    /* Declarations inside a method body. */
    case OP_NAME:
    case OP_SCOPE:
    case OP_METHOD:
    case OP_ALIAS:
    case OP_EXTERNAL: {
        aml_stream_t back = { s->p, s->end };
        aml_parse_terms(&back, ctx->scope);
        s->p = back.p;
        return NULL;
    }

    default:
        break;
    }

    /* Not an opcode: a name, which may be a value or a call. */
    return eval_name(s, ctx, failed);
}

bool aml_run_terms(aml_stream_t *s, aml_ctx_t *ctx)
{
    bool failed = false;

    while (s->p < s->end && !ctx->returned && !ctx->broke && !ctx->continued) {
        const uint8_t *before = s->p;

        aml_object_t *v = aml_eval_term(s, ctx, &failed);
        aml_unref(v);

        if (failed)
            return false;

        /* A term that consumed nothing would spin here forever.  It means an
         * opcode this subset does not know, and the rest of the list cannot be
         * trusted to be aligned any more. */
        if (s->p == before)
            return false;
    }

    return true;
}

/* ------------------------------------------------------------------ *
 *  Calling
 * ------------------------------------------------------------------ */

static aml_object_t *call_method(aml_node_t *node, aml_object_t **args,
                                 int argc, int depth)
{
    if (!node || node->kind != AML_NODE_METHOD || !node->code)
        return NULL;

    if (depth > MAX_DEPTH) {
        kprintf("aml    : method nesting too deep; giving up\n");
        return NULL;
    }

    aml_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.scope = node;                 /* names resolve relative to the method */
    ctx.depth = depth;

    for (int i = 0; i < argc && i < AML_MAX_ARGS; i++)
        ctx.args[i] = aml_ref(args[i]);

    aml_stream_t s = { node->code, node->code + node->code_len };
    bool ok = aml_run_terms(&s, &ctx);

    for (int i = 0; i < AML_MAX_ARGS; i++)
        aml_unref(ctx.args[i]);
    for (int i = 0; i < AML_MAX_LOCALS; i++)
        aml_unref(ctx.locals[i]);

    if (!ok && !ctx.returned) {
        char path[128];
        aml_ns_path(node, path, sizeof(path));
        kprintf("aml    : %s did not run to the end\n", path);

        aml_unref(ctx.result);
        return NULL;
    }

    /* A method with no Return is worth zero, which is what the specification
     * says and what firmware relies on. */
    return ctx.result ? ctx.result : aml_integer(0);
}

aml_object_t *aml_evaluate_node(aml_node_t *node, aml_object_t **args, int argc)
{
    if (!node)
        return NULL;

    if (node->kind == AML_NODE_METHOD)
        return call_method(node, args, argc, 0);

    return value_of(node);
}

aml_object_t *aml_evaluate(const char *path, aml_object_t **args, int argc)
{
    return aml_evaluate_node(aml_lookup(NULL, path), args, argc);
}
