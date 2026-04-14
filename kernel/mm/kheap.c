/* Kernel heap: first-fit free list with splitting and coalescing.
 *
 * Blocks form one doubly linked list in address order, which makes coalescing
 * a neighbour check rather than a search.
 */

#include "kheap.h"
#include "kprintf.h"

#define KHEAP_MAGIC 0x57484541u   /* "WHEA" -- catches overruns and bad frees */
#define MIN_SPLIT   32            /* don't split off slivers smaller than this */

struct block {
    uint32_t      magic;
    uint64_t      size;      /* payload bytes, excluding this header */
    bool          free;
    struct block *next;
    struct block *prev;
};

static struct block *heap_head;
static uint64_t      heap_total;
static uint64_t      heap_used;   /* payload bytes handed out */

void kheap_init(uint64_t base, uint64_t size)
{
    heap_head  = (struct block *)base;
    heap_total = size;
    heap_used  = 0;

    heap_head->magic = KHEAP_MAGIC;
    heap_head->size  = size - sizeof(struct block);
    heap_head->free  = true;
    heap_head->next  = NULL;
    heap_head->prev  = NULL;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = ALIGN_UP(size, 8);

    for (struct block *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        /* Split the block if the remainder is worth tracking. */
        if (b->size >= size + sizeof(struct block) + MIN_SPLIT) {
            struct block *rest =
                (struct block *)((uint8_t *)(b + 1) + size);

            rest->magic = KHEAP_MAGIC;
            rest->size  = b->size - size - sizeof(struct block);
            rest->free  = true;
            rest->next  = b->next;
            rest->prev  = b;

            if (b->next)
                b->next->prev = rest;
            b->next = rest;
            b->size = size;
        }

        b->free = false;
        heap_used += b->size;
        return (void *)(b + 1);
    }

    return NULL;    /* out of heap */
}

void *kzalloc(size_t size)
{
    uint8_t *p = kmalloc(size);
    if (p) {
        /* The block header records the real (possibly rounded up) size. */
        struct block *b = (struct block *)p - 1;
        for (uint64_t i = 0; i < b->size; i++)
            p[i] = 0;
    }
    return p;
}

/* Merge `b` with the following block if both are free. */
static void coalesce_with_next(struct block *b)
{
    struct block *n = b->next;

    if (!n || !n->free || !b->free)
        return;

    b->size += n->size + sizeof(struct block);
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)ptr - 1;

    if (b->magic != KHEAP_MAGIC)
        panic("kfree: bad or corrupted block at %p", ptr);
    if (b->free)
        panic("kfree: double free of %p", ptr);

    b->free = true;
    heap_used -= b->size;

    coalesce_with_next(b);
    if (b->prev)
        coalesce_with_next(b->prev);
}

uint64_t kheap_used_bytes(void)  { return heap_used; }
uint64_t kheap_total_bytes(void) { return heap_total; }
uint64_t kheap_free_bytes(void)  { return heap_total - heap_used; }
