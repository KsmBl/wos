/* Kernel heap: a first-fit free-list allocator over a fixed arena.
 *
 * The arena sits inside the identity-mapped low region, so kernel pointers are
 * valid in every address space and stay valid across a context switch.
 */
#ifndef WOS_KHEAP_H
#define WOS_KHEAP_H

#include "types.h"

/* Size of the arena carved out by pmm_init(). Large enough to hold process
 * control blocks, kernel stacks, file buffers and a whole executable image. */
#define KHEAP_SIZE (4u * 1024u * 1024u)

void kheap_init(uint32_t base, uint32_t size);

/* Allocate `size` bytes, 8-byte aligned. Returns NULL when the heap is full. */
void *kmalloc(size_t size);

/* Allocate and zero. */
void *kzalloc(size_t size);

/* Free a pointer previously returned by kmalloc/kzalloc. NULL is ignored. */
void kfree(void *ptr);

uint32_t kheap_used_bytes(void);
uint32_t kheap_free_bytes(void);
uint32_t kheap_total_bytes(void);

#endif /* WOS_KHEAP_H */
