/* Wireless networking: scanning, joining, and the WPA2 handshake.
 *
 * This sits between two things that know nothing of each other.  Below it is
 * a driver for one particular adapter, which can tune to a channel, send and
 * receive 802.11 frames, and hold an encryption key.  Above it is the IP
 * stack, which wants an Ethernet-shaped adapter it can send frames through.
 * Everything in between -- what a beacon means, how to ask an access point to
 * let us in, how a passphrase becomes a key, how an 802.11 frame becomes an
 * Ethernet one -- is here.
 *
 * The division is worth stating because it is not obvious how much of 802.11
 * lives in the adapter's own firmware.  Acknowledgements, retries, timing,
 * rate selection and the encryption of individual frames are all done by the
 * hardware, and the driver never sees them.  What the host must still do is
 * everything involving a decision: which networks exist, which one to join,
 * proving we know the passphrase, and what key the hardware should use once
 * that is proved.  That is what this file is.
 *
 * Joining a protected network runs like this:
 *
 *   scan          the adapter sweeps the channels and every beacon it hears
 *                 becomes an entry in a table, with the network's name, its
 *                 access point, its channel and what security it wants
 *   join          tune to the network's channel, then two exchanges with the
 *                 access point: authentication (a formality, for a network
 *                 that is not using the ancient shared-key scheme) and
 *                 association, which is where we say which cipher we intend
 *                 to use and are given an identifier to use from then on
 *   handshake     four EAPOL messages that prove both sides derived the same
 *                 key from the passphrase, without either sending it; the
 *                 access point also hands over the group key here
 *   configured    the key goes into the adapter, and the link carries data
 *
 * Nothing here is specific to any one adapter.  See iwlwifi.h for the one
 * this machine has.
 */
#ifndef WOS_WIFI_H
#define WOS_WIFI_H

#include "types.h"

/* An SSID is at most 32 bytes, and is not required to be text -- it is a byte
 * string, and may hold anything, including a zero.  It is stored here with a
 * terminator appended so it can be printed, and with its true length beside
 * it so it can be hashed correctly: the passphrase-to-key function takes the
 * name as a salt, and truncating a name at an embedded zero would derive the
 * wrong key. */
#define WIFI_SSID_MAX 32

/* How many networks a scan will remember.  A crowded place has more than
 * this; the weakest are dropped, which is the right thing to lose. */
#define WIFI_SCAN_MAX 48

/* The longest passphrase WPA2 allows.  A 64-character entry is not a
 * passphrase at all but the 256-bit key itself written in hex, which is
 * accepted too. */
#define WIFI_PASSPHRASE_MAX 64

typedef enum {
    WIFI_SECURITY_OPEN = 0,  /* no encryption at all                        */
    WIFI_SECURITY_WEP,       /* broken for twenty years; refused, not joined */
    WIFI_SECURITY_WPA,       /* the original WPA, TKIP                       */
    WIFI_SECURITY_WPA2,      /* RSN with CCMP -- what this implements        */
    WIFI_SECURITY_WPA3,      /* RSN with SAE; recognised, not implemented    */
} wifi_security_t;

typedef struct {
    char     ssid[WIFI_SSID_MAX + 1];
    uint8_t  ssid_len;
    uint8_t  bssid[6];        /* the access point's hardware address        */
    uint8_t  channel;
    int8_t   signal_dbm;      /* negative; closer to zero is stronger       */
    wifi_security_t security;
    bool     hidden;          /* beacons with the name suppressed           */

    /* Whether the network offers CCMP as its pairwise cipher.  A protected
     * network that does not is one this system cannot join, and knowing that
     * from the beacon means saying so before a passphrase is asked for
     * rather than after a handshake fails. */
    bool     ccmp;
} wifi_network_t;

typedef enum {
    WIFI_STATE_ABSENT = 0,    /* no wireless adapter in this machine        */
    WIFI_STATE_IDLE,          /* adapter up, not on any network             */
    WIFI_STATE_SCANNING,
    WIFI_STATE_JOINING,       /* authenticating and associating             */
    WIFI_STATE_HANDSHAKE,     /* proving the passphrase                     */
    WIFI_STATE_CONNECTED,
} wifi_state_t;

/* ------------------------------------------------------------------ *
 *  What the system uses
 * ------------------------------------------------------------------ */

/* Probe for a wireless adapter and start it.  Returns whether one was found.
 * Safe to call on a machine with none. */
bool wifi_init(void);

/* Whether there is an adapter at all -- the difference between "no networks
 * found" and "nothing to look with". */
bool wifi_present(void);

wifi_state_t wifi_state(void);

/* Sweep the channels and fill `out` with what was heard, strongest first.
 * Returns how many were written, or a negative error.
 *
 * Networks broadcasting the same name from several access points collapse to
 * the strongest of them: they are one network to anyone choosing which to
 * join, and listing a name five times helps nobody. */
int wifi_scan(wifi_network_t *out, int max, uint32_t timeout_ms);

/* The result of the most recent scan, without scanning again.  A connect by
 * name needs the channel and access point that go with it, and asking the
 * adapter to sweep every channel a second time to learn what it just heard
 * would add seconds to every connection. */
int wifi_scan_cached(wifi_network_t *out, int max);

/* Join a network.  `password` is NULL or empty for an open one, and either a
 * passphrase or 64 hex characters for a protected one.
 *
 * This runs the whole sequence and does not return until the link is up or
 * has failed.  On success the stack has been pointed at the wireless
 * adapter and given an address by DHCP.
 *
 * Returns 0, or a negative error:
 *   -W_ENODEV        no wireless adapter
 *   -W_ENOENT        no network of that name was found
 *   -W_EINVAL        a passphrase was needed and not given, or is malformed
 *   -W_EACCES        the access point rejected us -- in practice, wrong
 *                    passphrase; the handshake cannot tell us more than that
 *   -W_ENOTSUP       the network uses something not implemented here (WEP,
 *                    or WPA3's newer handshake)
 *   -W_ETIMEDOUT     the access point stopped answering partway through */
int wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/* Leave the current network, telling the access point rather than vanishing,
 * and hand the stack back to a wired adapter if there is one. */
int wifi_disconnect(void);

/* The network currently joined, or NULL.  Valid until the next state
 * change. */
const wifi_network_t *wifi_current(void);

/* A one-line reason the last connection attempt failed, for a command that
 * has to say something more useful than a number.  Empty if none has. */
const char *wifi_last_error(void);

/* ------------------------------------------------------------------ *
 *  What a driver provides
 * ------------------------------------------------------------------ */

/* The operations this layer needs from an adapter.  Everything works in whole
 * 802.11 frames: the driver moves them, and does not interpret them.
 *
 * A driver fills one of these in and calls wifi_attach().  Only one adapter
 * is supported at a time, which is one more than this machine has. */
typedef struct wifi_ops {
    /* Begin a scan across `channels` (an array of `count` channel numbers),
     * dwelling on each long enough to hear a beacon.  Results are not
     * returned here: the adapter passes every beacon and probe response it
     * hears to the receive path like any other frame, and this layer picks
     * them out.  Returns 0 or a negative error. */
    int (*scan_start)(void *priv, const uint8_t *channels, int count,
                      uint32_t dwell_ms);

    /* Whether the scan begun above has finished. */
    bool (*scan_done)(void *priv);

    /* Tune to one channel and stay there. */
    int (*set_channel)(void *priv, uint8_t channel);

    /* Tell the adapter which access point we are talking to.  Called once
     * before authentication with `associated` false, and again once the
     * association succeeds with it true and `aid` set -- the second call is
     * what makes the hardware start acknowledging frames as a member of the
     * network rather than a stranger on the channel. */
    int (*set_bss)(void *priv, const uint8_t bssid[6],
                   const uint8_t *ssid, uint32_t ssid_len,
                   bool associated, uint16_t aid);

    /* Send one 802.11 frame.  `encrypt` asks for the hardware to protect it
     * with the pairwise key, which is only meaningful once one is
     * installed; management frames during the handshake go out with it
     * false. */
    int (*tx)(void *priv, const void *frame, uint32_t len, bool encrypt);

    /* Take one received 802.11 frame if there is one, returning its length
     * or zero.  `signal_dbm` receives the strength it arrived at, which is
     * what makes a scan able to sort by it.  Never blocks.
     *
     * Frames the hardware decrypted arrive here already in the clear. */
    uint32_t (*rx)(void *priv, void *out, uint32_t cap, int8_t *signal_dbm);

    /* Install a key.  `pairwise` distinguishes the key used with the access
     * point from the group key used for broadcasts; `index` is the key slot
     * the access point named.  Passing a NULL key removes it, which is what
     * leaving a network does. */
    int (*set_key)(void *priv, int index, bool pairwise,
                   const uint8_t *key, uint32_t len);

    /* The adapter's own hardware address. */
    const uint8_t *(*mac)(void *priv);

    /* Called often while this layer waits, for a driver that has to service
     * its hardware to keep receiving.  May be NULL. */
    void (*poll)(void *priv);
} wifi_ops_t;

/* Register the adapter.  `priv` is handed back to every operation above. */
void wifi_attach(const wifi_ops_t *ops, void *priv);

#endif /* WOS_WIFI_H */
