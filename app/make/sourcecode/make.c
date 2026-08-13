/* make -- arguments, and the goals to build.  See make.h.
 *
 *   make                 build the first target in the Makefile
 *   make hello clean     build these targets, in this order
 *   make -f build.mk     read a Makefile by another name
 *   make CC=tcc          set a variable, overriding what the Makefile says
 *   make -n              print the commands without running any of them
 *   make -s              run them without printing them
 *   make -B              rebuild everything, whatever the times say
 *   make -C /app/ls/sourcecode   work in that directory
 */

#include "make.h"

int opt_dry_run;
int opt_silent;
int opt_always;

/* Tried in this order when no -f is given, which is the convention every
 * make follows. */
static const char *default_names[] = { "Makefile", "makefile" };

static void usage(void)
{
    wprintf("usage: make [-f file] [-C dir] [-n] [-s] [-B] "
            "[VAR=value ...] [target ...]\n"
            "\n"
            "  -f file  read this Makefile instead of Makefile or makefile\n"
            "  -C dir   change to this directory first\n"
            "  -n       print the commands, run none of them\n"
            "  -s       run the commands without printing them\n"
            "  -B       rebuild every target, whatever the times say\n"
            "\n"
            "A target is out of date when something it is built from has a\n"
            "later modification time than it has.  With no target named, the\n"
            "first one in the Makefile is built.\n");
}

/* A `NAME=value` argument: make's way of setting a variable for one build. */
static int is_argv_assignment(const char *arg)
{
    const char *eq = strchr(arg, '=');

    return eq && eq != arg;
}

static int apply_argv_assignment(const char *arg)
{
    char name[128];

    const char *eq  = strchr(arg, '=');
    wsize_t     len = (wsize_t)(eq - arg);

    if (len >= sizeof(name)) {
        wfprintf(W_STDERR, "make: variable name too long: %s\n", arg);
        return -1;
    }

    memcpy(name, arg, len);
    name[len] = '\0';

    /* Immediate, and marked as coming from the command line so the Makefile's
     * own assignment to the same name is ignored rather than overwriting it. */
    return var_set(name, eq + 1, 1, 0, 1);
}

int main(int argc, char **argv)
{
    const char *makefile = NULL;
    const char *goals[MAX_RULES];
    int         goal_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-f") == 0) {
            if (++i == argc) {
                wfprintf(W_STDERR, "make: -f needs a file\n");
                return 2;
            }
            makefile = argv[i];
        } else if (strcmp(arg, "-C") == 0) {
            if (++i == argc) {
                wfprintf(W_STDERR, "make: -C needs a directory\n");
                return 2;
            }
            int r = wchdir(argv[i]);
            if (r < 0) {
                wfprintf(W_STDERR, "make: %s: %s\n", argv[i], wstrerror(-r));
                return 2;
            }
        } else if (strcmp(arg, "-n") == 0) {
            opt_dry_run = 1;
        } else if (strcmp(arg, "-s") == 0) {
            opt_silent = 1;
        } else if (strcmp(arg, "-B") == 0) {
            opt_always = 1;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else if (arg[0] == '-' && arg[1]) {
            wfprintf(W_STDERR, "make: unknown option %s\n", arg);
            usage();
            return 2;
        } else if (is_argv_assignment(arg)) {
            if (apply_argv_assignment(arg) < 0)
                return 2;
        } else if (goal_count < MAX_RULES) {
            goals[goal_count++] = arg;
        } else {
            wfprintf(W_STDERR, "make: too many targets named\n");
            return 2;
        }
    }

    if (!makefile) {
        for (unsigned i = 0; i < sizeof(default_names) / sizeof(*default_names);
             i++) {
            wstat_t st;
            if (wstat(default_names[i], &st) == 0 && st.type == W_FT_FILE) {
                makefile = default_names[i];
                break;
            }
        }
    }

    if (!makefile) {
        wfprintf(W_STDERR, "make: *** No Makefile here.  Stop.\n");
        return 2;
    }

    if (makefile_read(makefile) < 0)
        return 2;

    if (goal_count == 0) {
        if (!default_goal) {
            wfprintf(W_STDERR, "make: *** %s has no rules.  Stop.\n", makefile);
            return 2;
        }
        goals[goal_count++] = default_goal;
    }

    for (int i = 0; i < goal_count; i++) {
        int r = make_target(goals[i], 0);

        if (r < 0)
            return 2;

        /* Saying so matters: a make that printed nothing at all leaves you
         * wondering whether it looked. */
        if (r == 0) {
            rule_t *rule = rule_find(goals[i]);

            if (rule && rule->recipe_count == 0)
                wprintf("make: nothing to be done for '%s'\n", goals[i]);
            else
                wprintf("make: '%s' is up to date\n", goals[i]);
        }
    }

    return 0;
}
