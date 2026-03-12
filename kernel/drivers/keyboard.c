/* PS/2 keyboard driver, scancode set 1, canonical line discipline. */

#include "keyboard.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "kprintf.h"

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
static bool caps_lock;

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
}

static void handle_char(char c)
{
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

static void keyboard_irq(regs_t *regs)
{
    (void)regs;

    uint8_t sc = inb(KBD_DATA);

    /* 0xE0 introduces a two-byte code (arrows, right ctrl, ...). We consume
     * the prefix and ignore the pair rather than mis-decoding the second byte
     * as a single-byte scancode. */
    static bool extended;
    if (sc == 0xE0) {
        extended = true;
        return;
    }
    if (extended) {
        extended = false;
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

    /* Ctrl+letter produces the corresponding control code; Ctrl+C discards
     * the line being typed, like a terminal would. */
    if (ctrl_down) {
        if (c == 'c' || c == 'C') {
            kputs("^C\n");
            line_len = 0;
            line_commit();
        }
        return;
    }

    handle_char(c);
}

size_t keyboard_read(char *buf, size_t max)
{
    if (max == 0)
        return 0;

    /* Wait for the IRQ handler to publish something. hlt parks the CPU until
     * the next interrupt, so this costs nothing while idle. */
    while (!keyboard_has_data()) {
        sti();
        __asm__ volatile("hlt");
    }

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
     * otherwise the first IRQ arrives with stale data. */
    while (inb(KBD_STATUS) & 0x01)
        (void)inb(KBD_DATA);

    register_interrupt_handler(IRQ_KEYBOARD, keyboard_irq);
    pic_clear_mask(1);
}
