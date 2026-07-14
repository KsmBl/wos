/* Process and thread management. */

#include "proc.h"
#include "sched.h"
#include "service.h"
#include "elf.h"
#include "vfs.h"
#include "display.h"
#include "pipe.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "kprintf.h"
#include "gdt.h"

extern void enter_user_mode(uint64_t entry, uint64_t user_stack)
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
            processes[i].term_rows = W_CONSOLE_HEIGHT;
            processes[i].term_cols = W_CONSOLE_WIDTH;
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
    uint64_t *sp = (uint64_t *)(t->kernel_stack + t->kernel_stack_size);

    /* One pad word so that after switch_context's `ret` pops the entry
     * address, rsp is 8 modulo 16 -- what System V promises a function on
     * entry. */
    *--sp = 0;

    *--sp = (uint64_t)entry;   /* return address for switch_context's `ret` */
    *--sp = 0;                 /* rbp    */
    *--sp = 0;                 /* rbx    */
    *--sp = 0;                 /* r12    */
    *--sp = 0;                 /* r13    */
    *--sp = 0;                 /* r14    */
    *--sp = 0;                 /* r15    */
    *--sp = 0x0000000000000002;/* rflags: reserved bit set, interrupts off
                                * until iretq loads the user flags          */

    t->rsp = (uint64_t)sp;
}

void proc_init(void)
{
    memset(processes, 0, sizeof(processes));
    memset(threads, 0, sizeof(threads));
    memset(&kernel_proc, 0, sizeof(kernel_proc));

    kernel_proc.used  = true;
    kernel_proc.pid   = 0;
    kernel_proc.uid   = W_ROOT_UID;   /* the kernel, and so the first shell */
    kernel_proc.term_rows = W_CONSOLE_HEIGHT;
    kernel_proc.term_cols = W_CONSOLE_WIDTH;
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
    idle->kernel_stack      = (uint64_t)__boot_stack_top - 16384;
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
static uint64_t setup_user_stack(char *const argv[], uint64_t stack_top)
{
    int argc = 0;
    while (argv && argv[argc] && argc < MAX_ARGS)
        argc++;

    uint64_t ptrs[MAX_ARGS];
    uint64_t sp = stack_top;

    /* The strings go at the very top, growing downwards. */
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = (uint64_t)strlen(argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        ptrs[i] = sp;
    }

    sp &= ~15UL;                    /* keep the array 16-byte aligned */

    uint64_t *stack = (uint64_t *)sp;
    *--stack = 0;                   /* argv[argc] == NULL */
    for (int i = argc - 1; i >= 0; i--)
        *--stack = ptrs[i];
    *--stack = (uint64_t)argc;

    return (uint64_t)stack;
}

/* Interrupts off, and a note of whether they were on, so a critical section
 * can put things back the way it found them rather than assuming. */
static bool interrupts_off(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile("cli");

    return (flags & 0x200) != 0;
}

static void interrupts_restore(bool were_on)
{
    if (were_on)
        __asm__ volatile("sti");
}

int32_t proc_spawn(const char *path, char *const argv[], process_t *parent)
{
    return proc_spawn_io(path, argv, parent, NULL);
}

int32_t proc_spawn_io(const char *path, char *const argv[], process_t *parent,
                      const struct spawn_io *io)
{
    if (!parent)
        parent = &kernel_proc;

    /* Read the executable while still in the parent's address space. */
    void    *image;
    uint64_t image_size;
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
    p->uid    = parent->uid;        /* a child runs as whoever started it */
    strlcpy(p->cwd, parent->cwd, sizeof(p->cwd));

    /* An armed seat grant is spent here, on the first child spawned after it
     * was made.  Spent rather than copied: the session leader gets the screen,
     * and everything it goes on to start -- a terminal, an editor -- is an
     * ordinary process that cannot take the display from it. */
    if (parent->seat_pending) {
        parent->seat_pending = false;
        p->seat = true;
    }

    /* A child sees the same terminal size as its parent unless it is being
     * given a fresh window of its own below. */
    p->term_rows = parent->term_rows;
    p->term_cols = parent->term_cols;

    vfs_init_fds(p);

    if (io && io->piped) {
        /* A window of its own: fd 0 reads from one pipe, fds 1 and 2 write to
         * another.  The refs taken here, from the child's side, balance the
         * unrefs vfs_close_all() does when the child exits. */
        pipe_ref(io->in, false);       /* fd 0: read end            */
        pipe_ref(io->out, true);       /* fd 1: write end           */
        pipe_ref(io->out, true);       /* fd 2: the same write end  */

        p->fds[0].type = FD_PIPE; p->fds[0].pipe = io->in;  p->fds[0].write_end = false;
        p->fds[1].type = FD_PIPE; p->fds[1].pipe = io->out; p->fds[1].write_end = true;
        p->fds[2].type = FD_PIPE; p->fds[2].pipe = io->out; p->fds[2].write_end = true;

        if (io->rows) p->term_rows = io->rows;
        if (io->cols) p->term_cols = io->cols;
    } else {
        /* An ordinary spawn inherits the parent's stdio, so a program run from
         * a shell inside a :term window writes into that window, not past it
         * to the console. */
        vfs_inherit_stdio(p, parent);
    }

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
    t->kernel_stack      = (uint64_t)kmalloc(KERNEL_STACK_SIZE);
    if (!t->kernel_stack) {
        paging_free_addrspace(p->space);
        p->used = false;
        kfree(image);
        return -W_ENOMEM;
    }

    p->thread       = t;
    p->thread_count = 1;

    /* Switch into the new address space so the loader can write through
     * ordinary user addresses; the kernel stays mapped throughout.
     *
     * Interrupts off for the whole of the borrowing, and not as a nicety.  The
     * scheduler restores the address space belonging to the thread it is
     * switching to, and this thread's process is the *parent* -- so being
     * preempted here comes back with the parent's tables installed, and the
     * rest of the program is loaded into the parent's memory.  It is bounded
     * work with nothing in it that blocks: a copy of an image already in
     * memory, and a stack. */
    bool         interrupts_were_on = interrupts_off();
    addrspace_t *previous           = paging_current();

    paging_switch(p->space);

    uint64_t entry = 0;
    r = elf_load(p, image, image_size, &entry);

    if (r == 0) {
        /* Map the user stack just below USER_STACK_TOP. */
        for (uint64_t off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
            uint64_t page = USER_STACK_TOP - USER_STACK_SIZE + off;
            if (!paging_map_alloc(p->space, page, PTE_WRITE | PTE_USER, true)) {
                r = -W_ENOMEM;
                break;
            }
        }
        p->stack_bytes = USER_STACK_SIZE;
    }

    uint64_t user_rsp = 0;
    if (r == 0)
        user_rsp = setup_user_stack(argv, USER_STACK_TOP);

    paging_switch(previous);
    interrupts_restore(interrupts_were_on);
    kfree(image);

    if (r < 0) {
        kfree((void *)t->kernel_stack);
        paging_free_addrspace(p->space);
        t->state = THREAD_UNUSED;
        p->used  = false;
        return r;
    }

    t->user_entry = entry;
    t->user_stack = user_rsp;

    /* Queueing the thread has to be atomic against the timer IRQ, which walks
     * the same run queue from schedule(). */
    bool queued_with_interrupts_on = interrupts_off();

    thread_prime_stack(t, user_thread_start);
    t->state = THREAD_READY;
    sched_add(t);

    interrupts_restore(queued_with_interrupts_on);

    return p->pid;
}

void proc_exit(int32_t status)
{
    process_t *p = proc_current();
    thread_t  *t = thread_current();

    if (p == &kernel_proc)
        panic("the kernel process tried to exit");

    vfs_close_all(p);

    /* Before the address space goes.  Whoever reaps this process frees every
     * frame it finds mapped, and a shared one is not this process's to give
     * back -- the compositor may still be drawing from it. */
    shm_unmap_all(p);

    /* If this process had the screen, the console gets it back.  A compositor
     * that faults must not take the machine's only output with it. */
    display_release(p->pid);

    p->exited      = true;
    p->exit_status = status;

    /* If this was a service, the manager has to stop claiming it is running.
     * A pid that is not one is ignored. */
    service_reap(p->pid, status);

    /* The address space is freed by whoever reaps this process: we are still
     * running on this thread's kernel stack, and although that lives in the
     * identity-mapped heap, the page directory we are using is the one we
     * would be freeing. */
    t->state = THREAD_ZOMBIE;

    sched_wake(WAIT_CHILD);
    schedule();

    panic("a zombie thread was scheduled again");
}

int32_t proc_kill(int32_t pid)
{
    process_t *p = proc_by_pid(pid);

    if (!p || p->exited)
        return -W_ESRCH;
    if (p == &kernel_proc)
        return -W_EPERM;

    p->killed = true;

    /* Whatever it is waiting for, it is not waiting for it any more.  Every
     * blocking path rechecks after being woken, finds the flag, and leaves. */
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = &threads[i];

        if (t->proc == p && t->state == THREAD_BLOCKED) {
            t->state       = THREAD_READY;
            t->wait_reason = WAIT_NONE;
            t->wake_at     = 0;
        }
    }

    return 0;
}

bool proc_should_exit(void)
{
    process_t *p = proc_current();

    return p && p->killed && !p->exited;
}

/* The body both waits share.  Reaping is the same work either way; the only
 * difference is what happens when there is nothing to reap yet. */
static int32_t reap_one(process_t *parent, int32_t pid, int32_t *status,
                        bool *any_children)
{
    *any_children = false;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *c = &processes[i];

        if (!c->used || c->parent != parent)
            continue;
        if (pid > 0 && c->pid != pid)
            continue;

        *any_children = true;

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

    return -W_ECHILD;
}

int32_t proc_reap(int32_t *status)
{
    bool any;
    return reap_one(proc_current(), -1, status, &any);
}

int32_t proc_wait(int32_t pid, int32_t *status)
{
    process_t *parent = proc_current();

    for (;;) {
        bool    any_children;
        int32_t reaped = reap_one(parent, pid, status, &any_children);

        if (reaped >= 0)
            return reaped;
        if (!any_children)
            return -W_ECHILD;

        sched_block(WAIT_CHILD);
    }
}

uint64_t proc_sbrk(int64_t increment)
{
    process_t *p = proc_current();
    uint64_t   old = p->heap_break;

    if (increment == 0)
        return old;

    if (increment > 0) {
        uint64_t new_break = old + (uint64_t)increment;

        /* Do not let the heap grow into the shared memory window, which is
         * what stands between it and the stack. */
        if (new_break > USER_MMAP_BASE)
            return (uint64_t)-1;

        for (uint64_t page = ALIGN_UP(old, PAGE_SIZE);
             page < ALIGN_UP(new_break, PAGE_SIZE); page += PAGE_SIZE) {
            if (paging_translate(p->space, page))
                continue;
            if (!paging_map_alloc(p->space, page, PTE_WRITE | PTE_USER, true))
                return (uint64_t)-1;
        }
        p->heap_break = new_break;
    } else {
        uint64_t shrink = (uint64_t)(-increment);
        if (shrink > old - p->heap_start)
            return (uint64_t)-1;

        uint64_t new_break = old - shrink;
        for (uint64_t page = ALIGN_UP(new_break, PAGE_SIZE);
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
    out->cpu_ticks      = (uint32_t)p->cpu_ticks;
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
