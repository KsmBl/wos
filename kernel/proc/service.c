/* Services.  See service.h for the model. */

#include "service.h"
#include "proc.h"
#include "sched.h"
#include "pit.h"
#include "wfs_kernel.h"
#include "kprintf.h"
#include "string.h"

#define SERVICE_DIR "/services"

/* A unit file is a handful of short lines; anything larger is not one. */
#define UNIT_MAX 512

struct service {
    bool     used;
    char     name[28];
    char     exec[96];
    char     description[64];
    bool     enabled;
    int32_t  pid;              /* 0 when not running */
    int32_t  exit_status;
    bool     ever_ran;
};

static struct service services[W_SERVICE_MAX];
static int            count;

/* ------------------------------------------------------------------ *
 *  Unit files
 * ------------------------------------------------------------------ */

static struct service *find(const char *name)
{
    for (int i = 0; i < count; i++)
        if (services[i].used && strcmp(services[i].name, name) == 0)
            return &services[i];
    return NULL;
}

/* Build the path of a service's unit file. */
static void unit_path(const char *name, char *out, size_t cap)
{
    strlcpy(out, SERVICE_DIR "/", cap);

    size_t at = strlen(out);
    if (at < cap)
        strlcpy(out + at, name, cap - at);
}

/* Take one "key=value" line.  Anything else -- a blank line, a comment, a key
 * this kernel does not know -- is skipped rather than refused: a unit file
 * written for a later version should still start its service. */
static void parse_line(struct service *s, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0' || *line == '#')
        return;

    char *eq = strchr(line, '=');
    if (!eq)
        return;

    *eq = '\0';
    const char *key   = line;
    const char *value = eq + 1;

    if (strcmp(key, "exec") == 0)
        strlcpy(s->exec, value, sizeof(s->exec));
    else if (strcmp(key, "description") == 0)
        strlcpy(s->description, value, sizeof(s->description));
    else if (strcmp(key, "enabled") == 0)
        s->enabled = (value[0] == '1' || value[0] == 'y' || value[0] == 't');
}

static bool read_unit(const char *name, struct service *out)
{
    char path[W_PATH_MAX + 1];
    unit_path(name, path, sizeof(path));

    uint32_t ino;
    if (wfs_lookup(path, &ino) != 0)
        return false;

    struct wfs_inode in;
    if (wfs_read_inode(ino, &in) != 0 || in.type != WFS_TYPE_FILE)
        return false;

    char     text[UNIT_MAX];
    uint32_t want = in.size < sizeof(text) - 1 ? in.size : sizeof(text) - 1;
    int      got  = want ? wfs_read(ino, 0, text, want) : 0;

    if (got < 0)
        return false;
    text[got] = '\0';

    memset(out, 0, sizeof(*out));
    strlcpy(out->name, name, sizeof(out->name));

    char *line = text;
    for (int i = 0; i <= got; i++) {
        if (text[i] == '\n' || text[i] == '\0') {
            char saved = text[i];
            text[i] = '\0';
            parse_line(out, line);
            text[i] = saved;
            line = text + i + 1;
        }
    }

    out->used = out->exec[0] != '\0';
    return out->used;
}

/* Write a unit file back from what is in memory.  Only enable and disable do
 * this, so what is written is what was read plus the one changed line. */
static int write_unit(const struct service *s)
{
    char path[W_PATH_MAX + 1];
    unit_path(s->name, path, sizeof(path));

    char text[UNIT_MAX];
    int  at = 0;

    /* No snprintf in the kernel, so it is built by hand -- three fixed lines
     * in a fixed order. */
    const char *parts[6] = {
        "description=", s->description,
        "\nexec=",      s->exec,
        "\nenabled=",   s->enabled ? "1\n" : "0\n",
    };

    for (int i = 0; i < 6; i++) {
        int n = (int)strlen(parts[i]);
        if (at + n >= (int)sizeof(text))
            return -W_EFBIG;
        memcpy(text + at, parts[i], (size_t)n);
        at += n;
    }

    uint32_t ino;
    if (wfs_lookup(path, &ino) != 0) {
        int r = wfs_create(path, WFS_TYPE_FILE, &ino);
        if (r < 0)
            return r;
    } else {
        wfs_truncate(ino);
    }

    int written = wfs_write(ino, 0, text, (uint32_t)at);
    return written == at ? 0 : -W_EIO;
}

/* ------------------------------------------------------------------ *
 *  Starting and stopping
 * ------------------------------------------------------------------ */

/* Has this service's process gone?  A pid is reused eventually, so the answer
 * has to come from the process table rather than from a guess. */
static void refresh(struct service *s)
{
    if (!s->pid)
        return;

    process_t *p = proc_by_pid(s->pid);
    if (!p || p->exited) {
        if (p)
            s->exit_status = p->exit_status;
        s->pid = 0;
    }
}

static int start(struct service *s)
{
    refresh(s);
    if (s->pid)
        return -W_EBUSY;

    uint32_t ino;
    if (wfs_lookup(s->exec, &ino) != 0)
        return -W_ENOENT;

    char *argv[] = { s->name, NULL };

    /* No parent: a service outlives whoever asked for it, and a shell that
     * exits must not take the display server with it. */
    int32_t pid = proc_spawn(s->exec, argv, NULL);
    if (pid < 0)
        return pid;

    s->pid      = pid;
    s->ever_ran = true;
    return 0;
}

/* Wait for a process that has been asked to stop to actually go.
 *
 * proc_kill() only marks it; it leaves at its next safe moment, which for
 * anything that waits is the next time it is scheduled.  Something has to wait
 * for that, or a restart would start the replacement while the original is
 * still holding the socket -- and the answer to "is it running" immediately
 * after a stop would be yes.
 *
 * Bounded, because a process that never reaches a safe moment must not take
 * the caller down with it.  It stays marked either way and goes when it can. */
static void wait_for_exit(struct service *s)
{
    for (int i = 0; i < 200 && s->pid; i++) {      /* up to two seconds */
        sched_sleep_until(pit_ticks() + 1);
        refresh(s);
    }
}

static int stop(struct service *s)
{
    refresh(s);
    if (!s->pid)
        return -W_EBUSY;

    int r = proc_kill(s->pid);
    if (r < 0)
        return r;

    wait_for_exit(s);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  What the rest of the kernel asks for
 * ------------------------------------------------------------------ */

int service_control(uint32_t action, const char *name)
{
    struct service *s = find(name);
    if (!s)
        return -W_ENOENT;

    switch (action) {
    case W_SVC_START:
        return start(s);

    case W_SVC_STOP:
        return stop(s);

    case W_SVC_RESTART: {
        int r = stop(s);
        if (r < 0 && r != -W_EBUSY)   /* not running is fine for a restart */
            return r;
        return start(s);
    }

    case W_SVC_ENABLE:
    case W_SVC_DISABLE:
        s->enabled = (action == W_SVC_ENABLE);
        return write_unit(s);

    default:
        return -W_EINVAL;
    }
}

int service_list(wservice_t *out, int max)
{
    int n = 0;

    for (int i = 0; i < count && n < max; i++) {
        struct service *s = &services[i];
        if (!s->used)
            continue;

        refresh(s);

        memset(&out[n], 0, sizeof(out[n]));
        strlcpy(out[n].name, s->name, sizeof(out[n].name));
        strlcpy(out[n].exec, s->exec, sizeof(out[n].exec));
        strlcpy(out[n].description, s->description, sizeof(out[n].description));
        out[n].enabled     = s->enabled ? 1 : 0;
        out[n].running     = s->pid ? 1 : 0;
        out[n].pid         = s->pid;
        out[n].exit_status = s->ever_ran ? s->exit_status : 0;
        n++;
    }

    return n;
}

void service_reap(int32_t pid, int32_t status)
{
    for (int i = 0; i < count; i++)
        if (services[i].used && services[i].pid == pid) {
            services[i].pid         = 0;
            services[i].exit_status = status;
            return;
        }
}

void service_init(void)
{
    uint32_t dir;

    memset(services, 0, sizeof(services));
    count = 0;

    if (wfs_lookup(SERVICE_DIR, &dir) != 0)
        return;                       /* a system with no services is fine */

    for (uint32_t i = 0; count < W_SERVICE_MAX; i++) {
        wdirent_t e;

        if (wfs_readdir(dir, i, &e) != 1)
            break;
        if (e.type != W_FT_FILE || e.name[0] == '.')
            continue;

        if (read_unit(e.name, &services[count]))
            count++;
    }

    for (int i = 0; i < count; i++)
        if (services[i].enabled) {
            int r = start(&services[i]);
            if (r < 0)
                kprintf("service: %s did not start (%d)\n",
                        services[i].name, -r);
        }
}

void service_print_report(void)
{
    int running = 0;

    for (int i = 0; i < count; i++)
        if (services[i].pid)
            running++;

    if (count == 0) {
        kputs("service: none described in " SERVICE_DIR "\n");
        return;
    }

    kprintf("service: %d described, %d started\n", count, running);

    /* kprintf pads numbers, not strings, so the columns are made by hand. */
    for (int i = 0; i < count; i++) {
        const char *state = services[i].pid ? "running"
                          : services[i].enabled ? "failed to start" : "stopped";

        kprintf("service:   %s", services[i].name);
        for (size_t pad = strlen(services[i].name); pad < 12; pad++)
            kputc(' ');
        kprintf("%s -- %s\n", state, services[i].description);
    }
}
