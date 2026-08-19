/* wifi -- find wireless networks and join one.
 *
 *   wifi                      what the adapter is doing now
 *   wifi scan                 list the networks in range
 *   wifi connect <name>       join an open network
 *   wifi connect <name> <key> join a protected one
 *   wifi connect <name> -     join a protected one, asking for the
 *                             passphrase without printing it
 *   wifi disconnect           leave the current network
 *
 * Scanning takes a few seconds and joining a protected network takes a few
 * more -- most of that is turning the passphrase into a key, which is
 * thousands of hashes by design, so that guessing passphrases is slow for
 * anyone who captures the handshake.
 */

#include <wkernel.h>

/* ------------------------------------------------------------------ *
 *  Printing
 * ------------------------------------------------------------------ */

static const char *security_name(unsigned char security)
{
    switch (security) {
    case W_WIFI_SECURITY_OPEN: return "open";
    case W_WIFI_SECURITY_WEP:  return "WEP";
    case W_WIFI_SECURITY_WPA:  return "WPA";
    case W_WIFI_SECURITY_WPA2: return "WPA2";
    case W_WIFI_SECURITY_WPA3: return "WPA3";
    default:                   return "?";
    }
}

static const char *state_name(int state)
{
    switch (state) {
    case W_WIFI_STATE_ABSENT:    return "no adapter";
    case W_WIFI_STATE_IDLE:      return "not connected";
    case W_WIFI_STATE_SCANNING:  return "scanning";
    case W_WIFI_STATE_JOINING:   return "joining";
    case W_WIFI_STATE_HANDSHAKE: return "authenticating";
    case W_WIFI_STATE_CONNECTED: return "connected";
    default:                     return "?";
    }
}

/* Signal strength as a bar, because a number in decibels means nothing to
 * most people and the comparison between two networks is the whole question.
 * Anything at or above -50 is as good as it gets; below -90 is unusable. */
static void print_signal(signed char dbm)
{
    int bars = (dbm + 90) / 10;

    if (bars < 0) bars = 0;
    if (bars > 4) bars = 4;

    for (int i = 0; i < 4; i++)
        wprintf("%s", i < bars ? "#" : ".");
    wprintf(" %4d dBm", dbm);
}

static void print_ip(unsigned int ip)
{
    wprintf("%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
            (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

static void print_mac(const unsigned char *mac)
{
    for (int i = 0; i < 6; i++)
        wprintf("%s%02x", i ? ":" : "", mac[i]);
}

/* ------------------------------------------------------------------ *
 *  What went wrong
 * ------------------------------------------------------------------ */

/* Say it in words.  The kernel keeps a sentence explaining the last failure,
 * which is almost always more use than the error number -- but the number is
 * there for the cases where nothing was recorded. */
static void report_failure(int err)
{
    wwifi_status_t st;

    if (wwifi_status(&st) == 0 && st.error[0]) {
        wfprintf(W_STDERR, "wifi: %s\n", st.error);
    } else {
        const char *why;

        switch (-err) {
        case W_EPERM:       why = "only root may change the network"; break;
        case W_ENODEV:      why = "this machine has no wireless adapter"; break;
        case W_ENOENT:      why = "no network of that name is in range"; break;
        case W_EINVAL:      why = "that passphrase is not usable"; break;
        case W_EACCES:      why = "the access point refused us"; break;
        case W_ENOTSUP:     why = "this network uses something WOS does not "
                                  "implement"; break;
        case W_ETIMEDOUT:   why = "the access point stopped answering"; break;
        default:            why = "the attempt failed"; break;
        }
        wfprintf(W_STDERR, "wifi: %s\n", why);
    }

    /* A wrong passphrase and an access point that simply went away look the
     * same from here, and saying so saves somebody retyping a passphrase
     * that was right all along. */
    if (-err == W_EACCES)
        wfprintf(W_STDERR,
                 "wifi: the handshake gives no reason -- a wrong passphrase "
                 "and a network that stopped answering look alike\n");
}

/* ------------------------------------------------------------------ *
 *  The commands
 * ------------------------------------------------------------------ */

static int cmd_status(void)
{
    wwifi_status_t st;

    if (wwifi_status(&st) < 0) {
        wfprintf(W_STDERR, "wifi: could not read the adapter's state\n");
        return 1;
    }

    if (st.state == W_WIFI_STATE_ABSENT) {
        wprintf("No wireless adapter.\n");
        return 1;
    }

    wprintf("State      %s\n", state_name(st.state));

    if (st.state == W_WIFI_STATE_CONNECTED) {
        wprintf("Network    %s (%s)\n", st.network.ssid,
                security_name(st.network.security));
        wprintf("Access pt  ");
        print_mac(st.network.bssid);
        wprintf("  channel %u\n", st.network.channel);
        wprintf("Signal     ");
        print_signal(st.network.signal_dbm);
        wprintf("\n");

        wprintf("Address    ");
        print_ip(st.ip);
        wprintf("  gateway ");
        print_ip(st.gateway);
        wprintf("\n");
        wprintf("Resolver   ");
        print_ip(st.dns);
        wprintf("%s\n", st.from_dhcp ? "  (from the network)"
                                     : "  (set by hand)");
    } else if (st.error[0]) {
        wprintf("Last error %s\n", st.error);
    }

    return 0;
}

static int cmd_scan(void)
{
    wnetwork_t found[W_WIFI_SCAN_MAX];

    wprintf("Scanning...\n");

    int n = wwifi_scan(found, W_WIFI_SCAN_MAX);

    if (n < 0) {
        report_failure(n);
        return 1;
    }
    if (n == 0) {
        wprintf("No networks found.\n");
        return 1;
    }

    wprintf("\n%-32s %-6s %-8s %s\n", "NETWORK", "CH", "SECURITY", "SIGNAL");

    for (int i = 0; i < n; i++) {
        wnetwork_t *net = &found[i];

        wprintf("%-32s %-6u %-8s ", net->ssid, net->channel,
                security_name(net->security));
        print_signal(net->signal_dbm);

        /* Flag the ones that cannot be joined, so nobody spends a minute
         * finding out the hard way. */
        if (net->security == W_WIFI_SECURITY_WEP ||
            net->security == W_WIFI_SECURITY_WPA)
            wprintf("  (not supported)");
        else if (net->security == W_WIFI_SECURITY_WPA2 &&
                 !(net->flags & W_WIFI_CCMP))
            wprintf("  (no usable cipher)");
        else if (net->security == W_WIFI_SECURITY_WPA3)
            wprintf("  (WPA3 only)");

        wprintf("\n");
    }

    wprintf("\n%d network%s.\n", n, n == 1 ? "" : "s");
    return 0;
}

/* Read a passphrase without showing it.  The console is put into raw mode so
 * that nothing is echoed, which is the only way to type one where somebody
 * might be looking at the screen. */
static int read_passphrase(char *out, int size)
{
    int at = 0;

    wprintf("Passphrase: ");
    wconsole_raw(W_CONSOLE_RAW);

    for (;;) {
        char c;

        if (wread(W_STDIN, &c, 1) != 1)
            break;

        if (c == '\n' || c == '\r')
            break;

        if (c == 8 || c == 127) {          /* backspace */
            if (at > 0)
                at--;
            continue;
        }

        if (c == 3) {                       /* interrupt */
            wconsole_raw(W_CONSOLE_CANONICAL);
            wprintf("\n");
            return -1;
        }

        if (at < size - 1)
            out[at++] = c;
    }

    out[at] = '\0';
    wconsole_raw(W_CONSOLE_CANONICAL);
    wprintf("\n");
    return at;
}

static int cmd_connect(const char *ssid, const char *password)
{
    char typed[W_WIFI_PASSPHRASE_MAX + 1];

    /* A single dash means "ask me", so a passphrase need never appear in a
     * command line, where it would sit in the shell's history. */
    if (password && password[0] == '-' && password[1] == '\0') {
        if (read_passphrase(typed, sizeof(typed)) < 0) {
            wfprintf(W_STDERR, "wifi: cancelled\n");
            return 1;
        }
        password = typed;
    }

    wprintf("Connecting to %s...\n", ssid);

    int r = wwifi_connect(ssid, password);

    /* Whatever happened, do not leave it in this program's memory. */
    for (unsigned i = 0; i < sizeof(typed); i++)
        typed[i] = 0;

    if (r < 0) {
        report_failure(r);
        return 1;
    }

    wprintf("\n");
    return cmd_status();
}

static int cmd_disconnect(void)
{
    int r = wwifi_disconnect();

    if (r < 0) {
        report_failure(r);
        return 1;
    }

    wprintf("Disconnected.\n");
    return 0;
}

static void usage(void)
{
    wfprintf(W_STDERR,
             "usage: wifi [status]\n"
             "       wifi scan\n"
             "       wifi connect <network> [passphrase | -]\n"
             "       wifi disconnect\n"
             "\n"
             "A passphrase of - is read from the terminal without being\n"
             "shown, so it stays out of the shell's history.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0)
        return cmd_status();

    if (strcmp(argv[1], "scan") == 0)
        return cmd_scan();

    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            wfprintf(W_STDERR, "wifi: connect needs a network name\n");
            return 2;
        }
        return cmd_connect(argv[2], argc > 3 ? argv[3] : NULL);
    }

    if (strcmp(argv[1], "disconnect") == 0)
        return cmd_disconnect();

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    wfprintf(W_STDERR, "wifi: no such command: %s\n", argv[1]);
    usage();
    return 2;
}
