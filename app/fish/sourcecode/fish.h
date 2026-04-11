/* fish -- a friendly interactive shell for WOS.
 *
 * The point of fish is what happens while you are typing: the command is
 * coloured as you go, the rest of a previous command is offered ahead of the
 * cursor, and Tab completes.  That is what this implements.
 */
#ifndef WOS_FISH_H
#define WOS_FISH_H

#include <wkernel.h>

#define FISH_LINE_MAX  512
#define FISH_MAX_ARGS  32
#define FISH_HISTORY   64

/* Read a line with highlighting, autosuggestion, history and completion.
 * Returns its length, or -1 if input failed. */
int fish_read_line(char *buf, int size);

/* The command history, most recent last. */
extern char *history[FISH_HISTORY];
extern int   history_count;

void fish_history_add(const char *line);

/* True if `name` names something that can actually be run: a fish builtin, a
 * whell builtin, or an application under /app. */
int fish_command_exists(const char *name);

/* Split a line into arguments in place. */
int fish_parse(char *line, char **argv, int max);

#endif /* WOS_FISH_H */
