/* The text buffer: an array of lines, each its own allocation. */

#include "vim.h"

char *lines[MAX_LINES];
int   line_count;
char  filename[W_PATH_MAX + 1];
int   modified;

/* Replace line `at` with a copy of `text`.  Returns 0, or -1 if out of
 * memory, in which case the old line is left alone. */
int line_set(int at, const char *text)
{
    wsize_t len = strlen(text);
    char   *copy = malloc(len + 1);

    if (!copy)
        return -1;

    memcpy(copy, text, len + 1);
    free(lines[at]);
    lines[at] = copy;
    return 0;
}

int line_insert(int at, const char *text)
{
    if (line_count >= MAX_LINES)
        return -1;

    for (int i = line_count; i > at; i--)
        lines[i] = lines[i - 1];

    lines[at] = NULL;
    line_count++;

    if (line_set(at, text) < 0) {
        /* Undo the shift so the buffer is not left with a hole. */
        for (int i = at; i < line_count - 1; i++)
            lines[i] = lines[i + 1];
        line_count--;
        return -1;
    }

    return 0;
}

void line_remove(int at)
{
    if (at < 0 || at >= line_count)
        return;

    free(lines[at]);
    for (int i = at; i < line_count - 1; i++)
        lines[i] = lines[i + 1];

    line_count--;
    lines[line_count] = NULL;

    /* A buffer always has at least one line, even if it is empty. */
    if (line_count == 0)
        line_insert(0, "");
}

void buffer_free(void)
{
    for (int i = 0; i < line_count; i++) {
        free(lines[i]);
        lines[i] = NULL;
    }
    line_count = 0;
}

void buffer_new(void)
{
    buffer_free();
    line_insert(0, "");
    modified = 0;
}

/* Read a file into the buffer.  A file that does not exist is not an error:
 * vim opens a new buffer under that name, and so does this. */
int buffer_load(const char *path)
{
    buffer_free();
    strlcpy(filename, path, sizeof(filename));

    int fd = wopen(path, W_O_RDONLY);
    if (fd < 0) {
        line_insert(0, "");
        modified = 0;
        return fd;              /* the caller reports it in the status line */
    }

    wstat_t st;
    unsigned size = 0;
    if (wstat(path, &st) == 0)
        size = st.size;

    if (size == 0) {
        wclose(fd);
        line_insert(0, "");
        modified = 0;
        return 0;
    }

    char *data = malloc(size + 1);
    if (!data) {
        wclose(fd);
        line_insert(0, "");
        return -W_ENOMEM;
    }

    int got = wread(fd, data, size);
    wclose(fd);

    if (got < 0) {
        free(data);
        line_insert(0, "");
        return got;
    }
    data[got] = '\0';

    /* Split on newlines. A trailing newline ends the last line rather than
     * starting an empty one, which is what every editor assumes. */
    int start = 0;
    for (int i = 0; i <= got; i++) {
        if (i == got || data[i] == '\n') {
            if (i == got && i == start)
                break;

            char saved = data[i];
            data[i] = '\0';
            if (line_insert(line_count, data + start) < 0) {
                data[i] = saved;
                break;
            }
            data[i] = saved;
            start = i + 1;
        }
    }

    free(data);

    if (line_count == 0)
        line_insert(0, "");

    modified = 0;
    return 0;
}

/* Write the buffer out, one '\n' after every line. */
int buffer_save(const char *path)
{
    int fd = wopen(path, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
    if (fd < 0)
        return fd;

    int written = 0;

    for (int i = 0; i < line_count; i++) {
        wsize_t len = strlen(lines[i]);

        if (len > 0) {
            int n = wwrite(fd, lines[i], len);
            if (n < 0) {
                wclose(fd);
                return n;
            }
            written += n;
        }

        int n = wwrite(fd, "\n", 1);
        if (n < 0) {
            wclose(fd);
            return n;
        }
        written += n;
    }

    wclose(fd);
    return written;
}
