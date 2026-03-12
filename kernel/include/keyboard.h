/* PS/2 keyboard driver with a canonical (line-buffered) discipline.
 *
 * The IRQ handler translates scancodes to characters and accumulates them in a
 * line buffer, echoing as it goes and honouring backspace.  Only when Enter is
 * pressed does the finished line become visible to readers.  That is the same
 * behaviour a Linux terminal has in canonical mode, and it means applications
 * get whole lines from a single read() without doing any editing themselves.
 */
#ifndef WOS_KEYBOARD_H
#define WOS_KEYBOARD_H

#include "types.h"

void keyboard_init(void);

/* True if at least one complete line is waiting to be read. */
bool keyboard_has_data(void);

/* Copy up to `max` bytes of completed input into `buf`.
 * Blocks until at least one byte is available.  The trailing '\n' is
 * included, exactly like a canonical-mode read on Linux.
 * Returns the number of bytes copied. */
size_t keyboard_read(char *buf, size_t max);

#endif /* WOS_KEYBOARD_H */
