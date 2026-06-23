/* Shared memory: pages two processes can both see.
 *
 * A pipe or a socket moves bytes by copying them.  That is the right shape for
 * a stream of messages and the wrong shape for a screenful of pixels: a
 * 640x400 window is a megabyte, and a client redrawing it sixty times a second
 * would push sixty megabytes a second through a 4 KiB socket buffer, of which
 * the compositor would then make a second copy.
 *
 * So the pixels do not move at all.  A client asks for a shared memory object,
 * maps it, draws into it, and passes the *descriptor* to the compositor over
 * the socket -- which is exactly what wl_shm does, and why the socket had to be
 * able to carry descriptors before any of this could work.  The compositor maps
 * the same object and reads the same physical frames.  Nothing is copied until
 * the compositor composites, and that copy is the one that was always going to
 * happen.
 *
 * An object is a fixed set of frames, allocated up front and never grown: a
 * pool is created at a size and the buffers inside it are carved out by the
 * protocol, not by the kernel.  Frames are ordinary RAM, so the mapping is
 * cached and writing to it is as fast as writing to anything else.
 *
 * The object lives as long as any descriptor names it or any process has it
 * mapped, whichever is longer.  A client that creates a pool, sends it and
 * exits leaves the compositor holding perfectly good pixels.
 */
#ifndef WOS_SHM_H
#define WOS_SHM_H

#include "types.h"
#include "wabi.h"

struct process;

/* Largest object that may be created.  A 1920x1200 window at four bytes a
 * pixel is 9 MiB, and double buffering it is 18 -- so this allows one screen's
 * worth twice over, and refuses the arithmetic mistake that asks for a
 * gigabyte. */
#define SHM_MAX_BYTES (32u * 1024u * 1024u)

/* Mappings one process may hold at once.  A compositor holds one per client
 * buffer pool, so this bounds how many clients can have windows up. */
#define SHM_MAX_MAPPINGS 32

typedef struct shm shm_t;

/* One region a process has mapped.  Kept per process rather than per object
 * because unmapping is per address space, and because the frames must be
 * detached before the address space is torn down -- the generic teardown frees
 * every frame it finds, and these belong to somebody else as well. */
typedef struct {
    shm_t   *shm;
    uint64_t base;         /* user virtual address, page aligned */
    uint32_t pages;
} shm_mapping_t;

/* Create an object of `bytes`, rounded up to whole pages and zeroed.  Returns
 * NULL if the size is zero or above SHM_MAX_BYTES, or if there is not enough
 * memory. */
shm_t *shm_create(uint32_t bytes);

/* How big it is, in bytes: what was asked for, rounded up to a page. */
uint32_t shm_bytes(const shm_t *s);

/* One more, one fewer descriptor or mapping naming this object.  The last drop
 * frees the frames. */
void shm_ref(shm_t *s);
void shm_unref(shm_t *s);

/* Map the whole object into `p` and return the address through `addr_out`.
 * Mapping it twice gives two independent mappings of the same frames, which is
 * harmless and occasionally what a program means.
 *
 * Returns 0, -W_ENOMEM when there is no room, or -W_EMFILE when the process
 * already holds SHM_MAX_MAPPINGS. */
int shm_map(struct process *p, shm_t *s, uint64_t *addr_out);

/* Remove the mapping that starts at `addr`.  Returns 0 or -W_EINVAL if the
 * process has no mapping there.  The frames survive if anything else still
 * names the object. */
int shm_unmap(struct process *p, uint64_t addr);

/* Drop every mapping a process holds.  Called as the process exits, before its
 * address space is freed: after this there are no shared frames left in it for
 * the teardown to mistake for its own. */
void shm_unmap_all(struct process *p);

#endif /* WOS_SHM_H */
