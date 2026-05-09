/* textmode -- show or change the console's character grid.
 *
 *   textmode                 print the current size and some presets
 *   textmode <cols> <rows>   switch to that grid, e.g. `textmode 160 50`
 *
 * The console is a linear framebuffer rendering an 8x16 font, so the grid can
 * be almost any size: cols*8 by rows*16 pixels, from 40x25 up to 240x75.
 */

#include <wkernel.h>

/* A few useful presets to suggest; any size in range works. */
static const struct { int cols, rows; } modes[] = {
    { 80, 25 }, { 100, 37 }, { 120, 40 }, { 128, 48 },
    { 160, 50 }, { 200, 60 },
};

static void list_modes(void)
{
    int cols = 0, rows = 0;
    wconsize(&rows, &cols);
    wprintf("Current text mode: %dx%d\n\n", cols, rows);

    wprintf("Some sizes (use: textmode <cols> <rows>; 40x25 up to 240x75):\n");
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        wprintf("  %d %d  (%dx%d px)%s\n", modes[i].cols, modes[i].rows,
                modes[i].cols * 8, modes[i].rows * 16,
                (modes[i].cols == cols && modes[i].rows == rows) ? "   (current)" : "");
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        list_modes();
        return 0;
    }

    if (argc != 3) {
        wfprintf(W_STDERR, "usage: textmode [<cols> <rows>]\n");
        return 2;
    }

    int cols = atoi(argv[1]);
    int rows = atoi(argv[2]);

    int r = wsetmode(cols, rows);
    if (r == -W_EINVAL) {
        wfprintf(W_STDERR, "textmode: %dx%d is not a supported mode\n", cols, rows);
        wfprintf(W_STDERR, "try one of:");
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
            wfprintf(W_STDERR, " %dx%d", modes[i].cols, modes[i].rows);
        wfprintf(W_STDERR, "\n");
        return 1;
    }
    if (r < 0) {
        wfprintf(W_STDERR, "textmode: %s\n", wstrerror(-r));
        return 1;
    }

    /* The mode switch cleared the screen; say what it is now so the prompt
     * that follows is not alone on a blank screen. */
    wprintf("Text mode is now %dx%d.\n", cols, rows);
    return 0;
}
