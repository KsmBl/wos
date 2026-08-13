/* Text into tokens.
 *
 * The whole file is tokenised at once into a linked list, which is what makes
 * the preprocessor simple: a macro expansion is a list spliced into another
 * list, and there is no second reader to keep in step with the first.
 *
 * Two things are kept on every token that a compiler without a preprocessor
 * would not need: whether it stood at the start of a line, because that is
 * what makes a '#' a directive rather than an operator; and whether a space
 * came before it, because stringifying a macro argument has to put the spaces
 * back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "wcc.h"

/* The multi-character operators, longest first: the scanner takes the first
 * that matches, so `<<=` has to be tried before `<<` and `<`. */
static const char *const punctuation[] = {
    "<<=", ">>=", "...",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
    "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "=", "<", ">",
    "(", ")", "[", "]", "{", "}", ".", ",", ";", ":", "?", "#",
    NULL,
};

int token_is(const token_t *t, const char *text)
{
    return t && (int)strlen(text) == t->len &&
           memcmp(t->text, text, (size_t)t->len) == 0;
}

static token_t *new_token(token_kind_t kind, const char *file, int line,
                          const char *start, int len)
{
    token_t *t = wcc_alloc(sizeof(*t));

    t->kind = kind;
    t->file = file;
    t->line = line;
    t->text = start;
    t->len  = len;
    return t;
}

static int starts_with(const char *p, const char *prefix)
{
    return strncmp(p, prefix, strlen(prefix)) == 0;
}

static int is_ident_start(int c)
{
    return isalpha(c) || c == '_';
}

static int is_ident_char(int c)
{
    return isalnum(c) || c == '_';
}

/* One escape sequence, from just past the backslash.  `*after` is left on the
 * character following it. */
static int read_escape(const char *p, const char **after)
{
    switch (*p) {
    case 'a':  *after = p + 1; return '\a';
    case 'b':  *after = p + 1; return '\b';
    case 'f':  *after = p + 1; return '\f';
    case 'n':  *after = p + 1; return '\n';
    case 'r':  *after = p + 1; return '\r';
    case 't':  *after = p + 1; return '\t';
    case 'v':  *after = p + 1; return '\v';
    case 'e':  *after = p + 1; return 27;     /* a GNU extension worth having */
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        int value = 0;
        for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++)
            value = value * 8 + (*p++ - '0');
        *after = p;
        return value & 0xFF;
    }
    case 'x': {
        int value = 0;
        p++;
        while (isxdigit((unsigned char)*p)) {
            int digit = isdigit((unsigned char)*p) ? *p - '0'
                      : (tolower(*p) - 'a' + 10);
            value = value * 16 + digit;
            p++;
        }
        *after = p;
        return value & 0xFF;
    }
    default:
        /* Anything else stands for itself, which covers \\ \' \" and \? and
         * is what the standard says for the rest. */
        *after = p + 1;
        return *p;
    }
}

static token_t *read_string(const char *file, int line, const char *start,
                            const char **after)
{
    const char *p = start + 1;
    buffer_t    text;

    buf_init(&text);

    while (*p != '"') {
        if (*p == '\0' || *p == '\n')
            error_at(file, line, "unterminated string literal");

        if (*p == '\\') {
            p++;
            buf_byte(&text, read_escape(p, &p));
        } else {
            buf_byte(&text, *p++);
        }
    }
    p++;

    buf_byte(&text, '\0');

    token_t *t = new_token(TK_STRING, file, line, start, (int)(p - start));
    t->string     = (char *)text.data;
    t->string_len = text.len;
    *after = p;
    return t;
}

static token_t *read_char(const char *file, int line, const char *start,
                          const char **after)
{
    const char *p = start + 1;
    int         value;

    if (*p == '\\') {
        p++;
        value = read_escape(p, &p);
    } else if (*p == '\0' || *p == '\n') {
        error_at(file, line, "unterminated character constant");
        return NULL;
    } else {
        value = *p++;
    }

    if (*p != '\'')
        error_at(file, line, "a character constant holds one character");
    p++;

    /* A plain char is signed here, and '\xff' is -1 in an int context. */
    token_t *t = new_token(TK_CHAR, file, line, start, (int)(p - start));
    t->value = (char)value;
    *after = p;
    return t;
}

static token_t *read_number(const char *file, int line, const char *start,
                            const char **after)
{
    const char   *p = start;
    unsigned long value = 0;
    int           base = 10;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;                                   /* a GNU extension */
        p += 2;
    } else if (p[0] == '0' && isdigit((unsigned char)p[1])) {
        base = 8;
        p++;
    }

    for (;;) {
        int digit;

        if (isdigit((unsigned char)*p))
            digit = *p - '0';
        else if (isxdigit((unsigned char)*p))
            digit = tolower(*p) - 'a' + 10;
        else
            break;

        if (digit >= base)
            break;

        value = value * (unsigned long)base + (unsigned long)digit;
        p++;
    }

    int is_unsigned = 0, is_long = 0;

    for (;;) {
        if (*p == 'u' || *p == 'U') { is_unsigned = 1; p++; }
        else if (*p == 'l' || *p == 'L') { is_long = 1; p++; }
        else break;
    }

    if (is_ident_start((unsigned char)*p))
        error_at(file, line, "invalid suffix on a number");

    token_t *t = new_token(TK_NUMBER, file, line, start, (int)(p - start));
    t->value       = (long)value;
    t->is_unsigned = is_unsigned;

    /* A decimal constant too large for an int is a long, which matters for
     * `1 << 40` and for the size of a pointer offset. */
    t->is_long = is_long || value > 0x7FFFFFFFUL;

    *after = p;
    return t;
}

token_t *lex(const char *file, char *text)
{
    token_t  head;
    token_t *tail = &head;
    const char *p = text;
    int line = 1;
    int at_line_start = 1;
    int has_space = 0;

    head.next = NULL;

    while (*p) {
        /* A backslash at the end of a line joins it to the next one, before
         * anything else looks at either. */
        if (p[0] == '\\' && p[1] == '\n') {
            p += 2;
            line++;
            has_space = 1;
            continue;
        }

        if (*p == '\n') {
            token_t *t = new_token(TK_NEWLINE, file, line, p, 1);
            tail = tail->next = t;
            p++;
            line++;
            at_line_start = 1;
            has_space = 0;
            continue;
        }

        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v') {
            p++;
            has_space = 1;
            continue;
        }

        if (starts_with(p, "//")) {
            while (*p && *p != '\n')
                p++;
            has_space = 1;
            continue;
        }

        if (starts_with(p, "/*")) {
            const char *end = strstr(p + 2, "*/");
            if (!end)
                error_at(file, line, "unterminated comment");

            for (const char *q = p; q < end; q++)
                if (*q == '\n')
                    line++;

            p = end + 2;
            has_space = 1;
            continue;
        }

        token_t *t = NULL;

        if (*p == '"') {
            t = read_string(file, line, p, &p);
        } else if (*p == '\'') {
            t = read_char(file, line, p, &p);
        } else if (isdigit((unsigned char)*p) ||
                   (*p == '.' && isdigit((unsigned char)p[1]))) {
            t = read_number(file, line, p, &p);
        } else if (is_ident_start((unsigned char)*p)) {
            const char *start = p;
            while (is_ident_char((unsigned char)*p))
                p++;
            t = new_token(TK_IDENT, file, line, start, (int)(p - start));
        } else {
            const char *found = NULL;
            for (int i = 0; punctuation[i]; i++)
                if (starts_with(p, punctuation[i])) {
                    found = punctuation[i];
                    break;
                }

            if (!found)
                error_at(file, line, "stray '%c' in the program", *p);

            t = new_token(TK_PUNCT, file, line, p, (int)strlen(found));
            p += t->len;
        }

        t->at_line_start = at_line_start;
        t->has_space     = has_space;
        at_line_start    = 0;
        has_space        = 0;

        tail = tail->next = t;
    }

    tail->next = new_token(TK_EOF, file, line, p, 0);
    tail->next->at_line_start = 1;
    return head.next;
}
