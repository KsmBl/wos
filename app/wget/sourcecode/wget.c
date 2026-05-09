/* wget -- download a URL to a file.
 *
 *   wget <url>            save to the file named in the URL (or index.html)
 *   wget -O <file> <url>  save to <file>
 *
 * HTTP only, and it follows up to a few redirects as long as they stay HTTP.
 */

#include <wkernel.h>

/* Choose an output name from the URL's path: the part after the last '/', or
 * index.html when the path is empty or ends in a slash. */
static void name_from_url(const char *url, char *out, int size)
{
    char host[256], path[1024];
    int  port;
    if (whttp_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        strlcpy(out, "index.html", (wsize_t)size);
        return;
    }

    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (base[0] == '\0')
        strlcpy(out, "index.html", (wsize_t)size);
    else
        strlcpy(out, base, (wsize_t)size);
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    const char *outfile = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
            outfile = argv[++i];
        else
            url = argv[i];
    }

    if (!url) {
        wfprintf(W_STDERR, "usage: wget [-O file] <url>\n");
        return 2;
    }

    /* Follow redirects, keeping the current URL in a buffer we can overwrite. */
    char current[1200];
    strlcpy(current, url, sizeof(current));

    whttp_t r;
    int rc = 0;
    for (int hop = 0; hop < 5; hop++) {
        wprintf("Connecting to %s ...\n", current);
        rc = whttp_get(current, &r);
        if (rc < 0)
            break;

        if (r.status >= 300 && r.status < 400 && r.location[0]) {
            wprintf("  %d redirect -> %s\n", r.status, r.location);
            strlcpy(current, r.location, sizeof(current));
            free(r.raw);
            continue;
        }
        break;
    }

    if (rc == -2) {
        wfprintf(W_STDERR, "wget: https is not supported (no TLS)\n");
        return 1;
    }
    if (rc < 0) {
        wfprintf(W_STDERR, "wget: %s: %s\n", url, wstrerror(-rc));
        return 1;
    }
    if (r.status != 200) {
        wfprintf(W_STDERR, "wget: server returned HTTP %d\n", r.status);
        free(r.raw);
        return 1;
    }

    char name[256];
    if (outfile)
        strlcpy(name, outfile, sizeof(name));
    else
        name_from_url(current, name, sizeof(name));

    int fd = wopen(name, W_O_WRONLY | W_O_CREAT | W_O_TRUNC);
    if (fd < 0) {
        wfprintf(W_STDERR, "wget: cannot write %s: %s\n", name, wstrerror(-fd));
        free(r.raw);
        return 1;
    }

    int written = 0;
    while (written < r.body_len) {
        int n = wwrite(fd, r.body + written, r.body_len - written);
        if (n <= 0) break;
        written += n;
    }
    wclose(fd);
    free(r.raw);

    wprintf("Saved %d bytes to %s\n", written, name);
    return 0;
}
