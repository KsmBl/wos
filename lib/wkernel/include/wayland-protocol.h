/* wayland-protocol.h -- the interfaces, as the protocol defines them.
 *
 * Every interface here is transcribed from the upstream XML: the same request
 * and event order, the same signatures, the same enumeration values.  That
 * matters more than it looks.  An opcode is just a position in this list, so a
 * message is only understood by real Wayland software if the list is in the
 * real order -- getting it "nearly right" produces a client that asks for a
 * region when it meant a surface.
 *
 * Two protocols are described: the core one from wayland.xml, and xdg_shell,
 * which is where windows come from.  Nothing in core Wayland is a window; a
 * wl_surface is a rectangle of pixels with no position, no title and no way to
 * be closed, and xdg_shell is the extension that makes one into something a
 * user would recognise.  That is why a client needs both.
 *
 * Interfaces are described here whether or not this system implements them,
 * because the description is what the wire format is made of.  A compositor
 * that does not implement wl_touch still has to be able to say so correctly.
 */
#ifndef WAYLAND_PROTOCOL_H
#define WAYLAND_PROTOCOL_H

#include "wayland-util.h"

/* ------------------------------------------------------------------ *
 *  The interfaces
 * ------------------------------------------------------------------ */

extern const struct wl_interface wl_display_interface;
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_callback_interface;
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_shm_pool_interface;
extern const struct wl_interface wl_shm_interface;
extern const struct wl_interface wl_buffer_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_region_interface;
extern const struct wl_interface wl_seat_interface;
extern const struct wl_interface wl_pointer_interface;
extern const struct wl_interface wl_keyboard_interface;
extern const struct wl_interface wl_output_interface;

extern const struct wl_interface xdg_wm_base_interface;
extern const struct wl_interface xdg_positioner_interface;
extern const struct wl_interface xdg_surface_interface;
extern const struct wl_interface xdg_toplevel_interface;
extern const struct wl_interface xdg_popup_interface;

/* Find an interface by the name a client sends in wl_registry.bind, or NULL.
 * The compositor needs this: bind names its interface as a string, and the
 * only way to check a client asked for what it thinks it asked for is to look
 * the name up. */
const struct wl_interface *wl_interface_by_name(const char *name);

/* ------------------------------------------------------------------ *
 *  Opcodes
 *
 *  A request or event is identified on the wire by its position in the
 *  interface, so these are the protocol, not a convenience.
 * ------------------------------------------------------------------ */

/* wl_display */
#define WL_DISPLAY_SYNC             0
#define WL_DISPLAY_GET_REGISTRY     1
#define WL_DISPLAY_ERROR            0
#define WL_DISPLAY_DELETE_ID        1

/* wl_display.error codes */
#define WL_DISPLAY_ERROR_INVALID_OBJECT 0
#define WL_DISPLAY_ERROR_INVALID_METHOD 1
#define WL_DISPLAY_ERROR_NO_MEMORY      2
#define WL_DISPLAY_ERROR_IMPLEMENTATION 3

/* wl_registry */
#define WL_REGISTRY_BIND            0
#define WL_REGISTRY_GLOBAL          0
#define WL_REGISTRY_GLOBAL_REMOVE   1

/* wl_callback */
#define WL_CALLBACK_DONE            0

/* wl_compositor */
#define WL_COMPOSITOR_CREATE_SURFACE 0
#define WL_COMPOSITOR_CREATE_REGION  1

/* wl_shm */
#define WL_SHM_CREATE_POOL          0
#define WL_SHM_FORMAT               0

/* wl_shm.format: the two every compositor must support, which are also the
 * only two anything here uses.  The numbers are DRM fourcc codes, except for
 * these two, which predate that and are 0 and 1. */
#define WL_SHM_FORMAT_ARGB8888      0
#define WL_SHM_FORMAT_XRGB8888      1

/* wl_shm_pool */
#define WL_SHM_POOL_CREATE_BUFFER   0
#define WL_SHM_POOL_DESTROY         1
#define WL_SHM_POOL_RESIZE          2

/* wl_buffer */
#define WL_BUFFER_DESTROY           0
#define WL_BUFFER_RELEASE           0

/* wl_surface */
#define WL_SURFACE_DESTROY              0
#define WL_SURFACE_ATTACH               1
#define WL_SURFACE_DAMAGE               2
#define WL_SURFACE_FRAME                3
#define WL_SURFACE_SET_OPAQUE_REGION    4
#define WL_SURFACE_SET_INPUT_REGION     5
#define WL_SURFACE_COMMIT               6
#define WL_SURFACE_SET_BUFFER_TRANSFORM 7
#define WL_SURFACE_SET_BUFFER_SCALE     8
#define WL_SURFACE_DAMAGE_BUFFER        9
#define WL_SURFACE_ENTER                0
#define WL_SURFACE_LEAVE                1

/* wl_region */
#define WL_REGION_DESTROY           0
#define WL_REGION_ADD               1
#define WL_REGION_SUBTRACT          2

/* wl_seat */
#define WL_SEAT_GET_POINTER         0
#define WL_SEAT_GET_KEYBOARD        1
#define WL_SEAT_GET_TOUCH           2
#define WL_SEAT_RELEASE             3
#define WL_SEAT_CAPABILITIES        0
#define WL_SEAT_NAME                1

#define WL_SEAT_CAPABILITY_POINTER  1
#define WL_SEAT_CAPABILITY_KEYBOARD 2
#define WL_SEAT_CAPABILITY_TOUCH    4

/* wl_keyboard */
#define WL_KEYBOARD_RELEASE         0
#define WL_KEYBOARD_KEYMAP          0
#define WL_KEYBOARD_ENTER           1
#define WL_KEYBOARD_LEAVE           2
#define WL_KEYBOARD_KEY             3
#define WL_KEYBOARD_MODIFIERS       4
#define WL_KEYBOARD_REPEAT_INFO     5

#define WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP 0
#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1    1

#define WL_KEYBOARD_KEY_STATE_RELEASED 0
#define WL_KEYBOARD_KEY_STATE_PRESSED  1

/* wl_pointer */
#define WL_POINTER_SET_CURSOR       0
#define WL_POINTER_RELEASE          1
#define WL_POINTER_ENTER            0
#define WL_POINTER_LEAVE            1
#define WL_POINTER_MOTION           2
#define WL_POINTER_BUTTON           3
#define WL_POINTER_AXIS             4

#define WL_POINTER_BUTTON_STATE_RELEASED 0
#define WL_POINTER_BUTTON_STATE_PRESSED  1

#define WL_POINTER_AXIS_VERTICAL_SCROLL   0
#define WL_POINTER_AXIS_HORIZONTAL_SCROLL 1

/* wl_output */
#define WL_OUTPUT_RELEASE           0
#define WL_OUTPUT_GEOMETRY          0
#define WL_OUTPUT_MODE              1
#define WL_OUTPUT_DONE              2
#define WL_OUTPUT_SCALE             3
#define WL_OUTPUT_NAME              4
#define WL_OUTPUT_DESCRIPTION       5

#define WL_OUTPUT_MODE_CURRENT      1
#define WL_OUTPUT_MODE_PREFERRED    2

#define WL_OUTPUT_SUBPIXEL_UNKNOWN  0
#define WL_OUTPUT_TRANSFORM_NORMAL  0

/* xdg_wm_base */
#define XDG_WM_BASE_DESTROY            0
#define XDG_WM_BASE_CREATE_POSITIONER  1
#define XDG_WM_BASE_GET_XDG_SURFACE    2
#define XDG_WM_BASE_PONG               3
#define XDG_WM_BASE_PING               0

#define XDG_WM_BASE_ERROR_ROLE                    0
#define XDG_WM_BASE_ERROR_DEFUNCT_SURFACES        1
#define XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP   2
#define XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT    3
#define XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE   4
#define XDG_WM_BASE_ERROR_INVALID_POSITIONER      5

/* xdg_surface */
#define XDG_SURFACE_DESTROY             0
#define XDG_SURFACE_GET_TOPLEVEL        1
#define XDG_SURFACE_GET_POPUP           2
#define XDG_SURFACE_SET_WINDOW_GEOMETRY 3
#define XDG_SURFACE_ACK_CONFIGURE       4
#define XDG_SURFACE_CONFIGURE           0

/* xdg_toplevel */
#define XDG_TOPLEVEL_DESTROY            0
#define XDG_TOPLEVEL_SET_PARENT         1
#define XDG_TOPLEVEL_SET_TITLE          2
#define XDG_TOPLEVEL_SET_APP_ID         3
#define XDG_TOPLEVEL_SHOW_WINDOW_MENU   4
#define XDG_TOPLEVEL_MOVE               5
#define XDG_TOPLEVEL_RESIZE             6
#define XDG_TOPLEVEL_SET_MAX_SIZE       7
#define XDG_TOPLEVEL_SET_MIN_SIZE       8
#define XDG_TOPLEVEL_SET_MAXIMIZED      9
#define XDG_TOPLEVEL_UNSET_MAXIMIZED   10
#define XDG_TOPLEVEL_SET_FULLSCREEN    11
#define XDG_TOPLEVEL_UNSET_FULLSCREEN  12
#define XDG_TOPLEVEL_SET_MINIMIZED     13
#define XDG_TOPLEVEL_CONFIGURE          0
#define XDG_TOPLEVEL_CLOSE              1
#define XDG_TOPLEVEL_CONFIGURE_BOUNDS   2
#define XDG_TOPLEVEL_WM_CAPABILITIES    3

/* The states a compositor puts in xdg_toplevel.configure. */
#define XDG_TOPLEVEL_STATE_MAXIMIZED   1
#define XDG_TOPLEVEL_STATE_FULLSCREEN  2
#define XDG_TOPLEVEL_STATE_RESIZING    3
#define XDG_TOPLEVEL_STATE_ACTIVATED   4
#define XDG_TOPLEVEL_STATE_TILED_LEFT  5
#define XDG_TOPLEVEL_STATE_TILED_RIGHT 6
#define XDG_TOPLEVEL_STATE_TILED_TOP   7
#define XDG_TOPLEVEL_STATE_TILED_BOTTOM 8

#endif /* WAYLAND_PROTOCOL_H */
