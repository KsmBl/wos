/* The PS/2 mouse.
 *
 * Its events go into the same stream the keyboard's do -- one input device as
 * far as anything reading it is concerned -- so there is no read function
 * here.  winputopen() is how a compositor gets both.
 */
#ifndef WOS_MOUSE_H
#define WOS_MOUSE_H

#include "types.h"

/* Find it, enable it, and take its interrupt.  Safe on a machine with no PS/2
 * controller and on one whose second port is empty; mouse_present() then stays
 * false and nothing else here does anything. */
void mouse_init(void);

bool mouse_present(void);

/* Where the pointer is now, in pixels.  The compositor draws the cursor from
 * the motion events; this is for anything that needs the position without
 * having watched every event -- a client asking where it was clicked. */
void mouse_position(int32_t *x, int32_t *y);

/* Tell the driver how big the screen is, so it can stop the pointer leaving
 * it.  Called when the console changes resolution: the pointer is clamped by
 * the kernel because a pointer that can go off the edge is one nobody can
 * bring back. */
void mouse_screen_size(int width, int height);

#endif /* WOS_MOUSE_H */
