/* Building the namespace, and finding things in it.
 *
 * The description of a machine is not one table.  The DSDT holds most of it,
 * and any number of SSDTs add to it -- a laptop routinely has half a dozen,
 * and the battery is as likely to be in one of those as in the DSDT.  They are
 * loaded in the order the root table lists them, which is the order the
 * firmware means them to be read: a later table may open a Scope() on a device
 * an earlier one declared, and doing it the other way round would create the
 * scope twice.
 *
 * The table copies are kept rather than freed.  A method's body is not copied
 * out of them -- a namespace node points into the table it was declared in --
 * so freeing one would leave every method in it pointing at reused heap.
 */

#include "aml_internal.h"
#include "acpi.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

#define MAX_TABLES 24

static uint8_t *tables[MAX_TABLES];
static int      table_count;
static bool     ready;

uint32_t aml_parse_skipped(void);

bool aml_ready(void) { return ready; }

void aml_stats(uint32_t *names, uint32_t *methods)
{
    if (!ready) {
        *names = *methods = 0;
        return;
    }

    aml_ns_counts(names, methods);
}

/* The header every ACPI table starts with; only the length matters here. */
struct table_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* acpi.c hands out copies of tables by signature.  This wants the bytes and
 * the length together, and to keep the copy. */
uint8_t *acpi_table_bytes(const char *signature, uint32_t *length_out)
{
    uint32_t len = 0;
    void    *t   = acpi_table(signature, &len);

    if (!t)
        return NULL;

    if (length_out)
        *length_out = len;
    return t;
}

static void load_one(uint8_t *table, uint32_t len)
{
    struct table_header *h = (struct table_header *)table;

    if (len <= sizeof(*h) || h->length > len)
        return;

    if (table_count >= MAX_TABLES) {
        kfree(table);
        return;
    }

    tables[table_count++] = table;

    aml_load_block(table + sizeof(*h), h->length - (uint32_t)sizeof(*h));
}

void aml_init(void)
{
    uint32_t len = 0;

    uint8_t *dsdt = acpi_dsdt(&len);
    if (!dsdt) {
        kprintf("aml    : no DSDT; nothing to interpret\n");
        return;
    }

    load_one(dsdt, len);

    /* Every SSDT, in the order the root table lists them. */
    for (int i = 0; ; i++) {
        uint32_t slen = 0;
        uint8_t *ssdt = acpi_table_nth("SSDT", i, &slen);

        if (!ssdt)
            break;

        load_one(ssdt, slen);
    }

    ready = true;

    uint32_t names, methods;
    aml_ns_counts(&names, &methods);

    uint32_t skipped = aml_parse_skipped();

    kprintf("aml    : %u names, %u methods, from %d table%s%s\n",
            names, methods, table_count, table_count == 1 ? "" : "s",
            skipped ? "" : "");

    if (skipped)
        kprintf("aml    : %u construct%s stepped over -- not implemented\n",
                skipped, skipped == 1 ? "" : "s");

    /* The embedded controller last, because finding it may mean looking for a
     * PNP0C09 device, which needs the namespace to exist. */
    aml_ec_init();
}

/* ------------------------------------------------------------------ *
 *  Finding a device by its hardware id
 * ------------------------------------------------------------------ */

/* _HID is either a string, or an integer holding an EISA id: three letters
 * packed five bits each, then four hex digits, in big-endian order with the
 * whole thing byte-swapped.  Both forms mean the same thing and firmware uses
 * whichever it feels like, so both are turned into text and compared. */
static void eisa_id_text(uint64_t id, char out[8])
{
    static const char hex[] = "0123456789ABCDEF";

    uint32_t v = (uint32_t)id;
    uint16_t manufacturer = (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));

    out[0] = (char)('@' + ((manufacturer >> 10) & 0x1F));
    out[1] = (char)('@' + ((manufacturer >> 5) & 0x1F));
    out[2] = (char)('@' + (manufacturer & 0x1F));
    out[3] = hex[(v >> 20) & 0x0F];
    out[4] = hex[(v >> 16) & 0x0F];
    out[5] = hex[(v >> 28) & 0x0F];
    out[6] = hex[(v >> 24) & 0x0F];
    out[7] = '\0';
}

static bool object_is_id(aml_object_t *o, const char *id)
{
    if (!o)
        return false;

    if (o->type == AML_STRING)
        return strcmp((const char *)o->buffer.bytes, id) == 0;

    if (o->type == AML_INTEGER) {
        char text[8];

        eisa_id_text(o->integer, text);
        return strcmp(text, id) == 0;
    }

    /* _CID may be a package of several ids. */
    if (o->type == AML_PACKAGE) {
        for (uint32_t i = 0; i < o->package.count; i++)
            if (object_is_id(o->package.items[i], id))
                return true;
    }

    return false;
}

struct search {
    const char *id;
    void      (*fn)(aml_node_t *, void *);
    void       *user;
};

static void consider(aml_node_t *n, void *user)
{
    struct search *search = user;

    if (n->kind != AML_NODE_DEVICE)
        return;

    for (int which = 0; which < 2; which++) {
        aml_node_t *id = aml_ns_child(n, which ? "_CID" : "_HID", 4);

        if (!id)
            continue;

        aml_object_t *v = aml_evaluate_node(id, NULL, 0);
        bool          hit = object_is_id(v, search->id);

        aml_unref(v);

        if (hit) {
            search->fn(n, search->user);
            return;
        }
    }
}

void aml_each_device(const char *id,
                     void (*fn)(aml_node_t *device, void *user), void *user)
{
    struct search search = { id, fn, user };

    if (!ready)
        return;

    aml_ns_walk(consider, &search);
}
