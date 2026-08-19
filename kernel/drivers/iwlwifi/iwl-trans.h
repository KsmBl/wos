/* The transport: what moves bytes between this machine and the adapter.
 *
 * Above this sit the commands -- "scan these channels", "use this key" --
 * which are the firmware's vocabulary and live in iwl-mvm.c.  Below it is the
 * hardware.  This layer knows about registers, rings and DMA, and nothing at
 * all about what the bytes it carries mean.
 */
#ifndef WOS_IWL_TRANS_H
#define WOS_IWL_TRANS_H

#include "types.h"
#include "dma.h"
#include "pci.h"
#include "iwl-fw.h"
#include "iwl-regs.h"

/* A transmit descriptor, as the hardware reads it: a count of scatter-gather
 * entries and then the entries themselves, each an address and a length.
 *
 * The packed layout matters exactly: the device walks this structure itself,
 * and a field in the wrong place sends it reading somewhere arbitrary. */
struct __attribute__((packed)) iwl_tfd_tb {
    uint32_t lo;              /* low 32 bits of the buffer address        */
    uint16_t hi_and_len;      /* 4 bits of address, 12 bits of length     */
};

struct __attribute__((packed)) iwl_tfd {
    uint8_t  reserved1[3];
    uint8_t  num_tbs;         /* how many entries below are in use        */
    struct iwl_tfd_tb tbs[IWL_NUM_TBS];
    uint32_t reserved2;
};

/* The header every command and frame carries into the firmware. */
struct __attribute__((packed)) iwl_cmd_header {
    uint8_t  cmd;             /* which command                            */
    uint8_t  group_id;        /* which family of commands it belongs to   */
    uint16_t sequence;        /* echoed back in the response              */
};

/* What the firmware puts in front of everything it sends back. */
struct __attribute__((packed)) iwl_rx_packet {
    uint32_t len_n_flags;
    struct iwl_cmd_header hdr;
    uint8_t  data[];
};

#define IWL_RX_PACKET_LEN(p) ((p)->len_n_flags & 0x00003FFF)

/* One transmit queue. */
typedef struct {
    dma_buffer_t descriptors;  /* the ring of iwl_tfd                     */
    dma_buffer_t buffers;      /* one command-sized buffer per slot       */
    uint32_t     write;        /* next slot to fill                       */
    uint32_t     read;         /* what the hardware has finished          */
    bool         active;
} iwl_tx_queue_t;

typedef struct {
    bool     present;

    /* Where the device sits on the bus.  Kept because a reset of the device
     * clears its configuration space, and bus mastering has to be turned on
     * again afterwards -- which needs the address to write it to. */
    pci_device_t pci;

    /* Where the device's registers are mapped. */
    volatile uint8_t *regs;

    uint32_t hw_rev;
    uint32_t hw_rf_id;
    uint8_t  mac[6];

    iwl_fw_t fw;

    /* The receive ring: a ring of addresses handed to the device, the
     * buffers they point at, and the place the device writes how far it has
     * got. */
    dma_buffer_t rx_free_table;   /* buffers handed to the device        */
    dma_buffer_t rx_used_table;   /* which of them it has filled         */
    dma_buffer_t rx_buffers;      /* the buffers themselves              */
    dma_buffer_t rx_status;       /* how far along the used table it is  */
    uint32_t     rx_read;

    /* The queue commands go out on, and the ones traffic goes out on. */
    iwl_tx_queue_t queue[IWL_NUM_QUEUES];

    /* A page the device touches so its memory controller stays awake. */
    dma_buffer_t keep_warm;

    /* Where the scheduler keeps its byte counts, which the firmware is told
     * about once it is alive. */
    dma_buffer_t scheduler;

    /* Set once the driver has said it is falling back to loading the
     * firmware by hand, so it says so once rather than per section. */
    bool     window_load_warned;

    bool     firmware_running;
    uint16_t cmd_sequence;

    /* The last response the firmware sent to a command, kept so the caller
     * that sent the command can read it. */
    uint8_t  response[512];
    uint32_t response_len;
    bool     response_valid;
} iwl_trans_t;

/* Find the adapter and map its registers.  Does not power it up. */
bool iwl_trans_probe(iwl_trans_t *trans);

/* Power the device up, load its firmware and wait for it to report that it is
 * running.  Returns false with a reason on the console. */
bool iwl_trans_start(iwl_trans_t *trans);

/* Put the device back to sleep and release everything. */
void iwl_trans_stop(iwl_trans_t *trans);

/* Read the adapter's own hardware address out of its configuration registers.
 * Returns false if neither copy holds a usable one. */
bool iwl_read_mac_address(iwl_trans_t *trans);

/* Register access.  These are ordinary memory reads and writes to the mapped
 * window, wrapped so that every one of them is visible as a device access
 * rather than a pointer dereference. */
uint32_t iwl_read32(iwl_trans_t *trans, uint32_t offset);
void     iwl_write32(iwl_trans_t *trans, uint32_t offset, uint32_t value);

/* The peripheral registers, which are reached through a window rather than
 * mapped.  The device must be awake for these to mean anything. */
/* One word of the device's own memory, through the address/data window. */
uint32_t iwl_read_mem32(iwl_trans_t *trans, uint32_t address);
void     iwl_write_mem32(iwl_trans_t *trans, uint32_t address, uint32_t value);

uint32_t iwl_read_prph(iwl_trans_t *trans, uint32_t address);
void     iwl_write_prph(iwl_trans_t *trans, uint32_t address, uint32_t value);

/* A 64-bit peripheral register, written as its two halves. */
void     iwl_write_prph64(iwl_trans_t *trans, uint32_t address, uint64_t value);

/* Ask for, and release, the right to touch the device's internals.  Between
 * these the device is held awake.  Returns false if it never woke. */
bool iwl_grab_nic_access(iwl_trans_t *trans);
void iwl_release_nic_access(iwl_trans_t *trans);

/* Send a command to the firmware and, if `wait` is set, wait for its reply.
 * `group` and `cmd` name the command; `data` is its body.
 *
 * Returns 0, or negative on a timeout.  The reply, if there was one, is in
 * trans->response. */
int iwl_send_cmd(iwl_trans_t *trans, uint8_t group, uint8_t cmd,
                 const void *data, uint16_t len, bool wait);

/* Put one 802.11 frame on a traffic queue. */
int iwl_tx_frame(iwl_trans_t *trans, int queue, const void *header,
                 uint32_t header_len, const void *body, uint32_t body_len);

/* Take the next thing the device has sent, if any.  Returns a pointer into
 * the receive ring valid until the next call, or NULL.  This is how both
 * command replies and received frames arrive -- they share one ring. */
struct iwl_rx_packet *iwl_rx_next(iwl_trans_t *trans);

#endif /* WOS_IWL_TRANS_H */
