/* Reading an Intel wireless firmware file. See iwl-fw.h. */

#include "iwl-fw.h"
#include "iwlwifi.h"
#include "wfs_kernel.h"
#include "ramfs.h"
#include "wfs.h"
#include "string.h"
#include "kprintf.h"

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Add a section to an image, unless it is one of the two markers -- those
 * change how the sections after them are labelled rather than being data.
 *
 * One thing about the real file is not fully understood.  Its blocks are not
 * grouped by image: runtime and calibration sections are interleaved, and the
 * marker that ends the first processor's sections appears once, tagged as a
 * runtime block, with calibration sections following it.  Read literally --
 * which is what happens here -- that marker advances the runtime image and
 * leaves the calibration image's sections all labelled as the first
 * processor's.  It may be that the marker is meant to apply to whichever
 * image comes next rather than to its own.
 *
 * It does not matter for this driver, which runs the runtime image only, and
 * that one comes out exactly as the file describes it: four sections for the
 * first processor, four for the second, and two paged.  It would matter to a
 * driver that also ran the calibration image, and it is written down here so
 * that whoever needs that knows where to look. */
static void add_section(iwl_fw_image_t *img, const uint8_t *tlv, uint32_t len,
                        int *cpu, bool *paged)
{
    uint32_t address = get_le32(tlv);

    if (address == IWL_FW_CPU_SEPARATOR) {
        *cpu = 2;
        return;
    }
    if (address == IWL_FW_PAGING_SEPARATOR) {
        *paged = true;
        return;
    }

    if (img->count >= IWL_FW_SECTION_MAX)
        return;

    iwl_fw_section_t *s = &img->section[img->count++];

    s->address = address;
    s->data    = tlv + 4;
    s->len     = len - 4;
    s->cpu     = *cpu;
    s->paged   = *paged;
}

/* Read the firmware file into a buffer the device can be pointed at directly.
 *
 * This goes at the filesystem rather than through the file layer above it
 * because that layer answers on behalf of a process -- it checks what the
 * caller is allowed to read -- and there is no process here.  The kernel
 * fetching its own firmware is not acting for anybody. */
static bool read_firmware(iwl_fw_t *fw)
{
    const char      *path = IWL_FIRMWARE_PATH;
    struct wfs_inode in;
    bool             ram = ramfs_owns(path);
    uint32_t         ino;

    int r = ram ? ramfs_lookup(path, &ino) : wfs_lookup(path, &ino);

    if (r < 0) {
        kprintf("iwlwifi: no firmware at %s -- the adapter cannot start "
                "without it\n", path);
        return false;
    }

    r = ram ? ramfs_read_inode(ino, &in) : wfs_read_inode(ino, &in);
    if (r < 0 || in.type != WFS_TYPE_FILE || in.size == 0) {
        kprintf("iwlwifi: %s is not a readable file\n", path);
        return false;
    }

    fw->image = dma_alloc(in.size, PAGE_SIZE);
    if (!fw->image.virt) {
        kprintf("iwlwifi: no room for a %u-byte firmware image\n", in.size);
        return false;
    }

    r = ram ? ramfs_read(ino, 0, fw->image.virt, in.size)
            : wfs_read(ino, 0, fw->image.virt, in.size);

    if (r < 0) {
        kprintf("iwlwifi: %s would not read\n", path);
        dma_free(&fw->image);
        return false;
    }

    fw->image_len = in.size;
    kprintf("iwlwifi: firmware, %u bytes from %s\n", in.size, path);
    return true;
}

bool iwl_fw_parse(iwl_fw_t *fw, const uint8_t *buf, uint32_t len)
{
    if (len < IWL_FW_HEADER_SIZE || get_le32(buf + 4) != IWL_FW_MAGIC) {
        kputs("iwlwifi: that is not a firmware image -- wrong magic number\n");
        return false;
    }

    memcpy(fw->descriptor, buf + 8, IWL_FW_DESCRIPTOR_LEN);
    fw->descriptor[IWL_FW_DESCRIPTOR_LEN] = '\0';
    fw->version = get_le32(buf + 0x48);
    fw->build   = get_le32(buf + 0x4c);

    /* Each image's sections are numbered from its own first processor, and
     * the markers inside the stream move these along. */
    int  rt_cpu = 1, init_cpu = 1;
    bool rt_paged = false, init_paged = false;

    uint32_t at = IWL_FW_HEADER_SIZE;
    int      blocks = 0;

    while (at + 8 <= len) {
        uint32_t type = get_le32(buf + at);
        uint32_t tlen = get_le32(buf + at + 4);

        at += 8;

        /* A length that runs past the end of the file means the file is
         * damaged; stopping is the only safe thing to do with the rest. */
        if (tlen > len - at) {
            kprintf("iwlwifi: firmware block of type %u claims %u bytes "
                    "with only %u left; stopping here\n",
                    type, tlen, len - at);
            break;
        }

        const uint8_t *data = buf + at;

        switch (type) {
        case IWL_TLV_SEC_RT:
            if (tlen > 4)
                add_section(&fw->regular, data, tlen, &rt_cpu, &rt_paged);
            break;
        case IWL_TLV_SEC_INIT:
            if (tlen > 4)
                add_section(&fw->init, data, tlen, &init_cpu, &init_paged);
            break;
        case IWL_TLV_NUM_OF_CPU:
            if (tlen >= 4)
                fw->num_cpus = get_le32(data);
            break;
        case IWL_TLV_PAGING:
            if (tlen >= 4)
                fw->paging_size = get_le32(data);
            break;
        case IWL_TLV_N_SCAN_CHANNELS:
            if (tlen >= 4)
                fw->n_scan_channels = get_le32(data);
            break;
        case IWL_TLV_PROBE_MAX_LEN:
            if (tlen >= 4)
                fw->probe_max_len = get_le32(data);
            break;
        case IWL_TLV_PHY_SKU:
            if (tlen >= 4)
                fw->phy_sku = get_le32(data);
            break;
        case IWL_TLV_FLAGS:
            if (tlen >= 4)
                fw->flags = get_le32(data);
            break;
        case IWL_TLV_FW_VERSION:
            if (tlen >= 4)
                fw->version = get_le32(data);
            break;
        /* These two come as an index and a bitmap, and there are several of
         * each: the firmware describes itself a bank of flags at a time. */
        case IWL_TLV_API_CHANGES_SET:
            if (tlen >= 8) {
                uint32_t index = get_le32(data);

                if (index < 4)
                    fw->api[index] = get_le32(data + 4);
            }
            break;
        case IWL_TLV_ENABLED_CAPABILITIES:
            if (tlen >= 8) {
                uint32_t index = get_le32(data);

                if (index < 4)
                    fw->capa[index] = get_le32(data + 4);
            }
            break;
        default:
            break;
        }

        /* Blocks are padded out to a multiple of four. */
        at += (tlen + 3) & ~3u;
        blocks++;
    }

    if (!fw->regular.count) {
        kputs("iwlwifi: the firmware file holds no runnable sections\n");
        return false;
    }

    fw->valid = true;

    kprintf("iwlwifi: firmware %u \"%s\"\n", fw->version, fw->descriptor);
    kprintf("iwlwifi: %d blocks, %d runtime sections, %d calibration "
            "sections, %u cpus\n",
            blocks, fw->regular.count, fw->init.count, fw->num_cpus);

    if (fw->paging_size)
        kprintf("iwlwifi: this firmware pages %u bytes in and out; "
                "paging is not implemented, so it may not start\n",
                fw->paging_size);

    return true;
}

bool iwl_fw_load(iwl_fw_t *fw)
{
    memset(fw, 0, sizeof(*fw));

    if (!read_firmware(fw))
        return false;

    if (!iwl_fw_parse(fw, (const uint8_t *)fw->image.virt, fw->image_len)) {
        dma_free(&fw->image);
        return false;
    }

    return true;
}

void iwl_fw_free(iwl_fw_t *fw)
{
    dma_free(&fw->image);
    fw->valid = false;
    fw->image_len = 0;
    fw->regular.count = 0;
    fw->init.count = 0;
}
