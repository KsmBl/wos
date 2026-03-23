/* hello -- a self-contained ring 3 test program.
 *
 * This one deliberately uses no library at all: it provides its own _start and
 * talks to the kernel through raw `int $0x80`, so it proves the ring 3 entry
 * path, the syscall gate and argument passing work before any of lib/wkernel
 * exists to hide them.
 *
 * Run it with an argument of "spin" to make it burn CPU for a while, which is
 * how preemption between two processes gets demonstrated, or "fault" to make
 * it touch a bad address and get killed.
 */

#include "wabi.h"

/* Raw syscall wrappers. The kernel takes the call number in eax and the
 * arguments in ebx, ecx and edx, returning the result in eax. */
static int syscall1(int n, int a)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void put(const char *s)
{
    syscall3(WSYS_WRITE, W_STDOUT, (int)s, (int)slen(s));
}

static void put_uint(unsigned v)
{
    char buf[12];
    int  n = 0;

    if (v == 0)
        buf[n++] = '0';
    while (v) {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    }

    char out[12];
    int  m = 0;
    while (n)
        out[m++] = buf[--n];

    syscall3(WSYS_WRITE, W_STDOUT, (int)out, m);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv)
{
    int pid = syscall1(WSYS_GETPID, 0);

    put("hello: running as pid ");
    put_uint((unsigned)pid);
    put(", argc=");
    put_uint((unsigned)argc);

    for (int i = 0; i < argc; i++) {
        put(" argv[");
        put_uint((unsigned)i);
        put("]=");
        put(argv[i]);
    }
    put("\n");

    /* Show that the memory syscalls work from ring 3. */
    wmeminfo_t mem;
    if (syscall1(WSYS_MEMINFO, (int)&mem) == 0) {
        put("hello: system RAM free ");
        put_uint(mem.free_bytes / 1024);
        put(" KiB of ");
        put_uint(mem.total_bytes / 1024);
        put(" KiB\n");
    }

    wprocmem_t me;
    if (syscall2(WSYS_PROCMEM, 0, (int)&me) == 0) {
        put("hello: my resident memory is ");
        put_uint(me.resident_bytes / 1024);
        put(" KiB\n");
    }

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "fault")) {
            put("hello: about to write through a bad pointer\n");
            *(volatile int *)0xDEAD0000 = 1;
            put("hello: still alive, which should not happen\n");
        }

        if (streq(argv[i], "spin")) {
            /* Burn several timeslices so the scheduler has to preempt us. */
            unsigned start = (unsigned)syscall1(WSYS_TICKS, 0);
            unsigned last  = start;
            int      slices = 0;

            while ((unsigned)syscall1(WSYS_TICKS, 0) - start < 30) {
                unsigned now = (unsigned)syscall1(WSYS_TICKS, 0);
                if (now != last) {
                    last = now;
                    slices++;
                }
            }

            put("hello: pid ");
            put_uint((unsigned)pid);
            put(" spun across ");
            put_uint((unsigned)slices);
            put(" ticks\n");
        }
    }

    return pid;
}

/* The entry point. The kernel starts us with the stack holding
 * [argc][argv[0]]..[NULL], which is exactly what main wants. */
__attribute__((section(".text.start"), naked)) void _start(void)
{
    __asm__ volatile(
        "movl (%esp), %eax\n\t"     /* argc            */
        "leal 4(%esp), %edx\n\t"    /* argv            */
        "pushl %edx\n\t"
        "pushl %eax\n\t"
        "call main\n\t"
        "addl $8, %esp\n\t"
        "movl %eax, %ebx\n\t"       /* exit status     */
        "movl $0, %eax\n\t"         /* WSYS_EXIT       */
        "int $0x80\n\t"
        "hlt");
}
