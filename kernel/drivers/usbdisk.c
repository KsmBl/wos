/* USB mass storage over the bulk-only transport. See usbdisk.h.
 *
 * Every command is the same three steps: send a 31-byte command block wrapper
 * carrying a SCSI command, move the data, then read a 13-byte status wrapper
 * back.  The device is a disk that speaks SCSI; the USB part is only how the
 * command gets there.
 */

#include "usbdisk.h"
#include "xhci.h"
#include "string.h"
#include "kprintf.h"

/* ------------------------------------------------------------------ *
 *  USB descriptors, only the fields that are read
 * ------------------------------------------------------------------ */

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02

#define USB_CLASS_MASS_STORAGE 0x08
#define USB_SUBCLASS_SCSI      0x06
#define USB_PROTOCOL_BULK_ONLY 0x50

typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint16_t usb_version;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device_version;
    uint8_t  manufacturer_string;
    uint8_t  product_string;
    uint8_t  serial_string;
    uint8_t  configurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;
    uint8_t  interfaces;
    uint8_t  value;
    uint8_t  configuration_string;
    uint8_t  attributes;
    uint8_t  max_power;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t number;
    uint8_t alternate;
    uint8_t endpoints;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_string;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint8_t  address;          /* bit 7: direction, bits 3:0: number */
    uint8_t  attributes;       /* bits 1:0: 2 is bulk                */
    uint16_t max_packet;
    uint8_t  interval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

/* ------------------------------------------------------------------ *
 *  Bulk-only transport
 * ------------------------------------------------------------------ */

#define CBW_SIGNATURE 0x43425355u      /* "USBC" */
#define CSW_SIGNATURE 0x53425355u      /* "USBS" */

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t  flags;                    /* bit 7 set: device to host */
    uint8_t  lun;
    uint8_t  command_length;
    uint8_t  command[16];
} __attribute__((packed)) cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;                   /* 0 good, 1 failed, 2 phase error */
} __attribute__((packed)) csw_t;

/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */

static bool     present;
static uint32_t sectors;
static uint32_t sector_size = 512;
static uint32_t next_tag = 1;
static char     name[32];

/* Transfers go through these rather than the caller's buffer: the controller
 * reads them by physical address, and the identity map only reaches the first
 * gigabyte, which a user-supplied pointer need not be in. */
static cbw_t    cbw_buffer;
static csw_t    csw_buffer;
static uint8_t  data_buffer[4096];

bool usbdisk_present(void)          { return present; }
uint32_t usbdisk_sector_count(void) { return sectors; }
const char *usbdisk_name(void)      { return name; }

/* ------------------------------------------------------------------ *
 *  One SCSI command
 * ------------------------------------------------------------------ */

/* Run a command with at most `length` bytes of data in `data_buffer`.
 * Returns false if the transport failed or the device rejected it. */
static bool scsi(const uint8_t *command, uint8_t command_length,
                 uint32_t length, bool in)
{
    memset(&cbw_buffer, 0, sizeof(cbw_buffer));
    cbw_buffer.signature       = CBW_SIGNATURE;
    cbw_buffer.tag             = next_tag++;
    cbw_buffer.transfer_length = length;
    cbw_buffer.flags           = in ? 0x80 : 0x00;
    cbw_buffer.lun             = 0;
    cbw_buffer.command_length  = command_length;
    memcpy(cbw_buffer.command, command, command_length);

    if (!xhci_bulk(false, &cbw_buffer, sizeof(cbw_buffer)))
        return false;

    if (length && !xhci_bulk(in, data_buffer, length)) {
        /* A device that will not do the data stage halts the endpoint; it
         * still owes a status, which cannot be read until the halt is
         * cleared. */
        xhci_clear_stall(in);
    }

    memset(&csw_buffer, 0, sizeof(csw_buffer));

    if (!xhci_bulk(true, &csw_buffer, sizeof(csw_buffer))) {
        /* One retry after clearing a halt on the in endpoint, which is the
         * usual reason a status does not arrive. */
        xhci_clear_stall(true);
        if (!xhci_bulk(true, &csw_buffer, sizeof(csw_buffer)))
            return false;
    }

    return csw_buffer.signature == CSW_SIGNATURE &&
           csw_buffer.tag == cbw_buffer.tag &&
           csw_buffer.status == 0;
}

/* A device that has just been plugged in reports a unit attention on its first
 * command, and only stops once someone has asked.  Trying a few times is what
 * everybody does. */
static bool test_unit_ready(void)
{
    uint8_t command[6] = { 0x00, 0, 0, 0, 0, 0 };

    for (int i = 0; i < 20; i++)
        if (scsi(command, sizeof(command), 0, true))
            return true;

    return false;
}

static bool inquiry(void)
{
    uint8_t command[6] = { 0x12, 0, 0, 0, 36, 0 };

    if (!scsi(command, sizeof(command), 36, true))
        return false;

    /* Bytes 8..15 are the vendor, 16..31 the product, both space padded. */
    int n = 0;
    for (int i = 8; i < 32 && n < (int)sizeof(name) - 1; i++) {
        char c = (char)data_buffer[i];
        if (c < 32 || c > 126)
            c = ' ';
        if (c == ' ' && (n == 0 || name[n - 1] == ' '))
            continue;
        name[n++] = c;
    }
    while (n > 0 && name[n - 1] == ' ')
        n--;
    name[n] = '\0';

    return true;
}

static bool read_capacity(void)
{
    uint8_t command[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (!scsi(command, sizeof(command), 8, true))
        return false;

    /* Big endian, and the first field is the address of the last block rather
     * than a count. */
    uint32_t last = ((uint32_t)data_buffer[0] << 24) |
                    ((uint32_t)data_buffer[1] << 16) |
                    ((uint32_t)data_buffer[2] << 8)  |
                     (uint32_t)data_buffer[3];
    sector_size   = ((uint32_t)data_buffer[4] << 24) |
                    ((uint32_t)data_buffer[5] << 16) |
                    ((uint32_t)data_buffer[6] << 8)  |
                     (uint32_t)data_buffer[7];

    if (sector_size != 512)
        return false;          /* the filesystem assumes 512 everywhere */

    sectors = last + 1;
    return sectors != 0;
}

/* ------------------------------------------------------------------ *
 *  Reading and writing
 * ------------------------------------------------------------------ */

bool usbdisk_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    if (!present || !count)
        return false;

    uint8_t *out = buf;

    /* The bounce buffer holds eight sectors, so longer requests are split. */
    while (count) {
        uint8_t chunk = count > 8 ? 8 : count;
        uint32_t bytes = (uint32_t)chunk * 512;

        uint8_t command[10] = {
            0x28, 0,
            (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
            (uint8_t)(lba >> 8),  (uint8_t)lba,
            0, 0, chunk, 0
        };

        if (!scsi(command, sizeof(command), bytes, true))
            return false;

        memcpy(out, data_buffer, bytes);

        out   += bytes;
        lba   += chunk;
        count = (uint8_t)(count - chunk);
    }

    return true;
}

bool usbdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    if (!present || !count)
        return false;

    const uint8_t *in = buf;

    while (count) {
        uint8_t chunk = count > 8 ? 8 : count;
        uint32_t bytes = (uint32_t)chunk * 512;

        memcpy(data_buffer, in, bytes);

        uint8_t command[10] = {
            0x2A, 0,
            (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
            (uint8_t)(lba >> 8),  (uint8_t)lba,
            0, 0, chunk, 0
        };

        if (!scsi(command, sizeof(command), bytes, false))
            return false;

        in    += bytes;
        lba   += chunk;
        count = (uint8_t)(count - chunk);
    }

    return true;
}

/* ------------------------------------------------------------------ *
 *  Finding the device
 * ------------------------------------------------------------------ */

/* Walk a configuration descriptor for a bulk-only mass storage interface and
 * its two bulk endpoints. */
static bool parse_configuration(const uint8_t *buf, uint32_t length,
                                uint8_t *configuration,
                                usb_endpoint_t *in, usb_endpoint_t *out)
{
    const usb_config_descriptor_t *config = (const usb_config_descriptor_t *)buf;

    if (length < sizeof(*config) || config->type != USB_DESC_CONFIG)
        return false;

    *configuration = config->value;

    bool in_interface = false;
    bool have_in = false, have_out = false;
    uint32_t off = config->length;

    while (off + 2 <= length) {
        uint8_t size = buf[off];
        uint8_t type = buf[off + 1];

        if (size < 2 || off + size > length)
            break;

        if (type == 0x04) {                /* interface */
            const usb_interface_descriptor_t *i =
                (const usb_interface_descriptor_t *)(buf + off);

            in_interface = i->interface_class == USB_CLASS_MASS_STORAGE &&
                           i->interface_subclass == USB_SUBCLASS_SCSI &&
                           i->interface_protocol == USB_PROTOCOL_BULK_ONLY;

            /* Only the first such interface is of interest; a second one would
             * be another logical unit of the same device. */
            if (in_interface && (have_in || have_out))
                break;
        } else if (type == 0x05 && in_interface) {     /* endpoint */
            const usb_endpoint_descriptor_t *e =
                (const usb_endpoint_descriptor_t *)(buf + off);

            if ((e->attributes & 0x03) == 0x02) {      /* bulk */
                usb_endpoint_t *slot = (e->address & 0x80) ? in : out;
                bool *have = (e->address & 0x80) ? &have_in : &have_out;

                if (!*have) {
                    slot->number     = e->address & 0x0F;
                    slot->in         = (e->address & 0x80) != 0;
                    slot->max_packet = e->max_packet;
                    *have = true;
                }
            }
        }

        off += size;
    }

    return have_in && have_out;
}

/* Ask one addressed device whether it is a disk, and set it up if it is. */
static bool probe_device(void)
{
    /* The device descriptor first, mostly to learn the real maximum packet
     * size of endpoint 0 -- but the control transfers below are short enough
     * that the assumed one works for all of them. */
    usb_device_descriptor_t device;

    if (!xhci_control(0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0,
                      &device, sizeof(device), true))
        return false;

    /* The configuration descriptor comes in two reads: the header says how
     * long the whole thing is, then it is read again in full. */
    static uint8_t config_buffer[512];
    usb_config_descriptor_t header;

    if (!xhci_control(0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0,
                      &header, sizeof(header), true))
        return false;

    uint32_t total = header.total_length;
    if (total < sizeof(header) || total > sizeof(config_buffer))
        return false;

    if (!xhci_control(0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0,
                      config_buffer, (uint16_t)total, true))
        return false;

    uint8_t configuration;
    usb_endpoint_t bulk_in, bulk_out;

    if (!parse_configuration(config_buffer, total, &configuration,
                             &bulk_in, &bulk_out))
        return false;

    if (!xhci_configure(configuration, &bulk_in, &bulk_out))
        return false;

    if (!test_unit_ready())
        return false;

    inquiry();                       /* only for the name; not fatal */

    if (!read_capacity())
        return false;

    present = true;
    return true;
}

bool usbdisk_init(void)
{
    if (!xhci_init())
        return false;

    /* Every device on the controller in turn: a machine that boots from USB
     * usually has a keyboard on it too, and the first port with something
     * plugged into it is as likely to be that as the disk. */
    for (int i = 0; i < 16 && xhci_next_device(); i++)
        if (probe_device())
            return true;

    return false;
}
