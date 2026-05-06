/* ping -- send ICMP echo requests to a host and time the replies.
 *
 *   ping <ip>            ping four times
 *   ping -c <n> <ip>     ping n times (0 means keep going)
 *
 * Only dotted-decimal addresses: WOS has no DNS.  The network is QEMU's
 * user-mode one, so 10.0.2.2 (the gateway) is the address that always answers.
 */

#include <wkernel.h>

/* Parse "a.b.c.d" into a network-order address. */
static int parse_ip(const char *s, unsigned int *out)
{
    unsigned int part[4];
    int          n = 0, digits = 0;
    unsigned int cur = 0;

    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10 + (unsigned)(*p - '0');
            if (cur > 255)
                return -1;
            digits++;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || n > 3)
                return -1;
            part[n++] = cur;
            cur = 0;
            digits = 0;
            if (*p == '\0')
                break;
        } else {
            return -1;
        }
    }
    if (n != 4)
        return -1;

    *out = part[0] | (part[1] << 8) | (part[2] << 16) | (part[3] << 24);
    return 0;
}

static void print_ip(unsigned int ip)
{
    wprintf("%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
            (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

/* Print microseconds as milliseconds with three decimals. */
static void print_ms(unsigned int us)
{
    wprintf("%u.%03u ms", us / 1000, us % 1000);
}

static void delay_ms(unsigned int ms)
{
    unsigned int until = wuptime_ms() + ms;
    while (wuptime_ms() < until)
        wyield();
}

int main(int argc, char **argv)
{
    int         count = 4;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else
            target = argv[i];
    }

    if (!target) {
        wfprintf(W_STDERR, "usage: ping [-c count] <ip>\n");
        return 2;
    }

    unsigned int ip;
    if (parse_ip(target, &ip) < 0) {
        wfprintf(W_STDERR, "ping: %s: not a dotted-decimal address\n", target);
        return 2;
    }

    wprintf("PING ");
    print_ip(ip);
    wprintf(" 32 bytes of data.\n");

    int sent = 0, received = 0;

    for (int seq = 1; count == 0 || seq <= count; seq++) {
        int r = wping(ip, seq, 1000);
        sent++;

        if (r >= 0) {
            received++;
            wprintf("32 bytes from ");
            print_ip(ip);
            wprintf(": icmp_seq=%d time=", seq);
            print_ms((unsigned)r);
            wprintf("\n");
        } else if (-r == W_ENODEV) {
            wfprintf(W_STDERR, "ping: no network card\n");
            return 1;
        } else if (-r == W_EHOSTUNREACH) {
            wprintf("From ");
            print_ip(ip);
            wprintf(": host unreachable (no ARP reply)\n");
        } else {
            wprintf("Request timed out for icmp_seq=%d\n", seq);
        }

        if (count == 0 || seq < count)
            delay_ms(800);
    }

    wprintf("\n--- ");
    print_ip(ip);
    wprintf(" ping statistics ---\n");
    wprintf("%d packets transmitted, %d received, %d%% packet loss\n",
            sent, received,
            sent ? (sent - received) * 100 / sent : 0);

    return received > 0 ? 0 : 1;
}
