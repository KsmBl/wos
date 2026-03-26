/* hello -- a demonstration program and the API's smoke test.
 *
 * It exercises most of wkernel: printing, memory and disk statistics, file
 * I/O, directory listing and the heap.  Because it is also what the kernel's
 * boot-time process self-test runs, breaking the API breaks the boot loudly
 * rather than quietly.
 *
 * Arguments it understands:
 *   spin    burn CPU for a while, so preemption between two copies is visible
 *   fault   write through a bad pointer, to show the process being killed
 *   files   create, write, read back and delete a file
 */

#include <wkernel.h>

static void show_identity(int argc, char **argv)
{
    wprintf("hello: pid %d, argc %d", wgetpid(), argc);
    for (int i = 0; i < argc; i++)
        wprintf(" argv[%d]=%s", i, argv[i]);
    wprintf("\n");
}

static void show_memory(void)
{
    wmeminfo_t mem;
    if (wmeminfo(&mem) == 0)
        wprintf("hello: RAM %s free of %s (kernel holds %s)\n",
                whuman(mem.free_bytes), whuman(mem.total_bytes),
                whuman(mem.kernel_bytes));

    wprocmem_t me;
    if (wprocmem(0, &me) == 0)
        wprintf("hello: I am resident in %s (code %s, data %s, stack %s)\n",
                whuman(me.resident_bytes), whuman(me.code_bytes),
                whuman(me.data_bytes), whuman(me.stack_bytes));

    wthreadmem_t th;
    if (wthreadmem(0, &th) == 0)
        wprintf("hello: thread %d has run for %u ticks\n",
                th.tid, th.cpu_ticks);

    wdiskinfo_t disk;
    if (wdiskinfo(&disk) == 0)
        wprintf("hello: disk %s used of %s\n",
                whuman(disk.used_bytes), whuman(disk.total_bytes));
}

static void show_heap(void)
{
    /* Large enough to force wsbrk to extend the heap more than once. */
    char *big = malloc(20000);
    if (!big) {
        wprintf("hello: malloc failed\n");
        return;
    }

    memset(big, 'x', 20000);
    big[19999] = '\0';

    wprocmem_t me;
    wprocmem(0, &me);
    wprintf("hello: after a 20000 byte malloc my heap is %s\n",
            whuman(me.heap_bytes));

    free(big);
}

static void test_files(void)
{
    const char *path = "/home/hello.tmp";
    const char *text = "written by hello through the wkernel API\n";

    int fd = wopen(path, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
    if (fd < 0) {
        wprintf("hello: cannot create %s: %s\n", path, wstrerror(-fd));
        return;
    }

    int n = wwrite(fd, text, strlen(text));
    wclose(fd);
    wprintf("hello: wrote %d bytes to %s\n", n, path);

    char buf[128];
    fd = wopen(path, W_O_RDONLY);
    if (fd >= 0) {
        n = wread(fd, buf, sizeof(buf) - 1);
        if (n >= 0) {
            buf[n] = '\0';
            wprintf("hello: read it back: %s", buf);
        }
        wclose(fd);
    }

    wstat_t st;
    if (wstat(path, &st) == 0)
        wprintf("hello: it is %u bytes in %u block(s)\n", st.size, st.blocks);

    int d = wopendir("/home");
    if (d >= 0) {
        wprintf("hello: /home contains");
        wdirent_t e;
        while (wreaddir(d, &e) == 1) {
            if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
                continue;
            wprintf(" %s%s", e.name, e.type == W_FT_DIR ? "/" : "");
        }
        wprintf("\n");
        wclosedir(d);
    }

    if (wunlink(path) == 0)
        wprintf("hello: removed %s\n", path);
}

static void spin(void)
{
    unsigned start  = wticks();
    unsigned last   = start;
    unsigned slices = 0;

    while (wticks() - start < 30) {
        unsigned now = wticks();
        if (now != last) {
            last = now;
            slices++;
        }
    }

    wprintf("hello: pid %d spun across %u ticks\n", wgetpid(), slices);
}

int main(int argc, char **argv)
{
    show_identity(argc, argv);

    int did_something = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "spin") == 0) {
            spin();
            did_something = 1;
        } else if (strcmp(argv[i], "files") == 0) {
            test_files();
            did_something = 1;
        } else if (strcmp(argv[i], "fault") == 0) {
            wprintf("hello: about to write through a bad pointer\n");
            *(volatile int *)0xDEAD0000 = 1;
            wprintf("hello: still alive, which should not happen\n");
            did_something = 1;
        }
    }

    if (!did_something) {
        show_memory();
        show_heap();
    }

    /* The exit status the parent sees; the self-test checks for it. */
    return wgetpid();
}
