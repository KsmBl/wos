/* vim -- a modal text editor for WOS.
 *
 * A WOS-native editor in the spirit of vim, not a build of the upstream one:
 * that is roughly 400,000 lines of C over a full libc, termios, fork and a
 * regex engine, none of which exists here.  What is here is the modal model
 * and the key bindings, so the muscle memory carries over.
 *
 * Supported: h j k l and the arrows, 0 ^ $, gg G, w b, Ctrl+D / Ctrl+U,
 * i a I A o O, x, dd, and the :w :q :wq :q! family.
 */

#include "vim.h"

static int  cx, cy;              /* cursor position within the buffer  */
static int  row_offset;          /* first buffer line shown on screen  */
static int  col_offset;          /* first column shown, for long lines */
static int  mode;
static int  running = 1;
static int  pending;             /* a half-typed operator, e.g. the first d */
static char status[128];
static char command[128];
static int  command_len;

/* The console size, read once at startup with wconsize() so the editor follows
 * whatever text mode is in force rather than assuming one. */
static int  con_w = W_CONSOLE_WIDTH;
static int  con_h = W_CONSOLE_HEIGHT;
static int  text_rows;           /* con_h - 1: the rows the buffer occupies */

/* The split geometry, all derived from the console size in layout_init().  The
 * editor keeps the left half, a separator column follows, then the terminal;
 * the row below both holds the status lines and the very bottom row the shared
 * command line. */
static int  sp_left_w;           /* editor width in a split      */
static int  sp_sep_col;          /* the separator column         */
static int  sp_right_x;          /* first terminal column        */
static int  sp_right_w;          /* terminal width               */
static int  sp_content_h;        /* window height above status   */
static int  sp_status_row;       /* the per-window status line   */
static int  cmd_row;             /* the shared command line      */

static void layout_init(void)
{
    int rows = 0, cols = 0;
    if (wconsize(&rows, &cols) == 0 && rows > 0 && cols > 0) {
        con_h = rows;
        con_w = cols;
    }
    text_rows     = con_h - 1;
    sp_left_w     = (con_w - 1) / 2;
    sp_sep_col    = sp_left_w + 1;
    sp_right_x    = sp_sep_col + 1;
    sp_right_w    = con_w - sp_right_x + 1;
    sp_content_h  = con_h - 2;
    sp_status_row = con_h - 1;
    cmd_row       = con_h;
}

/* The size of the editor's view of the buffer.  With no terminal open this is
 * the whole screen above the status line; when :term splits the window it
 * shrinks to the left half.  Set in main() once the console size is known. */
static int  view_w;
static int  view_h;

/* The :term window, and which window the keyboard is talking to. */
static struct wterm term;
enum { FOCUS_EDITOR = 0, FOCUS_TERM };
static int  focus;
static int  ctrl_w_pending;      /* saw Ctrl+W, waiting for the second key */

static int current_length(void)
{
    return (cy < line_count) ? (int)strlen(lines[cy]) : 0;
}

/* Keep the cursor inside the line.  Normal mode sits *on* the last character,
 * insert mode may sit one past it, exactly as vim does. */
static void clamp_cursor(void)
{
    if (cy < 0)
        cy = 0;
    if (cy >= line_count)
        cy = line_count - 1;

    int len = current_length();
    int most = (mode == MODE_INSERT) ? len : (len > 0 ? len - 1 : 0);

    if (cx > most)
        cx = most;
    if (cx < 0)
        cx = 0;
}

/* Scroll so the cursor is on screen. */
static void scroll_to_cursor(void)
{
    if (cy < row_offset)
        row_offset = cy;
    if (cy >= row_offset + view_h)
        row_offset = cy - view_h + 1;

    if (cx < col_offset)
        col_offset = cx;
    if (cx >= col_offset + view_w)
        col_offset = cx - view_w + 1;

    if (row_offset < 0)
        row_offset = 0;
    if (col_offset < 0)
        col_offset = 0;
}

static void draw_status(void)
{
    wgotoxy(con_h, 1);

    if (mode == MODE_COMMAND) {
        wcolor_reset();
        wprintf(":%s", command);
        wclear_line();
        return;
    }

    wcolor(W_BLACK, W_CYAN);

    /* Build both ends first, so the padding between them is just a
     * subtraction rather than a tally of what was printed. */
    char left[96];
    char right[40];

    wsnprintf(left, sizeof(left), "%s %s%s",
              (mode == MODE_INSERT) ? " -- INSERT -- " : " NORMAL ",
              filename[0] ? filename : "[No Name]",
              modified ? " [+]" : "");

    wsnprintf(right, sizeof(right), " %d,%d   %d lines ",
              cy + 1, cx + 1, line_count);

    int pad = con_w - (int)strlen(left) - (int)strlen(right);

    wprintf("%s", left);
    for (int i = 0; i < pad; i++)
        wprintf(" ");
    wprintf("%s", right);
    wcolor_reset();

    /* A message, when there is one, replaces the bar on the next redraw. */
    if (status[0]) {
        wgotoxy(con_h, 1);
        wcolor_reset();
        wprintf("%s", status);
        wclear_line();
    }
}

static void draw_single(void)
{
    for (int row = 0; row < text_rows; row++) {
        int at = row_offset + row;

        wgotoxy(row + 1, 1);

        if (at >= line_count) {
            /* Past the end of the buffer, vim shows a tilde column. */
            wcolor(W_BLUE | W_BRIGHT, W_DEFAULT);
            wprintf("~");
            wcolor_reset();
        } else {
            const char *text = lines[at];
            int len = (int)strlen(text);

            if (col_offset < len) {
                /* Clip to the screen by copying; wprintf has no precision
                 * specifier, only a width. */
                char shown[con_w + 1];
                int  n = len - col_offset;

                if (n > con_w)
                    n = con_w;

                memcpy(shown, text + col_offset, (wsize_t)n);
                shown[n] = '\0';
                wprintf("%s", shown);
            }
        }

        wclear_line();
    }

    draw_status();

    /* Leave the cursor where the user expects to type. */
    if (mode == MODE_COMMAND)
        wgotoxy(con_h, command_len + 2);
    else
        wgotoxy(cy - row_offset + 1, cx - col_offset + 1);
}

/* Write `text` at (row, col) and pad with spaces out to `width` columns, so a
 * window can be cleared without erasing its neighbour the way wclear_line
 * would. */
static void draw_field(int row, int col, int width, const char *text)
{
    wgotoxy(row, col);

    int n = 0;
    for (; text[n] && n < width; n++)
        ;

    char out[con_w + 1];
    memcpy(out, text, (wsize_t)n);
    for (int i = n; i < width; i++)
        out[i] = ' ';
    out[width] = '\0';
    wprintf("%s", out);
}

/* The editor's text, confined to the left window of a split. */
static void draw_split_editor(void)
{
    for (int row = 0; row < sp_content_h; row++) {
        int at = row_offset + row;
        char linebuf[sp_left_w + 1];

        if (at >= line_count) {
            linebuf[0] = '~';
            linebuf[1] = '\0';
        } else {
            const char *text = lines[at];
            int len = (int)strlen(text);
            int n = 0;

            if (col_offset < len) {
                n = len - col_offset;
                if (n > sp_left_w)
                    n = sp_left_w;
                memcpy(linebuf, text + col_offset, (wsize_t)n);
            }
            linebuf[n] = '\0';
        }

        wcolor_reset();
        draw_field(row + 1, 1, sp_left_w, linebuf);
    }
}

/* The separator column and the two window status lines. */
static void draw_split_chrome(void)
{
    /* Separator. */
    wcolor(W_BLUE | W_BRIGHT, W_DEFAULT);
    for (int row = 1; row <= sp_status_row; row++) {
        wgotoxy(row, sp_sep_col);
        wprintf("|");
    }
    wcolor_reset();

    /* Left status: the file, highlighted when the editor has focus. */
    char left[sp_left_w + 1];
    wsnprintf(left, sizeof(left), " %s%s",
              filename[0] ? filename : "[No Name]", modified ? " [+]" : "");
    if (focus == FOCUS_EDITOR) wcolor(W_BLACK, W_CYAN); else wcolor(W_WHITE, W_BLUE);
    draw_field(sp_status_row, 1, sp_left_w, left);

    /* Right status: the terminal, highlighted when it has focus. */
    char right[sp_right_w + 1];
    wsnprintf(right, sizeof(right), " %s", term.open ? "terminal" : "(closed)");
    if (focus == FOCUS_TERM) wcolor(W_BLACK, W_CYAN); else wcolor(W_WHITE, W_BLUE);
    draw_field(sp_status_row, sp_right_x, sp_right_w, right);
    wcolor_reset();
}

/* Everything in the split except the terminal's own cells and the cursor:
 * the editor text, the separator, the status lines and the command line.
 * Drawn only when it changes, so the fast poll loop is quiet. */
static void draw_split_static(void)
{
    draw_split_editor();
    draw_split_chrome();

    /* The command line, shared across the bottom. */
    wgotoxy(cmd_row, 1);
    wcolor_reset();
    if (mode == MODE_COMMAND)
        wprintf(":%s", command);
    else
        wprintf("%s", status);
    wclear_line();
}

/* Park the hardware cursor in whichever window has focus. */
static void place_cursor(void)
{
    if (mode == MODE_COMMAND) {
        wgotoxy(cmd_row, command_len + 2);
    } else if (term.open && focus == FOCUS_TERM) {
        int r, c;
        wterm_cursor(&term, &r, &c);
        wgotoxy(r, c);
    } else {
        wgotoxy(cy - row_offset + 1, cx - col_offset + 1);
    }
}

/* ---------------------------------------------------------------- *
 *  Editing
 * ---------------------------------------------------------------- */

static void insert_char(char c)
{
    int   len = current_length();
    char *text = malloc((wsize_t)len + 2);

    if (!text)
        return;

    memcpy(text, lines[cy], (wsize_t)cx);
    text[cx] = c;
    memcpy(text + cx + 1, lines[cy] + cx, (wsize_t)(len - cx) + 1);

    free(lines[cy]);
    lines[cy] = text;
    cx++;
    modified = 1;
}

static void delete_char_at(int at)
{
    int len = current_length();

    if (at < 0 || at >= len)
        return;

    memmove(lines[cy] + at, lines[cy] + at + 1, (wsize_t)(len - at));
    modified = 1;
}

/* Split the current line at the cursor, as Enter does in insert mode. */
static void split_line(void)
{
    if (line_insert(cy + 1, lines[cy] + cx) < 0)
        return;

    /* Truncating only after the copy succeeded keeps the tail intact if the
     * insert could not allocate. */
    lines[cy][cx] = '\0';

    cy++;
    cx = 0;
    modified = 1;
}

/* Join this line onto the end of the previous one, as Backspace at column 0
 * does in insert mode. */
static void join_with_previous(void)
{
    if (cy == 0)
        return;

    int   previous = (int)strlen(lines[cy - 1]);
    int   len = current_length();
    char *text = malloc((wsize_t)(previous + len + 1));

    if (!text)
        return;

    memcpy(text, lines[cy - 1], (wsize_t)previous);
    memcpy(text + previous, lines[cy], (wsize_t)len + 1);

    free(lines[cy - 1]);
    lines[cy - 1] = text;

    line_remove(cy);
    cy--;
    cx = previous;
    modified = 1;
}

/* ---------------------------------------------------------------- *
 *  Movement
 * ---------------------------------------------------------------- */

static void move_word_forward(void)
{
    const char *text = lines[cy];
    int len = current_length();

    /* Step over this word, then over the gap after it. */
    while (cx < len && text[cx] != ' ' && text[cx] != '\t')
        cx++;
    while (cx < len && (text[cx] == ' ' || text[cx] == '\t'))
        cx++;

    if (cx >= len && cy + 1 < line_count) {
        cy++;
        cx = 0;
    }
}

static void move_word_back(void)
{
    if (cx == 0) {
        if (cy > 0) {
            cy--;
            cx = current_length();
            if (cx > 0)
                cx--;
        }
        return;
    }

    const char *text = lines[cy];

    cx--;
    while (cx > 0 && (text[cx] == ' ' || text[cx] == '\t'))
        cx--;
    while (cx > 0 && text[cx - 1] != ' ' && text[cx - 1] != '\t')
        cx--;
}

/* ---------------------------------------------------------------- *
 *  The :term window
 * ---------------------------------------------------------------- */

static void close_terminal(void)
{
    if (term.open)
        wterm_close(&term);

    focus  = FOCUS_EDITOR;
    view_w = con_w;
    view_h = text_rows;
    wcls();                          /* wipe the split before the editor redraws */
    strlcpy(status, "terminal closed", sizeof(status));
}

/* Open a terminal in the right half and run `argline` in it.  An empty command
 * launches the shell, the way vim's :term does. */
static void open_terminal(const char *argline)
{
    if (term.open) {
        strlcpy(status, "a terminal is already open", sizeof(status));
        return;
    }

    /* Split the argument line into argv, in a local copy we own. */
    char argbuf[W_PATH_MAX + 1];
    char *argv[9];
    int   argc = 0;

    while (*argline == ' ')
        argline++;
    strlcpy(argbuf, argline, sizeof(argbuf));

    char *p = argbuf;
    while (*p && argc < 8) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    argv[argc] = NULL;

    char path[W_PATH_MAX + 1];
    if (argc == 0) {
        strlcpy(path, "/app/whell/launch", sizeof(path));
        argv[0] = "whell";
        argv[1] = NULL;
    } else if (strchr(argv[0], '/')) {
        strlcpy(path, argv[0], sizeof(path));
    } else {
        wsnprintf(path, sizeof(path), "/app/%s/launch", argv[0]);
    }

    /* Shrink the editor to the left half first, so its cursor stays visible. */
    view_w = sp_left_w;
    view_h = sp_content_h;
    wcls();

    int r = wterm_start(&term, path, argv, 1, sp_right_x,
                       sp_content_h, sp_right_w);
    if (r < 0) {
        view_w = con_w;
        view_h = text_rows;
        wcls();
        wsnprintf(status, sizeof(status),
                  "E: cannot start terminal: %s", wstrerror(-r));
        return;
    }

    focus = FOCUS_TERM;
    wsnprintf(status, sizeof(status),
              "terminal: %s  (Ctrl-W Ctrl-W switches windows)", argv[0]);
}

/* ---------------------------------------------------------------- *
 *  Ex commands
 * ---------------------------------------------------------------- */

static void run_command(void)
{
    char *c = command;

    while (*c == ' ')
        c++;

    int force = 0;
    int write = 0;
    int quit  = 0;

    /* ":term [cmd...]" opens a terminal window running cmd (default: a shell). */
    if (strcmp(c, "term") == 0 || strncmp(c, "term ", 5) == 0) {
        open_terminal(c + 4);
        return;
    }

    if (strcmp(c, "w") == 0 || strncmp(c, "w ", 2) == 0) {
        write = 1;
    } else if (strcmp(c, "wq") == 0 || strcmp(c, "x") == 0 ||
               strncmp(c, "wq ", 3) == 0) {
        write = 1;
        quit  = 1;
    } else if (strcmp(c, "q") == 0) {
        quit = 1;
    } else if (strcmp(c, "q!") == 0) {
        quit  = 1;
        force = 1;
    } else if (strcmp(c, "w!") == 0) {
        write = 1;
        force = 1;
    } else {
        wsnprintf(status, sizeof(status),
                  "E492: Not an editor command: %s", c);
        return;
    }

    if (write) {
        /* ":w name" writes somewhere else and adopts that name, as vim does. */
        const char *target = filename;
        char       *space = strchr(c, ' ');

        if (space) {
            while (*space == ' ')
                space++;
            if (*space) {
                target = space;
                strlcpy(filename, space, sizeof(filename));
            }
        }

        if (!target[0]) {
            strlcpy(status, "E32: No file name", sizeof(status));
            return;
        }

        int written = buffer_save(target);
        if (written < 0) {
            wsnprintf(status, sizeof(status), "E212: Can't open file for writing: %s",
                      wstrerror(-written));
            return;
        }

        modified = 0;
        wsnprintf(status, sizeof(status), "\"%s\" %d lines, %d bytes written",
                  target, line_count, written);
    }

    if (quit) {
        /* With a terminal open, :q closes that window first -- like closing a
         * split in real vim -- rather than leaving the editor.  It takes a
         * second :q to actually quit. */
        if (term.open && !write) {
            close_terminal();
            return;
        }

        if (modified && !force) {
            strlcpy(status,
                    "E37: No write since last change (add ! to override)",
                    sizeof(status));
            return;
        }
        running = 0;
    }
}

/* ---------------------------------------------------------------- *
 *  Key handling
 * ---------------------------------------------------------------- */

static void normal_key(int key)
{
    /* An operator waiting for its second key: only dd is supported. */
    if (pending == 'd') {
        pending = 0;
        if (key == 'd') {
            line_remove(cy);
            if (cy >= line_count)
                cy = line_count - 1;
            modified = 1;
        }
        return;
    }
    if (pending == 'g') {
        pending = 0;
        if (key == 'g') {
            cy = 0;
            cx = 0;
        }
        return;
    }

    switch (key) {
    case 'h': case W_KEY_LEFT:  cx--; break;
    case 'l': case W_KEY_RIGHT: cx++; break;
    case 'k': case W_KEY_UP:    cy--; break;
    case 'j': case W_KEY_DOWN:  cy++; break;

    case '0': case W_KEY_HOME: cx = 0; break;
    case '$': case W_KEY_END:  cx = current_length(); break;

    case '^': {
        const char *text = lines[cy];
        cx = 0;
        while (text[cx] == ' ' || text[cx] == '\t')
            cx++;
        break;
    }

    case 'G': cy = line_count - 1; cx = 0; break;
    case 'g': pending = 'g'; break;
    case 'd': pending = 'd'; break;

    case 'w': move_word_forward(); break;
    case 'b': move_word_back();    break;

    case 0x04: cy += text_rows / 2; break;    /* Ctrl+D */
    case 0x15: cy -= text_rows / 2; break;    /* Ctrl+U */
    case W_KEY_PGDN: cy += text_rows; break;
    case W_KEY_PGUP: cy -= text_rows; break;

    case 'i': mode = MODE_INSERT; break;
    case 'a': mode = MODE_INSERT; cx++; break;
    case 'I': mode = MODE_INSERT; cx = 0; break;
    case 'A': mode = MODE_INSERT; cx = current_length(); break;

    case 'o':
        if (line_insert(cy + 1, "") == 0) {
            cy++;
            cx = 0;
            mode = MODE_INSERT;
            modified = 1;
        }
        break;
    case 'O':
        if (line_insert(cy, "") == 0) {
            cx = 0;
            mode = MODE_INSERT;
            modified = 1;
        }
        break;

    case 'x':
    case W_KEY_DELETE:
        delete_char_at(cx);
        break;

    case ':':
        mode = MODE_COMMAND;
        command[0] = '\0';
        command_len = 0;
        status[0] = '\0';
        break;

    default:
        break;
    }
}

static void insert_key(int key)
{
    switch (key) {
    case W_KEY_ESCAPE:
        mode = MODE_NORMAL;
        /* vim steps left on leaving insert mode. */
        if (cx > 0)
            cx--;
        break;

    case '\n':
    case '\r':
        split_line();
        break;

    case '\b':
    case 0x7F:
        if (cx > 0) {
            cx--;
            delete_char_at(cx);
        } else {
            join_with_previous();
        }
        break;

    case W_KEY_DELETE: delete_char_at(cx); break;

    case W_KEY_LEFT:  cx--; break;
    case W_KEY_RIGHT: cx++; break;
    case W_KEY_UP:    cy--; break;
    case W_KEY_DOWN:  cy++; break;
    case W_KEY_HOME:  cx = 0; break;
    case W_KEY_END:   cx = current_length(); break;

    default:
        if (key == '\t') {
            /* A literal tab would need the console to expand it consistently
             * on both outputs; spaces avoid the question entirely. */
            for (int i = 0; i < 4; i++)
                insert_char(' ');
        } else if (key >= 32 && key < 127) {
            insert_char((char)key);
        }
        break;
    }
}

static void command_key(int key)
{
    switch (key) {
    case W_KEY_ESCAPE:
        mode = MODE_NORMAL;
        command[0] = '\0';
        command_len = 0;
        break;

    case '\n':
    case '\r':
        mode = MODE_NORMAL;
        run_command();
        command[0] = '\0';
        command_len = 0;
        break;

    case '\b':
    case 0x7F:
        if (command_len > 0) {
            command[--command_len] = '\0';
        } else {
            /* Backspacing off the start abandons the command, as vim does. */
            mode = MODE_NORMAL;
        }
        break;

    default:
        if (key >= 32 && key < 127 &&
            command_len < (int)sizeof(command) - 1) {
            command[command_len++] = (char)key;
            command[command_len] = '\0';
        }
        break;
    }
}

/* Route a key to the editor according to the current mode. */
static void dispatch_editor_key(int key)
{
    switch (mode) {
    case MODE_NORMAL:  normal_key(key);  break;
    case MODE_INSERT:  insert_key(key);  break;
    case MODE_COMMAND: command_key(key); break;
    }
}

int main(int argc, char **argv)
{
    int editor_dirty = 1;      /* redraw the split's editor chrome next pass */

    /* Learn the console size before drawing anything, so the layout matches
     * whatever text mode is in force. */
    layout_init();
    view_w = con_w;
    view_h = text_rows;

    buffer_new();

    if (argc > 1) {
        int r = buffer_load(argv[1]);
        if (r == -W_ENOENT)
            wsnprintf(status, sizeof(status), "\"%s\" [New]", argv[1]);
        else if (r < 0)
            wsnprintf(status, sizeof(status), "\"%s\" %s",
                      argv[1], wstrerror(-r));
        else
            wsnprintf(status, sizeof(status), "\"%s\" %d lines",
                      argv[1], line_count);
    } else {
        strlcpy(status, "vim for WOS -- :q to quit, :w to save",
                sizeof(status));
    }

    wconsole_raw(W_CONSOLE_RAW);
    wcls();

    while (running) {
        clamp_cursor();
        scroll_to_cursor();

        /* --- The simple case: no terminal, block for a key. --- */
        if (!term.open) {
            draw_single();

            int key = wgetkey();
            if (key < 0)
                break;

            if (mode != MODE_COMMAND)
                status[0] = '\0';
            dispatch_editor_key(key);
            continue;
        }

        /* --- Split with a live terminal: never block, so both windows keep
         *     moving.  Redraw the editor chrome only when something changed,
         *     let the terminal repaint its own changed cells, and poll for a
         *     key rather than waiting on one. --- */
        if (editor_dirty) {
            draw_split_static();
            editor_dirty = 0;
        }

        if (!wterm_pump(&term)) {      /* child exited: close the window */
            close_terminal();
            editor_dirty = 1;
            continue;
        }
        wterm_render(&term);
        place_cursor();

        if (!wpollin(W_STDIN)) {
            wyield();                 /* nothing to do; give the child a turn */
            continue;
        }

        int key = wgetkey();
        if (key < 0)
            break;

        /* Ctrl+W is the window-command prefix: Ctrl+W Ctrl+W (or w/h/l)
         * switches which window the keyboard talks to. */
        if (ctrl_w_pending) {
            ctrl_w_pending = 0;
            if (key == 0x17 || key == 'w' || key == 'h' || key == 'l')
                focus = (focus == FOCUS_EDITOR) ? FOCUS_TERM : FOCUS_EDITOR;
            editor_dirty = 1;         /* status highlight moved */
            continue;
        }
        if (key == 0x17) {            /* Ctrl+W */
            ctrl_w_pending = 1;
            continue;
        }

        if (focus == FOCUS_TERM) {
            wterm_input(&term, key);
            continue;
        }

        if (mode != MODE_COMMAND)
            status[0] = '\0';
        dispatch_editor_key(key);
        editor_dirty = 1;             /* a key may have changed the editor */
    }

    if (term.open)
        wterm_close(&term);

    wconsole_raw(W_CONSOLE_CANONICAL);
    wcls();
    buffer_free();

    return 0;
}
