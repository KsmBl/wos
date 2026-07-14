/* login -- the login screen, and the thing that starts your session.
 *
 * The kernel runs this instead of a shell once the machine is up.  It shows
 * one box per account, you walk to yours with the arrows, type the password
 * into it and press Enter; sway starts as you.
 *
 * It is a display manager in shape -- sddm and lightdm do the same three
 * things in the same order -- and the one interesting problem it has is the
 * order in which they must happen.  A process in WOS can drop to another user
 * but never climb back, and taking the screen needs root.  So a login manager
 * that authenticated first and then thought about the display would have
 * nothing left to grant with: it stopped being root at the moment it succeeded.
 *
 * Hence wseatgrant(), armed once at the top of main() while we are certainly
 * root.  It says "whatever I spawn next may have the screen and the keyboard",
 * and the only thing this program ever spawns is a session it has already
 * accepted a password for.  The arm is spent by that spawn and goes no
 * further, so the compositor holds the seat and the terminals it opens are
 * ordinary processes.
 *
 * When the session ends this program ends with it, and the kernel starts a
 * fresh one -- as root again, since the kernel is root.  That is what brings
 * the login screen back after a logout, and why nothing here has to undo the
 * drop it made.
 */

#include <wkernel.h>

/* One account's box, and the gap between two of them.  Sized so three fit
 * across an 80-column console with a margin: 3 * 24 + 2 * 2 = 76. */
#define BOX_W 24
#define BOX_H 9
#define GAP   2

/* The password field inside a box, and how many dots it has room for once its
 * own frame and the cursor after them are taken out. */
#define FIELD_W    (BOX_W - 6)
#define FIELD_DOTS (FIELD_W - 3)

/* Line-drawing glyphs from the console's own font, which is the IBM VGA set in
 * both text mode and the framebuffer.  The chosen account gets the double
 * frame -- a difference in shape, so it reads as chosen on a screen where the
 * colours have washed out as well as on one where they have not. */
struct frame {
    unsigned char tl, top, tr, side, bl, br;
};

static const struct frame frame_double = { 0xC9, 0xCD, 0xBB, 0xBA, 0xC8, 0xBC };
static const struct frame frame_single = { 0xDA, 0xC4, 0xBF, 0xB3, 0xC0, 0xD9 };

#define DOT 0xFE            /* what a typed character shows as */

static wuser_t users[W_MAX_USERS];
static int     user_count;
static int     selected;
static int     first;               /* leftmost account on screen */

static char password[128];
static int  password_len;

static char message[96];

static int rows, cols;
static int graphical;               /* is there a framebuffer to run sway on */

/* Where things sit vertically.  Computed once from the console size, so this
 * fills a 160x50 framebuffer console as readily as an 80x25 text one. */
static int boxes_top;
static int visible;                 /* how many boxes fit across */

/* ------------------------------------------------------------------ *
 *  Drawing
 * ------------------------------------------------------------------ */

/* A horizontal run: a corner, a fill, a corner. */
static void hline(char *out, wsize_t size, const struct frame *f,
                  int left_corner, int width)
{
    int at = 0;

    if (width < 2 || (wsize_t)width + 1 > size)
        width = (int)size - 1;

    out[at++] = (char)(left_corner ? f->tl : f->bl);
    for (int i = 0; i < width - 2; i++)
        out[at++] = (char)f->top;
    out[at++] = (char)(left_corner ? f->tr : f->br);
    out[at] = '\0';
}

static void centred(int row, int col, int width, const char *text)
{
    int len = (int)strlen(text);

    if (len > width)
        len = width;

    wgotoxy(row, col + (width - len) / 2);
    wprintf("%.*s", len, text);
}

/* An empty box: the frame, and blanks inside it so whatever the last account
 * drawn there had is gone. */
static void draw_frame(int row, int col, int width, int height,
                       const struct frame *f, int fg)
{
    char line[BOX_W + 1];

    wcolor(fg, W_DEFAULT);

    hline(line, sizeof(line), f, 1, width);
    wgotoxy(row, col);
    wprintf("%s", line);

    for (int i = 1; i < height - 1; i++) {
        wgotoxy(row + i, col);
        wprintf("%c", f->side);
        for (int j = 0; j < width - 2; j++)
            wprintf(" ");
        wprintf("%c", f->side);
    }

    hline(line, sizeof(line), f, 0, width);
    wgotoxy(row + height - 1, col);
    wprintf("%s", line);

    wcolor_reset();
}

/* What to say under a name.  Root is called what it is; everyone else is
 * described by what they may do, since that is the only thing distinguishing
 * two accounts here. */
static const char *describe(const wuser_t *u)
{
    static char text[BOX_W];

    if (u->uid == W_ROOT_UID)
        return "administrator";

    if (u->roles == 0)
        return "standard account";

    wsnprintf(text, sizeof(text), "%s%s%s%s",
              (u->roles & W_ROLE_APPEDITOR)  ? "app "  : "",
              (u->roles & W_ROLE_USEREDITOR) ? "user " : "",
              (u->roles & W_ROLE_EDITFREQ)   ? "freq " : "",
              (u->roles & W_ROLE_SYSCTLEDIT) ? "svc "  : "");

    return text;
}

/* A shadow, the way every dialog in a text user interface has had one since
 * Turbo Vision: the column to the right of a box and the row under it, in a
 * shade glyph rather than a dark colour, so it still reads as a shadow on a
 * console whose palette has no grey.
 *
 * Clipped rather than assumed to fit.  The boxes are centred, so on a console
 * whose width they exactly fill there is no column to the right of the last
 * one, and drawing there would wrap onto the next line. */
static void draw_shadow(int row, int col, int width, int height)
{
    wcolor(W_BLACK + W_BRIGHT, W_DEFAULT);

    if (col + width <= cols) {
        for (int i = 1; i < height; i++) {
            wgotoxy(row + i, col + width);
            wprintf("%c", 0xB1);
        }
    }

    int under = width;
    if (col + under > cols)
        under = cols - col;

    if (under > 0) {
        wgotoxy(row + height, col + 1);
        for (int i = 0; i < under; i++)
            wprintf("%c", 0xB1);
    }

    wcolor_reset();
}

static void draw_account(int index, int col)
{
    const wuser_t *u      = &users[index];
    int            chosen = (index == selected);
    int            row    = boxes_top;

    draw_shadow(row, col, BOX_W, BOX_H);

    draw_frame(row, col, BOX_W, BOX_H,
               chosen ? &frame_double : &frame_single,
               chosen ? W_CYAN + W_BRIGHT : W_BLACK + W_BRIGHT);

    /* The name, then what the account is. */
    wcolor(chosen ? W_WHITE + W_BRIGHT : W_WHITE, W_DEFAULT);
    centred(row + 2, col + 1, BOX_W - 2, u->name);
    wcolor_reset();

    wcolor(W_BLACK + W_BRIGHT, W_DEFAULT);
    centred(row + 3, col + 1, BOX_W - 2, describe(u));
    wcolor_reset();

    /* The password field.  Every box has one, drawn the same way, because a
     * field that appeared only under the chosen account would make the boxes
     * change height as you walked along them. */
    draw_frame(row + 5, col + 3, FIELD_W, 3, &frame_single,
               chosen ? W_WHITE : W_BLACK + W_BRIGHT);

    if (!chosen)
        return;

    int shown = password_len < FIELD_DOTS ? password_len : FIELD_DOTS;

    wgotoxy(row + 6, col + 4);
    wcolor(W_CYAN + W_BRIGHT, W_DEFAULT);
    for (int i = 0; i < shown; i++)
        wprintf("%c", DOT);
    wcolor_reset();
}

/* The bar at the top and the bar at the bottom, in the same reverse video the
 * rest of this system's full-screen programs use. */
static void draw_chrome(void)
{
    char right[64];
    wtime_t now;

    /* Bright on blue rather than the black on blue the other full-screen
     * programs use for a footer.  This one is read before anybody has logged
     * in, on whatever contrast the monitor happens to be set to. */
    wgotoxy(1, 1);
    wcolor(W_WHITE + W_BRIGHT, W_BLUE);
    wprintf("%-*.*s", cols, cols, " WOS");

    if (wtime_get(&now) == 0) {
        wsnprintf(right, sizeof(right), "%d-%02d-%02d %02d:%02d ",
                  now.year, now.month, now.day, now.hour, now.minute);
        int at = cols - (int)strlen(right) + 1;
        if (at > 6) {
            wgotoxy(1, at);
            wprintf("%s", right);
        }
    }
    wcolor_reset();

    wgotoxy(3, 1);
    wclear_line();
    wcolor(W_WHITE + W_BRIGHT, W_DEFAULT);
    centred(3, 1, cols, graphical ? "Choose an account"
                                  : "Choose an account -- this machine has no "
                                    "framebuffer, so sessions are text");
    wcolor_reset();

    /* Where you are in the list, on a machine with more accounts than fit
     * across the screen.  The arrows beside the boxes say there is more; this
     * says how much more, and unlike them it always has somewhere to go. */
    if (user_count > 1) {
        wsnprintf(right, sizeof(right), "%d of %d ", selected + 1, user_count);
        int at = cols - (int)strlen(right) + 1;
        if (at > 1) {
            wgotoxy(3, at);
            wcolor(W_BLACK + W_BRIGHT, W_DEFAULT);
            wprintf("%s", right);
            wcolor_reset();
        }
    }

    /* A line of instructions under the boxes, because the one thing a login
     * screen must never do is leave somebody wondering what it wants. */
    wgotoxy(boxes_top + BOX_H + 2, 1);
    wclear_line();
    wcolor(W_BLACK + W_BRIGHT, W_DEFAULT);
    centred(boxes_top + BOX_H + 2, 1, cols,
            user_count > 1 ? "Choose an account, type its password, press Enter"
                           : "Type the password and press Enter");
    wcolor_reset();

    wgotoxy(rows, 1);
    wcolor(W_WHITE + W_BRIGHT, W_BLUE);
    wprintf("%-*.*s", cols, cols,
            graphical
              ? "  arrows  choose account   Enter  log in   "
                "F2  console session   Esc  clear"
              : "  arrows  choose account   Enter  log in   Esc  clear");
    wcolor_reset();
}

/* The row of boxes, and the arrows saying there are more accounts off the
 * side.  Only as many as fit are drawn; a machine with thirty users scrolls
 * rather than shrinking the boxes to nothing. */
static void draw_accounts(void)
{
    if (selected < first)
        first = selected;
    if (selected >= first + visible)
        first = selected - visible + 1;

    int count = user_count - first;
    if (count > visible)
        count = visible;

    int span  = count * BOX_W + (count - 1) * GAP;
    int start = 1 + (cols - span) / 2;

    /* The whole band, rather than the slots about to be used.  The boxes are
     * centred on how many there are, so scrolling and a changing count both
     * move them sideways, and blanking only where the new ones will go leaves
     * pieces of the old ones beside them.  One row deeper than the boxes, for
     * the shadow that hangs below them. */
    for (int r = 0; r <= BOX_H; r++) {
        wgotoxy(boxes_top + r, 1);
        wclear_line();
    }

    for (int i = 0; i < count; i++)
        draw_account(first + i, start + i * (BOX_W + GAP));

    /* Arrows beside the band when there are accounts off the side of it, and
     * only where there is room: with the boxes filling the width there is no
     * column to put one in, which is what the counter in the heading is for. */
    wcolor(W_CYAN + W_BRIGHT, W_DEFAULT);

    if (first > 0 && start - 2 >= 1) {
        wgotoxy(boxes_top + BOX_H / 2, start - 2);
        wprintf("%c", 0x11);                          /* a left arrow  */
    }
    if (first + count < user_count && start + span + 1 <= cols) {
        wgotoxy(boxes_top + BOX_H / 2, start + span + 1);
        wprintf("%c", 0x10);                          /* a right arrow */
    }

    wcolor_reset();
}

static void draw_message(void)
{
    int row = rows - 2;

    wgotoxy(row, 1);
    wclear_line();

    if (!message[0])
        return;

    /* Always red: the only thing this screen has to say is that a password
     * was wrong. */
    wcolor(W_RED + W_BRIGHT, W_DEFAULT);
    centred(row, 1, cols, message);
    wcolor_reset();
}

static void draw(void)
{
    wcursor(0);
    draw_chrome();
    draw_accounts();
    draw_message();

    /* Leave the cursor where the next character will land, inside the chosen
     * account's field, so the box you are typing into is the one blinking. */
    int count = user_count - first;
    if (count > visible)
        count = visible;

    int span  = count * BOX_W + (count - 1) * GAP;
    int start = 1 + (cols - span) / 2;
    int col   = start + (selected - first) * (BOX_W + GAP);
    int shown = password_len < FIELD_DOTS ? password_len : FIELD_DOTS;

    wgotoxy(boxes_top + 6, col + 4 + shown);
    wcursor(1);
}

/* ------------------------------------------------------------------ *
 *  Logging in
 * ------------------------------------------------------------------ */

/* Hand the machine over.  Everything after a successful wlogin() happens as
 * the user, permanently: there is no way back to root from here, which is why
 * this function does not return on success -- it waits for the session and
 * then lets main() exit so the kernel can start a fresh login as itself. */
static int start_session(int want_graphics)
{
    const wuser_t *u = &users[selected];

    if (wlogin(u->name, password) < 0) {
        strlcpy(message, "That password was not right.", sizeof(message));
        password_len = 0;
        password[0]  = '\0';

        /* The same pause every login prompt has had for fifty years.  It does
         * not defeat anything determined -- there is one keyboard and no way
         * to try in parallel -- but it stops a mistyped password from being
         * retried faster than it can be read. */
        wsleep(700);
        return -1;
    }

    /* We are that user now. */
    wcursor(1);
    wconsole_raw(W_CONSOLE_CANONICAL);
    wcls();

    char home[W_PATH_MAX + 1];
    wsnprintf(home, sizeof(home), "/home/%s", u->name);
    wchdir(home);

    char path[W_SHELL_MAX + 1];
    if (want_graphics)
        strlcpy(path, "/app/sway/launch", sizeof(path));
    else if (wgetshell(-1, path, sizeof(path)) < 0)
        strlcpy(path, "/app/whell/launch", sizeof(path));

    char *argv[] = { want_graphics ? "sway" : "shell", NULL };

    int pid = wspawn(path, argv);
    if (pid < 0) {
        wprintf("login: cannot start %s: %s\n", path, wstrerror(-pid));
        wprintf("login: press a key.\n");
        wgetkey();
        return 0;
    }

    int status = 0;
    wwait(pid, &status);

    /* A session that failed says why, and the screen holds still until it has
     * been read.  Without this the kernel would restart the login screen over
     * the top of the explanation, and a machine whose compositor cannot start
     * would loop with nothing to show for it. */
    if (status != 0) {
        wprintf("\nlogin: the session ended with status %d.\n", status);
        wprintf("login: press a key for the login screen.\n");
        wgetkey();
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Setting up
 * ------------------------------------------------------------------ */

static void load_users(void)
{
    user_count = wuserlist(users, W_MAX_USERS);
    if (user_count < 0)
        user_count = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Armed here, at the top, while we are certainly root -- see the note on
     * this file.  Nothing is given away by arming: the grant is spent by a
     * spawn, and the only thing this program spawns is a session that has
     * already given a password. */
    wseatgrant();

    load_users();
    if (user_count == 0) {
        /* No account list means something is wrong with /userconfig that a
         * login screen cannot fix, so get out of the way and let the kernel
         * fall back to a shell rather than showing an empty picker. */
        wfprintf(W_STDERR, "login: no accounts; cannot offer a login.\n");
        return 1;
    }

    wdisplay_t screen;
    graphical = (wdisplayinfo(&screen) == 0 && screen.present);

    wconsize(&rows, &cols);

    /* Enough for the heading, the boxes with their shadow, the line of
     * instructions under them, and the two bars.  Anything smaller is refused
     * rather than drawn over itself. */
    if (rows < BOX_H + 10 || cols < BOX_W + 2) {
        wfprintf(W_STDERR, "login: the console is too small for a login "
                           "screen (%dx%d).\n", cols, rows);
        return 1;
    }

    visible = (cols + GAP) / (BOX_W + GAP);
    if (visible < 1)
        visible = 1;

    /* Centred in what is left between the heading and the instructions. */
    boxes_top = 5 + (rows - 10 - BOX_H) / 2;

    wconsole_raw(W_CONSOLE_RAW);
    wcls();

    for (;;) {
        draw();

        int key = wgetkey();

        switch (key) {
        case W_KEY_LEFT:
        case W_KEY_UP:
            if (selected > 0) {
                selected--;
                password_len = 0;
                password[0]  = '\0';
                message[0]   = '\0';
            }
            break;

        case W_KEY_RIGHT:
        case W_KEY_DOWN:
        case '\t':
            if (selected + 1 < user_count) {
                selected++;
                password_len = 0;
                password[0]  = '\0';
                message[0]   = '\0';
            }
            break;

        case W_KEY_HOME: selected = 0;              break;
        case W_KEY_END:  selected = user_count - 1; break;

        case W_KEY_ESCAPE:
            password_len = 0;
            password[0]  = '\0';
            message[0]   = '\0';
            break;

        case '\b':
        case 127:
            if (password_len > 0)
                password[--password_len] = '\0';
            message[0] = '\0';
            break;

        case '\n':
        case '\r':
            if (start_session(graphical) == 0)
                return 0;
            wconsole_raw(W_CONSOLE_RAW);
            wcls();
            break;

        case W_KEY_F2:
            /* A text session, for a machine with no framebuffer and for the
             * times when the compositor is the thing you are trying to fix.
             * It carries the seat too, so `sway` typed into it still works. */
            if (start_session(0) == 0)
                return 0;
            wconsole_raw(W_CONSOLE_RAW);
            wcls();
            break;

        default:
            if (key >= ' ' && key < 0x7F &&
                (wsize_t)password_len + 1 < sizeof(password)) {
                password[password_len++] = (char)key;
                password[password_len]   = '\0';
                message[0] = '\0';
            }
            break;
        }
    }
}
