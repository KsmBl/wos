/* vim -- a modal text editor for WOS.
 *
 * Shared declarations. The buffer is an array of lines, each its own
 * allocation, which keeps line insertion and deletion to a pointer shuffle.
 */
#ifndef WOS_VIM_H
#define WOS_VIM_H

#include <wkernel.h>

#define MAX_LINES 4096

/* The bottom row is the status and command line, so the text occupies
 * everything above it. */
#define TEXT_ROWS (W_CONSOLE_HEIGHT - 1)

enum mode {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_COMMAND
};

/* The buffer. */
extern char *lines[MAX_LINES];
extern int   line_count;
extern char  filename[W_PATH_MAX + 1];
extern int   modified;

int  line_set(int at, const char *text);
int  line_insert(int at, const char *text);
void line_remove(int at);
void buffer_new(void);
void buffer_free(void);
int  buffer_load(const char *path);
int  buffer_save(const char *path);

#endif /* WOS_VIM_H */
