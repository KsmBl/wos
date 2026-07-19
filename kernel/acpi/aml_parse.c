/* Reading a definition block into the namespace.
 *
 * This is the load pass.  It walks the bytecode looking for the things that
 * declare names -- Name, Scope, Device, Method, OperationRegion, Field -- and
 * creates a namespace node for each.  Everything else it steps over.
 *
 * A method's body is deliberately *not* read here.  It is code, it may only be
 * meaningful once its arguments exist, and running it at load time would mean
 * touching hardware while the machine is still being described.  All that is
 * recorded is where the body starts and how long it is; aml_exec.c reads it
 * when somebody calls the method.  That is also why the table copies are kept
 * for the life of the machine rather than freed after loading -- the code
 * pointers are into them.
 *
 * Nothing here fails the whole block.  Firmware contains constructs this
 * subset does not implement, and a table that stops being read at the first
 * one would lose every name after it -- including, on a laptop whose battery
 * is declared late, the battery.  An unknown construct is stepped over and
 * counted, and the count goes in the boot log.
 */

#include "aml_internal.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

/* Opcodes that declare something.  The rest of the language is in
 * aml_exec.c; these are the ones the load pass has to recognise. */
#define OP_ZERO         0x00
#define OP_ONE          0x01
#define OP_ALIAS        0x06
#define OP_NAME         0x08
#define OP_BYTE         0x0A
#define OP_WORD         0x0B
#define OP_DWORD        0x0C
#define OP_STRING       0x0D
#define OP_QWORD        0x0E
#define OP_SCOPE        0x10
#define OP_BUFFER       0x11
#define OP_PACKAGE      0x12
#define OP_VAR_PACKAGE  0x13
#define OP_METHOD       0x14
#define OP_EXTERNAL     0x15
#define OP_EXT          0x5B
#define OP_ONES         0xFF

/* Two-byte opcodes, after the 0x5B prefix. */
#define EXT_MUTEX       0x01
#define EXT_EVENT       0x02
#define EXT_REGION      0x80
#define EXT_FIELD       0x81
#define EXT_DEVICE      0x82
#define EXT_PROCESSOR   0x83
#define EXT_POWER_RES   0x84
#define EXT_THERMAL     0x85
#define EXT_INDEX_FIELD 0x86
#define EXT_BANK_FIELD  0x87

static uint32_t skipped;

uint32_t aml_parse_skipped(void) { return skipped; }

/* ------------------------------------------------------------------ *
 *  The two encodings everything else is built out of
 * ------------------------------------------------------------------ */

uint32_t aml_pkg_length(aml_stream_t *s)
{
    if (!aml_left(s, 1))
        return 0;

    const uint8_t *start = s->p;
    uint8_t        lead  = aml_byte(s);
    uint32_t       extra = lead >> 6;

    /* With no following bytes the whole six low bits are the length.  With
     * following bytes only the low four are, and the next bytes supply the
     * high ones -- the two encodings are different, not one with a longer
     * field, which is the detail that makes hand-written scanners wrong. */
    uint32_t length = extra ? (uint32_t)(lead & 0x0F) : (uint32_t)(lead & 0x3F);

    for (uint32_t i = 0; i < extra; i++)
        length |= (uint32_t)aml_byte(s) << (4 + i * 8);

    /* The length counts its own bytes, so what is left for the body is that
     * minus however many we just read. */
    uint32_t header = (uint32_t)(s->p - start);
    return length < header ? 0 : length - header;
}

bool aml_name_string(aml_stream_t *s, char *out, uint32_t size)
{
    uint32_t at = 0;

    if (size == 0)
        return false;

    out[0] = '\0';

    /* A rooted or relative prefix, kept in the text so aml_lookup() can tell
     * `\_SB_.BAT0` from a bare `BAT0` that has to be searched for. */
    if (aml_peek(s) == '\\') {
        aml_byte(s);
        if (at + 1 < size) out[at++] = '\\';
    } else {
        while (aml_peek(s) == '^') {
            aml_byte(s);
            if (at + 1 < size) out[at++] = '^';
        }
    }

    uint32_t segments = 1;

    switch (aml_peek(s)) {
    case 0x00:                       /* NullName */
        aml_byte(s);
        segments = 0;
        break;
    case 0x2E:                       /* DualNamePrefix */
        aml_byte(s);
        segments = 2;
        break;
    case 0x2F:                       /* MultiNamePrefix */
        aml_byte(s);
        segments = aml_byte(s);
        break;
    default:
        break;
    }

    for (uint32_t i = 0; i < segments; i++) {
        if (!aml_left(s, 4))
            return false;

        if (i && at + 1 < size)
            out[at++] = '.';

        for (int c = 0; c < 4; c++) {
            uint8_t ch = aml_byte(s);
            if (at + 1 < size)
                out[at++] = (char)ch;
        }
    }

    out[at < size ? at : size - 1] = '\0';
    return true;
}

/* ------------------------------------------------------------------ *
 *  Making the nodes a path needs
 * ------------------------------------------------------------------ */

aml_node_t *aml_ns_create_path(aml_node_t *from, const char *path, uint8_t kind)
{
    aml_node_t *at = aml_ns_root();

    if (!at || !path)
        return NULL;

    if (*path == '\\') {
        path++;
    } else if (*path == '^') {
        at = from ? from : at;
        while (*path == '^') {
            if (at->parent)
                at = at->parent;
            path++;
        }
        if (*path == '.')
            path++;
    } else {
        /* A relative name defines into the current scope.  It is not searched
         * for: Device(BAT0) in \_SB creates \_SB.BAT0 whether or not some
         * other scope also has a BAT0. */
        at = from ? from : at;
    }

    if (!*path)
        return at;

    for (;;) {
        uint32_t len = 0;
        while (path[len] && path[len] != '.')
            len++;

        bool last = (path[len] == '\0');

        /* Everything but the last segment is a scope that has to exist for
         * the name to hang off; only the last gets the kind asked for. */
        at = aml_ns_define(at, path, len, last ? kind : AML_NODE_SCOPE);
        if (!at || last)
            return at;

        path += len + 1;
    }
}

/* ------------------------------------------------------------------ *
 *  Data
 * ------------------------------------------------------------------ */

static aml_object_t *parse_data(aml_stream_t *s, aml_node_t *scope);

static aml_object_t *parse_package(aml_stream_t *s, aml_node_t *scope,
                                   bool variable)
{
    uint32_t       len  = aml_pkg_length(s);
    const uint8_t *stop = s->p + len;

    if (stop > s->end)
        stop = s->end;

    uint32_t count;

    if (variable) {
        /* VarPackage sizes itself from an expression.  At load time there is
         * nothing to evaluate it against, so the elements are counted as they
         * are read instead -- which is what the length is for anyway. */
        aml_stream_t sub = { s->p, stop };
        aml_object_t *n  = parse_data(&sub, scope);
        uint64_t      v  = 0;

        if (n && aml_to_integer(n, &v))
            count = (uint32_t)v;
        else
            count = 0;
        aml_unref(n);
        s->p = sub.p;
    } else {
        count = aml_byte(s);
    }

    if (count > 512)
        count = 512;              /* nothing real is this big; refuse to trust it */

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

    for (uint32_t i = 0; i < count; i++) {
        if (s->p >= stop) {
            /* Fewer elements than declared.  The specification allows it --
             * the rest are uninitialised -- and firmware does it. */
            pkg->package.items[i] = aml_integer(0);
            if (pkg->package.items[i])
                pkg->package.items[i]->type = AML_UNINIT;
            continue;
        }

        aml_stream_t sub = { s->p, stop };
        pkg->package.items[i] = parse_data(&sub, scope);
        s->p = sub.p;

        if (!pkg->package.items[i]) {
            pkg->package.items[i] = aml_integer(0);
            if (pkg->package.items[i])
                pkg->package.items[i]->type = AML_UNINIT;
        }
    }

    s->p = stop;
    return pkg;
}

static aml_object_t *parse_buffer(aml_stream_t *s, aml_node_t *scope)
{
    uint32_t       len  = aml_pkg_length(s);
    const uint8_t *stop = s->p + len;

    if (stop > s->end)
        stop = s->end;

    /* The declared size, which may be larger than the bytes that follow: a
     * buffer of 32 zero bytes is written as a size and nothing else. */
    aml_stream_t  sub  = { s->p, stop };
    aml_object_t *size = parse_data(&sub, scope);
    uint64_t      want = 0;

    if (size)
        aml_to_integer(size, &want);
    aml_unref(size);

    uint32_t initial = (uint32_t)(stop - sub.p);
    if (want > 0x10000)
        want = 0x10000;
    if (want < initial)
        want = initial;

    aml_object_t *buf = aml_buffer(NULL, (uint32_t)want);
    if (buf && initial)
        memcpy(buf->buffer.bytes, sub.p, initial);

    s->p = stop;
    return buf;
}

/* One DataObject.  Anything that is not data returns NULL and leaves the
 * stream where it can be stepped over. */
static aml_object_t *parse_data(aml_stream_t *s, aml_node_t *scope)
{
    uint8_t op = aml_peek(s);

    switch (op) {
    case OP_ZERO:  aml_byte(s); return aml_integer(0);
    case OP_ONE:   aml_byte(s); return aml_integer(1);
    case OP_ONES:  aml_byte(s); return aml_integer(~0ULL);

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
            aml_byte(s);              /* the terminator */
        return o;
    }

    case OP_BUFFER:
        aml_byte(s);
        return parse_buffer(s, scope);

    case OP_PACKAGE:
        aml_byte(s);
        return parse_package(s, scope, false);

    case OP_VAR_PACKAGE:
        aml_byte(s);
        return parse_package(s, scope, true);

    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------ *
 *  Fields
 * ------------------------------------------------------------------ */

/* A FieldList: a run of named bit ranges, offsets and skips, laid end to end
 * over the region.  The names are ordinary namespace nodes holding an
 * AML_FIELD object, so `Store(1, BAT0)` reaches the hardware through the same
 * path any other store takes. */
static void parse_field_list(aml_stream_t *s, aml_node_t *scope,
                             aml_node_t *region, uint8_t flags)
{
    uint32_t bit_at = 0;

    /* The low two bits of the field flags are the access width.  AnyAcc and
     * ByteAcc both mean bytes here; the wider ones matter to hardware that
     * refuses a narrow access, which the embedded controller does not. */
    static const uint8_t widths[] = { 1, 1, 2, 4, 8, 1, 1, 1 };
    uint8_t access = widths[flags & 0x07];

    while (s->p < s->end) {
        uint8_t lead = aml_peek(s);

        if (lead == 0x00) {                    /* ReservedField: a gap */
            aml_byte(s);
            bit_at += aml_pkg_length(s);
            continue;
        }

        if (lead == 0x01) {                    /* AccessField */
            aml_byte(s);
            uint8_t type = aml_byte(s);
            aml_byte(s);                       /* attribute */
            access = widths[type & 0x07];
            continue;
        }

        if (lead == 0x02) {                    /* ConnectField */
            aml_byte(s);
            continue;
        }

        if (lead == 0x03) {                    /* ExtendedAccessField */
            aml_byte(s);
            aml_byte(s);
            aml_byte(s);
            aml_byte(s);
            continue;
        }

        /* A named field: four characters and a bit width. */
        if (!aml_left(s, 4))
            return;

        const char *seg = (const char *)s->p;
        s->p += 4;

        uint32_t width = aml_pkg_length(s);

        aml_node_t *n = aml_ns_define(scope, seg, 4, AML_NODE_OBJECT);
        if (n) {
            aml_object_t *f = kmalloc(sizeof(*f));

            if (f) {
                memset(f, 0, sizeof(*f));
                f->type             = AML_FIELD;
                f->refs             = 1;
                f->field.region     = region;
                f->field.bit_offset = bit_at;
                f->field.bit_len    = width;
                f->field.access     = access;

                aml_unref(n->value);
                n->value = f;
            }
        }

        bit_at += width;
    }
}

/* ------------------------------------------------------------------ *
 *  Declarations
 * ------------------------------------------------------------------ */

/* Step over a term whose opcode we recognise but whose body we do not want.
 * Everything with a PkgLength can be skipped exactly; anything else ends the
 * list, since guessing a length is how a parser starts reading operands as
 * opcodes. */
static bool skip_unknown(aml_stream_t *s)
{
    skipped++;
    s->p = s->end;
    return true;
}

static bool parse_extended(aml_stream_t *s, aml_node_t *scope)
{
    uint8_t op = aml_byte(s);
    char    name[128];

    switch (op) {
    case EXT_REGION: {
        if (!aml_name_string(s, name, sizeof(name)))
            return false;

        uint8_t space = aml_byte(s);

        aml_object_t *off = parse_data(s, scope);
        aml_object_t *len = parse_data(s, scope);
        uint64_t      o = 0, l = 0;

        if (off) aml_to_integer(off, &o);
        if (len) aml_to_integer(len, &l);
        aml_unref(off);
        aml_unref(len);

        aml_node_t *n = aml_ns_create_path(scope, name, AML_NODE_REGION);
        if (n) {
            n->space  = space;
            n->offset = o;
            n->length = l;
        }
        return true;
    }

    case EXT_FIELD: {
        uint32_t       len  = aml_pkg_length(s);
        const uint8_t *stop = s->p + len;

        if (stop > s->end)
            stop = s->end;

        if (!aml_name_string(s, name, sizeof(name))) {
            s->p = stop;
            return true;
        }

        aml_node_t *region = aml_lookup(scope, name);
        uint8_t     flags  = aml_byte(s);

        /* A field over a region that was never declared still has to be
         * walked, or the names it defines go missing and a method that reads
         * one fails in a way that looks like a bad method. */
        aml_stream_t sub = { s->p, stop };
        parse_field_list(&sub, scope, region, flags);

        s->p = stop;
        return true;
    }

    case EXT_INDEX_FIELD:
    case EXT_BANK_FIELD: {
        /* Both are a field reached through another field rather than directly.
         * The names are defined so lookups find them, with no region behind
         * them, so reading one fails rather than reading the wrong address. */
        uint32_t       len  = aml_pkg_length(s);
        const uint8_t *stop = s->p + len;

        if (stop > s->end)
            stop = s->end;

        aml_name_string(s, name, sizeof(name));
        aml_name_string(s, name, sizeof(name));
        if (op == EXT_BANK_FIELD) {
            aml_object_t *bank = parse_data(s, scope);
            aml_unref(bank);
        }

        uint8_t      flags = aml_byte(s);
        aml_stream_t sub   = { s->p, stop };

        parse_field_list(&sub, scope, NULL, flags);
        skipped++;

        s->p = stop;
        return true;
    }

    case EXT_DEVICE:
    case EXT_POWER_RES:
    case EXT_THERMAL:
    case EXT_PROCESSOR: {
        uint32_t       len  = aml_pkg_length(s);
        const uint8_t *stop = s->p + len;

        if (stop > s->end)
            stop = s->end;

        if (!aml_name_string(s, name, sizeof(name))) {
            s->p = stop;
            return true;
        }

        /* PowerResource and Processor carry a few fixed fields before their
         * body; Device does not. */
        if (op == EXT_POWER_RES) {
            aml_byte(s);                 /* system level */
            aml_byte(s); aml_byte(s);    /* resource order */
        } else if (op == EXT_PROCESSOR) {
            aml_byte(s);                 /* id */
            for (int i = 0; i < 4; i++)
                aml_byte(s);             /* block address */
            aml_byte(s);                 /* block length */
        }

        aml_node_t *n = aml_ns_create_path(scope, name, AML_NODE_DEVICE);

        aml_stream_t sub = { s->p, stop };
        aml_parse_terms(&sub, n ? n : scope);

        s->p = stop;
        return true;
    }

    case EXT_MUTEX:
        if (!aml_name_string(s, name, sizeof(name)))
            return false;
        aml_byte(s);                     /* sync level */
        aml_ns_create_path(scope, name, AML_NODE_MUTEX);
        return true;

    case EXT_EVENT:
        if (!aml_name_string(s, name, sizeof(name)))
            return false;
        aml_ns_create_path(scope, name, AML_NODE_OBJECT);
        return true;

    default:
        return skip_unknown(s);
    }
}

bool aml_parse_terms(aml_stream_t *s, aml_node_t *scope)
{
    char name[128];

    while (s->p < s->end) {
        uint8_t op = aml_peek(s);

        switch (op) {
        case OP_NAME: {
            aml_byte(s);
            if (!aml_name_string(s, name, sizeof(name)))
                return false;

            aml_object_t *value = parse_data(s, scope);
            aml_node_t   *n     = aml_ns_create_path(scope, name,
                                                     AML_NODE_OBJECT);

            if (n) {
                aml_unref(n->value);
                n->value = value;       /* the node takes the reference */
            } else {
                aml_unref(value);
            }
            break;
        }

        case OP_SCOPE: {
            aml_byte(s);

            uint32_t       len  = aml_pkg_length(s);
            const uint8_t *stop = s->p + len;

            if (stop > s->end)
                stop = s->end;

            if (!aml_name_string(s, name, sizeof(name))) {
                s->p = stop;
                break;
            }

            /* Scope() opens something that already exists, or should.  Firmware
             * does open scopes on names declared in another table, so this
             * creates rather than requires -- an SSDT loaded before the DSDT
             * would otherwise lose everything inside. */
            aml_node_t *n = aml_lookup(scope, name);
            if (!n)
                n = aml_ns_create_path(scope, name, AML_NODE_SCOPE);

            aml_stream_t sub = { s->p, stop };
            aml_parse_terms(&sub, n ? n : scope);

            s->p = stop;
            break;
        }

        case OP_METHOD: {
            aml_byte(s);

            uint32_t       len  = aml_pkg_length(s);
            const uint8_t *stop = s->p + len;

            if (stop > s->end)
                stop = s->end;

            if (!aml_name_string(s, name, sizeof(name))) {
                s->p = stop;
                break;
            }

            uint8_t     flags = aml_byte(s);
            aml_node_t *n     = aml_ns_create_path(scope, name,
                                                   AML_NODE_METHOD);

            if (n) {
                n->kind      = AML_NODE_METHOD;
                n->code      = s->p;
                n->code_len  = (uint32_t)(stop - s->p);
                n->arg_count = flags & 0x07;
            }

            s->p = stop;
            break;
        }

        case OP_ALIAS: {
            aml_byte(s);

            char target[128];
            if (!aml_name_string(s, target, sizeof(target)) ||
                !aml_name_string(s, name, sizeof(name)))
                return false;

            /* An alias shares the object rather than copying it, so a store
             * through either name is seen through both. */
            aml_node_t *src = aml_lookup(scope, target);
            aml_node_t *dst = aml_ns_create_path(scope, name, AML_NODE_OBJECT);

            if (src && dst) {
                dst->kind      = src->kind;
                dst->code      = src->code;
                dst->code_len  = src->code_len;
                dst->arg_count = src->arg_count;
                dst->space     = src->space;
                dst->offset    = src->offset;
                dst->length    = src->length;

                aml_unref(dst->value);
                dst->value = aml_ref(src->value);
            }
            break;
        }

        case OP_EXTERNAL:
            /* A promise that a name is declared in another table.  Nothing to
             * create: whichever table declares it will. */
            aml_byte(s);
            if (!aml_name_string(s, name, sizeof(name)))
                return false;
            aml_byte(s);                 /* object type */
            aml_byte(s);                 /* argument count */
            break;

        case OP_EXT:
            aml_byte(s);
            if (!parse_extended(s, scope))
                return false;
            break;

        default:
            /* Not a declaration.  At the top level of a table that means a
             * construct this subset does not implement, and there is no way
             * to know how long it is -- so the rest of this list is given up
             * rather than parsed as if the operands were opcodes. */
            return skip_unknown(s);
        }
    }

    return true;
}

bool aml_load_block(const uint8_t *aml, uint32_t len)
{
    aml_stream_t s = { aml, aml + len };
    aml_node_t  *root = aml_ns_root();

    if (!root || !aml || len == 0)
        return false;

    return aml_parse_terms(&s, root);
}
