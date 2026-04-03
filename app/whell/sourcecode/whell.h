/* whell -- the WOS shell.
 *
 * Shared declarations between the main loop and the builtins.  Every builtin
 * has the same shape as main(): it takes argc/argv where argv[0] is the
 * command name, and returns an exit status where 0 means success.
 */
#ifndef WHELL_H
#define WHELL_H

#include <wkernel.h>

#define WHELL_MAX_ARGS 32
#define WHELL_LINE_MAX 512

/* Terminal width, used to lay ls out in columns. The VGA console is 80x25
 * and there is no way to ask it, so this is simply what it is. */
#define WHELL_COLUMNS 80

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
int cmd_ls(int argc, char **argv);
int cmd_free(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_df(int argc, char **argv);
int cmd_ps(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_clear(int argc, char **argv);
int cmd_help(int argc, char **argv);
int cmd_shutdown(int argc, char **argv);

#endif /* WHELL_H */
