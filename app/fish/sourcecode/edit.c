/* The fish line editor: highlighting, autosuggestion, history and completion.
 *
 * Everything here happens between keystrokes, so the whole line is repainted
 * after every key.  At 80 columns that is cheap, and it keeps the drawing
 * code to one function instead of a set of incremental updates that have to
 * agree with each other.
 */

#include "fish.h"

#define MAX_MATCHES 64

char *history[FISH_HISTORY];
int   history_count;

static char match_names[MAX_MATCHES][W_NAME_MAX + 1];
static char match_is_dir[MAX_MATCHES];
static int  match_count;

/* The builtins fish handles itself.  Everything else a user would call a
 * command is a program under /app, which completion and highlighting find by
 * scanning that directory rather than from a list kept here. */
static const char *fish_builtins[] = {
    "cd", "exit", "history", "help",
    NULL
};

void fish_history_add(const char *line)
{
    if (!line[0])
        return;

    /* Repeating the last command adds nothing to the history. */
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
        return;

    wsize_t len = strlen(line);
    char   *copy = malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, line, len + 1);

    if (history_count == FISH_HISTORY) {
        free(history[0]);
        for (int i = 0; i < FISH_HISTORY - 1; i++)
            history[i] = history[i + 1];
        history_count--;
    }

    history[history_count++] = copy;
}

int fish_command_exists(const char *name)
{
    if (!name[0])
        return 0;

    for (int i = 0; fish_builtins[i]; i++)
        if (strcmp(name, fish_builtins[i]) == 0)
            return 1;

    wstat_t st;

    /* A name with a slash is a path; anything else lives under /app. */
    if (strchr(name, '/'))
        return wstat(name, &st) == 0 && st.type == W_FT_FILE;

    char path[W_PATH_MAX + 1];
    wsnprintf(path, sizeof(path), "/app/%s/launch", name);

    return wstat(path, &st) == 0 && st.type == W_FT_FILE;
}

/* ---------------------------------------------------------------- *
 *  Completion
 * ---------------------------------------------------------------- */

static void add_match(const char *name, int is_dir)
{
    if (match_count >= MAX_MATCHES)
        return;

    /* Completion can reach the same name twice: a directory under /app whose
     * name also happens to be a builtin. */
    for (int i = 0; i < match_count; i++)
        if (strcmp(match_names[i], name) == 0)
            return;

    strlcpy(match_names[match_count], name, W_NAME_MAX + 1);
    match_is_dir[match_count] = (char)is_dir;
    match_count++;
}

static void match_commands(const char *prefix, int prefix_len)
{
    for (int i = 0; fish_builtins[i]; i++)
        if (prefix_len == 0 ||
            strncmp(fish_builtins[i], prefix, (wsize_t)prefix_len) == 0)
            add_match(fish_builtins[i], 0);

    int d = wopendir("/app");
    if (d < 0)
        return;

    wdirent_t e;
    while (wreaddir(d, &e) == 1) {
        if (e.type != W_FT_DIR || e.name[0] == '.')
            continue;
        if (prefix_len > 0 &&
            strncmp(e.name, prefix, (wsize_t)prefix_len) != 0)
            continue;

        char launch[W_PATH_MAX + 1];
        wsnprintf(launch, sizeof(launch), "/app/%s/launch", e.name);

        wstat_t st;
        if (wstat(launch, &st) == 0 && st.type == W_FT_FILE)
            add_match(e.name, 0);
    }
    wclosedir(d);
}

static void match_files(const char *word, const char **prefix_out)
{
    char dir[W_PATH_MAX + 1];
    const char *slash = strrchr(word, '/');
    const char *prefix;

    if (!slash) {
        strlcpy(dir, ".", sizeof(dir));
        prefix = word;
    } else {
        int dir_len = (int)(slash - word);
        if (dir_len == 0) {
            strlcpy(dir, "/", sizeof(dir));
        } else {
            if (dir_len > W_PATH_MAX)
                dir_len = W_PATH_MAX;
            memcpy(dir, word, (wsize_t)dir_len);
            dir[dir_len] = '\0';
        }
        prefix = slash + 1;
    }

    *prefix_out = prefix;
    int prefix_len = (int)strlen(prefix);

    int d = wopendir(dir);
    if (d < 0)
        return;

    wdirent_t e;
    while (wreaddir(d, &e) == 1) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;
        if (e.name[0] == '.' && (prefix_len == 0 || prefix[0] != '.'))
            continue;
        if (prefix_len > 0 &&
            strncmp(e.name, prefix, (wsize_t)prefix_len) != 0)
            continue;

        add_match(e.name, e.type == W_FT_DIR);
    }
    wclosedir(d);
}

static int common_prefix_length(void)
{
    if (match_count == 0)
        return 0;

    int common = (int)strlen(match_names[0]);

    for (int i = 1; i < match_count; i++) {
        int j = 0;
        while (j < common && match_names[i][j] == match_names[0][j])
            j++;
        common = j;
    }
    return common;
}

/* Work out what Tab should append. Returns the number of matches. */
static int complete(const char *buf, int len, char *add, int add_size)
{
    add[0] = '\0';
    match_count = 0;

    int start = len;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t')
        start--;

    const char *word = buf + start;

    int is_command = 1;
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ' && buf[i] != '\t') {
            is_command = 0;
            break;
        }

    const char *prefix;

    if (is_command && !strchr(word, '/')) {
        prefix = word;
        match_commands(prefix, (int)strlen(prefix));
    } else {
        match_files(word, &prefix);
    }

    if (match_count == 0)
        return 0;

    int prefix_len = (int)strlen(prefix);
    int take = (match_count == 1) ? (int)strlen(match_names[0])
                                  : common_prefix_length();

    int out = 0;
    for (int i = prefix_len; i < take && out + 1 < add_size; i++)
        add[out++] = match_names[0][i];

    if (match_count == 1 && out + 1 < add_size)
        add[out++] = match_is_dir[0] ? '/' : ' ';

    add[out] = '\0';
    return match_count;
}

/* ---------------------------------------------------------------- *
 *  Drawing
 * ---------------------------------------------------------------- */

static int prompt_width;

static void draw_prompt(void)
{
    char cwd[W_PATH_MAX + 1];

    if (wgetcwd(cwd, sizeof(cwd)) < 0)
        strlcpy(cwd, "?", sizeof(cwd));

    wuser_t me;
    const char *who = (wuserinfo(-1, &me) == 0) ? me.name : "?";

    wcolor(W_CYAN | W_BRIGHT, W_DEFAULT);
    wprintf("%s", who);
    wcolor(W_WHITE, W_DEFAULT);
    wprintf(" ");
    wcolor(W_GREEN | W_BRIGHT, W_DEFAULT);
    wprintf("%s", cwd);
    wcolor(W_WHITE, W_DEFAULT);
    wprintf("> ");
    wcolor_reset();

    prompt_width = (int)strlen(who) + 1 + (int)strlen(cwd) + 2;
}

/* The first word decides the colour: green if it can actually be run, red if
 * it cannot.  That is fish's most useful trick -- a typo is visible before
 * Enter is pressed. */
static void draw_highlighted(const char *buf, int len)
{
    int end = 0;
    while (end < len && buf[end] != ' ' && buf[end] != '\t')
        end++;

    char command[W_PATH_MAX + 1];
    int  copy = (end < W_PATH_MAX) ? end : W_PATH_MAX;
    memcpy(command, buf, (wsize_t)copy);
    command[copy] = '\0';

    if (end > 0)
        wcolor(fish_command_exists(command) ? (W_GREEN | W_BRIGHT)
                                            : (W_RED | W_BRIGHT), W_DEFAULT);

    for (int i = 0; i < len; i++) {
        if (i == end)
            wcolor_reset();
        wprintf("%c", buf[i]);
    }
    wcolor_reset();
}

/* The most recent history entry that starts with what has been typed. */
static const char *find_suggestion(const char *buf, int len)
{
    if (len == 0)
        return NULL;

    for (int i = history_count - 1; i >= 0; i--)
        if (strncmp(history[i], buf, (wsize_t)len) == 0 &&
            (int)strlen(history[i]) > len)
            return history[i];

    return NULL;
}

/* Move the cursor to a column on the current row, without needing to know
 * which row that is: return to column 1 and step right. */
static void move_to_column(int column)
{
    wprintf("\r");
    if (column > 0)
        wprintf("\033[%dC", column);
}

static void redraw(const char *buf, int len, int cursor, const char *suggestion)
{
    move_to_column(0);
    wclear_line();

    draw_prompt();
    draw_highlighted(buf, len);

    if (suggestion) {
        /* Bright black reads as grey, which is how fish shows the part you
         * have not typed yet. */
        wcolor(W_BLACK | W_BRIGHT, W_DEFAULT);
        wprintf("%s", suggestion + len);
        wcolor_reset();
    }

    move_to_column(prompt_width + cursor);
}

/* ---------------------------------------------------------------- *
 *  The editor
 * ---------------------------------------------------------------- */

int fish_read_line(char *buf, int size)
{
    int len = 0;
    int cursor = 0;
    int history_at = history_count;      /* one past the end: the live line */
    char saved[FISH_LINE_MAX];

    buf[0] = '\0';
    saved[0] = '\0';

    wconsole_raw(W_CONSOLE_RAW);

    for (;;) {
        const char *suggestion = find_suggestion(buf, len);
        redraw(buf, len, cursor, suggestion);

        int key = wgetkey();
        if (key < 0) {
            wconsole_raw(W_CONSOLE_CANONICAL);
            return -1;
        }

        if (key == '\n' || key == '\r') {
            wprintf("\n");
            break;
        }

        if (key == 0x03) {                       /* Ctrl+C */
            wprintf("\n");
            len = 0;
            cursor = 0;
            buf[0] = '\0';
            break;
        }

        if (key == 0x04 && len == 0) {           /* Ctrl+D on an empty line */
            wconsole_raw(W_CONSOLE_CANONICAL);
            return -2;
        }

        switch (key) {
        case '\b':
        case 0x7F:
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor,
                        (wsize_t)(len - cursor + 1));
                cursor--;
                len--;
            }
            continue;

        case W_KEY_DELETE:
            if (cursor < len) {
                memmove(buf + cursor, buf + cursor + 1,
                        (wsize_t)(len - cursor));
                len--;
            }
            continue;

        case W_KEY_LEFT:  if (cursor > 0)   cursor--; continue;
        case W_KEY_HOME:  cursor = 0;                 continue;
        case 0x01:        cursor = 0;                 continue;   /* Ctrl+A */

        case W_KEY_RIGHT:
        case W_KEY_END:
        case 0x05:                                                 /* Ctrl+E */
        case 0x06:                                                 /* Ctrl+F */
            /* At the end of the line these accept the suggestion, which is
             * how fish turns a previous command into the current one. */
            if (cursor == len && suggestion) {
                strlcpy(buf, suggestion, (wsize_t)size);
                len = (int)strlen(buf);
                cursor = len;
            } else if (key == W_KEY_RIGHT || key == 0x06) {
                if (cursor < len)
                    cursor++;
            } else {
                cursor = len;
            }
            continue;

        case 0x15:                                                 /* Ctrl+U */
            len = 0;
            cursor = 0;
            buf[0] = '\0';
            continue;

        case 0x0B:                                                 /* Ctrl+K */
            buf[cursor] = '\0';
            len = cursor;
            continue;

        case W_KEY_UP:
        case W_KEY_DOWN: {
            if (key == W_KEY_UP) {
                if (history_at == 0)
                    continue;
                if (history_at == history_count)
                    strlcpy(saved, buf, sizeof(saved));
                history_at--;
            } else {
                if (history_at >= history_count)
                    continue;
                history_at++;
            }

            if (history_at == history_count)
                strlcpy(buf, saved, (wsize_t)size);
            else
                strlcpy(buf, history[history_at], (wsize_t)size);

            len = (int)strlen(buf);
            cursor = len;
            continue;
        }

        case '\t': {
            char add[W_PATH_MAX + 2];
            int  matches = complete(buf, cursor, add, sizeof(add));

            if (add[0]) {
                /* Insert at the cursor rather than appending, so completing
                 * mid-line does not scramble the rest of it. */
                int add_len = (int)strlen(add);
                if (len + add_len < size) {
                    memmove(buf + cursor + add_len, buf + cursor,
                            (wsize_t)(len - cursor + 1));
                    memcpy(buf + cursor, add, (wsize_t)add_len);
                    len += add_len;
                    cursor += add_len;
                }
            } else if (matches > 1) {
                wprintf("\n");
                int trows = 0, tcols = W_CONSOLE_WIDTH;
                wconsize(&trows, &tcols);
                int column = 0;
                for (int i = 0; i < matches; i++) {
                    int width = (int)strlen(match_names[i]) +
                                (match_is_dir[i] ? 1 : 0);
                    if (column + width + 2 > tcols) {
                        wprintf("\n");
                        column = 0;
                    }
                    wprintf("%s%s  ", match_names[i],
                            match_is_dir[i] ? "/" : "");
                    column += width + 2;
                }
                wprintf("\n");
            }
            continue;
        }

        default:
            break;
        }

        if (key >= 32 && key < 127 && len + 1 < size) {
            memmove(buf + cursor + 1, buf + cursor,
                    (wsize_t)(len - cursor + 1));
            buf[cursor] = (char)key;
            cursor++;
            len++;
        }
    }

    wconsole_raw(W_CONSOLE_CANONICAL);
    buf[len] = '\0';
    return len;
}
