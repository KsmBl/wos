/* What the four AML files say to each other.  Not an interface anybody outside
 * kernel/acpi/ should need: aml.h is that. */
#ifndef WOS_AML_INTERNAL_H
#define WOS_AML_INTERNAL_H

#include "aml.h"

/* ------------------------------------------------------------------ *
 *  Reading the bytecode
 * ------------------------------------------------------------------ */

/* A position in a definition block.  `end` is one past the last byte the
 * current construct may read, so a malformed package length cannot walk a
 * parser off the end of the table -- every read checks it. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} aml_stream_t;

static inline bool aml_left(const aml_stream_t *s, uint32_t n)
{
    return (uint32_t)(s->end - s->p) >= n;
}

static inline uint8_t aml_peek(const aml_stream_t *s)
{
    return aml_left(s, 1) ? *s->p : 0;
}

static inline uint8_t aml_byte(aml_stream_t *s)
{
    return aml_left(s, 1) ? *s->p++ : 0;
}

/* A package length: the top two bits of the first byte say how many more
 * bytes follow, and the encoding of the first byte differs between the
 * one-byte form and the rest.  Returns the length including its own bytes. */
uint32_t aml_pkg_length(aml_stream_t *s);

/* A name string, written into `out` as a dotted path with a leading '\' or
 * '^'s preserved.  Returns false on a malformed one. */
bool aml_name_string(aml_stream_t *s, char *out, uint32_t size);

/* ------------------------------------------------------------------ *
 *  The namespace, from inside
 * ------------------------------------------------------------------ */

aml_node_t *aml_ns_root(void);
aml_node_t *aml_ns_child(aml_node_t *parent, const char *seg, uint32_t seg_len);
aml_node_t *aml_ns_define(aml_node_t *parent, const char *seg, uint32_t seg_len,
                          uint8_t kind);
void        aml_ns_counts(uint32_t *names, uint32_t *methods);
void        aml_ns_path(const aml_node_t *n, char *out, uint32_t size);
void        aml_ns_walk(void (*fn)(aml_node_t *, void *), void *user);

/* Create every node along a path, returning the last.  Used when a Scope() or
 * a Device() names something whose parents have not been declared. */
aml_node_t *aml_ns_create_path(aml_node_t *from, const char *path, uint8_t kind);

/* A table's bytes and length together, the copy kept for the caller.  A thin
 * wrapper on acpi_table() that the region code and the loader both want. */
uint8_t *acpi_table_bytes(const char *signature, uint32_t *length_out);

/* How many constructs the load pass stepped over, for the boot log. */
uint32_t aml_parse_skipped(void);

/* ------------------------------------------------------------------ *
 *  Loading
 * ------------------------------------------------------------------ */

/* Read one definition block -- the body of a DSDT or an SSDT -- into the
 * namespace.  Returns false only if the block is unusable. */
bool aml_load_block(const uint8_t *aml, uint32_t len);

/* One TermList, in the scope of `scope`.  The declaration parser and the
 * interpreter share it: at load time it defines things, and inside a method
 * the same opcodes may appear and mean the same. */
bool aml_parse_terms(aml_stream_t *s, aml_node_t *scope);

/* ------------------------------------------------------------------ *
 *  Running
 * ------------------------------------------------------------------ */

#define AML_MAX_ARGS   7
#define AML_MAX_LOCALS 8

typedef struct {
    aml_object_t *args[AML_MAX_ARGS];
    aml_object_t *locals[AML_MAX_LOCALS];

    aml_object_t *result;        /* what Return() left           */
    bool          returned;
    bool          broke;         /* Break, inside a While        */
    bool          continued;

    aml_node_t   *scope;         /* for relative name lookups    */
    int           depth;         /* method nesting, to stop runaway recursion */
} aml_ctx_t;

/* Evaluate one term and yield its value, or NULL for a term that has none
 * (a Store, an If).  `*failed` is set when something went wrong, which is not
 * the same as a term with no value. */
aml_object_t *aml_eval_term(aml_stream_t *s, aml_ctx_t *ctx, bool *failed);

/* Run a TermList as code until it ends or returns. */
bool aml_run_terms(aml_stream_t *s, aml_ctx_t *ctx);

/* ------------------------------------------------------------------ *
 *  Operation regions
 * ------------------------------------------------------------------ */

/* Read and write a field's bits.  These are where an OperationRegion turns
 * into an inb() or a memory access, and where the embedded controller's
 * handshake happens. */
aml_object_t *aml_field_read(aml_object_t *field);
bool          aml_field_write(aml_object_t *field, uint64_t value);

/* The embedded controller, which is the region space a battery's fields live
 * in on every laptop.  Returns false when there is no EC or it did not
 * answer. */
bool aml_ec_read(uint8_t offset, uint8_t *out);
bool aml_ec_write(uint8_t offset, uint8_t value);
void aml_ec_init(void);
bool aml_ec_present(void);

#endif /* WOS_AML_INTERNAL_H */
