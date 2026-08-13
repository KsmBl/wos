/* The linker: objects and archives into something the machine can run.
 *
 * It reads ELF64 relocatable objects -- the ones this compiler writes and the
 * ones the host's gcc put in libwkernel.a, which have to work identically --
 * lays their sections out at the addresses WOS loads a program at, resolves
 * the symbols between them, applies the relocations and writes an ET_EXEC.
 *
 * The layout is lib/wkernel/user.ld expressed in code: text at 0x40000000,
 * then read-only data, then data, then bss, each starting on a page because
 * the kernel's loader maps whole pages and two segments with different
 * permissions cannot share one.
 *
 * Five relocation types appear in practice and all five are here.  Anything
 * else is refused by name rather than ignored, because a relocation silently
 * left unapplied is a program that runs until it reaches that address.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcc.h"

#define USER_BASE  0x40000000UL
#define PAGE_SIZE  0x1000UL

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8

#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_WRITE     0x1

#define SHN_UNDEF  0
#define SHN_ABS    0xFFF1
#define SHN_COMMON 0xFFF2

#define STT_SECTION 3
#define STT_FILE    4

/* ------------------------------------------------------------------ *
 *  Reading the pieces of an ELF file
 * ------------------------------------------------------------------ */

static unsigned long read_u(const unsigned char *p, int bytes)
{
    unsigned long value = 0;

    for (int i = 0; i < bytes; i++)
        value |= (unsigned long)p[i] << (i * 8);

    return value;
}

static void write_u(unsigned char *p, unsigned long value, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (unsigned char)(value >> (i * 8));
}

typedef struct object {
    struct object *next;
    const char    *name;          /* for messages: file, or archive(member) */
    unsigned char *image;
    long           size;

    int            section_count;
    const unsigned char *sections;    /* the section header table */
    const unsigned char *symtab;
    long                 symbol_count;
    const char          *symstr;
    const char          *shstr;

    /* Where each of this object's sections ended up, and whether it was
     * placed at all. */
    unsigned long *placed_at;
    int           *is_placed;
} object_t;

static object_t *objects;
static object_t *objects_tail;

/* A section header field, by index. */
static const unsigned char *section_header(const object_t *o, int index)
{
    return o->sections + (long)index * 64;
}

static unsigned long sh_field(const object_t *o, int index, int offset,
                              int bytes)
{
    return read_u(section_header(o, index) + offset, bytes);
}

#define SH_NAME(o, i)      sh_field(o, i, 0, 4)
#define SH_TYPE(o, i)      sh_field(o, i, 4, 4)
#define SH_FLAGS(o, i)     sh_field(o, i, 8, 8)
#define SH_OFFSET(o, i)    sh_field(o, i, 24, 8)
#define SH_SIZE(o, i)      sh_field(o, i, 32, 8)
#define SH_LINK(o, i)      sh_field(o, i, 40, 4)
#define SH_INFO(o, i)      sh_field(o, i, 44, 4)
#define SH_ALIGN(o, i)     sh_field(o, i, 48, 8)

static const char *section_name(const object_t *o, int index)
{
    return o->shstr + SH_NAME(o, index);
}

/* One symbol table entry: name, info, section, value, size. */
static const unsigned char *symbol_entry(const object_t *o, long index)
{
    return o->symtab + index * 24;
}

static const char *symbol_name(const object_t *o, long index)
{
    return o->symstr + read_u(symbol_entry(o, index), 4);
}

static int symbol_bind(const object_t *o, long index)
{
    return symbol_entry(o, index)[4] >> 4;
}

static int symbol_type(const object_t *o, long index)
{
    return symbol_entry(o, index)[4] & 0xF;
}

static int symbol_section(const object_t *o, long index)
{
    return (int)read_u(symbol_entry(o, index) + 6, 2);
}

static unsigned long symbol_value(const object_t *o, long index)
{
    return read_u(symbol_entry(o, index) + 8, 8);
}

static unsigned long symbol_size(const object_t *o, long index)
{
    return read_u(symbol_entry(o, index) + 16, 8);
}

/* Parse enough of an object to work with, or return NULL if it is not one. */
static object_t *parse_object(const char *name, unsigned char *image, long size)
{
    if (size < 64 || image[0] != 0x7F || memcmp(image + 1, "ELF", 3) != 0)
        return NULL;

    if (image[4] != 2 || image[5] != 1)
        fatal("%s: not a 64-bit little-endian object", name);

    if (read_u(image + 16, 2) != 1)
        fatal("%s: not a relocatable object", name);

    object_t *o = wcc_alloc(sizeof(*o));

    o->name  = name;
    o->image = image;
    o->size  = size;

    unsigned long shoff = read_u(image + 40, 8);
    o->section_count = (int)read_u(image + 60, 2);
    o->sections      = image + shoff;

    int shstrndx = (int)read_u(image + 62, 2);
    o->shstr = (const char *)(image + SH_OFFSET(o, shstrndx));

    for (int i = 0; i < o->section_count; i++) {
        if (SH_TYPE(o, i) == SHT_SYMTAB) {
            o->symtab       = image + SH_OFFSET(o, i);
            o->symbol_count = (long)(SH_SIZE(o, i) / 24);
            o->symstr       = (const char *)(image + SH_OFFSET(o, (int)SH_LINK(o, i)));
        }
    }

    o->placed_at = wcc_alloc(sizeof(unsigned long) * (size_t)o->section_count);
    o->is_placed = wcc_alloc(sizeof(int) * (size_t)o->section_count);
    return o;
}

/* ------------------------------------------------------------------ *
 *  The symbol table across every object
 * ------------------------------------------------------------------ */

typedef struct global_symbol {
    struct global_symbol *next;
    const char           *name;
    object_t             *owner;
    long                  index;      /* within the owner's symbol table */
    unsigned long         address;    /* filled in once the layout is known */
    int                   resolved;
    unsigned long         common_size;
    unsigned long         common_align;
} global_symbol_t;

static global_symbol_t *global_symbols;

static global_symbol_t *find_global(const char *name)
{
    for (global_symbol_t *g = global_symbols; g; g = g->next)
        if (strcmp(g->name, name) == 0)
            return g;

    return NULL;
}

static global_symbol_t *intern_global(const char *name)
{
    global_symbol_t *g = find_global(name);

    if (g)
        return g;

    g = wcc_alloc(sizeof(*g));
    g->name = name;
    g->next = global_symbols;
    global_symbols = g;
    return g;
}

/* Take an object into the link: its defined globals become available and its
 * undefined ones become things still to find. */
static void include_object(object_t *o)
{
    if (objects_tail)
        objects_tail = objects_tail->next = o;
    else
        objects = objects_tail = o;

    for (long i = 0; i < o->symbol_count; i++) {
        if (symbol_bind(o, i) != 1)               /* only the globals */
            continue;

        const char *name = symbol_name(o, i);
        if (!name || !*name)
            continue;

        global_symbol_t *g = intern_global(name);
        int section = symbol_section(o, i);

        if (section == SHN_UNDEF)
            continue;

        if (section == SHN_COMMON) {
            /* A tentative definition: whoever needs the most space wins, and
             * it ends up in .bss. */
            if (symbol_value(o, i) > g->common_align)
                g->common_align = symbol_value(o, i);
            if (symbol_size(o, i) > g->common_size)
                g->common_size = symbol_size(o, i);
            continue;
        }

        if (g->resolved) {
            /* Two definitions of the same name.  The first one wins, which is
             * what makes an archive member that duplicates something already
             * linked harmless. */
            continue;
        }

        g->owner    = o;
        g->index    = i;
        g->resolved = 1;
    }
}

static int has_undefined_symbols(void)
{
    for (global_symbol_t *g = global_symbols; g; g = g->next)
        if (!g->resolved && !g->common_size)
            return 1;

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Archives
 * ------------------------------------------------------------------ */

typedef struct archive_member {
    struct archive_member *next;
    char                  *name;
    unsigned char         *data;
    long                   size;
    int                    included;
} archive_member_t;

typedef struct archive {
    struct archive   *next;
    const char       *path;
    archive_member_t *members;
} archive_t;

static archive_t *archives;

static long parse_ar_number(const char *field, int len)
{
    long value = 0;

    for (int i = 0; i < len && field[i] >= '0' && field[i] <= '9'; i++)
        value = value * 10 + (field[i] - '0');

    return value;
}

static archive_t *read_archive(const char *path, unsigned char *image,
                               long size)
{
    archive_t *a = wcc_alloc(sizeof(*a));
    a->path = path;

    archive_member_t *tail = NULL;
    const char       *long_names = NULL;
    long              at = 8;                    /* past "!<arch>\n" */

    while (at + 60 <= size) {
        const char *header = (const char *)(image + at);
        long        member_size = parse_ar_number(header + 48, 10);
        char        raw_name[17];

        memcpy(raw_name, header, 16);
        raw_name[16] = '\0';

        /* Trailing spaces, and the '/' that terminates a short name. */
        for (int i = 15; i >= 0; i--) {
            if (raw_name[i] == ' ')
                raw_name[i] = '\0';
            else
                break;
        }

        unsigned char *data = image + at + 60;
        at += 60 + member_size + (member_size & 1);

        /* "/" is the symbol index and "//" the table of long names; neither
         * is a member to link. */
        if (strcmp(raw_name, "/") == 0 || strcmp(raw_name, "/SYM64/") == 0)
            continue;

        if (strcmp(raw_name, "//") == 0) {
            long_names = (const char *)data;
            continue;
        }

        char *name;
        if (raw_name[0] == '/' && long_names) {
            long offset = parse_ar_number(raw_name + 1, 15);
            const char *from = long_names + offset;
            long len = 0;
            while (from[len] && from[len] != '/' && from[len] != '\n')
                len++;
            name = wcc_strndup(from, (size_t)len);
        } else {
            size_t len = strlen(raw_name);
            if (len && raw_name[len - 1] == '/')
                raw_name[len - 1] = '\0';
            name = wcc_strdup(raw_name);
        }

        archive_member_t *m = wcc_alloc(sizeof(*m));
        m->name = name;
        m->data = data;
        m->size = member_size;

        if (tail)
            tail = tail->next = m;
        else
            a->members = tail = m;
    }

    return a;
}

/* Does this member define something still missing? */
static int member_answers_something(object_t *o)
{
    for (long i = 0; i < o->symbol_count; i++) {
        if (symbol_bind(o, i) != 1 || symbol_section(o, i) == SHN_UNDEF)
            continue;

        global_symbol_t *g = find_global(symbol_name(o, i));
        if (g && !g->resolved)
            return 1;
    }

    return 0;
}

/* Pull members out of the archives until no pass adds anything, which is what
 * makes an archive whose members depend on each other work whatever order
 * they are stored in. */
static void resolve_from_archives(void)
{
    int progress = 1;

    while (progress) {
        progress = 0;

        for (archive_t *a = archives; a; a = a->next) {
            for (archive_member_t *m = a->members; m; m = m->next) {
                if (m->included)
                    continue;

                char *label = wcc_alloc(strlen(a->path) + strlen(m->name) + 4);
                sprintf(label, "%s(%s)", a->path, m->name);

                object_t *o = parse_object(label, m->data, m->size);
                if (!o) {
                    m->included = 1;      /* not an object; nothing to take */
                    continue;
                }

                if (!member_answers_something(o))
                    continue;

                m->included = 1;
                include_object(o);
                progress = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Laying the output out
 * ------------------------------------------------------------------ */

typedef enum { OUT_TEXT, OUT_RODATA, OUT_DATA, OUT_BSS, OUT_COUNT } out_id_t;

/* Which output section an input section belongs in, by name. */
static int output_for(const object_t *o, int index)
{
    const char   *name  = section_name(o, index);
    unsigned long flags = SH_FLAGS(o, index);

    if (!(flags & SHF_ALLOC))
        return -1;

    /* .eh_frame is allocated and describes how to unwind the stack; nothing
     * here unwinds, and user.ld discards it for the same reason. */
    if (strncmp(name, ".eh_frame", 9) == 0 || strncmp(name, ".note", 5) == 0)
        return -1;

    if (SH_TYPE(o, index) == SHT_NOBITS)
        return OUT_BSS;

    if (flags & SHF_EXECINSTR)
        return OUT_TEXT;

    if (flags & SHF_WRITE)
        return OUT_DATA;

    return OUT_RODATA;
}

static unsigned long align_up(unsigned long value, unsigned long alignment)
{
    if (alignment < 1)
        alignment = 1;

    return (value + alignment - 1) / alignment * alignment;
}

int link_executable(char **inputs, int input_count, const char *path)
{
    /* --- read everything --- */
    for (int i = 0; i < input_count; i++) {
        FILE *f = fopen(inputs[i], "rb");
        if (!f) {
            fprintf(stderr, "wcc: cannot open %s\n", inputs[i]);
            return 1;
        }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);

        unsigned char *image = wcc_alloc((size_t)size);
        if (fread(image, 1, (size_t)size, f) != (size_t)size) {
            fprintf(stderr, "wcc: short read on %s\n", inputs[i]);
            fclose(f);
            return 1;
        }
        fclose(f);

        if (size >= 8 && memcmp(image, "!<arch>\n", 8) == 0) {
            archive_t *a = read_archive(inputs[i], image, size);
            a->next  = archives;
            archives = a;
            continue;
        }

        object_t *o = parse_object(inputs[i], image, size);
        if (!o) {
            fprintf(stderr, "wcc: %s is not an object or an archive\n",
                    inputs[i]);
            return 1;
        }

        include_object(o);
    }

    /* The entry point has to be looked for like anything else, or crt0 would
     * never be pulled out of the library. */
    intern_global("_start");
    resolve_from_archives();

    if (has_undefined_symbols()) {
        for (global_symbol_t *g = global_symbols; g; g = g->next)
            if (!g->resolved && !g->common_size)
                fprintf(stderr, "wcc: undefined symbol: %s\n", g->name);
        return 1;
    }

    /* --- lay out the sections --- */
    unsigned long address = USER_BASE;
    buffer_t      out[OUT_COUNT];
    unsigned long start[OUT_COUNT], length[OUT_COUNT];

    for (int i = 0; i < OUT_COUNT; i++)
        buf_init(&out[i]);

    for (int which = 0; which < OUT_COUNT; which++) {
        address = align_up(address, PAGE_SIZE);
        start[which] = address;

        for (object_t *o = objects; o; o = o->next) {
            for (int s = 1; s < o->section_count; s++) {
                if (output_for(o, s) != which)
                    continue;

                unsigned long alignment = SH_ALIGN(o, s);
                unsigned long here = start[which] + (unsigned long)out[which].len;
                unsigned long aligned = align_up(here, alignment);

                while (start[which] + (unsigned long)out[which].len < aligned)
                    buf_byte(&out[which], 0);

                o->placed_at[s] = aligned;
                o->is_placed[s] = 1;

                if (SH_TYPE(o, s) == SHT_NOBITS) {
                    /* No contents in the file; the loader zeroes it. */
                    out[which].len += (int)SH_SIZE(o, s);
                } else {
                    buf_put(&out[which], o->image + SH_OFFSET(o, s),
                            (int)SH_SIZE(o, s));
                }
            }
        }

        /* Anything still tentative gets its space at the end of .bss. */
        if (which == OUT_BSS) {
            for (global_symbol_t *g = global_symbols; g; g = g->next) {
                if (g->resolved || !g->common_size)
                    continue;

                unsigned long here = start[which] +
                                     (unsigned long)out[which].len;
                unsigned long aligned = align_up(here, g->common_align);

                out[which].len += (int)(aligned - here);
                g->address  = aligned;
                g->resolved = 1;
                out[which].len += (int)g->common_size;
            }
        }

        length[which] = (unsigned long)out[which].len;
        address = start[which] + length[which];
    }

    /* --- give every symbol its address --- */
    for (global_symbol_t *g = global_symbols; g; g = g->next) {
        if (g->address)
            continue;                            /* a common symbol */

        object_t *o = g->owner;
        int section = symbol_section(o, g->index);

        if (section == SHN_ABS) {
            g->address = symbol_value(o, g->index);
            continue;
        }

        if (!o->is_placed[section]) {
            /* Defined in a section that was dropped -- .eh_frame or a note.
             * Nothing can point at it usefully, and saying so beats a zero. */
            fprintf(stderr, "wcc: %s is defined in a section that is not "
                            "loaded (%s)\n", g->name, section_name(o, section));
            return 1;
        }

        g->address = o->placed_at[section] + symbol_value(o, g->index);
    }

    /* --- apply the relocations --- */
    for (object_t *o = objects; o; o = o->next) {
        for (int s = 1; s < o->section_count; s++) {
            if (SH_TYPE(o, s) != SHT_RELA)
                continue;

            int target = (int)SH_INFO(o, s);
            if (target <= 0 || target >= o->section_count || !o->is_placed[target])
                continue;

            int which = output_for(o, target);
            if (which < 0 || which == OUT_BSS)
                continue;

            long count = (long)(SH_SIZE(o, s) / 24);
            const unsigned char *entries = o->image + SH_OFFSET(o, s);

            for (long i = 0; i < count; i++) {
                const unsigned char *r = entries + i * 24;

                unsigned long offset = read_u(r, 8);
                unsigned long info   = read_u(r + 8, 8);
                long          addend = (long)read_u(r + 16, 8);

                long type   = (long)(info & 0xFFFFFFFFUL);
                long symbol = (long)(info >> 32);

                /* Where the fixup goes, both in memory and in our buffer. */
                unsigned long at = o->placed_at[target] + offset;
                unsigned char *fix = out[which].data +
                                     (at - start[which]);

                /* What it points at. */
                unsigned long value;

                if (symbol_type(o, symbol) == STT_SECTION) {
                    int in_section = symbol_section(o, symbol);
                    if (!o->is_placed[in_section])
                        continue;
                    value = o->placed_at[in_section];
                } else if (symbol_section(o, symbol) == SHN_UNDEF ||
                           symbol_bind(o, symbol) == 1) {
                    global_symbol_t *g = find_global(symbol_name(o, symbol));

                    if (!g || !g->resolved) {
                        fprintf(stderr, "wcc: undefined symbol: %s\n",
                                symbol_name(o, symbol));
                        return 1;
                    }
                    value = g->address;
                } else if (symbol_section(o, symbol) == SHN_ABS) {
                    value = symbol_value(o, symbol);
                } else {
                    int in_section = symbol_section(o, symbol);
                    if (!o->is_placed[in_section])
                        continue;
                    value = o->placed_at[in_section] +
                            symbol_value(o, symbol);
                }

                switch (type) {
                case R_X86_64_64:
                    write_u(fix, value + (unsigned long)addend, 8);
                    break;

                case R_X86_64_32:
                case R_X86_64_32S:
                    write_u(fix, value + (unsigned long)addend, 4);
                    break;

                case R_X86_64_PC32:
                case R_X86_64_PLT32:
                    write_u(fix, (unsigned long)((long)value + addend -
                                                 (long)at), 4);
                    break;

                default:
                    fprintf(stderr, "wcc: %s: relocation type %ld is not "
                                    "supported\n", o->name, type);
                    return 1;
                }
            }
        }
    }

    /* --- write the executable --- */
    global_symbol_t *entry = find_global("_start");
    if (!entry || !entry->resolved) {
        fprintf(stderr, "wcc: no _start -- is libwkernel.a on the command "
                        "line?\n");
        return 1;
    }

    /* Two segments: one that is read and executed, one that is read and
     * written.  The kernel counts them separately, which is why they are not
     * one. */
    unsigned long text_start = start[OUT_TEXT];
    unsigned long text_end   = start[OUT_RODATA] + length[OUT_RODATA];
    unsigned long data_start = start[OUT_DATA];
    unsigned long data_end   = start[OUT_BSS] + length[OUT_BSS];
    unsigned long data_file_size = (start[OUT_DATA] + length[OUT_DATA]) -
                                   data_start;

    int header_size = 64 + 2 * 56;
    unsigned long text_offset = align_up((unsigned long)header_size, PAGE_SIZE);
    unsigned long data_offset = text_offset + (text_end - text_start);

    data_offset = align_up(data_offset, PAGE_SIZE);

    buffer_t file;
    buf_init(&file);

    buf_byte(&file, 0x7F);
    buf_put(&file, "ELF", 3);
    buf_byte(&file, 2);
    buf_byte(&file, 1);
    buf_byte(&file, 1);
    buf_zero(&file, 9);
    buf_u16(&file, 2);                       /* ET_EXEC   */
    buf_u16(&file, 62);                      /* EM_X86_64 */
    buf_u32(&file, 1);
    buf_u64(&file, entry->address);
    buf_u64(&file, 64);                      /* e_phoff       */
    buf_u64(&file, 0);                       /* no sections   */
    buf_u32(&file, 0);
    buf_u16(&file, 64);
    buf_u16(&file, 56);                      /* e_phentsize   */
    buf_u16(&file, 2);                       /* e_phnum       */
    buf_u16(&file, 0);
    buf_u16(&file, 0);
    buf_u16(&file, 0);

    /* PT_LOAD, read and execute. */
    buf_u32(&file, 1);
    buf_u32(&file, 5);                       /* PF_R | PF_X */
    buf_u64(&file, text_offset);
    buf_u64(&file, text_start);
    buf_u64(&file, text_start);
    buf_u64(&file, text_end - text_start);
    buf_u64(&file, text_end - text_start);
    buf_u64(&file, PAGE_SIZE);

    /* PT_LOAD, read and write.  memsz is larger than filesz by the size of
     * .bss, which is how the loader knows to zero it. */
    buf_u32(&file, 1);
    buf_u32(&file, 6);                       /* PF_R | PF_W */
    buf_u64(&file, data_offset);
    buf_u64(&file, data_start);
    buf_u64(&file, data_start);
    buf_u64(&file, data_file_size);
    buf_u64(&file, data_end - data_start);
    buf_u64(&file, PAGE_SIZE);

    while ((unsigned long)file.len < text_offset)
        buf_byte(&file, 0);

    buf_put(&file, out[OUT_TEXT].data, out[OUT_TEXT].len);
    while ((unsigned long)file.len < text_offset +
                                     (start[OUT_RODATA] - text_start))
        buf_byte(&file, 0);
    buf_put(&file, out[OUT_RODATA].data, out[OUT_RODATA].len);

    while ((unsigned long)file.len < data_offset)
        buf_byte(&file, 0);
    buf_put(&file, out[OUT_DATA].data, out[OUT_DATA].len);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "wcc: cannot write %s\n", path);
        return 1;
    }

    size_t written = fwrite(file.data, 1, (size_t)file.len, f);
    fclose(f);

    if (written != (size_t)file.len) {
        fprintf(stderr, "wcc: short write on %s\n", path);
        return 1;
    }

    return 0;
}
