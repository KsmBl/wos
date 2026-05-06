/* Realtek RTL8139 driver. See rtl8139.h. */

#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

/* Register offsets from the I/O base. */
#define REG_IDR0     0x00    /* MAC address, 6 bytes            */
#define REG_TSD0     0x10    /* transmit status, 4 x 4 bytes    */
#define REG_TSAD0    0x20    /* transmit start address, 4 x 4   */
#define REG_RBSTART  0x30    /* receive buffer physical address */
#define REG_CR       0x37    /* command                         */
#define REG_CAPR     0x38    /* current address of packet read  */
#define REG_IMR      0x3C    /* interrupt mask                  */
#define REG_ISR      0x3E    /* interrupt status                */
#define REG_TCR      0x40    /* transmit config                 */
#define REG_RCR      0x44    /* receive config                  */
#define REG_CONFIG1  0x52

#define CR_BUFE      0x01    /* receive buffer empty            */
#define CR_TE        0x04    /* transmitter enable              */
#define CR_RE        0x08    /* receiver enable                 */
#define CR_RST       0x10    /* software reset                  */

#define ISR_ROK      0x01
#define ISR_TOK      0x04

#define TSD_OWN      0x2000  /* clear while the card is sending */

/* The receive ring is 8 KiB; WRAP lets the card run a packet past the end into
 * the slack rather than splitting it, so a whole frame is always contiguous. */
#define RX_RING      8192
#define RX_SLACK     1600
#define RX_BUF_SIZE  (RX_RING + 16 + RX_SLACK)

#define TX_SLOTS     4
#define TX_BUF_SIZE  2048

static uint16_t io_base;
static uint8_t  mac[6];

static uint8_t *rx_buf;
static uint32_t rx_offset;                 /* our read cursor into the ring */

static uint8_t *tx_buf[TX_SLOTS];
static int      tx_cur;

static inline uint32_t phys_of(const void *p)
{
    /* The kernel heap is identity mapped, so its virtual and physical
     * addresses are the same -- exactly what a 32-bit DMA engine needs. */
    return (uint32_t)(uintptr_t)p;
}

bool rtl8139_init(void)
{
    pci_device_t dev = pci_find(0x10EC, 0x8139);
    if (!dev.found)
        return false;

    pci_enable_bus_master(&dev);
    io_base = (uint16_t)(dev.bar0 & ~0x3u);     /* BAR0 is an I/O BAR */

    /* Power on, then software reset and wait for it to complete. */
    outb(io_base + REG_CONFIG1, 0x00);
    outb(io_base + REG_CR, CR_RST);
    for (int i = 0; i < 100000 && (inb(io_base + REG_CR) & CR_RST); i++)
        io_wait();

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + REG_IDR0 + i);

    rx_buf = kmalloc(RX_BUF_SIZE);
    for (int i = 0; i < TX_SLOTS; i++)
        tx_buf[i] = kmalloc(TX_BUF_SIZE);
    if (!rx_buf) {
        for (int i = 0; i < TX_SLOTS; i++)
            if (!tx_buf[i]) return false;
        return false;
    }
    memset(rx_buf, 0, RX_BUF_SIZE);

    outl(io_base + REG_RBSTART, phys_of(rx_buf));
    rx_offset = 0;
    outw(io_base + REG_CAPR, (uint16_t)(0 - 16));

    /* Poll, so mask every interrupt; ISR still records events for us to read. */
    outw(io_base + REG_IMR, 0x0000);

    /* Accept broadcast (for ARP) and frames addressed to us; WRAP set, no DMA
     * burst limit, no receive threshold limit. */
    outl(io_base + REG_RCR, 0x0000000A | (1u << 7) | (7u << 8) | (7u << 13));

    outb(io_base + REG_CR, CR_RE | CR_TE);

    kprintf("net    : rtl8139 at io 0x%x, mac %x:%x:%x:%x:%x:%x\n",
            io_base, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

const uint8_t *rtl8139_mac(void) { return mac; }

int rtl8139_send(const void *frame, uint32_t len)
{
    if (len > TX_BUF_SIZE)
        return -1;
    if (len < 60)
        len = 60;               /* pad to the minimum Ethernet frame */

    int slot = tx_cur;
    tx_cur = (tx_cur + 1) % TX_SLOTS;

    memcpy(tx_buf[slot], frame, len);
    outl(io_base + REG_TSAD0 + slot * 4, phys_of(tx_buf[slot]));

    /* Writing the length (with OWN clear) starts the transfer. */
    outl(io_base + REG_TSD0 + slot * 4, len);

    /* Wait for the card to take it. */
    for (int i = 0; i < 100000; i++) {
        if (inl(io_base + REG_TSD0 + slot * 4) & TSD_OWN)
            return 0;
        io_wait();
    }
    return -1;
}

uint32_t rtl8139_poll(void *out, uint32_t cap)
{
    if (inb(io_base + REG_CR) & CR_BUFE)
        return 0;                           /* ring empty */

    /* Acknowledge the receive so the card keeps delivering. */
    outw(io_base + REG_ISR, ISR_ROK);

    uint8_t *p = rx_buf + rx_offset;
    uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t len    = (uint16_t)(p[2] | (p[3] << 8));   /* includes 4-byte CRC */

    uint32_t payload = 0;
    if ((status & ISR_ROK) && len >= 4) {
        payload = (uint32_t)(len - 4);
        if (payload > cap)
            payload = cap;
        memcpy(out, p + 4, payload);
    }

    /* Advance past this packet, dword-aligned, wrapping within the ring. */
    rx_offset = (rx_offset + len + 4 + 3) & ~3u;
    rx_offset %= RX_RING;
    outw(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));

    return payload;
}
