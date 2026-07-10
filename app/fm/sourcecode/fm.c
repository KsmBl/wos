/* fm -- a file manager for the console.
 *
 *     fm            start in the working directory
 *     fm /app       start somewhere else
 *
 * One pane, arrow keys, and a line at the bottom saying what the keys do.  It
 * is the shape `mc` and `ranger` have, without the two panes and without the
 * configuration: this is the small version, meant to be obvious rather than
 * powerful.
 *
 * Two things it does differently from the tools it resembles, both because of
 * what WOS has:
 *
 * There is no rename.  The filesystem has no call for it, so `m` copies the
 * file to its new name and deletes the old one.  For a file that is being
 * renamed within a directory the result is the same; for a large file it is
 * slower than it looks, and if it fails halfway the original is still there,
 * which is the safer of the two ways to fail.
 *
 * And it never asks the shell to do anything.  Running a program, deleting a
 * file and making a directory all go straight to the kernel, so `fm` works the
 * same under `whell`, under `fish`, and in a window with no shell at all.
 */

#include <wkernel.h>

#define MAX_ENTRIES 512
#define VIEW_BYTES  (64 * 1024)

struct entry {
    char     name[W_NAME_MAX + 2];   /* room for a trailing '/' */
    uint32_t size;
    int      is_dir;
    int      is_parent;              /* the ".." at the top */
};

static struct entry entries[MAX_ENTRIES];
static int          count;
static int          selected;
static int          top;             /* first entry shown */
static int          truncated;       /* the directory had more than we hold */

static char path[W_PATH_MAX + 1] = "/";
static char message[128];
static int  rows, cols;

/* Where the listing starts and how tall it is: a header, the list, a message
 * line and a key line. */
#define LIST_TOP    2
#define LIST_HEIGHT (rows - 3)

/* ------------------------------------------------------------------ *
 *  Paths
 * ------------------------------------------------------------------ */

/* dir + "/" + name, without the double slash at the root. */
static void join(char *out, wsize_t size, const char *dir, const char *name)
{
    if (dir[0] == '/' && dir[1] == '\0')
        wsnprintf(out, size, "/%s", name);
    else
        wsnprintf(out, size, "%s/%s", dir, name);
}

static void parent_of(char *dir)
{
    char *slash = strrchr(dir, '/');

    if (!slash || slash == dir) {
        dir[0] = '/';
        dir[1] = '\0';
        return;
    }
    *slash = '\0';
}

/* ------------------------------------------------------------------ *
 *  Reading a directory
 * ------------------------------------------------------------------ */

static int compare(const struct entry *a, const struct entry *b)
{
    /* Directories first, then by name.  ".." is always first: it is where
     * "up" is, and hunting for it in alphabetical order is tiresome. */
    if (a->is_parent != b->is_parent)
        return a->is_parent ? -1 : 1;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void sort_entries(void)
{
    /* Insertion sort: a directory here holds a few dozen names, and this is
     * shorter than anything cleverer and stable into the bargain. */
    for (int i = 1; i < count; i++) {
        struct entry key = entries[i];
        int          j   = i - 1;

        while (j >= 0 && compare(&entries[j], &key) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void load(void)
{
    count     = 0;
    truncated = 0;

    int dir = wopendir(path);
    if (dir < 0) {
        wsnprintf(message, sizeof(message), "%s: %s", path, wstrerror(-dir));
        return;
    }

    if (strcmp(path, "/") != 0) {
        strlcpy(entries[count].name, "..", sizeof(entries[count].name));
        entries[count].is_dir    = 1;
        entries[count].is_parent = 1;
        entries[count].size      = 0;
        count++;
    }

    wdirent_t e;
    while (wreaddir(dir, &e) == 1) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;

        if (count >= MAX_ENTRIES) {
            truncated++;
            continue;
        }

        strlcpy(entries[count].name, e.name, sizeof(entries[count].name));
        entries[count].is_dir    = (e.type == W_FT_DIR);
        entries[count].is_parent = 0;
        entries[count].size      = 0;

        if (!entries[count].is_dir) {
            char    full[W_PATH_MAX + 1];
            wstat_t st;

            join(full, sizeof(full), path, e.name);
            if (wstat(full, &st) == 0)
                entries[count].size = st.size;
        }

        count++;
    }

    wclosedir(dir);
    sort_entries();

    if (selected >= count)
        selected = count ? count - 1 : 0;
    top = 0;
}

/* ------------------------------------------------------------------ *
 *  Drawing
 * ------------------------------------------------------------------ */

static void draw_header(void)
{
    char left[W_PATH_MAX + 32];

    wgotoxy(1, 1);
    wcolor(W_BLACK, W_CYAN);

    wsnprintf(left, sizeof(left), " %s", path);
    wprintf("%-*.*s", cols, cols, left);

    /* The count, written over the right-hand end of the same bar. */
    char right[32];
    wsnprintf(right, sizeof(right), "%d item%s ", count, count == 1 ? "" : "s");

    int at = cols - (int)strlen(right) + 1;
    if (at > 1) {
        wgotoxy(1, at);
        wprintf("%s", right);
    }

    wcolor_reset();
}

static void draw_list(void)
{
    int height = LIST_HEIGHT;

    /* Keep the selection on screen. */
    if (selected < top)
        top = selected;
    if (selected >= top + height)
        top = selected - height + 1;
    if (top < 0)
        top = 0;

    for (int i = 0; i < height; i++) {
        int index = top + i;

        wgotoxy(LIST_TOP + i, 1);
        wclear_line();

        if (index >= count)
            continue;

        struct entry *e     = &entries[index];
        int           here  = (index == selected);

        if (here)
            wcolor(W_BLACK, W_WHITE);
        else if (e->is_dir)
            wcolor(W_CYAN + W_BRIGHT, W_DEFAULT);

        /* name on the left, size on the right, and the name truncated rather
         * than allowed to push the size off the end. */
        char size_text[16];
        if (e->is_dir)
            strlcpy(size_text, "<dir>", sizeof(size_text));
        else
            strlcpy(size_text, whuman(e->size), sizeof(size_text));

        int name_width = cols - (int)strlen(size_text) - 4;
        if (name_width < 8)
            name_width = 8;

        wprintf(" %-*.*s %s", name_width, name_width, e->name, size_text);

        if (here || e->is_dir)
            wcolor_reset();
    }
}

static void draw_footer(void)
{
    wgotoxy(rows - 1, 1);
    wclear_line();
    if (message[0]) {
        wcolor(W_YELLOW + W_BRIGHT, W_DEFAULT);
        wprintf(" %.*s", cols - 2, message);
        wcolor_reset();
    }

    wgotoxy(rows, 1);
    wcolor(W_BLACK, W_BLUE);
    wprintf("%-*.*s", cols, cols,
            " enter open  v view  x run  n newdir  t touch  c copy  m move  "
            "D delete  q quit");
    wcolor_reset();
}

static void draw(void)
{
    wcursor(0);
    draw_header();
    draw_list();
    draw_footer();
    wgotoxy(rows, cols);
}

/* ------------------------------------------------------------------ *
 *  Asking things
 * ------------------------------------------------------------------ */

/* Read a line on the message row.  Returns 0 if Escape was pressed. */
static int ask(const char *prompt, char *out, wsize_t size, const char *initial)
{
    wsize_t len = 0;

    out[0] = '\0';
    if (initial)
        len = strlcpy(out, initial, size);
    if (len >= size)
        len = size - 1;

    for (;;) {
        wgotoxy(rows - 1, 1);
        wclear_line();
        wcolor(W_WHITE + W_BRIGHT, W_DEFAULT);
        wprintf(" %s%s", prompt, out);
        wcolor_reset();
        wcursor(1);

        int key = wgetkey();

        if (key == W_KEY_ESCAPE) {
            wcursor(0);
            return 0;
        }
        if (key == '\n' || key == '\r') {
            wcursor(0);
            return out[0] != '\0';
        }
        if (key == '\b' || key == 127) {
            if (len > 0)
                out[--len] = '\0';
            continue;
        }
        if (key >= ' ' && key < 0x7F && len + 1 < size) {
            out[len++] = (char)key;
            out[len]   = '\0';
        }
    }
}

static int confirm(const char *question)
{
    wgotoxy(rows - 1, 1);
    wclear_line();
    wcolor(W_RED + W_BRIGHT, W_DEFAULT);
    wprintf(" %s [y/N] ", question);
    wcolor_reset();

    int key = wgetkey();
    return key == 'y' || key == 'Y';
}

/* ------------------------------------------------------------------ *
 *  Doing things
 * ------------------------------------------------------------------ */

static int copy_file(const char *from, const char *to)
{
    int in = wopen(from, W_O_RDONLY);
    if (in < 0)
        return in;

    int out = wopen(to, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
    if (out < 0) {
        wclose(in);
        return out;
    }

    static char buf[4096];
    int         result = 0;

    for (;;) {
        int n = wread(in, buf, sizeof(buf));
        if (n < 0)  { result = n; break; }
        if (n == 0) break;

        int written = wwrite(out, buf, (wsize_t)n);
        if (written < 0) { result = written; break; }
        if (written < n) { result = -W_ENOSPC; break; }
    }

    wclose(in);
    wclose(out);

    /* A half-written copy is worse than none: it looks like a file. */
    if (result < 0)
        wunlink(to);

    return result;
}

/* Show a file, a screenful at a time. */
static void view(const char *file)
{
    static char text[VIEW_BYTES];

    int fd = wopen(file, W_O_RDONLY);
    if (fd < 0) {
        wsnprintf(message, sizeof(message), "%s: %s", file, wstrerror(-fd));
        return;
    }

    int n = wread(fd, text, sizeof(text));
    wclose(fd);

    if (n < 0) {
        wsnprintf(message, sizeof(message), "%s: %s", file, wstrerror(-n));
        return;
    }

    /* Split into lines, replacing anything unprintable with a dot so a binary
     * file is shown rather than being allowed to reprogram the terminal. */
    for (int i = 0; i < n; i++)
        if ((unsigned char)text[i] < ' ' && text[i] != '\n' && text[i] != '\t')
            text[i] = '.';

    int line_start[4096];
    int lines = 0;

    line_start[lines++] = 0;
    for (int i = 0; i < n && lines < 4096; i++)
        if (text[i] == '\n')
            line_start[lines++] = i + 1;

    int at   = 0;
    int page = rows - 2;

    for (;;) {
        wcursor(0);
        wgotoxy(1, 1);
        wcolor(W_BLACK, W_CYAN);
        wprintf("%-*.*s", cols, cols, file);
        wcolor_reset();

        for (int i = 0; i < page; i++) {
            wgotoxy(2 + i, 1);
            wclear_line();

            int index = at + i;
            if (index >= lines)
                continue;

            int start = line_start[index];
            int end   = (index + 1 < lines) ? line_start[index + 1] - 1 : n;

            if (end > start)
                wprintf("%.*s", (end - start) > cols ? cols : (end - start),
                        text + start);
        }

        wgotoxy(rows, 1);
        wcolor(W_BLACK, W_BLUE);
        wprintf("%-*.*s", cols, cols,
                n >= (int)sizeof(text)
                ? "  up/down  page up/down   q back    (showing the first 64K)"
                : "  up/down  page up/down   q back");
        wcolor_reset();

        int key = wgetkey();

        if (key == 'q' || key == W_KEY_ESCAPE)
            break;
        if (key == W_KEY_DOWN && at + page < lines) at++;
        if (key == W_KEY_UP   && at > 0)            at--;
        if (key == W_KEY_PGDN) at += page;
        if (key == W_KEY_PGUP) at -= page;
        if (at > lines - 1) at = lines - 1;
        if (at < 0)         at = 0;
    }
}

/* Run the selected file as a program, with the console back to normal so it
 * behaves as it would from a shell. */
static void run(const char *file)
{
    char *argv[2];

    argv[0] = (char *)file;
    argv[1] = NULL;

    wconsole_raw(W_CONSOLE_CANONICAL);
    wcursor(1);
    wcls();

    int pid = wspawn(file, argv);
    if (pid < 0) {
        wprintf("%s: %s\n", file, wstrerror(-pid));
    } else {
        int status = 0;
        wwait(pid, &status);
        wprintf("\n[%s exited with %d -- press Enter]\n", file, status);
    }

    char line[8];
    wgetline(line, sizeof(line));

    wconsole_raw(W_CONSOLE_RAW);
    wcls();
}

/* ------------------------------------------------------------------ *
 *  Keys
 * ------------------------------------------------------------------ */

static void enter_selected(void)
{
    if (count == 0)
        return;

    struct entry *e = &entries[selected];

    if (e->is_parent) {
        parent_of(path);
        selected = 0;
        load();
        return;
    }

    if (e->is_dir) {
        char next[W_PATH_MAX + 1];

        join(next, sizeof(next), path, e->name);
        if (strlen(next) > W_PATH_MAX) {
            strlcpy(message, "that path is longer than the system allows",
                    sizeof(message));
            return;
        }

        strlcpy(path, next, sizeof(path));
        selected = 0;
        load();
        return;
    }

    /* A file: showing it is the safe thing to do with Enter.  Running it is
     * `x`, which is a key somebody has to mean to press. */
    char full[W_PATH_MAX + 1];
    join(full, sizeof(full), path, e->name);
    view(full);
}

static void delete_selected(void)
{
    if (count == 0 || entries[selected].is_parent)
        return;

    struct entry *e = &entries[selected];
    char          full[W_PATH_MAX + 1];
    char          question[160];

    join(full, sizeof(full), path, e->name);
    wsnprintf(question, sizeof(question), "delete %s%s?", e->name,
              e->is_dir ? " (must be empty)" : "");

    if (!confirm(question)) {
        strlcpy(message, "left alone", sizeof(message));
        return;
    }

    int r = e->is_dir ? wrmdir(full) : wunlink(full);

    if (r < 0)
        wsnprintf(message, sizeof(message), "%s: %s", e->name, wstrerror(-r));
    else
        wsnprintf(message, sizeof(message), "deleted %s", e->name);

    load();
}

static void copy_selected(int and_delete)
{
    if (count == 0 || entries[selected].is_parent)
        return;

    struct entry *e = &entries[selected];

    if (e->is_dir) {
        strlcpy(message, "this copies files, not directories",
                sizeof(message));
        return;
    }

    char to[W_PATH_MAX + 1];
    if (!ask(and_delete ? "move to: " : "copy to: ", to, sizeof(to), e->name))
        return;

    char from[W_PATH_MAX + 1];
    char dest[W_PATH_MAX + 1];

    join(from, sizeof(from), path, e->name);
    if (to[0] == '/')
        strlcpy(dest, to, sizeof(dest));
    else
        join(dest, sizeof(dest), path, to);

    if (strcmp(from, dest) == 0) {
        strlcpy(message, "that is where it already is", sizeof(message));
        return;
    }

    int r = copy_file(from, dest);
    if (r < 0) {
        wsnprintf(message, sizeof(message), "%s: %s", to, wstrerror(-r));
        return;
    }

    /* Only once the copy is safely there.  There is no rename call, so a move
     * is a copy and a delete, and doing the delete first would risk having
     * neither. */
    if (and_delete) {
        r = wunlink(from);
        if (r < 0) {
            wsnprintf(message, sizeof(message),
                      "copied, but %s could not be removed: %s", e->name,
                      wstrerror(-r));
            load();
            return;
        }
    }

    wsnprintf(message, sizeof(message), "%s %s to %s",
              and_delete ? "moved" : "copied", e->name, to);
    load();
}

static void make_directory(void)
{
    char name[W_NAME_MAX + 2];

    if (!ask("new directory: ", name, sizeof(name), NULL))
        return;

    char full[W_PATH_MAX + 1];
    join(full, sizeof(full), path, name);

    int r = wmkdir(full);
    if (r < 0)
        wsnprintf(message, sizeof(message), "%s: %s", name, wstrerror(-r));
    else
        wsnprintf(message, sizeof(message), "created %s/", name);

    load();
}

static void make_file(void)
{
    char name[W_NAME_MAX + 2];

    if (!ask("new file: ", name, sizeof(name), NULL))
        return;

    char full[W_PATH_MAX + 1];
    join(full, sizeof(full), path, name);

    int fd = wopen(full, W_O_WRONLY | W_O_CREAT);
    if (fd < 0) {
        wsnprintf(message, sizeof(message), "%s: %s", name, wstrerror(-fd));
    } else {
        wclose(fd);
        wsnprintf(message, sizeof(message), "created %s", name);
    }

    load();
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        strlcpy(path, argv[1], sizeof(path));

        /* A trailing slash is how people type a directory and not how the
         * filesystem stores one. */
        wsize_t len = strlen(path);
        while (len > 1 && path[len - 1] == '/')
            path[--len] = '\0';
    } else if (wgetcwd(path, sizeof(path)) < 0) {
        strlcpy(path, "/", sizeof(path));
    }

    wstat_t st;
    if (wstat(path, &st) < 0 || st.type != W_FT_DIR) {
        wfprintf(W_STDERR, "fm: %s is not a directory\n", path);
        return 1;
    }

    wconsize(&rows, &cols);
    if (rows < 6 || cols < 24) {
        wfprintf(W_STDERR, "fm: this terminal is too small\n");
        return 1;
    }

    wconsole_raw(W_CONSOLE_RAW);
    wcls();
    load();

    for (;;) {
        /* Re-read the size every time round: in a window the size changes
         * whenever the compositor says so, and a program that cached it would
         * draw over the edge. */
        int r, c;
        wconsize(&r, &c);
        if (r != rows || c != cols) {
            rows = r;
            cols = c;
            wcls();
        }

        draw();

        int key = wgetkey();

        message[0] = '\0';

        switch (key) {
        case W_KEY_UP:    if (selected > 0) selected--; break;
        case W_KEY_DOWN:  if (selected + 1 < count) selected++; break;
        case W_KEY_PGUP:  selected -= LIST_HEIGHT; break;
        case W_KEY_PGDN:  selected += LIST_HEIGHT; break;
        case W_KEY_HOME:  selected = 0; break;
        case W_KEY_END:   selected = count - 1; break;

        case W_KEY_RIGHT:
        case '\n':
        case '\r':
            enter_selected();
            break;

        case W_KEY_LEFT:
        case '\b':
        case 127:
            parent_of(path);
            selected = 0;
            load();
            break;

        case 'v': {
            if (count && !entries[selected].is_dir) {
                char full[W_PATH_MAX + 1];
                join(full, sizeof(full), path, entries[selected].name);
                view(full);
            }
            break;
        }

        case 'x': {
            if (count && !entries[selected].is_dir) {
                char full[W_PATH_MAX + 1];
                join(full, sizeof(full), path, entries[selected].name);
                run(full);
            }
            break;
        }

        case 'n': make_directory(); break;
        case 't': make_file();      break;
        case 'c': copy_selected(0); break;
        case 'm': copy_selected(1); break;
        case 'D': delete_selected(); break;
        case 'r': load(); strlcpy(message, "reread", sizeof(message)); break;

        case 'q':
        case W_KEY_ESCAPE:
            wconsole_raw(W_CONSOLE_CANONICAL);
            wcursor(1);
            wcls();
            return 0;

        default:
            break;
        }

        if (selected < 0)      selected = 0;
        if (selected >= count) selected = count ? count - 1 : 0;

        if (truncated && !message[0])
            wsnprintf(message, sizeof(message),
                      "%d more entries than this can show", truncated);
    }
}
