/* chess -- a two-player chess game for WOS.
 *
 * Hotseat: two people share the keyboard, entering moves in coordinate
 * notation ("e2e4", "e7e8q" to promote).  All the real rules are enforced --
 * legal piece movement, you may not leave your own king in check, castling, en
 * passant, promotion -- and the game recognises check, checkmate and
 * stalemate.  There is no computer opponent; this is two humans and a referee.
 *
 * The board is drawn with a coloured checkerboard, white pieces in bright
 * white as capitals and black pieces in bright yellow as lowercase, the usual
 * convention.
 */

#include <wkernel.h>

/* b[row][col]: row 0 is rank 8 at the top, row 7 is rank 1 at the bottom;
 * col 0 is file a.  Uppercase is white, lowercase black, space is empty. */
static char b[8][8];
static int  white_to_move;
static int  cr_wk, cr_wq, cr_bk, cr_bq;   /* castling rights          */
static int  ep_r, ep_c;                    /* en-passant target, or -1 */
static int  fullmove;
static char message[96];

struct move {
    int  fr, fc, tr, tc;
    char promo;        /* promotion piece (lowercase), or 0 */
    int  castle;       /* +1 kingside, -1 queenside, 0 none */
    int  is_ep;        /* en-passant capture                */
    int  dbl;          /* two-square pawn push              */
};

/* ------------------------------------------------------------------ *
 *  Board helpers
 * ------------------------------------------------------------------ */

static int on_board(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }
static int is_white(char p)       { return p >= 'A' && p <= 'Z'; }
static int is_black(char p)       { return p >= 'a' && p <= 'z'; }
static int is_empty(char p)       { return p == ' '; }
static char to_lower(char p)      { return (p >= 'A' && p <= 'Z') ? (char)(p - 'A' + 'a') : p; }

static int mine(char p, int white) { return white ? is_white(p) : is_black(p); }
static int foe(char p, int white)  { return white ? is_black(p) : is_white(p); }

static void setup(void)
{
    static const char back[8] = { 'r','n','b','q','k','b','n','r' };
    for (int c = 0; c < 8; c++) {
        b[0][c] = back[c];
        b[1][c] = 'p';
        for (int r = 2; r < 6; r++)
            b[r][c] = ' ';
        b[6][c] = 'P';
        b[7][c] = (char)(back[c] - 'a' + 'A');
    }
    white_to_move = 1;
    cr_wk = cr_wq = cr_bk = cr_bq = 1;
    ep_r = ep_c = -1;
    fullmove = 1;
    message[0] = '\0';
}

/* Is square (r,c) attacked by the given side? */
static int attacked(int r, int c, int by_white)
{
    /* Pawns.  A white pawn on (r+1,c±1) attacks (r,c); a black pawn on
     * (r-1,c±1) does. */
    int pr = by_white ? r + 1 : r - 1;
    char pw = by_white ? 'P' : 'p';
    if (on_board(pr, c - 1) && b[pr][c - 1] == pw) return 1;
    if (on_board(pr, c + 1) && b[pr][c + 1] == pw) return 1;

    /* Knights. */
    static const int ko[8][2] = { {-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1} };
    char kn = by_white ? 'N' : 'n';
    for (int i = 0; i < 8; i++) {
        int rr = r + ko[i][0], cc = c + ko[i][1];
        if (on_board(rr, cc) && b[rr][cc] == kn) return 1;
    }

    /* King. */
    char kg = by_white ? 'K' : 'k';
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int rr = r + dr, cc = c + dc;
            if (on_board(rr, cc) && b[rr][cc] == kg) return 1;
        }

    /* Rook / queen along ranks and files. */
    static const int ro[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
    char R = by_white ? 'R' : 'r', Q = by_white ? 'Q' : 'q';
    for (int i = 0; i < 4; i++) {
        int rr = r + ro[i][0], cc = c + ro[i][1];
        while (on_board(rr, cc)) {
            char p = b[rr][cc];
            if (!is_empty(p)) {
                if (p == R || p == Q) return 1;
                break;
            }
            rr += ro[i][0]; cc += ro[i][1];
        }
    }

    /* Bishop / queen along diagonals. */
    static const int bo[4][2] = { {-1,-1},{-1,1},{1,-1},{1,1} };
    char B = by_white ? 'B' : 'b';
    for (int i = 0; i < 4; i++) {
        int rr = r + bo[i][0], cc = c + bo[i][1];
        while (on_board(rr, cc)) {
            char p = b[rr][cc];
            if (!is_empty(p)) {
                if (p == B || p == Q) return 1;
                break;
            }
            rr += bo[i][0]; cc += bo[i][1];
        }
    }

    return 0;
}

static void find_king(int white, int *kr, int *kc)
{
    char k = white ? 'K' : 'k';
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (b[r][c] == k) { *kr = r; *kc = c; return; }
    *kr = *kc = -1;
}

static int in_check(int white)
{
    int kr, kc;
    find_king(white, &kr, &kc);
    if (kr < 0) return 0;
    return attacked(kr, kc, !white);
}

/* ------------------------------------------------------------------ *
 *  Move generation
 * ------------------------------------------------------------------ */

static void add_move(struct move *list, int *n, int fr, int fc, int tr, int tc,
                     char promo, int castle, int is_ep, int dbl)
{
    struct move *m = &list[(*n)++];
    m->fr = fr; m->fc = fc; m->tr = tr; m->tc = tc;
    m->promo = promo; m->castle = castle; m->is_ep = is_ep; m->dbl = dbl;
}

static void add_pawn(struct move *list, int *n, int fr, int fc, int tr, int tc,
                     int is_ep, int dbl, int white)
{
    int last = white ? 0 : 7;
    if (tr == last) {
        add_move(list, n, fr, fc, tr, tc, 'q', 0, 0, 0);
        add_move(list, n, fr, fc, tr, tc, 'r', 0, 0, 0);
        add_move(list, n, fr, fc, tr, tc, 'b', 0, 0, 0);
        add_move(list, n, fr, fc, tr, tc, 'n', 0, 0, 0);
    } else {
        add_move(list, n, fr, fc, tr, tc, 0, 0, is_ep, dbl);
    }
}

/* Every pseudo-legal move for `white`, before the check filter. */
static int gen_pseudo(struct move *list, int white)
{
    int n = 0;
    int dir   = white ? -1 : 1;
    int start = white ? 6 : 1;

    static const int ko[8][2] = { {-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1} };
    static const int ro[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
    static const int bo[4][2] = { {-1,-1},{-1,1},{1,-1},{1,1} };

    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            char p = b[r][c];
            if (is_empty(p) || !mine(p, white)) continue;
            char t = to_lower(p);

            if (t == 'p') {
                if (on_board(r + dir, c) && is_empty(b[r + dir][c])) {
                    add_pawn(list, &n, r, c, r + dir, c, 0, 0, white);
                    if (r == start && is_empty(b[r + 2 * dir][c]))
                        add_pawn(list, &n, r, c, r + 2 * dir, c, 0, 1, white);
                }
                for (int dc = -1; dc <= 1; dc += 2) {
                    int tr = r + dir, tc = c + dc;
                    if (!on_board(tr, tc)) continue;
                    if (foe(b[tr][tc], white))
                        add_pawn(list, &n, r, c, tr, tc, 0, 0, white);
                    else if (tr == ep_r && tc == ep_c)
                        add_pawn(list, &n, r, c, tr, tc, 1, 0, white);
                }
            } else if (t == 'n') {
                for (int i = 0; i < 8; i++) {
                    int tr = r + ko[i][0], tc = c + ko[i][1];
                    if (on_board(tr, tc) && !mine(b[tr][tc], white))
                        add_move(list, &n, r, c, tr, tc, 0, 0, 0, 0);
                }
            } else if (t == 'k') {
                for (int dr = -1; dr <= 1; dr++)
                    for (int dc = -1; dc <= 1; dc++) {
                        if (!dr && !dc) continue;
                        int tr = r + dr, tc = c + dc;
                        if (on_board(tr, tc) && !mine(b[tr][tc], white))
                            add_move(list, &n, r, c, tr, tc, 0, 0, 0, 0);
                    }
                /* Castling: rights set, squares empty, king not passing through
                 * an attacked square. */
                int hr = white ? 7 : 0;
                int ck = white ? cr_wk : cr_bk;
                int cq = white ? cr_wq : cr_bq;
                if (r == hr && c == 4 && !attacked(hr, 4, !white)) {
                    if (ck && is_empty(b[hr][5]) && is_empty(b[hr][6]) &&
                        !attacked(hr, 5, !white) && !attacked(hr, 6, !white))
                        add_move(list, &n, r, c, hr, 6, 0, +1, 0, 0);
                    if (cq && is_empty(b[hr][1]) && is_empty(b[hr][2]) &&
                        is_empty(b[hr][3]) &&
                        !attacked(hr, 3, !white) && !attacked(hr, 2, !white))
                        add_move(list, &n, r, c, hr, 2, 0, -1, 0, 0);
                }
            } else {
                /* Sliding pieces: bishop, rook, queen. */
                const int (*dirs)[2] = (t == 'b') ? bo : (t == 'r') ? ro : NULL;
                int ndir = 4, both = (t == 'q');
                for (int pass = 0; pass < (both ? 2 : 1); pass++) {
                    const int (*d)[2] = both ? (pass == 0 ? ro : bo) : dirs;
                    for (int i = 0; i < ndir; i++) {
                        int tr = r + d[i][0], tc = c + d[i][1];
                        while (on_board(tr, tc)) {
                            if (is_empty(b[tr][tc])) {
                                add_move(list, &n, r, c, tr, tc, 0, 0, 0, 0);
                            } else {
                                if (foe(b[tr][tc], white))
                                    add_move(list, &n, r, c, tr, tc, 0, 0, 0, 0);
                                break;
                            }
                            tr += d[i][0]; tc += d[i][1];
                        }
                    }
                }
            }
        }
    return n;
}

/* Apply a move to the board.  Does not touch rights or side to move -- that is
 * done by commit() for a real move; this is also used to test legality on a
 * scratch board. */
static void do_move(const struct move *m, int white)
{
    char p = b[m->fr][m->fc];

    b[m->fr][m->fc] = ' ';

    if (m->is_ep)
        b[m->fr][m->tc] = ' ';          /* the pawn passed is captured */

    if (m->promo)
        p = white ? (char)(m->promo - 'a' + 'A') : m->promo;

    b[m->tr][m->tc] = p;

    if (m->castle > 0) {                /* kingside: rook h -> f */
        b[m->tr][5] = b[m->tr][7];
        b[m->tr][7] = ' ';
    } else if (m->castle < 0) {         /* queenside: rook a -> d */
        b[m->tr][3] = b[m->tr][0];
        b[m->tr][0] = ' ';
    }
}

/* A move is legal if, after playing it, the mover is not in check. */
static int legal(const struct move *m, int white)
{
    char save[8][8];
    memcpy(save, b, sizeof(b));
    do_move(m, white);
    int ok = !in_check(white);
    memcpy(b, save, sizeof(b));
    return ok;
}

static int gen_legal(struct move *list, int white)
{
    struct move pseudo[256];
    int np = gen_pseudo(pseudo, white);
    int n = 0;
    for (int i = 0; i < np; i++)
        if (legal(&pseudo[i], white))
            list[n++] = pseudo[i];
    return n;
}

/* Play a real move: apply it and update rights, the en-passant square, the
 * move number and the side to move. */
static void commit(const struct move *m, int white)
{
    char p = b[m->fr][m->fc];

    do_move(m, white);

    /* Castling rights: lost when the king or a rook leaves its square, or when
     * a rook is captured on its square. */
    if (to_lower(p) == 'k') {
        if (white) cr_wk = cr_wq = 0; else cr_bk = cr_bq = 0;
    }
    if ((m->fr == 7 && m->fc == 0) || (m->tr == 7 && m->tc == 0)) cr_wq = 0;
    if ((m->fr == 7 && m->fc == 7) || (m->tr == 7 && m->tc == 7)) cr_wk = 0;
    if ((m->fr == 0 && m->fc == 0) || (m->tr == 0 && m->tc == 0)) cr_bq = 0;
    if ((m->fr == 0 && m->fc == 7) || (m->tr == 0 && m->tc == 7)) cr_bk = 0;

    ep_r = ep_c = -1;
    if (m->dbl) {
        ep_r = (m->fr + m->tr) / 2;
        ep_c = m->fc;
    }

    if (!white)
        fullmove++;
    white_to_move = !white;
}

/* ------------------------------------------------------------------ *
 *  Rendering
 * ------------------------------------------------------------------ */

static void draw(void)
{
    wcls();
    wcolor_reset();
    wprintf("  WOS Chess\r\n\r\n");
    wprintf("     a  b  c  d  e  f  g  h\r\n");

    for (int r = 0; r < 8; r++) {
        wcolor_reset();
        wprintf("  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int light = ((r + c) & 1) == 0;
            int bg = light ? W_CYAN : W_BLUE;
            char p = b[r][c];

            if (is_empty(p)) {
                wcolor(W_WHITE, bg);
                wprintf("   ");
            } else {
                int fg = is_white(p) ? (W_WHITE | W_BRIGHT) : (W_YELLOW | W_BRIGHT);
                wcolor(fg, bg);
                wprintf(" %c ", p);
            }
        }
        wcolor_reset();
        wprintf(" %d\r\n", 8 - r);
    }

    wcolor_reset();
    wprintf("     a  b  c  d  e  f  g  h\r\n\r\n");

    if (message[0])
        wprintf("%s\r\n", message);

    wprintf("%s to move.  Enter a move (e2e4), or 'help'.\r\n",
            white_to_move ? "White" : "Black");
    wprintf("> ");
}

/* ------------------------------------------------------------------ *
 *  Input
 * ------------------------------------------------------------------ */

static int read_line(char *buf, int size)
{
    int n = wread(W_STDIN, buf, size - 1);
    if (n <= 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = '\0';
    return n;
}

static void show_help(void)
{
    wcls();
    wcolor_reset();
    wprintf("WOS Chess -- help\r\n\r\n");
    wprintf("Moves are coordinates: <from><to>, e.g.\r\n");
    wprintf("  e2e4     move\r\n");
    wprintf("  e1g1     castle (move the king two squares)\r\n");
    wprintf("  e7e8q    promote (q, r, b or n; q if omitted)\r\n\r\n");
    wprintf("White is UPPERCASE, black is lowercase.\r\n");
    wprintf("All rules are enforced, including check, castling,\r\n");
    wprintf("en passant and promotion.\r\n\r\n");
    wprintf("Commands: new, resign, quit, help\r\n\r\n");
    wprintf("Press Enter to continue.");
    char tmp[8];
    read_line(tmp, sizeof(tmp));
}

/* Find the legal move matching a coordinate string, or return 0. */
static int parse_move(const char *s, struct move *out, int white)
{
    if (strlen(s) < 4)
        return 0;
    int fc = s[0] - 'a';
    int fr = 8 - (s[1] - '0');
    int tc = s[2] - 'a';
    int tr = 8 - (s[3] - '0');
    char promo = (strlen(s) >= 5) ? to_lower(s[4]) : 0;

    if (!on_board(fr, fc) || !on_board(tr, tc))
        return 0;

    struct move list[256];
    int n = gen_legal(list, white);
    for (int i = 0; i < n; i++) {
        struct move *m = &list[i];
        if (m->fr != fr || m->fc != fc || m->tr != tr || m->tc != tc)
            continue;
        if (m->promo) {
            char want = promo ? promo : 'q';
            if (m->promo != want)
                continue;
        }
        *out = *m;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    setup();

    for (;;) {
        struct move list[256];
        int have = gen_legal(list, white_to_move);
        int check = in_check(white_to_move);

        if (have == 0) {
            draw();
            wprintf("\r\n");
            if (check)
                wprintf("Checkmate -- %s wins.\r\n",
                        white_to_move ? "Black" : "White");
            else
                wprintf("Stalemate -- draw.\r\n");
            wprintf("Type 'new' for another game, or 'quit'.\r\n> ");
        } else {
            if (check)
                wsnprintf(message, sizeof(message), "Check!");
            draw();
        }

        char line[64];
        if (read_line(line, sizeof(line)) < 0)
            break;

        message[0] = '\0';

        if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0)
            break;
        if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            show_help();
            continue;
        }
        if (strcmp(line, "new") == 0) {
            setup();
            continue;
        }
        if (strcmp(line, "resign") == 0) {
            draw();
            wprintf("\r\n%s resigns -- %s wins.\r\n",
                    white_to_move ? "White" : "Black",
                    white_to_move ? "Black" : "White");
            wprintf("Type 'new' for another game, or 'quit'.\r\n> ");
            if (read_line(line, sizeof(line)) < 0 || strcmp(line, "new") != 0)
                break;
            setup();
            continue;
        }

        if (have == 0)
            continue;   /* game over: only new/quit above do anything */

        struct move m;
        if (!parse_move(line, &m, white_to_move)) {
            wsnprintf(message, sizeof(message),
                      "Illegal move: %s", line);
            continue;
        }
        commit(&m, white_to_move);
    }

    wcolor_reset();
    wcls();
    return 0;
}
