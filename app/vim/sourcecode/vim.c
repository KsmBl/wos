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
    if (cy >= row_offset + TEXT_ROWS)
        row_offset = cy - TEXT_ROWS + 1;

    if (cx < col_offset)
        col_offset = cx;
    if (cx >= col_offset + W_CONSOLE_WIDTH)
        col_offset = cx - W_CONSOLE_WIDTH + 1;

    if (row_offset < 0)
        row_offset = 0;
    if (col_offset < 0)
        col_offset = 0;
}

static void draw_status(void)
{
    wgotoxy(W_CONSOLE_HEIGHT, 1);

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

    int pad = W_CONSOLE_WIDTH - (int)strlen(left) - (int)strlen(right);

    wprintf("%s", left);
    for (int i = 0; i < pad; i++)
        wprintf(" ");
    wprintf("%s", right);
    wcolor_reset();

    /* A message, when there is one, replaces the bar on the next redraw. */
    if (status[0]) {
        wgotoxy(W_CONSOLE_HEIGHT, 1);
        wcolor_reset();
        wprintf("%s", status);
        wclear_line();
    }
}

static void draw(void)
{
    for (int row = 0; row < TEXT_ROWS; row++) {
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
                char shown[W_CONSOLE_WIDTH + 1];
                int  n = len - col_offset;

                if (n > W_CONSOLE_WIDTH)
                    n = W_CONSOLE_WIDTH;

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
        wgotoxy(W_CONSOLE_HEIGHT, command_len + 2);
    else
        wgotoxy(cy - row_offset + 1, cx - col_offset + 1);
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

    case 0x04: cy += TEXT_ROWS / 2; break;    /* Ctrl+D */
    case 0x15: cy -= TEXT_ROWS / 2; break;    /* Ctrl+U */
    case W_KEY_PGDN: cy += TEXT_ROWS; break;
    case W_KEY_PGUP: cy -= TEXT_ROWS; break;

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

int main(int argc, char **argv)
{
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
        draw();

        int key = wgetkey();
        if (key < 0)
            break;

        /* Any keystroke clears the last message, so it does not linger over
         * the status bar forever. */
        if (mode != MODE_COMMAND)
            status[0] = '\0';

        switch (mode) {
        case MODE_NORMAL:  normal_key(key);  break;
        case MODE_INSERT:  insert_key(key);  break;
        case MODE_COMMAND: command_key(key); break;
        }
    }

    wconsole_raw(W_CONSOLE_CANONICAL);
    wcls();
    buffer_free();

    return 0;
}
