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

#endif /* WOS_KEYBOARD_H */
