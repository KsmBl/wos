/* lynx -- a text web browser for WOS.
 *
 * Fetches a page over HTTP, renders the HTML to text -- dropping tags and
 * scripts, decoding entities, turning block elements into line breaks and
 * numbering links -- and shows it in a full-screen pager.  Type a link's
 * number and Enter to follow it, g to open a new URL, b to go back, q to quit.
 *
 * A WOS-native browser in the spirit of lynx, not a build of it: no CSS, no
 * JavaScript, no tables layout, and HTTP only (no TLS).  It renders the text of
 * the web, which for a great many pages is the point.
 */

#include <wkernel.h>

#define MAX_LINES 6000
#define MAX_LINKS 800

static char  *page;                 /* rendered text, newline separated */
static int    page_cap, page_len;
static int    line_off[MAX_LINES];
static int    nlines;

static char   link_url[MAX_LINKS][512];
static int    nlinks;

static int    screen_cols = 80, screen_rows = 25;

static int ci_eq_n(const char *a, const char *b, int n);

/* ------------------------------------------------------------------ *
 *  URL resolution
 * ------------------------------------------------------------------ */

/* Resolve href against the current page URL into `out`. */
static void resolve_url(const char *base, const char *href, char *out, int size)
{
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        strlcpy(out, href, (wsize_t)size);
        return;
    }

    /* Split base into scheme+host and path. */
    char scheme_host[300];
    const char *p = base;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    int n = 0;
    while (base[n] && base + n < p) n++;              /* keep "http://" */
    int hlen = 0;
    while (p[hlen] && p[hlen] != '/') hlen++;
    wsnprintf(scheme_host, sizeof(scheme_host), "http://%.*s", hlen, p);

    if (href[0] == '/') {
        wsnprintf(out, (wsize_t)size, "%s%s", scheme_host, href);
    } else {
        /* Relative to the base's directory. */
        char dir[600];
        strlcpy(dir, base, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash && slash > dir + 7) slash[1] = '\0';
        else strlcpy(dir, scheme_host, sizeof(dir));
        wsnprintf(out, (wsize_t)size, "%s%s", dir, href);
    }
    (void)scheme_host;
}

/* ------------------------------------------------------------------ *
 *  HTML -> text rendering
 * ------------------------------------------------------------------ */

static int cur_col;

static void emit_raw(char c)
{
    if (page_len + 1 >= page_cap)
        return;
    page[page_len++] = c;
}

static void emit_newline(void)
{
    /* Avoid piling up blank lines. */
    if (page_len > 0 && page[page_len - 1] == '\n' &&
        (page_len < 2 || page[page_len - 2] == '\n'))
        return;
    emit_raw('\n');
    cur_col = 0;
}

/* Emit a word, wrapping at the screen width. */
static void emit_word(const char *w, int len)
{
    if (len == 0)
        return;
    if (cur_col > 0 && cur_col + 1 + len > screen_cols) {
        emit_raw('\n');
        cur_col = 0;
    } else if (cur_col > 0) {
        emit_raw(' ');
        cur_col++;
    }
    for (int i = 0; i < len; i++)
        emit_raw(w[i]);
    cur_col += len;
}

/* Decode a run of text (entities and whitespace) and emit it word by word. */
static void emit_text(const char *s, int len)
{
    char word[256];
    int  wl = 0;

    for (int i = 0; i < len; i++) {
        char c = s[i];

        if (c == '&') {
            /* Entity. */
            int j = i + 1, code = -1;
            char name[12]; int nl = 0;
            while (j < len && s[j] != ';' && nl < 11 && s[j] != '&' && s[j] != ' ')
                name[nl++] = s[j++];
            name[nl] = '\0';
            char sub = 0;
            if (name[0] == '#') sub = (char)atoi(name + 1);
            else if (strcmp(name, "amp") == 0)  sub = '&';
            else if (strcmp(name, "lt") == 0)   sub = '<';
            else if (strcmp(name, "gt") == 0)   sub = '>';
            else if (strcmp(name, "quot") == 0) sub = '"';
            else if (strcmp(name, "apos") == 0) sub = '\'';
            else if (strcmp(name, "nbsp") == 0) sub = ' ';
            (void)code;
            if (sub && j < len && s[j] == ';') {
                if (sub == ' ') { if (wl > 0) { emit_word(word, wl); wl = 0; } }
                else if (wl < (int)sizeof(word) - 1) word[wl++] = sub;
                i = j;
                continue;
            }
            /* Not an entity we know; fall through and keep the '&'. */
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (wl > 0) { emit_word(word, wl); wl = 0; }
        } else if (wl < (int)sizeof(word) - 1) {
            word[wl++] = c;
        }
    }
    if (wl > 0)
        emit_word(word, wl);
}

static int ci_eq(const char *a, const char *b)   /* case-insensitive equal */
{
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int is_block(const char *tag)
{
    static const char *b[] = { "p", "div", "br", "h1", "h2", "h3", "h4", "h5",
        "h6", "li", "ul", "ol", "tr", "table", "hr", "blockquote", "pre",
        "section", "article", "header", "footer", "nav", "form", 0 };
    for (int i = 0; b[i]; i++)
        if (ci_eq(tag, b[i])) return 1;
    return 0;
}

static void render(const char *base, const char *html, int len)
{
    page_len = 0; nlines = 0; nlinks = 0; cur_col = 0;
    int skip = 0;                       /* inside <script>/<style>        */
    int pending_link = -1;              /* link number awaiting its []     */

    int i = 0;
    while (i < len) {
        if (html[i] == '<') {
            /* Read the tag. */
            int j = i + 1;
            char tag[64]; int tl = 0;
            int closing = 0;
            if (j < len && html[j] == '/') { closing = 1; j++; }
            while (j < len && html[j] != '>' && html[j] != ' ' &&
                   html[j] != '\t' && html[j] != '\n' && tl < 63)
                tag[tl++] = html[j++];
            tag[tl] = '\0';

            /* The whole tag, for attribute scanning. */
            int tag_start = i;
            while (j < len && html[j] != '>') j++;
            int tag_end = j;            /* at '>' or len */

            if (ci_eq(tag, "script") || ci_eq(tag, "style") ||
                ci_eq(tag, "title") || ci_eq(tag, "head"))
                skip = !closing;
            else if (!skip) {
                if (ci_eq(tag, "a") && !closing) {
                    /* Find href="...". */
                    const char *seg = html + tag_start;
                    int seglen = tag_end - tag_start;
                    const char *h = 0;
                    for (int k = 0; k + 5 < seglen; k++)
                        if ((seg[k]=='h'||seg[k]=='H') && ci_eq_n(seg+k, "href", 4)) { h = seg + k + 4; break; }
                    if (h) {
                        while (*h == ' ' || *h == '=') h++;
                        char q = 0;
                        if (*h == '"' || *h == '\'') { q = *h; h++; }
                        char url[512]; int ul = 0;
                        while (*h && ul < 511 && (q ? *h != q : (*h != ' ' && *h != '>')))
                            url[ul++] = *h++;
                        url[ul] = '\0';
                        if (nlinks < MAX_LINKS && url[0]) {
                            resolve_url(base, url, link_url[nlinks], 512);
                            pending_link = nlinks;
                            nlinks++;
                        }
                    }
                } else if (ci_eq(tag, "a") && closing) {
                    if (pending_link >= 0) {
                        char mark[12];
                        int m = wsnprintf(mark, sizeof(mark), "[%d]", pending_link + 1);
                        emit_word(mark, m);
                        pending_link = -1;
                    }
                } else if (is_block(tag)) {
                    emit_newline();
                    if (ci_eq(tag, "li") && !closing) emit_word("*", 1);
                }
            }
            i = (tag_end < len) ? tag_end + 1 : len;
            continue;
        }

        /* Text run up to the next tag. */
        int start = i;
        while (i < len && html[i] != '<') i++;
        if (!skip)
            emit_text(html + start, i - start);
    }
    emit_raw('\0');
    page_len--;                         /* drop the NUL from the count */

    /* Index the lines. */
    nlines = 0;
    line_off[nlines++] = 0;
    for (int k = 0; k < page_len && nlines < MAX_LINES; k++)
        if (page[k] == '\n') {
            page[k] = '\0';
            if (nlines < MAX_LINES) line_off[nlines++] = k + 1;
        }
}

static int ci_eq_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i];
        if (ca != cb) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 *  Fetch (following redirects)
 * ------------------------------------------------------------------ */

static int fetch(const char *url, char *final_url, int final_size, whttp_t *out)
{
    char cur[1200];
    strlcpy(cur, url, sizeof(cur));

    for (int hop = 0; hop < 6; hop++) {
        int rc = whttp_get(cur, out);
        if (rc < 0)
            return rc;
        if (out->status >= 300 && out->status < 400 && out->location[0]) {
            char next[1200];
            resolve_url(cur, out->location, next, sizeof(next));
            strlcpy(cur, next, sizeof(cur));
            free(out->raw);
            continue;
        }
        strlcpy(final_url, cur, (wsize_t)final_size);
        return 0;
    }
    return -W_EIO;                      /* too many redirects */
}

/* ------------------------------------------------------------------ *
 *  Pager
 * ------------------------------------------------------------------ */

static void draw(const char *url, int top, const char *msg)
{
    wcls();
    int text_rows = screen_rows - 1;
    for (int r = 0; r < text_rows; r++) {
        int ln = top + r;
        wgotoxy(r + 1, 1);
        if (ln < nlines)
            wprintf("%s", page + line_off[ln]);
        wclear_line();
    }

    wgotoxy(screen_rows, 1);
    wcolor(W_BLACK, W_CYAN);
    char bar[512];
    wsnprintf(bar, sizeof(bar), " %s  (%d links)  [num]=follow g=go b=back q=quit %s",
              url, nlinks, msg ? msg : "");
    int pad = screen_cols - (int)strlen(bar);
    wprintf("%s", bar);
    for (int i = 0; i < pad; i++) wprintf(" ");
    wcolor_reset();
    wgotoxy(screen_rows, 1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        wfprintf(W_STDERR, "usage: lynx <url>\n");
        return 2;
    }

    wconsize(&screen_rows, &screen_cols);
    page_cap = 512 * 1024;
    page = malloc(page_cap);
    if (!page) { wfprintf(W_STDERR, "lynx: out of memory\n"); return 1; }

    /* A small history of URLs for 'b'. */
    static char history[16][1200];
    int hist = 0;
    strlcpy(history[0], argv[1], sizeof(history[0]));

    char url[1200];
    strlcpy(url, argv[1], sizeof(url));

    int prev = wconsole_raw(W_CONSOLE_RAW);

    int running = 1;
    while (running) {
        whttp_t r;
        char final_url[1200];
        int rc = fetch(url, final_url, sizeof(final_url), &r);

        if (rc == -2) {
            wconsole_raw(W_CONSOLE_CANONICAL);
            wprintf("lynx: %s is https, which WOS cannot fetch (no TLS)\n", url);
            wconsole_raw(W_CONSOLE_RAW);
            /* fall through to a one-line page */
            page_len = 0; nlines = 0; nlinks = 0;
            strlcpy(page, "(this page could not be loaded)", page_cap);
            line_off[0] = 0; nlines = 1;
        } else if (rc < 0) {
            wconsole_raw(W_CONSOLE_CANONICAL);
            wcls();
            wprintf("lynx: cannot load %s: %s\n", url, wstrerror(-rc));
            return 1;
        } else {
            strlcpy(url, final_url, sizeof(url));
            render(url, r.body, r.body_len);
            free(r.raw);
        }

        int top = 0;
        int text_rows = screen_rows - 1;
        int reload = 0;
        char msg[64]; msg[0] = '\0';

        while (!reload && running) {
            draw(url, top, msg);
            msg[0] = '\0';

            int key = wgetkey();
            if (key < 0) { running = 0; break; }

            if (key == 'q' || key == 'Q') {
                running = 0;
            } else if (key == W_KEY_DOWN || key == 'j') {
                if (top < nlines - 1) top++;
            } else if (key == W_KEY_UP || key == 'k') {
                if (top > 0) top--;
            } else if (key == ' ' || key == W_KEY_PGDN) {
                top += text_rows - 1; if (top > nlines - 1) top = nlines - 1;
                if (top < 0) top = 0;
            } else if (key == 'b' || key == W_KEY_PGUP) {
                if (key == 'b') {
                    if (hist > 0) { hist--; strlcpy(url, history[hist], sizeof(url)); reload = 1; }
                    else strlcpy(msg, "(no history)", sizeof(msg));
                } else {
                    top -= text_rows - 1; if (top < 0) top = 0;
                }
            } else if (key == 'g' || key == 'G') {
                wconsole_raw(W_CONSOLE_CANONICAL);
                wgotoxy(screen_rows, 1);
                wcolor_reset();
                wprintf("URL: "); wclear_line();
                char buf[1024];
                int n = wread(W_STDIN, buf, sizeof(buf) - 1);
                if (n > 0) {
                    while (n > 0 && (buf[n-1]=='\n'||buf[n-1]=='\r')) n--;
                    buf[n] = '\0';
                    if (buf[0]) {
                        strlcpy(url, buf, sizeof(url));
                        if (hist < 15) strlcpy(history[++hist], url, sizeof(history[0]));
                        reload = 1;
                    }
                }
                wconsole_raw(W_CONSOLE_RAW);
            } else if (key >= '0' && key <= '9') {
                /* Read the rest of a link number. */
                int num = key - '0';
                for (;;) {
                    int k2 = wgetkey();
                    if (k2 >= '0' && k2 <= '9') num = num * 10 + (k2 - '0');
                    else break;
                }
                if (num >= 1 && num <= nlinks) {
                    strlcpy(url, link_url[num - 1], sizeof(url));
                    if (hist < 15) strlcpy(history[++hist], url, sizeof(history[0]));
                    reload = 1;
                } else {
                    wsnprintf(msg, sizeof(msg), "(no link %d)", num);
                }
            }
        }
    }

    wconsole_raw(prev);
    wcls();
    free(page);
    return 0;
}
