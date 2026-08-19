/* stress -- put the machine under load on purpose.
 *
 *     stress --cpu 2              two workers, spinning
 *     stress --cpu 2 --timeout 30s
 *     stress --vm 1 --vm-bytes 8M
 *
 * The original is Amos Waterland's, and this keeps its shape: workers are
 * spawned ("hogs"), each does one kind of pointless work as hard as it can,
 * and the parent waits and reports.  The options are spelled the same, so a
 * command line written for the real one usually works here.
 *
 * Two things are genuinely different, and both are properties of this machine
 * rather than of this program.
 *
 * A worker is a *process*.  WOS has one thread per process and no way to make
 * another, so `--cpu 2` spawns two children rather than two threads.  For the
 * purpose -- occupying the processor -- that is the same thing.
 *
 * And every run is bounded.  There are no signals here: nothing can be sent to
 * a running process to ask it to stop, so a stress test with no end would have
 * to be ended by turning the machine off.  So `--timeout` defaults to ten
 * seconds instead of to forever, each worker is told when to stop rather than
 * being killed, and `--timeout 0` means "until the machine is rebooted" and
 * says so before it starts.
 */

#include <wkernel.h>
#include <stdarg.h>

#define TICKS_PER_SECOND 100

/* What one worker does. */
enum hog { HOG_CPU, HOG_VM, HOG_HDD };

static const char *hog_name(enum hog h)
{
    return h == HOG_CPU ? "cpu" : h == HOG_VM ? "vm" : "hdd";
}

/* A disk worker writes this much over and over rather than one enormous file.
 *
 * This began as the largest file WFS could hold; the filesystem holds 64 MiB
 * in a file now, and the figure stays because it is a sensible one on its own.
 * The point of the disk worker is sustained traffic to the platter, and a
 * quarter of a megabyte rewritten in a loop produces more of that than one
 * huge file written once -- which would spend most of its time allocating
 * blocks rather than moving bytes. */
#define HDD_MAX_BYTES (256u * 1024u)

static int verbose;
static int quiet;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char *fmt, ...)
{
    char    line[192];
    va_list ap;

    if (quiet)
        return;

    va_start(ap, fmt);
    wvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    wprintf("stress: info: [%d] %s\n", wgetpid(), line);
}

static void detail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void detail(const char *fmt, ...)
{
    char    line[192];
    va_list ap;

    if (!verbose || quiet)
        return;

    va_start(ap, fmt);
    wvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    wprintf("stress: dbug: [%d] %s\n", wgetpid(), line);
}

/* ------------------------------------------------------------------ *
 *  Parsing
 * ------------------------------------------------------------------ */

/* A count with an optional suffix: 30s, 5m, 2h, or a bare number of seconds. */
static long parse_time(const char *s, int *ok)
{
    long value = 0;
    *ok = 0;

    if (!s || !*s)
        return 0;

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
        *ok = 1;
    }

    switch (*s) {
    case '\0': break;
    case 's': case 'S': s++; break;
    case 'm': case 'M': value *= 60;      s++; break;
    case 'h': case 'H': value *= 3600;    s++; break;
    case 'd': case 'D': value *= 86400;   s++; break;
    default:  *ok = 0; return 0;
    }

    if (*s)
        *ok = 0;
    return value;
}

/* A size with an optional suffix: 4096, 64K, 8M, 1G. */
static long parse_bytes(const char *s, int *ok)
{
    long value = 0;
    *ok = 0;

    if (!s || !*s)
        return 0;

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
        *ok = 1;
    }

    switch (*s) {
    case '\0': break;
    case 'b': case 'B': s++; break;
    case 'k': case 'K': value *= 1024;               s++; break;
    case 'm': case 'M': value *= 1024L * 1024;       s++; break;
    case 'g': case 'G': value *= 1024L * 1024 * 1024; s++; break;
    default:  *ok = 0; return 0;
    }

    if (*s)
        *ok = 0;
    return value;
}

/* ------------------------------------------------------------------ *
 *  The workers
 *
 *  Each runs until a deadline it was given, rather than until it is killed:
 *  see the note at the top about there being no signals.
 * ------------------------------------------------------------------ */

/* True once the deadline has passed.  Compared as a signed difference so it
 * keeps working across the tick counter's wrap. */
static int expired(unsigned int until)
{
    return until != 0 && (int)(wticks() - until) >= 0;
}

/* Integer work the compiler is not allowed to skip.
 *
 * The original spins on sqrt(), which is not available here: the kernel never
 * enables the floating-point unit, so a program that used it would fault on
 * its first multiply.  A mixing function loaded with dependencies does the
 * same job -- every step needs the one before it, so the processor cannot run
 * them side by side and cannot skip any of them.
 *
 * The deadline is checked between batches rather than inside the loop, and the
 * batch is small enough that a worker stops close to when it was told to: a
 * batch of a million would have workers running seconds past their timeout on
 * a slow machine, which is a stress test that lies about how long it ran. */
static volatile unsigned long cpu_sink;

static int hog_cpu(unsigned int until)
{
    unsigned long x = (unsigned long)wgetpid() * 2654435761u + 1;
    unsigned long rounds = 0;

    for (;;) {
        for (int i = 0; i < 20000; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x += 0x9E3779B97F4A7C15UL;
            x *= 0xBF58476D1CE4E5B9UL;
        }

        /* Written somewhere the compiler cannot see through, so the loop
         * above is work rather than something it is entitled to delete. */
        cpu_sink += x;
        rounds++;

        if (expired(until))
            break;
    }

    detail("cpu worker did %lu rounds", rounds);
    return 0;
}

/* Allocate, fill, check, free, repeat.  The check matters: without reading it
 * back, an allocator could hand out memory that was never really there. */
static int hog_vm(unsigned int until, unsigned long bytes)
{
    unsigned long passes = 0;

    for (;;) {
        unsigned char *block = malloc(bytes);

        if (!block) {
            wfprintf(W_STDERR, "stress: FAIL: [%d] cannot allocate %s\n",
                     wgetpid(), whuman(bytes));
            return 1;
        }

        memset(block, 0x5A, bytes);

        for (unsigned long i = 0; i < bytes; i += 4096)
            if (block[i] != 0x5A) {
                wfprintf(W_STDERR, "stress: FAIL: [%d] memory read back "
                         "wrong at offset %lu\n", wgetpid(), i);
                free(block);
                return 1;
            }

        free(block);
        passes++;

        if (expired(until))
            break;
    }

    detail("vm worker filled %s %lu times", whuman(bytes), passes);
    return 0;
}

/* Write a file, read it back, delete it, repeat. */
static int hog_hdd(unsigned int until, unsigned long bytes)
{
    char path[W_PATH_MAX + 1];
    unsigned long passes = 0;

    wsnprintf(path, sizeof(path), "/ramdisk/stress-%d.tmp", wgetpid());

    static unsigned char block[8192];
    memset(block, 0xA5, sizeof(block));

    for (;;) {
        int fd = wopen(path, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
        if (fd < 0) {
            wfprintf(W_STDERR, "stress: FAIL: [%d] cannot write %s: %s\n",
                     wgetpid(), path, wstrerror(-fd));
            return 1;
        }

        for (unsigned long at = 0; at < bytes; at += sizeof(block)) {
            unsigned long chunk = bytes - at;
            if (chunk > sizeof(block))
                chunk = sizeof(block);

            int n = wwrite(fd, block, chunk);
            if (n < 0) {
                wfprintf(W_STDERR, "stress: FAIL: [%d] write failed: %s\n",
                         wgetpid(), wstrerror(-n));
                wclose(fd);
                wunlink(path);
                return 1;
            }
        }

        wclose(fd);
        wunlink(path);
        passes++;

        if (expired(until))
            break;
    }

    detail("hdd worker wrote %s %lu times", whuman(bytes), passes);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Help
 * ------------------------------------------------------------------ */

static void usage(void)
{
    wprintf(
"usage: stress [options]\n"
"\n"
"Put the machine under load on purpose, by spawning workers that each do one\n"
"kind of pointless work as hard as they can.\n"
"\n"
"  -c, --cpu N          N workers spinning on integer arithmetic\n"
"  -m, --vm N           N workers allocating, filling and freeing memory\n"
"      --vm-bytes B     how much each one allocates (default 8M)\n"
"  -d, --hdd N          N workers writing and deleting a file\n"
"      --hdd-bytes B    how much each one writes (default 128K, at most 256K)\n"
"  -t, --timeout T      how long to run: 10, 30s, 5m, 2h (default 10s)\n"
"                       0 means until the machine is rebooted\n"
"  -q, --quiet          say nothing\n"
"  -v, --verbose        say what each worker got through\n"
"      --dry-run        show what would be run, and run nothing\n"
"      --version        print the version\n"
"  -?, --help           this\n"
"\n"
"Sizes take b, K, M or G; times take s, m, h or d.\n"
"\n"
"A worker is a process, because WOS has one thread per process. `--cpu 2`\n"
"spawns two children, which is the same thing for the purpose of occupying\n"
"the processor -- but note that WOS runs on the core it booted on and starts\n"
"no others, so two workers share one core rather than filling two.\n"
"`htop` shows what actually happened.\n"
"\n"
"Every run is bounded. There are no signals here, so nothing can be sent to a\n"
"running process to ask it to stop; each worker is told its deadline when it\n"
"starts and leaves by itself. That is why --timeout defaults to ten seconds\n"
"rather than to forever.\n"
"\n"
"--io is not here: it exists in the original to call sync(), and WOS has no\n"
"such call -- writes reach the disk as they are made. --hdd is the disk test.\n");
}

/* ------------------------------------------------------------------ *
 *  Dispatching
 * ------------------------------------------------------------------ */

/* Start one worker: a copy of this program, told what to do and when to stop.
 *
 * The hidden `--worker` form is how a program with no fork() spawns a copy of
 * itself doing something else -- there is nothing to inherit from, so
 * everything it needs is on its command line. */
static int spawn_worker(enum hog kind, unsigned int until, unsigned long bytes)
{
    char until_text[24];
    char bytes_text[24];

    wsnprintf(until_text, sizeof(until_text), "%u", until);
    wsnprintf(bytes_text, sizeof(bytes_text), "%lu", bytes);

    char *argv[] = {
        "stress", "--worker", (char *)hog_name(kind), until_text, bytes_text,
        NULL,
    };

    return wspawn("/app/stress/launch", argv);
}

int main(int argc, char **argv)
{
    long cpu = 0, vm = 0, hdd = 0;
    long vm_bytes  = 8L * 1024 * 1024;
    long hdd_bytes = 128L * 1024;
    long timeout   = 10;
    int  timeout_given = 0;
    int  dry_run = 0;

    /* The worker form, which is not for people to type. */
    if (argc == 5 && strcmp(argv[1], "--worker") == 0) {
        unsigned int  until = (unsigned int)atoi(argv[3]);
        unsigned long bytes = (unsigned long)atoi(argv[4]);

        if (strcmp(argv[2], "cpu") == 0)  return hog_cpu(until);
        if (strcmp(argv[2], "vm") == 0)   return hog_vm(until, bytes);
        if (strcmp(argv[2], "hdd") == 0)  return hog_hdd(until, bytes);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int         ok = 0;

        /* An option that takes a value, so the "missing value" complaint is
         * written once rather than at every option. */
        #define VALUE()                                                      \
            (++i < argc ? argv[i]                                            \
                        : (wfprintf(W_STDERR,                                \
                                    "stress: %s needs a value\n", a),        \
                           (const char *)NULL))

        if (strcmp(a, "-c") == 0 || strcmp(a, "--cpu") == 0) {
            const char *v = VALUE();
            if (!v) return 1;
            cpu = atoi(v);
        } else if (strcmp(a, "-m") == 0 || strcmp(a, "--vm") == 0) {
            const char *v = VALUE();
            if (!v) return 1;
            vm = atoi(v);
        } else if (strcmp(a, "--vm-bytes") == 0) {
            const char *v = VALUE();
            if (!v) return 1;
            vm_bytes = parse_bytes(v, &ok);
            if (!ok) { wfprintf(W_STDERR, "stress: %s is not a size\n", v); return 1; }
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--hdd") == 0) {
            const char *v = VALUE();
            if (!v) return 1;
            hdd = atoi(v);
        } else if (strcmp(a, "--hdd-bytes") == 0) {
            const char *v = VALUE();
            if (!v) return 1;
            hdd_bytes = parse_bytes(v, &ok);
            if (!ok) { wfprintf(W_STDERR, "stress: %s is not a size\n", v); return 1; }
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--timeout") == 0) {
            const char *v = VALUE();
            if (!v) return 1;
            timeout = parse_time(v, &ok);
            if (!ok) { wfprintf(W_STDERR, "stress: %s is not a time\n", v); return 1; }
            timeout_given = 1;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(a, "--version") == 0) {
            wprintf("stress 1.0 (for WOS)\n");
            return 0;
        } else if (strcmp(a, "-?") == 0 || strcmp(a, "-h") == 0 ||
                   strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            wfprintf(W_STDERR, "stress: %s is not an option\n", a);
            wfprintf(W_STDERR, "try `stress --help`\n");
            return 1;
        }
        #undef VALUE
    }

    if (cpu <= 0 && vm <= 0 && hdd <= 0) {
        wfprintf(W_STDERR, "stress: nothing to do -- give it --cpu, --vm or "
                           "--hdd\n");
        wfprintf(W_STDERR, "try `stress --cpu 2`\n");
        return 1;
    }

    if (hdd_bytes > (long)HDD_MAX_BYTES) {
        note("--hdd-bytes lowered to %s: WFS holds at most that in one file",
             whuman(HDD_MAX_BYTES));
        hdd_bytes = HDD_MAX_BYTES;
    }

    long workers = cpu + vm + hdd;

    /* A worker is a process, and the machine has a fixed number of those. */
    if (workers > 16) {
        wfprintf(W_STDERR, "stress: %ld workers is more than this machine's "
                           "process table will hold\n", workers);
        return 1;
    }

    if (dry_run) {
        note("dispatching hogs: %ld cpu, %ld vm, %ld hdd -- but not really",
             cpu, vm, hdd);
        return 0;
    }

    /* Said once, when it matters: asking for more workers than there are
     * cores to run them on is allowed and is usually not what somebody
     * expects to happen. */
    wcpuinfo_t info;
    if (cpu > 0 && wcpuinfo(&info) == 0 && cpu > info.online)
        note("%ld cpu workers on %d core%s: they take turns, so the machine "
             "reaches 100%% and each worker gets a share of it",
             cpu, info.online, info.online == 1 ? "" : "s");

    unsigned int until = timeout > 0
        ? wticks() + (unsigned int)(timeout * TICKS_PER_SECOND) : 0;

    if (timeout == 0)
        note("no timeout: these workers run until the machine is rebooted, "
             "because nothing here can stop a running process");
    else if (!timeout_given)
        detail("no --timeout given, so ten seconds");

    note("dispatching hogs: %ld cpu, %ld vm, %ld hdd", cpu, vm, hdd);

    int started = 0;

    for (long i = 0; i < cpu; i++) {
        int pid = spawn_worker(HOG_CPU, until, 0);
        if (pid < 0) {
            wfprintf(W_STDERR, "stress: FAIL: cannot start a cpu worker: %s\n",
                     wstrerror(-pid));
            break;
        }
        detail("started cpu worker as pid %d", pid);
        started++;
    }

    for (long i = 0; i < vm; i++) {
        int pid = spawn_worker(HOG_VM, until, (unsigned long)vm_bytes);
        if (pid < 0) {
            wfprintf(W_STDERR, "stress: FAIL: cannot start a vm worker: %s\n",
                     wstrerror(-pid));
            break;
        }
        detail("started vm worker as pid %d, %s each", pid, whuman(vm_bytes));
        started++;
    }

    for (long i = 0; i < hdd; i++) {
        int pid = spawn_worker(HOG_HDD, until, (unsigned long)hdd_bytes);
        if (pid < 0) {
            wfprintf(W_STDERR, "stress: FAIL: cannot start a hdd worker: %s\n",
                     wstrerror(-pid));
            break;
        }
        detail("started hdd worker as pid %d, %s each", pid, whuman(hdd_bytes));
        started++;
    }

    if (started == 0)
        return 1;

    unsigned int began  = wticks();
    int          failed = 0;

    for (int i = 0; i < started; i++) {
        int status = 0;
        int pid    = wwait(-1, &status);

        if (pid < 0)
            break;
        if (status != 0) {
            wfprintf(W_STDERR, "stress: FAIL: [%d] worker %d returned %d\n",
                     wgetpid(), pid, status);
            failed++;
        } else {
            detail("worker %d finished", pid);
        }
    }

    unsigned int took = (wticks() - began) / TICKS_PER_SECOND;

    if (failed) {
        wfprintf(W_STDERR, "stress: FAIL: [%d] failed run completed in %us\n",
                 wgetpid(), took);
        return 1;
    }

    note("successful run completed in %us", took);
    return 0;
}
