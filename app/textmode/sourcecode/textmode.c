/* textmode -- show or change the console's character grid.
 *
 *   textmode                 print the current size and the choices
 *   textmode <cols> <rows>   switch to that mode, e.g. `textmode 80 25`
 *
 * VGA text modes are not arbitrary: the columns come from the dot clock (40 or
 * 80) and the rows from the character-cell height and the number of scan
 * lines, so only a handful of sizes exist.  wsetmode() refuses the rest.
 */

#include <wkernel.h>

static const struct { int cols, rows; } modes[] = {
    { 80, 25 }, { 80, 30 }, { 80, 50 }, { 80, 60 },
    { 40, 25 }, { 40, 50 },
};

static void list_modes(void)
{
    int cols = 0, rows = 0;
    wconsize(&rows, &cols);
    wprintf("Current text mode: %dx%d\n\n", cols, rows);

    wprintf("Available modes (use: textmode <cols> <rows>):\n");
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        wprintf("  %d %d%s\n", modes[i].cols, modes[i].rows,
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
