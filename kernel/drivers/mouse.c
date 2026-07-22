/* The PS/2 mouse, on the second port of the same controller the keyboard is on.
 *
 * Two devices, one controller, one status register.  The keyboard driver owns
 * the ports; this owns the second interrupt, and the two are told apart by
 * where they arrive rather than by anything in the byte -- a mouse byte is
 * indistinguishable from a scancode, so IRQ 12 is the only thing that says
 * which device sent it.
 *
 * The controller is a device from 1984 and it shows.  Anything sent to the
 * mouse rather than to the controller has to be introduced with 0xD4; the mouse
 * acknowledges every byte with 0xFA and the acknowledgement arrives on the same
 * data port everything else does; and enabling the second port at all means a
 * read-modify-write of a configuration byte that also controls the keyboard.
 * All of that happens at boot, before interrupts are unmasked, so the replies
 * are read by polling and never collide with the handler.
 *
 * Movement arrives as three bytes, or four with a wheel.  They are relative and
 * signed, and the sign lives in the first byte rather than in the value, which
 * is the detail that makes a naive driver's pointer jump to a corner.  The
 * absolute position is kept here rather than in the compositor because only the
 * kernel knows how big the screen is -- and because a pointer that can leave
 * the screen is one nobody can get back.
 */

#include "mouse.h"
#include "keyboard.h"
#include "display.h"
#include "io.h"
#include "isr.h"
#include "pic.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_FROM_MOUSE  0x20

#define CMD_READ_CONFIG    0x20
#define CMD_WRITE_CONFIG   0x60
#define CMD_ENABLE_MOUSE   0xA8
#define CMD_TO_MOUSE       0xD4

#define MOUSE_SET_SAMPLE   0xF3
#define MOUSE_GET_ID       0xF2
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE       0xF4
#define MOUSE_ACK          0xFA

static bool present;
static bool has_wheel;
static int  screen_w = 640, screen_h = 400;

/* Where the pointer is.  Kept in whole pixels; the mouse reports in its own
 * counts and one count is taken as one pixel, which at the default resolution
 * of these packets is about right and is what a machine with no acceleration
 * setting can honestly do. */
static int32_t pointer_x, pointer_y;
static uint32_t buttons_held;

bool mouse_present(void) { return present; }

void mouse_position(int32_t *x, int32_t *y)
{
    *x = pointer_x;
    *y = pointer_y;
}

/* ------------------------------------------------------------------ *
 *  Talking to it, at boot
 * ------------------------------------------------------------------ */

static bool wait_writable(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL))
            return true;
    return false;
}

static bool wait_readable(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    return false;
}

static bool controller_command(uint8_t command)
{
    if (!wait_writable())
        return false;
    outb(PS2_CMD, command);
    return true;
}

static bool controller_write(uint8_t value)
{
    if (!wait_writable())
        return false;
    outb(PS2_DATA, value);
    return true;
}

static int controller_read(void)
{
    if (!wait_readable())
        return -1;
    return inb(PS2_DATA);
}

/* One byte to the mouse rather than to the controller, and its acknowledgement.
 * Every command is acknowledged with 0xFA, and a device that does not is one
 * that is not there. */
static bool mouse_command(uint8_t value)
{
    if (!controller_command(CMD_TO_MOUSE) || !controller_write(value))
        return false;

    return controller_read() == MOUSE_ACK;
}

/* The scroll wheel is not advertised; it is unlocked.  Setting the sample rate
 * to 200, then 100, then 80 is a knock that an IntelliMouse answers by changing
 * its device id to 3 and its packets to four bytes.  A plain mouse ignores the
 * sequence and keeps saying 0, which is exactly what we want to find out. */
static bool enable_wheel(void)
{
    static const uint8_t knock[] = { 200, 100, 80 };

    for (int i = 0; i < 3; i++) {
        if (!mouse_command(MOUSE_SET_SAMPLE) || !mouse_command(knock[i]))
            return false;
    }

    if (!mouse_command(MOUSE_GET_ID))
        return false;

    return controller_read() == 3;
}

/* ------------------------------------------------------------------ *
 *  Packets
 * ------------------------------------------------------------------ */

static uint8_t packet[4];
static int     packet_at;

static void deliver_motion(int32_t dx, int32_t dy)
{
    winput_t e;

    memset(&e, 0, sizeof(e));
    e.type    = W_INPUT_POINTER_MOTION;
    e.mods    = keyboard_modifiers();
    e.time_ms = pit_uptime_ms();
    e.x       = pointer_x;
    e.y       = pointer_y;
    e.dx      = dx;
    e.dy      = dy;

    keyboard_push_input(&e);
}

static void deliver_button(uint32_t code, uint32_t state)
{
    winput_t e;

    memset(&e, 0, sizeof(e));
    e.type    = W_INPUT_POINTER_BUTTON;
    e.code    = code;
    e.state   = state;
    e.mods    = keyboard_modifiers();
    e.time_ms = pit_uptime_ms();
    e.x       = pointer_x;
    e.y       = pointer_y;

    keyboard_push_input(&e);
}

static void deliver_axis(int32_t steps)
{
    winput_t e;

    memset(&e, 0, sizeof(e));
    e.type    = W_INPUT_POINTER_AXIS;
    e.code    = W_AXIS_VERTICAL;
    e.mods    = keyboard_modifiers();
    e.time_ms = pit_uptime_ms();
    e.x       = pointer_x;
    e.y       = pointer_y;
    e.dy      = steps;

    keyboard_push_input(&e);
}

static void handle_packet(void)
{
    uint8_t flags = packet[0];

    /* Bit 3 is always set in a first byte.  When it is not, the stream has
     * lost its place -- which happens if bytes were dropped -- and the only
     * recovery is to throw this one away and resynchronise on the next. */
    if (!(flags & 0x08)) {
        packet_at = 0;
        return;
    }

    /* Overflow means the mouse moved further between packets than the field
     * can express.  The value is meaningless rather than large, so the axis is
     * left alone instead of being believed. */
    int32_t dx = (flags & 0x40) ? 0 : (int32_t)packet[1];
    int32_t dy = (flags & 0x80) ? 0 : (int32_t)packet[2];

    /* The sign is in the flags, not in the byte: a nine-bit value split in
     * two.  Sign-extending the byte on its own gives a pointer that leaps
     * across the screen on every leftward movement. */
    if (dx && (flags & 0x10))
        dx -= 256;
    if (dy && (flags & 0x20))
        dy -= 256;

    /* The mouse's Y counts up as it moves away from the user; the screen's
     * counts down.  One of the two has to be flipped and it is this one. */
    dy = -dy;

    if (dx || dy) {
        pointer_x += dx;
        pointer_y += dy;

        if (pointer_x < 0)              pointer_x = 0;
        if (pointer_y < 0)              pointer_y = 0;
        if (pointer_x > screen_w - 1)   pointer_x = screen_w - 1;
        if (pointer_y > screen_h - 1)   pointer_y = screen_h - 1;

        deliver_motion(dx, dy);
    }

    /* Buttons are levels rather than edges, so an event is what changed. */
    static const uint32_t codes[] = { W_BTN_LEFT, W_BTN_RIGHT, W_BTN_MIDDLE };
    uint32_t now = flags & 0x07;

    for (int i = 0; i < 3; i++) {
        uint32_t bit = 1u << i;

        if ((now & bit) != (buttons_held & bit))
            deliver_button(codes[i], (now & bit) ? 1 : 0);
    }

    buttons_held = now;

    if (has_wheel) {
        /* Four bits, signed, and the upper nibble carries the fourth and
         * fifth buttons which nothing here uses. */
        int32_t wheel = (int32_t)(packet[3] & 0x0F);

        if (wheel & 0x08)
            wheel -= 16;

        if (wheel)
            deliver_axis(wheel);
    }
}

static void mouse_irq(regs_t *regs)
{
    (void)regs;

    /* Reading the status first is what distinguishes a mouse byte from a
     * keyboard one on a controller that shares a data port. */
    uint8_t status = inb(PS2_STATUS);

    if (!(status & STATUS_OUTPUT_FULL))
        return;

    uint8_t byte = inb(PS2_DATA);

    if (!(status & STATUS_FROM_MOUSE))
        return;

    packet[packet_at++] = byte;

    if (packet_at >= (has_wheel ? 4 : 3)) {
        packet_at = 0;
        handle_packet();
    }
}

/* ------------------------------------------------------------------ *
 *  Setting up
 * ------------------------------------------------------------------ */

/* The handshake, which must not be interrupted -- see mouse_init(). */
static bool bring_up(void)
{
    /* The second port on, and its interrupt enabled in the configuration byte
     * -- which also holds the keyboard's settings, so it is read, one bit is
     * changed, and it goes back. */
    if (!controller_command(CMD_ENABLE_MOUSE))
        return false;

    if (!controller_command(CMD_READ_CONFIG))
        return false;

    int config = controller_read();
    if (config < 0)
        return false;

    config |= 0x02;                    /* second port interrupt */
    config &= ~0x20;                   /* second port clock on  */

    if (!controller_command(CMD_WRITE_CONFIG) ||
        !controller_write((uint8_t)config))
        return false;

    if (!mouse_command(MOUSE_SET_DEFAULTS))
        return false;                  /* nothing acknowledged: no mouse */

    has_wheel = enable_wheel();

    return mouse_command(MOUSE_ENABLE);
}

void mouse_init(void)
{
    if (inb(PS2_STATUS) == 0xFF)
        return;                        /* no controller at all */

    /* Interrupts off for the whole handshake.
     *
     * The keyboard and the mouse share one data port, and the controller
     * raises the keyboard's interrupt when it has a byte for us.  Every reply
     * read here -- the configuration byte, every 0xFA -- would otherwise be
     * taken by keyboard_irq() and handed to the line discipline as a
     * scancode.  That is exactly what happened: the configuration read timed
     * out because the keyboard driver had already eaten the answer. */
    cli();

    bool ok = bring_up();

    /* Anything the controller was left holding is not a scancode, and letting
     * the keyboard handler have it would type a character nobody pressed. */
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++)
        (void)inb(PS2_DATA);

    sti();

    if (!ok)
        return;

    /* Start in the middle of the screen, which is where a pointer that has
     * never moved should be -- a corner looks like a pointer that is stuck. */
    wdisplay_t screen;
    display_info(&screen);

    if (screen.present && screen.width && screen.height) {
        screen_w = (int)screen.width;
        screen_h = (int)screen.height;
    }

    pointer_x = screen_w / 2;
    pointer_y = screen_h / 2;

    present = true;
    register_interrupt_handler(IRQ_MOUSE, mouse_irq);
    pic_clear_mask(12);

    kprintf("mouse  : PS/2 pointer%s, %dx%d\n",
            has_wheel ? " with a wheel" : "", screen_w, screen_h);
}

void mouse_screen_size(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    screen_w = width;
    screen_h = height;

    if (pointer_x > screen_w - 1) pointer_x = screen_w - 1;
    if (pointer_y > screen_h - 1) pointer_y = screen_h - 1;
}
