/* Tab completion.
 *
 * Completes the word at the end of the line.  The first word is a command, so
 * it is matched against the builtins and against /app -- which is where every
 * executable lives, so listing it is the same thing bash does with PATH.  Any
 * later word is a path, matched against the directory it names.
 */

#include "whell.h"

#define MAX_MATCHES 64

static char match_names[MAX_MATCHES][W_NAME_MAX + 1];
static char match_is_dir[MAX_MATCHES];
static int  match_count;

const char *whell_completion_name(int index)
{
    if (index < 0 || index >= match_count)
        return NULL;
    return match_names[index];
}

int whell_completion_is_dir(int index)
{
    if (index < 0 || index >= match_count)
        return 0;
    return match_is_dir[index];
}

static void add_match(const char *name, int is_dir)
{
    if (match_count >= MAX_MATCHES)
        return;

    strlcpy(match_names[match_count], name, W_NAME_MAX + 1);
    match_is_dir[match_count] = (char)is_dir;
    match_count++;
}

/* Collect the names in `dir` that begin with `prefix`. */
static void match_directory(const char *dir, const char *prefix,
                            int prefix_len, int want_dirs_only)
{
    int d = wopendir(dir);
    if (d < 0)
        return;

    wdirent_t e;
    while (wreaddir(d, &e) == 1) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;

        /* Hidden entries only show up once the user has typed the dot, which
         * is how shells avoid burying the useful names. */
        if (e.name[0] == '.' && (prefix_len == 0 || prefix[0] != '.'))
            continue;

        if (want_dirs_only && e.type != W_FT_DIR)
            continue;

        if (prefix_len > 0 && strncmp(e.name, prefix, (wsize_t)prefix_len) != 0)
            continue;

        add_match(e.name, e.type == W_FT_DIR);
    }

    wclosedir(d);
}

/* Commands: the builtins, plus every /app/<name> that really holds a launch
 * binary -- a directory without one is not a command and completing to it
 * would only produce "command not found". */
static void match_commands(const char *prefix, int prefix_len)
{
    for (int i = 0; ; i++) {
        const char *name = whell_builtin_name(i);
        if (!name)
            break;
        if (prefix_len == 0 ||
            strncmp(name, prefix, (wsize_t)prefix_len) == 0)
            add_match(name, 0);
    }

    int d = wopendir("/app");
    if (d < 0)
        return;

    wdirent_t e;
    while (wreaddir(d, &e) == 1) {
        if (e.type != W_FT_DIR)
            continue;
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0)
            continue;
        if (prefix_len > 0 &&
            strncmp(e.name, prefix, (wsize_t)prefix_len) != 0)
            continue;

        char launch[W_PATH_MAX + 1];
        wsnprintf(launch, sizeof(launch), "/app/%s/launch", e.name);

        wstat_t st;
        if (wstat(launch, &st) == 0 && st.type == W_FT_FILE)
            add_match(e.name, 0);
    }

    wclosedir(d);
}

/* How many leading characters all the matches share. */
static int common_prefix_length(void)
{
    if (match_count == 0)
        return 0;

    int common = (int)strlen(match_names[0]);

    for (int i = 1; i < match_count; i++) {
        int j = 0;
        while (j < common && match_names[i][j] == match_names[0][j])
            j++;
        common = j;
    }

    return common;
}

int whell_complete(const char *buf, int len, char *add, int add_size)
{
    add[0] = '\0';
    match_count = 0;

    /* The word under completion runs from the last separator to the end. */
    int start = len;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t')
        start--;

    const char *word = buf + start;
    int word_len = len - start;

    int is_command = 1;
    for (int i = 0; i < start; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') {
            is_command = 0;
            break;
        }
    }

    const char *prefix;
    int prefix_len;

    if (is_command && !strchr(word, '/')) {
        prefix = word;
        prefix_len = word_len;
        match_commands(prefix, prefix_len);
    } else {
        /* Split the word into the directory to search and the partial name. */
        char dir[W_PATH_MAX + 1];
        const char *slash = strrchr(word, '/');

        if (!slash) {
            strlcpy(dir, ".", sizeof(dir));
            prefix = word;
        } else {
            int dir_len = (int)(slash - word);
            if (dir_len == 0) {
                strlcpy(dir, "/", sizeof(dir));     /* completing in the root */
            } else {
                if (dir_len > W_PATH_MAX)
                    dir_len = W_PATH_MAX;
                memcpy(dir, word, (wsize_t)dir_len);
                dir[dir_len] = '\0';
            }
            prefix = slash + 1;
        }

        prefix_len = (int)strlen(prefix);
        match_directory(dir, prefix, prefix_len, 0);
    }

    if (match_count == 0)
        return 0;

    /* Work out what can be appended without ambiguity: the whole name when
     * there is one match, otherwise the part every match agrees on. */
    int take = (match_count == 1) ? (int)strlen(match_names[0])
                                  : common_prefix_length();

    int out = 0;
    for (int i = prefix_len; i < take && out + 1 < add_size; i++)
        add[out++] = match_names[0][i];

    /* A single match is finished, so close it off: a separator for a
     * directory the user will keep typing into, a space for anything else. */
    if (match_count == 1 && out + 1 < add_size)
        add[out++] = match_is_dir[0] ? '/' : ' ';

    add[out] = '\0';
    return match_count;
}
