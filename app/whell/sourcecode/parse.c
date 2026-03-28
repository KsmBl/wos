/* Command line splitting. */

#include "whell.h"

/* Split in place: overwrite each separator with a NUL and record where the
 * pieces start.  Quotes group whitespace into one argument and are removed;
 * because a quoted run is always shorter than what it replaces, the unquoted
 * text can be compacted into the same buffer. */
int whell_parse(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        char *out = p;
        argv[argc++] = out;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote)
                    *out++ = *p++;
                if (*p == quote)
                    p++;
            } else {
                *out++ = *p++;
            }
        }

        /* If the argument ends at the end of the line there is no separator
         * to overwrite, so terminate before advancing past it. */
        int at_end = (*p == '\0');
        if (!at_end)
            p++;
        *out = '\0';

        if (at_end)
            break;
    }

    argv[argc] = NULL;
    return argc;
}
