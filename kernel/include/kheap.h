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
#define KHEAP_SIZE (8UL * 1024UL * 1024UL)

void kheap_init(uint64_t base, uint64_t size);

/* Allocate `size` bytes, 8-byte aligned. Returns NULL when the heap is full. */
void *kmalloc(size_t size);

/* Allocate and zero. */
void *kzalloc(size_t size);

/* Free a pointer previously returned by kmalloc/kzalloc. NULL is ignored. */
void kfree(void *ptr);

uint64_t kheap_used_bytes(void);
uint64_t kheap_free_bytes(void);
uint64_t kheap_total_bytes(void);

#endif /* WOS_KHEAP_H */
