/* The configuration file, and the commands it is written in.
 *
 * sway's configuration is not a list of settings, it is a list of commands --
 * the same commands `swaymsg` sends at runtime.  `bindsym $mod+Return exec
 * wlterm` binds a key to a command, and the command is the same text swaymsg
 * would send.  That is why one parser reads both, and why `reload` is possible
 * at all: rereading the file is running it again.
 *
 * The file is read from the same place sway reads it, in the same order:
 *
 *     ~/.config/sway/config
 *     /etc/sway/config
 *
 * Directives this compositor cannot honour are accepted and ignored rather
 * than refused.  A configuration written for the real sway should start this
 * one, and it will not if an unknown word is an error -- but each one that is
 * ignored is logged, so `sway.log` says exactly what did not happen.
 */

#include "sway.h"

/* ------------------------------------------------------------------ *
 *  Words
 * ------------------------------------------------------------------ */

#define MAX_WORDS 24

struct variable {
    char name[32];
    char value[128];
};

static struct variable variables[MAX_VARS];
static int             variable_count;

/* Split a line into words, honouring double quotes so a title or a command can
 * contain spaces.  The line is modified in place. */
static int split(char *line, char *words[], int max)
{
    int n = 0;

    while (*line && n < max) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line)
            break;

        if (*line == '"') {
            words[n++] = ++line;
            while (*line && *line != '"')
                line++;
        } else {
            words[n++] = line;
            while (*line && *line != ' ' && *line != '\t')
                line++;
        }

        if (*line)
            *line++ = '\0';
    }

    return n;
}

const char *config_expand(const char *word)
{
    if (!word || word[0] != '$')
        return word;

    for (int i = 0; i < variable_count; i++)
        if (strcmp(variables[i].name, word + 1) == 0)
            return variables[i].value;

    return word;
}

static void variable_set(const char *name, const char *value)
{
    for (int i = 0; i < variable_count; i++)
        if (strcmp(variables[i].name, name) == 0) {
            strlcpy(variables[i].value, value, sizeof(variables[i].value));
            return;
        }

    if (variable_count >= MAX_VARS)
        return;

    strlcpy(variables[variable_count].name, name,
            sizeof(variables[variable_count].name));
    strlcpy(variables[variable_count].value, value,
            sizeof(variables[variable_count].value));
    variable_count++;
}

/* ------------------------------------------------------------------ *
 *  Colours
 * ------------------------------------------------------------------ */

static uint32_t parse_colour(const char *s, uint32_t fallback)
{
    if (!s)
        return fallback;
    if (*s == '#')
        s++;

    uint32_t value = 0;
    int      digits = 0;

    for (; *s; s++, digits++) {
        int d;

        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return fallback;

        value = value * 16 + (uint32_t)d;
    }

    /* #rrggbbaa is allowed and the alpha is dropped: nothing here blends, and
     * silently drawing an opaque window where a translucent one was asked for
     * is better than refusing the file over it. */
    if (digits == 8)
        return (value >> 8) & 0xFFFFFF;
    if (digits == 6)
        return value & 0xFFFFFF;

    return fallback;
}

/* ------------------------------------------------------------------ *
 *  The pointer
 * ------------------------------------------------------------------ */

/* A number sway writes with a decimal point, in hundredths.
 *
 * `pointer_accel 0.5` is how the setting is written everywhere it is
 * documented, and atoi() reads that as 0 -- the one value that means "leave it
 * alone", so a file asking for half speed would silently get none of it.  Two
 * digits after the point is as fine as the setting goes: the range is two units
 * wide and the mouse underneath it counts in whole pixels.
 */
static int parse_hundredths(const char *s, int *out)
{
    int sign = 1, whole = 0, frac = 0, scale = 10, digits = 0;

    if (!s)
        return 0;

    if (*s == '-' || *s == '+')
        sign = (*s++ == '-') ? -1 : 1;

    for (; *s >= '0' && *s <= '9'; s++, digits++)
        whole = whole * 10 + (*s - '0');

    if (*s == '.' || *s == ',') {
        s++;
        for (; *s >= '0' && *s <= '9'; s++, digits++) {
            if (scale) {
                frac += (*s - '0') * scale;
                scale /= 10;
            }
        }
    }

    if (!digits || *s)
        return 0;

    *out = sign * (whole * 100 + frac);
    return 1;
}

/* libinput's -1..1 as a speed the kernel can use.
 *
 * The two halves of the range are not the same size, because they are not the
 * same question: below zero the pointer is being slowed down, and a quarter
 * speed is as slow as a screen this size can be crossed at, while above zero it
 * is being sped up and four times is a fair reach for a hand that would rather
 * not move.  Real libinput's curve is not this, but real libinput has the
 * timing to build one from; this is the honest straight line through the same
 * three points -- slowest, unchanged, fastest.
 */
static int speed_from_accel(int accel)
{
    if (accel < -100) accel = -100;
    if (accel >  100) accel =  100;

    return accel <= 0 ? 100 + accel * 3 / 4    /* -1.0 -> 25%  */
                      : 100 + accel * 3;       /*  1.0 -> 400% */
}

/* Hand the setting to the kernel and say what came of it.
 *
 * Called for every `pointer_accel`, and once from config_defaults(), because
 * the speed outlives the compositor: it is the kernel's, so a sway that had
 * been asked for a fast pointer and is started again without that line would
 * otherwise inherit the speed from the session before it. */
static void pointer_speed_apply(void)
{
    int percent = speed_from_accel(sway.config.pointer_accel);
    int r       = wpointerspeed(percent);

    if (r == -W_ENODEV)
        sway_log("pointer_accel: no mouse on this machine, so nothing to slow "
                 "down or speed up");
    else if (r < 0)
        sway_log("pointer_accel: the kernel would not set the speed: %s",
                 wstrerror(-r));
    else
        sway_log("pointer_accel: the pointer moves at %d%% of the mouse", r);
}

/* ------------------------------------------------------------------ *
 *  The cursor
 * ------------------------------------------------------------------ */

static int clamp_cursor_size(int size)
{
    if (size < CURSOR_SIZE_MIN) return CURSOR_SIZE_MIN;
    if (size > CURSOR_SIZE_MAX) return CURSOR_SIZE_MAX;
    return size;
}

/* ------------------------------------------------------------------ *
 *  Key combinations
 * ------------------------------------------------------------------ */

/* "$mod+Shift+q" -> a key code and a set of modifiers. */
static int parse_binding(const char *combo, uint32_t *code, uint32_t *mods)
{
    char buf[96];
    strlcpy(buf, combo, sizeof(buf));

    *code = 0;
    *mods = 0;

    char *at = buf;
    while (at) {
        char *plus = strchr(at, '+');
        if (plus)
            *plus = '\0';

        const char *part = config_expand(at);
        uint32_t    mod  = wmodifier_from_name(part);

        if (mod) {
            *mods |= mod;
        } else {
            uint32_t key = wkeycode_from_name(part);
            if (!key)
                return 0;
            *code = key;
        }

        at = plus ? plus + 1 : NULL;
    }

    return *code != 0;
}

static void binding_add(const char *combo, const char *command)
{
    uint32_t code, mods;

    if (!parse_binding(combo, &code, &mods)) {
        sway_log("config: %s is not a key this keyboard has", combo);
        return;
    }

    /* A second binding for the same keys replaces the first, so a later line
     * in the file wins -- which is what a person editing one expects. */
    for (struct binding *b = sway.bindings; b; b = b->next)
        if (b->code == code && b->mods == mods) {
            strlcpy(b->command, command, sizeof(b->command));
            return;
        }

    struct binding *b = malloc(sizeof(*b));
    if (!b)
        return;

    b->code = code;
    b->mods = mods;
    strlcpy(b->command, command, sizeof(b->command));

    b->next       = sway.bindings;
    sway.bindings = b;
}

/* ------------------------------------------------------------------ *
 *  Commands
 *
 *  Everything below runs both from the configuration file and from swaymsg,
 *  because in sway they are the same language.
 * ------------------------------------------------------------------ */

/* Append, bounded.  strcat would be shorter and would let a long line in the
 * configuration file write past the end of a command buffer. */
static void append(char *out, wsize_t size, const char *word)
{
    wsize_t len = strlen(out);
    if (len < size)
        strlcpy(out + len, word, size - len);
}

/* Rebuild a command's words back into one string, expanding variables.  Used
 * by `exec`, whose argument is the whole of the rest of the line. */
static void join(char *out, wsize_t size, char *words[], int from, int count)
{
    out[0] = '\0';

    for (int i = from; i < count; i++) {
        if (i > from)
            append(out, size, " ");
        append(out, size, config_expand(words[i]));
    }
}

/* `\n` in a configuration file is two characters; a line break is one.  Only
 * the two escapes worth having are handled -- a break, and a backslash for
 * anybody who wanted one -- because a configuration file is not a C string and
 * inventing the rest of that language here would be inventing it. */
static void unescape(char *s)
{
    char *out = s;

    for (const char *at = s; *at; at++) {
        if (*at != '\\' || !at[1]) {
            *out++ = *at;
            continue;
        }

        at++;

        if (*at == 'n')       *out++ = '\n';
        else if (*at == '\\') *out++ = '\\';
        else {
            *out++ = '\\';
            *out++ = *at;
        }
    }

    *out = '\0';
}

static int direction_from_name(const char *name, enum direction *out)
{
    if (strcmp(name, "left") == 0)  { *out = DIR_LEFT;  return 1; }
    if (strcmp(name, "right") == 0) { *out = DIR_RIGHT; return 1; }
    if (strcmp(name, "up") == 0)    { *out = DIR_UP;    return 1; }
    if (strcmp(name, "down") == 0)  { *out = DIR_DOWN;  return 1; }
    return 0;
}

/* One command, already split into words. */
static void run(char *words[], int count)
{
    if (count == 0)
        return;

    const char *cmd = config_expand(words[0]);

    /* --- starting programs --- */

    if (strcmp(cmd, "exec") == 0 || strcmp(cmd, "exec_always") == 0) {
        char line[COMMAND_MAX];
        join(line, sizeof(line), words, 1, count);
        sway_spawn(line);
        return;
    }

    /* --- windows --- */

    if (strcmp(cmd, "kill") == 0) {
        shell_close(sway.focused);
        return;
    }

    /* --- the bar --- */

    /* `bar position top`, and the same words the `bar { ... }` block uses --
     * the block hands its lines here with `bar` put back on the front, so
     * there is one place that knows what a bar setting means whether it came
     * from the file or from swaymsg.
     *
     * Real sway takes a bar id here (`bar bar-0 position top`) because it can
     * have several.  There is one bar and it is drawn by the compositor, so
     * the id is accepted and skipped rather than demanded. */
    if (strcmp(cmd, "bar") == 0 && count > 1) {
        int at = 1;

        if (count > 2 && strcmp(config_expand(words[1]), "position") != 0 &&
                         strcmp(config_expand(words[1]), "mode") != 0)
            at = 2;                       /* a bar id we do not need */

        const char *what = config_expand(words[at]);

        if (strcmp(what, "position") == 0 && count > at + 1) {
            const char *where = config_expand(words[at + 1]);

            if (strcmp(where, "top") == 0)
                sway.config.bar_top = 1;
            else if (strcmp(where, "bottom") == 0)
                sway.config.bar_top = 0;
            else {
                sway_log("bar position %s: not top or bottom", where);
                return;
            }
        } else if (strcmp(what, "mode") == 0 && count > at + 1) {
            /* sway's three: there and taking room, there while the modifier is
             * held, or not there at all. */
            const char *mode = config_expand(words[at + 1]);

            if (strcmp(mode, "dock") == 0)
                sway.config.bar = BAR_DOCK;
            else if (strcmp(mode, "hide") == 0)
                sway.config.bar = BAR_HIDE;
            else if (strcmp(mode, "invisible") == 0)
                sway.config.bar = BAR_INVISIBLE;
            else {
                sway_log("bar mode %s: dock, hide or invisible", mode);
                return;
            }
        } else if (strcmp(what, "status_text") == 0 && count > at + 1) {
            /* Not sway's `status_command`, which runs a program and reads its
             * output: there is no shell pipeline here to run one through, and
             * a bar that started a process to print a clock would be a strange
             * thing on a machine this size.  What it is instead is the same
             * template the background takes -- the two figures a status line
             * usually holds, and the clock, without a program in between. */
            char text[BACKGROUND_TEXT_MAX];

            join(text, sizeof(text), words, at + 1, count);
            unescape(text);

            strlcpy(sway.config.status_text, text,
                    sizeof(sway.config.status_text));
        } else if (strcmp(what, "status_command") == 0) {
            sway_log("bar status_command: no program is run for the bar here; "
                     "`bar status_text \"...\"` takes ${CPU}, ${MEM}, "
                     "${TIME}, ${DATE} and ${BATTERY}");
            return;
        } else {
            sway_log("bar %s: read but not acted on", what);
            return;
        }

        sway_update_usable();
        layout_arrange();
        sway.dirty = 1;
        return;
    }

    if (strcmp(cmd, "focus") == 0 && count > 1) {
        enum direction dir;
        if (direction_from_name(config_expand(words[1]), &dir))
            layout_focus_direction(dir);
        return;
    }

    if (strcmp(cmd, "fullscreen") == 0) {
        struct workspace *ws = ws_current();
        ws->fullscreen = ws->fullscreen ? NULL : sway.focused;
        layout_arrange();
        return;
    }

    /* --- moving --- */

    if (strcmp(cmd, "move") == 0 && count > 1) {
        const char *what = config_expand(words[1]);
        enum direction dir;

        if (direction_from_name(what, &dir)) {
            layout_move_direction(dir);
            return;
        }

        /* `move container to workspace 3`, and the shorter forms of it that
         * sway also accepts. */
        for (int i = 1; i < count; i++)
            if (strcmp(config_expand(words[i]), "workspace") == 0 &&
                i + 1 < count) {
                layout_move_to_workspace(atoi(config_expand(words[i + 1])));
                return;
            }
        return;
    }

    /* --- splitting --- */

    if (strcmp(cmd, "splith") == 0) { layout_split(L_SPLITH); return; }
    if (strcmp(cmd, "splitv") == 0) { layout_split(L_SPLITV); return; }

    if (strcmp(cmd, "split") == 0 && count > 1) {
        const char *how = config_expand(words[1]);

        if (how[0] == 'h')      layout_split(L_SPLITH);
        else if (how[0] == 'v') layout_split(L_SPLITV);
        else                    layout_toggle_split();
        return;
    }

    if (strcmp(cmd, "layout") == 0 && count > 1) {
        const char *how = config_expand(words[1]);

        if (strcmp(how, "splith") == 0)
            layout_set_layout(L_SPLITH);
        else if (strcmp(how, "splitv") == 0)
            layout_set_layout(L_SPLITV);
        else if (strcmp(how, "toggle") == 0)
            layout_set_layout(ws_current()->focus && ws_current()->focus->parent
                              && ws_current()->focus->parent->layout == L_SPLITH
                              ? L_SPLITV : L_SPLITH);
        else
            sway_log("layout %s: this compositor has splith and splitv only",
                     how);
        return;
    }

    /* --- workspaces --- */

    if (strcmp(cmd, "workspace") == 0 && count > 1) {
        const char *which = config_expand(words[1]);

        if (strcmp(which, "next") == 0)
            layout_switch_workspace(sway.current + 2 > MAX_WORKSPACES
                                    ? 1 : sway.current + 2);
        else if (strcmp(which, "prev") == 0)
            layout_switch_workspace(sway.current == 0 ? MAX_WORKSPACES
                                                      : sway.current);
        else
            layout_switch_workspace(atoi(which));
        return;
    }

    /* --- the compositor itself --- */

    if (strcmp(cmd, "reload") == 0) {
        config_load(sway.config_path);
        layout_arrange();
        return;
    }

    if (strcmp(cmd, "exit") == 0) {
        sway.running = 0;
        return;
    }

    if (strcmp(cmd, "nop") == 0)
        return;

    /* --- settings --- */

    if (strcmp(cmd, "set") == 0 && count > 2) {
        char value[128];
        join(value, sizeof(value), words, 2, count);

        /* The name keeps its $ off: `set $mod Mod4` defines "mod". */
        variable_set(words[1][0] == '$' ? words[1] + 1 : words[1], value);

        /* Two variables the compositor itself cares about, so that a screen
         * with no windows on it can say which key opens one. */
        if (strcmp(words[1], "$mod") == 0) {
            uint32_t m = wmodifier_from_name(value);
            if (m)
                sway.config.mod = m;
        }
        if (strcmp(words[1], "$term") == 0)
            strlcpy(sway.config.terminal, value, sizeof(sway.config.terminal));
        return;
    }

    if (strcmp(cmd, "bindsym") == 0 && count > 2) {
        int at = 1;

        /* Options like --release and --to-code come before the keys. */
        while (at < count && words[at][0] == '-' && words[at][1] == '-')
            at++;
        if (at + 1 >= count)
            return;

        char command[COMMAND_MAX];
        join(command, sizeof(command), words, at + 1, count);
        binding_add(words[at], command);
        return;
    }

    if (strcmp(cmd, "default_border") == 0 && count > 1) {
        const char *how = config_expand(words[1]);

        if (strcmp(how, "none") == 0) {
            sway.config.border_width = 0;
            sway.config.title_height = 0;
        } else if (strcmp(how, "pixel") == 0) {
            sway.config.border_width = count > 2 ? atoi(words[2]) : 1;
            sway.config.title_height = 0;
        } else {
            sway.config.border_width = 1;
            sway.config.title_height = 20;
        }
        layout_arrange();
        return;
    }

    /* `input <identifier> <setting> <value>`, which is also what the lines of
     * an `input <identifier> { ... }` block arrive as -- the block hands them
     * here with its own first words put back on the front, the way a bar block
     * does, so both spellings are one implementation.
     *
     * The identifier is read and dropped.  sway needs it because a machine can
     * have several mice and a configuration file names the one it means; this
     * machine has the pointer the kernel found, and there is nothing for a
     * second name to select.  A file that sets the speed for a device this
     * machine has not got therefore sets it for the one it has, which is the
     * behaviour that makes a real configuration file work rather than the one
     * that is strictly right. */
    if (strcmp(cmd, "input") == 0 && count > 2) {
        const char *setting = config_expand(words[2]);
        const char *value   = count > 3 ? config_expand(words[3]) : NULL;

        if (strcmp(setting, "pointer_accel") == 0 && value) {
            int accel;

            if (!parse_hundredths(value, &accel)) {
                sway_log("pointer_accel %s: not a number between -1 and 1",
                         value);
                return;
            }

            if (accel < -100 || accel > 100)
                sway_log("pointer_accel %s: outside -1 to 1, taking the end of "
                         "the range", value);

            sway.config.pointer_accel = accel;
            pointer_speed_apply();
            return;
        }

        if (strcmp(setting, "accel_profile") == 0 && value) {
            /* flat is what this is: one multiplier, no curve.  A file asking
             * for it is already getting it, and one asking for adaptive is
             * asking for the timing a PS/2 mouse cannot give. */
            if (strcmp(value, "flat") != 0)
                sway_log("accel_profile %s: the pointer here is flat -- speed "
                         "is a multiplier, not a curve", value);
            return;
        }

        sway_log("input %s: read but not acted on", setting);
        return;
    }

    /* The cursor's size and colour.
     *
     * Not sway's, because sway has not got them: there they are an X cursor
     * theme, which is a directory of images, a loader for them and a hotspot
     * per shape.  Here the cursor is a shape this compositor draws, so how big
     * and what colour are two numbers rather than a theme -- and being able to
     * say so in the same file as everything else is worth more than the name
     * matching a directive that could not be honoured anyway. */
    if (strcmp(cmd, "cursor") == 0 && count > 2) {
        const char *what = config_expand(words[1]);

        if (strcmp(what, "size") == 0) {
            int size = atoi(config_expand(words[2]));

            sway.config.cursor_size = clamp_cursor_size(size);

            if (size != sway.config.cursor_size)
                sway_log("cursor size %d: kept to %d, the range this shape "
                         "scales over", size, sway.config.cursor_size);
        } else if (strcmp(what, "color") == 0 || strcmp(what, "colour") == 0) {
            sway.config.cursor_colour = parse_colour(words[2], 0xFFFFFF);
        } else {
            sway_log("cursor %s: the cursor has a size and a colour", what);
            return;
        }

        sway.dirty = 1;
        return;
    }

    /* What is written across the background.  ${CPU} and ${MEM} in it are
     * replaced as they change, which is why the text is kept as written and
     * expanded elsewhere: a template that had been expanded once would be a
     * reading from the moment it was set and never move again. */
    if (strcmp(cmd, "background_text") == 0) {
        char text[BACKGROUND_TEXT_MAX];

        join(text, sizeof(text), words, 1, count);
        unescape(text);

        strlcpy(sway.config.background_text, text,
                sizeof(sway.config.background_text));

        sway_update_background_text();
        sway.dirty = 1;
        return;
    }

    if (strcmp(cmd, "gaps") == 0 && count > 2) {
        const char *which = config_expand(words[1]);
        int         value = atoi(config_expand(words[count - 1]));

        if (strcmp(which, "inner") == 0)
            sway.config.gaps_inner = value;
        else if (strcmp(which, "outer") == 0)
            sway.config.gaps_outer = value;
        layout_arrange();
        return;
    }

    if (strcmp(cmd, "client.focused") == 0 && count > 3) {
        sway.config.focused.border      = parse_colour(words[1], 0x4C7899);
        sway.config.focused.background  = parse_colour(words[2], 0x285577);
        sway.config.focused.text        = parse_colour(words[3], 0xFFFFFF);
        if (count > 4)
            sway.config.focused.indicator = parse_colour(words[4], 0x2E9EF4);
        if (count > 5)
            sway.config.focused.child_border = parse_colour(words[5], 0x285577);
        sway.dirty = 1;
        return;
    }

    if ((strcmp(cmd, "client.unfocused") == 0 ||
         strcmp(cmd, "client.focused_inactive") == 0) && count > 3) {
        sway.config.unfocused.border     = parse_colour(words[1], 0x333333);
        sway.config.unfocused.background = parse_colour(words[2], 0x222222);
        sway.config.unfocused.text       = parse_colour(words[3], 0x888888);
        if (count > 5)
            sway.config.unfocused.child_border = parse_colour(words[5], 0x222222);
        sway.dirty = 1;
        return;
    }

    /* `output * bg #101820 solid_color` -- the only output setting that means
     * anything here, since the resolution is the console's and cannot change. */
    if (strcmp(cmd, "output") == 0 && count > 3) {
        if (strcmp(config_expand(words[2]), "bg") == 0 ||
            strcmp(config_expand(words[2]), "background") == 0) {
            sway.config.background = parse_colour(words[3], 0x101820);
            sway.dirty = 1;
            return;
        }
        sway_log("output %s: this compositor has one screen at the size the "
                 "console left it", config_expand(words[1]));
        return;
    }

    /* Accepted and ignored, each for a reason worth stating once. */
    if (strcmp(cmd, "font") == 0) {
        sway_log("font: this compositor draws one 8x16 font and cannot change "
                 "it");
        return;
    }
    if (strcmp(cmd, "floating_modifier") == 0 ||
        strcmp(cmd, "floating_minimum_size") == 0 ||
        strcmp(cmd, "floating_maximum_size") == 0) {
        sway_log("%s: nothing floats here -- there is no pointer to drag a "
                 "window with", cmd);
        return;
    }
    /* `seat <id> xcursor_theme <theme> <size>` is how a real sway configuration
     * asks for a bigger pointer, so the size is taken from it even though the
     * theme cannot be: there is one shape here, drawn rather than loaded, and
     * no image decoder to load another with.  The sizes an X cursor theme ships
     * are multiples of 24, and that is the number this divides by. */
    if (strcmp(cmd, "seat") == 0) {
        /* Looked for by name rather than at a fixed word, because a seat may
         * or may not be named: `seat * xcursor_theme ...` on a line and
         * `xcursor_theme ...` inside a `seat { ... }` block arrive here with
         * different numbers of words in front of the one that matters. */
        for (int i = 1; i < count; i++) {
            if (strcmp(config_expand(words[i]), "xcursor_theme") != 0)
                continue;

            if (i + 2 < count) {
                int pixels = atoi(config_expand(words[i + 2]));

                sway.config.cursor_size = clamp_cursor_size((pixels + 12) / 24);
                sway.dirty = 1;
            }

            sway_log("xcursor_theme: the cursor here is one built-in shape, so "
                     "the theme is read and only the size is kept");
            return;
        }
    }

    if (strcmp(cmd, "seat") == 0) {
        sway_log("seat: there is one seat, and it is this compositor's");
        return;
    }
    if (strcmp(cmd, "workspace_layout") == 0 ||
        strcmp(cmd, "smart_borders") == 0 ||
        strcmp(cmd, "focus_follows_mouse") == 0 ||
        strcmp(cmd, "mouse_warping") == 0 ||
        strcmp(cmd, "hide_edge_borders") == 0 ||
        strcmp(cmd, "include") == 0 ||
        strcmp(cmd, "xwayland") == 0) {
        sway_log("%s: not implemented, ignored", cmd);
        return;
    }

    sway_log("unknown command: %s", cmd);
}

void config_run_command(const char *command)
{
    char  buf[COMMAND_MAX];
    char *words[MAX_WORDS];

    strlcpy(buf, command, sizeof(buf));

    /* Several commands on one line, separated by commas, is sway's syntax and
     * is how a binding does two things at once. */
    char *at = buf;
    while (at) {
        char *comma = strchr(at, ',');
        if (comma)
            *comma = '\0';

        int n = split(at, words, MAX_WORDS);
        if (n > 0)
            run(words, n);

        at = comma ? comma + 1 : NULL;
    }
}

/* ------------------------------------------------------------------ *
 *  Reading the file
 * ------------------------------------------------------------------ */

void config_defaults(void)
{
    struct config *c = &sway.config;

    memset(c, 0, sizeof(*c));

    /* sway's own defaults, so a machine with no configuration file still
     * behaves the way the documentation says. */
    c->mod          = W_MOD_LOGO;
    c->border_width = 1;
    c->title_height = 20;
    c->gaps_inner   = 0;
    c->gaps_outer   = 0;
    c->background   = 0x101820;
    c->bar          = BAR_DOCK;
    c->bar_top      = 1;
    c->pointer_accel = 0;         /* one count to one pixel */

    c->cursor_size   = 1;         /* the shape at the size it is drawn */
    c->cursor_colour = 0xFFFFFF;
    c->background_text[0] = '\0'; /* the key hints, until somebody says else */
    c->status_text[0]     = '\0'; /* the battery and the clock */

    c->focused.border       = 0x4C7899;
    c->focused.background   = 0x285577;
    c->focused.text         = 0xFFFFFF;
    c->focused.indicator    = 0x2E9EF4;
    c->focused.child_border = 0x285577;

    c->unfocused.border       = 0x333333;
    c->unfocused.background   = 0x222222;
    c->unfocused.text         = 0x888888;
    c->unfocused.indicator    = 0x292D2E;
    c->unfocused.child_border = 0x222222;

    strlcpy(c->terminal, "wlterm", sizeof(c->terminal));

    /* The pointer speed is the kernel's and outlives this process, so the
     * default has to be put back rather than assumed: a session that was asked
     * for a fast pointer and a sway started after it with no such line should
     * not inherit one.  Nothing is logged -- a machine with no mouse refuses
     * this, and it has not been asked for anything to complain about. */
    (void)wpointerspeed(W_POINTER_SPEED_DEFAULT);
}

/* A block is a heading and a set of lines that belong to it:
 *
 *     input "type:pointer" {
 *         pointer_accel 0.5
 *     }
 *
 * which says exactly what `input "type:pointer" pointer_accel 0.5` says on one
 * line.  So a block is not parsed as an object; its heading is remembered, and
 * each line inside is run with the heading in front of it.  One command means
 * one thing whether it was written in a block, written on a line, or sent by
 * swaymsg -- which is the whole reason this parser reads all three.
 *
 * A heading nothing understands leaves the prefix empty and the lines inside
 * are skipped, so a real sway configuration file goes through without every
 * line of a block it has that this has not being reported as unknown. */
static int  block_depth;
static char block_prefix[COMMAND_MAX];

static void config_line(char *line)
{
    char *at = line;
    while (*at == ' ' || *at == '\t')
        at++;

    /* A '#' begins a comment only at the start of a line.
     *
     * Anywhere else it is a colour: `client.focused #4c7899 #285577 #ffffff`
     * is most of the appearance section, and treating the first '#' on the
     * line as a comment would silently reduce that to `client.focused` and
     * report it as an unknown command.  sway's own parser draws the line in
     * the same place. */
    if (!*at || *at == '#')
        return;

    if (block_depth > 0) {
        if (strchr(at, '}')) {
            if (--block_depth == 0)
                block_prefix[0] = '\0';
        } else if (strchr(at, '{')) {
            block_depth++;
        } else if (block_prefix[0] && block_depth == 1) {
            char line[COMMAND_MAX];
            wsnprintf(line, sizeof(line), "%s %s", block_prefix, at);
            config_run_command(line);
        }
        return;
    }

    char *brace = strchr(at, '{');
    if (brace) {
        /* The heading, before split() takes the spaces out of it: it goes back
         * in front of every line inside, quotes and all, so `input
         * "type:pointer"` still reads as two words when it gets there. */
        char heading[COMMAND_MAX];

        *brace = '\0';
        block_depth++;

        strlcpy(heading, at, sizeof(heading));
        for (int i = (int)strlen(heading); i > 0; i--) {
            if (heading[i - 1] != ' ' && heading[i - 1] != '\t')
                break;
            heading[i - 1] = '\0';
        }

        char *words[MAX_WORDS];
        int   n = split(at, words, MAX_WORDS);

        if (n > 0 && (strcmp(words[0], "bar") == 0 ||
                      strcmp(words[0], "input") == 0 ||
                      strcmp(words[0], "seat") == 0)) {
            strlcpy(block_prefix, heading, sizeof(block_prefix));
            return;
        }

        if (n > 0)
            sway_log("%s { ... }: read but not acted on", words[0]);
        return;
    }

    config_run_command(at);
}

int config_load(const char *path)
{
    int fd = wopen(path, W_O_RDONLY);
    if (fd < 0)
        return fd;

    /* Bindings are rebuilt rather than added to, so `reload` after deleting a
     * line actually removes that binding. */
    struct binding *b = sway.bindings;
    while (b) {
        struct binding *next = b->next;
        free(b);
        b = next;
    }
    sway.bindings   = NULL;
    variable_count  = 0;
    block_depth     = 0;
    block_prefix[0] = '\0';

    char text[8192];
    int  n = wread(fd, text, sizeof(text) - 1);
    wclose(fd);

    if (n <= 0)
        return -W_EIO;
    text[n] = '\0';

    char *line = text;
    for (int i = 0; i <= n; i++) {
        if (text[i] != '\n' && text[i] != '\0')
            continue;

        text[i] = '\0';
        config_line(line);
        line = text + i + 1;
    }

    sway_log("read %s", path);
    return 0;
}
