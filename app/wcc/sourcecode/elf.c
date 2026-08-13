/* Writing an ELF64 relocatable object.
 *
 * The layout is the plain one every assembler produces: a header, the section
 * contents one after another, then the section header table.  What goes in it
 * is four sections and their relocations, a symbol table and two string
 * tables -- nothing else, because nothing else is read by the linker in link.c
 * or by GNU ld, which is the other thing these objects are checked against.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcc.h"

/* ELF constants, spelled out rather than included from anywhere: this file is
 * the one place that needs them, and the kernel's own elf.h describes only the
 * executables it loads. */
#define ET_REL      1
#define EM_X86_64   62
#define EV_CURRENT  1

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

#define STB_LOCAL  0
#define STB_GLOBAL 1

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3

#define SHN_UNDEF 0

symbol_t *unit_symbol(unit_t *unit, const char *name)
{
    for (symbol_t *s = unit->symbols; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;

    symbol_t *s = wcc_alloc(sizeof(*s));
    s->name = name;

    if (unit->last_symbol)
        unit->last_symbol = unit->last_symbol->next = s;
    else
        unit->symbols = unit->last_symbol = s;

    return s;
}

void unit_reloc(unit_t *unit, section_id_t section, int offset,
                const char *symbol, int type, long addend)
{
    reloc_t *r = wcc_alloc(sizeof(*r));

    r->section = section;
    r->offset  = offset;
    r->symbol  = symbol;
    r->type    = type;
    r->addend  = addend;

    if (unit->last_reloc)
        unit->last_reloc = unit->last_reloc->next = r;
    else
        unit->relocs = unit->last_reloc = r;
}

/* The section a relocation lives in decides which .rela.* it goes to. */
static int has_relocs(const unit_t *unit, section_id_t section)
{
    for (reloc_t *r = unit->relocs; r; r = r->next)
        if (r->section == section)
            return 1;
    return 0;
}

int write_object(const unit_t *unit, const char *path)
{
    static const char *const section_names[SEC_COUNT] = {
        ".text", ".rodata", ".data", ".bss",
    };

    buffer_t shstr, str, symtab, out;
    buf_init(&shstr);
    buf_init(&str);
    buf_init(&symtab);
    buf_init(&out);

    buf_byte(&shstr, 0);
    buf_byte(&str, 0);

    /* Section numbering: 0 is the null section, then the four contents, then
     * the relocation sections that exist, then .symtab, .strtab, .shstrtab. */
    int index_of[SEC_COUNT];
    int rela_of[SEC_COUNT];
    int next_index = 1;

    for (int i = 0; i < SEC_COUNT; i++) {
        index_of[i] = next_index++;
        rela_of[i]  = has_relocs(unit, (section_id_t)i) ? next_index++ : 0;
    }

    int symtab_index   = next_index++;
    int strtab_index   = next_index++;
    int shstrtab_index = next_index++;
    int section_count  = next_index;

    /* The symbol table: the local symbols first, which ELF requires, and
     * sh_info says where the global ones start. */
    buf_zero(&symtab, 24);                       /* the null symbol */
    int local_count = 1;

    /* A section symbol per section, so a relocation can name a section
     * rather than a symbol -- which is what GNU tools expect to see. */
    for (int i = 0; i < SEC_COUNT; i++) {
        local_count++;
        buf_u32(&symtab, 0);                     /* no name          */
        buf_byte(&symtab, STT_SECTION);
        buf_byte(&symtab, 0);
        buf_u16(&symtab, (unsigned)index_of[i]);
        buf_u64(&symtab, 0);
        buf_u64(&symtab, 0);
    }

    for (symbol_t *s = unit->symbols; s; s = s->next) {
        if (s->is_global)
            continue;

        s->elf_index = local_count++;

        buf_u32(&symtab, (unsigned)buf_string(&str, s->name));
        buf_byte(&symtab, (unsigned char)((STB_LOCAL << 4) |
                 (s->is_function ? STT_FUNC : STT_OBJECT)));
        buf_byte(&symtab, 0);
        buf_u16(&symtab, s->is_defined ? (unsigned)index_of[s->section]
                                       : SHN_UNDEF);
        buf_u64(&symtab, (unsigned long)s->value);
        buf_u64(&symtab, (unsigned long)s->size);
    }

    int first_global = local_count;
    int symbol_count = local_count;

    for (symbol_t *s = unit->symbols; s; s = s->next) {
        if (!s->is_global)
            continue;

        s->elf_index = symbol_count++;

        buf_u32(&symtab, (unsigned)buf_string(&str, s->name));
        buf_byte(&symtab, (unsigned char)((STB_GLOBAL << 4) |
                 (s->is_function ? STT_FUNC : STT_OBJECT)));
        buf_byte(&symtab, 0);
        buf_u16(&symtab, s->is_defined ? (unsigned)index_of[s->section]
                                       : SHN_UNDEF);
        buf_u64(&symtab, (unsigned long)s->value);
        buf_u64(&symtab, (unsigned long)s->size);
    }

    /* The relocation sections. */
    buffer_t rela[SEC_COUNT];
    for (int i = 0; i < SEC_COUNT; i++)
        buf_init(&rela[i]);

    for (reloc_t *r = unit->relocs; r; r = r->next) {
        symbol_t *s = NULL;

        for (symbol_t *q = unit->symbols; q; q = q->next)
            if (strcmp(q->name, r->symbol) == 0) {
                s = q;
                break;
            }

        int index;
        long addend = r->addend;

        if (s) {
            index = s->elf_index;
        } else {
            /* A name with no symbol should not happen; making it a section
             * relocation would silently point at the wrong thing. */
            fatal("internal: relocation against unknown symbol %s", r->symbol);
            index = 0;
        }

        buf_u64(&rela[r->section], (unsigned long)r->offset);
        buf_u64(&rela[r->section],
                ((unsigned long)index << 32) | (unsigned)r->type);
        buf_u64(&rela[r->section], (unsigned long)addend);
    }

    /* Section names, in the order the headers will be written. */
    int name_offset[SEC_COUNT], rela_name_offset[SEC_COUNT];
    for (int i = 0; i < SEC_COUNT; i++) {
        name_offset[i] = buf_string(&shstr, section_names[i]);

        if (rela_of[i]) {
            char rela_name[32];
            sprintf(rela_name, ".rela%s", section_names[i]);
            rela_name_offset[i] = buf_string(&shstr, rela_name);
        } else {
            rela_name_offset[i] = 0;
        }
    }

    int symtab_name   = buf_string(&shstr, ".symtab");
    int strtab_name   = buf_string(&shstr, ".strtab");
    int shstrtab_name = buf_string(&shstr, ".shstrtab");

    /* Now the file itself: the header, then every section's contents, then
     * the section header table. */
    int offset = 64;                              /* past the ELF header */

    int content_offset[SEC_COUNT], rela_offset[SEC_COUNT];

    for (int i = 0; i < SEC_COUNT; i++) {
        offset = (offset + 15) / 16 * 16;
        content_offset[i] = offset;

        if (i != SEC_BSS)
            offset += unit->section[i].len;

        if (rela_of[i]) {
            offset = (offset + 7) / 8 * 8;
            rela_offset[i] = offset;
            offset += rela[i].len;
        } else {
            rela_offset[i] = 0;
        }
    }

    offset = (offset + 7) / 8 * 8;
    int symtab_offset = offset;
    offset += symtab.len;

    int strtab_offset = offset;
    offset += str.len;

    int shstrtab_offset = offset;
    offset += shstr.len;

    offset = (offset + 7) / 8 * 8;
    int sh_offset = offset;

    /* --- the ELF header --- */
    buf_byte(&out, 0x7F);
    buf_put(&out, "ELF", 3);
    buf_byte(&out, 2);            /* 64-bit          */
    buf_byte(&out, 1);            /* little endian   */
    buf_byte(&out, EV_CURRENT);
    buf_zero(&out, 9);
    buf_u16(&out, ET_REL);
    buf_u16(&out, EM_X86_64);
    buf_u32(&out, EV_CURRENT);
    buf_u64(&out, 0);             /* no entry point in an object */
    buf_u64(&out, 0);             /* no program headers          */
    buf_u64(&out, (unsigned long)sh_offset);
    buf_u32(&out, 0);
    buf_u16(&out, 64);            /* e_ehsize    */
    buf_u16(&out, 0);
    buf_u16(&out, 0);
    buf_u16(&out, 64);            /* e_shentsize */
    buf_u16(&out, (unsigned)section_count);
    buf_u16(&out, (unsigned)shstrtab_index);

    /* --- the contents --- */
    for (int i = 0; i < SEC_COUNT; i++) {
        while (out.len < content_offset[i])
            buf_byte(&out, 0);

        if (i != SEC_BSS)
            buf_put(&out, unit->section[i].data, unit->section[i].len);

        if (rela_of[i]) {
            while (out.len < rela_offset[i])
                buf_byte(&out, 0);
            buf_put(&out, rela[i].data, rela[i].len);
        }
    }

    while (out.len < symtab_offset)
        buf_byte(&out, 0);
    buf_put(&out, symtab.data, symtab.len);
    buf_put(&out, str.data, str.len);
    buf_put(&out, shstr.data, shstr.len);

    while (out.len < sh_offset)
        buf_byte(&out, 0);

    /* --- the section headers --- */
    buf_zero(&out, 64);                            /* the null section */

    for (int i = 0; i < SEC_COUNT; i++) {
        unsigned flags = SHF_ALLOC;

        if (i == SEC_TEXT)
            flags |= SHF_EXECINSTR;
        else if (i == SEC_DATA || i == SEC_BSS)
            flags |= SHF_WRITE;

        buf_u32(&out, (unsigned)name_offset[i]);
        buf_u32(&out, i == SEC_BSS ? SHT_NOBITS : SHT_PROGBITS);
        buf_u64(&out, flags);
        buf_u64(&out, 0);                          /* sh_addr    */
        buf_u64(&out, (unsigned long)content_offset[i]);
        buf_u64(&out, (unsigned long)(i == SEC_BSS ? unit->section[i].len
                                                   : unit->section[i].len));
        buf_u32(&out, 0);                          /* sh_link    */
        buf_u32(&out, 0);                          /* sh_info    */
        buf_u64(&out, 16);                         /* sh_addralign */
        buf_u64(&out, 0);                          /* sh_entsize */

        if (rela_of[i]) {
            buf_u32(&out, (unsigned)rela_name_offset[i]);
            buf_u32(&out, SHT_RELA);
            buf_u64(&out, 0);
            buf_u64(&out, 0);
            buf_u64(&out, (unsigned long)rela_offset[i]);
            buf_u64(&out, (unsigned long)rela[i].len);
            buf_u32(&out, (unsigned)symtab_index); /* sh_link: the symbols  */
            buf_u32(&out, (unsigned)index_of[i]);  /* sh_info: what it fixes */
            buf_u64(&out, 8);
            buf_u64(&out, 24);                     /* one entry is 24 bytes */
        }
    }

    buf_u32(&out, (unsigned)symtab_name);
    buf_u32(&out, SHT_SYMTAB);
    buf_u64(&out, 0);
    buf_u64(&out, 0);
    buf_u64(&out, (unsigned long)symtab_offset);
    buf_u64(&out, (unsigned long)symtab.len);
    buf_u32(&out, (unsigned)strtab_index);         /* sh_link: the strings  */
    buf_u32(&out, (unsigned)first_global);         /* sh_info: first global */
    buf_u64(&out, 8);
    buf_u64(&out, 24);

    buf_u32(&out, (unsigned)strtab_name);
    buf_u32(&out, SHT_STRTAB);
    buf_u64(&out, 0);
    buf_u64(&out, 0);
    buf_u64(&out, (unsigned long)strtab_offset);
    buf_u64(&out, (unsigned long)str.len);
    buf_u32(&out, 0);
    buf_u32(&out, 0);
    buf_u64(&out, 1);
    buf_u64(&out, 0);

    buf_u32(&out, (unsigned)shstrtab_name);
    buf_u32(&out, SHT_STRTAB);
    buf_u64(&out, 0);
    buf_u64(&out, 0);
    buf_u64(&out, (unsigned long)shstrtab_offset);
    buf_u64(&out, (unsigned long)shstr.len);
    buf_u32(&out, 0);
    buf_u32(&out, 0);
    buf_u64(&out, 1);
    buf_u64(&out, 0);

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    size_t written = fwrite(out.data, 1, (size_t)out.len, f);
    fclose(f);

    return written == (size_t)out.len ? 0 : -1;
}
