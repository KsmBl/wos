/* whell -- the WOS shell.
 *
 * Shared declarations between the main loop and the builtins.
 *
 * There are only three builtins, because there only need to be three: cd and
 * exit change state that belongs to the shell process itself, and help
 * describes the shell.  Everything a user would call a command -- ls, cat,
 * free and the rest -- is an ordinary program under /app.
 */
#ifndef WHELL_H
#define WHELL_H

#include <wkernel.h>

#define WHELL_MAX_ARGS 32
#define WHELL_LINE_MAX 512

/* Split `line` into arguments in place, honouring "double quotes" and
 * 'single quotes'.  Writes NULs into `line` and points argv at the pieces.
 * Returns the number of arguments, at most `max`. */
int whell_parse(char *line, char **argv, int max);

/* Print the prompt. Exposed so completion can redraw it after listing. */
void whell_print_prompt(void);

/* The name of builtin `index`, or NULL past the end.  Completion walks this
 * rather than keeping its own copy of the list. */
const char *whell_builtin_name(int index);

/* Complete the word at the end of `buf` (which is `len` characters long).
 *
 * Writes the text to append into `add` -- the rest of the name when exactly
 * one thing matches, otherwise the part every match agrees on, which may be
 * empty.  A single match is closed with '/' if it is a directory and ' '
 * otherwise.
 *
 * Returns the number of matches; the names are then readable with
 * whell_completion_name(). */
int whell_complete(const char *buf, int len, char *add, int add_size);
const char *whell_completion_name(int index);
int whell_completion_is_dir(int index);

/* Builtins. */
int cmd_cd(int argc, char **argv);
int cmd_help(int argc, char **argv);

#endif /* WHELL_H */
