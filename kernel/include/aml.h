/* An AML interpreter: enough of the ACPI Machine Language to call a method and
 * believe the answer.
 *
 * The firmware describes this machine in a bytecode.  Some of that description
 * is data sitting in a fixed shape -- the S5 sleep type is, which is why
 * acpi.c could read it with a byte scan and no interpreter.  The rest is
 * genuinely a program: the battery's charge comes back from `_BST`, a method
 * that reads the embedded controller, does arithmetic on what it finds, and
 * returns a package.  There is no way to that number except to run it.
 *
 * So this is a real interpreter and not a pattern match.  It builds a
 * namespace out of the DSDT and the SSDTs, and evaluates methods against it:
 * integers, buffers, strings and packages; the arithmetic, logic and control
 * flow; operation regions in memory, in I/O space, in PCI configuration space
 * and in the embedded controller; and fields cut out of those regions at bit
 * granularity.
 *
 * What it is not is a complete implementation.  There are no interrupts, no
 * GPE handling, no notifications, no serialised-method semantics beyond
 * running one thing at a time, and nothing that would let a method block.  The
 * shape of the subset is set by what the battery methods need, which turns out
 * to be most of the language's expression evaluator and not much of its
 * machinery.  Anything unimplemented fails loudly rather than returning a
 * number nobody measured -- an invented charge is worse than no charge.
 */
#ifndef WOS_AML_H
#define WOS_AML_H

#include "types.h"

/* ------------------------------------------------------------------ *
 *  Values
 * ------------------------------------------------------------------ */

enum aml_type {
    AML_UNINIT = 0,      /* a name that exists but has never been stored to */
    AML_INTEGER,
    AML_STRING,
    AML_BUFFER,
    AML_PACKAGE,
    AML_FIELD,           /* a run of bits inside an operation region       */
    AML_REFERENCE,       /* what Index() and RefOf() produce               */
};

typedef struct aml_object aml_object_t;
typedef struct aml_node   aml_node_t;

struct aml_object {
    uint8_t type;
    int32_t refs;

    union {
        uint64_t integer;

        /* Strings and buffers share this.  A string's `bytes` is NUL
         * terminated and `len` excludes the NUL, so it can be handed to
         * anything expecting a C string without being copied. */
        struct {
            uint8_t *bytes;
            uint32_t len;
        } buffer;

        struct {
            aml_object_t **items;
            uint32_t       count;
        } package;

        struct {
            aml_node_t *region;      /* the OperationRegion it cuts into */
            uint32_t    bit_offset;
            uint32_t    bit_len;
            uint8_t     access;      /* bytes per access: 1, 2, 4 or 8    */
        } field;

        /* Index() into a buffer or package yields one of these: the thing
         * indexed, and which element.  Store() through it writes back. */
        struct {
            aml_object_t *target;
            uint32_t      index;
            bool          is_index;  /* false for a plain RefOf */
        } reference;
    };
};

aml_object_t *aml_integer(uint64_t value);
aml_object_t *aml_buffer(const uint8_t *bytes, uint32_t len);
aml_object_t *aml_string(const char *text);
aml_object_t *aml_ref(aml_object_t *o);       /* take a reference   */
void          aml_unref(aml_object_t *o);     /* drop one           */

/* The integer value of anything that can sensibly have one: an integer as
 * itself, a field by reading it, a buffer as its first eight bytes, a string
 * by parsing hex digits.  Returns false for a package. */
bool aml_to_integer(aml_object_t *o, uint64_t *out);

/* ------------------------------------------------------------------ *
 *  The namespace
 * ------------------------------------------------------------------ */

enum aml_node_kind {
    AML_NODE_SCOPE = 0,      /* \, \_SB, and anything Scope() opens      */
    AML_NODE_DEVICE,
    AML_NODE_METHOD,
    AML_NODE_OBJECT,         /* Name(), and the fields of a Field()      */
    AML_NODE_REGION,
    AML_NODE_MUTEX,
};

struct aml_node {
    char        name[5];             /* four characters, NUL terminated */
    uint8_t     kind;

    aml_node_t *parent;
    aml_node_t *children;
    aml_node_t *next;                /* sibling */

    aml_object_t *value;             /* for AML_NODE_OBJECT              */

    /* AML_NODE_METHOD: where its body is, and how many arguments it takes.
     * The body points into the table copy, which is kept for the life of the
     * machine precisely so this stays valid. */
    const uint8_t *code;
    uint32_t       code_len;
    uint8_t        arg_count;

    /* AML_NODE_REGION */
    uint8_t  space;                  /* AML_REGION_*                     */
    uint64_t offset;
    uint64_t length;
};

/* The address spaces an OperationRegion can name.  The ones past
 * EmbeddedControl exist in the specification and not here; a region in one is
 * created so that a Field() over it still parses, and reading it fails. */
#define AML_REGION_MEMORY  0
#define AML_REGION_IO      1
#define AML_REGION_PCI     2
#define AML_REGION_EC      3

/* Look a name up the way AML does: relative to `from` and then up through its
 * parents, unless the path is rooted at `\`.  `from` may be NULL for the root.
 * Returns NULL if there is no such name. */
aml_node_t *aml_lookup(aml_node_t *from, const char *path);

/* ------------------------------------------------------------------ *
 *  Loading and running
 * ------------------------------------------------------------------ */

/* Build the namespace from the DSDT and every SSDT the machine has.  Safe on a
 * machine with no ACPI; aml_ready() then stays false.  Called once, from
 * acpi_init(), after the tables have been found. */
void aml_init(void);

/* True when a namespace was built.  Nothing else here is worth calling when it
 * is false. */
bool aml_ready(void);

/* What the boot log says: how many names were defined, and how many methods.
 * Both zero when aml_ready() is false. */
void aml_stats(uint32_t *names, uint32_t *methods);

/* Evaluate a method, or read a named object.  `path` is an absolute AML path
 * such as "\\_SB_.BAT0._BST".  `args` may be NULL when `argc` is 0.
 *
 * Returns a value the caller owns and releases with aml_unref(), or NULL if
 * the name does not exist or evaluation failed.  A failure is logged; it is
 * never reported as a value. */
aml_object_t *aml_evaluate(const char *path, aml_object_t **args, int argc);

/* The same, for a node already found.  aml_evaluate() is this with a lookup in
 * front of it. */
aml_object_t *aml_evaluate_node(aml_node_t *node, aml_object_t **args, int argc);

/* Walk every device in the namespace whose _HID or _CID matches `id`, calling
 * `fn` for each.  This is how a driver finds its hardware: the battery asks
 * for PNP0C0A and gets the nodes the firmware called batteries.
 *
 * `_HID` may be a string or an integer holding a packed EISA id, and both are
 * compared against `id` as text, so the caller writes "PNP0C0A" either way. */
void aml_each_device(const char *id,
                     void (*fn)(aml_node_t *device, void *user), void *user);

/* Read one element out of a package, as an integer.  Returns false if the
 * object is not a package, the index is past its end, or the element is not
 * something with an integer value -- which is the whole of the error handling
 * a caller of _BST needs. */
bool aml_package_integer(aml_object_t *pkg, uint32_t index, uint64_t *out);

#endif /* WOS_AML_H */
