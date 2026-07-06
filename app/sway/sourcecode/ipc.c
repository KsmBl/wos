/* The IPC socket: sway's other interface.
 *
 * A compositor that can only be driven from the keyboard is a compositor
 * nothing can be scripted against.  i3 solved this with a socket carrying
 * length-prefixed messages, sway inherited the protocol unchanged, and this is
 * that protocol -- the same magic string, the same header, the same message
 * numbers, the same JSON:
 *
 *     "i3-ipc"   six bytes
 *     uint32     payload length
 *     uint32     message type
 *     ...        payload
 *
 * So `swaymsg` here is the real swaymsg's shape, and a program written to
 * drive sway is written to drive this.
 *
 * JSON is written out by hand.  There is no library, the documents are small
 * and their shape is fixed, and a compositor that needed a parser to describe
 * its own state would be carrying one for nothing.
 */

#include "sway.h"

#define IPC_MAX_CLIENTS 4
#define IPC_BUFFER      8192

#define IPC_MAGIC "i3-ipc"
#define IPC_MAGIC_LEN 6

/* The message numbers, from i3's protocol. */
#define IPC_RUN_COMMAND       0
#define IPC_GET_WORKSPACES    1
#define IPC_SUBSCRIBE         2
#define IPC_GET_OUTPUTS       3
#define IPC_GET_TREE          4
#define IPC_GET_MARKS         5
#define IPC_GET_BAR_CONFIG    6
#define IPC_GET_VERSION       7
#define IPC_GET_BINDING_MODES 8
#define IPC_GET_CONFIG        9
#define IPC_SEND_TICK        10

struct ipc_client {
    int      fd;
    int      in_use;
    uint8_t  in[IPC_BUFFER];
    uint32_t have;
};

static int               listen_fd = -1;
static struct ipc_client clients[IPC_MAX_CLIENTS];

/* ------------------------------------------------------------------ *
 *  Writing JSON
 * ------------------------------------------------------------------ */

static char     out[IPC_BUFFER];
static wsize_t  out_len;

static void put(const char *s)
{
    wsize_t n = strlen(s);

    if (out_len + n + 1 > sizeof(out))
        return;                        /* truncated rather than overrun */

    memcpy(out + out_len, s, n);
    out_len += n;
    out[out_len] = '\0';
}

static void put_number(const char *key, long value)
{
    char buf[64];
    wsnprintf(buf, sizeof(buf), "\"%s\":%d,", key, (int)value);
    put(buf);
}

static void put_bool(const char *key, int value)
{
    char buf[64];
    wsnprintf(buf, sizeof(buf), "\"%s\":%s,", key, value ? "true" : "false");
    put(buf);
}

/* A string, with the characters JSON will not carry escaped.  A window title
 * is whatever the client set it to, so it may well contain a quote. */
static void put_string(const char *key, const char *value)
{
    char buf[80];

    wsnprintf(buf, sizeof(buf), "\"%s\":\"", key);
    put(buf);

    for (const char *s = value; *s; s++) {
        char c[8];

        if (*s == '"' || *s == '\\')
            wsnprintf(c, sizeof(c), "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            wsnprintf(c, sizeof(c), "\\u%04x", (unsigned char)*s);
        else
            wsnprintf(c, sizeof(c), "%c", *s);

        put(c);
    }

    put("\",");
}

/* Remove a trailing comma before closing an object or an array, since JSON
 * does not allow one and every field above writes one. */
static void close_with(const char *bracket)
{
    if (out_len > 0 && out[out_len - 1] == ',')
        out[--out_len] = '\0';
    put(bracket);
}

static void put_rect(int x, int y, int w, int h)
{
    char buf[128];

    wsnprintf(buf, sizeof(buf),
              "\"rect\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},",
              x, y, w, h);
    put(buf);
}

/* ------------------------------------------------------------------ *
 *  The documents
 * ------------------------------------------------------------------ */

static void describe_node(struct node *n, struct workspace *ws);

static void describe_view(struct view *v)
{
    put("{");
    put_number("id", (long)(unsigned long)v);
    put_string("type", "con");
    put_string("name", v->title[0] ? v->title : "window");
    put_string("app_id", v->app_id);
    put_string("layout", "none");
    put_bool("focused", sway.focused == v);
    put_bool("visible", v->mapped);
    put_rect(v->x, v->y, v->w, v->h);
    put("\"nodes\":[]");
    put("}");
}

static void describe_node(struct node *n, struct workspace *ws)
{
    if (!n)
        return;

    if (n->is_view) {
        if (n->view)
            describe_view(n->view);
        return;
    }

    put("{");
    put_number("id", (long)(unsigned long)n);
    put_string("type", "con");
    put_string("layout", n->layout == L_SPLITH ? "splith" : "splitv");
    put_rect(n->x, n->y, n->w, n->h);
    put("\"nodes\":[");

    for (struct node *c = n->children; c; c = c->next) {
        describe_node(c, ws);
        if (c->next)
            put(",");
    }

    put("]}");
}

static void get_tree(void)
{
    out_len = 0;
    out[0]  = '\0';

    put("{");
    put_number("id", 1);
    put_string("type", "root");
    put_string("name", "root");
    put_rect(0, 0, (int)sway.screen.width, (int)sway.screen.height);
    put("\"nodes\":[{");
    put_number("id", 2);
    put_string("type", "output");
    put_string("name", "WOS-1");
    put_rect(0, 0, (int)sway.screen.width, (int)sway.screen.height);
    put("\"nodes\":[");

    for (int i = 0; i < MAX_WORKSPACES; i++) {
        struct workspace *ws = &sway.workspaces[i];

        if (layout_view_count(ws) == 0 && i != sway.current)
            continue;

        put("{");
        put_number("id", 100 + i);
        put_string("type", "workspace");
        put_string("name", ws->name);
        put_number("num", ws->number);
        put_string("layout", ws->root && ws->root->layout == L_SPLITV
                             ? "splitv" : "splith");
        put_bool("focused", i == sway.current);
        put_rect(0, sway.usable_y, (int)sway.screen.width, sway.usable_h);
        put("\"nodes\":[");

        if (ws->root)
            for (struct node *c = ws->root->children; c; c = c->next) {
                describe_node(c, ws);
                if (c->next)
                    put(",");
            }

        put("]},");
    }

    close_with("]}]}");
}

static void get_workspaces(void)
{
    out_len = 0;
    out[0]  = '\0';
    put("[");

    for (int i = 0; i < MAX_WORKSPACES; i++) {
        struct workspace *ws = &sway.workspaces[i];

        if (layout_view_count(ws) == 0 && i != sway.current)
            continue;

        put("{");
        put_number("id", 100 + i);
        put_number("num", ws->number);
        put_string("name", ws->name);
        put_bool("visible", i == sway.current);
        put_bool("focused", i == sway.current);
        put_bool("urgent", 0);
        put_string("output", "WOS-1");
        put_number("windows", layout_view_count(ws));
        put_rect(0, sway.usable_y, (int)sway.screen.width, sway.usable_h);
        close_with("},");
    }

    close_with("]");
}

static void get_outputs(void)
{
    out_len = 0;
    out[0]  = '\0';

    put("[{");
    put_string("name", "WOS-1");
    put_string("make", "WOS");
    put_string("model", "framebuffer");
    put_bool("active", 1);
    put_bool("focused", 1);
    put_number("scale", 1);
    put_string("current_workspace", ws_current()->name);
    put_rect(0, 0, (int)sway.screen.width, (int)sway.screen.height);
    close_with("}]");
}

static void get_version(void)
{
    out_len = 0;
    out[0]  = '\0';

    put("{");
    put_number("major", 1);
    put_number("minor", 0);
    put_number("patch", 0);
    put_string("human_readable", "1.0 (sway for WOS)");
    put_string("loaded_config_file_name", sway.config_path);
    close_with("}");
}

/* ------------------------------------------------------------------ *
 *  Messages
 * ------------------------------------------------------------------ */

static void reply(int fd, uint32_t type, const char *payload)
{
    uint8_t  header[IPC_MAGIC_LEN + 8];
    uint32_t len = (uint32_t)strlen(payload);

    memcpy(header, IPC_MAGIC, IPC_MAGIC_LEN);
    for (int i = 0; i < 4; i++) {
        header[IPC_MAGIC_LEN + i]     = (uint8_t)(len >> (i * 8));
        header[IPC_MAGIC_LEN + 4 + i] = (uint8_t)(type >> (i * 8));
    }

    wmsg_t h = { header, sizeof(header), 0, 0 };
    wsend(fd, &h);

    /* In chunks a poll has promised room for, the same rule the Wayland
     * connection follows: a reply larger than the socket buffer would block
     * against a client that is not reading yet. */
    uint32_t at = 0;
    while (at < len) {
        uint32_t take = len - at;
        if (take > W_SEND_CHUNK)
            take = W_SEND_CHUNK;

        wmsg_t body = { (void *)(payload + at), take, 0, 0 };
        int    sent = wsend(fd, &body);
        if (sent <= 0)
            return;

        at += (uint32_t)sent;
    }
}

static void handle_message(struct ipc_client *c, uint32_t type,
                           const char *payload, uint32_t len)
{
    char command[COMMAND_MAX];

    switch (type) {
    case IPC_RUN_COMMAND:
        if (len >= sizeof(command))
            len = sizeof(command) - 1;
        memcpy(command, payload, len);
        command[len] = '\0';

        sway_log("ipc: %s", command);
        config_run_command(command);

        /* i3 replies with one object per command run.  One command, one
         * object -- and `success` is true because a command that could not be
         * carried out is logged rather than refused, exactly as it is when it
         * comes from the configuration file. */
        reply(c->fd, type, "[{\"success\":true}]");
        return;

    case IPC_GET_WORKSPACES:
        get_workspaces();
        reply(c->fd, type, out);
        return;

    case IPC_GET_OUTPUTS:
        get_outputs();
        reply(c->fd, type, out);
        return;

    case IPC_GET_TREE:
        get_tree();
        reply(c->fd, type, out);
        return;

    case IPC_GET_VERSION:
        get_version();
        reply(c->fd, type, out);
        return;

    case IPC_GET_BINDING_MODES:
        reply(c->fd, type, "[\"default\"]");
        return;

    case IPC_GET_MARKS:
        reply(c->fd, type, "[]");
        return;

    case IPC_GET_CONFIG:
        out_len = 0;
        out[0]  = '\0';
        put("{");
        put_string("config", sway.config_path);
        close_with("}");
        reply(c->fd, type, out);
        return;

    case IPC_SUBSCRIBE:
        /* Accepted, and nothing is ever sent.  A subscriber that is told it
         * failed will usually give up; one that is told it succeeded and hears
         * nothing behaves the way it does when nothing happens -- which, for
         * events this compositor does not raise, is the truth. */
        reply(c->fd, type, "{\"success\":true}");
        return;

    case IPC_SEND_TICK:
        reply(c->fd, type, "{\"success\":true}");
        return;

    default:
        reply(c->fd, type, "{\"success\":false,\"error\":\"unknown message\"}");
        return;
    }
}

static void read_client(struct ipc_client *c)
{
    wmsg_t m = { c->in + c->have, IPC_BUFFER - c->have, 0, 0 };
    int    n = wrecv(c->fd, &m);

    if (n <= 0) {
        wclose(c->fd);
        c->in_use = 0;
        return;
    }

    c->have += (uint32_t)n;

    for (;;) {
        if (c->have < IPC_MAGIC_LEN + 8)
            return;
        if (memcmp(c->in, IPC_MAGIC, IPC_MAGIC_LEN) != 0) {
            sway_log("ipc: not an i3-ipc message; dropping the client");
            wclose(c->fd);
            c->in_use = 0;
            return;
        }

        uint32_t len = 0, type = 0;
        for (int i = 3; i >= 0; i--) {
            len  = (len << 8)  | c->in[IPC_MAGIC_LEN + i];
            type = (type << 8) | c->in[IPC_MAGIC_LEN + 4 + i];
        }

        if (len > IPC_BUFFER - IPC_MAGIC_LEN - 8) {
            wclose(c->fd);
            c->in_use = 0;
            return;
        }
        if (c->have < IPC_MAGIC_LEN + 8 + len)
            return;                          /* the rest has not arrived */

        handle_message(c, type, (const char *)c->in + IPC_MAGIC_LEN + 8, len);

        uint32_t used = IPC_MAGIC_LEN + 8 + len;
        if (used < c->have)
            memmove(c->in, c->in + used, c->have - used);
        c->have -= used;

        if (!c->in_use)
            return;
    }
}

/* ------------------------------------------------------------------ *
 *  The loop's share
 * ------------------------------------------------------------------ */

int ipc_init(const char *path)
{
    listen_fd = wlisten(path);
    if (listen_fd < 0) {
        sway_log("ipc: cannot listen on %s: %s", path, wstrerror(-listen_fd));
        return listen_fd;
    }

    sway.ipc_fd = listen_fd;
    sway_log("ipc: listening on %s", path);
    return 0;
}

void ipc_poll_fds(wpollfd_t *watch, int *n, int max)
{
    if (listen_fd >= 0 && *n < max) {
        watch[*n].fd      = listen_fd;
        watch[*n].events  = W_POLLIN;
        watch[*n].revents = 0;
        (*n)++;
    }

    for (int i = 0; i < IPC_MAX_CLIENTS && *n < max; i++) {
        if (!clients[i].in_use)
            continue;

        watch[*n].fd      = clients[i].fd;
        watch[*n].events  = W_POLLIN;
        watch[*n].revents = 0;
        (*n)++;
    }
}

void ipc_handle(const wpollfd_t *watch, int count)
{
    for (int i = 0; i < count; i++) {
        if (watch[i].fd != listen_fd || !(watch[i].revents & W_POLLIN))
            continue;

        int fd = waccept(listen_fd);
        if (fd < 0)
            continue;

        struct ipc_client *slot = NULL;
        for (int j = 0; j < IPC_MAX_CLIENTS; j++)
            if (!clients[j].in_use) {
                slot = &clients[j];
                break;
            }

        if (!slot) {
            wclose(fd);
            continue;
        }

        memset(slot, 0, sizeof(*slot));
        slot->fd     = fd;
        slot->in_use = 1;
    }

    for (int j = 0; j < IPC_MAX_CLIENTS; j++) {
        if (!clients[j].in_use)
            continue;

        for (int i = 0; i < count; i++)
            if (watch[i].fd == clients[j].fd &&
                (watch[i].revents & (W_POLLIN | W_POLLHUP))) {
                read_client(&clients[j]);
                break;
            }
    }
}

void ipc_shutdown(void)
{
    for (int j = 0; j < IPC_MAX_CLIENTS; j++)
        if (clients[j].in_use) {
            wclose(clients[j].fd);
            clients[j].in_use = 0;
        }

    if (listen_fd >= 0)
        wclose(listen_fd);
    listen_fd = -1;
}
