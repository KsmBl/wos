/* swaymsg -- talk to the compositor.
 *
 *   swaymsg <command>              run a sway command
 *   swaymsg -t get_workspaces      ask it something
 *   swaymsg -t get_tree -r         and print the raw JSON
 *
 * The same protocol i3 defined and sway kept: a magic string, a length, a type
 * and a payload, over a socket.  Which means the interesting property is not
 * that this program works, but that it is not the only thing that can -- any
 * program that speaks i3's IPC speaks to this compositor.
 *
 * The command language is the one in the configuration file, because in sway
 * they are the same language.  `swaymsg splitv` does what the line `splitv`
 * does when the compositor reads it at startup.
 */

#include <wkernel.h>
#include <wipc.h>

static const struct {
    const char *name;
    uint32_t    type;
} message_types[] = {
    { "run_command",       WIPC_RUN_COMMAND       },
    { "get_workspaces",    WIPC_GET_WORKSPACES    },
    { "get_outputs",       WIPC_GET_OUTPUTS       },
    { "get_tree",          WIPC_GET_TREE          },
    { "get_marks",         WIPC_GET_MARKS         },
    { "get_version",       WIPC_GET_VERSION       },
    { "get_binding_modes", WIPC_GET_BINDING_MODES },
    { "get_config",        WIPC_GET_CONFIG        },
    { "send_tick",         WIPC_SEND_TICK         },
    { "subscribe",         WIPC_SUBSCRIBE         },
};

static void usage(void)
{
    wfprintf(W_STDERR,
             "usage: swaymsg [-t <type>] [-r] [message]\n\n"
             "  with no -t, the message is a sway command:\n"
             "    swaymsg splitv\n"
             "    swaymsg 'workspace 2'\n"
             "    swaymsg exit\n\n"
             "  types: ");

    for (unsigned i = 0; i < sizeof(message_types) / sizeof(message_types[0]);
         i++)
        wfprintf(W_STDERR, "%s%s", i ? ", " : "", message_types[i].name);
    wfprintf(W_STDERR, "\n");
}

/* ------------------------------------------------------------------ *
 *  Reading a reply
 *
 *  Just enough JSON to pull named values out of a flat object.  A parser would
 *  be the right thing if this printed everything; it prints a handful of
 *  fields, and -r exists for anybody who wants the document itself.
 * ------------------------------------------------------------------ */

/* One field of a reply, through the library's reader.  Returns where the
 * search started from, so the callers that walk a list of objects can carry
 * on past the one they just read. */
static const char *field(const char *json, const char *key, char *out,
                         wsize_t size)
{
    return wipc_field(json, key, out, size) ? json : NULL;
}

static void print_workspaces(const char *json)
{
    const char *at = json;

    wprintf("%-4s %-8s %-8s %s\n", "NUM", "NAME", "WINDOWS", "");

    while ((at = strstr(at, "\"num\":")) != NULL) {
        char num[16], name[32], windows[16], focused[16];

        field(at, "num", num, sizeof(num));
        field(at, "name", name, sizeof(name));
        field(at, "windows", windows, sizeof(windows));
        field(at, "focused", focused, sizeof(focused));

        wprintf("%-4s %-8s %-8s %s\n", num, name, windows,
                strcmp(focused, "true") == 0 ? "(focused)" : "");
        at += 6;
    }
}

static void print_version(const char *json)
{
    char human[64], config[W_PATH_MAX + 1];

    field(json, "human_readable", human, sizeof(human));
    field(json, "loaded_config_file_name", config, sizeof(config));

    wprintf("sway version %s\n", human);
    wprintf("configuration: %s\n", config);
}

static void print_outputs(const char *json)
{
    char name[32], workspace[32], width[16], height[16];

    field(json, "name", name, sizeof(name));
    field(json, "current_workspace", workspace, sizeof(workspace));
    field(json, "width", width, sizeof(width));
    field(json, "height", height, sizeof(height));

    wprintf("%s  %sx%s  workspace %s\n", name, width, height, workspace);
}

/* The tree, one line per node, indented by depth.  Enough to see the shape of
 * the layout without reading braces.
 *
 * A node's own fields are read only from its header -- the part before its
 * "nodes" list -- because searching the whole document from a node would find
 * its children's fields for anything it has not got itself.  And the rect each
 * node carries is an object of its own, so it is stepped over rather than
 * counted as a node. */
static void print_tree(const char *json)
{
    int depth = 0;

    for (const char *at = json; *at; at++) {
        if (strncmp(at, "\"rect\":{", 8) == 0) {
            at = strchr(at, '}');
            if (!at)
                break;
            continue;
        }

        if (*at == '[') {
            depth++;
            continue;
        }
        if (*at == ']') {
            if (depth > 0)
                depth--;
            continue;
        }
        if (*at != '{')
            continue;

        /* The header: everything before this node's children. */
        char        header[512];
        const char *end = strstr(at, "\"nodes\":");
        wsize_t     len = end ? (wsize_t)(end - at) : strlen(at);

        if (len >= sizeof(header))
            len = sizeof(header) - 1;
        memcpy(header, at, len);
        header[len] = '\0';

        char type[24], name[48], layout[16], focused[8];

        type[0] = name[0] = layout[0] = focused[0] = '\0';
        field(header, "type", type, sizeof(type));
        field(header, "name", name, sizeof(name));
        field(header, "layout", layout, sizeof(layout));
        field(header, "focused", focused, sizeof(focused));

        for (int i = 0; i < depth; i++)
            wprintf("  ");

        if (strcmp(layout, "none") == 0)
            wprintf("%s%s\n", name[0] ? name : "window",
                    strcmp(focused, "true") == 0 ? "  *" : "");
        else if (name[0] && layout[0])
            wprintf("%s %s [%s]\n", type, name, layout);
        else if (name[0])
            wprintf("%s %s\n", type, name);
        else
            wprintf("%s [%s]\n", type, layout);
    }
}

int main(int argc, char **argv)
{
    uint32_t    type = WIPC_RUN_COMMAND;
    const char *type_name = "run_command";
    int         raw = 0;
    int         at  = 1;

    while (at < argc && argv[at][0] == '-') {
        if (strcmp(argv[at], "-t") == 0 && at + 1 < argc) {
            type_name = argv[++at];
            at++;

            unsigned i;
            for (i = 0; i < sizeof(message_types) / sizeof(message_types[0]);
                 i++)
                if (strcmp(message_types[i].name, type_name) == 0) {
                    type = message_types[i].type;
                    break;
                }

            if (i == sizeof(message_types) / sizeof(message_types[0])) {
                wfprintf(W_STDERR, "swaymsg: no message type called %s\n",
                         type_name);
                usage();
                return 1;
            }
        } else if (strcmp(argv[at], "-r") == 0 ||
                   strcmp(argv[at], "--raw") == 0) {
            raw = 1;
            at++;
        } else if (strcmp(argv[at], "-h") == 0 ||
                   strcmp(argv[at], "--help") == 0) {
            usage();
            return 0;
        } else {
            wfprintf(W_STDERR, "swaymsg: %s is not an option\n", argv[at]);
            usage();
            return 1;
        }
    }

    /* Everything left is the message, joined back into one line the way a
     * shell would have had it. */
    char payload[256];
    payload[0] = '\0';

    for (int i = at; i < argc; i++) {
        if (i > at)
            strlcpy(payload + strlen(payload), " ",
                    sizeof(payload) - strlen(payload));
        strlcpy(payload + strlen(payload), argv[i],
                sizeof(payload) - strlen(payload));
    }

    if (type == WIPC_RUN_COMMAND && !payload[0]) {
        usage();
        return 1;
    }

    static char json[16384];
    int         r = wipc_message(WIPC_SWAY_SOCKET, type, payload, json,
                                 sizeof(json));

    if (r < 0) {
        wfprintf(W_STDERR, "swaymsg: cannot reach sway: %s\n", wstrerror(-r));
        wfprintf(W_STDERR, -r == W_EPERM
                 ? "swaymsg: that compositor belongs to another user\n"
                 : "swaymsg: is it running? `systemctl status sway`\n");
        return 1;
    }

    if (raw) {
        wprintf("%s\n", json);
        return 0;
    }

    if (type == WIPC_RUN_COMMAND) {
        /* Nothing worth printing when it worked, which is what the real
         * swaymsg does too. */
        if (strstr(json, "\"success\":true"))
            return 0;
        wprintf("%s\n", json);
        return 1;
    }

    if (type == WIPC_GET_WORKSPACES)  print_workspaces(json);
    else if (type == WIPC_GET_VERSION) print_version(json);
    else if (type == WIPC_GET_OUTPUTS) print_outputs(json);
    else if (type == WIPC_GET_TREE)    print_tree(json);
    else                              wprintf("%s\n", json);

    return 0;
}
