/* i3's IPC, from the asking side.  See wipc.h. */

#include <wipc.h>

void wipc_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}

uint32_t wipc_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wipc_message(const char *path, uint32_t type, const char *payload,
                 char *reply, wsize_t size)
{
    if (!path)
        path = WIPC_SWAY_SOCKET;

    int fd = wconnect(path);
    if (fd < 0)
        return fd;

    uint8_t  header[WIPC_HEADER];
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;

    memcpy(header, WIPC_MAGIC, WIPC_MAGIC_LEN);
    wipc_put32(header + WIPC_MAGIC_LEN, len);
    wipc_put32(header + WIPC_MAGIC_LEN + 4, type);

    wmsg_t out = { header, sizeof(header), 0, 0 };
    wsend(fd, &out);

    if (len) {
        wmsg_t body = { (void *)payload, len, 0, 0 };
        wsend(fd, &body);
    }

    /* The reply may arrive in pieces, and the header says how many bytes to
     * wait for.  Reading once would work most of the time, which is the worst
     * kind of working. */
    static uint8_t buf[16384];
    uint32_t       have = 0;

    while (have < sizeof(buf)) {
        wpollfd_t ready = { fd, W_POLLIN, 0 };

        if (wpoll(&ready, 1, 2000) <= 0)
            break;

        wmsg_t m = { buf + have, sizeof(buf) - have, 0, 0 };
        int    n = wrecv(fd, &m);

        if (n <= 0)
            break;

        have += (uint32_t)n;

        if (have >= WIPC_HEADER &&
            have >= WIPC_HEADER + wipc_get32(buf + WIPC_MAGIC_LEN))
            break;
    }

    wclose(fd);

    if (have < WIPC_HEADER)
        return -W_EIO;

    uint32_t body_len = wipc_get32(buf + WIPC_MAGIC_LEN);

    if (body_len > have - WIPC_HEADER)
        body_len = have - WIPC_HEADER;
    if (size && body_len > size - 1)
        body_len = (uint32_t)size - 1;

    if (reply && size) {
        memcpy(reply, buf + WIPC_HEADER, body_len);
        reply[body_len] = '\0';
    }

    return (int)body_len;
}

int wipc_command(const char *path, const char *command)
{
    char reply[256];
    int  r = wipc_message(path, WIPC_RUN_COMMAND, command, reply,
                          sizeof(reply));

    if (r < 0)
        return r;

    /* The compositor answers a command with a list of one object saying
     * whether it worked.  Anything else is a refusal. */
    return strstr(reply, "\"success\":true") ? 0 : -W_EINVAL;
}

int wipc_field(const char *json, const char *key, char *out, wsize_t size)
{
    char pattern[64];

    if (!json || !out || !size)
        return 0;

    wsnprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *at = strstr(json, pattern);

    out[0] = '\0';
    if (!at)
        return 0;

    at += strlen(pattern);

    wsize_t n = 0;

    if (*at == '"') {
        at++;
        while (*at && *at != '"' && n + 1 < size) {
            /* A backslash escapes the character after it, including the quote
             * that would otherwise end the string. */
            if (*at == '\\' && at[1])
                at++;
            out[n++] = *at++;
        }
    } else {
        while (*at && *at != ',' && *at != '}' && *at != ']' && n + 1 < size)
            out[n++] = *at++;
    }

    out[n] = '\0';
    return 1;
}
