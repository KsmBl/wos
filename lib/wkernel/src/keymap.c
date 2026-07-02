/* Key codes, key names and the characters they make.
 *
 * On Linux this is xkbcommon's job, and it is a large job: a keymap is a file
 * in its own language, describing several layouts, dead keys, compose
 * sequences and the rules for switching between them.  WOS has one layout and
 * no interpreter for that language, so the three questions anybody actually
 * asks are answered directly here:
 *
 *     what key is "Return"          -- a compositor reading `bindsym`
 *     what does key 30 print        -- a terminal turning keys into text
 *     what is key 30 called         -- anything explaining itself to a person
 *
 * The codes are the Linux evdev codes that wl_keyboard.key carries, so these
 * answers are about the same numbers a real Wayland client would see.  The
 * names are the X11 keysym names sway's configuration file is written in, so
 * `bindsym $mod+Return exec wlterm` means here what it means there.
 *
 * A program that wants the real thing can still have it: nothing here is on
 * the wire, and a compositor that one day ships an XKB keymap would send it
 * through wl_keyboard.keymap and leave this to the programs that never asked.
 */

#include <wkernel.h>

/* The main block, in evdev order.  The code *is* the index: evdev numbered
 * these keys from the AT set 1 scancodes, and the scancodes were assigned by
 * walking the keyboard, so the table is the keyboard read left to right and
 * top to bottom. */
static const char *const unshifted[] = {
    /*  0 */ NULL,        "Escape",     "1",          "2",
    /*  4 */ "3",         "4",          "5",          "6",
    /*  8 */ "7",         "8",          "9",          "0",
    /* 12 */ "minus",     "equal",      "BackSpace",  "Tab",
    /* 16 */ "q",         "w",          "e",          "r",
    /* 20 */ "t",         "y",          "u",          "i",
    /* 24 */ "o",         "p",          "bracketleft","bracketright",
    /* 28 */ "Return",    "Control_L",  "a",          "s",
    /* 32 */ "d",         "f",          "g",          "h",
    /* 36 */ "j",         "k",          "l",          "semicolon",
    /* 40 */ "apostrophe","grave",      "Shift_L",    "backslash",
    /* 44 */ "z",         "x",          "c",          "v",
    /* 48 */ "b",         "n",          "m",          "comma",
    /* 52 */ "period",    "slash",      "Shift_R",    "KP_Multiply",
    /* 56 */ "Alt_L",     "space",      "Caps_Lock",  "F1",
    /* 60 */ "F2",        "F3",         "F4",         "F5",
    /* 64 */ "F6",        "F7",         "F8",         "F9",
    /* 68 */ "F10",       "Num_Lock",   "Scroll_Lock","KP_7",
    /* 72 */ "KP_8",      "KP_9",       "KP_Subtract","KP_4",
    /* 76 */ "KP_5",      "KP_6",       "KP_Add",     "KP_1",
    /* 80 */ "KP_2",      "KP_3",       "KP_0",       "KP_Decimal",
    /* 84 */ NULL,        NULL,         NULL,         "F11",
    /* 88 */ "F12",
};

/* The keys behind an 0xE0 prefix, which evdev numbered separately. */
static const struct {
    uint32_t    code;
    const char *name;
} extended[] = {
    {  96, "KP_Enter"  }, {  97, "Control_R" }, {  98, "KP_Divide" },
    { 100, "Alt_R"     }, { 102, "Home"      }, { 103, "Up"        },
    { 104, "Prior"     }, { 105, "Left"      }, { 106, "Right"     },
    { 107, "End"       }, { 108, "Down"      }, { 109, "Next"      },
    { 110, "Insert"    }, { 111, "Delete"    }, { 125, "Super_L"   },
    { 126, "Super_R"   }, { 127, "Menu"      },
};

/* What each key prints, and what it prints with Shift held.  Indexed the same
 * way, and empty where a key prints nothing. */
static const char plain[] =
    "\0\0" "1234567890-=" "\b\t"
    "qwertyuiop[]" "\n\0"
    "asdfghjkl;'`" "\0" "\\"
    "zxcvbnm,./" "\0" "*" "\0" " ";

static const char shifted[] =
    "\0\0" "!@#$%^&*()_+" "\b\t"
    "QWERTYUIOP{}" "\n\0"
    "ASDFGHJKL:\"~" "\0" "|"
    "ZXCVBNM<>?" "\0" "*" "\0" " ";

#define PLAIN_KEYS ((uint32_t)(sizeof(plain) - 1))

const char *wkeyname(uint32_t keycode)
{
    if (keycode < sizeof(unshifted) / sizeof(unshifted[0]) &&
        unshifted[keycode])
        return unshifted[keycode];

    for (unsigned i = 0; i < sizeof(extended) / sizeof(extended[0]); i++)
        if (extended[i].code == keycode)
            return extended[i].name;

    return NULL;
}

/* Case-insensitive, because a configuration file written by a person says
 * "Return" in one line and "return" in the next and means the same key. */
static int same_key(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

uint32_t wkeycode_from_name(const char *name)
{
    if (!name || !name[0])
        return 0;

    for (uint32_t i = 1; i < sizeof(unshifted) / sizeof(unshifted[0]); i++)
        if (unshifted[i] && same_key(unshifted[i], name))
            return i;

    for (unsigned i = 0; i < sizeof(extended) / sizeof(extended[0]); i++)
        if (same_key(extended[i].name, name))
            return extended[i].code;

    /* A shifted character names the key that prints it, which is how
     * `bindsym $mod+Shift+Q kill` finds the q key. */
    if (!name[1]) {
        for (uint32_t i = 1; i < PLAIN_KEYS; i++)
            if (shifted[i] == name[0] || plain[i] == name[0])
                return i;
    }

    return 0;
}

uint32_t wkeychar(uint32_t keycode, uint32_t mods)
{
    if (keycode >= PLAIN_KEYS)
        return 0;

    char c = (mods & W_MOD_SHIFT) ? shifted[keycode] : plain[keycode];
    if (c == 0)
        return 0;

    /* Caps lock inverts what Shift decided, and only for letters. */
    if (mods & W_MOD_CAPS) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }

    /* Ctrl turns a letter into its control code, the way a terminal does. */
    if (mods & W_MOD_CTRL) {
        if (c >= 'a' && c <= 'z')
            return (uint32_t)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z')
            return (uint32_t)(c - 'A' + 1);
        return 0;
    }

    return (uint32_t)(unsigned char)c;
}

uint32_t wmodifier_from_name(const char *name)
{
    if (same_key(name, "Shift"))                          return W_MOD_SHIFT;
    if (same_key(name, "Ctrl") || same_key(name, "Control"))
        return W_MOD_CTRL;
    if (same_key(name, "Alt") || same_key(name, "Mod1"))  return W_MOD_ALT;
    if (same_key(name, "Mod4") || same_key(name, "Super") ||
        same_key(name, "Logo"))
        return W_MOD_LOGO;
    if (same_key(name, "Lock") || same_key(name, "Caps")) return W_MOD_CAPS;
    return 0;
}
