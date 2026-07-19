/* The AML namespace, and the values that live in it.
 *
 * The namespace is a tree of four-character names.  `\` is the root, `_SB_` is
 * the system bus under it, and a battery is somewhere like `\_SB_.PCI0.BAT0`.
 * Every name is exactly four characters in the bytecode -- shorter ones are
 * padded with underscores, which is why `_SB` and `_SB_` are the same name and
 * why this file pads before it compares.
 *
 * Lookup is the part with a rule worth knowing.  A path that starts with `\`
 * is absolute.  A path with no prefix is searched for in the current scope and
 * then in each parent up to the root -- so a method in `\_SB_.BAT0` that says
 * `_STA` finds its own device's, and one that says `\_SB_.PCI0._STA` says
 * exactly which.  Getting this wrong does not fail; it finds somebody else's
 * object, which is worse.
 */

#include "aml.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

/* ------------------------------------------------------------------ *
 *  Values
 * ------------------------------------------------------------------ */

static aml_object_t *object_alloc(uint8_t type)
{
    aml_object_t *o = kmalloc(sizeof(*o));

    if (!o)
        return NULL;

    memset(o, 0, sizeof(*o));
    o->type = type;
    o->refs = 1;
    return o;
}

aml_object_t *aml_integer(uint64_t value)
{
    aml_object_t *o = object_alloc(AML_INTEGER);

    if (o)
        o->integer = value;
    return o;
}

aml_object_t *aml_buffer(const uint8_t *bytes, uint32_t len)
{
    aml_object_t *o = object_alloc(AML_BUFFER);

    if (!o)
        return NULL;

    /* One byte more than asked for, always zeroed.  A zero-length buffer then
     * still has an allocation to point at, and a buffer used as a string has
     * its terminator without anybody having to remember to add one. */
    o->buffer.bytes = kmalloc(len + 1);
    if (!o->buffer.bytes) {
        kfree(o);
        return NULL;
    }

    if (bytes && len)
        memcpy(o->buffer.bytes, bytes, len);
    else
        memset(o->buffer.bytes, 0, len);

    o->buffer.bytes[len] = 0;
    o->buffer.len        = len;
    return o;
}

aml_object_t *aml_string(const char *text)
{
    uint32_t len = text ? (uint32_t)strlen(text) : 0;

    aml_object_t *o = aml_buffer((const uint8_t *)text, len);
    if (o)
        o->type = AML_STRING;
    return o;
}

aml_object_t *aml_ref(aml_object_t *o)
{
    if (o)
        o->refs++;
    return o;
}

void aml_unref(aml_object_t *o)
{
    if (!o || --o->refs > 0)
        return;

    switch (o->type) {
    case AML_STRING:
    case AML_BUFFER:
        kfree(o->buffer.bytes);
        break;

    case AML_PACKAGE:
        for (uint32_t i = 0; i < o->package.count; i++)
            aml_unref(o->package.items[i]);
        kfree(o->package.items);
        break;

    case AML_REFERENCE:
        aml_unref(o->reference.target);
        break;

    default:
        break;
    }

    kfree(o);
}

/* Hex digits, for a string used as a number.  AML's ToInteger takes a decimal
 * string or an "0x"-prefixed hex one; this takes both, and stops at the first
 * character that is neither. */
bool aml_to_integer(aml_object_t *o, uint64_t *out)
{
    if (!o)
        return false;

    switch (o->type) {
    case AML_INTEGER:
        *out = o->integer;
        return true;

    case AML_UNINIT:
        *out = 0;
        return true;

    case AML_BUFFER: {
        /* The first eight bytes, little endian, and short buffers are not an
         * error -- a two-byte buffer is a sixteen-bit number. */
        uint64_t v = 0;
        uint32_t n = o->buffer.len < 8 ? o->buffer.len : 8;

        for (uint32_t i = 0; i < n; i++)
            v |= (uint64_t)o->buffer.bytes[i] << (i * 8);

        *out = v;
        return true;
    }

    case AML_STRING: {
        const char *s = (const char *)o->buffer.bytes;
        uint64_t    v = 0;
        int         base = 10;

        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        }

        for (; *s; s++) {
            int digit;

            if (*s >= '0' && *s <= '9')      digit = *s - '0';
            else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
            else break;

            if (digit >= base)
                break;

            v = v * (uint64_t)base + (uint64_t)digit;
        }

        *out = v;
        return true;
    }

    default:
        return false;      /* packages, references and fields go elsewhere */
    }
}

bool aml_package_integer(aml_object_t *pkg, uint32_t index, uint64_t *out)
{
    if (!pkg || pkg->type != AML_PACKAGE || index >= pkg->package.count)
        return false;

    return aml_to_integer(pkg->package.items[index], out);
}

/* ------------------------------------------------------------------ *
 *  The tree
 * ------------------------------------------------------------------ */

static aml_node_t *root;
static uint32_t    node_count;
static uint32_t    method_count;

aml_node_t *aml_ns_root(void)
{
    if (root)
        return root;

    root = kmalloc(sizeof(*root));
    if (!root)
        return NULL;

    memset(root, 0, sizeof(*root));
    strlcpy(root->name, "\\", sizeof(root->name));
    root->kind = AML_NODE_SCOPE;
    return root;
}

void aml_ns_counts(uint32_t *names, uint32_t *methods)
{
    *names   = node_count;
    *methods = method_count;
}

/* A name segment as it is written in the bytecode: four characters, padded
 * with underscores.  Comparing padded forms means `_SB` and `_SB_` are one
 * name, which they are. */
static void pad_segment(const char *in, uint32_t len, char out[5])
{
    uint32_t i = 0;

    for (; i < 4 && i < len && in[i]; i++)
        out[i] = in[i];
    for (; i < 4; i++)
        out[i] = '_';

    out[4] = '\0';
}

static aml_node_t *child_named(aml_node_t *parent, const char seg[5])
{
    if (!parent)
        return NULL;

    for (aml_node_t *n = parent->children; n; n = n->next)
        if (memcmp(n->name, seg, 4) == 0)
            return n;

    return NULL;
}

aml_node_t *aml_ns_child(aml_node_t *parent, const char *seg, uint32_t seg_len)
{
    char padded[5];

    pad_segment(seg, seg_len, padded);
    return child_named(parent, padded);
}

/* Find a child, or make one.  Redefinition is normal rather than exceptional:
 * an SSDT routinely opens a Scope() on a device the DSDT already declared, and
 * the second Scope() must land in the first one's node rather than beside it.
 */
aml_node_t *aml_ns_define(aml_node_t *parent, const char *seg, uint32_t seg_len,
                          uint8_t kind)
{
    char padded[5];

    if (!parent)
        return NULL;

    pad_segment(seg, seg_len, padded);

    aml_node_t *n = child_named(parent, padded);
    if (n) {
        /* An existing scope being reopened as a device, or the other way
         * about, keeps the more specific kind.  A plain scope is the one that
         * gives way, since that is what an implicit parent was created as. */
        if (n->kind == AML_NODE_SCOPE && kind != AML_NODE_SCOPE)
            n->kind = kind;
        return n;
    }

    n = kmalloc(sizeof(*n));
    if (!n)
        return NULL;

    memset(n, 0, sizeof(*n));
    memcpy(n->name, padded, 5);
    n->kind   = kind;
    n->parent = parent;

    /* Appended rather than pushed, so a walk of the namespace comes out in
     * the order the firmware declared things.  That only matters for the boot
     * log, but a log whose order changes between boots is a log nobody
     * trusts. */
    aml_node_t **at = &parent->children;
    while (*at)
        at = &(*at)->next;
    *at = n;

    node_count++;
    if (kind == AML_NODE_METHOD)
        method_count++;

    return n;
}

/* ------------------------------------------------------------------ *
 *  Lookup
 * ------------------------------------------------------------------ */

/* One step of a path: up to four characters, ending at a '.' or the end. */
static uint32_t segment_length(const char *p)
{
    uint32_t n = 0;

    while (p[n] && p[n] != '.')
        n++;
    return n;
}

/* Follow a dotted path down from `at`, with no searching upwards. */
static aml_node_t *walk_down(aml_node_t *at, const char *path)
{
    while (*path && at) {
        uint32_t len = segment_length(path);

        at = aml_ns_child(at, path, len);
        path += len;
        if (*path == '.')
            path++;
    }

    return at;
}

aml_node_t *aml_lookup(aml_node_t *from, const char *path)
{
    aml_node_t *base = aml_ns_root();

    if (!path || !base)
        return NULL;

    /* Rooted: no searching, the path says exactly where. */
    if (*path == '\\') {
        path++;
        while (*path == '.')
            path++;
        if (!*path)
            return base;
        return walk_down(base, path);
    }

    /* `^` climbs one level per caret, and is also not a search. */
    if (*path == '^') {
        aml_node_t *at = from ? from : base;

        while (*path == '^') {
            if (at->parent)
                at = at->parent;
            path++;
        }
        if (*path == '.')
            path++;
        return *path ? walk_down(at, path) : at;
    }

    /* Relative: this scope, then each parent, up to the root.  Only the first
     * segment is searched for -- once it matches, the rest of the path is
     * followed from there, and a miss is a miss.  That is the rule in the
     * specification, and doing it per segment instead would let a path find
     * half of itself in one scope and half in another. */
    if (!from)
        from = base;

    uint32_t first = segment_length(path);

    for (aml_node_t *at = from; at; at = at->parent) {
        aml_node_t *hit = aml_ns_child(at, path, first);

        if (hit) {
            const char *rest = path + first;
            if (*rest == '.')
                rest++;
            return *rest ? walk_down(hit, rest) : hit;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ *
 *  Walking it
 * ------------------------------------------------------------------ */

/* The full path of a node, for the log.  Built backwards into the end of the
 * buffer, since a node knows its parent and not its children. */
void aml_ns_path(const aml_node_t *n, char *out, uint32_t size)
{
    if (size == 0)
        return;

    char     tmp[256];
    uint32_t at = sizeof(tmp);

    tmp[--at] = '\0';

    for (const aml_node_t *p = n; p && p->parent; p = p->parent) {
        uint32_t len = 4;

        /* Trailing underscores are padding, and printing them makes every
         * path in the log four characters wider than anybody writes it. */
        while (len > 1 && p->name[len - 1] == '_')
            len--;

        if (at < len + 1)
            break;

        at -= len;
        memcpy(tmp + at, p->name, len);
        tmp[--at] = p->parent->parent ? '.' : '\\';
    }

    strlcpy(out, tmp + at, size);
}

static void walk(aml_node_t *n, void (*fn)(aml_node_t *, void *), void *user)
{
    for (aml_node_t *c = n->children; c; c = c->next) {
        fn(c, user);
        walk(c, fn, user);
    }
}

void aml_ns_walk(void (*fn)(aml_node_t *, void *), void *user)
{
    aml_node_t *r = aml_ns_root();

    if (r)
        walk(r, fn, user);
}
