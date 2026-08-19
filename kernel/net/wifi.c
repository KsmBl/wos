/* 802.11 scanning, joining, and the WPA2 four-way handshake.  See wifi.h. */

#include "wifi.h"
#include "netdev.h"
#include "net.h"
#include "crypto.h"
#include "iwlwifi.h"
#include "pit.h"
#include "smp.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* ------------------------------------------------------------------ *
 *  802.11 on the wire
 * ------------------------------------------------------------------ */

/* The frame control field, which says what a frame is.  Type and subtype
 * together name it; the flags below say which direction it is going and
 * whether its body is encrypted. */
#define FC_TYPE_MGMT   0
#define FC_TYPE_DATA   2

#define FC_SUB_ASSOC_REQ   0
#define FC_SUB_ASSOC_RESP  1
#define FC_SUB_PROBE_REQ   4
#define FC_SUB_PROBE_RESP  5
#define FC_SUB_BEACON      8
#define FC_SUB_DISASSOC   10
#define FC_SUB_AUTH       11
#define FC_SUB_DEAUTH     12

#define FC_SUB_DATA        0
#define FC_SUB_QOS_DATA    8

#define FC_TO_DS      0x0100
#define FC_FROM_DS    0x0200
#define FC_PROTECTED  0x4000

#define FC_TYPE(fc)    (((fc) >> 2) & 3)
#define FC_SUBTYPE(fc) (((fc) >> 4) & 15)
#define FC_MAKE(type, sub) ((uint16_t)(((type) << 2) | ((sub) << 4)))

struct __attribute__((packed)) ieee80211_hdr {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  addr1[6];      /* receiver  */
    uint8_t  addr2[6];      /* sender    */
    uint8_t  addr3[6];      /* the third depends on direction; see below */
    uint16_t seq_ctrl;
};

/* Information elements: the tagged, variable-length fields that carry
 * everything interesting in a management frame. */
#define IE_SSID          0
#define IE_RATES         1
#define IE_DS_PARAMS     3
#define IE_RSN          48
#define IE_EXT_RATES    50
#define IE_VENDOR      221

/* Cipher and authentication suites, as they appear inside an RSN element.
 * Each is a three-byte organisation identifier and a one-byte type; the
 * identifier below is the one the standard itself uses. */
#define RSN_OUI_0 0x00
#define RSN_OUI_1 0x0F
#define RSN_OUI_2 0xAC

#define RSN_CIPHER_WEP40  1
#define RSN_CIPHER_TKIP   2
#define RSN_CIPHER_CCMP   4
#define RSN_CIPHER_WEP104 5

#define RSN_AKM_8021X 1
#define RSN_AKM_PSK   2
#define RSN_AKM_SAE   8      /* WPA3 */

/* The eight bytes that stand between an 802.11 frame body and the Ethernet
 * payload inside it: a logical link control header saying "what follows is
 * identified the way Ethernet identifies it", then the Ethernet type. */
static const uint8_t llc_snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };

#define ETH_P_EAPOL 0x888E

/* ------------------------------------------------------------------ *
 *  EAPOL-Key, the handshake's one message format
 * ------------------------------------------------------------------ */

struct __attribute__((packed)) eapol_hdr {
    uint8_t  version;
    uint8_t  type;             /* 3 = key */
    uint16_t length;           /* of everything after this field */
};

struct __attribute__((packed)) eapol_key {
    uint8_t  descriptor_type;  /* 2 = RSN */
    uint16_t key_info;
    uint16_t key_length;
    uint8_t  replay_counter[8];
    uint8_t  key_nonce[32];
    uint8_t  key_iv[16];
    uint8_t  key_rsc[8];
    uint8_t  key_id[8];
    uint8_t  key_mic[16];
    uint16_t key_data_length;
    /* key_data follows */
};

#define KEY_INFO_VERSION  0x0007  /* which MIC and key-wrap to use          */
#define KEY_INFO_PAIRWISE 0x0008
#define KEY_INFO_INSTALL  0x0040
#define KEY_INFO_ACK      0x0080
#define KEY_INFO_MIC      0x0100
#define KEY_INFO_SECURE   0x0200
#define KEY_INFO_ERROR    0x0400
#define KEY_INFO_REQUEST  0x0800
#define KEY_INFO_ENCRYPTED 0x1000

/* Version 2 is HMAC-SHA1 for the integrity code and AES key wrap for the
 * group key -- what WPA2 with CCMP uses, and the only one implemented. */
#define KEY_VERSION_SHA1_AES 2

/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */

/* The channels a scan visits.  The 2.4 GHz band first because it is where
 * most networks still are and the sweep can stop early if what was wanted is
 * found; then the 5 GHz channels this adapter's regulatory domain is likely
 * to allow.  Passive channels the hardware refuses are skipped by the
 * hardware, not here. */
static const uint8_t scan_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
    149, 153, 157, 161, 165
};

static const wifi_ops_t *ops;
static void             *ops_priv;

static wifi_state_t   state = WIFI_STATE_ABSENT;
static wifi_network_t scan_table[WIFI_SCAN_MAX];
static int            scan_count;

static wifi_network_t current_network;
static bool           have_current;
static uint16_t       current_aid;
static uint8_t        our_mac[6];
static char           last_error[96];

/* The session keys, valid while connected.  The confirmation key signs
 * handshake messages, the encryption key unwraps the group key, and the
 * temporal key is what the hardware encrypts data with. */
static struct {
    bool    valid;
    uint8_t kck[16];
    uint8_t kek[16];
    uint8_t tk[16];
    uint8_t pmk[32];
} keys;

static uint16_t tx_sequence;

static void fail(const char *why)
{
    strlcpy(last_error, why, sizeof(last_error));
}

/* ------------------------------------------------------------------ *
 *  Small helpers
 * ------------------------------------------------------------------ */

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Fill a buffer with bytes an observer cannot predict.
 *
 * This is the weakest thing in this file and it is worth being honest about
 * it.  There is no entropy pool in this kernel, so the source here is the
 * timestamp counter, stirred with the adapter's address and hashed.  A nonce
 * only has to be unique per handshake rather than secret, and the counter
 * gives that -- but it is not what a system with a real random source would
 * use, and if one is ever added this is the first place it should go. */
static void random_bytes(uint8_t *out, uint32_t len)
{
    static uint64_t counter;
    uint8_t seed[SHA1_DIGEST_LEN + 8 + 6];
    uint8_t digest[SHA1_DIGEST_LEN];
    uint32_t done = 0;

    /* Zeroed rather than left as whatever was on the stack.  Uninitialised
     * memory is not a source of randomness -- it is undefined behaviour, and
     * a compiler is entitled to do something surprising with a read of it.
     * The variation here comes from the counter below. */
    memset(digest, 0, sizeof(digest));

    while (done < len) {
        uint64_t now = rdtsc() ^ (counter++ * 0x9E3779B97F4A7C15UL);
        uint32_t take = len - done;

        memcpy(seed, digest, SHA1_DIGEST_LEN);          /* chain            */
        memcpy(seed + SHA1_DIGEST_LEN, &now, 8);
        memcpy(seed + SHA1_DIGEST_LEN + 8, our_mac, 6);

        sha1(seed, sizeof(seed), digest);

        if (take > SHA1_DIGEST_LEN)
            take = SHA1_DIGEST_LEN;
        memcpy(out + done, digest, take);
        done += take;
    }
}

/* Let the driver run, and let the timer tick.  Every wait in this file goes
 * through here: these functions are called from a syscall, entered with
 * interrupts off, and a millisecond deadline never expires if the timer
 * cannot interrupt. */
static void wifi_poll(void)
{
    sti();

    /* Let the other processors in.  A wireless handshake waits for seconds at
     * a time, and the kernel lock is held for all of it unless it is given up
     * deliberately -- see klock_pause. */
    klock_pause();

    if (ops && ops->poll)
        ops->poll(ops_priv);
}

static void wifi_wait(uint32_t ms)
{
    uint64_t deadline = time_now_ms() + ms;

    while (time_now_ms() < deadline) {
        wifi_poll();
        io_wait();
    }
}

/* ------------------------------------------------------------------ *
 *  Information elements
 * ------------------------------------------------------------------ */

/* Find one element in a frame body.  Returns its value and length, or NULL.
 * The walk is bounded by the frame it was given: these come from the air, and
 * an element claiming to be longer than the frame holding it is the first
 * thing a hostile beacon would try. */
static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t id,
                              uint8_t *out_len)
{
    uint32_t at = 0;

    while (at + 2 <= len) {
        uint8_t ie_id  = ies[at];
        uint8_t ie_len = ies[at + 1];

        if (at + 2 + ie_len > len)
            break;

        if (ie_id == id) {
            *out_len = ie_len;
            return ies + at + 2;
        }
        at += 2 + ie_len;
    }

    *out_len = 0;
    return NULL;
}

/* Work out what security a network is using from its elements.
 *
 * An RSN element means WPA2 or WPA3, told apart by whether the authentication
 * suite is the pre-shared key one or the newer simultaneous-authentication
 * one.  A vendor element carrying Microsoft's identifier and type 1 is the
 * original WPA.  Neither, with the privacy bit set in the capability field,
 * means WEP.  Neither, without it, means the network is open. */
static wifi_security_t read_security(const uint8_t *ies, uint32_t len,
                                     uint16_t capability)
{
    uint8_t        ie_len;
    const uint8_t *rsn = find_ie(ies, len, IE_RSN, &ie_len);

    if (rsn && ie_len >= 8) {
        uint32_t at = 2 + 4;            /* version, then the group cipher */

        if (at + 2 > ie_len)
            return WIFI_SECURITY_WPA2;

        uint16_t pairwise = get_le16(rsn + at);
        at += 2 + (uint32_t)pairwise * 4;

        if (at + 2 > ie_len)
            return WIFI_SECURITY_WPA2;

        uint16_t akm_count = get_le16(rsn + at);
        at += 2;

        for (uint16_t i = 0; i < akm_count && at + 4 <= ie_len; i++, at += 4) {
            if (rsn[at] == RSN_OUI_0 && rsn[at + 1] == RSN_OUI_1 &&
                rsn[at + 2] == RSN_OUI_2 && rsn[at + 3] == RSN_AKM_SAE)
                return WIFI_SECURITY_WPA3;
        }
        return WIFI_SECURITY_WPA2;
    }

    /* A vendor element is only WPA if it carries the right identifier; the
     * same element number is used by everyone for everything. */
    uint32_t at = 0;
    while (at + 2 <= len) {
        uint8_t id = ies[at], vlen = ies[at + 1];

        if (at + 2 + vlen > len)
            break;
        if (id == IE_VENDOR && vlen >= 4) {
            const uint8_t *v = ies + at + 2;

            if (v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01)
                return WIFI_SECURITY_WPA;
        }
        at += 2 + vlen;
    }

    return (capability & 0x0010) ? WIFI_SECURITY_WEP : WIFI_SECURITY_OPEN;
}

/* Whether a network's RSN element offers CCMP as a pairwise cipher.  A
 * network that only offers TKIP cannot be joined by this implementation, and
 * saying so is better than failing in the handshake. */
static bool rsn_offers_ccmp(const uint8_t *ies, uint32_t len)
{
    uint8_t        ie_len;
    const uint8_t *rsn = find_ie(ies, len, IE_RSN, &ie_len);

    if (!rsn || ie_len < 8)
        return false;

    uint32_t at = 2 + 4;

    if (at + 2 > ie_len)
        return false;

    uint16_t count = get_le16(rsn + at);
    at += 2;

    for (uint16_t i = 0; i < count && at + 4 <= ie_len; i++, at += 4)
        if (rsn[at] == RSN_OUI_0 && rsn[at + 1] == RSN_OUI_1 &&
            rsn[at + 2] == RSN_OUI_2 && rsn[at + 3] == RSN_CIPHER_CCMP)
            return true;

    return false;
}

/* ------------------------------------------------------------------ *
 *  The scan table
 * ------------------------------------------------------------------ */

static void scan_reset(void)
{
    scan_count = 0;
    memset(scan_table, 0, sizeof(scan_table));
}

/* Record a network, or update what is already recorded about it.
 *
 * Several access points commonly broadcast one network's name, and to anyone
 * choosing what to join they are one entry.  So the table is keyed by name,
 * and an entry keeps the details of the strongest access point offering it --
 * which is the one worth joining. */
static void scan_record(const wifi_network_t *seen)
{
    for (int i = 0; i < scan_count; i++) {
        wifi_network_t *e = &scan_table[i];

        if (e->ssid_len == seen->ssid_len &&
            memcmp(e->ssid, seen->ssid, seen->ssid_len) == 0) {
            if (seen->signal_dbm > e->signal_dbm)
                *e = *seen;
            return;
        }
    }

    if (scan_count < WIFI_SCAN_MAX) {
        scan_table[scan_count++] = *seen;
        return;
    }

    /* Full.  Replace the weakest entry, but only if this one beats it --
     * losing the faintest network is the least harmful thing to lose. */
    int weakest = 0;

    for (int i = 1; i < scan_count; i++)
        if (scan_table[i].signal_dbm < scan_table[weakest].signal_dbm)
            weakest = i;

    if (seen->signal_dbm > scan_table[weakest].signal_dbm)
        scan_table[weakest] = *seen;
}

/* Turn a beacon or probe response into a table entry. */
static void scan_beacon(const uint8_t *frame, uint32_t len, int8_t signal)
{
    const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)frame;
    uint32_t at = sizeof(*hdr);

    /* timestamp, beacon interval, capability -- then the elements */
    if (len < at + 12)
        return;

    uint16_t capability = get_le16(frame + at + 10);
    const uint8_t *ies = frame + at + 12;
    uint32_t ies_len = len - at - 12;

    wifi_network_t net;
    memset(&net, 0, sizeof(net));

    memcpy(net.bssid, hdr->addr3, 6);
    net.signal_dbm = signal;
    net.security   = read_security(ies, ies_len, capability);
    net.ccmp       = rsn_offers_ccmp(ies, ies_len);

    uint8_t        ie_len;
    const uint8_t *ssid = find_ie(ies, ies_len, IE_SSID, &ie_len);

    if (ssid && ie_len > 0 && ie_len <= WIFI_SSID_MAX) {
        /* An access point that hides its name still beacons, with the name
         * blanked -- either zero-length or all zeroes.  There is nothing to
         * be done with such an entry except show that something is there. */
        bool blank = true;

        for (uint8_t i = 0; i < ie_len; i++)
            if (ssid[i] != 0) {
                blank = false;
                break;
            }

        if (blank) {
            net.hidden = true;
        } else {
            memcpy(net.ssid, ssid, ie_len);
            net.ssid_len = ie_len;
        }
    } else {
        net.hidden = true;
    }

    if (net.hidden) {
        /* A hidden network has no name to be keyed by, and every one of them
         * would otherwise collapse into every other.  Naming each by its
         * access point keeps them apart -- and there is nothing better to
         * show, since the name is exactly what is being withheld. */
        static const char hex[] = "0123456789abcdef";
        char *p = net.ssid;

        for (const char *s = "(hidden "; *s; s++)
            *p++ = *s;
        for (int i = 3; i < 6; i++) {
            if (i > 3)
                *p++ = ':';
            *p++ = hex[net.bssid[i] >> 4];
            *p++ = hex[net.bssid[i] & 0xF];
        }
        *p++ = ')';
        *p = '\0';

        net.ssid_len = (uint8_t)(p - net.ssid);
    }

    const uint8_t *ds = find_ie(ies, ies_len, IE_DS_PARAMS, &ie_len);

    if (ds && ie_len >= 1)
        net.channel = ds[0];

    scan_record(&net);
}

/* Sort strongest first.  The table is tens of entries at most, so the
 * simplest sort there is costs nothing worth measuring. */
static void scan_sort(void)
{
    for (int i = 1; i < scan_count; i++) {
        wifi_network_t key = scan_table[i];
        int j = i - 1;

        while (j >= 0 && scan_table[j].signal_dbm < key.signal_dbm) {
            scan_table[j + 1] = scan_table[j];
            j--;
        }
        scan_table[j + 1] = key;
    }
}

/* ------------------------------------------------------------------ *
 *  Receiving
 * ------------------------------------------------------------------ */

/* Frames that arrive while we are waiting for a particular one.  The waiting
 * code says what it wants; everything else is either filed away (beacons
 * during a scan) or dropped.
 *
 * Data frames are the exception: once connected they are pulled by the IP
 * stack through the netdev, not here. */
struct rx_want {
    uint8_t  type;
    uint8_t  subtype;
    bool     got;
    uint8_t  frame[NETDEV_MTU_FRAME];
    uint32_t len;
};

/* Take one frame from the adapter and dispose of it.  Returns true if it was
 * the one being waited for. */
static bool rx_dispatch(struct rx_want *want)
{
    uint8_t  buf[NETDEV_MTU_FRAME];
    int8_t   signal = -100;
    uint32_t len;

    if (!ops || !ops->rx)
        return false;

    len = ops->rx(ops_priv, buf, sizeof(buf), &signal);
    if (len < sizeof(struct ieee80211_hdr))
        return false;

    const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)buf;
    uint16_t fc      = hdr->frame_control;
    uint8_t  type    = (uint8_t)FC_TYPE(fc);
    uint8_t  subtype = (uint8_t)FC_SUBTYPE(fc);

    if (type == FC_TYPE_MGMT &&
        (subtype == FC_SUB_BEACON || subtype == FC_SUB_PROBE_RESP)) {
        scan_beacon(buf, len, signal);
        /* A beacon is never what anything waits for, but during a scan it is
         * the whole point, so it is filed and not treated as a miss. */
        return false;
    }

    /* Being thrown off the network can happen at any moment, and pretending
     * otherwise leaves everything above waiting for frames that will never
     * come. */
    if (type == FC_TYPE_MGMT &&
        (subtype == FC_SUB_DEAUTH || subtype == FC_SUB_DISASSOC)) {
        if (have_current && memcmp(hdr->addr2, current_network.bssid, 6) == 0) {
            uint16_t reason = 0;

            if (len >= sizeof(*hdr) + 2)
                reason = get_le16(buf + sizeof(*hdr));

            kprintf("wifi   : the access point disconnected us (reason %u)\n",
                    reason);
            state = WIFI_STATE_IDLE;
            keys.valid = false;
            have_current = false;
        }
        return false;
    }

    if (want && !want->got && type == want->type && subtype == want->subtype) {
        want->len = len < sizeof(want->frame) ? len : sizeof(want->frame);
        memcpy(want->frame, buf, want->len);
        want->got = true;
        return true;
    }

    return false;
}

/* Wait for one particular management frame.  Returns false on timeout. */
static bool rx_wait(struct rx_want *want, uint32_t timeout_ms)
{
    uint64_t deadline = time_now_ms() + timeout_ms;

    want->got = false;

    while (time_now_ms() < deadline) {
        wifi_poll();
        if (rx_dispatch(want))
            return true;
        io_wait();
    }
    return false;
}

/* ------------------------------------------------------------------ *
 *  Sending management frames
 * ------------------------------------------------------------------ */

/* Fill in the header common to every management frame we send: to the access
 * point, from us, about its network. */
static uint32_t mgmt_header(uint8_t *frame, uint8_t subtype,
                            const uint8_t *bssid)
{
    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)frame;

    memset(hdr, 0, sizeof(*hdr));
    hdr->frame_control = FC_MAKE(FC_TYPE_MGMT, subtype);
    memcpy(hdr->addr1, bssid, 6);
    memcpy(hdr->addr2, our_mac, 6);
    memcpy(hdr->addr3, bssid, 6);
    hdr->seq_ctrl = (uint16_t)(tx_sequence++ << 4);

    return sizeof(*hdr);
}

/* The rate elements every association request carries.  What is advertised
 * here barely matters for a modern access point -- it will pick a rate from
 * its own capabilities -- but an association request without them is
 * malformed and gets refused. */
static uint32_t put_rate_ies(uint8_t *out)
{
    static const uint8_t basic[] = {
        0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24
    };
    static const uint8_t extended[] = { 0x30, 0x48, 0x60, 0x6C };
    uint32_t at = 0;

    out[at++] = IE_RATES;
    out[at++] = sizeof(basic);
    memcpy(out + at, basic, sizeof(basic));
    at += sizeof(basic);

    out[at++] = IE_EXT_RATES;
    out[at++] = sizeof(extended);
    memcpy(out + at, extended, sizeof(extended));
    at += sizeof(extended);

    return at;
}

/* The RSN element we send in the association request: version 1, CCMP for
 * both the group and pairwise cipher, and the pre-shared key authentication
 * suite.  This same sequence of bytes goes into the second handshake message,
 * where the access point checks it against what it sees here -- so it is
 * built once and kept. */
static uint8_t  our_rsn_ie[32];
static uint32_t our_rsn_ie_len;

static void build_rsn_ie(void)
{
    uint8_t *p = our_rsn_ie;
    uint32_t at = 0;

    p[at++] = IE_RSN;
    p[at++] = 20;                       /* filled in as a constant length */

    put_le16(p + at, 1); at += 2;       /* version */

    p[at++] = RSN_OUI_0; p[at++] = RSN_OUI_1;
    p[at++] = RSN_OUI_2; p[at++] = RSN_CIPHER_CCMP;   /* group cipher */

    put_le16(p + at, 1); at += 2;       /* one pairwise cipher */
    p[at++] = RSN_OUI_0; p[at++] = RSN_OUI_1;
    p[at++] = RSN_OUI_2; p[at++] = RSN_CIPHER_CCMP;

    put_le16(p + at, 1); at += 2;       /* one authentication suite */
    p[at++] = RSN_OUI_0; p[at++] = RSN_OUI_1;
    p[at++] = RSN_OUI_2; p[at++] = RSN_AKM_PSK;

    put_le16(p + at, 0); at += 2;       /* no optional capabilities */

    our_rsn_ie_len = at;
}

/* Open-system authentication: a formality that predates WPA and is still
 * required before an association will be accepted.  Two frames, no secrets. */
static int do_authenticate(const wifi_network_t *net, uint32_t timeout_ms)
{
    uint8_t  frame[64];
    uint32_t at = mgmt_header(frame, FC_SUB_AUTH, net->bssid);

    put_le16(frame + at, 0); at += 2;   /* algorithm: open system */
    put_le16(frame + at, 1); at += 2;   /* sequence number one    */
    put_le16(frame + at, 0); at += 2;   /* status, zero from us   */

    struct rx_want want = { FC_TYPE_MGMT, FC_SUB_AUTH, false, { 0 }, 0 };

    /* Three attempts: the first frames after tuning to a channel are the
     * ones most likely to be lost, and an access point that missed the
     * request simply says nothing. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ops->tx(ops_priv, frame, at, false) < 0) {
            fail("the adapter would not send the authentication request");
            return -W_EIO;
        }
        if (rx_wait(&want, timeout_ms / 3))
            break;
    }

    if (!want.got) {
        fail("the access point did not answer the authentication request");
        return -W_ETIMEDOUT;
    }

    if (want.len < sizeof(struct ieee80211_hdr) + 6) {
        fail("the access point sent a malformed authentication reply");
        return -W_EIO;
    }

    uint16_t status = get_le16(want.frame + sizeof(struct ieee80211_hdr) + 4);

    if (status != 0) {
        kprintf("wifi   : authentication refused, status %u\n", status);
        fail("the access point refused to authenticate us");
        return -W_EACCES;
    }

    return 0;
}

/* Association: we say who we are and what we can do, and are given an
 * identifier to use for the rest of the session. */
static int do_associate(const wifi_network_t *net, bool protected_network,
                        uint32_t timeout_ms)
{
    uint8_t  frame[256];
    uint32_t at = mgmt_header(frame, FC_SUB_ASSOC_REQ, net->bssid);

    /* Capability: an infrastructure station, and privacy when the network
     * uses it.  Listen interval of ten beacons -- we never sleep, so this is
     * only a promise about how long the access point should buffer for us. */
    put_le16(frame + at, protected_network ? 0x0011 : 0x0001); at += 2;
    put_le16(frame + at, 10); at += 2;

    frame[at++] = IE_SSID;
    frame[at++] = net->ssid_len;
    memcpy(frame + at, net->ssid, net->ssid_len);
    at += net->ssid_len;

    at += put_rate_ies(frame + at);

    if (protected_network) {
        memcpy(frame + at, our_rsn_ie, our_rsn_ie_len);
        at += our_rsn_ie_len;
    }

    struct rx_want want = { FC_TYPE_MGMT, FC_SUB_ASSOC_RESP, false, { 0 }, 0 };

    for (int attempt = 0; attempt < 3; attempt++) {
        if (ops->tx(ops_priv, frame, at, false) < 0) {
            fail("the adapter would not send the association request");
            return -W_EIO;
        }
        if (rx_wait(&want, timeout_ms / 3))
            break;
    }

    if (!want.got) {
        fail("the access point did not answer the association request");
        return -W_ETIMEDOUT;
    }

    if (want.len < sizeof(struct ieee80211_hdr) + 6) {
        fail("the access point sent a malformed association reply");
        return -W_EIO;
    }

    const uint8_t *body = want.frame + sizeof(struct ieee80211_hdr);
    uint16_t status = get_le16(body + 2);

    if (status != 0) {
        kprintf("wifi   : association refused, status %u\n", status);
        fail("the access point refused to associate us");
        return -W_EACCES;
    }

    /* The two high bits of the identifier are always set and are not part of
     * the number. */
    current_aid = (uint16_t)(get_le16(body + 4) & 0x3FFF);

    return 0;
}

/* ------------------------------------------------------------------ *
 *  The four-way handshake
 * ------------------------------------------------------------------ */

/* Derive the session keys.
 *
 * Both sides already hold the master key -- it comes from the passphrase and
 * the network's name, and neither side sends it.  What they exchange are two
 * nonces, and the session keys come from hashing the master key together with
 * both addresses and both nonces.
 *
 * The addresses and nonces go in sorted order, smaller first.  That is what
 * lets both ends compute the same thing without agreeing who is who. */
static void derive_ptk(const uint8_t *anonce, const uint8_t *snonce,
                       const uint8_t *ap_mac)
{
    uint8_t data[6 + 6 + 32 + 32];
    uint8_t ptk[48];
    uint32_t at = 0;

    if (memcmp(our_mac, ap_mac, 6) < 0) {
        memcpy(data + at, our_mac, 6); at += 6;
        memcpy(data + at, ap_mac, 6);  at += 6;
    } else {
        memcpy(data + at, ap_mac, 6);  at += 6;
        memcpy(data + at, our_mac, 6); at += 6;
    }

    if (memcmp(snonce, anonce, 32) < 0) {
        memcpy(data + at, snonce, 32); at += 32;
        memcpy(data + at, anonce, 32); at += 32;
    } else {
        memcpy(data + at, anonce, 32); at += 32;
        memcpy(data + at, snonce, 32); at += 32;
    }

    sha1_prf(keys.pmk, sizeof(keys.pmk), "Pairwise key expansion",
             data, at, ptk, sizeof(ptk));

    memcpy(keys.kck, ptk, 16);
    memcpy(keys.kek, ptk + 16, 16);
    memcpy(keys.tk,  ptk + 32, 16);
    keys.valid = true;
}

/* Send an EAPOL-Key frame: an 802.11 data frame to the access point, carrying
 * a link-control header, the EAPOL header, and the key message.
 *
 * `mic` says whether to sign it, which every message from us except the
 * first does.  The signature covers the whole EAPOL body with the signature
 * field itself zeroed -- so it is computed after the frame is otherwise
 * complete, and written into the hole left for it. */
static int send_eapol(const uint8_t *bssid, uint16_t key_info,
                      const uint8_t *replay_counter, const uint8_t *nonce,
                      const uint8_t *key_data, uint16_t key_data_len,
                      bool mic)
{
    uint8_t  frame[512];
    uint32_t at = 0;

    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)frame;

    memset(hdr, 0, sizeof(*hdr));
    hdr->frame_control = (uint16_t)(FC_MAKE(FC_TYPE_DATA, FC_SUB_DATA) | FC_TO_DS);
    memcpy(hdr->addr1, bssid, 6);      /* to the access point   */
    memcpy(hdr->addr2, our_mac, 6);    /* from us               */
    memcpy(hdr->addr3, bssid, 6);      /* ultimately for it too */
    hdr->seq_ctrl = (uint16_t)(tx_sequence++ << 4);
    at = sizeof(*hdr);

    memcpy(frame + at, llc_snap, sizeof(llc_snap));
    at += sizeof(llc_snap);
    put_be16(frame + at, ETH_P_EAPOL);
    at += 2;

    struct eapol_hdr *eh = (struct eapol_hdr *)(frame + at);
    uint32_t eapol_start = at;

    eh->version = 2;
    eh->type    = 3;                   /* a key message */
    at += sizeof(*eh);

    struct eapol_key *key = (struct eapol_key *)(frame + at);

    memset(key, 0, sizeof(*key));
    key->descriptor_type = 2;                       /* RSN */
    at += sizeof(*key);

    /* Every multi-byte field in an EAPOL-Key message is big-endian, unlike
     * everything else in 802.11, which is little-endian.  Writing them by
     * hand rather than through the struct keeps that visible. */
    put_be16((uint8_t *)&key->key_info, key_info);
    put_be16((uint8_t *)&key->key_length, 16);      /* a CCMP key     */

    if (replay_counter)
        memcpy(key->replay_counter, replay_counter, 8);
    if (nonce)
        memcpy(key->key_nonce, nonce, 32);

    if (key_data_len) {
        memcpy(frame + at, key_data, key_data_len);
        at += key_data_len;
    }
    put_be16((uint8_t *)&key->key_data_length, key_data_len);

    /* The length in the EAPOL header covers everything after it. */
    put_be16((uint8_t *)&eh->length,
             (uint16_t)(at - eapol_start - sizeof(*eh)));

    if (mic) {
        uint8_t computed[SHA1_DIGEST_LEN];

        memset(key->key_mic, 0, sizeof(key->key_mic));
        hmac_sha1(keys.kck, sizeof(keys.kck),
                  frame + eapol_start, at - eapol_start, computed);
        memcpy(key->key_mic, computed, 16);
    }

    return ops->tx(ops_priv, frame, at, false);
}

/* Wait for an EAPOL-Key message from the access point.
 *
 * These arrive as data frames rather than management ones, so they do not go
 * through rx_wait: this pulls frames itself and picks out the ones carrying
 * EAPOL.  Anything else that arrives during the handshake is dropped -- there
 * is no link to carry it yet. */
static int recv_eapol(const uint8_t *bssid, uint8_t *out, uint32_t cap,
                      uint32_t timeout_ms)
{
    uint64_t deadline = time_now_ms() + timeout_ms;

    while (time_now_ms() < deadline) {
        uint8_t  buf[NETDEV_MTU_FRAME];
        int8_t   signal;
        uint32_t len;

        wifi_poll();

        len = ops->rx(ops_priv, buf, sizeof(buf), &signal);
        if (len < sizeof(struct ieee80211_hdr) + sizeof(llc_snap) + 2) {
            io_wait();
            continue;
        }

        const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)buf;
        uint16_t fc = hdr->frame_control;

        if (FC_TYPE(fc) != FC_TYPE_DATA)
            continue;
        if (memcmp(hdr->addr2, bssid, 6) != 0)
            continue;

        /* A QoS data frame carries two extra bytes of control between the
         * header and the body. */
        uint32_t at = sizeof(*hdr);

        if (FC_SUBTYPE(fc) == FC_SUB_QOS_DATA)
            at += 2;

        if (len < at + sizeof(llc_snap) + 2)
            continue;
        if (memcmp(buf + at, llc_snap, sizeof(llc_snap)) != 0)
            continue;
        at += sizeof(llc_snap);

        if (get_be16(buf + at) != ETH_P_EAPOL)
            continue;
        at += 2;

        uint32_t body = len - at;

        if (body > cap)
            body = cap;
        memcpy(out, buf + at, body);
        return (int)body;
    }

    return -W_ETIMEDOUT;
}

/* Pull the group key out of the third message.
 *
 * Its key data is encrypted with the encryption key just derived, and inside
 * is a sequence of tagged blocks; the one carrying the standard's identifier
 * and type 1 holds the group key, after two bytes saying which slot it goes
 * in. */
static bool extract_gtk(const uint8_t *key_data, uint16_t len,
                        uint8_t *gtk, uint32_t *gtk_len, int *gtk_index)
{
    uint8_t  plain[256];
    uint32_t plain_len;

    if (len < 16 || len > sizeof(plain) + 8 || (len % 8) != 0)
        return false;

    if (!aes_unwrap_key(keys.kek, sizeof(keys.kek), key_data, len, plain))
        return false;
    plain_len = len - 8;

    uint32_t at = 0;

    while (at + 2 <= plain_len) {
        uint8_t id  = plain[at];
        uint8_t ilen = plain[at + 1];

        if (id == 0xDD && at + 2 + ilen <= plain_len && ilen >= 6) {
            const uint8_t *v = plain + at + 2;

            if (v[0] == RSN_OUI_0 && v[1] == RSN_OUI_1 &&
                v[2] == RSN_OUI_2 && v[3] == 1) {
                uint32_t klen = ilen - 6;

                if (klen > 32)
                    klen = 32;
                *gtk_index = v[4] & 0x03;
                memcpy(gtk, v + 6, klen);
                *gtk_len = klen;
                return true;
            }
        }

        if (id == 0)
            break;                       /* padding: the block is over */
        at += 2 + ilen;
    }

    return false;
}

/* Run the whole handshake.  Four messages, two of them ours. */
static int do_handshake(const wifi_network_t *net, uint32_t timeout_ms)
{
    uint8_t body[512];
    uint8_t snonce[32];
    int     len;

    state = WIFI_STATE_HANDSHAKE;

    /* ---- message one: the access point's nonce ---- */
    len = recv_eapol(net->bssid, body, sizeof(body), timeout_ms);
    if (len < (int)sizeof(struct eapol_hdr) + (int)sizeof(struct eapol_key)) {
        fail("the access point never started the key handshake");
        return -W_ETIMEDOUT;
    }

    struct eapol_key *m1 =
        (struct eapol_key *)(body + sizeof(struct eapol_hdr));
    uint16_t info = get_be16((const uint8_t *)&m1->key_info);

    if ((info & KEY_INFO_VERSION) != KEY_VERSION_SHA1_AES) {
        kprintf("wifi   : the network wants key scheme %u, "
                "which is not implemented\n", info & KEY_INFO_VERSION);
        fail("the network uses a key scheme this system does not implement");
        return -W_ENOTSUP;
    }

    random_bytes(snonce, sizeof(snonce));
    derive_ptk(m1->key_nonce, snonce, net->bssid);

    /* ---- message two: our nonce, signed, with what we asked to use ---- */
    if (send_eapol(net->bssid,
                   KEY_VERSION_SHA1_AES | KEY_INFO_PAIRWISE | KEY_INFO_MIC,
                   m1->replay_counter, snonce,
                   our_rsn_ie, (uint16_t)our_rsn_ie_len, true) < 0) {
        fail("the adapter would not send the second handshake message");
        return -W_EIO;
    }

    /* ---- message three: the group key, and proof they know the key too ---- */
    len = recv_eapol(net->bssid, body, sizeof(body), timeout_ms);
    if (len < (int)sizeof(struct eapol_hdr) + (int)sizeof(struct eapol_key)) {
        /* This is where a wrong passphrase ends up.  The access point cannot
         * verify the second message, so it does not answer -- there is no
         * "wrong password" message in this protocol, only silence. */
        fail("the access point rejected the passphrase");
        return -W_EACCES;
    }

    struct eapol_key *m3 =
        (struct eapol_key *)(body + sizeof(struct eapol_hdr));
    info = get_be16((const uint8_t *)&m3->key_info);

    /* The signature covers exactly what the EAPOL header says the message
     * is, which is not always all that arrived: a frame can carry padding
     * past the end of its payload, and hashing that too would never
     * verify. */
    uint32_t eapol_len = (uint32_t)sizeof(struct eapol_hdr) +
                         get_be16((const uint8_t *)&((struct eapol_hdr *)body)->length);

    if (eapol_len > (uint32_t)len)
        eapol_len = (uint32_t)len;

    /* Check their signature before believing anything in the message.  This
     * is what proves the access point holds the same master key, and without
     * it anything on the channel could hand us a group key of its choosing. */
    {
        uint8_t theirs[16], computed[SHA1_DIGEST_LEN];

        memcpy(theirs, m3->key_mic, 16);
        memset(m3->key_mic, 0, 16);
        hmac_sha1(keys.kck, sizeof(keys.kck), body, eapol_len, computed);

        if (!crypto_equal(theirs, computed, 16)) {
            fail("the access point's handshake signature did not verify");
            return -W_EACCES;
        }
        memcpy(m3->key_mic, theirs, 16);
    }

    uint8_t  gtk[32];
    uint32_t gtk_len = 0;
    int      gtk_index = 1;
    uint16_t data_len = get_be16((const uint8_t *)&m3->key_data_length);
    const uint8_t *key_data = (const uint8_t *)(m3 + 1);

    /* Never read past what actually arrived, whatever the length claims. */
    {
        uint32_t consumed  = (uint32_t)(key_data - body);
        uint32_t available = eapol_len > consumed ? eapol_len - consumed : 0;

        if (data_len > available)
            data_len = (uint16_t)available;
    }

    if ((info & KEY_INFO_ENCRYPTED) && data_len) {
        if (!extract_gtk(key_data, data_len, gtk, &gtk_len, &gtk_index)) {
            /* Not fatal on its own: without the group key we miss broadcast
             * traffic, and unicast still works. */
            kputs("wifi   : the group key could not be unwrapped; "
                  "broadcast traffic will not be readable\n");
            gtk_len = 0;
        }
    }

    /* ---- message four: we are satisfied ---- */
    if (send_eapol(net->bssid,
                   KEY_VERSION_SHA1_AES | KEY_INFO_PAIRWISE |
                   KEY_INFO_MIC | KEY_INFO_SECURE,
                   m3->replay_counter, NULL, NULL, 0, true) < 0) {
        fail("the adapter would not send the last handshake message");
        return -W_EIO;
    }

    /* Only now do the keys go into the hardware.  Installing them earlier
     * would have the adapter encrypt the fourth message, which the access
     * point is not yet expecting to be encrypted. */
    if (ops->set_key(ops_priv, 0, true, keys.tk, sizeof(keys.tk)) < 0) {
        fail("the adapter would not accept the session key");
        return -W_EIO;
    }

    if (gtk_len)
        ops->set_key(ops_priv, gtk_index, false, gtk, gtk_len);

    return 0;
}

/* ------------------------------------------------------------------ *
 *  The adapter as the IP stack sees it
 * ------------------------------------------------------------------ */

static int      wifi_netdev_send(netdev_t *dev, const void *frame, uint32_t len);
static uint32_t wifi_netdev_poll(netdev_t *dev, void *out, uint32_t cap);
static bool     wifi_netdev_link_up(netdev_t *dev);

static netdev_t wifi_netdev = {
    .name     = "wlan0",
    .wireless = true,
    .send     = wifi_netdev_send,
    .poll     = wifi_netdev_poll,
    .link_up  = wifi_netdev_link_up,
};

/* Ethernet in, 802.11 out.
 *
 * The Ethernet header's three fields become four in 802.11: where Ethernet
 * says only who a frame is from and who it is for, 802.11 must also say which
 * access point is carrying it.  With the to-the-network flag set, the first
 * address is the access point, the second is us, and the third is the machine
 * the frame is really for. */
static int wifi_netdev_send(netdev_t *dev, const void *frame, uint32_t len)
{
    (void)dev;

    uint8_t out[NETDEV_MTU_FRAME];
    uint32_t at = 0;

    if (state != WIFI_STATE_CONNECTED || !ops)
        return -1;
    if (len < 14)
        return -1;

    const uint8_t *eth = (const uint8_t *)frame;
    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)out;

    memset(hdr, 0, sizeof(*hdr));
    hdr->frame_control = (uint16_t)(FC_MAKE(FC_TYPE_DATA, FC_SUB_DATA) | FC_TO_DS);

    /* The protected bit is set here and the hardware is also asked to encrypt,
     * just below.  Whether both are wanted is one for whoever finishes the
     * driver: some adapters set this bit themselves as they encrypt and
     * object to finding it already set, and there is no way to tell from here
     * which kind this is.  If frames go out and come back undecryptable, this
     * line is the first thing to try removing. */
    if (keys.valid)
        hdr->frame_control |= FC_PROTECTED;

    memcpy(hdr->addr1, current_network.bssid, 6);
    memcpy(hdr->addr2, our_mac, 6);
    memcpy(hdr->addr3, eth, 6);              /* the real destination */
    hdr->seq_ctrl = (uint16_t)(tx_sequence++ << 4);
    at = sizeof(*hdr);

    /* The Ethernet type is kept, wrapped in the link-control header that
     * says an Ethernet type is what follows. */
    memcpy(out + at, llc_snap, sizeof(llc_snap));
    at += sizeof(llc_snap);
    memcpy(out + at, eth + 12, 2);
    at += 2;

    uint32_t payload = len - 14;

    if (at + payload > sizeof(out))
        return -1;
    memcpy(out + at, eth + 14, payload);
    at += payload;

    return ops->tx(ops_priv, out, at, keys.valid);
}

/* 802.11 in, Ethernet out.  The reverse of the above, and the place where
 * anything that is not ordinary traffic gets filed instead of returned. */
static uint32_t wifi_netdev_poll(netdev_t *dev, void *out, uint32_t cap)
{
    (void)dev;

    uint8_t  buf[NETDEV_MTU_FRAME];
    int8_t   signal;
    uint32_t len;

    if (!ops)
        return 0;

    len = ops->rx(ops_priv, buf, sizeof(buf), &signal);
    if (len < sizeof(struct ieee80211_hdr))
        return 0;

    const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)buf;
    uint16_t fc = hdr->frame_control;

    /* Management frames still arrive once connected -- beacons from the
     * access point, and the one that says we have been thrown off. */
    if (FC_TYPE(fc) == FC_TYPE_MGMT) {
        uint8_t subtype = (uint8_t)FC_SUBTYPE(fc);

        if (subtype == FC_SUB_DEAUTH || subtype == FC_SUB_DISASSOC) {
            if (have_current &&
                memcmp(hdr->addr2, current_network.bssid, 6) == 0) {
                kputs("wifi   : the access point disconnected us\n");
                state = WIFI_STATE_IDLE;
                keys.valid = false;
            }
        }
        return 0;
    }

    if (FC_TYPE(fc) != FC_TYPE_DATA)
        return 0;

    uint32_t at = sizeof(*hdr);

    if (FC_SUBTYPE(fc) == FC_SUB_QOS_DATA)
        at += 2;

    if (len < at + sizeof(llc_snap) + 2)
        return 0;
    if (memcmp(buf + at, llc_snap, sizeof(llc_snap)) != 0)
        return 0;
    at += sizeof(llc_snap);

    uint16_t ethertype = get_be16(buf + at);
    at += 2;

    /* A key message arriving now is the access point renewing the group key.
     * Not handling it means broadcast traffic stops being readable until the
     * next connection; handling it properly needs the two-message group
     * handshake, which is not implemented. */
    if (ethertype == ETH_P_EAPOL)
        return 0;

    uint32_t payload = len - at;

    if (14 + payload > cap)
        return 0;

    uint8_t *eth = (uint8_t *)out;

    /* Coming from the network, the first address is us and the third is
     * whoever sent it. */
    memcpy(eth, hdr->addr1, 6);
    memcpy(eth + 6, hdr->addr3, 6);
    put_be16(eth + 12, ethertype);
    memcpy(eth + 14, buf + at, payload);

    return 14 + payload;
}

static bool wifi_netdev_link_up(netdev_t *dev)
{
    (void)dev;
    return state == WIFI_STATE_CONNECTED;
}

/* ------------------------------------------------------------------ *
 *  Public
 * ------------------------------------------------------------------ */

void wifi_attach(const wifi_ops_t *new_ops, void *priv)
{
    ops      = new_ops;
    ops_priv = priv;

    memcpy(our_mac, ops->mac(priv), 6);
    memcpy(wifi_netdev.mac, our_mac, 6);

    build_rsn_ie();
    state = WIFI_STATE_IDLE;

    netdev_register(&wifi_netdev);
}

bool wifi_init(void)
{
    /* One driver, because this machine has one wireless adapter.  A second
     * would be probed here and would attach the same way. */
    return iwl_init();
}

bool wifi_present(void)
{
    return ops != NULL;
}

wifi_state_t wifi_state(void)
{
    return state;
}

const wifi_network_t *wifi_current(void)
{
    return have_current ? &current_network : NULL;
}

const char *wifi_last_error(void)
{
    return last_error;
}

int wifi_scan(wifi_network_t *out, int max, uint32_t timeout_ms)
{
    if (!ops)
        return -W_ENODEV;

    wifi_state_t previous = state;

    state = WIFI_STATE_SCANNING;
    scan_reset();

    /* Split the time given across the channels, with a floor: dwelling for
     * less than about a tenth of a second on a channel can miss a beacon
     * entirely, since access points send them roughly ten times a second. */
    uint32_t dwell = timeout_ms / (uint32_t)(sizeof(scan_channels));

    if (dwell < 110)
        dwell = 110;

    int r = ops->scan_start(ops_priv, scan_channels,
                            (int)sizeof(scan_channels), dwell);

    if (r < 0) {
        state = previous;
        fail("the adapter would not start a scan");
        return r;
    }

    uint64_t deadline = time_now_ms() + timeout_ms;

    while (time_now_ms() < deadline) {
        wifi_poll();
        rx_dispatch(NULL);              /* beacons file themselves */

        if (ops->scan_done(ops_priv)) {
            /* Keep draining briefly: the last channel's beacons are still
             * working their way out of the adapter's receive ring when it
             * declares the sweep over. */
            uint64_t drain = time_now_ms() + 150;

            while (time_now_ms() < drain) {
                wifi_poll();
                rx_dispatch(NULL);
                io_wait();
            }
            break;
        }
        io_wait();
    }

    scan_sort();
    state = previous;

    return wifi_scan_cached(out, max);
}

int wifi_scan_cached(wifi_network_t *out, int max)
{
    int n = scan_count < max ? scan_count : max;

    for (int i = 0; i < n; i++)
        out[i] = scan_table[i];
    return n;
}

/* Turn what the user typed into the 256-bit master key.
 *
 * A 64-character hex string is the key itself and is used directly; anything
 * else is a passphrase and is stretched with the network's name as salt.
 * That stretching is the slow step -- 8192 hashes -- and it is why joining a
 * protected network takes a moment longer than an open one. */
static int derive_pmk(const wifi_network_t *net, const char *password)
{
    uint32_t len = (uint32_t)strlen(password);

    if (len == 64) {
        bool hex = true;

        for (uint32_t i = 0; i < 64; i++) {
            char c = password[i];

            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) {
                hex = false;
                break;
            }
        }

        if (hex) {
            for (int i = 0; i < 32; i++) {
                uint8_t byte = 0;

                for (int nibble = 0; nibble < 2; nibble++) {
                    char c = password[i * 2 + nibble];
                    uint8_t v;

                    if (c <= '9')      v = (uint8_t)(c - '0');
                    else if (c <= 'F') v = (uint8_t)(c - 'A' + 10);
                    else               v = (uint8_t)(c - 'a' + 10);

                    byte = (uint8_t)((byte << 4) | v);
                }
                keys.pmk[i] = byte;
            }
            return 0;
        }
    }

    if (len < 8 || len > 63) {
        fail("a WPA2 passphrase must be between 8 and 63 characters");
        return -W_EINVAL;
    }

    pbkdf2_sha1(password, (const uint8_t *)net->ssid, net->ssid_len,
                4096, keys.pmk, sizeof(keys.pmk));
    return 0;
}

/* Find a network by name, scanning first if we have not recently. */
static const wifi_network_t *lookup(const char *ssid)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < scan_count; i++)
            if (strcmp(scan_table[i].ssid, ssid) == 0)
                return &scan_table[i];

        if (pass == 0) {
            wifi_network_t discard[1];

            kprintf("wifi   : looking for \"%s\"\n", ssid);
            wifi_scan(discard, 1, 4000);
        }
    }
    return NULL;
}

int wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    last_error[0] = '\0';

    if (!ops)
        return -W_ENODEV;
    if (!ssid || !*ssid) {
        fail("no network name was given");
        return -W_EINVAL;
    }

    const wifi_network_t *found = lookup(ssid);

    if (!found) {
        fail("no network of that name is in range");
        return -W_ENOENT;
    }

    wifi_network_t net = *found;      /* a scan may overwrite the table */
    bool protected_network = (net.security != WIFI_SECURITY_OPEN);

    /* The name in the table for a hidden network is one this code made up out
     * of the access point's address -- there is no real one to associate
     * with.  Joining one means sending a directed probe carrying the name,
     * which is not implemented; refusing is better than associating under a
     * name the access point has never heard of. */
    if (net.hidden) {
        fail("this network hides its name, which this system cannot join");
        return -W_ENOTSUP;
    }

    if (net.security == WIFI_SECURITY_WEP) {
        fail("this network uses WEP, which is broken and not implemented");
        return -W_ENOTSUP;
    }
    if (net.security == WIFI_SECURITY_WPA) {
        fail("this network uses the original WPA with TKIP, "
             "which is not implemented");
        return -W_ENOTSUP;
    }
    if (net.security == WIFI_SECURITY_WPA2 && !net.ccmp) {
        fail("this network offers no cipher this system implements");
        return -W_ENOTSUP;
    }
    if (net.security == WIFI_SECURITY_WPA3) {
        /* Most WPA3 access points also accept WPA2 in a transitional mode,
         * and those advertise both suites, so this only refuses the ones
         * that are WPA3 alone. */
        fail("this network requires WPA3, whose handshake is not implemented");
        return -W_ENOTSUP;
    }

    if (protected_network) {
        if (!password || !*password) {
            fail("this network needs a passphrase");
            return -W_EINVAL;
        }

        int r = derive_pmk(&net, password);

        if (r < 0)
            return r;
    }

    state = WIFI_STATE_JOINING;
    have_current = false;
    keys.valid = false;

    kprintf("wifi   : joining \"%s\" on channel %u\n", net.ssid, net.channel);

    /* Stop sweeping and sit on the network's channel. */
    if (ops->set_channel(ops_priv, net.channel) < 0) {
        fail("the adapter would not tune to the network's channel");
        state = WIFI_STATE_IDLE;
        return -W_EIO;
    }

    if (ops->set_bss(ops_priv, net.bssid, (const uint8_t *)net.ssid,
                     net.ssid_len, false, 0) < 0) {
        fail("the adapter would not accept the access point's address");
        state = WIFI_STATE_IDLE;
        return -W_EIO;
    }

    int r = do_authenticate(&net, timeout_ms / 4);

    if (r < 0) {
        state = WIFI_STATE_IDLE;
        return r;
    }

    r = do_associate(&net, protected_network, timeout_ms / 4);
    if (r < 0) {
        state = WIFI_STATE_IDLE;
        return r;
    }

    /* Now that we are a member, tell the adapter so -- from here it
     * acknowledges frames and keeps time with the access point. */
    ops->set_bss(ops_priv, net.bssid, (const uint8_t *)net.ssid,
                 net.ssid_len, true, current_aid);

    current_network = net;
    have_current = true;

    if (protected_network) {
        r = do_handshake(&net, timeout_ms / 2);
        if (r < 0) {
            state = WIFI_STATE_IDLE;
            have_current = false;
            return r;
        }
    }

    state = WIFI_STATE_CONNECTED;

    /* The link carries frames now, but we have no address on this network.
     * Point the stack here and ask for one. */
    net_bind(&wifi_netdev);

    r = net_dhcp(6000);
    if (r < 0) {
        /* Connected but unaddressed.  Worth saying plainly, because every
         * symptom of it looks like the network being broken. */
        kputs("wifi   : joined, but no address was offered; "
              "set one by hand to use this network\n");
        fail("joined the network, but no address was offered");
    }

    kprintf("wifi   : connected to \"%s\"\n", net.ssid);
    return 0;
}

int wifi_disconnect(void)
{
    if (!ops)
        return -W_ENODEV;
    if (state != WIFI_STATE_CONNECTED && !have_current)
        return 0;

    /* Say so rather than simply going quiet: an access point that is not told
     * keeps the association alive, and keeps buffering for us. */
    if (have_current) {
        uint8_t  frame[64];
        uint32_t at = mgmt_header(frame, FC_SUB_DEAUTH, current_network.bssid);

        put_le16(frame + at, 3); at += 2;    /* leaving, of our own accord */
        ops->tx(ops_priv, frame, at, false);
        wifi_wait(20);                       /* let it go out before we stop */
    }

    ops->set_key(ops_priv, 0, true, NULL, 0);
    ops->set_bss(ops_priv, current_network.bssid, NULL, 0, false, 0);

    memset(&keys, 0, sizeof(keys));
    have_current = false;
    state = WIFI_STATE_IDLE;

    /* Hand the stack back to a wired adapter if the machine has one. */
    for (int i = 0; i < netdev_count(); i++) {
        netdev_t *dev = netdev_at(i);

        if (dev && !dev->wireless) {
            net_bind(dev);
            return 0;
        }
    }

    net_bind(NULL);
    return 0;
}
