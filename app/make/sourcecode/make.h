/* make -- decide what has to be rebuilt, and rebuild it.
 *
 * A subset of the Unix tool: variables and `$(VAR)`, targets with
 * prerequisites, tab-indented recipes, the automatic variables `$@`, `$<` and
 * `$^`, `.PHONY`, and staleness decided by comparing the modification times
 * WFS keeps for every file.
 *
 * What it deliberately does not have: pattern rules, built-in rules, functions
 * like $(wildcard), conditionals, `include`, and parallel builds.  Every one of
 * those is a thing a Makefile written for this machine can do without, and
 * leaving them out is what keeps the whole program readable in an afternoon.
 *
 * The pieces:
 *
 *   parse.c  turns a Makefile into variables and rules, and expands `$(...)`
 *   build.c  walks the rules, decides what is stale and runs the recipes
 *   make.c   arguments, and the goals to build
 */
#ifndef WOS_MAKE_H
#define WOS_MAKE_H

#include <wkernel.h>

/* Fixed ceilings rather than growing arrays.  A Makefile on this machine
 * describes one program, and these are far above what that takes; meeting one
 * is a mistake in the Makefile, and is reported as such. */
#define MAX_RULES        128
#define MAX_TARGETS       8    /* targets sharing one recipe: `a b: c`    */
#define MAX_PREREQS      64
#define MAX_RECIPE_LINES 32
#define MAX_VARS         64

#define MAKE_LINE_MAX   1024   /* one logical line, continuations joined  */
#define MAKE_EXPAND_MAX 4096   /* the result of expanding one of them     */

typedef struct {
    char *name;
    char *value;

    /* `=` keeps the text and expands it every time it is used, so a variable
     * can mention one defined later.  `:=` expands once, here and now. */
    int   recursive;

    /* Set on the command line, where make's rule is that it wins: an
     * assignment in the file is ignored rather than overwriting it. */
    int   from_argv;
} var_t;

/* Where a rule is in the walk.  A rule met while it is still VISITING is one
 * that depends on itself, which is a Makefile that cannot be built. */
enum { RULE_UNVISITED = 0, RULE_VISITING, RULE_DONE };

typedef struct {
    char *target;
    char *prereq[MAX_PREREQS];
    int   prereq_count;
    char *recipe[MAX_RECIPE_LINES];
    int   recipe_count;

    int   phony;        /* named in .PHONY: build it whatever the times say */
    int   state;        /* RULE_*                                          */
    int   rebuilt;      /* its recipe ran during this make                 */
} rule_t;

/* ---- parse.c ---------------------------------------------------------- */

/* Read a Makefile into the rules and variables below.  Returns 0, or -1 after
 * reporting what was wrong with it and where. */
int makefile_read(const char *path);

/* Assign a variable.  `immediate` expands the value now (`:=`), `append` adds
 * to what is there (`+=`), `from_argv` marks it as coming from the command
 * line.  Returns 0 or -1. */
int var_set(const char *name, const char *value, int immediate, int append,
            int from_argv);

/* The value of a variable, expanded.  An unset variable is the empty string,
 * as in make -- a Makefile that mentions one is not an error. */
const char *var_get(const char *name);

/* Copy `in` to `out`, replacing `$(VAR)`, `${VAR}`, `$$`, and -- when `ctx` is
 * a rule rather than NULL -- `$@`, `$<` and `$^`.  Returns 0 or -1. */
int expand(const char *in, const rule_t *ctx, char *out, int size);

/* The rules, in the order the Makefile gave them. */
extern rule_t rules[MAX_RULES];
extern int    rule_count;

rule_t *rule_find(const char *target);

/* What `make` with no target named builds: the first target in the file, which
 * is why a Makefile puts the thing it is for at the top.  NULL if the file has
 * no rules at all. */
extern const char *default_goal;

/* ---- build.c ---------------------------------------------------------- */

/* Bring one target up to date, building whatever it depends on first.
 * Returns 1 if a recipe ran, 0 if nothing needed doing, or -1 on an error that
 * has already been reported. */
int make_target(const char *target, int depth);

/* ---- make.c ----------------------------------------------------------- */

extern int opt_dry_run;   /* -n: print the recipes, run none of them     */
extern int opt_silent;    /* -s: run them without printing them          */
extern int opt_always;    /* -B: rebuild everything, whatever the times  */

/* Duplicate a string, or report running out of memory and exit. */
char *xstrdup(const char *s);

#endif /* WOS_MAKE_H */
