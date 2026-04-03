/* cd, pwd, cat and help. */

#include "whell.h"

#define HOME_DIR "/home"

/* Where `cd -` goes back to. */
static char previous_dir[W_PATH_MAX + 1];
static int  have_previous;

int cmd_pwd(int argc, char **argv)
{
    char cwd[W_PATH_MAX + 1];

    int r = wgetcwd(cwd, sizeof(cwd));
    if (r < 0) {
        wfprintf(W_STDERR, "pwd: %s\n", wstrerror(-r));
        return 1;
    }

    wprintf("%s\n", cwd);
    return 0;
}

int cmd_cd(int argc, char **argv)
{
    char here[W_PATH_MAX + 1];
    const char *target;

    if (wgetcwd(here, sizeof(here)) < 0)
        here[0] = '\0';

    if (argc < 2) {
        target = HOME_DIR;
    } else if (strcmp(argv[1], "-") == 0) {
        if (!have_previous) {
            wfprintf(W_STDERR, "cd: OLDPWD not set\n");
            return 1;
        }
        target = previous_dir;
        /* Like Linux, `cd -` announces where it landed. */
        wprintf("%s\n", previous_dir);
    } else {
        target = argv[1];
    }

    int r = wchdir(target);
    if (r < 0) {
        wfprintf(W_STDERR, "cd: %s: %s\n", target, wstrerror(-r));
        return 1;
    }

    if (here[0]) {
        strlcpy(previous_dir, here, sizeof(previous_dir));
        have_previous = 1;
    }

    return 0;
}

int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        wfprintf(W_STDERR, "cat: no file given\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        int fd = wopen(argv[i], W_O_RDONLY);
        if (fd < 0) {
            wfprintf(W_STDERR, "cat: %s: %s\n", argv[i], wstrerror(-fd));
            status = 1;
            continue;
        }

        char buf[512];
        int  n;
        while ((n = wread(fd, buf, sizeof(buf))) > 0)
            wwrite(W_STDOUT, buf, (wsize_t)n);

        if (n < 0) {
            wfprintf(W_STDERR, "cat: %s: %s\n", argv[i], wstrerror(-n));
            status = 1;
        }

        wclose(fd);
    }

    return status;
}

int cmd_clear(int argc, char **argv)
{
    wcls();
    return 0;
}

int cmd_help(int argc, char **argv)
{
    wprintf("whell -- the WOS shell\n\n");
    wprintf("Builtins:\n");
    wprintf("  ls [-l] [-a] [path...]   list directory contents\n");
    wprintf("  cd [path | -]            change directory (no argument: %s)\n",
            HOME_DIR);
    wprintf("  pwd                      print the working directory\n");
    wprintf("  free [-b|-k|-m|-h]       show memory use\n");
    wprintf("  df [-b|-k|-m|-h]         show disk use\n");
    wprintf("  ps                       show processes and their memory\n");
    wprintf("  cat file...              print files\n");
    wprintf("  touch file...            create empty files\n");
    wprintf("  mkdir dir...             create directories\n");
    wprintf("  rm [-r] [-f] file...     remove files, -r for directories\n");
    wprintf("  clear                    clear the screen\n");
    wprintf("  shutdown                 power the machine off\n");
    wprintf("  help                     this text\n");
    wprintf("  exit [status]            leave the shell\n\n");
    wprintf("Anything else is looked up as /app/<name>/launch, so typing\n");
    wprintf("`hello` runs /app/hello/launch. A name containing a '/' is\n");
    wprintf("run as a path directly.\n");
    return 0;
}
