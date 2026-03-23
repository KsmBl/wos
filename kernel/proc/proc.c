/* Process and thread management. */

#include "proc.h"
#include "sched.h"
#include "elf.h"
#include "vfs.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "kprintf.h"
#include "gdt.h"

extern void enter_user_mode(uint32_t entry, uint32_t user_stack)
    __attribute__((noreturn));

static process_t processes[MAX_PROCESSES];
static thread_t  threads[MAX_THREADS];

static int32_t next_pid = 1;
static int32_t next_tid = 1;

/* The boot context, adopted as the idle thread. */
static process_t kernel_proc;
static thread_t *idle;

thread_t *sched_current_thread(void);

thread_t *thread_current(void)
{
    return sched_current_thread();
}

process_t *proc_current(void)
{
    thread_t *t = thread_current();
    return t ? t->proc : &kernel_proc;
}

process_t *proc_by_pid(int32_t pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].used && processes[i].pid == pid)
            return &processes[i];
    return NULL;
}

static process_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!processes[i].used) {
            memset(&processes[i], 0, sizeof(processes[i]));
            processes[i].used = true;
            processes[i].pid  = next_pid++;
            return &processes[i];
        }
    }
    return NULL;
}

static thread_t *thread_alloc(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            memset(&threads[i], 0, sizeof(threads[i]));
            threads[i].tid = next_tid++;
            return &threads[i];
        }
    }
    return NULL;
}

/* The trampoline a freshly created user thread returns into.  It runs on the
 * thread's own kernel stack with interrupts off, then never comes back. */
static void user_thread_start(void)
{
    thread_t *t = thread_current();

    enter_user_mode(t->user_entry, t->user_stack);
}

/* Prime a kernel stack so that switch_context() will "return" into `entry`.
 * The layout mirrors exactly what switch_context pushes. */
static void thread_prime_stack(thread_t *t, void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)(t->kernel_stack + t->kernel_stack_size);

    *--sp = (uint32_t)entry;   /* return address for switch_context's `ret` */
    *--sp = 0;                 /* ebp    */
    *--sp = 0;                 /* ebx    */
    *--sp = 0;                 /* esi    */
    *--sp = 0;                 /* edi    */
    *--sp = 0x00000002;        /* eflags: reserved bit set, interrupts off
                                * until iret loads the user flags           */

    t->esp = (uint32_t)sp;
}

void proc_init(void)
{
    memset(processes, 0, sizeof(processes));
    memset(threads, 0, sizeof(threads));
    memset(&kernel_proc, 0, sizeof(kernel_proc));

    kernel_proc.used  = true;
    kernel_proc.pid   = 0;
    kernel_proc.space = paging_kernel_space();
    strlcpy(kernel_proc.name, "kernel", sizeof(kernel_proc.name));
    strlcpy(kernel_proc.cwd, "/", sizeof(kernel_proc.cwd));
    vfs_init_fds(&kernel_proc);

    /* Adopt the current boot context as a thread so the scheduler always has
     * somewhere to switch back to.  Its esp is filled in by the first
     * switch_context call, so it needs no primed stack. */
    idle = thread_alloc();
    if (!idle)
        panic("proc_init: no thread slot for the idle thread");

    extern uint8_t __boot_stack_top[];
    idle->proc              = &kernel_proc;
    idle->kernel_stack      = (uint32_t)__boot_stack_top - 16384;
    idle->kernel_stack_size = 16384;
    idle->state             = THREAD_RUNNING;

    kernel_proc.thread       = idle;
    kernel_proc.thread_count = 1;

    sched_init(idle);
}

/* Copy argv into the top of the new process's user stack and return the esp
 * the program should start with.
 *
 * The layout is the usual one, so crt0 can find its arguments the standard
 * way:   [argc][argv[0]]...[argv[n-1]][NULL][string data]
 */
static uint32_t setup_user_stack(char *const argv[], uint32_t stack_top)
{
    int argc = 0;
    while (argv && argv[argc] && argc < MAX_ARGS)
        argc++;

    uint32_t ptrs[MAX_ARGS];
    uint32_t sp = stack_top;

    /* The strings go at the very top, growing downwards. */
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        ptrs[i] = sp;
    }

    sp &= ~3u;                      /* the pointer array must be aligned */

    uint32_t *stack = (uint32_t *)sp;
    *--stack = 0;                   /* argv[argc] == NULL */
    for (int i = argc - 1; i >= 0; i--)
        *--stack = ptrs[i];
    *--stack = (uint32_t)argc;

    return (uint32_t)stack;
}

int32_t proc_spawn(const char *path, char *const argv[], process_t *parent)
{
    if (!parent)
        parent = &kernel_proc;

    /* Read the executable while still in the parent's address space. */
    void    *image;
    uint32_t image_size;
    int r = vfs_read_file(parent, path, &image, &image_size);
    if (r < 0)
        return r;

    process_t *p = proc_alloc();
    if (!p) {
        kfree(image);
        return -W_ENOMEM;
    }

    thread_t *t = thread_alloc();
    if (!t) {
        p->used = false;
        kfree(image);
        return -W_ENOMEM;
    }

    p->space = paging_new_addrspace();
    if (!p->space) {
        p->used = false;
        kfree(image);
        return -W_ENOMEM;
    }

    p->parent = parent;
    strlcpy(p->cwd, parent->cwd, sizeof(p->cwd));
    vfs_init_fds(p);

    /* Name the process after the application directory, so /app/whell/launch
     * shows up as "whell" rather than "launch". */
    {
        const char *base = path;
        const char *slash = strrchr(path, '/');
        if (slash && slash != path) {
            /* Step back over "/launch" to find the directory name. */
            const char *prev = slash - 1;
            while (prev > path && *prev != '/')
                prev--;
            base = (*prev == '/') ? prev + 1 : prev;
            uint32_t len = (uint32_t)(slash - base);
            if (len >= sizeof(p->name))
                len = sizeof(p->name) - 1;
            memcpy(p->name, base, len);
            p->name[len] = '\0';
        } else {
            strlcpy(p->name, base, sizeof(p->name));
        }
    }

    t->proc              = p;
    t->kernel_stack_size = KERNEL_STACK_SIZE;
    t->kernel_stack      = (uint32_t)kmalloc(KERNEL_STACK_SIZE);
    if (!t->kernel_stack) {
        paging_free_addrspace(p->space);
        p->used = false;
        kfree(image);
        return -W_ENOMEM;
    }

    p->thread       = t;
    p->thread_count = 1;

    /* Switch into the new address space so the loader can write through
     * ordinary user addresses; the kernel stays mapped throughout. */
    addrspace_t *previous = paging_current();
    paging_switch(p->space);

    uint32_t entry = 0;
    r = elf_load(p, image, image_size, &entry);

    if (r == 0) {
        /* Map the user stack just below USER_STACK_TOP. */
        for (uint32_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
            uint32_t page = USER_STACK_TOP - USER_STACK_SIZE + off;
            if (!paging_map_alloc(p->space, page, PTE_WRITE | PTE_USER, true)) {
                r = -W_ENOMEM;
                break;
            }
        }
        p->stack_bytes = USER_STACK_SIZE;
    }

    uint32_t user_esp = 0;
    if (r == 0)
        user_esp = setup_user_stack(argv, USER_STACK_TOP);

    paging_switch(previous);
    kfree(image);

    if (r < 0) {
        kfree((void *)t->kernel_stack);
        paging_free_addrspace(p->space);
        t->state = THREAD_UNUSED;
        p->used  = false;
        return r;
    }

    t->user_entry = entry;
    t->user_stack = user_esp;

    /* Queueing the thread has to be atomic against the timer IRQ, which walks
     * the same run queue from schedule(). */
    bool interrupts_were_on;
    {
        uint32_t flags;
        __asm__ volatile("pushfl; popl %0" : "=r"(flags));
        interrupts_were_on = (flags & 0x200) != 0;
    }
    __asm__ volatile("cli");

    thread_prime_stack(t, user_thread_start);
    t->state = THREAD_READY;
    sched_add(t);

    if (interrupts_were_on)
        __asm__ volatile("sti");

    return p->pid;
}

void proc_exit(int32_t status)
{
    process_t *p = proc_current();
    thread_t  *t = thread_current();

    if (p == &kernel_proc)
        panic("the kernel process tried to exit");

    vfs_close_all(p);

    p->exited      = true;
    p->exit_status = status;

    /* The address space is freed by whoever reaps this process: we are still
     * running on this thread's kernel stack, and although that lives in the
     * identity-mapped heap, the page directory we are using is the one we
     * would be freeing. */
    t->state = THREAD_ZOMBIE;

    sched_wake(WAIT_CHILD);
    schedule();

    panic("a zombie thread was scheduled again");
}

int32_t proc_wait(int32_t pid, int32_t *status)
{
    process_t *parent = proc_current();

    for (;;) {
        bool any_children = false;

        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *c = &processes[i];

            if (!c->used || c->parent != parent)
                continue;
            if (pid > 0 && c->pid != pid)
                continue;

            any_children = true;

            if (!c->exited)
                continue;

            int32_t reaped = c->pid;
            if (status)
                *status = c->exit_status;

            /* Now it is safe to tear down: the child is not running. */
            if (c->thread) {
                sched_remove(c->thread);
                if (c->thread->kernel_stack)
                    kfree((void *)c->thread->kernel_stack);
                c->thread->state = THREAD_UNUSED;
                c->thread = NULL;
            }
            if (c->space) {
                paging_free_addrspace(c->space);
                c->space = NULL;
            }
            c->used = false;

            return reaped;
        }

        if (!any_children)
            return -W_ECHILD;

        sched_block(WAIT_CHILD);
    }
}

uint32_t proc_sbrk(int32_t increment)
{
    process_t *p = proc_current();
    uint32_t   old = p->heap_break;

    if (increment == 0)
        return old;

    if (increment > 0) {
        uint32_t new_break = old + (uint32_t)increment;

        /* Do not let the heap grow into the stack. */
        if (new_break > USER_STACK_TOP - USER_STACK_SIZE)
            return (uint32_t)-1;

        for (uint32_t page = ALIGN_UP(old, PAGE_SIZE);
             page < ALIGN_UP(new_break, PAGE_SIZE); page += PAGE_SIZE) {
            if (paging_translate(p->space, page))
                continue;
            if (!paging_map_alloc(p->space, page, PTE_WRITE | PTE_USER, true))
                return (uint32_t)-1;
        }
        p->heap_break = new_break;
    } else {
        uint32_t shrink = (uint32_t)(-increment);
        if (shrink > old - p->heap_start)
            return (uint32_t)-1;

        uint32_t new_break = old - shrink;
        for (uint32_t page = ALIGN_UP(new_break, PAGE_SIZE);
             page < ALIGN_UP(old, PAGE_SIZE); page += PAGE_SIZE)
            paging_unmap(p->space, page);

        p->heap_break = new_break;
    }

    return old;
}

void proc_meminfo(const process_t *p, wprocmem_t *out)
{
    memset(out, 0, sizeof(*out));

    out->pid            = p->pid;
    strlcpy(out->name, p->name, sizeof(out->name));
    out->resident_bytes = p->space ? paging_user_bytes(p->space) : 0;
    out->code_bytes     = p->code_bytes;
    out->data_bytes     = p->data_bytes;
    out->stack_bytes    = p->stack_bytes;
    out->heap_bytes     = p->heap_break - p->heap_start;
    out->thread_count   = p->thread_count;
}

void thread_meminfo(const thread_t *t, wthreadmem_t *out)
{
    memset(out, 0, sizeof(*out));

    out->tid                = t->tid;
    out->pid                = t->proc ? t->proc->pid : 0;
    out->kernel_stack_bytes = t->kernel_stack_size;
    out->user_stack_bytes   = t->proc ? t->proc->stack_bytes : 0;
    out->cpu_ticks          = t->cpu_ticks;
}

int32_t proc_list(wprocmem_t *out, int32_t max)
{
    int32_t n = 0;

    for (int i = 0; i < MAX_PROCESSES && n < max; i++)
        if (processes[i].used)
            proc_meminfo(&processes[i], &out[n++]);

    return n;
}
