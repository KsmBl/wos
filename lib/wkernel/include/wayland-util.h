/* wayland-util.h -- the types both sides of the Wayland protocol are built on.
 *
 * These are libwayland's own names and shapes, not lookalikes.  A protocol is
 * only worth calling compatible if a program written against the real thing
 * compiles against this one, so wl_interface, wl_message, wl_argument, wl_list
 * and wl_fixed_t are what they are upstream, down to the field order.
 *
 * The one deliberate absence is the floating-point half of wl_fixed_t.
 * wl_fixed_to_double() and wl_fixed_from_double() are not here, because the
 * WOS kernel never enables the FPU and a program that used them would fault on
 * its first surface coordinate.  The integer conversions do everything the
 * protocol actually needs -- wl_fixed_t is a 24.8 integer, and the double form
 * was always a convenience rather than the format.
 */
#ifndef WAYLAND_UTIL_H
#define WAYLAND_UTIL_H

#include <wkernel.h>

/* ------------------------------------------------------------------ *
 *  Fixed point
 *
 *  The protocol's coordinate type: signed 24.8 fixed point in an int32.
 * ------------------------------------------------------------------ */

typedef int32_t wl_fixed_t;

static inline int wl_fixed_to_int(wl_fixed_t f)   { return f / 256; }
static inline wl_fixed_t wl_fixed_from_int(int i) { return (wl_fixed_t)(i * 256); }

/* ------------------------------------------------------------------ *
 *  Interfaces
 *
 *  What an object of a given type can be asked and can say.  A message's
 *  signature is the string libwayland uses:
 *
 *      i  int32     u  uint32    f  fixed     s  string
 *      o  object    n  new id    a  array     h  file descriptor
 *      ?  the next argument may be null
 *      0-9  the version this message appeared in, at the front
 *
 *  The whole wire format falls out of these strings: everything that packs and
 *  unpacks a message reads one and does what it says, which is why adding an
 *  interface here is a matter of describing it rather than writing code.
 * ------------------------------------------------------------------ */

struct wl_interface;

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;   /* one per o/n argument, or NULL */
};

struct wl_interface {
    const char *name;
    int         version;
    int         method_count;            /* requests: client to server */
    const struct wl_message *methods;
    int         event_count;             /* events: server to client   */
    const struct wl_message *events;
};

/* One argument of a message, on its way to or from the wire. */
union wl_argument {
    int32_t     i;
    uint32_t    u;
    wl_fixed_t  f;
    const char *s;
    void       *o;      /* wl_proxy on the client, wl_resource on the server */
    uint32_t    n;      /* a new object's id */
    struct wl_array *a;
    int32_t     h;      /* a file descriptor */
};

/* Most arguments one message may carry.  The largest in the protocols here is
 * wl_shm_pool.create_buffer, at six. */
#define WL_MAX_ARGS 12

/* ------------------------------------------------------------------ *
 *  wl_list: the intrusive doubly linked list libwayland uses everywhere
 * ------------------------------------------------------------------ */

struct wl_list {
    struct wl_list *prev;
    struct wl_list *next;
};

static inline void wl_list_init(struct wl_list *list)
{
    list->prev = list;
    list->next = list;
}

static inline void wl_list_insert(struct wl_list *list, struct wl_list *elm)
{
    elm->prev        = list;
    elm->next        = list->next;
    list->next       = elm;
    elm->next->prev  = elm;
}

static inline void wl_list_remove(struct wl_list *elm)
{
    elm->prev->next = elm->next;
    elm->next->prev = elm->prev;
    elm->next = NULL;
    elm->prev = NULL;
}

static inline int wl_list_empty(const struct wl_list *list)
{
    return list->next == list;
}

static inline int wl_list_length(const struct wl_list *list)
{
    int n = 0;
    for (const struct wl_list *e = list->next; e != list; e = e->next)
        n++;
    return n;
}

/* The address of the structure a member belongs to. */
#define wl_container_of(ptr, sample, member) \
    ((__typeof__(sample))((char *)(ptr) - ((char *)&(sample)->member - (char *)(sample))))

#define wl_list_for_each(pos, head, member)                              \
    for (pos = wl_container_of((head)->next, pos, member);               \
         &pos->member != (head);                                         \
         pos = wl_container_of(pos->member.next, pos, member))

#define wl_list_for_each_safe(pos, tmp, head, member)                    \
    for (pos = wl_container_of((head)->next, pos, member),               \
         tmp = wl_container_of(pos->member.next, tmp, member);           \
         &pos->member != (head);                                         \
         pos = tmp,                                                      \
         tmp = wl_container_of(pos->member.next, tmp, member))

/* ------------------------------------------------------------------ *
 *  wl_array: a growable block of bytes
 *
 *  What the protocol's 'a' arguments are, and what wl_keyboard.enter carries
 *  its list of held keys in.
 * ------------------------------------------------------------------ */

struct wl_array {
    wsize_t size;
    wsize_t alloc;
    void   *data;
};

void  wl_array_init(struct wl_array *array);
void  wl_array_release(struct wl_array *array);
void *wl_array_add(struct wl_array *array, wsize_t size);

#endif /* WAYLAND_UTIL_H */
