/* A small HTTP/1.0 client, shared by curl, wget and lynx.
 *
 * Plain HTTP only -- WOS has no TLS, so https:// URLs are refused.  It resolves
 * the host, opens a TCP connection, sends a GET, and reads the whole response
 * into one buffer, then splits the status line, headers and body.
 */

#include "wkernel.h"

#define HTTP_CAP (256 * 1024)      /* largest response we will hold */

/* Split a URL into host, port and path.  Returns 0, or -1 for a URL we cannot
 * handle (https, or malformed). */
int whttp_parse_url(const char *url, char *host, int host_size,
                    int *port, char *path, int path_size)
{
    if (strncmp(url, "https://", 8) == 0)
        return -2;                 /* TLS not supported */
    if (strncmp(url, "http://", 7) == 0)
        url += 7;

    *port = 80;

    /* Host runs until '/', ':' or end. */
    int n = 0;
    while (*url && *url != '/' && *url != ':' && n < host_size - 1)
        host[n++] = *url++;
    host[n] = '\0';
    if (n == 0)
        return -1;

    if (*url == ':') {
        url++;
        int p = 0;
        while (*url >= '0' && *url <= '9') { p = p * 10 + (*url - '0'); url++; }
        if (p > 0) *port = p;
    }

    if (*url == '\0') {
        strlcpy(path, "/", (wsize_t)path_size);
    } else {
        strlcpy(path, url, (wsize_t)path_size);
    }
    return 0;
}

/* Fetch `url`.  On success returns 0 and fills `out`; the caller frees
 * out->raw with free().  On failure returns a negative W_E* code, or -2 for an
 * https URL. */
int whttp_get(const char *url, whttp_t *out)
{
    memset(out, 0, sizeof(*out));

    char host[256], path[1024];
    int  port;
    int  pr = whttp_parse_url(url, host, sizeof(host), &port, path, sizeof(path));
    if (pr < 0)
        return pr == -2 ? -2 : -W_EINVAL;

    unsigned int ip;
    int r = wresolve(host, &ip);
    if (r < 0)
        return r;

    int h = wtcp_open(ip, port);
    if (h < 0)
        return h;

    /* HTTP/1.0 with Connection: close, so the server ends the body by closing
     * -- no chunked decoding needed. */
    char req[1400];
    int  rl = wsnprintf(req, sizeof(req),
                        "GET %s HTTP/1.0\r\n"
                        "Host: %s\r\n"
                        "User-Agent: wos/0.1\r\n"
                        "Accept: text/html, text/plain, */*\r\n"
                        "Connection: close\r\n\r\n",
                        path, host);
    if (wtcp_send(h, req, rl) < 0) {
        wtcp_close(h);
        return -W_EIO;
    }

    char *buf = malloc(HTTP_CAP);
    if (!buf) { wtcp_close(h); return -W_ENOMEM; }

    int total = 0;
    for (;;) {
        int n = wtcp_recv(h, buf + total, HTTP_CAP - 1 - total);
        if (n <= 0)
            break;                 /* 0 == the server closed the connection */
        total += n;
        if (total >= HTTP_CAP - 1)
            break;
    }
    buf[total] = '\0';
    wtcp_close(h);

    out->raw = buf;
    out->raw_len = total;

    /* Status line: "HTTP/1.x NNN ...". */
    if (strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf, ' ');
        if (sp) out->status = atoi(sp + 1);
    }

    /* Headers end at the blank line; the body is what follows. */
    char *sep = buf;
    for (int i = 0; i + 3 < total; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            sep = buf + i;
            break;
        }
    if (sep != buf) {
        *sep = '\0';               /* terminate the header block */
        out->body = sep + 4;
        out->body_len = total - (int)(out->body - buf);
    } else {
        out->body = buf;
        out->body_len = total;
    }

    /* A redirect's target, if any. */
    const char *loc = strstr(buf, "\nLocation:");
    if (!loc) loc = strstr(buf, "\nlocation:");
    if (loc) {
        loc += 10;
        while (*loc == ' ') loc++;
        int i = 0;
        while (*loc && *loc != '\r' && *loc != '\n' && i < (int)sizeof(out->location) - 1)
            out->location[i++] = *loc++;
        out->location[i] = '\0';
    }

    return 0;
}
