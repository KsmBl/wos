/* curl -- fetch a URL and print it.
 *
 *   curl <url>        print the body
 *   curl -i <url>     print headers and body
 *   curl -I <url>     print headers only
 *
 * HTTP only (no https), and dotted addresses or DNS names, e.g.
 *   curl http://example.com
 */

#include <wkernel.h>

static void fail(const char *what, int err)
{
    if (err == -2)
        wfprintf(W_STDERR, "curl: https is not supported (no TLS)\n");
    else if (-err == W_EHOSTUNREACH)
        wfprintf(W_STDERR, "curl: could not resolve or reach %s\n", what);
    else if (-err == W_ECONNREFUSED)
        wfprintf(W_STDERR, "curl: connection refused\n");
    else if (-err == W_ENODEV)
        wfprintf(W_STDERR, "curl: no network card\n");
    else
        wfprintf(W_STDERR, "curl: %s: %s\n", what, wstrerror(-err));
}

int main(int argc, char **argv)
{
    int show_headers = 0, show_body = 1;
    const char *url = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-I") == 0)      { show_headers = 1; show_body = 0; }
        else if (strcmp(argv[i], "-i") == 0) { show_headers = 1; show_body = 1; }
        else url = argv[i];
    }

    if (!url) {
        wfprintf(W_STDERR, "usage: curl [-i|-I] <url>\n");
        return 2;
    }

    whttp_t r;
    int rc = whttp_get(url, &r);
    if (rc < 0) {
        fail(url, rc);
        return 1;
    }

    if (show_headers) {
        wprintf("%s\n\n", r.raw);            /* the header block */
    }
    if (show_body) {
        wwrite(W_STDOUT, r.body, r.body_len);
        if (r.body_len > 0 && r.body[r.body_len - 1] != '\n')
            wprintf("\n");
    }

    free(r.raw);
    return (r.status >= 200 && r.status < 400) ? 0 : 1;
}
