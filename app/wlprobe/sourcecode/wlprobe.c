/* wlprobe -- ask a display server what it can do, and say so.
 *
 * A real Wayland client, written the way every Wayland client is written:
 *
 *     connect, get the registry, add a listener, roundtrip
 *
 * and then print what came back.  It binds nothing and draws nothing; it
 * exists to answer the question "is the display server up, and what does it
 * advertise", which is the first thing worth knowing when a client will not
 * start.
 *
 *     wlprobe                        the default display, wayland-0
 *     wlprobe wayland-test           another display by name
 *     wlprobe /ramdisk/wayland-0     or by path
 */

#include <wkernel.h>
#include <wayland-client.h>

#define MAX_GLOBALS 32

struct global {
    uint32_t name;
    uint32_t version;
    char     interface[48];
};

struct probe {
    struct global globals[MAX_GLOBALS];
    int           count;
    int           lost;
};

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version)
{
    struct probe *p = data;

    if (p->count >= MAX_GLOBALS) {
        p->lost++;
        return;
    }

    p->globals[p->count].name    = name;
    p->globals[p->count].version = version;
    strlcpy(p->globals[p->count].interface, interface,
            sizeof(p->globals[p->count].interface));
    p->count++;
}

static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name)
{
    /* Globals can go as well as arrive -- an output being unplugged is the
     * usual reason.  Nothing here holds one long enough to see it, but a
     * listener that left this out would be an incomplete one. */
    (void)data;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    handle_global,
    handle_global_remove,
};

/* What each interface is for, so the output says something to somebody who has
 * not memorised the protocol. */
static const char *purpose(const char *interface)
{
    if (strcmp(interface, "wl_compositor") == 0)
        return "makes surfaces";
    if (strcmp(interface, "wl_shm") == 0)
        return "shared memory buffers";
    if (strcmp(interface, "wl_seat") == 0)
        return "keyboard and pointer";
    if (strcmp(interface, "wl_output") == 0)
        return "a screen";
    if (strcmp(interface, "xdg_wm_base") == 0)
        return "windows";
    return "";
}

int main(int argc, char **argv)
{
    const char  *name = (argc > 1) ? argv[1] : NULL;
    struct probe p    = { .count = 0, .lost = 0 };

    struct wl_display *display = wl_display_connect(name);
    if (!display) {
        wfprintf(W_STDERR, "wlprobe: cannot reach %s\n",
                 name ? name : "wayland-0");
        wfprintf(W_STDERR, "wlprobe: is a display server running? "
                           "`systemctl status sway`\n");
        return 1;
    }

    wprintf("connected to %s\n", name ? name : "wayland-0");

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &p);

    /* The roundtrip is what makes the list complete: the server sends every
     * global as soon as the registry exists, and the sync it answers last
     * cannot come back before all of them have. */
    if (wl_display_roundtrip(display) < 0) {
        wfprintf(W_STDERR, "wlprobe: the connection broke (protocol error %d)\n",
                 wl_display_get_error(display));
        return 1;
    }

    if (p.count == 0) {
        wprintf("the display server advertises nothing\n");
    } else {
        wprintf("%d global%s:\n", p.count, p.count == 1 ? "" : "s");

        for (int i = 0; i < p.count; i++) {
            const char *what = purpose(p.globals[i].interface);

            wprintf("  %2u  %-16s version %u", p.globals[i].name,
                    p.globals[i].interface, p.globals[i].version);
            if (what[0])
                wprintf("   %s", what);
            wprintf("\n");
        }
    }

    if (p.lost)
        wprintf("(%d more than this program has room for)\n", p.lost);

    /* A second roundtrip, to show the connection is still good afterwards --
     * which is the other half of "is the server healthy". */
    if (wl_display_roundtrip(display) < 0) {
        wfprintf(W_STDERR, "wlprobe: the server stopped answering\n");
        return 1;
    }
    wprintf("the server is answering\n");

    wl_display_disconnect(display);
    return 0;
}
