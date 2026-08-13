/* wcc -- the driver: what was asked for, and in what order to do it.
 *
 *   wcc [-c] [-o out] [-I dir] [-D name[=value]] [-E] input...
 *
 * With -c each input is compiled to an object.  Without it, the objects and
 * any archives named are linked into an executable.  -E stops after the
 * preprocessor, which is how you find out what a header really said.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcc.h"

#define MAX_INPUTS 64

static const char *output_path;
static int         stop_after_preprocessing;
static int         compile_only;
static int         dump_tokens;

/* Replace the extension of `path` -- "foo/bar.c" becomes "bar.o". */
static char *object_name_for(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strrchr(base, '.');
    size_t      stem  = dot ? (size_t)(dot - base) : strlen(base);

    char *name = wcc_alloc(stem + 3);
    memcpy(name, base, stem);
    strcpy(name + stem, ".o");
    return name;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void usage(void)
{
    printf("usage: wcc [-c] [-o out] [-I dir] [-D name[=value]] "
           "[-E] input...\n"
           "\n"
           "  -c        compile each input to an object, do not link\n"
           "  -o out    where to put the result\n"
           "  -I dir    look here for #include <...>\n"
           "  -D n[=v]  define a macro before reading anything\n"
           "  -E        preprocess only, and print the result\n"
           "  -T file   the linker script to take the layout from\n"
           "\n"
           "Inputs ending in .c are compiled; .o and .a are for the linker.\n");
}

/* Preprocess one file and print what came out, which is what -E is for. */
static void print_preprocessed(token_t *tokens)
{
    int line = 0;
    const char *file = NULL;

    for (token_t *t = tokens; t; t = t->next) {
        if (t->file != file || t->line != line) {
            if (file)
                printf("\n");
            file = t->file;
            line = t->line;
        } else if (t->has_space) {
            printf(" ");
        }

        if (t->kind == TK_STRING)
            printf("\"%s\"", t->string);
        else
            printf("%.*s", t->len, t->text);
    }
    printf("\n");
}

static token_t *front_end(const char *path)
{
    char *text = read_file(path);

    if (!text)
        fatal("cannot read %s", path);

    return preprocess(lex(path, text));
}

/* Compile one .c file into one object.  Returns the object's path. */
static const char *compile_one(const char *path, const char *object_path)
{
    token_t *tokens = front_end(path);

    if (stop_after_preprocessing) {
        print_preprocessed(tokens);
        return NULL;
    }

    if (dump_tokens) {
        for (token_t *t = tokens; t; t = t->next)
            printf("%s:%d %d [%.*s]\n", t->file, t->line, (int)t->kind,
                   t->len, t->text);
        return NULL;
    }

    obj_t *program = parse(tokens);

    unit_t unit;
    memset(&unit, 0, sizeof(unit));
    for (int i = 0; i < SEC_COUNT; i++)
        buf_init(&unit.section[i]);

    generate(&unit, program);

    if (write_object(&unit, object_path) != 0)
        fatal("cannot write %s", object_path);

    return object_path;
}

int main(int argc, char **argv)
{
    const char *inputs[MAX_INPUTS];
    int         input_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-c") == 0) {
            compile_only = 1;
        } else if (strcmp(arg, "-E") == 0) {
            stop_after_preprocessing = 1;
        } else if (strcmp(arg, "--tokens") == 0) {
            dump_tokens = 1;
        } else if (strcmp(arg, "-o") == 0) {
            if (++i == argc)
                fatal("-o needs a file");
            output_path = argv[i];
        } else if (strncmp(arg, "-o", 2) == 0 && arg[2]) {
            output_path = arg + 2;
        } else if (strcmp(arg, "-I") == 0) {
            if (++i == argc)
                fatal("-I needs a directory");
            pp_add_include_path(argv[i]);
        } else if (strncmp(arg, "-I", 2) == 0 && arg[2]) {
            pp_add_include_path(arg + 2);
        } else if (strcmp(arg, "-D") == 0) {
            if (++i == argc)
                fatal("-D needs a name");
            pp_define_from_argument(argv[i]);
        } else if (strncmp(arg, "-D", 2) == 0 && arg[2]) {
            pp_define_from_argument(arg + 2);
        } else if (strcmp(arg, "-T") == 0) {
            /* The layout is fixed and known; taking the flag and ignoring it
             * means the generated Makefiles do not have to special-case this
             * compiler. */
            if (++i == argc)
                fatal("-T needs a file");
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else if (arg[0] == '-' && arg[1]) {
            /* -g, -O2, -Wall and the rest: this compiler has one setting for
             * each of those and it is not negotiable, so saying so and
             * carrying on beats refusing to build. */
            fprintf(stderr, "wcc: ignoring %s\n", arg);
        } else if (input_count < MAX_INPUTS) {
            inputs[input_count++] = arg;
        } else {
            fatal("too many input files");
        }
    }

    if (input_count == 0) {
        usage();
        return 1;
    }

    /* Where the compiler's own headers live: stddef.h and the rest, which
     * belong to a compiler rather than to a C library.  Last, so a -I can put
     * something in front of them. */
    pp_add_include_path("/lib/wcc/include");
    pp_add_include_path("/include");

    char       *link_inputs[MAX_INPUTS];
    int         link_count = 0;

    for (int i = 0; i < input_count; i++) {
        const char *path = inputs[i];

        if (ends_with(path, ".c")) {
            const char *object = (compile_only && output_path)
                                   ? output_path : object_name_for(path);

            /* Without -c the object is a step on the way, and the name it
             * takes is the one it would have had anyway. */
            const char *made = compile_one(path, object);
            if (made)
                link_inputs[link_count++] = (char *)made;
        } else if (ends_with(path, ".o") || ends_with(path, ".a")) {
            link_inputs[link_count++] = (char *)path;
        } else {
            fatal("%s: not a .c, .o or .a file", path);
        }
    }

    if (stop_after_preprocessing || dump_tokens || compile_only)
        return 0;

    if (link_count == 0)
        fatal("nothing to link");

    return link_executable(link_inputs, link_count,
                           output_path ? output_path : "a.out");
}
