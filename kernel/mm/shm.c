/* Shared memory objects.  See shm.h for what they are for. */

#include "shm.h"
#include "proc.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"

struct shm {
    int       refs;
    uint32_t  pages;
    uint64_t *frame;         /* physical address of each page */
};

/* ------------------------------------------------------------------ *
 *  The object
 * ------------------------------------------------------------------ */

shm_t *shm_create(uint32_t bytes)
{
    if (bytes == 0 || bytes > SHM_MAX_BYTES)
        return NULL;

    uint32_t pages = (uint32_t)(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE);

    shm_t *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;

    s->frame = kzalloc((size_t)pages * sizeof(uint64_t));
    if (!s->frame) {
        kfree(s);
        return NULL;
    }

    /* Allocated up front rather than on first touch.  A client that has been
     * told its pool exists will draw into all of it, and finding out halfway
     * through that the machine is full would leave it holding a buffer it
     * cannot use and no way to say so. */
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t f = pmm_alloc_frame();
        if (!f) {
            for (uint32_t j = 0; j < i; j++)
                pmm_free_frame(s->frame[j]);
            kfree(s->frame);
            kfree(s);
            return NULL;
        }

        /* Zeroed, because these frames are about to be visible to another
         * process: whatever the last owner left in them is not this one's to
         * read.  They are below the identity map's ceiling or reachable
         * through it, so writing them directly is safe here. */
        if (f < LOW_MEMORY_LIMIT)
            memset((void *)f, 0, PAGE_SIZE);

        s->frame[i] = f;
    }

    s->pages = pages;
    s->refs  = 1;
    return s;
}

uint32_t shm_bytes(const shm_t *s)
{
    return s ? s->pages * (uint32_t)PAGE_SIZE : 0;
}

void shm_ref(shm_t *s)
{
    if (s)
        s->refs++;
}

void shm_unref(shm_t *s)
{
    if (!s || --s->refs > 0)
        return;

    for (uint32_t i = 0; i < s->pages; i++)
        pmm_free_frame(s->frame[i]);

    kfree(s->frame);
    kfree(s);
}

/* ------------------------------------------------------------------ *
 *  Mapping
 * ------------------------------------------------------------------ */

/* The lowest address in the mapping window with `pages` free pages above it.
 *
 * Restarting the sweep whenever a mapping is stepped over is what makes this
 * correct without keeping the table sorted: the table is short, and a
 * compositor maps a pool once and keeps it. */
static uint64_t find_region(struct process *p, uint32_t pages)
{
    uint64_t want = (uint64_t)pages * PAGE_SIZE;
    uint64_t base = USER_MMAP_BASE;
    bool     moved;

    do {
        moved = false;

        for (int i = 0; i < SHM_MAX_MAPPINGS; i++) {
            const shm_mapping_t *m = &p->maps[i];
            if (!m->shm)
                continue;

            uint64_t end = m->base + (uint64_t)m->pages * PAGE_SIZE;
            if (base < end && m->base < base + want) {
                base  = end;
                moved = true;
            }
        }
    } while (moved);

    if (base + want > USER_MMAP_TOP || base + want < base)
        return 0;
    return base;
}

int shm_map(struct process *p, shm_t *s, uint64_t *addr_out)
{
    if (!p || !s)
        return -W_EINVAL;

    shm_mapping_t *slot = NULL;
    for (int i = 0; i < SHM_MAX_MAPPINGS; i++)
        if (!p->maps[i].shm) {
            slot = &p->maps[i];
            break;
        }
    if (!slot)
        return -W_EMFILE;

    uint64_t base = find_region(p, s->pages);
    if (!base)
        return -W_ENOMEM;

    for (uint32_t i = 0; i < s->pages; i++) {
        if (paging_map(p->space, base + (uint64_t)i * PAGE_SIZE, s->frame[i],
                       PTE_WRITE | PTE_USER))
            continue;

        /* Undo the part that went in.  paging_map does not own these frames,
         * so the pages come out without them being freed. */
        for (uint32_t j = 0; j < i; j++)
            paging_unmap_keep(p->space, base + (uint64_t)j * PAGE_SIZE);
        return -W_ENOMEM;
    }

    shm_ref(s);
    slot->shm   = s;
    slot->base  = base;
    slot->pages = s->pages;

    *addr_out = base;
    return 0;
}

int shm_unmap(struct process *p, uint64_t addr)
{
    for (int i = 0; i < SHM_MAX_MAPPINGS; i++) {
        shm_mapping_t *m = &p->maps[i];

        if (!m->shm || m->base != addr)
            continue;

        for (uint32_t j = 0; j < m->pages; j++)
            paging_unmap_keep(p->space, m->base + (uint64_t)j * PAGE_SIZE);

        shm_unref(m->shm);
        m->shm = NULL;
        return 0;
    }

    return -W_EINVAL;
}

void shm_unmap_all(struct process *p)
{
    for (int i = 0; i < SHM_MAX_MAPPINGS; i++)
        if (p->maps[i].shm)
            shm_unmap(p, p->maps[i].base);
}
