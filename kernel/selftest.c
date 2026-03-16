/* Boot-time self-tests. */

#include "selftest.h"
#include "kprintf.h"
#include "pit.h"
#include "pmm.h"
#include "kheap.h"
#include "paging.h"

static uint32_t failures;

static void check(bool ok, const char *what)
{
    kprintf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

void selftest_interrupts(void)
{
    kputs("\n-- interrupt self-test --\n");
    failures = 0;

    kputs("  raising a breakpoint (int $3); execution continues after it\n");
    __asm__ volatile("int $3");
    check(true, "breakpoint exception returned");

    uint32_t before = pit_ticks();
    pit_sleep(500);
    uint32_t elapsed = pit_ticks() - before;
    kprintf("  timer produced %u ticks in ~500 ms (expected ~%u)\n",
            elapsed, PIT_HZ / 2);
    check(elapsed > 0, "timer interrupts are arriving");

    if (failures)
        panic("%u interrupt self-test failure(s)", failures);
}

/* Allocate and immediately free a batch of frames, checking that the
 * accounting moves by exactly the right amount in both directions. */
static void test_frame_allocator(void)
{
    const int    count = 16;
    uint32_t     frames[16];
    uint32_t     free_before = pmm_free_bytes();

    for (int i = 0; i < count; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i])
            panic("pmm: out of memory during the self-test");
    }

    check(pmm_free_bytes() == free_before - count * PAGE_SIZE,
          "allocating 16 frames reduces free memory by exactly 64 KiB");

    /* Frames must not be handed out twice. */
    bool distinct = true;
    for (int i = 0; i < count && distinct; i++)
        for (int j = i + 1; j < count; j++)
            if (frames[i] == frames[j]) {
                distinct = false;
                break;
            }
    check(distinct, "every allocated frame is distinct");

    for (int i = 0; i < count; i++)
        pmm_free_frame(frames[i]);

    check(pmm_free_bytes() == free_before, "freeing them restores free memory");
}

/* Exercise splitting, coalescing and the payload staying intact across
 * unrelated allocations. */
static void test_kernel_heap(void)
{
    const int count = 64;
    uint8_t  *blocks[64];
    uint32_t  used_before = kheap_used_bytes();

    for (int i = 0; i < count; i++) {
        size_t size = (size_t)(8 + i * 37);
        blocks[i] = kmalloc(size);
        if (!blocks[i])
            panic("kheap: out of memory during the self-test");
        for (size_t b = 0; b < size; b++)
            blocks[i][b] = (uint8_t)(i + 1);
    }

    bool intact = true;
    for (int i = 0; i < count && intact; i++) {
        size_t size = (size_t)(8 + i * 37);
        for (size_t b = 0; b < size; b++)
            if (blocks[i][b] != (uint8_t)(i + 1)) {
                intact = false;
                break;
            }
    }
    check(intact, "64 heap blocks keep their contents while others are in use");

    /* Free in an interleaved order so coalescing has to handle both
     * neighbours, not just the easy forward case. */
    for (int i = 0; i < count; i += 2)
        kfree(blocks[i]);
    for (int i = 1; i < count; i += 2)
        kfree(blocks[i]);

    check(kheap_used_bytes() == used_before,
          "freeing every block returns the heap to its starting size");

    uint8_t *z = kzalloc(512);
    bool zeroed = true;
    for (int i = 0; i < 512; i++)
        if (z[i] != 0) {
            zeroed = false;
            break;
        }
    check(zeroed, "kzalloc returns zeroed memory");
    kfree(z);
}

/* Build a second address space, map a user page into it, and confirm the
 * mapping works and is fully reclaimed on teardown.  This is the same path
 * that loading a program will take. */
static void test_address_space(void)
{
    uint32_t free_before = pmm_free_bytes();

    addrspace_t *as = paging_new_addrspace();
    if (!as)
        panic("paging: cannot create an address space");

    addrspace_t *kernel = paging_current();
    paging_switch(as);

    bool mapped = paging_map_alloc(as, USER_BASE, PTE_WRITE | PTE_USER, true);
    check(mapped, "a user page can be mapped at 0x40000000");

    volatile uint32_t *p = (volatile uint32_t *)USER_BASE;
    bool was_zero = (*p == 0);
    *p = 0xDEADBEEF;
    check(was_zero && *p == 0xDEADBEEF,
          "the new page reads back zeroed, then holds what we write");

    check(paging_translate(as, USER_BASE) != 0,
          "the page translates to a physical frame");
    check(paging_user_can_access(as, USER_BASE, true),
          "ring 3 may write the user page");
    check(!paging_user_can_access(as, 0x00100000, false),
          "ring 3 may not touch kernel memory");

    check(paging_user_bytes(as) >= PAGE_SIZE,
          "the address space reports its resident size");

    paging_switch(kernel);
    paging_free_addrspace(as);

    check(pmm_free_bytes() == free_before,
          "tearing the address space down frees every frame it owned");
}

void selftest_memory(void)
{
    kputs("\n-- memory self-test --\n");
    failures = 0;

    kprintf("  RAM    : %s total, %s used, %s free\n",
            fmt_bytes(pmm_total_bytes()), fmt_bytes(pmm_used_bytes()),
            fmt_bytes(pmm_free_bytes()));
    kprintf("  kernel : %s reserved at boot\n",
            fmt_bytes(pmm_kernel_bytes()));
    kprintf("  heap   : %s arena at %p\n",
            fmt_bytes(kheap_total_bytes()), (void *)pmm_heap_base());

    test_frame_allocator();
    test_kernel_heap();
    test_address_space();

    if (failures)
        panic("%u memory self-test failure(s)", failures);
    kputs("-- memory self-test passed --\n");
}

void selftest_page_fault(void)
{
    kputs("\n-- page-fault demonstration --\n");
    kputs("writing through a null pointer; the handler should report it\n");

    volatile uint32_t *nowhere = (volatile uint32_t *)0;
    *nowhere = 1;

    panic("the null page was writable -- it should not have been");
}
