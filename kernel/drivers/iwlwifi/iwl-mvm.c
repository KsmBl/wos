/* Talking to the firmware once it is running, and presenting the adapter to
 * the 802.11 layer.  See iwlwifi.h -- and its warning.
 *
 * Everything below the firmware is registers and rings, and iwl-trans.c deals
 * with those.  Everything at this level is a conversation: the host sends a
 * numbered command with a body, and the firmware answers or acts.  The
 * numbering and the bodies are the firmware's interface, not the hardware's,
 * and they change between firmware versions -- which is the reason this file
 * carries more uncertainty than any other in the driver.
 *
 * ---------------------------------------------------------------------------
 * What is here and what is not
 *
 * The order of operations is the stable, valuable part, and it is written out
 * in full below: it has been the same across many firmware generations, and
 * anyone bringing this driver up against real silicon will need it.
 *
 * The command bodies are the unstable part.  Several of them -- the station
 * command, the MAC context, the scan request -- are large structures whose
 * layout is versioned by the firmware's own capability flags, and this author
 * could not reproduce them faithfully without the hardware or its
 * documentation to check against.  Rather than fill them with plausible
 * numbers, the functions that need them say so and fail.  A driver that
 * refuses to scan is honest; one that sends a malformed scan request and
 * wedges the firmware is not.
 * ---------------------------------------------------------------------------
 */

#include "iwl-trans.h"
#include "iwlwifi.h"
#include "wifi.h"
#include "netdev.h"
#include "pit.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"
#include "wabi.h"

/* Command groups.  The original commands are all in the legacy group; later
 * additions were given groups of their own so the one-byte command number
 * could start over. */
#define IWL_GROUP_LEGACY     0x0
#define IWL_GROUP_LONG       0x1
#define IWL_GROUP_SYSTEM     0x2
#define IWL_GROUP_MAC_CONF   0x3
#define IWL_GROUP_PHY_OPS    0x4
#define IWL_GROUP_DATA_PATH  0x5

/* Commands in the legacy group.  These numbers have been stable for a long
 * time and are the ones this author is most confident of; the structures that
 * go with them are another matter. */
#define IWL_CMD_MVM_ALIVE            0x01
#define IWL_CMD_INIT_COMPLETE        0x04
#define IWL_CMD_PHY_CONTEXT          0x08
#define IWL_CMD_SCAN_CFG             0x0c
#define IWL_CMD_SCAN_REQ_UMAC        0x0d
#define IWL_CMD_SCAN_ABORT_UMAC      0x0e
#define IWL_CMD_SCAN_COMPLETE_UMAC   0x0f
#define IWL_CMD_ADD_STA_KEY          0x17
#define IWL_CMD_ADD_STA              0x18
#define IWL_CMD_REMOVE_STA           0x19
#define IWL_CMD_TX                   0x1c
#define IWL_CMD_MAC_CONTEXT          0x28
#define IWL_CMD_TIME_EVENT           0x29
#define IWL_CMD_BINDING_CONTEXT      0x2b
#define IWL_CMD_TIME_QUOTA           0x2c
#define IWL_CMD_PHY_CONFIGURATION    0x6a
#define IWL_CMD_PHY_DB               0x6c
#define IWL_CMD_POWER_TABLE          0x77
#define IWL_CMD_NVM_ACCESS           0x88
#define IWL_CMD_TX_ANT_CONFIG        0x98
#define IWL_CMD_MCC_UPDATE           0xc8
#define IWL_CMD_RX_PHY               0xc0
#define IWL_CMD_RX_MPDU              0xc1
#define IWL_CMD_LTR_CONFIG           0xee

/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */

static iwl_trans_t trans;

static struct {
    bool     started;
    uint8_t  channel;
    uint8_t  bssid[6];
    bool     associated;
    uint16_t aid;

    /* Frames pulled off the ring that the layer above has not taken yet.
     * One is enough: the caller drains this before asking for another. */
    uint8_t  pending[NETDEV_MTU_FRAME];
    uint32_t pending_len;
    int8_t   pending_signal;

    bool     scanning;
    uint64_t scan_deadline;
} mvm;

/* Whether the parts of the firmware interface this driver cannot build have
 * been reached.  Said once rather than on every call, because the wireless
 * layer retries and the message would otherwise fill the console. */
static bool warned_incomplete;

static int not_implemented(const char *what)
{
    if (!warned_incomplete) {
        kprintf("iwlwifi: %s needs a firmware command whose layout this "
                "driver does not have\n", what);
        kputs("iwlwifi: the adapter is found and its firmware parsed, but "
              "it cannot be commanded yet -- see iwl-mvm.c\n");
        warned_incomplete = true;
    }
    return -W_ENOSYS;
}

/* ------------------------------------------------------------------ *
 *  Receiving
 *
 *  Everything the firmware sends arrives on one ring, tagged with the
 *  command it belongs to.  Received frames are one such tag; command
 *  replies, notifications and statistics are others.  This sorts them.
 * ------------------------------------------------------------------ */

/* Take one thing off the ring.  Frames are kept for the layer above;
 * everything else is dealt with here or discarded. */
static void iwl_service(void)
{
    if (!trans.firmware_running)
        return;
    if (mvm.pending_len)
        return;                    /* the last frame has not been taken */

    struct iwl_rx_packet *pkt = iwl_rx_next(&trans);

    if (!pkt)
        return;

    uint32_t len = IWL_RX_PACKET_LEN(pkt);

    switch (pkt->hdr.cmd) {
    case IWL_CMD_RX_MPDU: {
        /* A received frame, behind a descriptor giving its length and how
         * strongly it arrived.  The descriptor's layout is one of the things
         * that varies by firmware version; what is done here is the shape of
         * it rather than a layout known to be right. */
        if (len < 8)
            break;

        /* The signal strength is reported as a positive number of decibels
         * below a reference, and everything above wants it negative. */
        int8_t signal = -((int8_t)(pkt->data[4] & 0x7F));
        uint32_t frame_len = (uint32_t)pkt->data[0] | ((uint32_t)pkt->data[1] << 8);

        if (frame_len == 0 || frame_len > len)
            break;
        if (frame_len > sizeof(mvm.pending))
            frame_len = sizeof(mvm.pending);

        memcpy(mvm.pending, pkt->data + 8, frame_len);
        mvm.pending_len = frame_len;
        mvm.pending_signal = signal;
        break;
    }

    case IWL_CMD_SCAN_COMPLETE_UMAC:
        mvm.scanning = false;
        break;

    default:
        /* Statistics, beacons the firmware handled itself, notifications
         * about things this driver does not act on. */
        break;
    }
}

/* ------------------------------------------------------------------ *
 *  The operations the 802.11 layer calls
 * ------------------------------------------------------------------ */

static void op_poll(void *priv)
{
    (void)priv;
    iwl_service();
}

static const uint8_t *op_mac(void *priv)
{
    (void)priv;
    return trans.mac;
}

static uint32_t op_rx(void *priv, void *out, uint32_t cap, int8_t *signal_dbm)
{
    (void)priv;

    iwl_service();

    if (!mvm.pending_len)
        return 0;

    uint32_t len = mvm.pending_len;

    if (len > cap)
        len = cap;
    memcpy(out, mvm.pending, len);

    if (signal_dbm)
        *signal_dbm = mvm.pending_signal;

    mvm.pending_len = 0;
    return len;
}

static int op_set_channel(void *priv, uint8_t channel)
{
    (void)priv;

    /* Tuning is a phy-context command.  Its body carries the band, the
     * channel, the width and the antenna configuration, in a structure whose
     * size changed with the firmware that introduced wider channels. */
    mvm.channel = channel;
    return not_implemented("tuning to a channel");
}

static int op_set_bss(void *priv, const uint8_t bssid[6],
                      const uint8_t *ssid, uint32_t ssid_len,
                      bool associated, uint16_t aid)
{
    (void)priv;
    (void)ssid;
    (void)ssid_len;

    if (bssid)
        memcpy(mvm.bssid, bssid, 6);
    mvm.associated = associated;
    mvm.aid = aid;

    /* This is two commands: the MAC context, which describes the network we
     * are a member of, and the station command, which describes the access
     * point as a peer the firmware should acknowledge and encrypt for. */
    return not_implemented("joining a network");
}

static int op_scan_start(void *priv, const uint8_t *channels, int count,
                         uint32_t dwell_ms)
{
    (void)priv;
    (void)channels;
    (void)count;

    mvm.scanning = true;
    mvm.scan_deadline = time_now_ms() + dwell_ms;

    /* The scan request carries the channel list, the dwell times, the probe
     * requests to send and which addresses to send them from.  Its layout is
     * versioned by the firmware's capability flags -- there are several
     * incompatible ones in service -- and this driver does not have it. */
    mvm.scanning = false;
    return not_implemented("scanning for networks");
}

static bool op_scan_done(void *priv)
{
    (void)priv;

    iwl_service();

    /* A scan that the firmware never acknowledged is over as soon as the
     * time it was given runs out, so nothing waits forever on it. */
    if (mvm.scanning && time_now_ms() > mvm.scan_deadline)
        mvm.scanning = false;

    return !mvm.scanning;
}

static int op_tx(void *priv, const void *frame, uint32_t len, bool encrypt)
{
    (void)priv;
    (void)frame;
    (void)len;
    (void)encrypt;

    /* Sending needs a transmit command header in front of the frame: rate,
     * antenna, key index, lifetime and flags.  The frame itself would then
     * follow it as a second entry in the descriptor. */
    return not_implemented("sending a frame");
}

static int op_set_key(void *priv, int index, bool pairwise,
                      const uint8_t *key, uint32_t len)
{
    (void)priv;
    (void)index;
    (void)pairwise;
    (void)key;
    (void)len;

    return not_implemented("installing an encryption key");
}

static const wifi_ops_t iwl_ops = {
    .scan_start  = op_scan_start,
    .scan_done   = op_scan_done,
    .set_channel = op_set_channel,
    .set_bss     = op_set_bss,
    .tx          = op_tx,
    .rx          = op_rx,
    .set_key     = op_set_key,
    .mac         = op_mac,
    .poll        = op_poll,
};

/* ------------------------------------------------------------------ *
 *  Bring-up
 * ------------------------------------------------------------------ */

/* What has to happen, in order, for this adapter to carry traffic.  Steps one
 * to four are implemented; the rest need the command layouts described at the
 * top of this file.
 *
 *   1. find the adapter on the bus and map its registers      (iwl-trans.c)
 *   2. read and parse the firmware file                       (iwl-fw.c)
 *   3. power the device up and set up its rings               (iwl-trans.c)
 *   4. load the firmware and wait for it to say it is alive   (iwl-trans.c)
 *   5. read the non-volatile memory: the hardware address, which channels
 *      this device is allowed to use, and its calibration data
 *   6. send the phy database the firmware asks for, which is calibration
 *      data the firmware itself produced on a previous run
 *   7. configure the radio: antennas, transmit power, regulatory domain
 *   8. create a MAC context -- the firmware's notion of "a station on a
 *      network" -- and a phy context, which is a channel
 *   9. bind the two together and give the binding a share of the airtime
 *  10. from here the adapter will scan, and everything else follows
 */
bool iwl_init(void)
{
    if (mvm.started)
        return true;

    memset(&mvm, 0, sizeof(mvm));

    if (!iwl_trans_probe(&trans))
        return false;                  /* no adapter; nothing to report */

    if (!iwl_trans_start(&trans)) {
        /* The adapter is there but would not start.  iwl_trans_start has
         * already said why. */
        return false;
    }

    /* Step five onwards.  Without the hardware address read from the
     * device's own memory there is nothing to put in a frame's sender
     * field, so this is where bring-up stops. */
    kputs("iwlwifi: firmware is up; reading the adapter's configuration\n");

    iwl_read_mac_address(&trans);

    if (!trans.mac[0] && !trans.mac[1] && !trans.mac[2] &&
        !trans.mac[3] && !trans.mac[4] && !trans.mac[5]) {
        kputs("iwlwifi: the adapter's hardware address could not be read "
              "-- the driver stops here\n");
        iwl_trans_stop(&trans);
        return false;
    }

    wifi_attach(&iwl_ops, &mvm);
    mvm.started = true;
    return true;
}
