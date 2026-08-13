/* Deciding what is out of date, and running the recipes that fix it.
 *
 * The whole of make is one idea: a target is stale when something it is built
 * from is newer than it is.  Everything here is that comparison, plus the walk
 * that makes sure a prerequisite is itself up to date before its time is read.
 */

#include "make.h"

/* The shell recipes are run through.  Not the user's login shell: a recipe is
 * written against one syntax and has to get the one it was written for, and
 * whell is the one every Makefile on this machine can assume. */
#define RECIPE_SHELL "/app/whell/launch"

/* A Makefile whose rules chain deeper than this is either enormous or a
 * mistake, and the recursion here runs on a fixed stack. */
#define MAX_DEPTH 64

/* Run one line of a recipe.  Returns 0, or -1 after reporting the failure. */
static int run_recipe_line(rule_t *r, const char *line)
{
    char cmd[MAKE_EXPAND_MAX];

    if (expand(line, r, cmd, sizeof(cmd)) < 0)
        return -1;

    /* Two prefixes, as in make: '@' runs the command without printing it, '-'
     * carries on even if it fails. */
    int   quiet  = 0;
    int   ignore = 0;
    char *p      = cmd;

    while (*p == '@' || *p == '-') {
        if (*p == '@')
            quiet = 1;
        else
            ignore = 1;
        p++;
    }

    while (*p == ' ' || *p == '\t')
        p++;

    if (!*p)
        return 0;                       /* a line of prefixes and nothing else */

    /* -n prints even the quiet ones: the point of it is to see what would
     * happen, and half the commands is not that. */
    if (!opt_silent && (!quiet || opt_dry_run))
        wprintf("%s\n", p);

    if (opt_dry_run)
        return 0;

    char *argv[] = { "whell", "-c", p, NULL };

    int pid = wspawn(RECIPE_SHELL, argv);
    if (pid < 0) {
        wfprintf(W_STDERR, "make: cannot run a recipe: %s\n", wstrerror(-pid));
        return -1;
    }

    int status = 0;
    int reaped = wwait(pid, &status);
    if (reaped < 0) {
        wfprintf(W_STDERR, "make: waiting for a recipe failed: %s\n",
                 wstrerror(-reaped));
        return -1;
    }

    if (status != 0) {
        if (ignore) {
            wprintf("make: [%s] Error %d (ignored)\n", r->target, status);
            return 0;
        }
        wfprintf(W_STDERR, "make: *** [%s] Error %d\n", r->target, status);
        return -1;
    }

    return 0;
}

static int run_recipe(rule_t *r)
{
    for (int i = 0; i < r->recipe_count; i++)
        if (run_recipe_line(r, r->recipe[i]) < 0)
            return -1;

    return 0;
}

int make_target(const char *target, int depth)
{
    if (depth > MAX_DEPTH) {
        wfprintf(W_STDERR, "make: '%s' is nested too deeply to build\n", target);
        return -1;
    }

    rule_t *r = rule_find(target);

    wstat_t  st;
    int      exists = (wstat(target, &st) == 0);
    uint32_t when   = exists ? st.mtime : 0;

    /* Something with no rule is a file that was already there -- a source
     * file.  Nothing has to happen to it; it only has to exist. */
    if (!r) {
        if (exists)
            return 0;

        wfprintf(W_STDERR, "make: *** No rule to make target '%s'.  Stop.\n",
                 target);
        return -1;
    }

    /* A rule met on the way down to itself is a cycle, and following it would
     * be an infinite walk rather than a build. */
    if (r->state == RULE_VISITING) {
        wfprintf(W_STDERR,
                 "make: *** '%s' depends on itself.  Stop.\n", target);
        return -1;
    }
    if (r->state == RULE_DONE)
        return r->rebuilt;

    r->state = RULE_VISITING;

    uint32_t newest          = 0;
    int      prereq_rebuilt  = 0;
    int      time_unknown    = 0;

    for (int i = 0; i < r->prereq_count; i++) {
        int got = make_target(r->prereq[i], depth + 1);
        if (got < 0) {
            r->state = RULE_DONE;
            return -1;
        }
        if (got == 1)
            prereq_rebuilt = 1;

        wstat_t pst;
        if (wstat(r->prereq[i], &pst) == 0) {
            /* A file the clock could not date cannot be compared with, so it
             * counts as newer: rebuilding something that did not need it costs
             * a compile, and skipping one that did costs a wrong binary. */
            if (pst.mtime == 0)
                time_unknown = 1;
            else if (pst.mtime > newest)
                newest = pst.mtime;
        }
    }

    int stale = opt_always || r->phony || !exists || prereq_rebuilt ||
                time_unknown || when == 0 || newest > when;

    /* A target with prerequisites and no recipe -- `all: hello world` -- is a
     * name for a group, and building it is building them. */
    if (stale && r->recipe_count > 0) {
        if (run_recipe(r) < 0) {
            r->state = RULE_DONE;
            return -1;
        }
        r->rebuilt = 1;
    } else {
        r->rebuilt = stale && prereq_rebuilt;
    }

    r->state = RULE_DONE;
    return r->rebuilt;
}
