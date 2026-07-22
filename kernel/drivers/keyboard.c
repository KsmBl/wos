/* PS/2 keyboard driver, scancode set 1, canonical line discipline. */

#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "kprintf.h"
#include "sched.h"
#include "pit.h"
#include "proc.h"
#include "mouse.h"
#include "string.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define SC_RELEASE  0x80    /* set in the scancode when a key comes back up */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D
#define SC_ALT      0x38
#define SC_CAPSLOCK 0x3A

#define LINE_MAX  256
#define RING_SIZE 1024

/* Scancode -> character, for set 1 make codes 0x00-0x7F.
 * Zero means "no printable character" (modifiers, function keys, ...). */
static const char keymap[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          /* F1-F10        */
    0, 0,                                   /* num, scroll   */
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    /* the remainder is zero-filled */
};

static const char keymap_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

static bool shift_down;
static bool ctrl_down;
static bool alt_down;
static bool logo_down;
static bool caps_lock;
static bool raw_mode;

/* Event mode: how many descriptors have the keyboard as a stream of key
 * transitions, and the transitions themselves.
 *
 * The ring is short on purpose.  It holds a fraction of a second of typing,
 * and a compositor that has fallen far enough behind to fill it has a worse
 * problem than the keystrokes it is about to lose. */
#define EVENT_RING 128

static int  event_refs;
static volatile winput_t event_ring[EVENT_RING];
static volatile size_t   event_head, event_tail;

/* The line currently being typed; not visible to readers until Enter. */
static char   line[LINE_MAX];
static size_t line_len;

/* Completed lines waiting to be consumed. */
static volatile char   ring[RING_SIZE];
static volatile size_t ring_head;    /* next write position */
static volatile size_t ring_tail;    /* next read position  */

static void ring_push(char c)
{
    size_t next = (ring_head + 1) % RING_SIZE;
    if (next == ring_tail)
        return;                      /* full: drop, better than corrupting */
    ring[ring_head] = c;
    ring_head = next;
}

static void ring_push_string(const char *s)
{
    while (*s)
        ring_push(*s++);
}

bool keyboard_has_data(void)
{
    return ring_head != ring_tail;
}

/* Commit the current line to the ring buffer and start a fresh one. */
static void line_commit(void)
{
    for (size_t i = 0; i < line_len; i++)
        ring_push(line[i]);
    ring_push('\n');
    line_len = 0;

    /* Anything waiting on console input can now make progress. */
    sched_wake(WAIT_INPUT);
}

static void handle_char(char c)
{
    /* Raw mode: publish the keystroke as it is and let the reader decide what
     * it means.  No echo, no line buffer, no editing. */
    if (raw_mode) {
        ring_push(c);
        sched_wake(WAIT_INPUT);
        return;
    }

    switch (c) {
    case '\n':
        kputc('\n');
        line_commit();
        break;
    case '\b':
        if (line_len > 0) {
            line_len--;
            kputc('\b');            /* the console erases as it backs up */
        }
        break;
    default:
        if (line_len + 1 < LINE_MAX) {
            line[line_len++] = c;
            kputc(c);
        }
        break;
    }
}

/* ------------------------------------------------------------------ *
 *  Event mode
 * ------------------------------------------------------------------ */

/* What the second byte of a two-byte scancode means, as an evdev key code.
 *
 * The single-byte codes need no table at all: the evdev numbers for the main
 * block of the keyboard were defined from these very scancodes, so the code
 * and the scancode are the same number.  Only the keys that arrive behind an
 * 0xE0 were given numbers somewhere else, and this is that list -- including
 * the Super key, which the console has never had a use for and which is the
 * modifier a tiling compositor is mostly driven by. */
static uint32_t extended_keycode(uint8_t sc)
{
    switch (sc) {
    case 0x1C: return 96;    /* KEY_KPENTER    */
    case 0x1D: return 97;    /* KEY_RIGHTCTRL  */
    case 0x35: return 98;    /* KEY_KPSLASH    */
    case 0x38: return 100;   /* KEY_RIGHTALT   */
    case 0x47: return 102;   /* KEY_HOME       */
    case 0x48: return 103;   /* KEY_UP         */
    case 0x49: return 104;   /* KEY_PAGEUP     */
    case 0x4B: return 105;   /* KEY_LEFT       */
    case 0x4D: return 106;   /* KEY_RIGHT      */
    case 0x4F: return 107;   /* KEY_END        */
    case 0x50: return 108;   /* KEY_DOWN       */
    case 0x51: return 109;   /* KEY_PAGEDOWN   */
    case 0x52: return 110;   /* KEY_INSERT     */
    case 0x53: return 111;   /* KEY_DELETE     */
    case 0x5B: return 125;   /* KEY_LEFTMETA   */
    case 0x5C: return 126;   /* KEY_RIGHTMETA  */
    case 0x5D: return 127;   /* KEY_COMPOSE    */
    default:   return 0;     /* nothing this keyboard layer knows */
    }
}

static uint32_t modifier_mask(void)
{
    uint32_t m = 0;

    if (shift_down) m |= W_MOD_SHIFT;
    if (caps_lock)  m |= W_MOD_CAPS;
    if (ctrl_down)  m |= W_MOD_CTRL;
    if (alt_down)   m |= W_MOD_ALT;
    if (logo_down)  m |= W_MOD_LOGO;
    return m;
}

/* The character a key would produce, or 0.  Sent alongside the key code so a
 * program that only wants text does not have to carry a keymap; a compositor
 * that does carry one ignores it and uses the code. */
static uint32_t key_unicode(uint8_t sc)
{
    if (sc >= 128)
        return 0;

    char c = shift_down ? keymap_shift[sc] : keymap[sc];
    if (c == 0)
        return 0;

    if (caps_lock) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }

    /* Ctrl turns a letter into its control code, the way a terminal does, so
     * Ctrl+C reaches a program inside a window as 0x03. */
    if (ctrl_down) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        else
            return 0;
    }

    return (uint32_t)(unsigned char)c;
}

static void event_push(uint32_t code, bool pressed, uint32_t unicode)
{
    size_t next = (event_head + 1) % EVENT_RING;
    if (next == event_tail)
        return;                     /* full: drop the oldest end of the type */

    /* Cleared before it is filled, because the slot holds whatever event was
     * in it a ring ago.  Without this a key event would carry the pointer
     * position from some earlier motion, which is worse than carrying none. */
    winput_t e;
    memset(&e, 0, sizeof(e));

    /* Where the pointer is, so a client that wants to know where the mouse
     * was when a key was pressed does not have to track it. */
    mouse_position(&e.x, &e.y);

    e.type    = W_INPUT_KEY;
    e.code    = code;
    e.state   = pressed ? 1 : 0;
    e.mods    = modifier_mask();
    e.unicode = unicode;
    e.time_ms = pit_uptime_ms();

    event_ring[event_head] = e;
    event_head = next;
    sched_wake(WAIT_INPUT);
}

/* The same ring, filled by the mouse.
 *
 * One stream for both devices, because a compositor wants them interleaved in
 * the order they happened: a click that arrives before the motion that led to
 * it lands on whatever used to be under the pointer.  Two rings could not
 * promise that, and the reader would have to merge them by timestamp.
 *
 * The event is dropped when nobody is in event mode, for the same reason a
 * keystroke is: the console has nothing to do with a pointer, and a ring
 * filling up behind a console that will never read it is a ring that loses the
 * events of whoever opens the device next. */
void keyboard_push_input(const winput_t *e)
{
    if (event_refs == 0)
        return;

    size_t next = (event_head + 1) % EVENT_RING;
    if (next == event_tail)
        return;

    event_ring[event_head] = *e;
    event_head = next;
    sched_wake(WAIT_INPUT);
}

/* What is held right now, for an event the mouse is filling in.  A click while
 * Shift is down has to say so, and only this file knows. */
uint32_t keyboard_modifiers(void)
{
    return modifier_mask();
}

/* One scancode, in event mode.  Every key produces an event, modifiers
 * included and releases included -- that is the whole reason for this mode. */
static void handle_event(uint8_t sc, bool extended)
{
    bool    pressed = !(sc & SC_RELEASE);
    uint8_t made    = (uint8_t)(sc & ~SC_RELEASE);

    uint32_t code = extended ? extended_keycode(made) : made;
    if (code == 0)
        return;

    /* The modifier state has to be up to date before the event is queued, so
     * that a press of `c` while Ctrl is held reports Ctrl as held. */
    if (extended) {
        if (code == 97)  ctrl_down = pressed;    /* right control */
        if (code == 100) alt_down  = pressed;    /* right alt     */
        if (code == 125 || code == 126)
            logo_down = pressed;
    } else {
        switch (made) {
        case SC_LSHIFT:
        case SC_RSHIFT:   shift_down = pressed; break;
        case SC_CTRL:     ctrl_down  = pressed; break;
        case SC_ALT:      alt_down   = pressed; break;
        case SC_CAPSLOCK: if (pressed) caps_lock = !caps_lock; break;
        default: break;
        }
    }

    event_push(code, pressed, extended ? 0 : (pressed ? key_unicode(made) : 0));
}

static void keyboard_irq(regs_t *regs)
{
    (void)regs;

    uint8_t sc = inb(KBD_DATA);

    /* 0xE0 introduces a two-byte code: the arrows, Home, End, Delete and the
     * right-hand modifiers. */
    static bool extended;
    if (sc == 0xE0) {
        extended = true;
        return;
    }

    /* Event mode comes first, and takes everything.  The disciplines below are
     * about turning keys into text, which is exactly what a compositor is not
     * asking for. */
    if (event_refs > 0) {
        bool was_extended = extended;
        extended = false;
        handle_event(sc, was_extended);
        return;
    }

    if (extended) {
        extended = false;

        /* Only presses matter, and only a raw reader has any use for these;
         * in canonical mode the driver is assembling a line and an arrow key
         * has no meaning within it. */
        if ((sc & SC_RELEASE) || !raw_mode)
            return;

        /* Deliver them as the escape sequences a terminal would send, so a
         * program decodes special keys the same way whether it is reading
         * from this console or from a serial terminal. */
        switch (sc) {
        case 0x48: ring_push_string("\033[A");  break;   /* up     */
        case 0x50: ring_push_string("\033[B");  break;   /* down   */
        case 0x4D: ring_push_string("\033[C");  break;   /* right  */
        case 0x4B: ring_push_string("\033[D");  break;   /* left   */
        case 0x47: ring_push_string("\033[H");  break;   /* home   */
        case 0x4F: ring_push_string("\033[F");  break;   /* end    */
        case 0x49: ring_push_string("\033[5~"); break;   /* pg up  */
        case 0x51: ring_push_string("\033[6~"); break;   /* pg dn  */
        case 0x53: ring_push_string("\033[3~"); break;   /* delete */
        default:   return;                               /* not interesting */
        }

        sched_wake(WAIT_INPUT);
        return;
    }

    if (sc & SC_RELEASE) {
        uint8_t made = (uint8_t)(sc & ~SC_RELEASE);
        if (made == SC_LSHIFT || made == SC_RSHIFT)
            shift_down = false;
        else if (made == SC_CTRL)
            ctrl_down = false;
        return;
    }

    switch (sc) {
    case SC_LSHIFT:
    case SC_RSHIFT:   shift_down = true;      return;
    case SC_CTRL:     ctrl_down = true;       return;
    case SC_ALT:                              return;
    case SC_CAPSLOCK: caps_lock = !caps_lock; return;
    default: break;
    }

    /* The function keys, which are single-byte scancodes rather than the 0xE0
     * pairs the arrows arrive as -- so they are decoded here rather than up
     * there, but delivered the same way, as the sequences a terminal sends.
     * F1-F4 have short forms in every terminal; the rest are numbered, and
     * with a gap at 16 that VT220s left and nobody has filled since. */
    if (!(sc & SC_RELEASE) && raw_mode) {
        static const char *const fkeys[] = {
            "\033OP",   "\033OQ",   "\033OR",   "\033OS",     /* F1-F4   */
            "\033[15~", "\033[17~", "\033[18~", "\033[19~",   /* F5-F8   */
            "\033[20~", "\033[21~",                           /* F9, F10 */
        };

        if (sc >= 0x3B && sc <= 0x44) {
            ring_push_string(fkeys[sc - 0x3B]);
            sched_wake(WAIT_INPUT);
            return;
        }
        if (sc == 0x57 || sc == 0x58) {           /* F11, F12 */
            ring_push_string(sc == 0x57 ? "\033[23~" : "\033[24~");
            sched_wake(WAIT_INPUT);
            return;
        }
    }

    char c = shift_down ? keymap_shift[sc] : keymap[sc];
    if (c == 0)
        return;

    /* Caps lock affects letters only, and inverts whatever shift decided. */
    if (caps_lock) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }

    if (ctrl_down) {
        /* Raw readers get the control code and decide for themselves; in
         * canonical mode the driver acts on Ctrl+C by discarding the line
         * being typed, the way a terminal would. */
        if (raw_mode) {
            if (c >= 'a' && c <= 'z')
                handle_char((char)(c - 'a' + 1));
            else if (c >= 'A' && c <= 'Z')
                handle_char((char)(c - 'A' + 1));
            return;
        }

        if (c == 'c' || c == 'C') {
            kputs("^C\n");
            line_len = 0;
            line_commit();
        }
        return;
    }

    handle_char(c);
}

void keyboard_events_ref(void)
{
    if (event_refs++ == 0) {
        /* Everything half-typed under the old discipline goes, exactly as a
         * raw/canonical switch drops it: a line assembled for the console is
         * not something to hand to a compositor. */
        line_len   = 0;
        ring_head  = ring_tail;
        event_head = event_tail;

        /* No key can be known to be held: they were pressed while nobody was
         * watching for releases. */
        shift_down = ctrl_down = alt_down = logo_down = false;
    }
}

void keyboard_events_unref(void)
{
    if (event_refs > 0 && --event_refs == 0) {
        event_head = event_tail;
        line_len   = 0;
        ring_head  = ring_tail;
        shift_down = ctrl_down = alt_down = logo_down = false;
    }
}

bool keyboard_events_active(void)
{
    return event_refs > 0;
}

bool keyboard_events_pending(void)
{
    return event_head != event_tail;
}

int keyboard_read_events(winput_t *out, int max)
{
    if (max <= 0)
        return 0;

    while (!keyboard_events_pending()) {
        sched_block(WAIT_INPUT);

        /* A process asked to stop while waiting for a key would otherwise wait
         * for one that is never going to come. */
        if (proc_should_exit())
            return 0;
    }

    int n = 0;
    while (n < max && keyboard_events_pending()) {
        out[n++]   = *(const winput_t *)&event_ring[event_tail];
        event_tail = (event_tail + 1) % EVENT_RING;
    }
    return n;
}

void keyboard_set_raw(bool raw)
{
    if (raw == raw_mode)
        return;

    /* Drop anything half-typed. Handing a partial line to a reader working
     * under the other discipline's rules would only confuse it. */
    line_len  = 0;
    ring_head = ring_tail;

    raw_mode = raw;
}

bool keyboard_raw(void)
{
    return raw_mode;
}

size_t keyboard_read(char *buf, size_t max)
{
    if (max == 0)
        return 0;

    /* Sleep until the IRQ handler publishes a line.  Before the scheduler
     * exists sched_block() falls back to halting the CPU, so this works
     * during early boot too. */
    while (!keyboard_has_data())
        sched_block(WAIT_INPUT);

    size_t n = 0;
    while (n < max && keyboard_has_data()) {
        buf[n++] = ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SIZE;
    }
    return n;
}

void keyboard_init(void)
{
    /* Drain any scancode the firmware left in the controller's buffer,
     * otherwise the first IRQ arrives with stale data.
     *
     * Bounded, and skipped entirely when the status register reads back all
     * ones.  A machine with no PS/2 controller -- which every USB-only laptop
     * is -- answers that port with 0xFF, whose low bit says "a byte is
     * waiting", forever.  Draining it unbounded is an infinite loop early in
     * boot, with nothing on screen yet to say where the kernel stopped. */
    uint8_t status = inb(KBD_STATUS);
    if (status == 0xFF) {
        kputs("kbd    : no PS/2 controller\n");
        return;
    }

    for (int i = 0; i < 64 && (inb(KBD_STATUS) & 0x01); i++)
        (void)inb(KBD_DATA);

    register_interrupt_handler(IRQ_KEYBOARD, keyboard_irq);
    pic_clear_mask(1);
}
