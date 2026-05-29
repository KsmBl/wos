/* xHCI host controller. See xhci.h.
 *
 * The shape of the thing: the controller owns a set of rings in main memory and
 * the driver owns the other end of each.  Work is posted by writing a Transfer
 * Request Block into a ring and ringing a doorbell register; the controller
 * answers by writing a completion event into the event ring, which the driver
 * reads back.  Commands to the controller itself go the same way, through the
 * command ring and doorbell 0.
 *
 * Everything the controller reads has to be physical memory it can reach, which
 * here means memory below a gigabyte: that region is identity mapped, so a
 * kernel pointer to it is already the physical address the controller wants.
 * That is why every structure below comes from pmm_alloc_frame_low().
 */

#include "xhci.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

/* ------------------------------------------------------------------ *
 *  Registers
 * ------------------------------------------------------------------ */

/* Capability registers, at the start of the register space. */
#define CAP_CAPLENGTH   0x00        /* byte: where the operational regs begin */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* Operational registers, at CAPLENGTH. */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(p)    (0x400 + 0x10 * ((p) - 1))

#define USBCMD_RS       (1u << 0)   /* run */
#define USBCMD_HCRST    (1u << 1)   /* reset */
#define USBCMD_INTE     (1u << 2)

#define USBSTS_HCH      (1u << 0)   /* halted */
#define USBSTS_HSE      (1u << 2)   /* host system error */
#define USBSTS_CNR      (1u << 11)  /* controller not ready */

#define PORTSC_CCS      (1u << 0)   /* a device is connected */
#define PORTSC_PED      (1u << 1)   /* port enabled */
#define PORTSC_PR       (1u << 4)   /* reset */
#define PORTSC_PP       (1u << 9)   /* power */
#define PORTSC_CSC      (1u << 17)  /* connect status changed */
#define PORTSC_PRC      (1u << 21)  /* reset complete */
/* The change flags, cleared by writing a one.
 *
 * PED and PR are the same kind of trap and are easier to forget: PED reports
 * that the port is enabled, and writing that same one back *disables* it.  A
 * machine that booted from a USB stick arrives here with the port enabled by
 * its firmware, so a read-modify-write that keeps PED switches off the port the
 * disk is on -- and the disk then looks like a device that was never there.
 * PORTSC_KEEP drops all three, and everything that writes PORTSC goes through
 * it. */
#define PORTSC_RW1CS    0x00FE0000u
#define PORTSC_KEEP(sc) ((sc) & ~(PORTSC_RW1CS | PORTSC_PED | PORTSC_PR))
#define PORTSC_SPEED(v) (((v) >> 10) & 0xF)

/* Runtime registers, at RTSOFF; interrupter 0 is at +0x20. */
#define RT_IMAN         0x20
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38

/* ------------------------------------------------------------------ *
 *  Transfer Request Blocks
 * ------------------------------------------------------------------ */

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) trb_t;

/* TRB types, in bits [15:10] of the control field. */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_RESET_EP        14
#define TRB_TRANSFER_EVENT  32
#define TRB_COMMAND_EVENT   33
#define TRB_PORT_EVENT      34

#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(c)  (((c) >> 10) & 0x3F)
#define TRB_CYCLE       (1u << 0)
#define TRB_ENT         (1u << 1)
#define TRB_ISP         (1u << 2)    /* interrupt on short packet */
#define TRB_CHAIN       (1u << 4)
#define TRB_IOC         (1u << 5)    /* interrupt on completion */
#define TRB_IDT         (1u << 6)    /* immediate data */
#define TRB_TOGGLE      (1u << 1)    /* on a link TRB: flip the cycle state */
#define TRB_DIR_IN      (1u << 16)

#define COMPLETION_OF(status) (((status) >> 24) & 0xFF)
#define COMPLETION_SUCCESS    1
#define COMPLETION_SHORT      13

/* A ring the driver writes and the controller reads.  One page, minus the link
 * TRB at the end that points back to the start. */
#define RING_TRBS   (4096 / (int)sizeof(trb_t))
#define RING_USABLE (RING_TRBS - 1)

typedef struct {
    trb_t   *trbs;
    uint32_t index;
    uint8_t  cycle;
} ring_t;

/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */

static volatile uint8_t *cap;        /* capability registers      */
static volatile uint8_t *op;         /* operational registers     */
static volatile uint8_t *rt;         /* runtime registers         */
static volatile uint32_t *doorbell;

static uint32_t max_slots, max_ports;
static uint32_t context_size;        /* 32 or 64 bytes, per HCCPARAMS1.CSZ */

static uint64_t *dcbaa;
static ring_t    cmd_ring;
static ring_t    event_ring;
static trb_t    *event_trbs;
static uint32_t  event_index;
static uint8_t   event_cycle = 1;

static uint8_t   slot_id;
static uint8_t   root_port;
static uint32_t  port_cursor = 1;    /* the next root port to try */
static uint32_t  port_speed;
static void     *input_context;
static void     *device_context;

static ring_t ep0_ring;
static ring_t bulk_in_ring;
static ring_t bulk_out_ring;
static uint8_t bulk_in_dci, bulk_out_dci;

static bool ready;

/* Why the last attempt got no further, for the boot log.  A machine that finds
 * no disk is the one case where the kernel cannot show its working any other
 * way -- there is no device to ask afterwards. */
static const char *failure = "not tried";

bool xhci_device_ready(void) { return ready; }

const char *xhci_error(void) { return failure; }

/* ------------------------------------------------------------------ *
 *  Register access
 * ------------------------------------------------------------------ */

static uint32_t rd32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static void wr32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* The specification requires 64-bit registers to be written as two 32-bit
 * halves where the controller does not support 64-bit access, and allows it
 * everywhere, so everything here does it that way. */
static void wr64(volatile uint8_t *base, uint32_t off, uint64_t v)
{
    wr32(base, off, (uint32_t)v);
    wr32(base, off + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ *
 *  Memory
 * ------------------------------------------------------------------ */

/* A zeroed page below a gigabyte, where the identity map makes the pointer and
 * the physical address the same number. */
static void *dma_page(void)
{
    uint64_t phys = pmm_alloc_frame_low();
    if (!phys)
        return NULL;

    memset((void *)(uintptr_t)phys, 0, PAGE_SIZE);
    return (void *)(uintptr_t)phys;
}

static bool ring_init(ring_t *r)
{
    r->trbs = dma_page();
    if (!r->trbs)
        return false;

    r->index = 0;
    r->cycle = 1;

    /* The last TRB sends the controller back to the start, and flips the cycle
     * bit it compares against so the ring can be told apart from itself one lap
     * later. */
    r->trbs[RING_USABLE].parameter = (uint64_t)(uintptr_t)r->trbs;
    r->trbs[RING_USABLE].status    = 0;
    r->trbs[RING_USABLE].control   = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | r->cycle;

    return true;
}

/* Put one TRB on a ring.  The cycle bit is written last, because that is the
 * bit the controller watches to decide the entry is real. */
static void ring_push(ring_t *r, uint64_t parameter, uint32_t status,
                      uint32_t control)
{
    trb_t *t = &r->trbs[r->index];

    t->parameter = parameter;
    t->status    = status;
    t->control   = control | r->cycle;

    if (++r->index == RING_USABLE) {
        /* Hand the link TRB to the controller with the current cycle, then
         * carry on at the top with the cycle inverted. */
        r->trbs[RING_USABLE].control =
            TRB_TYPE(TRB_LINK) | TRB_TOGGLE | r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
}

/* ------------------------------------------------------------------ *
 *  Events
 * ------------------------------------------------------------------ */

/* Wait for the next event the controller posts, for at most `ms`.
 * Returns false on timeout. */
static bool event_wait(trb_t *out, uint32_t ms)
{
    uint32_t deadline = pit_ticks() + (ms * PIT_HZ) / 1000 + 2;

    for (;;) {
        trb_t *e = &event_trbs[event_index];

        if ((e->control & TRB_CYCLE) == event_cycle) {
            *out = *e;

            if (++event_index == RING_TRBS) {
                event_index = 0;
                event_cycle ^= 1;
            }

            /* Tell the controller how far we have read.  Bit 3 is the busy
             * flag, written back as a one to clear it. */
            wr64(rt, RT_ERDP,
                 (uint64_t)(uintptr_t)&event_trbs[event_index] | (1u << 3));
            return true;
        }

        if (rd32(op, OP_USBSTS) & USBSTS_HSE)
            return false;

        if ((int32_t)(pit_ticks() - deadline) > 0)
            return false;

        __asm__ volatile("pause");
    }
}

/* Wait for an event of a particular kind, discarding the others -- port status
 * changes arrive unasked for and are of no interest once a device is up. */
static bool event_wait_type(trb_t *out, uint32_t type, uint32_t ms)
{
    for (int i = 0; i < 64; i++) {
        if (!event_wait(out, ms))
            return false;
        if (TRB_TYPE_OF(out->control) == type)
            return true;
    }
    return false;
}

/* Post a command and wait for its completion event. */
static bool command(uint64_t parameter, uint32_t status, uint32_t control,
                    trb_t *completion)
{
    trb_t event;

    ring_push(&cmd_ring, parameter, status, control);
    doorbell[0] = 0;

    if (!event_wait_type(&event, TRB_COMMAND_EVENT, 1000))
        return false;

    if (completion)
        *completion = event;

    return COMPLETION_OF(event.status) == COMPLETION_SUCCESS;
}

/* ------------------------------------------------------------------ *
 *  Contexts
 *
 *  A device is described to the controller by a slot context followed by one
 *  context per endpoint, each either 32 or 64 bytes depending on the
 *  controller.  The input context used to change any of that has one extra
 *  control context in front, saying which of the others to look at.
 * ------------------------------------------------------------------ */

static uint32_t *context_at(void *base, int index)
{
    return (uint32_t *)((uint8_t *)base + (uint64_t)index * context_size);
}

/* The device context index of an endpoint: 1 is control, and each endpoint
 * after that takes two, out before in. */
static uint8_t dci_of(uint8_t number, bool in)
{
    return (uint8_t)(number * 2 + (in ? 1 : 0));
}

/* The maximum packet size a control endpoint has before anything has been read
 * from the device, which depends only on how fast the port trained. */
static uint16_t default_max_packet(void)
{
    switch (port_speed) {
    case 1: return 8;      /* full speed: 8 is the safe assumption */
    case 2: return 8;      /* low speed  */
    case 3: return 64;     /* high speed */
    default: return 512;   /* super speed and above */
    }
}

/* ------------------------------------------------------------------ *
 *  Bringing the controller up
 * ------------------------------------------------------------------ */

/* Ask the firmware to let go.
 *
 * On a machine that booted through the BIOS, or one whose UEFI driver is still
 * attached, the controller belongs to the firmware until it is asked for it.
 * Taking it without asking leaves two drivers writing the same registers. */
static void take_ownership(void)
{
    uint32_t hcc = rd32(cap, CAP_HCCPARAMS1);
    uint32_t off = (hcc >> 16) & 0xFFFF;     /* in 32-bit words from cap */

    while (off) {
        volatile uint8_t *ext = cap + off * 4;
        uint32_t head = rd32(ext, 0);

        if ((head & 0xFF) == 1) {            /* USB legacy support */
            /* Bit 24 is ours to set, bit 16 is the firmware's to clear. */
            wr32(ext, 0, head | (1u << 24));

            for (int i = 0; i < 1000; i++) {
                if (!(rd32(ext, 0) & (1u << 16)))
                    break;
                pit_sleep(1);
            }

            /* And stop it raising system management interrupts at us. */
            wr32(ext, 4, rd32(ext, 4) & ~0xE0001FE0u);
            return;
        }

        uint32_t next = (head >> 8) & 0xFF;
        if (!next)
            return;
        off += next;
    }
}

static bool reset_controller(void)
{
    /* Stop it first: resetting a running controller is undefined. */
    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) & ~USBCMD_RS);

    for (int i = 0; i < 100 && !(rd32(op, OP_USBSTS) & USBSTS_HCH); i++)
        pit_sleep(10);

    wr32(op, OP_USBCMD, USBCMD_HCRST);

    for (int i = 0; i < 200; i++) {
        if (!(rd32(op, OP_USBCMD) & USBCMD_HCRST) &&
            !(rd32(op, OP_USBSTS) & USBSTS_CNR))
            return true;
        pit_sleep(10);
    }

    return false;
}

/* The controller may want pages of its own to work in, and says how many.
 * Withholding them from a controller that asked is a hang later. */
static bool allocate_scratchpad(void)
{
    uint32_t hcs2 = rd32(cap, CAP_HCSPARAMS2);
    uint32_t count = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);

    if (!count)
        return true;

    uint64_t *array = dma_page();
    if (!array)
        return false;

    if (count > PAGE_SIZE / sizeof(uint64_t))
        count = PAGE_SIZE / sizeof(uint64_t);

    for (uint32_t i = 0; i < count; i++) {
        void *page = dma_page();
        if (!page)
            return false;
        array[i] = (uint64_t)(uintptr_t)page;
    }

    dcbaa[0] = (uint64_t)(uintptr_t)array;
    return true;
}

/* Find the next port with something on it, reset it, and return its number.
 * Zero when there are no more. */
static uint8_t find_port(void)
{
    for (uint32_t p = port_cursor; p <= max_ports; p++) {
        port_cursor = p + 1;
        uint32_t sc = rd32(op, OP_PORTSC(p));

        if (!(sc & PORTSC_CCS))
            continue;

        /* Powered ports that are already enabled trained themselves, which is
         * what a USB 3 port does on connect.  A USB 2 port needs telling. */
        if (!(sc & PORTSC_PED)) {
            wr32(op, OP_PORTSC(p), PORTSC_KEEP(sc) | PORTSC_PR);

            for (int i = 0; i < 100; i++) {
                pit_sleep(10);
                sc = rd32(op, OP_PORTSC(p));
                if (sc & PORTSC_PRC)
                    break;
            }

            /* Acknowledge the change bits, which stay set until written. */
            wr32(op, OP_PORTSC(p), PORTSC_KEEP(sc) | PORTSC_PRC | PORTSC_CSC);
            sc = rd32(op, OP_PORTSC(p));
        }

        if (sc & PORTSC_PED) {
            port_speed = PORTSC_SPEED(sc);

            /* A device is allowed to take its time between the port coming up
             * and being able to answer; the specification's figure is 10 ms,
             * and nothing is lost by giving it more. */
            pit_sleep(50);
            return (uint8_t)p;
        }
    }

    port_cursor = max_ports + 1;
    return 0;
}

/* Give the device an address, and endpoint 0 a ring to talk on. */
static bool address_device(void)
{
    trb_t completion;

    /* Give back the slot the last device had: the controller has a limited
     * number of them, and nothing here talks to two devices at once. */
    if (slot_id) {
        command(0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24),
                NULL);
        dcbaa[slot_id] = 0;
        slot_id = 0;
        ready = false;
    }

    if (!command(0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &completion))
        return false;

    slot_id = (uint8_t)(completion.control >> 24);
    if (!slot_id || slot_id > max_slots)
        return false;

    if (!input_context)
        input_context = dma_page();
    if (!device_context)
        device_context = dma_page();
    if (!input_context || !device_context)
        return false;

    memset(input_context, 0, PAGE_SIZE);
    memset(device_context, 0, PAGE_SIZE);

    if (!ep0_ring.trbs && !ring_init(&ep0_ring))
        return false;

    ep0_ring.index = 0;
    ep0_ring.cycle = 1;
    memset(ep0_ring.trbs, 0, PAGE_SIZE);
    ep0_ring.trbs[RING_USABLE].parameter = (uint64_t)(uintptr_t)ep0_ring.trbs;
    ep0_ring.trbs[RING_USABLE].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | 1;

    dcbaa[slot_id] = (uint64_t)(uintptr_t)device_context;

    /* Control context: add the slot context and endpoint 0. */
    uint32_t *add = context_at(input_context, 0);
    add[1] = 0x3;

    /* Slot context: one endpoint, no hubs in the way, on this root port. */
    uint32_t *slot = context_at(input_context, 1);
    slot[0] = (1u << 27) | (port_speed << 20);
    slot[1] = (uint32_t)root_port << 16;

    /* Endpoint 0 context: a control endpoint on the ring just built. */
    uint32_t *ep0 = context_at(input_context, 2);
    ep0[1] = (4u << 3)                                  /* control endpoint */
           | ((uint32_t)default_max_packet() << 16)
           | (3u << 1);                                 /* error count      */
    ep0[2] = (uint32_t)(uintptr_t)ep0_ring.trbs | 1;    /* dequeue | cycle  */
    ep0[3] = 0;
    ep0[4] = 8;                                         /* average length   */

    return command((uint64_t)(uintptr_t)input_context, 0,
                   TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24),
                   NULL);
}

/* ------------------------------------------------------------------ *
 *  Transfers
 * ------------------------------------------------------------------ */

bool xhci_control(uint8_t request_type, uint8_t request, uint16_t value,
                  uint16_t index, void *data, uint16_t length, bool in)
{
    if (!slot_id)
        return false;

    /* The setup packet travels inside the TRB rather than being pointed at. */
    uint64_t setup = (uint64_t)request_type
                   | ((uint64_t)request << 8)
                   | ((uint64_t)value << 16)
                   | ((uint64_t)index << 32)
                   | ((uint64_t)length << 48);

    /* Transfer type: 0 no data, 2 out, 3 in. */
    uint32_t transfer_type = length ? (in ? 3u : 2u) : 0u;

    ring_push(&ep0_ring, setup, 8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (transfer_type << 16));

    if (length)
        ring_push(&ep0_ring, (uint64_t)(uintptr_t)data, length,
                  TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));

    /* The status stage runs the opposite way to the data, and is the one that
     * asks for an interrupt: its completion means the whole thing is done. */
    ring_push(&ep0_ring, 0, 0,
              TRB_TYPE(TRB_STATUS) | TRB_IOC | (in ? 0 : TRB_DIR_IN));

    doorbell[slot_id] = 1;                 /* endpoint 0 */

    trb_t event;
    if (!event_wait_type(&event, TRB_TRANSFER_EVENT, 1000))
        return false;

    uint32_t code = COMPLETION_OF(event.status);
    return code == COMPLETION_SUCCESS || code == COMPLETION_SHORT;
}

bool xhci_configure(uint8_t configuration, const usb_endpoint_t *in,
                    const usb_endpoint_t *out)
{
    if (!slot_id || !in || !out)
        return false;

    if (!ring_init(&bulk_in_ring) || !ring_init(&bulk_out_ring))
        return false;

    bulk_in_dci  = dci_of(in->number, true);
    bulk_out_dci = dci_of(out->number, false);

    uint8_t last = bulk_in_dci > bulk_out_dci ? bulk_in_dci : bulk_out_dci;

    memset(input_context, 0, PAGE_SIZE);

    uint32_t *add = context_at(input_context, 0);
    add[1] = 1u                                  /* the slot context   */
           | (1u << bulk_in_dci)
           | (1u << bulk_out_dci);

    /* The slot context is copied from the device's own, with the endpoint
     * count raised to cover the new endpoints. */
    uint32_t *slot = context_at(input_context, 1);
    uint32_t *live = context_at(device_context, 0);
    slot[0] = (live[0] & 0x07FFFFFF) | ((uint32_t)last << 27);
    slot[1] = live[1];
    slot[2] = live[2];
    slot[3] = live[3];

    /* Endpoint type 2 is bulk out, 6 is bulk in. */
    uint32_t *ep_in = context_at(input_context, bulk_in_dci + 1);
    ep_in[1] = (6u << 3) | ((uint32_t)in->max_packet << 16) | (3u << 1);
    ep_in[2] = (uint32_t)(uintptr_t)bulk_in_ring.trbs | 1;
    ep_in[4] = in->max_packet;

    uint32_t *ep_out = context_at(input_context, bulk_out_dci + 1);
    ep_out[1] = (2u << 3) | ((uint32_t)out->max_packet << 16) | (3u << 1);
    ep_out[2] = (uint32_t)(uintptr_t)bulk_out_ring.trbs | 1;
    ep_out[4] = out->max_packet;

    /* SET_CONFIGURATION first, then tell the controller about the endpoints
     * the configuration brought with it. */
    if (!xhci_control(0x00, 0x09, configuration, 0, NULL, 0, false))
        return false;

    if (!command((uint64_t)(uintptr_t)input_context, 0,
                 TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)slot_id << 24), NULL))
        return false;

    ready = true;
    return true;
}

bool xhci_bulk(bool in, void *data, uint32_t length)
{
    if (!ready)
        return false;

    ring_t *ring = in ? &bulk_in_ring : &bulk_out_ring;
    uint8_t dci  = in ? bulk_in_dci : bulk_out_dci;

    /* The transfer length field is 17 bits, and one TRB is all that is needed
     * for the block sizes this driver asks for. */
    if (length > 0x10000)
        return false;

    ring_push(ring, (uint64_t)(uintptr_t)data, length,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);

    doorbell[slot_id] = dci;

    trb_t event;
    if (!event_wait_type(&event, TRB_TRANSFER_EVENT, 5000))
        return false;

    uint32_t code = COMPLETION_OF(event.status);
    return code == COMPLETION_SUCCESS || code == COMPLETION_SHORT;
}

bool xhci_clear_stall(bool in)
{
    uint8_t dci = in ? bulk_in_dci : bulk_out_dci;
    ring_t *ring = in ? &bulk_in_ring : &bulk_out_ring;

    /* Two halves: the controller's idea of the endpoint, and the device's. */
    if (!command(0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)slot_id << 24)
                     | ((uint32_t)dci << 16), NULL))
        return false;

    /* CLEAR_FEATURE(ENDPOINT_HALT) on the endpoint itself. */
    uint8_t address = (uint8_t)((in ? 0x80 : 0x00) |
                                (in ? bulk_in_dci / 2 : bulk_out_dci / 2));
    if (!xhci_control(0x02, 0x01, 0, address, NULL, 0, false))
        return false;

    /* Both sides are back to the start of the ring now. */
    ring->index = 0;
    ring->cycle = 1;
    memset(ring->trbs, 0, PAGE_SIZE);
    ring->trbs[RING_USABLE].parameter = (uint64_t)(uintptr_t)ring->trbs;
    ring->trbs[RING_USABLE].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | 1;

    return true;
}

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */

/* Bring up controller number `index`.
 *
 * Returns 1 when it is running with something plugged into it, 0 when there is
 * no such controller -- which ends the search, since they are numbered in order
 * -- and -1 when there is one but it was no use. */
static int start_controller(int index)
{
    /* 0x0C/0x03/0x30: serial bus controller, USB, xHCI. */
    pci_device_t dev = pci_find_class_index(0x0C, 0x03, 0x30, index);
    if (!dev.found) {
        if (index == 0)
            failure = "no xHCI controller on the PCI bus";
        return 0;
    }

    uint64_t bar = pci_bar_address(&dev, 0);
    if (!bar) {
        failure = "the controller has no memory-mapped registers";
        return -1;
    }

    pci_power_on(&dev);
    pci_enable_bus_master(&dev);

    cap = paging_map_device(bar, 0x10000);
    if (!cap) {
        failure = "the controller's registers could not be mapped";
        return -1;
    }

    /* Everything below belongs to this controller, not the last one. */
    slot_id     = 0;
    ready       = false;
    port_cursor = 1;
    ep0_ring.trbs = NULL;
    bulk_in_ring.trbs = NULL;
    bulk_out_ring.trbs = NULL;
    event_index = 0;
    event_cycle = 1;

    op = cap + (rd32(cap, CAP_CAPLENGTH) & 0xFF);
    rt = cap + (rd32(cap, CAP_RTSOFF) & ~0x1Fu);
    doorbell = (volatile uint32_t *)(cap + (rd32(cap, CAP_DBOFF) & ~0x3u));

    if (rd32(cap, CAP_CAPLENGTH) == 0xFFFFFFFFu) {
        failure = "the controller's registers read as all-ones";
        return -1;
    }

    uint32_t hcs1 = rd32(cap, CAP_HCSPARAMS1);
    max_slots = hcs1 & 0xFF;
    max_ports = (hcs1 >> 24) & 0xFF;
    context_size = (rd32(cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

    if (!max_slots || !max_ports) {
        failure = "the controller reports no slots or no ports";
        return -1;
    }

    take_ownership();

    if (!reset_controller()) {
        failure = "the controller would not reset";
        return -1;
    }

    /* One slot is all this driver ever uses. */
    wr32(op, OP_CONFIG, (rd32(op, OP_CONFIG) & ~0xFFu) | max_slots);

    dcbaa = dma_page();
    if (!dcbaa || !allocate_scratchpad()) {
        failure = "out of memory for the controller's own structures";
        return -1;
    }
    wr64(op, OP_DCBAAP, (uint64_t)(uintptr_t)dcbaa);

    if (!ring_init(&cmd_ring)) {
        failure = "out of memory for the command ring";
        return -1;
    }
    wr64(op, OP_CRCR, (uint64_t)(uintptr_t)cmd_ring.trbs | 1);

    /* The event ring is described to the controller by a table of segments;
     * one segment is enough. */
    event_trbs = dma_page();
    uint64_t *erst = dma_page();
    if (!event_trbs || !erst) {
        failure = "out of memory for the event ring";
        return -1;
    }

    erst[0] = (uint64_t)(uintptr_t)event_trbs;
    erst[1] = RING_TRBS;                 /* segment size, in TRBs */

    wr32(rt, RT_ERSTSZ, 1);
    wr64(rt, RT_ERDP, (uint64_t)(uintptr_t)event_trbs);
    wr64(rt, RT_ERSTBA, (uint64_t)(uintptr_t)erst);

    event_index = 0;
    event_cycle = 1;
    (void)event_ring;

    /* Run. */
    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) | USBCMD_RS);

    for (int i = 0; i < 100 && (rd32(op, OP_USBSTS) & USBSTS_HCH); i++)
        pit_sleep(10);

    if (rd32(op, OP_USBSTS) & USBSTS_HCH) {
        failure = "the controller would not start";
        return -1;
    }

    /* Power the ports.  Firmware is entitled to leave them off, and a port with
     * no power on it reports nothing connected however long it is watched --
     * which looks exactly like an empty machine. */
    for (uint32_t p = 1; p <= max_ports; p++) {
        uint32_t sc = rd32(op, OP_PORTSC(p));
        if (!(sc & PORTSC_PP))
            wr32(op, OP_PORTSC(p), PORTSC_KEEP(sc) | PORTSC_PP);
    }

    /* And wait for something to appear.  A device that is already plugged in
     * still has to be seen, trained and reported; giving up sooner is how a
     * disk that is plainly there gets missed.  A second is the ceiling, paid
     * only by a machine with a controller and nothing on it -- as soon as any
     * port reports a connection, this stops. */
    for (int i = 0; i < 10; i++) {
        pit_sleep(100);

        int connected = 0;
        for (uint32_t p = 1; p <= max_ports; p++)
            if (rd32(op, OP_PORTSC(p)) & PORTSC_CCS)
                connected++;

        if (connected) {
            kprintf("usb    : controller %d, %u ports, %d with a device\n",
                    index, max_ports, connected);
            failure = "a device answered but is not a disk";
            return 1;
        }
    }

    /* The ports as they actually are.  A machine that finds nothing is the one
     * that most needs this: whether the ports have power, whether anything is
     * connected, and what state the link is in are all in these bits, and there
     * is no way to ask afterwards. */
    kprintf("usb    : controller %d, %u ports, nothing plugged in; portsc",
            index, max_ports);
    for (uint32_t p = 1; p <= max_ports && p <= 8; p++)
        kprintf(" %x", rd32(op, OP_PORTSC(p)));
    kputs("\n");
    failure = "nothing is plugged into any USB port";
    return -1;
}

/* The controller being used, and how many have been tried. */
static int controller_index = -1;

bool xhci_init(void)
{
    /* Every controller in turn: a machine with two of them keeps its front
     * sockets on one and its back sockets on the other as often as not, and
     * the disk is on whichever it is on. */
    for (int i = 0; i < 4; i++) {
        int r = start_controller(i);

        if (r > 0) {
            controller_index = i;
            return true;
        }
        if (r == 0)
            break;              /* no controller number i, so none after it */
    }

    return false;
}

/* Move to the next controller, for when this one had no disk on it. */
bool xhci_next_controller(void)
{
    for (int i = controller_index + 1; i < 4; i++) {
        int r = start_controller(i);

        if (r > 0) {
            controller_index = i;
            return true;
        }
        if (r == 0)
            break;
    }

    return false;
}

bool xhci_next_device(void)
{
    if (!op)
        return false;

    root_port = find_port();
    if (!root_port)
        return false;

    if (!address_device()) {
        kprintf("usb    : port %u would not take an address\n", root_port);
        failure = "a device would not take an address";
        return false;
    }

    return true;
}

uint8_t xhci_port(void) { return root_port; }
