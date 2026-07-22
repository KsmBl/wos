/* PS/2 keyboard driver, with the two disciplines a Linux terminal has.
 *
 * In *canonical* mode -- the default -- the IRQ handler translates scancodes
 * to characters and accumulates them in a line buffer, echoing as it goes and
 * honouring backspace.  Only when Enter is pressed does the finished line
 * become visible to readers, so applications get whole lines from a single
 * read() without doing any editing themselves.
 *
 * In *raw* mode every keystroke becomes readable immediately and nothing is
 * echoed.  A program that wants to react to individual keys -- whell, for tab
 * completion -- needs this, and takes on echoing and editing itself in
 * exchange.  Ctrl+letter arrives as the corresponding control code.
 */
#ifndef WOS_KEYBOARD_H
#define WOS_KEYBOARD_H

#include "types.h"
#include "wabi.h"

void keyboard_init(void);

/* Switch between raw and canonical mode.  Anything already typed but not yet
 * submitted is discarded, so a mode change never delivers half a line under
 * the other discipline's rules. */
void keyboard_set_raw(bool raw);
bool keyboard_raw(void);

/* True if input is waiting to be read: a complete line in canonical mode, or
 * any keystroke at all in raw mode. */
bool keyboard_has_data(void);

/* Copy up to `max` bytes of completed input into `buf`.
 * Blocks until at least one byte is available.  The trailing '\n' is
 * included, exactly like a canonical-mode read on Linux.
 * Returns the number of bytes copied. */
size_t keyboard_read(char *buf, size_t max);

/* ------------------------------------------------------------------ *
 *  Event mode: the keyboard as a stream of key transitions
 *
 *  A third discipline, above the other two.  A compositor cannot use either of
 *  them: canonical mode gives it lines, raw mode gives it characters, and both
 *  throw away the two things it needs -- that a key was released, and which
 *  physical key it was regardless of what it prints.
 *
 *  While event mode is on the other two are bypassed completely.  Nothing is
 *  echoed and the console reads nothing, because there is one keyboard and the
 *  compositor has it.
 * ------------------------------------------------------------------ */

/* Turn event mode on or off.  Reference counted: the last close puts the
 * keyboard back to the discipline it had.  Anything half-typed is dropped at
 * each transition, the same way a raw/canonical switch drops it. */
void keyboard_events_ref(void);
void keyboard_events_unref(void);

/* True when some process has the keyboard in event mode. */
bool keyboard_events_active(void);

/* True if at least one whole event is waiting. */
bool keyboard_events_pending(void);

/* Copy up to `max` events out, blocking until at least one arrives.  Returns
 * how many were written. */
int keyboard_read_events(winput_t *out, int max);

/* Put an event into that same stream from another driver.  The mouse uses it:
 * one device as far as a reader is concerned, so that a click and the motion
 * that led to it arrive in the order they happened. */
void keyboard_push_input(const winput_t *e);

/* The modifiers held right now, for a driver filling in an event of its own. */
uint32_t keyboard_modifiers(void);

#endif /* WOS_KEYBOARD_H */
