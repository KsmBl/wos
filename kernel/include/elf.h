/* Minimal ELF32 definitions and the program loader.
 *
 * WOS only ever loads statically linked ET_EXEC binaries for i386, so this
 * covers the program headers and nothing else -- no sections, no relocations,
 * no dynamic linking.
 */
#ifndef WOS_ELF_H
#define WOS_ELF_H

#include "types.h"

#define EI_NIDENT 16

/* e_ident indices and the values we require. */
#define EI_CLASS 4
#define EI_DATA  5
#define ELFCLASS32  1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define EM_386  3

#define PT_LOAD 1

/* p_flags bits. */
#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

struct process;

/* Load an in-memory ELF image into the *current* address space, which must
 * belong to `proc`.  Fills in the process's code/data byte counts and heap
 * start, and stores the entry point in `entry_out`.
 * Returns 0 or a negative W_E* code. */
int elf_load(struct process *proc, const void *image, uint32_t size,
             uint32_t *entry_out);

#endif /* WOS_ELF_H */
