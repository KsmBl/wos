/* The screen, as something a process can be given.
 *
 * The framebuffer console owns the display for as long as nobody else wants
 * it, which is nearly always.  A compositor wants it: it draws windows, and
 * windows and a text console cannot share a screen.
 *
 * So the screen is lent out, one process at a time.  While it is lent the
 * console keeps working -- it tracks the cursor, it scrolls, it records
 * everything printed -- and simply stops putting any of it on the glass.  When
 * the loan ends the console repaints, and the output of whatever was running
 * behind the compositor is all still there.
 *
 * The loan ends when the borrower gives it back or when the borrower exits.
 * The second matters more than the first: a compositor that faults must not
 * take the screen with it, leaving a machine that is running perfectly and
 * cannot say so.
 */
#ifndef WOS_DISPLAY_H
#define WOS_DISPLAY_H

#include "types.h"
#include "wabi.h"

/* What the screen is, whether or not anyone has it.  A machine with no
 * framebuffer -- one still in VGA text mode -- reports `present` clear and
 * zero for the rest, which is a compositor's cue to say so and exit rather
 * than to draw into nothing. */
void display_info(wdisplay_t *out);

/* Take the screen.  Returns 0, -W_ENODEV when there is no framebuffer to take,
 * or -W_EBUSY when another process already has it.  Asking twice from the same
 * process succeeds and changes nothing. */
int display_acquire(int32_t pid);

/* Give it back.  A pid that does not hold it is ignored, which is what lets
 * this be called unconditionally as any process exits. */
void display_release(int32_t pid);

/* Put a rectangle of 32-bit pixels on the screen.  Only the holder may, and
 * the rectangle is clipped to the screen rather than trusted.
 * Returns 0 or -W_EPERM. */
int display_blit(int32_t pid, const uint32_t *src, uint32_t src_stride_px,
                 int x, int y, int w, int h);

#endif /* WOS_DISPLAY_H */
