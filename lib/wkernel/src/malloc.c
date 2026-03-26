/* Heap allocator for applications: a first-fit free list over wsbrk().
 *
 * Blocks are kept in one address-ordered list so that coalescing on free is a
 * neighbour check rather than a search.  The heap only ever grows, which is
 * fine for programs that run and exit; the memory goes back to the system
 * when the process does.
 */

#include "wkernel.h"

#define ALLOC_MAGIC 0x574D454Du   /* "WMEM" -- catches bad or double frees */
#define MIN_SPLIT   32            /* don't split off slivers below this    */
#define GROW_CHUNK  (16u * 1024u) /* ask the kernel for memory in batches  */

struct block {
    unsigned int  magic;
    wsize_t       size;      /* payload bytes, excluding this header */
    int           free;
    struct block *next;
    struct block *prev;
};

static struct block *heap_head;
static struct block *heap_tail;

/* Extend the heap by at least `need` payload bytes and return the new block. */
static struct block *heap_grow(wsize_t need)
{
    wsize_t want = need + sizeof(struct block);
    if (want < GROW_CHUNK)
        want = GROW_CHUNK;

    void *base = wsbrk((int)want);
    if (base == (void *)-1)
        return NULL;

    struct block *b = base;
    b->magic = ALLOC_MAGIC;
    b->size  = want - sizeof(struct block);
    b->free  = 1;
    b->next  = NULL;
    b->prev  = heap_tail;

    if (heap_tail)
        heap_tail->next = b;
    else
        heap_head = b;
    heap_tail = b;

    return b;
}

/* Merge b with the block after it if both are free and adjacent. */
static void coalesce_with_next(struct block *b)
{
    struct block *n = b->next;

    if (!n || !b->free || !n->free)
        return;

    /* Only merge blocks that really touch; wsbrk may have handed out
     * non-contiguous regions. */
    if ((unsigned char *)(b + 1) + b->size != (unsigned char *)n)
        return;

    b->size += n->size + sizeof(struct block);
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    else
        heap_tail = b;
}

void *malloc(wsize_t size)
{
    if (size == 0)
        return NULL;

    size = (size + 7u) & ~7u;      /* keep every payload 8-byte aligned */

    struct block *b = heap_head;
    while (b && !(b->free && b->size >= size))
        b = b->next;

    if (!b) {
        b = heap_grow(size);
        if (!b)
            return NULL;
    }

    if (b->size >= size + sizeof(struct block) + MIN_SPLIT) {
        struct block *rest = (struct block *)((unsigned char *)(b + 1) + size);

        rest->magic = ALLOC_MAGIC;
        rest->size  = b->size - size - sizeof(struct block);
        rest->free  = 1;
        rest->next  = b->next;
        rest->prev  = b;

        if (b->next)
            b->next->prev = rest;
        else
            heap_tail = rest;

        b->next = rest;
        b->size = size;
    }

    b->free = 0;
    return b + 1;
}

void free(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)ptr - 1;

    /* A corrupted header means the caller passed something that did not come
     * from malloc, or overran an earlier block. Do nothing rather than
     * scribble further. */
    if (b->magic != ALLOC_MAGIC || b->free)
        return;

    b->free = 1;
    coalesce_with_next(b);
    if (b->prev)
        coalesce_with_next(b->prev);
}

void *calloc(wsize_t count, wsize_t size)
{
    wsize_t total = count * size;

    /* Reject a multiplication that wrapped, which would under-allocate. */
    if (count != 0 && total / count != size)
        return NULL;

    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, wsize_t size)
{
    if (!ptr)
        return malloc(size);

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct block *b = (struct block *)ptr - 1;
    if (b->magic != ALLOC_MAGIC)
        return NULL;

    if (b->size >= size)
        return ptr;

    void *fresh = malloc(size);
    if (!fresh)
        return NULL;            /* the original block is still valid */

    memcpy(fresh, ptr, b->size);
    free(ptr);
    return fresh;
}
