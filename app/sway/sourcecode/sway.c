/* sway: starting up, waiting, and the key that decides everything.
 *
 * The loop is the whole compositor in outline.  Wait on three kinds of
 * descriptor -- the keyboard, the clients, the IPC socket -- act on whichever
 * spoke, and if anything changed, draw a frame.  Nothing is polled and nothing
 * spins: with nobody typing and nothing redrawing, sway uses no processor at
 * all, which on a machine where the compositor is always running matters more
 * than how fast it can draw.
 *
 * The interesting decision is what happens to a keystroke.  A compositor sees
 * every key before any window does, and has to decide, for each one, whether
 * it is an instruction to the compositor or text for the focused window.  sway
 * decides by looking it up in the bindings; anything not bound is forwarded
 * untouched.  That is why $mod exists: it is the modifier that marks a
 * keystroke as being for the compositor, so everything else can belong to the
 * program.
 */

#include "sway.h"
#include <stdarg.h>

struct sway sway;

#define LOG_PATH    "/ramdisk/sway.log"
#define SOCKET_NAME "wayland-0"
#define IPC_PATH    "/ramdisk/sway-ipc.sock"

static int log_fd = -1;

void sway_log(const char *fmt, ...)
{
    char    line[224];
    va_list ap;

    va_start(ap, fmt);
    int n = wvsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);

    if (n < 0 || log_fd < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;

    line[n]     = '\n';
    line[n + 1] = '\0';
    wwrite(log_fd, line, (wsize_t)(n + 1));
}

/* ------------------------------------------------------------------ *
 *  Starting programs
 * ------------------------------------------------------------------ */

/* `exec wlterm --title foo` -- a program and its arguments.
 *
 * A bare name is looked for under /app, the way the shell does it, so a
 * configuration file says `exec wlterm` rather than the full path.  A name
 * with a slash in it is taken as written. */
void sway_spawn(const char *command)
{
    char  buf[COMMAND_MAX];
    char *argv[16];
    int   argc = 0;

    strlcpy(buf, command, sizeof(buf));

    char *at = buf;
    while (*at && argc < 15) {
        while (*at == ' ')
            at++;
        if (!*at)
            break;

        argv[argc++] = at;
        while (*at && *at != ' ')
            at++;
        if (*at)
            *at++ = '\0';
    }
    argv[argc] = NULL;

    if (argc == 0)
        return;

    char path[W_PATH_MAX + 1];
    if (strchr(argv[0], '/'))
        strlcpy(path, argv[0], sizeof(path));
    else
        wsnprintf(path, sizeof(path), "/app/%s/launch", argv[0]);

    int pid = wspawn(path, argv);
    if (pid < 0) {
        sway_log("exec %s: %s", argv[0], wstrerror(-pid));

        /* Said on the bar as well as in the log.  A keybinding that quietly
         * does nothing is the hardest kind of thing to diagnose, and the
         * screen is where somebody pressing the key is looking. */
        wsnprintf(sway.status, sizeof(sway.status), "%s: %s", argv[0],
                  wstrerror(-pid));
        sway.dirty = 1;
        return;
    }

    sway_log("exec %s -> pid %d", argv[0], pid);
}

/* Children that have finished.  A compositor cannot wait for them -- it has
 * clients to serve -- so they are collected whenever it is convenient, which
 * is every time round the loop. */
static void reap_children(void)
{
    while (wreap(NULL) >= 0)
        ;
}

/* ------------------------------------------------------------------ *
 *  Keys
 * ------------------------------------------------------------------ */

static struct binding *find_binding(uint32_t code, uint32_t mods)
{
    /* Caps Lock is a state, not an intention: a binding written for Super+Q
     * should still fire with Caps Lock on. */
    mods &= ~W_MOD_CAPS;

    for (struct binding *b = sway.bindings; b; b = b->next)
        if (b->code == code && (b->mods & ~W_MOD_CAPS) == mods)
            return b;
    return NULL;
}

static void handle_key(const winput_t *ev)
{
    sway.mods = ev->mods;

    if (ev->state) {
        struct binding *b = find_binding(ev->code, ev->mods);

        if (b) {
            /* Taken by the compositor, and not forwarded.  A window must not
             * also receive the Q of Super+Shift+Q that closed it. */
            sway_log("key %u+%x -> %s", ev->code, ev->mods, b->command);
            config_run_command(b->command);
            return;
        }
    }

    /* Not a binding, so it belongs to whatever has the focus.  Modifiers go
     * first: a client works out what a key means from the modifiers it was
     * last told about, so telling it after the key would be a frame late. */
    if (sway.focused) {
        shell_send_modifiers(sway.focused, ev->mods);
        shell_send_key(sway.focused, ev->code, ev->state, ev->time_ms);
    }
}

static void read_input(void)
{
    winput_t events[16];

    int n = wread(sway.input_fd, events, sizeof(events));
    if (n <= 0)
        return;

    for (int i = 0; i < n / (int)sizeof(events[0]); i++)
        handle_key(&events[i]);
}

/* ------------------------------------------------------------------ *
 *  The bar's clock
 * ------------------------------------------------------------------ */

static void update_status(void)
{
    static uint32_t last_minute = 0xFFFFFFFF;
    wtime_t         now;

    if (wtime_get(&now) < 0)
        return;

    uint32_t minute = (uint32_t)(now.hour * 60 + now.minute);
    if (minute == last_minute)
        return;

    last_minute = minute;
    wsnprintf(sway.status, sizeof(sway.status), "%04d-%02d-%02d  %02d:%02d",
              now.year, now.month, now.day, now.hour, now.minute);
    sway.dirty = 1;
}

/* ------------------------------------------------------------------ *
 *  Setting up
 * ------------------------------------------------------------------ */

/* Where the configuration lives, in sway's own order of preference. */
static int find_config(char *out, wsize_t size)
{
    char      home[W_PATH_MAX + 1];
    wstat_t   st;

    int uid = wgetuid();
    wuser_t  user;

    if (wuserinfo(uid, &user) == 0 && user.name[0])
        wsnprintf(home, sizeof(home), "/home/%s", user.name);
    else
        strlcpy(home, "/home/root", sizeof(home));

    wsnprintf(out, size, "%s/.config/sway/config", home);
    if (wstat(out, &st) == 0)
        return 0;

    strlcpy(out, "/etc/sway/config", size);
    if (wstat(out, &st) == 0)
        return 0;

    return -W_ENOENT;
}

static int start_display(void)
{
    sway.display = wl_display_create();
    if (!sway.display) {
        wfprintf(W_STDERR, "sway: out of memory\n");
        return -1;
    }

    int r = wl_display_add_socket(sway.display, SOCKET_NAME);
    if (r < 0) {
        wfprintf(W_STDERR, "sway: cannot listen on %s: %s\n", SOCKET_NAME,
                 wstrerror(-r));

        if (-r == W_EEXIST)
            wfprintf(W_STDERR,
                     "sway: something else is already the display server.\n"
                     "      `systemctl status wayland` -- stop it first.\n");
        return -1;
    }

    return 0;
}

static int take_the_screen(void)
{
    wdisplayinfo(&sway.screen);

    if (!sway.screen.present) {
        wfprintf(W_STDERR, "sway: this machine has no framebuffer -- the "
                           "console is still in text mode,\n"
                           "      and there is nothing to draw windows on.\n");
        return -1;
    }

    int r = wdisplaygrab();
    if (r < 0) {
        wfprintf(W_STDERR, "sway: cannot take the screen: %s\n", wstrerror(-r));
        if (-r == W_EPERM)
            wfprintf(W_STDERR, "sway: the screen belongs to everybody, so "
                               "taking it needs root.\n");
        else if (-r == W_EBUSY)
            wfprintf(W_STDERR, "sway: process %u already has it.\n",
                     sway.screen.owner);
        return -1;
    }

    sway.back = malloc((wsize_t)sway.screen.width * sway.screen.height * 4);
    if (!sway.back) {
        wfprintf(W_STDERR, "sway: not enough memory for a %ux%u screen\n",
                 sway.screen.width, sway.screen.height);
        wdisplaydrop();
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    log_fd = wopen(LOG_PATH, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);

    config_defaults();

    if (start_display() < 0)
        return 1;

    if (take_the_screen() < 0)
        return 1;

    /* The keyboard, after the screen: if the screen could not be had there is
     * no reason to take the keyboard away from the console as well. */
    sway.input_fd = winputopen();
    if (sway.input_fd < 0) {
        wfprintf(W_STDERR, "sway: cannot take the keyboard: %s\n",
                 wstrerror(-sway.input_fd));
        wdisplaydrop();
        return 1;
    }

    sway.usable_y = 0;
    sway.usable_h = (int)sway.screen.height - (sway.config.bar ? 20 : 0);

    shell_init();
    layout_init();
    ipc_init(IPC_PATH);

    sway_log("sway on a %ux%u screen", sway.screen.width, sway.screen.height);

    /* The configuration comes after the globals exist, because it may contain
     * `exec` lines that start clients which will connect immediately. */
    if (find_config(sway.config_path, sizeof(sway.config_path)) == 0) {
        config_load(sway.config_path);
    } else {
        sway_log("no configuration file; using the built-in defaults");
        wsnprintf(sway.config_path, sizeof(sway.config_path),
                  "/home/root/.config/sway/config");

        /* Enough to be usable with no file at all, so a broken configuration
         * is never a machine you cannot open a window on. */
        config_run_command("bindsym Mod4+Return exec wlterm");
        config_run_command("bindsym Mod4+Shift+q kill");
        config_run_command("bindsym Mod4+Shift+e exit");
    }

    /* The bar height depends on nothing the configuration can change yet, but
     * `bar` itself can be turned off, so this is settled after reading it. */
    sway.usable_h = (int)sway.screen.height - (sway.config.bar ? 20 : 0);
    layout_arrange();

    sway.running = 1;
    sway.dirty   = 1;

    while (sway.running) {
        wpollfd_t watch[W_POLL_MAX];
        int       n = 0;

        watch[n].fd      = sway.input_fd;
        watch[n].events  = W_POLLIN;
        watch[n].revents = 0;
        n++;

        ipc_poll_fds(watch, &n, W_POLL_MAX);
        n += wl_display_poll_fds(sway.display, watch + n, W_POLL_MAX - n);

        /* A second at most, so the clock on the bar stays right and a service
         * asked to stop is noticed even with nothing happening. */
        int ready = wpoll(watch, n, 1000);

        if (ready > 0) {
            if (watch[0].revents & W_POLLIN)
                read_input();

            ipc_handle(watch, n);
            wl_display_handle(sway.display, watch, n);
        }

        reap_children();
        update_status();

        if (sway.dirty)
            render_frame();

        /* Answered whether or not a frame was drawn: a client that draws in a
         * frame callback and is not given one stops drawing for good. */
        shell_frame_done();
        wl_display_flush_clients(sway.display);
    }

    sway_log("leaving");

    ipc_shutdown();
    wl_display_destroy(sway.display);
    wclose(sway.input_fd);
    wdisplaydrop();

    /* The console repaints itself, and everything printed behind sway is
     * there. */
    wprintf("sway: the screen is yours again\n");
    return 0;
}
