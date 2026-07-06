/* Lending the screen to a process.  See display.h. */

#include "display.h"
#include "fbcon.h"
#include "string.h"

/* Who has the screen, or 0 for nobody.  One number is the whole of the
 * ownership model: there is one screen, and either a process has it or the
 * console does. */
static int32_t owner;

void display_info(wdisplay_t *out)
{
    int      w = 0, h = 0;
    uint32_t stride_px = 0;

    memset(out, 0, sizeof(*out));

    if (!fbcon_geometry(&w, &h, &stride_px))
        return;                    /* present stays 0: nothing to draw on */

    out->present = 1;
    out->width   = (uint32_t)w;
    out->height  = (uint32_t)h;
    out->stride  = stride_px * 4;  /* bytes, which is what a caller indexes by */
    out->bpp     = 32;
    out->owner   = (uint32_t)(owner > 0 ? owner : 0);
}

int display_acquire(int32_t pid)
{
    int      w, h;
    uint32_t stride_px;

    if (!fbcon_geometry(&w, &h, &stride_px))
        return -W_ENODEV;

    if (owner && owner != pid)
        return -W_EBUSY;

    owner = pid;
    fbcon_suspend();
    return 0;
}

void display_release(int32_t pid)
{
    if (owner != pid)
        return;

    owner = 0;
    fbcon_resume();
}

int display_blit(int32_t pid, const uint32_t *src, uint32_t src_stride_px,
                 int x, int y, int w, int h)
{
    if (owner != pid)
        return -W_EPERM;

    fbcon_blit(src, src_stride_px, x, y, w, h);
    return 0;
}
