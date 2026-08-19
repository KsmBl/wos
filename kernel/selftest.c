/* Boot-time self-tests.  Built out entirely by `make SELFTEST=0`; see
 * selftest.h, which then declares them away. */

#include "selftest.h"
#include "spinlock.h"

#ifndef WOS_NO_SELFTEST

#include "kprintf.h"
#include "pit.h"
#include "pmm.h"
#include "kheap.h"
#include "paging.h"
#include "ata.h"
#include "wfs_kernel.h"
#include "ramfs.h"
#include "rtc.h"
#include "proc.h"
#include "sched.h"
#include "socket.h"
#include "pipe.h"
#include "string.h"
#include "crypto.h"
#include "net.h"
#include "wabi.h"

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
    const int count = 16;

    /* 64-bit, both of them: a frame on a machine with real memory in it can
     * sit above the 4 GiB line, and so can the free figure.  Truncating either
     * makes this test fail on the machines it most wants to pass on. */
    uint64_t frames[16];
    uint64_t free_before = pmm_free_bytes();

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
    uint64_t free_before = pmm_free_bytes();

    addrspace_t *as = paging_new_addrspace();
    if (!as)
        panic("paging: cannot create an address space");

    /* Interrupts off for the duration.
     *
     * What follows loads a second address space on a thread that has no
     * process, and then reads through it.  A timer tick landing in the middle
     * would run the scheduler, which puts every thread into the address space
     * belonging to its process -- the kernel's, for this one -- and the
     * mapping under test would be gone from under the very next instruction.
     *
     * That is the scheduler being right rather than wrong.  A kernel thread
     * running on a borrowed address space is exactly what it should not be
     * left doing, and the only reason this one may is that nothing is going to
     * take it away in between. */
    bool interrupts_were_on = interrupts_off();

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

    interrupts_restore(interrupts_were_on);

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

/* Read /home/boots.txt, bump the number in it and write it back.  If the disk
 * is genuinely persistent this climbs by one on every boot. */
static void test_boot_counter(void)
{
    const char *path = "/boots.txt";
    char        buf[32];
    uint32_t    count = 0;
    uint32_t    ino;

    int r = wfs_lookup(path, &ino);
    if (r == 0) {
        int n = wfs_read(ino, 0, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
                count = count * 10 + (uint32_t)(buf[i] - '0');
        }
    } else if (r == -W_ENOENT) {
        r = wfs_create(path, WFS_TYPE_FILE, &ino);
        if (r < 0) {
            check(false, "creating the boot counter file");
            return;
        }
    } else {
        check(false, "reading the boot counter file");
        return;
    }

    count++;

    /* Render the number by hand; the kernel has no sprintf. */
    char     text[32];
    uint32_t len = 0;
    char     digits[11];
    uint32_t d = 0;
    uint32_t v = count;

    if (v == 0)
        digits[d++] = '0';
    while (v) {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (d)
        text[len++] = digits[--d];
    text[len++] = '\n';

    r = wfs_truncate(ino);
    if (r == 0)
        r = wfs_write(ino, 0, text, len);

    check(r == (int)len, "the boot counter file can be rewritten");
    kprintf("  this is boot number %u since the image was built\n", count);
}

static void test_filesystem_paths(void)
{
    uint32_t ino;

    check(wfs_lookup("/", &ino) == 0 && ino == WFS_ROOT_INO,
          "the root directory resolves to inode 1");

    int r = wfs_lookup("/home/root/readme.txt", &ino);
    check(r == 0, "a nested path resolves");

    if (r == 0) {
        struct wfs_inode in;
        check(wfs_read_inode(ino, &in) == 0 && in.type == WFS_TYPE_FILE
                  && in.size > 0,
              "the file's inode reports a non-zero size");

        char head[16];
        int  n = wfs_read(ino, 0, head, sizeof(head) - 1);
        if (n > 0)
            head[n] = '\0';
        check(n > 0 && strncmp(head, "Welcome to WOS.", 15) == 0,
              "reading the file returns its contents");

        /* Reading from an offset must not restart at the beginning. */
        char mid[8];
        n = wfs_read(ino, 8, mid, 4);
        check(n == 4 && strncmp(mid, "to W", 4) == 0,
              "reading from an offset returns the right bytes");
    }

    check(wfs_lookup("/nope", &ino) == -W_ENOENT,
          "a missing path reports ENOENT");
    check(wfs_lookup("/home/root/readme.txt/x", &ino) == -W_ENOTDIR,
          "walking through a file reports ENOTDIR");
}

static void test_filesystem_write(void)
{
    const char *path = "/selftest.tmp";
    wdiskinfo_t before, after;
    uint32_t    ino;

    wfs_statfs(&before);

    /* A previous boot may have left this behind. */
    wfs_unlink(path);

    int r = wfs_create(path, WFS_TYPE_FILE, &ino);
    check(r == 0, "a new file can be created");
    if (r < 0)
        return;

    /* Deliberately larger than one block, so this covers the multi-block
     * path and the indirect block rather than just the easy case. */
    const uint32_t size = 40 * 1024;
    uint8_t *data = kmalloc(size);
    if (!data) {
        check(false, "allocating the write buffer");
        return;
    }
    for (uint32_t i = 0; i < size; i++)
        data[i] = (uint8_t)(i * 7 + (i >> 8));

    r = wfs_write(ino, 0, data, size);
    check(r == (int)size, "a 40 KiB write spanning many blocks succeeds");

    uint8_t *back = kmalloc(size);
    if (back) {
        memset(back, 0, size);
        r = wfs_read(ino, 0, back, size);
        check(r == (int)size && memcmp(data, back, size) == 0,
              "reading it back returns exactly what was written");
        kfree(back);
    }

    wfs_statfs(&after);
    check(after.free_blocks < before.free_blocks,
          "the free block count dropped after writing");

    kfree(data);

    /* The time the write left behind.  Comparing it against the clock rather
     * than against a time taken before the write means this does not depend on
     * how long the write took; a second either side is the resolution of the
     * clock itself.
     *
     * Then the same file is made deliberately ancient and written again, which
     * is the part that matters to make: a write has to move the time, and
     * waiting a second at boot to prove it is not a trade worth making. */
    struct wfs_inode stamped;
    uint32_t now = rtc_epoch();

    if (wfs_read_inode(ino, &stamped) == 0)
        check(stamped.mtime != 0 && stamped.mtime + 2 >= now &&
              stamped.mtime <= now + 2,
              "a written file carries the time from the clock");

    wfs_utime(ino, 1);
    wfs_write(ino, 0, "x", 1);

    if (wfs_read_inode(ino, &stamped) == 0)
        check(stamped.mtime > 1, "writing it again moves the time forward");

    r = wfs_unlink(path);
    check(r == 0, "the file can be deleted");

    wfs_statfs(&after);
    check(after.free_blocks == before.free_blocks,
          "deleting it returns every block to the free list");
}

static void test_filesystem_dirs(void)
{
    wdirent_t ent;
    uint32_t  ino;
    bool      found_home = false;

    if (wfs_lookup("/", &ino) != 0)
        return;

    for (uint32_t cursor = 0; wfs_readdir(ino, &cursor, &ent) == 1; )
        if (strcmp(ent.name, "home") == 0 && ent.type == W_FT_DIR)
            found_home = true;

    check(found_home, "readdir lists /home as a directory");

    check(wfs_create("/tmpdir", WFS_TYPE_DIR, &ino) == 0,
          "a directory can be created");
    check(wfs_lookup("/tmpdir/.", &ino) == 0,
          "the new directory contains a working \".\" entry");
    check(wfs_create("/tmpdir/inner.txt", WFS_TYPE_FILE, &ino) == 0,
          "a file can be created inside it");
    check(wfs_unlink("/tmpdir") == -W_ENOTEMPTY,
          "a non-empty directory refuses to be removed");
    check(wfs_unlink("/tmpdir/inner.txt") == 0 && wfs_unlink("/tmpdir") == 0,
          "emptying it first lets it be removed");
}

/* A file past the old ceiling, which is the only way to reach the
 * double-indirect block.
 *
 * WFS used to hold 267 KiB in one file: eleven direct blocks and one indirect
 * one.  That was chosen when the largest thing stored here was the kernel,
 * and it stopped being enough -- the kernel outgrew it, and the wireless
 * adapter's firmware is 1.45 MiB.  The block of blocks of pointers that
 * lifted the limit to 64 MiB is reached only by a file large enough to need
 * it, so nothing else in this suite touches that code at all.
 *
 * The free-space check at the end is the point as much as the readback is: a
 * pointer block that is allocated and never freed leaks silently, a few
 * kilobytes at a time, and the only symptom is a disk that slowly fills. */
static void test_filesystem_large(void)
{
    const char    *path = "/selftest-large.tmp";
    const uint32_t chunk = 32 * 1024;
    const uint32_t size  = 384 * 1024;      /* well past the old 267 KiB */
    wdiskinfo_t    before, after;
    uint32_t       ino;

    wfs_statfs(&before);
    wfs_unlink(path);                       /* a previous boot may have left it */

    int r = wfs_create(path, WFS_TYPE_FILE, &ino);

    check(r == 0, "a file larger than the old limit can be created");
    if (r < 0)
        return;

    uint8_t *buf = kmalloc(chunk);

    if (!buf) {
        check(false, "allocating the buffer for the large-file test");
        return;
    }

    /* The pattern depends on the absolute offset, so a block written to the
     * wrong place -- the failure a wrong index calculation produces -- shows
     * up as a mismatch rather than as plausible-looking data. */
    bool wrote = true;

    for (uint32_t at = 0; at < size; at += chunk) {
        for (uint32_t i = 0; i < chunk; i++)
            buf[i] = (uint8_t)((at + i) * 31 + ((at + i) >> 11));

        if (wfs_write(ino, at, buf, chunk) != (int)chunk)
            wrote = false;
    }
    check(wrote, "384 KiB writes through the double-indirect block");

    uint32_t stored = 0;
    struct wfs_inode in;

    if (wfs_read_inode(ino, &in) == 0)
        stored = in.size;
    check(stored == size, "and the file is as long as what was written");

    bool matched = true;

    for (uint32_t at = 0; at < size && matched; at += chunk) {
        if (wfs_read(ino, at, buf, chunk) != (int)chunk) {
            matched = false;
            break;
        }
        for (uint32_t i = 0; i < chunk; i++)
            if (buf[i] != (uint8_t)((at + i) * 31 + ((at + i) >> 11))) {
                kprintf("  byte %u came back as %x, not %x\n",
                        at + i, buf[i],
                        (uint8_t)((at + i) * 31 + ((at + i) >> 11)));
                matched = false;
                break;
            }
    }
    check(matched, "and reads back byte for byte");

    kfree(buf);

    check(wfs_unlink(path) == 0, "the large file can be deleted");

    wfs_statfs(&after);
    check(after.free_bytes == before.free_bytes,
          "and every block it used came back, the pointer blocks included");

    if (after.free_bytes != before.free_bytes)
        kprintf("  %s went missing across the large-file test\n",
                fmt_bytes(before.free_bytes - after.free_bytes));
}

void selftest_filesystem(void)
{
    kputs("\n-- filesystem self-test --\n");
    failures = 0;

    if (!wfs_mounted()) {
        kputs("  no filesystem mounted; skipping\n");
        return;
    }

    wdiskinfo_t info;
    wfs_statfs(&info);
    kprintf("  disk   : %s total, %s used, %s free (%u byte blocks)\n",
            fmt_bytes(info.total_bytes), fmt_bytes(info.used_bytes),
            fmt_bytes(info.free_bytes), info.block_size);
    kprintf("  inodes : %u of %u free\n", info.free_inodes, info.total_inodes);

    test_filesystem_paths();
    test_filesystem_write();
    test_filesystem_large();
    test_filesystem_dirs();
    test_boot_counter();

    if (failures)
        panic("%u filesystem self-test failure(s)", failures);
    kputs("-- filesystem self-test passed --\n");
}

/* The RAM disk: that it holds what is written to it, and that what it holds is
 * exactly what is in it -- pages taken as a file grows and given back when it
 * goes away, with nothing kept in reserve. */
void selftest_ramdisk(void)
{
    static uint8_t pattern[4096];
    static uint8_t readback[4096];
    const uint32_t pages = 10;
    const uint32_t size  = pages * sizeof(pattern);

    kputs("\n-- RAM disk self-test --\n");
    failures = 0;

    for (uint32_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (uint8_t)(i * 7 + 1);

    uint64_t empty = pmm_free_bytes();
    kprintf("  free   : %s before anything is written to %s\n",
            fmt_bytes(empty), RAMFS_MOUNT);

    uint32_t ino = 0;
    check(ramfs_create("/ramdisk/selftest.bin", WFS_TYPE_FILE, &ino) == 0,
          "a file can be created");

    uint32_t written = 0;
    for (uint32_t i = 0; i < pages; i++) {
        int n = ramfs_write(ino, i * sizeof(pattern), pattern, sizeof(pattern));
        if (n > 0)
            written += (uint32_t)n;
    }
    check(written == size, "40 KiB of it can be written");

    bool identical = true;
    for (uint32_t i = 0; i < pages; i++) {
        if (ramfs_read(ino, i * sizeof(readback), readback, sizeof(readback))
                != (int)sizeof(readback) ||
            memcmp(readback, pattern, sizeof(readback)) != 0)
            identical = false;
    }
    check(identical, "it reads back byte for byte");

    struct wfs_inode in;
    check(ramfs_read_inode(ino, &in) == 0 && in.size == size,
          "its size is what was written");

    uint64_t held = empty - pmm_free_bytes();
    kprintf("  held   : %s for a %s file\n", fmt_bytes(held), fmt_bytes(size));
    check(held == size, "it took exactly the memory the file needs");

    check(ramfs_read(ino, size, readback, sizeof(readback)) == 0,
          "reading past the end returns nothing");

    check(ramfs_unlink("/ramdisk/selftest.bin") == 0, "it can be deleted");
    check(ramfs_lookup("/ramdisk/selftest.bin", NULL) < 0, "and is then gone");
    check(pmm_free_bytes() == empty, "every page it held came back");

    if (failures)
        panic("%u RAM disk self-test failure(s)", failures);
    kputs("-- RAM disk self-test passed --\n");
}

/* Local sockets, exercised below the descriptor layer.
 *
 * Everything here happens without blocking, which is what makes it safe to run
 * during a boot with nothing else to switch to: a connection is queued before
 * it is accepted, and no buffer is ever filled. */
void selftest_sockets(void)
{
    kputs("\n-- socket self-test --\n");
    failures = 0;

    int err = 0;
    socket_t *listener = socket_listen("/selftest.sock", W_ROOT_UID, &err);
    check(listener != NULL, "an address can be listened on");
    if (!listener) {
        panic("socket self-test cannot continue without a listener");
    }

    check(socket_listen("/selftest.sock", W_ROOT_UID, &err) == NULL && err == -W_EEXIST,
          "the same address cannot be listened on twice");

    check(socket_connect("/nobody-is-here.sock", W_ROOT_UID, &err) == NULL &&
          err == -W_ENOENT, "connecting to nothing reports ENOENT");

    socket_t *client = socket_connect("/selftest.sock", W_ROOT_UID, &err);
    check(client != NULL, "a client can connect");

    check(socket_pollin(listener), "the listener reports a connection waiting");

    socket_t *server = socket_accept(listener, &err);
    check(server != NULL, "the listener accepts it");

    /* Bytes, both ways. */
    const char *hello = "hello from the client";
    char        buf[64];
    int         fdn = 0;

    check(socket_send(client, hello, (uint32_t)strlen(hello), NULL, 0) ==
          (int)strlen(hello), "the client can send");
    check(socket_pollin(server), "the server sees it waiting");

    memset(buf, 0, sizeof(buf));
    fdn = 0;
    check(socket_recv(server, buf, sizeof(buf), NULL, &fdn) ==
          (int)strlen(hello), "the server receives the same length");
    check(strcmp(buf, hello) == 0, "and the same bytes");

    const char *reply = "and back again";
    check(socket_send(server, reply, (uint32_t)strlen(reply), NULL, 0) ==
          (int)strlen(reply), "the server can reply");

    memset(buf, 0, sizeof(buf));
    fdn = 0;
    check(socket_recv(client, buf, sizeof(buf), NULL, &fdn) ==
          (int)strlen(reply), "the client receives the reply");
    check(strcmp(buf, reply) == 0, "with the bytes intact");

    /* A descriptor, passed across.  A pipe is the easiest thing to prove it
     * with: write into one end here, pass the other, and read it out on the
     * far side of the socket. */
    pipe_t *p = pipe_create();
    check(p != NULL, "a pipe can be created to pass across");

    file_t passing;
    memset(&passing, 0, sizeof(passing));
    passing.type      = FD_PIPE;
    passing.pipe      = p;
    passing.write_end = false;               /* the read end travels */

    check(pipe_write(p, "through the socket", 18) == 18,
          "something is put into the pipe");

    check(socket_send(client, "fd", 2, &passing, 1) == 2,
          "the descriptor is sent with a message");

    file_t arrived[2];
    memset(buf, 0, sizeof(buf));
    fdn = 2;
    check(socket_recv(server, buf, sizeof(buf), arrived, &fdn) == 2,
          "the message arrives");
    check(fdn == 1, "and one descriptor with it");
    check(arrived[0].type == FD_PIPE && arrived[0].pipe == p,
          "which names the same pipe");

    memset(buf, 0, sizeof(buf));
    check(pipe_read(arrived[0].pipe, buf, sizeof(buf)) == 18 &&
          memcmp(buf, "through the socket", 18) == 0,
          "and reads out what was put in before it was sent");

    /* The reference the send took, and the one the receive handed over. */
    vfs_fd_drop(&arrived[0]);
    pipe_unref(p, false);

    /* Closing one end is visible from the other. */
    socket_unref(client);
    check(socket_pollin(server), "closing an end makes the other readable");
    memset(buf, 0, sizeof(buf));
    fdn = 0;
    check(socket_recv(server, buf, sizeof(buf), NULL, &fdn) == 0,
          "and a read there reports the end of the stream");
    check(socket_hungup(server), "which is reported as a hangup");

    socket_unref(server);
    socket_unref(listener);

    check(socket_connect("/selftest.sock", W_ROOT_UID, &err) == NULL && err == -W_ENOENT,
          "the address is gone once the listener closes");

    if (failures)
        panic("%u socket self-test failure(s)", failures);
    kputs("-- socket self-test passed --\n");
}

void selftest_processes(void)
{
    kputs("\n-- process self-test --\n");
    failures = 0;

    const char *prog = "/app/hello/launch";
    uint32_t    ino;

    if (wfs_lookup(prog, &ino) != 0) {
        kprintf("  %s is missing; skipping\n", prog);
        return;
    }

    uint64_t free_before = pmm_free_bytes();

    /* One process, with arguments, reaped for its exit status. */
    {
        char *argv[] = { "hello", "first", NULL };
        int32_t pid = proc_spawn(prog, argv, NULL);
        check(pid > 0, "a program can be spawned into ring 3");

        if (pid > 0) {
            int32_t status = 0;
            int32_t reaped = proc_wait(pid, &status);
            check(reaped == pid, "waiting for it returns its pid");
            /* hello returns its own pid as the exit status. */
            check(status == pid, "its exit status arrives intact");
        }
    }

    /* Two at once, both spinning, to show they are actually interleaved. */
    {
        char *argv[] = { "hello", "spin", NULL };
        int32_t a = proc_spawn(prog, argv, NULL);
        int32_t b = proc_spawn(prog, argv, NULL);

        check(a > 0 && b > 0 && a != b,
              "two processes can run at the same time");

        int32_t reaped = 0;
        for (int i = 0; i < 2; i++)
            if (proc_wait(-1, NULL) > 0)
                reaped++;
        check(reaped == 2, "both are reaped");
    }

    /* File and directory calls exercised from ring 3 through wkernel. */
    {
        char *argv[] = { "hello", "files", NULL };
        int32_t pid = proc_spawn(prog, argv, NULL);
        int32_t status = 0;

        if (pid > 0)
            proc_wait(pid, &status);

        check(pid > 0 && status == pid,
              "a process can create, read and delete files through wkernel");
    }

    /* A process that faults must die on its own, without taking us with it. */
    {
        char *argv[] = { "hello", "fault", NULL };
        int32_t pid = proc_spawn(prog, argv, NULL);
        int32_t status = 0;

        if (pid > 0)
            proc_wait(pid, &status);

        check(pid > 0 && status == -1,
              "a process that touches a bad address is killed, not the kernel");
    }

    check(pmm_free_bytes() == free_before,
          "every process freed all of its memory on exit");

    if (failures)
        panic("%u process self-test failure(s)", failures);
    kputs("-- process self-test passed --\n");
}

/* Compare a result against a vector written as hex, which is how every
 * specification prints them -- keeping them in that form here means a vector
 * can be checked against the document it came from by eye. */
static bool matches_hex(const uint8_t *got, const char *hex)
{
    for (int i = 0; hex[i * 2]; i++) {
        uint8_t want = 0;

        for (int nibble = 0; nibble < 2; nibble++) {
            char c = hex[i * 2 + nibble];
            uint8_t v = (c >= 'a') ? (uint8_t)(c - 'a' + 10) : (uint8_t)(c - '0');

            want = (uint8_t)((want << 4) | v);
        }
        if (got[i] != want)
            return false;
    }
    return true;
}

void selftest_crypto(void)
{
    uint8_t out[40];

    kputs("\n-- cryptography self-test --\n");
    failures = 0;

    sha1("abc", 3, out);
    check(matches_hex(out, "a9993e364706816aba3e25717850c26c9cd0d89d"),
          "SHA-1 of \"abc\" (FIPS 180-2)");

    {
        uint8_t key[20];

        memset(key, 0x0b, sizeof(key));
        hmac_sha1(key, sizeof(key), "Hi There", 8, out);
        check(matches_hex(out, "b617318655057264e28bc0b6fb378c8ef146be00"),
              "HMAC-SHA1 case 1 (RFC 2202)");

        sha1_prf(key, sizeof(key), "prefix", (const uint8_t *)"Hi There", 8, out, 20);
        check(matches_hex(out, "bcd4c650b30b9684951829e0d75f9d54b862175e"),
              "the 802.11 pseudo-random function");
    }

    /* The passphrase-to-master-key vector from the 802.11i annex.  This is
     * the single most load-bearing number in the whole wireless path: get it
     * wrong and every protected network refuses us. */
    pbkdf2_sha1("password", (const uint8_t *)"IEEE", 4, 4096, out, 32);
    check(matches_hex(out,
          "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"),
          "WPA2 master key from a passphrase (802.11i)");

    {
        aes_ctx_t ctx;
        uint8_t   key[16], block[16];

        for (int i = 0; i < 16; i++) {
            key[i]   = (uint8_t)i;
            block[i] = (uint8_t)(i * 0x11);
        }

        aes_set_encrypt_key(&ctx, key, 16);
        aes_encrypt_block(&ctx, block, out);
        check(matches_hex(out, "69c4e0d86a7b0430d8cdb78070b4c55a"),
              "AES-128 of the FIPS-197 example block");

        aes_set_decrypt_key(&ctx, key, 16);
        aes_decrypt_block(&ctx, out, out);
        check(memcmp(out, block, 16) == 0, "AES-128 decryption undoes it");
    }

    {
        /* RFC 3394 section 4.1, which is exactly the shape the group key
         * arrives in during the third handshake message. */
        uint8_t kek[16];
        uint8_t wrapped[24] = {
            0x1F, 0xA6, 0x8B, 0x0A, 0x81, 0x12, 0xB4, 0x47,
            0xAE, 0xF3, 0x4B, 0xD8, 0xFB, 0x5A, 0x7B, 0x82,
            0x9D, 0x3E, 0x86, 0x23, 0x71, 0xD2, 0xCF, 0xE5
        };

        for (int i = 0; i < 16; i++)
            kek[i] = (uint8_t)i;

        check(aes_unwrap_key(kek, 16, wrapped, 24, out) &&
              matches_hex(out, "00112233445566778899aabbccddeeff"),
              "AES key unwrap (RFC 3394)");

        kek[0] ^= 1;
        check(!aes_unwrap_key(kek, 16, wrapped, 24, out),
              "key unwrap refuses a wrong key rather than returning rubbish");
    }

    {
        /* RFC 3610 packet vector 1: a 13-byte nonce and an 8-byte tag, the
         * same parameters CCMP uses on every protected data frame. */
        uint8_t key[16] = {
            0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
            0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF
        };
        uint8_t nonce[13] = {
            0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
            0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5
        };
        uint8_t aad[8], payload[23], cipher[31], plain[23];

        for (int i = 0; i < 8; i++)
            aad[i] = (uint8_t)i;
        for (int i = 0; i < 23; i++)
            payload[i] = (uint8_t)(8 + i);

        aes_ccm_encrypt(key, nonce, aad, 8, payload, 23, cipher);
        check(matches_hex(cipher,
              "588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0"),
              "AES-CCM ciphertext and tag (RFC 3610)");

        check(aes_ccm_decrypt(key, nonce, aad, 8, cipher, 31, plain) &&
              memcmp(plain, payload, 23) == 0,
              "AES-CCM decryption recovers the payload");

        cipher[3] ^= 0x40;
        check(!aes_ccm_decrypt(key, nonce, aad, 8, cipher, 31, plain),
              "AES-CCM rejects a frame whose bits were changed");
    }

    if (failures)
        panic("%u cryptography self-test failure(s)", failures);
    kputs("-- cryptography self-test passed --\n");
}

void selftest_network(void)
{
    if (!net_ready()) {
        /* A machine with no card is not a failure; it is most machines. */
        kputs("\n-- network self-test skipped: no adapter --\n");
        return;
    }

    kputs("\n-- network self-test --\n");
    failures = 0;

    /* Ask the network for an address.
     *
     * This is the only part of the wireless work that can be exercised
     * without a wireless adapter, and it is worth exercising: DHCP is what a
     * link joined by `wifi connect` uses to become usable, and until now it
     * had no way to be run at all.  QEMU's user-mode network has a server on
     * it that answers, so booting under QEMU tests the real exchange against
     * a real server -- four messages, option parsing and all.
     *
     * The address it hands out is the one the stack already had, so nothing
     * is disturbed by asking. */
    uint32_t before = net_local_ip();
    int      r = net_dhcp(4000);

    if (r == -W_ETIMEDOUT) {
        /* Nothing answered.  On a machine whose network has no server that
         * is the right answer and not a fault, so it is reported rather than
         * failed -- the point of the check is that the exchange runs and
         * comes back, not that a server exists to run it with. */
        kputs("  [skip] no DHCP server answered; the addresses are unchanged\n");
    } else {
        check(r == 0, "the DHCP exchange completed");
        check(net_dhcp_configured(), "the addresses came from the server");

        uint32_t ip = net_local_ip();

        check(ip != 0, "the server gave us an address");
        check(net_prefix_length() > 0 && net_prefix_length() <= 32,
              "the netmask it came with is a sensible one");
        check(net_gateway_ip() != 0, "and a gateway to reach the rest by");

        kprintf("  address %u.%u.%u.%u/%u, was %u.%u.%u.%u\n",
                ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 24) & 0xFF, net_prefix_length(),
                before & 0xFF, (before >> 8) & 0xFF,
                (before >> 16) & 0xFF, (before >> 24) & 0xFF);
    }

    if (failures)
        panic("%u network self-test failure(s)", failures);
    kputs("-- network self-test passed --\n");
}

void selftest_page_fault(void)
{
    kputs("\n-- page-fault demonstration --\n");
    kputs("writing through a null pointer; the handler should report it\n");

    volatile uint32_t *nowhere = (volatile uint32_t *)0;
    *nowhere = 1;

    panic("the null page was writable -- it should not have been");
}

#endif /* WOS_NO_SELFTEST */
