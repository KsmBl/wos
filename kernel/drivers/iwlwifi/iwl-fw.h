/* Reading an Intel wireless firmware file.
 *
 * The file is a small header followed by a stream of tagged blocks: a type, a
 * length, and that many bytes, padded out to a multiple of four.  Most of the
 * blocks describe the firmware -- its version, how many processors it runs
 * on, what it is capable of -- and a handful of them are the firmware itself,
 * each carrying the address inside the device that it loads at.
 *
 * Unlike the rest of this driver, the code behind this header has been
 * checked against the real thing: the 1.45 MiB image this machine's adapter
 * uses parses into the sections listed below, its version comes out as 46 to
 * match its filename, and every block in it is accounted for.
 */
#ifndef WOS_IWL_FW_H
#define WOS_IWL_FW_H

#include "types.h"
#include "dma.h"

/* The header, before any of the tagged blocks. */
#define IWL_FW_MAGIC        0x0a4c5749u    /* "IWL\n" */
#define IWL_FW_HEADER_SIZE  0x58
#define IWL_FW_DESCRIPTOR_LEN 64

/* Block types.  Only the ones this driver acts on are named; the rest are
 * skipped, and there are many -- roughly three quarters of the blocks in a
 * modern firmware file describe how to debug it. */
#define IWL_TLV_PROBE_MAX_LEN         6
#define IWL_TLV_FLAGS                18
#define IWL_TLV_SEC_RT               19   /* a section of the running firmware */
#define IWL_TLV_SEC_INIT             20   /* ... of the calibration firmware   */
#define IWL_TLV_DEF_CALIB            22
#define IWL_TLV_PHY_SKU              23
#define IWL_TLV_NUM_OF_CPU           27
#define IWL_TLV_API_CHANGES_SET      29
#define IWL_TLV_ENABLED_CAPABILITIES 30
#define IWL_TLV_N_SCAN_CHANNELS      31
#define IWL_TLV_PAGING               32
#define IWL_TLV_FW_VERSION           36
#define IWL_TLV_FW_MEM_SEG           51

/* Two addresses that are not addresses.  A section whose load address is one
 * of these is a marker rather than data: the first says the sections that
 * follow belong to the device's second processor, and the second says what
 * follows is paged in on demand rather than resident. */
#define IWL_FW_CPU_SEPARATOR     0xFFFFCCCCu
#define IWL_FW_PAGING_SEPARATOR  0xAAAABBBBu

#define IWL_FW_SECTION_MAX 24

typedef struct {
    uint32_t       address;   /* where in the device this loads */
    const uint8_t *data;
    uint32_t       len;
    int            cpu;       /* 1 or 2 */
    bool           paged;     /* past the paging separator      */
} iwl_fw_section_t;

typedef struct {
    iwl_fw_section_t section[IWL_FW_SECTION_MAX];
    int              count;
} iwl_fw_image_t;

typedef struct {
    bool     valid;

    /* The file itself, held whole because the sections point into it. */
    dma_buffer_t image;
    uint32_t     image_len;

    char     descriptor[IWL_FW_DESCRIPTOR_LEN + 1];
    uint32_t version;
    uint32_t build;

    iwl_fw_image_t regular;   /* what runs once the adapter is up   */
    iwl_fw_image_t init;      /* what runs first, to calibrate      */

    uint32_t num_cpus;
    uint32_t paging_size;
    uint32_t n_scan_channels;
    uint32_t probe_max_len;
    uint32_t phy_sku;
    uint32_t flags;

    /* The firmware's own statement of what it understands and what it can
     * do, as bitmaps indexed by the block's first word. */
    uint32_t api[4];
    uint32_t capa[4];
} iwl_fw_t;

/* Read the firmware from the filesystem and parse it.
 *
 * On success the sections point into `fw->image`, which stays allocated until
 * iwl_fw_free.  Returns false, with a reason on the console, if the parts are
 * missing or the file is not a firmware image. */
bool iwl_fw_load(iwl_fw_t *fw);

/* Parse an image already in memory.  iwl_fw_load calls this once it has the
 * parts joined; it is separate so it can be run against a real firmware file
 * on a development machine, which is how the block layout above was
 * checked. */
bool iwl_fw_parse(iwl_fw_t *fw, const uint8_t *buf, uint32_t len);

void iwl_fw_free(iwl_fw_t *fw);

#endif /* WOS_IWL_FW_H */
