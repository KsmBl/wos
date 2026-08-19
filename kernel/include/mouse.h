/* The PS/2 mouse.
 *
 * Its events go into the same stream the keyboard's do -- one input device as
 * far as anything reading it is concerned -- so there is no read function
 * here.  winputopen() is how a compositor gets both.
 */
#ifndef WOS_MOUSE_H
#define WOS_MOUSE_H

#include "types.h"
#include "wosconfig.h"

/* Find it, enable it, and take its interrupt.  Safe on a machine with no PS/2
 * controller and on one whose second port is empty; mouse_present() then stays
 * false and nothing else here does anything. */
#if CONFIG_MOUSE

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

/* How far the pointer goes for how far the mouse moves, as a percentage --
 * W_POINTER_SPEED_* in wabi.h.  Here rather than in the compositor for the
 * same reason the position is: the counts are turned into pixels in one place,
 * and a setting applied anywhere else would leave the kernel's pointer and the
 * drawn cursor disagreeing about where the mouse is.
 *
 * A value outside the range is clamped rather than refused, and the value that
 * ends up in force is returned, so a caller can say what it got. */
int mouse_set_speed(int percent);
int mouse_speed(void);


#else
/* Without the driver: no pointer, and everything that asks says so rather
 * than needing to know whether it is there. */
static inline void mouse_init(void) { }
static inline bool mouse_present(void) { return false; }
static inline void mouse_position(int32_t *x, int32_t *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
}
static inline void mouse_screen_size(int width, int height) { (void)width; (void)height; }
static inline int  mouse_set_speed(int percent) { (void)percent; return -1; }
static inline int  mouse_speed(void) { return 0; }
#endif /* CONFIG_MOUSE */

#endif /* WOS_MOUSE_H */
