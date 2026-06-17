/* systemctl -- see what the system is running, and change it.
 *
 *   systemctl                    list every service
 *   systemctl status [name]      one service in full, or all of them
 *   systemctl start   <name>     run it now
 *   systemctl stop    <name>     ask it to leave
 *   systemctl restart <name>
 *   systemctl enable  <name>     run it at the next boot
 *   systemctl disable <name>     do not
 *
 * A service is described by a file in /services and started by the kernel, so
 * it belongs to the machine rather than to whoever started it: closing the
 * shell does not take it down.
 *
 * Enabling is not starting.  "Should this run at the next boot" and "is it
 * running now" are different questions, and a manager that answered one with
 * the other would be deciding things nobody asked it to.
 *
 * Changing anything needs root or the systemctleditor role; looking needs
 * nothing.
 */

#include <wkernel.h>

static wservice_t services[W_SERVICE_MAX];
static int        count;

static void usage(void)
{
    wfprintf(W_STDERR,
             "usage: systemctl [list | status [name] | start <name> | "
             "stop <name> |\n"
             "                  restart <name> | enable <name> | "
             "disable <name>]\n\n");
    wfprintf(W_STDERR, "  with no arguments, lists every service\n");
    wfprintf(W_STDERR,
             "\nStarting, stopping and enabling need root or the "
             "systemctleditor role.\n");
}

static int load(void)
{
    count = wservicelist(services, W_SERVICE_MAX);
    if (count < 0) {
        wfprintf(W_STDERR, "systemctl: %s\n", wstrerror(-count));
        return -1;
    }
    return 0;
}

static const wservice_t *find(const char *name)
{
    for (int i = 0; i < count; i++)
        if (strcmp(services[i].name, name) == 0)
            return &services[i];
    return NULL;
}

/* Green for running, red for a service that should be running and is not, and
 * plain for one nobody asked to run. */
static void print_state(const wservice_t *s)
{
    if (s->running) {
        wcolor(W_GREEN | W_BRIGHT, W_DEFAULT);
        wprintf("%-9s", "running");
    } else if (s->enabled) {
        wcolor(W_RED | W_BRIGHT, W_DEFAULT);
        wprintf("%-9s", "stopped");
    } else {
        wprintf("%-9s", "stopped");
    }
    wcolor_reset();
}

static int list(void)
{
    if (count == 0) {
        wprintf("No services are described in /services.\n");
        return 0;
    }

    wprintf("%-12s %-9s %-9s %5s  %s\n",
            "SERVICE", "STATE", "AT BOOT", "PID", "DESCRIPTION");

    for (int i = 0; i < count; i++) {
        const wservice_t *s = &services[i];

        wprintf("%-12s ", s->name);
        print_state(s);
        wprintf("%-9s ", s->enabled ? "enabled" : "disabled");

        if (s->pid)
            wprintf("%5d  ", s->pid);
        else
            wprintf("%5s  ", "-");

        wprintf("%s\n", s->description);
    }

    return 0;
}

static void status_of(const wservice_t *s)
{
    wprintf("%s", s->name);
    if (s->description[0])
        wprintf(" -- %s", s->description);
    wprintf("\n");

    wprintf("   state: ");
    print_state(s);
    if (s->running)
        wprintf(" as pid %d", s->pid);
    else if (s->exit_status)
        wprintf(" (last exited with %d)", s->exit_status);
    wprintf("\n");

    wprintf(" at boot: %s\n", s->enabled ? "enabled" : "disabled");
    wprintf("    runs: %s\n", s->exec);
}

static int status(const char *name)
{
    if (!name) {
        for (int i = 0; i < count; i++) {
            if (i)
                wprintf("\n");
            status_of(&services[i]);
        }
        return 0;
    }

    const wservice_t *s = find(name);
    if (!s) {
        wfprintf(W_STDERR, "systemctl: no service called %s\n", name);
        return 1;
    }

    status_of(s);
    return 0;
}

/* One of the actions, and what to say about how it went. */
static int act(int action, const char *name, const char *done)
{
    int r = wservicectl(action, name);

    if (r == 0) {
        wprintf("%s %s\n", done, name);
        return 0;
    }

    switch (-r) {
    case W_EPERM:
        wfprintf(W_STDERR, "systemctl: not permitted -- needs root or the "
                           "systemctleditor role\n");
        break;
    case W_ENOENT:
        wfprintf(W_STDERR, "systemctl: no service called %s "
                           "(or its program is missing)\n", name);
        break;
    case W_EBUSY:
        wfprintf(W_STDERR, "systemctl: %s is already in that state\n", name);
        break;
    default:
        wfprintf(W_STDERR, "systemctl: %s: %s\n", name, wstrerror(-r));
        break;
    }

    return 1;
}

int main(int argc, char **argv)
{
    if (load() < 0)
        return 1;

    if (argc < 2 || strcmp(argv[1], "list") == 0)
        return list();

    if (strcmp(argv[1], "status") == 0)
        return status(argc > 2 ? argv[2] : NULL);

    if (argc < 3) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "start") == 0)
        return act(W_SVC_START, argv[2], "started");
    if (strcmp(argv[1], "stop") == 0)
        return act(W_SVC_STOP, argv[2], "stopping");
    if (strcmp(argv[1], "restart") == 0)
        return act(W_SVC_RESTART, argv[2], "restarted");
    if (strcmp(argv[1], "enable") == 0)
        return act(W_SVC_ENABLE, argv[2], "enabled at boot:");
    if (strcmp(argv[1], "disable") == 0)
        return act(W_SVC_DISABLE, argv[2], "disabled at boot:");

    usage();
    return 1;
}
