/* ELF32 program loader. */

#include "elf.h"
#include "proc.h"
#include "paging.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* Reject anything we cannot actually run, with a specific reason: a silent
 * failure here looks identical to a broken scheduler from the outside. */
static int elf_validate(const Elf32_Ehdr *eh, uint32_t size)
{
    if (size < sizeof(Elf32_Ehdr))
        return -W_ENOEXEC;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return -W_ENOEXEC;

    if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA]  != ELFDATA2LSB)
        return -W_ENOEXEC;

    if (eh->e_type != ET_EXEC || eh->e_machine != EM_386)
        return -W_ENOEXEC;

    if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
        eh->e_phentsize != sizeof(Elf32_Phdr))
        return -W_ENOEXEC;

    if (eh->e_phoff + (uint32_t)eh->e_phnum * eh->e_phentsize > size)
        return -W_ENOEXEC;

    return 0;
}

int elf_load(struct process *proc, const void *image, uint32_t size,
             uint32_t *entry_out)
{
    const Elf32_Ehdr *eh = image;

    int r = elf_validate(eh, size);
    if (r < 0)
        return r;

    const Elf32_Phdr *ph = (const Elf32_Phdr *)((const uint8_t *)image + eh->e_phoff);
    uint32_t highest = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
            continue;

        uint32_t vaddr = ph[i].p_vaddr;
        uint32_t memsz = ph[i].p_memsz;
        uint32_t filesz = ph[i].p_filesz;

        /* Keep the program out of kernel space and out of the stack region. */
        if (vaddr < USER_BASE || vaddr + memsz > USER_STACK_TOP - USER_STACK_SIZE)
            return -W_ENOEXEC;
        if (filesz > memsz || ph[i].p_offset + filesz > size)
            return -W_ENOEXEC;

        uint32_t start = ALIGN_DOWN(vaddr, PAGE_SIZE);
        uint32_t end   = ALIGN_UP(vaddr + memsz, PAGE_SIZE);

        for (uint32_t page = start; page < end; page += PAGE_SIZE) {
            if (paging_translate(proc->space, page))
                continue;      /* two segments sharing a page */

            /* Everything is mapped writable: the loader has to write here,
             * and without per-segment protection there is nothing to gain
             * from turning the write bit off afterwards. */
            if (!paging_map_alloc(proc->space, page,
                                  PTE_WRITE | PTE_USER, true))
                return -W_ENOMEM;
        }

        /* Safe to write through user addresses: the caller switched to this
         * address space before calling, and the kernel is mapped in it. */
        memcpy((void *)vaddr, (const uint8_t *)image + ph[i].p_offset, filesz);

        /* .bss is the part of memsz beyond filesz; the pages were zeroed on
         * allocation, so only a partially reused page needs clearing. */
        if (memsz > filesz)
            memset((void *)(vaddr + filesz), 0, memsz - filesz);

        if (ph[i].p_flags & PF_X)
            proc->code_bytes += end - start;
        else
            proc->data_bytes += end - start;

        if (vaddr + memsz > highest)
            highest = vaddr + memsz;
    }

    if (highest == 0)
        return -W_ENOEXEC;

    if (eh->e_entry < USER_BASE || eh->e_entry >= highest)
        return -W_ENOEXEC;

    /* The heap starts on the first page past the image. */
    proc->heap_start = ALIGN_UP(highest, PAGE_SIZE);
    proc->heap_break = proc->heap_start;

    *entry_out = eh->e_entry;
    return 0;
}
