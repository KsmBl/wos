/* asciiquarium -- an animated aquarium for the terminal.
 *
 * A WOS-native take on Kirk Baucom's asciiquarium: fish drift across the
 * water, bubbles rise, seaweed sways.  The point of the port is that it never
 * stops redrawing, so running it beside the editor in vim's :term makes the
 * preemptive scheduler visible -- both windows animate at once.
 *
 * It sizes itself to the terminal through wconsize(), so it fills the whole
 * console at 80x25 and a vim window just as happily.  Rendering is
 * double-buffered: each frame is composed into a grid and only the cells that
 * changed are sent, which keeps the byte stream small and the picture steady.
 *
 * Press q (or Escape) to leave.
 */

#include <wkernel.h>

#define MAXR W_CONSOLE_MAX_HEIGHT
#define MAXC W_CONSOLE_MAX_WIDTH
#define MAX_FISH   16
#define MAX_BUBBLE 20

static int ROWS = 25;
static int COLS = 80;

/* Two grids: what is on screen now, and what the next frame wants.  Only the
 * differences between them are actually written. */
static char cur_ch[MAXR][MAXC],  next_ch[MAXR][MAXC];
static signed char cur_co[MAXR][MAXC], next_co[MAXR][MAXC];

/* A tiny deterministic PRNG: no entropy source exists, and it does not need
 * one -- it only has to look irregular. */
static unsigned rng = 0x1234567u;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7FFFu;
}

struct fish {
    int   x, y;         /* head column (may be off-screen), row     */
    int   dir;          /* +1 swims right, -1 swims left            */
    int   speed;        /* frames between steps                     */
    int   phase;        /* counts frames toward the next step       */
    int   color;
    const char *right;  /* sprite when swimming right               */
    const char *left;   /* sprite when swimming left                */
};

struct bubble { int x, y, active; };

static struct fish   fish[MAX_FISH];
static struct bubble bubble[MAX_BUBBLE];
static int fish_count;

static const char *const fish_right[] = { "><>",  "><(('>", ">-=>",  "><)))'>" };
static const char *const fish_left[]  = { "<><",  "<'))><", "<=-<",  "<'(((><" };
static const int          fish_colors[] = { W_YELLOW, W_CYAN, W_GREEN | W_BRIGHT,
                                            W_MAGENTA | W_BRIGHT };

static int surface_row(void) { return 0; }
static int floor_row(void)   { return ROWS - 1; }

/* Water is everything between the surface and the sandy floor. */
static int water_top(void) { return surface_row() + 1; }
static int water_bot(void) { return floor_row() - 1; }

static void spawn_fish(int i)
{
    int kind = rnd() % (int)(sizeof(fish_right) / sizeof(fish_right[0]));
    int dir  = (rnd() & 1) ? 1 : -1;

    fish[i].right = fish_right[kind];
    fish[i].left  = fish_left[kind];
    fish[i].color = fish_colors[kind];
    fish[i].dir   = dir;
    fish[i].speed = 1 + (int)(rnd() % 3);
    fish[i].phase = 0;

    int span = water_bot() - water_top();
    fish[i].y = water_top() + (span > 0 ? (int)(rnd() % (unsigned)(span + 1)) : 0);

    /* Enter from whichever edge it is heading away from. */
    int len = (int)strlen(fish[i].right);
    fish[i].x = (dir > 0) ? -len - (int)(rnd() % 12)
                          : COLS + (int)(rnd() % 12);
}

static void init_scene(void)
{
    fish_count = (ROWS * COLS) / 120;
    if (fish_count < 2)
        fish_count = 2;
    if (fish_count > MAX_FISH)
        fish_count = MAX_FISH;

    for (int i = 0; i < fish_count; i++)
        spawn_fish(i);

    for (int i = 0; i < MAX_BUBBLE; i++)
        bubble[i].active = 0;
}

static void put(int y, int x, char c, int color)
{
    if (y < 0 || y >= ROWS || x < 0 || x >= COLS)
        return;
    next_ch[y][x] = c;
    next_co[y][x] = (signed char)color;
}

static void put_str(int y, int x, const char *s, int color)
{
    for (int i = 0; s[i]; i++)
        put(y, x + i, s[i], color);
}

static void compose(void)
{
    /* Clear to water. */
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            next_ch[y][x] = ' ';
            next_co[y][x] = W_BLUE;
        }

    /* The surface: a rolling line of ripples whose phase drifts with time. */
    static int wave;
    wave++;
    for (int x = 0; x < COLS; x++)
        put(surface_row(), x, ((x + wave / 4) & 3) == 0 ? '~' : '-',
            W_CYAN | W_BRIGHT);

    /* The sandy floor. */
    for (int x = 0; x < COLS; x++)
        put(floor_row(), x, '_', W_YELLOW);

    /* Seaweed swaying just above the floor. */
    for (int x = 2; x < COLS - 1; x += 7) {
        int h = 2 + (int)((x * 7u) % 3u);
        for (int k = 0; k < h; k++) {
            int y = floor_row() - 1 - k;
            if (y <= water_top())
                break;
            int sway = ((wave / 6) + k + x) & 1;
            put(y, x + sway, sway ? ')' : '(', W_GREEN);
        }
    }

    /* Bubbles rise and pop at the surface. */
    for (int i = 0; i < MAX_BUBBLE; i++)
        if (bubble[i].active)
            put(bubble[i].y, bubble[i].x, 'o', W_CYAN);

    /* Fish, drawn last so they pass in front of the plants. */
    for (int i = 0; i < fish_count; i++) {
        const char *sprite = (fish[i].dir > 0) ? fish[i].right : fish[i].left;
        put_str(fish[i].y, fish[i].x, sprite, fish[i].color);
    }
}

static void step(void)
{
    for (int i = 0; i < fish_count; i++) {
        if (++fish[i].phase < fish[i].speed)
            continue;
        fish[i].phase = 0;
        fish[i].x += fish[i].dir;

        int len = (int)strlen(fish[i].dir > 0 ? fish[i].right : fish[i].left);
        if (fish[i].dir > 0 && fish[i].x > COLS)
            spawn_fish(i);
        else if (fish[i].dir < 0 && fish[i].x + len < 0)
            spawn_fish(i);

        /* Now and then a fish lets out a bubble. */
        if ((rnd() % 20) == 0) {
            for (int b = 0; b < MAX_BUBBLE; b++)
                if (!bubble[b].active) {
                    bubble[b].active = 1;
                    bubble[b].x = fish[i].x + (fish[i].dir > 0 ? len : -1);
                    bubble[b].y = fish[i].y;
                    break;
                }
        }
    }

    for (int b = 0; b < MAX_BUBBLE; b++) {
        if (!bubble[b].active)
            continue;
        bubble[b].y--;
        if (bubble[b].y <= surface_row())
            bubble[b].active = 0;
    }
}

/* Send only the cells that differ from what is already on screen. */
static void render(void)
{
    int last_color = -999;
    int cy = -1, cx = -1;

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            if (next_ch[y][x] == cur_ch[y][x] && next_co[y][x] == cur_co[y][x])
                continue;

            if (y != cy || x != cx)
                wgotoxy(y + 1, x + 1);

            if (next_co[y][x] != last_color) {
                last_color = next_co[y][x];
                wcolor(last_color, W_BLUE);
            }

            char c = next_ch[y][x];
            wwrite(W_STDOUT, &c, 1);

            cur_ch[y][x] = next_ch[y][x];
            cur_co[y][x] = next_co[y][x];
            cy = y;
            cx = x + 1;      /* the terminal advanced the cursor for us */
        }
    }
}

/* Take the terminal's size, held to what the scene can be drawn in.  Returns
 * 1 when it changed, which means the tank is a different shape and everything
 * in it has to be put back. */
static int measure(void)
{
    int r = 0, c = 0;

    if (wconsize(&r, &c) < 0)
        return 0;

    if (r > MAXR) r = MAXR;
    if (c > MAXC) c = MAXC;
    if (r < 5)  r = 5;
    if (c < 10) c = 10;

    if (r == ROWS && c == COLS)
        return 0;

    ROWS = r;
    COLS = c;
    return 1;
}

/* Return 1 if the user asked to quit. */
static int drained_quit(void)
{
    while (wpollin(W_STDIN)) {
        char c;
        if (wread(W_STDIN, &c, 1) <= 0)
            return 1;                 /* input closed: nobody left to watch */
        if (c == 'q' || c == 'Q' || c == 0x1B || c == 0x03)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    measure();

    /* Raw mode so q is seen at once and not echoed; a no-op when stdin is a
     * pipe, which is exactly right there. */
    int prev = wconsole_raw(W_CONSOLE_RAW);

    wcursor(0);
    wcolor(W_WHITE, W_BLUE);
    wcls();

    /* Force the first frame to draw every cell. */
    for (int y = 0; y < MAXR; y++)
        for (int x = 0; x < MAXC; x++) {
            cur_ch[y][x] = 0;
            cur_co[y][x] = -100;
        }

    init_scene();

    int quit = 0;
    while (!quit) {
        /* A tank that changed shape is stocked again: the fish were placed
         * for the old one and half of them would be outside this one. */
        if (measure()) {
            for (int y = 0; y < MAXR; y++)
                for (int x = 0; x < MAXC; x++) {
                    cur_ch[y][x] = 0;
                    cur_co[y][x] = -100;
                }
            init_scene();
            wcls();
        }

        compose();
        render();
        step();

        /* Hold the frame for ~120 ms, staying responsive to q throughout. */
        unsigned start = wticks();
        while (wticks() - start < 12) {
            if (drained_quit()) { quit = 1; break; }
            wsleep(10);        /* wait for the frame, do not spin for it */
        }
    }

    wcolor_reset();
    wcursor(1);
    wcls();
    wconsole_raw(prev);
    return 0;
}
