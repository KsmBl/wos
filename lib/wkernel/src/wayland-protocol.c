/* The interface tables.
 *
 * This file is data.  Upstream it is generated from wayland.xml and
 * xdg-shell.xml by wayland-scanner; here it is written out, because WOS has no
 * XML parser and no build step to run one in.  What matters is that it says the
 * same thing: the same messages in the same order with the same signatures,
 * because an opcode is a position in these arrays and a signature is the wire
 * format of the message.
 *
 * The `types` array is the shared pool the real generated code uses too.  Each
 * message points into it at the run of entries matching its object and new-id
 * arguments; a NULL entry stands for an argument that is not one of those.
 */

#include "wayland-protocol.h"

static const struct wl_interface *types[] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,   /*  0- 7: filler  */
    &wl_callback_interface,        /*  8 */
    &wl_registry_interface,        /*  9 */
    &wl_surface_interface,         /* 10 */
    &wl_region_interface,          /* 11 */
    &wl_buffer_interface,          /* 12 */
    &wl_shm_pool_interface,        /* 13 */
    &wl_pointer_interface,         /* 14 */
    &wl_keyboard_interface,        /* 15 */
    &wl_output_interface,          /* 16 */
    &xdg_positioner_interface,     /* 17 */
    &xdg_surface_interface,        /* 18 */
    &xdg_toplevel_interface,       /* 19 */
    &xdg_popup_interface,          /* 20 */
};

/* ------------------------------------------------------------------ *
 *  wl_display -- object 1, and the only one that exists unasked for
 * ------------------------------------------------------------------ */

static const struct wl_message wl_display_requests[] = {
    { "sync",         "n", types + 8 },
    { "get_registry", "n", types + 9 },
};

static const struct wl_message wl_display_events[] = {
    { "error",     "ous", types + 0 },
    { "delete_id", "u",   types + 0 },
};

const struct wl_interface wl_display_interface = {
    "wl_display", 1,
    2, wl_display_requests,
    2, wl_display_events,
};

/* ------------------------------------------------------------------ *
 *  wl_registry
 *
 *  bind's signature is "usun" rather than the "un" the XML reads, because a
 *  new_id whose interface is not fixed by the protocol carries the interface
 *  name and version on the wire ahead of the id.  It is the one message whose
 *  signature is not a transcription of its argument list, and the wire format
 *  is what a signature has to describe.
 * ------------------------------------------------------------------ */

static const struct wl_message wl_registry_requests[] = {
    { "bind", "usun", types + 0 },
};

static const struct wl_message wl_registry_events[] = {
    { "global",        "usu", types + 0 },
    { "global_remove", "u",   types + 0 },
};

const struct wl_interface wl_registry_interface = {
    "wl_registry", 1,
    1, wl_registry_requests,
    2, wl_registry_events,
};

/* ------------------------------------------------------------------ *
 *  wl_callback -- one event, then it is gone
 * ------------------------------------------------------------------ */

static const struct wl_message wl_callback_events[] = {
    { "done", "u", types + 0 },
};

const struct wl_interface wl_callback_interface = {
    "wl_callback", 1,
    0, NULL,
    1, wl_callback_events,
};

/* ------------------------------------------------------------------ *
 *  wl_compositor
 * ------------------------------------------------------------------ */

static const struct wl_message wl_compositor_requests[] = {
    { "create_surface", "n", types + 10 },
    { "create_region",  "n", types + 11 },
};

const struct wl_interface wl_compositor_interface = {
    "wl_compositor", 6,
    2, wl_compositor_requests,
    0, NULL,
};

/* ------------------------------------------------------------------ *
 *  wl_shm and wl_shm_pool -- where a window's pixels live
 * ------------------------------------------------------------------ */

static const struct wl_message wl_shm_pool_requests[] = {
    { "create_buffer", "niiiiu", types + 12 },
    { "destroy",       "",       types + 0  },
    { "resize",        "i",      types + 0  },
};

const struct wl_interface wl_shm_pool_interface = {
    "wl_shm_pool", 1,
    3, wl_shm_pool_requests,
    0, NULL,
};

static const struct wl_message wl_shm_requests[] = {
    { "create_pool", "nhi", types + 13 },
};

static const struct wl_message wl_shm_events[] = {
    { "format", "u", types + 0 },
};

const struct wl_interface wl_shm_interface = {
    "wl_shm", 1,
    1, wl_shm_requests,
    1, wl_shm_events,
};

/* ------------------------------------------------------------------ *
 *  wl_buffer
 *
 *  release is the event that makes double buffering possible: it says the
 *  compositor has finished reading those pixels and the client may draw over
 *  them again.  A client that ignores it and redraws anyway will tear.
 * ------------------------------------------------------------------ */

static const struct wl_message wl_buffer_requests[] = {
    { "destroy", "", types + 0 },
};

static const struct wl_message wl_buffer_events[] = {
    { "release", "", types + 0 },
};

const struct wl_interface wl_buffer_interface = {
    "wl_buffer", 1,
    1, wl_buffer_requests,
    1, wl_buffer_events,
};

/* ------------------------------------------------------------------ *
 *  wl_surface -- a rectangle of pixels, with no position and no title
 * ------------------------------------------------------------------ */

static const struct wl_message wl_surface_requests[] = {
    { "destroy",               "",      types + 0  },
    { "attach",                "?oii",  types + 12 },
    { "damage",                "iiii",  types + 0  },
    { "frame",                 "n",     types + 8  },
    { "set_opaque_region",     "?o",    types + 11 },
    { "set_input_region",      "?o",    types + 11 },
    { "commit",                "",      types + 0  },
    { "set_buffer_transform",  "2i",    types + 0  },
    { "set_buffer_scale",      "3i",    types + 0  },
    { "damage_buffer",         "4iiii", types + 0  },
};

static const struct wl_message wl_surface_events[] = {
    { "enter", "o", types + 16 },
    { "leave", "o", types + 16 },
};

const struct wl_interface wl_surface_interface = {
    "wl_surface", 6,
    10, wl_surface_requests,
    2,  wl_surface_events,
};

/* ------------------------------------------------------------------ *
 *  wl_region
 * ------------------------------------------------------------------ */

static const struct wl_message wl_region_requests[] = {
    { "destroy",  "",     types + 0 },
    { "add",      "iiii", types + 0 },
    { "subtract", "iiii", types + 0 },
};

const struct wl_interface wl_region_interface = {
    "wl_region", 1,
    3, wl_region_requests,
    0, NULL,
};

/* ------------------------------------------------------------------ *
 *  wl_seat, wl_keyboard, wl_pointer
 * ------------------------------------------------------------------ */

static const struct wl_message wl_seat_requests[] = {
    { "get_pointer",  "n",  types + 14 },
    { "get_keyboard", "n",  types + 15 },
    { "get_touch",    "n",  types + 0  },
    { "release",      "5",  types + 0  },
};

static const struct wl_message wl_seat_events[] = {
    { "capabilities", "u",  types + 0 },
    { "name",         "2s", types + 0 },
};

const struct wl_interface wl_seat_interface = {
    "wl_seat", 9,
    4, wl_seat_requests,
    2, wl_seat_events,
};

static const struct wl_message wl_keyboard_requests[] = {
    { "release", "3", types + 0 },
};

static const struct wl_message wl_keyboard_events[] = {
    { "keymap",      "uhu",   types + 0  },
    { "enter",       "uoa",   types + 10 },
    { "leave",       "uo",    types + 10 },
    { "key",         "uuuu",  types + 0  },
    { "modifiers",   "uuuuu", types + 0  },
    { "repeat_info", "4ii",   types + 0  },
};

const struct wl_interface wl_keyboard_interface = {
    "wl_keyboard", 9,
    1, wl_keyboard_requests,
    6, wl_keyboard_events,
};

static const struct wl_message wl_pointer_requests[] = {
    { "set_cursor", "u?oii", types + 10 },
    { "release",    "3",     types + 0  },
};

static const struct wl_message wl_pointer_events[] = {
    { "enter",  "uoff", types + 10 },
    { "leave",  "uo",   types + 10 },
    { "motion", "uff",  types + 0  },
    { "button", "uuuu", types + 0  },
    { "axis",   "uuf",  types + 0  },
};

const struct wl_interface wl_pointer_interface = {
    "wl_pointer", 9,
    2, wl_pointer_requests,
    5, wl_pointer_events,
};

/* ------------------------------------------------------------------ *
 *  wl_output
 * ------------------------------------------------------------------ */

static const struct wl_message wl_output_requests[] = {
    { "release", "3", types + 0 },
};

static const struct wl_message wl_output_events[] = {
    { "geometry",    "iiiiissi", types + 0 },
    { "mode",        "uiii",     types + 0 },
    { "done",        "2",        types + 0 },
    { "scale",       "2i",       types + 0 },
    { "name",        "4s",       types + 0 },
    { "description", "4s",       types + 0 },
};

const struct wl_interface wl_output_interface = {
    "wl_output", 4,
    1, wl_output_requests,
    6, wl_output_events,
};

/* ------------------------------------------------------------------ *
 *  xdg_shell -- where windows come from
 * ------------------------------------------------------------------ */

static const struct wl_message xdg_wm_base_requests[] = {
    { "destroy",           "",   types + 0  },
    { "create_positioner", "n",  types + 17 },
    { "get_xdg_surface",   "no", types + 18 },   /* new xdg_surface, wl_surface */
    { "pong",              "u",  types + 0  },
};

static const struct wl_message xdg_wm_base_events[] = {
    { "ping", "u", types + 0 },
};

const struct wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 6,
    4, xdg_wm_base_requests,
    1, xdg_wm_base_events,
};

/* Positioners place popups.  Nothing here makes a popup, but the interface has
 * to exist for a client that asks whether it could. */
static const struct wl_message xdg_positioner_requests[] = {
    { "destroy",          "",     types + 0 },
    { "set_size",         "ii",   types + 0 },
    { "set_anchor_rect",  "iiii", types + 0 },
    { "set_anchor",       "u",    types + 0 },
    { "set_gravity",      "u",    types + 0 },
    { "set_constraint_adjustment", "u", types + 0 },
    { "set_offset",       "ii",   types + 0 },
};

const struct wl_interface xdg_positioner_interface = {
    "xdg_positioner", 6,
    7, xdg_positioner_requests,
    0, NULL,
};

static const struct wl_message xdg_surface_requests[] = {
    { "destroy",             "",     types + 0  },
    { "get_toplevel",        "n",    types + 19 },
    { "get_popup",           "n?oo", types + 20 },
    { "set_window_geometry", "iiii", types + 0  },
    { "ack_configure",       "u",    types + 0  },
};

static const struct wl_message xdg_surface_events[] = {
    { "configure", "u", types + 0 },
};

const struct wl_interface xdg_surface_interface = {
    "xdg_surface", 6,
    5, xdg_surface_requests,
    1, xdg_surface_events,
};

static const struct wl_message xdg_toplevel_requests[] = {
    { "destroy",           "",     types + 0  },
    { "set_parent",        "?o",   types + 19 },
    { "set_title",         "s",    types + 0  },
    { "set_app_id",        "s",    types + 0  },
    { "show_window_menu",  "ouii", types + 0  },
    { "move",              "ou",   types + 0  },
    { "resize",            "ouu",  types + 0  },
    { "set_max_size",      "ii",   types + 0  },
    { "set_min_size",      "ii",   types + 0  },
    { "set_maximized",     "",     types + 0  },
    { "unset_maximized",   "",     types + 0  },
    { "set_fullscreen",    "?o",   types + 16 },
    { "unset_fullscreen",  "",     types + 0  },
    { "set_minimized",     "",     types + 0  },
};

static const struct wl_message xdg_toplevel_events[] = {
    { "configure",        "iia", types + 0 },
    { "close",            "",    types + 0 },
    { "configure_bounds", "4ii", types + 0 },
    { "wm_capabilities",  "5a",  types + 0 },
};

const struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 6,
    14, xdg_toplevel_requests,
    4,  xdg_toplevel_events,
};

static const struct wl_message xdg_popup_requests[] = {
    { "destroy", "",   types + 0  },
    { "grab",    "ou", types + 0  },
};

static const struct wl_message xdg_popup_events[] = {
    { "configure",   "iiii", types + 0 },
    { "popup_done",  "",     types + 0 },
};

const struct wl_interface xdg_popup_interface = {
    "xdg_popup", 6,
    2, xdg_popup_requests,
    2, xdg_popup_events,
};

/* ------------------------------------------------------------------ *
 *  Lookup by name
 * ------------------------------------------------------------------ */

static const struct wl_interface *const all[] = {
    &wl_display_interface,   &wl_registry_interface,  &wl_callback_interface,
    &wl_compositor_interface,&wl_shm_interface,       &wl_shm_pool_interface,
    &wl_buffer_interface,    &wl_surface_interface,   &wl_region_interface,
    &wl_seat_interface,      &wl_pointer_interface,   &wl_keyboard_interface,
    &wl_output_interface,    &xdg_wm_base_interface,  &xdg_positioner_interface,
    &xdg_surface_interface,  &xdg_toplevel_interface, &xdg_popup_interface,
};

const struct wl_interface *wl_interface_by_name(const char *name)
{
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (strcmp(all[i]->name, name) == 0)
            return all[i];
    return NULL;
}
