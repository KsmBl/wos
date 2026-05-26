/* uptime -- how long the machine has been running.
 *
 * The line is the one htop draws in its header, from the same function, so the
 * two cannot drift apart.  There is no load average: WOS has no notion of one,
 * and inventing a number that looks like Linux's would be worse than leaving
 * it out.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            wprintf("usage: uptime [-p]\n");
            wprintf("  -p  the uptime alone, without the clock\n");
            return 0;
        }

        if (strcmp(argv[i], "-p") == 0) {
            wprintf("up %s\n", wuptime_string());
            return 0;
        }

        wfprintf(W_STDERR, "uptime: unexpected argument: %s\n", argv[i]);
        return 1;
    }

    /* The clock first, the way uptime has always started, then how long since
     * this machine started and how many users it knows about.  wuserlist()
     * reports what it wrote, so it is asked for a real list rather than a
     * count. */
    wtime_t   now;
    wuser_t   list[16];
    int       users = wuserlist(list, 16);

    if (wtime_get(&now) == 0)
        wprintf(" %02d:%02d:%02d  up %s,  %d user%s\n",
                now.hour, now.minute, now.second, wuptime_string(),
                users < 0 ? 1 : users, (users == 1) ? "" : "s");
    else
        wprintf(" up %s\n", wuptime_string());

    return 0;
}
