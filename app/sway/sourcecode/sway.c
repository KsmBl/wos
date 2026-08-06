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
#include <wstatus.h>
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
    uint32_t was = sway.mods;

    sway.mods = ev->mods;

    /* A bar that hides appears while the modifier is held, so the modifier
     * going down or coming up is a reason to redraw even though no binding
     * matched and no window was told anything. */
    if (sway.config.bar == BAR_HIDE &&
        ((was ^ ev->mods) & sway.config.mod))
        sway.dirty = 1;

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

/* ------------------------------------------------------------------ *
 *  The pointer
 * ------------------------------------------------------------------ */

/* Which view is under a point on the screen.
 *
 * Walked front to back, which on a tiling compositor is the same as any order
 * -- windows do not overlap -- except for a fullscreen one, which covers the
 * workspace and is therefore checked first.  The rectangle is the frame,
 * title bar and border included, because clicking a title bar is clicking the
 * window. */
static struct view *view_at(int x, int y)
{
    struct workspace *ws = ws_current();

    if (ws->fullscreen)
        return ws->fullscreen->mapped ? ws->fullscreen : NULL;

    for (struct view *v = sway.views; v; v = v->next) {
        if (!v->mapped || v->node == NULL)
            continue;

        /* A view on another workspace is laid out but not on screen. */
        if (layout_workspace_of(v) != sway.current)
            continue;

        if (x >= v->x && x < v->x + v->w && y >= v->y && y < v->y + v->h)
            return v;
    }

    return NULL;
}

/* The client's own top-left corner, which is inside the decoration the
 * compositor drew.  A client is never told about its frame, so a coordinate
 * measured from the frame would be off by the border and the title bar --
 * which is exactly the kind of error that puts a text cursor one line out. */
static void surface_local(struct view *v, int x, int y, int *sx, int *sy)
{
    *sx = x - (v->x + sway.config.border_width);
    *sy = y - (v->y + sway.config.title_height + sway.config.border_width);
}

/* A click on the bar: the workspace blocks are the only part of it that does
 * anything, and they are laid out here the same way draw_bar() lays them out.
 * Non-zero when the click was the bar's, whether or not it hit a block -- a
 * click on the empty half of the bar must not also reach the window under it. */
static int bar_click(int x, int y)
{
    /* Only where it can be seen.  A hidden bar is not a strip of screen that
     * swallows clicks, and one that is showing because the modifier is held is
     * as clickable as a docked one. */
    if (!sway_bar_showing())
        return 0;

    int bar_y = sway.config.bar_top ? 0 : (int)sway.screen.height - BAR_HEIGHT;

    if (y < bar_y || y >= bar_y + BAR_HEIGHT)
        return 0;

    int at = 0;

    for (int i = 0; i < MAX_WORKSPACES; i++) {
        struct workspace *ws = &sway.workspaces[i];

        if (i != sway.current && layout_view_count(ws) == 0)
            continue;

        int width = render_text_width(ws->name) + 16;

        if (x >= at && x < at + width) {
            layout_switch_workspace(i + 1);
            return 1;
        }

        at += width;
    }

    return 1;
}

/* Where the pointer is now, and what that means for who has it.
 *
 * Enter and leave are sent only when the view under the cursor changes, which
 * is what the protocol requires: a client that received an enter for every
 * motion event would redraw its hover state continuously. */
static void pointer_moved(const winput_t *ev)
{
    sway.cursor_x = ev->x;
    sway.cursor_y = ev->y;
    sway.dirty    = 1;                 /* the cursor itself has to be redrawn */

    struct view *now = view_at(ev->x, ev->y);

    if (now != sway.pointer_focus) {
        shell_pointer_leave(sway.pointer_focus);
        sway.pointer_focus = now;

        if (now) {
            int sx, sy;
            surface_local(now, ev->x, ev->y, &sx, &sy);
            shell_pointer_enter(now, sx, sy);
        }
    }

    if (now) {
        int sx, sy;
        surface_local(now, ev->x, ev->y, &sx, &sy);
        shell_pointer_motion(now, ev->time_ms, sx, sy);
    }
}

static void pointer_button(const winput_t *ev)
{
    sway.mods = ev->mods;

    if (ev->state && bar_click(ev->x, ev->y))
        return;

    struct view *v = sway.pointer_focus;

    /* Click to focus, on the press rather than the release: a click that
     * focuses and a click that the window acts on are the same click, and the
     * window must already have the keyboard by the time it hears about it. */
    if (ev->state && v && v != sway.focused)
        layout_focus(v);

    shell_pointer_button(v, ev->time_ms, ev->code, ev->state);
}

static void pointer_axis(const winput_t *ev)
{
    shell_pointer_axis(sway.pointer_focus, ev->time_ms, ev->code, ev->dy);
}

static void read_input(void)
{
    winput_t events[16];

    int n = wread(sway.input_fd, events, sizeof(events));
    if (n <= 0)
        return;

    for (int i = 0; i < n / (int)sizeof(events[0]); i++) {
        const winput_t *ev = &events[i];

        switch (ev->type) {
        case W_INPUT_KEY:            handle_key(ev);    break;
        case W_INPUT_POINTER_MOTION: pointer_moved(ev); break;
        case W_INPUT_POINTER_BUTTON: pointer_button(ev); break;
        case W_INPUT_POINTER_AXIS:   pointer_axis(ev);  break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ *
 *  The bar's right-hand side
 * ------------------------------------------------------------------ */

/* What is left of the screen once the bar has had its strip, and where that
 * strip is.  Both come from one place because they are one decision: a bar at
 * the top takes rows off the top, and windows have to start below them.
 *
 * Called again whenever the bar is turned off or moved, so `swaymsg bar
 * position top` rearranges rather than drawing over the window at the top. */
void sway_update_usable(void)
{
    /* Only a docked bar takes room from the windows.  One that hides is drawn
     * over them when it appears, which is what makes hiding worth having on a
     * 640x400 screen: twenty pixels back. */
    int bar_h = (sway.config.bar == BAR_DOCK) ? BAR_HEIGHT : 0;

    sway.usable_y = (bar_h && sway.config.bar_top) ? bar_h : 0;
    sway.usable_h = (int)sway.screen.height - bar_h;
}

/* ------------------------------------------------------------------ *
 *  ${...}
 *
 *  Two things show a line somebody wrote with figures in it: the background,
 *  and the right-hand side of the bar.  Both go through wstatus_expand() --
 *  see wstatus.h -- so that "what can I put in it?" has one answer, and so
 *  that the settings window can show the same expansion of the same names
 *  without a second copy of what they mean.
 * ------------------------------------------------------------------ */

/* The right-hand side of the bar.
 *
 * Written out every time round the loop and compared with what is already
 * there, because that is the honest test of whether the screen needs redoing:
 * the clock answers the same thing for a minute at a time, and a compositor
 * that redrew to write the same number would be a load of its own. */
static void update_status(void)
{
    char next[sizeof(sway.status)];

    if (sway.config.status_text[0]) {
        wstatus_expand(sway.config.status_text, next, sizeof(next));
    } else {
        /* What a machine nobody has configured shows: the charge, when there
         * is a battery to report one, and the date and time.  Written as a
         * template like any other, so the default and a configured line are
         * the same kind of thing. */
        char battery[24];

        wstatus_expand("${BATTERY}", battery, sizeof(battery));
        wstatus_expand(battery[0] ? "${BATTERY}   ${DATE}  ${TIME}"
                                  : "${DATE}  ${TIME}", next, sizeof(next));
    }

    if (strcmp(next, sway.status) == 0)
        return;

    strlcpy(sway.status, next, sizeof(sway.status));
    sway.dirty = 1;
}

/* The text across the background, expanded the same way.  Called every time
 * round the loop, so a template that has just arrived over the IPC socket is
 * on the screen by the next frame rather than by the next second. */
void sway_update_background_text(void)
{
    char next[BACKGROUND_LINE_MAX];

    wstatus_expand(sway.config.background_text, next, sizeof(next));

    if (strcmp(next, sway.background_line) == 0)
        return;

    strlcpy(sway.background_line, next, sizeof(sway.background_line));
    sway.dirty = 1;
}

/* Whether the bar is on the screen at this moment: always while it is docked,
 * never while it is invisible, and while the modifier is held when it hides.
 * The last is the only place this compositor reads the modifier without a
 * binding having matched. */
int sway_bar_showing(void)
{
    if (sway.config.bar == BAR_DOCK)
        return 1;
    if (sway.config.bar == BAR_HIDE)
        return (sway.mods & sway.config.mod) != 0;

    return 0;
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

    /* Whether there is a pointer at all decides what the seat advertises and
     * whether a cursor is drawn, so it is settled before any client can bind
     * the seat and hear the answer.  Telling clients there is a mouse when
     * there is not leaves them waiting for motion that never comes. */
    {
        wpointer_t pointer;

        if (wpointer(&pointer) == 0 && pointer.present) {
            sway.have_pointer = 1;
            sway.cursor_x     = pointer.x;
            sway.cursor_y     = pointer.y;
        }
    }

    sway_update_usable();

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

    /* Settled again after the configuration, which can turn the bar off and
     * can move it to the other end of the screen. */
    sway_update_usable();
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
        sway_update_background_text();

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
