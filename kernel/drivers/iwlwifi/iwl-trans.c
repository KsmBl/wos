/* The transport for the Intel 9000-series adapter. See iwl-trans.h.
 *
 * The power-up handshake, the firmware loading sequence and the register
 * values they use are derived from the iwlwifi driver in the Linux kernel,
 * which is dual licensed and used here under BSD-3-Clause.  The copyright
 * notice and the reasoning are at the top of iwl-regs.h; the README says the
 * same in the place somebody would look first.
 */

#include "iwl-trans.h"
#include "iwlwifi.h"
#include "pci.h"
#include "paging.h"
#include "pit.h"
#include "smp.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"

/* Intel's identifier, and the devices of this generation.  The 9560 in this
 * machine is 0x9df0 -- a CNVi part, where the radio is in the chipset -- and
 * the others are the discrete cards of the same family, which are programmed
 * identically. */
#define IWL_VENDOR_INTEL 0x8086

static const uint16_t supported_devices[] = {
    0x9df0,   /* Wireless-AC 9560, CNVi, as in this laptop      */
    0xa370,   /* Wireless-AC 9462/9560, the other CNVi identifier */
    0x2526,   /* Wireless-AC 9260, discrete                     */
    0x271b,   /* Wireless-AC 9160                               */
    0x30dc,   /* Wireless-AC 9560, yet another                  */
    0x31dc,   /* Wireless-AC 9560                               */
};

/* How long to wait for the device to answer a handshake.  These are generous:
 * the cost of waiting too long is a slow boot on a broken adapter, and the
 * cost of waiting too little is a driver that fails on a slow machine. */
#define IWL_POLL_TIMEOUT_MS   200
#define IWL_ALIVE_TIMEOUT_MS  2000

/* ------------------------------------------------------------------ *
 *  Register access
 * ------------------------------------------------------------------ */

uint32_t iwl_read32(iwl_trans_t *trans, uint32_t offset)
{
    return *(volatile uint32_t *)(trans->regs + offset);
}

void iwl_write32(iwl_trans_t *trans, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(trans->regs + offset) = value;
}

static void iwl_set_bit(iwl_trans_t *trans, uint32_t offset, uint32_t bits)
{
    iwl_write32(trans, offset, iwl_read32(trans, offset) | bits);
}

static void iwl_clear_bit(iwl_trans_t *trans, uint32_t offset, uint32_t bits)
{
    iwl_write32(trans, offset, iwl_read32(trans, offset) & ~bits);
}

/* Spin until a register reads what is wanted, or the time runs out. */
static bool iwl_poll_bit(iwl_trans_t *trans, uint32_t offset,
                         uint32_t bits, uint32_t mask, uint32_t timeout_ms)
{
    uint64_t deadline = time_now_ms() + timeout_ms;

    do {
        if ((iwl_read32(trans, offset) & mask) == bits)
            return true;
        io_wait();
    } while (time_now_ms() < deadline);

    return false;
}

/* Read a word of the device's own memory through the window at the bottom of
 * the register block: an address is written to one register and the word at
 * it comes back from another.
 *
 * This is how the driver can check its own work.  A DMA transfer that reports
 * nothing might have failed or might merely have failed to announce itself,
 * and those are completely different problems -- reading back what was
 * supposed to have been written tells them apart. */
uint32_t iwl_read_mem32(iwl_trans_t *trans, uint32_t address)
{
    iwl_write32(trans, HBUS_TARG_MEM_RADDR, address);
    return iwl_read32(trans, HBUS_TARG_MEM_RDAT);
}

void iwl_write_mem32(iwl_trans_t *trans, uint32_t address, uint32_t value)
{
    iwl_write32(trans, HBUS_TARG_MEM_WADDR, address);
    iwl_write32(trans, HBUS_TARG_MEM_WDAT, value);
}

uint32_t iwl_read_prph(iwl_trans_t *trans, uint32_t address)
{
    iwl_write32(trans, HBUS_TARG_PRPH_RADDR,
                ((address & 0x000FFFFF) | HBUS_PRPH_ACCESS_MASK));
    return iwl_read32(trans, HBUS_TARG_PRPH_RDAT);
}

void iwl_write_prph(iwl_trans_t *trans, uint32_t address, uint32_t value)
{
    iwl_write32(trans, HBUS_TARG_PRPH_WADDR,
                ((address & 0x000FFFFF) | HBUS_PRPH_ACCESS_MASK));
    iwl_write32(trans, HBUS_TARG_PRPH_WDAT, value);
}

/* A 64-bit peripheral register is two consecutive 32-bit ones, low half
 * first.  The addresses the receive tables live at are all of this kind. */
void iwl_write_prph64(iwl_trans_t *trans, uint32_t address, uint64_t value)
{
    iwl_write_prph(trans, address, (uint32_t)value);
    iwl_write_prph(trans, address + 4, (uint32_t)(value >> 32));
}

/* Make sure the device is reachable.
 *
 * On this generation it already is, and that is the whole subtlety.  Once
 * apm_init has set INIT_DONE the adapter sits in its powered-up active state
 * and stays there until it is stopped; its clock is running and its insides
 * answer.  There is nothing to ask for.
 *
 * Asking anyway is not merely redundant, it is destructive: asserting
 * MAC_ACCESS_REQ on a 9000 drops MAC_CLOCK_READY while the request is
 * renegotiated, so a driver that sets the bit and then waits for the clock
 * has broken the very thing it is waiting for.  This driver's first meeting
 * with real silicon was spent on exactly that -- the register said
 *
 *     powered up     GP_CNTRL 0x8040005    clock ready, init done
 *     after request  GP_CNTRL 0x804000c    clock ready gone, request set
 *
 * which is the device doing as it was told, by a driver telling it the wrong
 * thing.  The sleep-and-wake handshake belongs to the older families this
 * driver does not support.
 */
bool iwl_grab_nic_access(iwl_trans_t *trans)
{
    if (iwl_read32(trans, CSR_GP_CNTRL) & CSR_GP_CNTRL_MAC_CLOCK_READY)
        return true;

    /* Not reachable, which should not happen while INIT_DONE is held.  Set it
     * again and give the clock a moment rather than failing outright. */
    iwl_set_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_INIT_DONE);

    if (iwl_poll_bit(trans, CSR_GP_CNTRL,
                     CSR_GP_CNTRL_MAC_CLOCK_READY,
                     CSR_GP_CNTRL_MAC_CLOCK_READY,
                     IWL_POLL_TIMEOUT_MS))
        return true;

    kprintf("iwlwifi: the adapter stopped answering "
            "(GP_CNTRL 0x%x, HW_IF_CONFIG 0x%x)\n",
            iwl_read32(trans, CSR_GP_CNTRL),
            iwl_read32(trans, CSR_HW_IF_CONFIG_REG));
    return false;
}

void iwl_release_nic_access(iwl_trans_t *trans)
{
    /* Nothing was taken, so there is nothing to give back: the device is held
     * active by INIT_DONE for as long as the driver is running.  This stays
     * as a named place for the release to go if a family that needs one is
     * ever added. */
    (void)trans;
}

/* ------------------------------------------------------------------ *
 *  Finding the adapter
 * ------------------------------------------------------------------ */

bool iwl_trans_probe(iwl_trans_t *trans)
{
    memset(trans, 0, sizeof(*trans));

    /* One scan for all six ids rather than one scan each.  A bus scan is
     * sixty-five thousand slots probed with a pair of port accesses apiece,
     * and on an emulated machine every one of those leaves the processor --
     * doing it six times put eight seconds into the boot of a machine that
     * has no wireless adapter at all. */
    pci_device_t dev = pci_find_any(IWL_VENDOR_INTEL, supported_devices,
                                    (int)(sizeof(supported_devices) /
                                          sizeof(*supported_devices)));

    if (!dev.found)
        return false;

    kprintf("iwlwifi: Intel wireless adapter %x:%x at %x:%x.%x\n",
            dev.vendor, dev.device, dev.bus, dev.slot, dev.func);

    pci_power_on(&dev);
    pci_enable_bus_master(&dev);

    /* Read the command register back rather than trusting the write.
     *
     * Bus mastering is the one bit whose absence is invisible from every
     * other test: the registers still answer, configuration still sticks, and
     * the device simply never touches memory.  That is indistinguishable from
     * a DMA engine that does not work, so it is worth one read to know. */
    {
        uint32_t cmd = pci_read32(dev.bus, dev.slot, dev.func, 0x04);

        kprintf("iwlwifi: pci command 0x%x (%s bus master, %s memory space)\n",
                cmd & 0xFFFF,
                (cmd & 0x4) ? "with" : "WITHOUT",
                (cmd & 0x2) ? "with" : "WITHOUT");
    }

    /* The registers are behind a 64-bit memory base address register.  The
     * window is 8 KiB, which covers the control registers and the flow
     * handler; the peripheral registers are not in it at all and are reached
     * through the window registers instead. */
    uint64_t bar = pci_bar_address(&dev, 0);

    if (!bar) {
        kputs("iwlwifi: the adapter's registers are not mapped anywhere\n");
        return false;
    }

    trans->pci = dev;
    trans->regs = (volatile uint8_t *)paging_map_device(bar, 8192);
    if (!trans->regs) {
        kputs("iwlwifi: could not map the adapter's registers\n");
        return false;
    }

    trans->hw_rev   = iwl_read32(trans, CSR_HW_REV);
    trans->hw_rf_id = iwl_read32(trans, CSR_HW_RF_ID);

    /* All ones means the device is not answering -- usually because it is
     * still powered down, occasionally because the mapping is wrong.  Either
     * way nothing below this point would work. */
    if (trans->hw_rev == 0xFFFFFFFF) {
        kputs("iwlwifi: the adapter's registers read as all ones; "
              "it is not responding\n");
        return false;
    }

    kprintf("iwlwifi: hardware revision 0x%x, radio 0x%x\n",
            trans->hw_rev, trans->hw_rf_id);

    trans->present = true;
    return true;
}

/* ------------------------------------------------------------------ *
 *  Powering the device up
 * ------------------------------------------------------------------ */

/* Take ownership of the device.
 *
 * On a CNVi part the adapter is shared with the platform, and it may be held
 * by the firmware the machine booted with.  This asks for it and waits to be
 * told it has been handed over. */
/* Claim the device, and see whether the claim stuck.
 *
 * This is the part that was got wrong the first time this driver met the
 * hardware, and it is worth writing down because the register does not behave
 * the way its name suggests.  "NIC ready" is not a bit the adapter raises to
 * announce itself -- it is a bit *we* write to say we are taking the device,
 * which then reads back set if the device allowed it and clear if it did not.
 *
 * Polling for it without ever writing it, which is what this did at first,
 * waits forever for something nothing will ever do. */
static bool iwl_set_hw_ready(iwl_trans_t *trans)
{
    iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_BIT_NIC_READY);

    if (!iwl_poll_bit(trans, CSR_HW_IF_CONFIG_REG,
                      CSR_HW_IF_CONFIG_BIT_NIC_READY,
                      CSR_HW_IF_CONFIG_BIT_NIC_READY, 50))
        return false;

    /* Tell the device an operating system is here and running.
     *
     * This matters on a part like this one, where the adapter is shared with
     * the platform rather than owned outright by whatever is in the slot: it
     * is the difference between a device that believes it is on its own and
     * one that believes it has a driver.  It costs one write and its absence
     * is not reported anywhere. */
    iwl_set_bit(trans, CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
    return true;
}

static bool iwl_prepare_card(iwl_trans_t *trans)
{
    /* Usually it is simply free and the first claim works. */
    if (iwl_set_hw_ready(trans))
        return true;

    /* It is not.  On a CNVi part the adapter is shared with the platform and
     * may still be held by the firmware the machine booted with, so the link
     * power management that would let it sleep mid-handover is turned off and
     * the device is asked to prepare itself, repeatedly, while we keep trying
     * to claim it.
     *
     * The patience here is not arbitrary: handing the device over takes real
     * time on the far side, and a claim that fails at fifty milliseconds can
     * succeed at eight hundred. */
    iwl_set_bit(trans, CSR_DBG_LINK_PWR_MGMT_REG,
                CSR_RESET_LINK_PWR_MGMT_DISABLED);
    pit_sleep(2);

    for (int attempt = 0; attempt < 10; attempt++) {
        iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_PREPARE);

        uint64_t deadline = time_now_ms() + 150;

        while (time_now_ms() < deadline)
            if (iwl_set_hw_ready(trans))
                return true;

        pit_sleep(25);
    }

    kputs("iwlwifi: the adapter would not hand itself over\n");
    return false;
}

/* Reset the whole device, then claim it again.
 *
 * This matters more on this adapter than on most.  A CNVi part has no
 * function-level reset, so nothing resets it when a virtual machine using it
 * stops -- it keeps whatever state the last driver left, across as many
 * attempts as you care to make.  A driver that never issues this is not
 * starting from a known state; it is starting from wherever the previous
 * run happened to stop, which is why the second attempt at bring-up can
 * behave differently from the first.
 *
 * The reset drops ownership as well, so the card has to be claimed again
 * afterwards. */
static bool iwl_sw_reset(iwl_trans_t *trans)
{
    iwl_set_bit(trans, CSR_RESET, CSR_RESET_SW_RESET);
    pit_sleep(6);

    return iwl_prepare_card(trans);
}

/* The power-up proper.  Every driver for this hardware does these steps in
 * this order, and the ordering is not incidental: the clock cannot be asked
 * for before the power source is chosen, and the initialisation-done bit
 * cannot be set before the clock is running. */
static bool iwl_apm_init(iwl_trans_t *trans)
{
    /* Disable the link power management that would otherwise let the bus go
     * quiet underneath an in-flight transfer. */
    iwl_set_bit(trans, CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_L1A_NO_L0S_RX);

    /* Set the flow handler's wait threshold to its maximum -- a documented
     * workaround for a hardware error the device raises under load. */
    iwl_set_bit(trans, CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);

    /* Let an interrupt from the management bus pull the PCI Express link up
     * out of its low-power state.  Without this the device can be asked to
     * wake and never hear the request, because the link it would answer over
     * is itself asleep -- which is exactly what "would not wake up" looks
     * like from here. */
    iwl_set_bit(trans, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_BIT_HAP_WAKE_L1A);

    /* Move the adapter from powered-up-idle to powered-up-active, and wait
     * for its clock to stabilise.  Only once that has happened is anything
     * inside the device -- the peripheral registers, its memory -- reachable
     * at all. */
    iwl_set_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_INIT_DONE);

    if (!iwl_poll_bit(trans, CSR_GP_CNTRL,
                      CSR_GP_CNTRL_MAC_CLOCK_READY,
                      CSR_GP_CNTRL_MAC_CLOCK_READY,
                      IWL_POLL_TIMEOUT_MS)) {
        kprintf("iwlwifi: the adapter's clock never started (GP_CNTRL 0x%x)\n",
                iwl_read32(trans, CSR_GP_CNTRL));
        return false;
    }

    kprintf("iwlwifi: powered up (GP_CNTRL 0x%x)\n",
            iwl_read32(trans, CSR_GP_CNTRL));

    /* And that is the whole of it on this generation.
     *
     * Older adapters need their power management block set up here -- a DMA
     * clock enabled, a power source chosen, an L1 state disabled -- through
     * the APMG peripheral registers.  The 9000 series has no APMG block at
     * all: those addresses are not merely unnecessary, they are nothing, and
     * writing to them writes into a hole.  The registers are still named in
     * iwl-regs.h because they are part of the family's history, but nothing
     * here touches them.
     *
     * The device is now in its powered-up active state and stays there, held
     * by INIT_DONE, until iwl_apm_stop clears it. */
    return true;
}

static void iwl_apm_stop(iwl_trans_t *trans)
{
    /* Stop the bus master before anything else: the device may still be
     * reading rings that are about to be freed. */
    iwl_set_bit(trans, CSR_RESET, CSR_RESET_STOP_MASTER);
    iwl_poll_bit(trans, CSR_RESET, CSR_RESET_MASTER_DISABLED,
                 CSR_RESET_MASTER_DISABLED, 100);

    iwl_set_bit(trans, CSR_RESET, CSR_RESET_SW_RESET);
    pit_sleep(5);

    iwl_clear_bit(trans, CSR_GP_CNTRL, CSR_GP_CNTRL_INIT_DONE);
}

/* ------------------------------------------------------------------ *
 *  Rings
 * ------------------------------------------------------------------ */

/* The receive ring is three pieces of memory: a ring of addresses the device
 * reads to know where to put frames, the buffers those addresses point at,
 * and a small area where the device writes how far it has got. */
/* The receive ring on this generation is three tables rather than one.
 *
 * The free table holds buffers the driver is giving to the device.  The used
 * table is where the device says which of them it has filled.  And a small
 * status area is where it says how far along the used table it has got.  The
 * older single-table arrangement -- one ring the device walks, with the
 * driver chasing it -- is what this driver had first, and it belongs to
 * hardware a generation earlier.
 *
 * A buffer is identified by a small number rather than by its position, and
 * that number is carried in the low bits of its address in the free table.
 * The buffers are page-aligned so those bits are free, which is why this
 * works at all: the device hands back the number and the driver looks the
 * buffer up by it. */
static bool iwl_alloc_rx(iwl_trans_t *trans)
{
    trans->rx_free_table = dma_alloc(IWL_RX_RING_SIZE * 8, 4096);
    trans->rx_used_table = dma_alloc(IWL_RX_RING_SIZE * 4, 4096);
    trans->rx_status     = dma_alloc(16, 16);
    trans->rx_buffers    = dma_alloc((uint64_t)IWL_RX_RING_SIZE * IWL_RX_BUF_SIZE,
                                     4096);

    if (!trans->rx_free_table.virt || !trans->rx_used_table.virt ||
        !trans->rx_status.virt || !trans->rx_buffers.virt)
        return false;

    uint64_t *free_table = (uint64_t *)trans->rx_free_table.virt;

    for (int i = 0; i < IWL_RX_RING_SIZE; i++) {
        uint64_t phys = trans->rx_buffers.phys + (uint64_t)i * IWL_RX_BUF_SIZE;

        /* The identifier is one-based, so that a used-table entry of zero is
         * distinguishable from a buffer that was never handed over. */
        free_table[i] = phys | (uint64_t)(i + 1);
    }

    trans->rx_read = 0;
    return true;
}

static bool iwl_alloc_tx_queue(iwl_tx_queue_t *q)
{
    q->descriptors = dma_alloc(IWL_TX_RING_SIZE * sizeof(struct iwl_tfd), 4096);
    /* One command-sized buffer per slot.  Frames larger than this go out as
     * a second scatter-gather entry pointing at the caller's memory, so this
     * only has to hold a command header and a small body. */
    q->buffers = dma_alloc((uint64_t)IWL_TX_RING_SIZE * 512, 4096);

    if (!q->descriptors.virt || !q->buffers.virt)
        return false;

    q->write = 0;
    q->read  = 0;
    q->active = true;
    return true;
}

static void iwl_free_rings(iwl_trans_t *trans)
{
    dma_free(&trans->rx_free_table);
    dma_free(&trans->rx_used_table);
    dma_free(&trans->rx_buffers);
    dma_free(&trans->rx_status);
    dma_free(&trans->keep_warm);
    dma_free(&trans->scheduler);

    for (int i = 0; i < IWL_NUM_QUEUES; i++) {
        dma_free(&trans->queue[i].descriptors);
        dma_free(&trans->queue[i].buffers);
        trans->queue[i].active = false;
    }
}

/* Tell the device where the rings are. */
static bool iwl_configure_rings(iwl_trans_t *trans)
{
    if (!iwl_grab_nic_access(trans))
        return false;

    /* Receive: stop the engine, tell it where the three tables are, start it.
     * These are full 64-bit addresses written as two words, not the shifted
     * short forms the previous generation used. */
    iwl_write_prph(trans, RFH_RXF_DMA_CFG, 0);
    iwl_write_prph(trans, RFH_RXF_RXQ_ACTIVE, 0);

    iwl_write_prph64(trans, RFH_Q_FRBDCB_BA_LSB(0), trans->rx_free_table.phys);
    iwl_write_prph64(trans, RFH_Q_URBDCB_BA_LSB(0), trans->rx_used_table.phys);
    iwl_write_prph64(trans, RFH_Q_URBD_STTS_WPTR_LSB(0), trans->rx_status.phys);

    iwl_write_prph(trans, RFH_Q_FRBDCB_WIDX(0), 0);
    iwl_write_prph(trans, RFH_Q_FRBDCB_RIDX(0), 0);
    iwl_write_prph(trans, RFH_Q_URBDCB_WIDX(0), 0);

    iwl_write_prph(trans, RFH_RXF_DMA_CFG,
                   RFH_DMA_EN_ENABLE_VAL | RFH_RXF_DMA_RB_SIZE_4K |
                   RFH_RXF_DMA_MIN_RB_4_8 | RFH_RXF_DMA_DROP_TOO_LARGE |
                   RFH_RXF_DMA_RBDCB_SIZE_512);

    /* Let the device's own reads and writes go through the processor's
     * caches, and use the larger transfer size that a PCI Express link
     * prefers. */
    iwl_write_prph(trans, RFH_GEN_CFG,
                   RFH_GEN_CFG_RFH_DMA_SNOOP |
                   RFH_GEN_CFG_SERVICE_DMA_SNOOP |
                   RFH_GEN_CFG_RB_CHUNK_SIZE_128);

    /* Queue zero, in both its free and used halves. */
    iwl_write_prph(trans, RFH_RXF_RXQ_ACTIVE, (1u << 0) | (1u << 16));

    /* Read the configuration back.  If these hold what was written then the
     * receive engine has been set up correctly and any failure to deliver is
     * a failure to move bytes; if they do not, the setup never landed and the
     * engine was never going to do anything. */
    kprintf("iwlwifi: rfh dma cfg 0x%x, active 0x%x, free base 0x%x\n",
            iwl_read_prph(trans, RFH_RXF_DMA_CFG),
            iwl_read_prph(trans, RFH_RXF_RXQ_ACTIVE),
            iwl_read_prph(trans, RFH_Q_FRBDCB_BA_LSB(0)));

    /* The keep-warm page: the device touches it so that its memory
     * controller does not power down between transfers. */
    iwl_write32(trans, FH_KW_MEM_ADDR_REG,
                (uint32_t)(trans->keep_warm.phys >> 4));

    /* Transmit: each queue's ring base, in the same 256-byte units. */
    for (int i = 0; i < IWL_NUM_QUEUES; i++)
        if (trans->queue[i].active)
            iwl_write32(trans, FH_MEM_CBBC_QUEUE(i),
                        (uint32_t)(trans->queue[i].descriptors.phys >> 8));

    iwl_write32(trans, FH_TSSR_TX_MSG_CONFIG_REG, 0x0000FF00);

    /* Hand over every buffer in the free table.  The index written is the
     * next slot the device should fill from, so the whole table minus one is
     * the ring's way of saying "all of these are yours".
     *
     * This one is an ordinary register rather than one behind the window,
     * which is easy to miss: writing it through the window would appear to
     * work and hand over nothing. */
    iwl_write32(trans, RFH_Q_FRBDCB_WIDX_TRG(0), IWL_RX_RING_SIZE - 1);

    iwl_release_nic_access(trans);
    return true;
}

/* ------------------------------------------------------------------ *
 *  Loading the firmware
 * ------------------------------------------------------------------ */

/* Push one section into the device over the service DMA channel.
 *
 * The device is not running any code at this point.  What is happening is
 * that the flow handler is being told to copy from host memory to an address
 * inside the device, which is how the firmware gets there in the first
 * place. */
/* Write a section into the device a word at a time, through the memory
 * window, without involving the DMA engine at all.
 *
 * This is the slow way and it is not how the vendor's driver loads firmware.
 * It is here because on this adapter, under passthrough, the DMA engine
 * accepts every instruction and never moves anything -- while the same
 * destination is perfectly writable by hand, which the round-trip check
 * above demonstrates.  Rather than leave the driver unable to start at all,
 * it does the copy itself.
 *
 * The address register auto-increments, so the whole section is one address
 * write followed by a stream of data writes: about a word per bus access
 * rather than two, which is the difference between this being slow and being
 * unusable. */
static void iwl_load_section_by_window(iwl_trans_t *trans, uint32_t dst,
                                       const uint8_t *src, uint32_t len)
{
    const uint32_t *words = (const uint32_t *)(const void *)src;
    uint32_t        count = len / 4;

    iwl_write32(trans, HBUS_TARG_MEM_WADDR, dst);
    for (uint32_t i = 0; i < count; i++)
        iwl_write32(trans, HBUS_TARG_MEM_WDAT, words[i]);

    /* A section that is not a whole number of words gets its tail padded
     * with zeroes, which is what the firmware image itself is padded with. */
    if (len & 3) {
        uint32_t tail = 0;

        memcpy(&tail, src + count * 4, len & 3);
        iwl_write32(trans, HBUS_TARG_MEM_WDAT, tail);
    }
}

static bool iwl_load_section(iwl_trans_t *trans, uint32_t dst,
                             uint64_t src_phys, uint32_t len)
{
    const int chnl = FH_SRVC_CHNL;

    if (!iwl_grab_nic_access(trans))
        return false;

    iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(chnl),
                FH_TCSR_TX_CONFIG_DMA_PAUSE);

    iwl_write32(trans, FH_SRVC_CHNL_SRAM_ADDR_REG(chnl), dst);

    iwl_write32(trans, FH_TFDIB_CTRL0_REG(chnl), (uint32_t)src_phys);
    iwl_write32(trans, FH_TFDIB_CTRL1_REG(chnl),
                (uint32_t)((src_phys >> 32) << FH_TFDIB_CTRL1_ADDR_BITSHIFT) | len);

    iwl_write32(trans, FH_TCSR_CHNL_TX_BUF_STS_REG(chnl),
                (1u << FH_TCSR_CHNL_TX_BUF_STS_TB_NUM_POS) |
                (1u << FH_TCSR_CHNL_TX_BUF_STS_TB_IDX_POS) |
                FH_TCSR_CHNL_TX_BUF_STS_TFDB_VALID);

    /* No credit-disable bit here.  The value this driver had for it, 0x8, is
     * simply not a bit of this register: the hardware read the write back
     * without it, which is how it was found to be wrong. */
    iwl_write32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(chnl),
                FH_TCSR_TX_CONFIG_DMA_ENABLE |
                FH_TCSR_TX_CONFIG_CIRQ_HOST_ENDTFD);

    /* Once, for the first transfer: show that every register the engine was
     * programmed through holds what it was given.  If the address and length
     * did not stick, the engine is being told to move nothing from nowhere. */
    static bool reported;

    if (!reported) {
        reported = true;
        kprintf("iwlwifi: fh sram 0x%x, tfdib0 0x%x, tfdib1 0x%x, "
                "bufsts 0x%x (src 0x%x len %u)\n",
                iwl_read32(trans, FH_SRVC_CHNL_SRAM_ADDR_REG(chnl)),
                iwl_read32(trans, FH_TFDIB_CTRL0_REG(chnl)),
                iwl_read32(trans, FH_TFDIB_CTRL1_REG(chnl)),
                iwl_read32(trans, FH_TCSR_CHNL_TX_BUF_STS_REG(chnl)),
                (uint32_t)src_phys, len);
    }

    iwl_release_nic_access(trans);

    /* Wait for it to land -- and watch both places it might be announced.
     *
     * CSR_FH_INT_STATUS has a bit per DMA channel, but only for channels 0
     * and 1.  The channel used here is 9, the one reserved for pushing
     * firmware into a device that is not running any yet, and its completion
     * is reported instead as a single "some transmit finished" bit in
     * CSR_INT.  Watching only the per-channel register means watching a bit
     * that nothing will ever set, which is what this driver did to begin
     * with.
     *
     * Both are accepted: a transfer that finished is a transfer that
     * finished, whichever register says so. */
    uint64_t deadline = time_now_ms() + 1000;
    bool     done = false;

    while (time_now_ms() < deadline) {
        if ((iwl_read32(trans, CSR_INT) & CSR_INT_BIT_FH_TX) ||
            (iwl_read32(trans, CSR_FH_INT_STATUS) & FH_INT_TX_CHNL0)) {
            done = true;
            break;
        }
        io_wait();
    }

    if (!done) {
        /* Nothing was announced.  Before believing the transfer failed, look
         * at where it was supposed to land: if the bytes are there, then the
         * DMA worked and only the completion signal was missed, which is a
         * different and much smaller problem. */
        /* The source is DMA memory below the identity map, so its physical
         * address is also the address the kernel reads it at. */
        uint32_t landed = iwl_read_mem32(trans, dst);
        uint32_t wanted = *(const volatile uint32_t *)(uintptr_t)src_phys;

        kprintf("iwlwifi: the transfer to 0x%x was not announced "
                "(CSR_INT 0x%x, FH_INT 0x%x, TX_CONFIG 0x%x)\n",
                dst,
                iwl_read32(trans, CSR_INT),
                iwl_read32(trans, CSR_FH_INT_STATUS),
                iwl_read32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(chnl)));

        if (landed == wanted) {
            kprintf("iwlwifi: but the data arrived (0x%x at 0x%x); "
                    "carrying on without the signal\n", landed, dst);
            return true;
        }

        /* The engine did nothing.  Do the copy by hand instead -- the device's
         * memory is demonstrably writable, so there is no reason to give up
         * merely because the machinery for moving bytes into it is unwilling. */
        if (!trans->window_load_warned) {
            trans->window_load_warned = true;
            kputs("iwlwifi: the DMA engine will not run; "
                  "loading the firmware through the memory window instead\n");
        }

        iwl_load_section_by_window(trans, dst,
                                   (const uint8_t *)(uintptr_t)src_phys, len);

        landed = iwl_read_mem32(trans, dst);
        if (landed != wanted) {
            kprintf("iwlwifi: writing it by hand did not take either "
                    "(0x%x at 0x%x, wanted 0x%x)\n", landed, dst, wanted);
            return false;
        }
        return true;
    }

    /* These registers are cleared by writing the bit back. */
    iwl_write32(trans, CSR_INT, CSR_INT_BIT_FH_TX);
    iwl_write32(trans, CSR_FH_INT_STATUS, FH_INT_TX_CHNL0);
    return true;
}

/* Load the sections belonging to one of the device's two processors.
 *
 * After each section the firmware is told how much has arrived, as a bit per
 * section: the first processor's progress goes in the low half of the word
 * and the second's in the high half.  The device is watching this while it is
 * being loaded, and a loader that stays silent is one it stops waiting for. */
static bool iwl_load_cpu_sections(iwl_trans_t *trans,
                                  const iwl_fw_image_t *img, int cpu)
{
    uint32_t section_bits = 1;
    int      shift = (cpu == 1) ? 0 : 16;

    for (int i = 0; i < img->count; i++) {
        const iwl_fw_section_t *s = &img->section[i];

        if (s->cpu != cpu)
            continue;

        /* Paged sections are not resident: they live in host memory and the
         * firmware asks for them as it needs them.  Supporting that means a
         * page table the firmware owns, which is not implemented -- so they
         * are skipped, and this firmware may well refuse to start without
         * them. */
        if (s->paged)
            continue;

        /* The section data sits inside the firmware image buffer, which was
         * allocated for DMA, so its physical address is its address. */
        uint64_t phys = trans->fw.image.phys +
                        (uint64_t)(s->data - (const uint8_t *)trans->fw.image.virt);

        /* Transfers are bounded by what one descriptor can describe. */
        uint32_t at = 0;

        while (at < s->len) {
            uint32_t chunk = s->len - at;

            if (chunk > FH_MEM_TB_MAX_LENGTH)
                chunk = FH_MEM_TB_MAX_LENGTH;

            if (!iwl_load_section(trans, s->address + at, phys + at, chunk))
                return false;
            at += chunk;
        }

        /* Tell the firmware this section has arrived.  The bits accumulate,
         * so each write says how much of the image is in rather than which
         * piece just landed. */
        uint32_t status = iwl_read32(trans, FH_UCODE_LOAD_STATUS);

        iwl_write32(trans, FH_UCODE_LOAD_STATUS,
                    status | (section_bits << shift));
        section_bits = (section_bits << 1) | 1;
    }

    /* And then say that this processor's sections are all in.
     *
     * The per-section writes above are progress; this is completion, and it
     * is a different value rather than one more bit -- the low half filled
     * for the first processor, the whole word for the second.  The firmware
     * waits for it.  Without it the image sits in the device complete and
     * untouched, which is exactly what this driver saw: every section loaded
     * and verified, and nothing ever ran. */
    iwl_write32(trans, FH_UCODE_LOAD_STATUS,
                (cpu == 1) ? 0x0000FFFF : 0xFFFFFFFF);

    return true;
}

static bool iwl_load_image(iwl_trans_t *trans, const iwl_fw_image_t *img)
{
    /* Take the processors out of reset before sending them anything.  This
     * one write is the difference between a DMA engine that carries the
     * firmware and one that accepts every instruction and moves nothing. */
    iwl_write_prph(trans, RELEASE_CPU_RESET, RELEASE_CPU_RESET_BIT);

    /* Read it back, and read the registers the transfer will be programmed
     * through.  This answers the question everything else has been dancing
     * around: whether the device's insides can be reached at all.  The flow
     * handler's registers are plain memory-mapped words and demonstrably
     * work; the peripheral registers are reached through a window, and if
     * that window is not working then every write through it -- including the
     * one just made -- has gone nowhere, which would explain a DMA engine
     * that is configured perfectly and does nothing. */
    kprintf("iwlwifi: cpu reset reads back 0x%x (wrote 0x%x)\n",
            iwl_read_prph(trans, RELEASE_CPU_RESET), RELEASE_CPU_RESET_BIT);

    /* Settle the question of whether peripheral writes work at all.
     *
     * 0xA030B4 is a scratch word the firmware does not act on -- the vendor's
     * own driver writes a pattern into it purely so that something
     * recognisable is there afterwards.  If a pattern written here comes back
     * unchanged then the window works and the CPU-reset write above landed
     * too; if it does not, then every peripheral write this driver has made
     * has gone nowhere, which would explain a DMA engine that is configured
     * perfectly and moves nothing. */
    iwl_write_prph(trans, 0xA030B4, 0x01010101);
    kprintf("iwlwifi: prph scratch reads back 0x%x (wrote 0x1010101)\n",
            iwl_read_prph(trans, 0xA030B4));

    /* And the same question for the device's memory, which is where the
     * firmware is going.  This one is unambiguous: the address written to is
     * ordinary memory inside the adapter, so a pattern put there must come
     * back exactly.  If it does, the DMA engine is being asked to do
     * something the host could do by hand, and the fault is in how it is
     * asked; if it does not, nothing inside the device is reachable and the
     * transfer was never going to work. */
    {
        uint32_t before = iwl_read_mem32(trans, 0x404000);

        iwl_write_mem32(trans, 0x404000, 0xA5A5A5A5);

        uint32_t after = iwl_read_mem32(trans, 0x404000);

        kprintf("iwlwifi: device memory round trip: was 0x%x, wrote 0xa5a5a5a5, "
                "reads 0x%x -- %s\n",
                before, after,
                after == 0xA5A5A5A5 ? "the window works"
                                    : "the window does NOT work");
    }

    if (!iwl_load_cpu_sections(trans, img, 1))
        return false;

    /* The second processor's sections, if the firmware has any.  A single
     * processor image simply has none tagged for it and this does nothing. */
    return iwl_load_cpu_sections(trans, img, 2);
}

/* ------------------------------------------------------------------ *
 *  Commands
 * ------------------------------------------------------------------ */

/* Fill in one scatter-gather entry of a descriptor. */
static void tfd_add(struct iwl_tfd *tfd, uint64_t phys, uint32_t len)
{
    if (tfd->num_tbs >= IWL_NUM_TBS)
        return;

    struct iwl_tfd_tb *tb = &tfd->tbs[tfd->num_tbs];

    tb->lo = (uint32_t)phys;
    tb->hi_and_len = (uint16_t)(((phys >> 32) & 0xF) | (len << 4));
    tfd->num_tbs++;
}

int iwl_send_cmd(iwl_trans_t *trans, uint8_t group, uint8_t cmd,
                 const void *data, uint16_t len, bool wait)
{
    iwl_tx_queue_t *q = &trans->queue[IWL_CMD_QUEUE];

    if (!q->active)
        return -1;
    if (len + sizeof(struct iwl_cmd_header) > 512)
        return -1;

    uint32_t slot = q->write & (IWL_TX_RING_SIZE - 1);
    uint8_t *buf  = (uint8_t *)q->buffers.virt + (uint64_t)slot * 512;
    uint64_t phys = q->buffers.phys + (uint64_t)slot * 512;

    struct iwl_cmd_header *hdr = (struct iwl_cmd_header *)buf;

    hdr->cmd      = cmd;
    hdr->group_id = group;
    /* The sequence number comes back in the reply, which is how a reply is
     * matched to the command that asked for it.  The queue number is part of
     * it because the firmware answers on the queue it was asked on. */
    hdr->sequence = (uint16_t)((IWL_CMD_QUEUE << 8) |
                               (trans->cmd_sequence++ & 0xFF));

    if (len && data)
        memcpy(buf + sizeof(*hdr), data, len);

    struct iwl_tfd *tfd = (struct iwl_tfd *)q->descriptors.virt + slot;

    memset(tfd, 0, sizeof(*tfd));
    tfd_add(tfd, phys, (uint32_t)(sizeof(*hdr) + len));

    q->write++;

    /* Telling the device the write pointer moved is what starts it. */
    iwl_write32(trans, HBUS_TARG_WRPTR,
                (q->write & (IWL_TX_RING_SIZE - 1)) | (IWL_CMD_QUEUE << 8));

    if (!wait)
        return 0;

    trans->response_valid = false;

    uint64_t deadline = time_now_ms() + IWL_POLL_TIMEOUT_MS;

    while (time_now_ms() < deadline) {
        struct iwl_rx_packet *pkt = iwl_rx_next(trans);

        if (pkt && pkt->hdr.cmd == cmd && pkt->hdr.group_id == group) {
            uint32_t plen = IWL_RX_PACKET_LEN(pkt);

            if (plen > sizeof(trans->response))
                plen = sizeof(trans->response);
            memcpy(trans->response, pkt->data, plen);
            trans->response_len = plen;
            trans->response_valid = true;
            return 0;
        }
        io_wait();
    }

    kprintf("iwlwifi: command 0x%x/0x%x went unanswered\n", group, cmd);
    return -1;
}

int iwl_tx_frame(iwl_trans_t *trans, int queue, const void *header,
                 uint32_t header_len, const void *body, uint32_t body_len)
{
    if (queue < 0 || queue >= IWL_NUM_QUEUES)
        return -1;

    iwl_tx_queue_t *q = &trans->queue[queue];

    if (!q->active)
        return -1;
    if (header_len + body_len > 512)
        return -1;

    uint32_t slot = q->write & (IWL_TX_RING_SIZE - 1);
    uint8_t *buf  = (uint8_t *)q->buffers.virt + (uint64_t)slot * 512;
    uint64_t phys = q->buffers.phys + (uint64_t)slot * 512;

    memcpy(buf, header, header_len);
    if (body_len)
        memcpy(buf + header_len, body, body_len);

    struct iwl_tfd *tfd = (struct iwl_tfd *)q->descriptors.virt + slot;

    memset(tfd, 0, sizeof(*tfd));
    tfd_add(tfd, phys, header_len + body_len);

    q->write++;
    iwl_write32(trans, HBUS_TARG_WRPTR,
                (q->write & (IWL_TX_RING_SIZE - 1)) | (queue << 8));

    return 0;
}

struct iwl_rx_packet *iwl_rx_next(iwl_trans_t *trans)
{
    if (!trans->rx_status.virt || !trans->rx_used_table.virt)
        return NULL;

    /* The first sixteen bits of the status area say how far along the used
     * table the device has written.  Twelve of them are the index; our own
     * cursor is masked the same way before the two are compared, because it
     * counts upwards without limit and would otherwise never match again. */
    uint32_t closed = *(volatile uint16_t *)trans->rx_status.virt & 0x0FFF;

    if (closed == (trans->rx_read & 0x0FFF))
        return NULL;

    uint32_t slot = trans->rx_read & (IWL_RX_RING_SIZE - 1);
    const volatile uint32_t *used = (const volatile uint32_t *)
                                    trans->rx_used_table.virt;

    /* The used table gives the identifier of the buffer that was filled, not
     * its position -- the device is free to fill them in whatever order it
     * likes, and usually does. */
    uint32_t id = used[slot] & 0x0FFF;

    trans->rx_read++;

    if (id == 0 || id > IWL_RX_RING_SIZE)
        return NULL;                    /* nothing was really put there */

    uint8_t *buf = (uint8_t *)trans->rx_buffers.virt +
                   (uint64_t)(id - 1) * IWL_RX_BUF_SIZE;

    /* Give the buffer back by advancing the free list's write index past it.
     * The caller is expected to be done with this frame before asking for
     * another, which is what makes that safe. */
    iwl_write32(trans, RFH_Q_FRBDCB_WIDX_TRG(0),
                (trans->rx_read + IWL_RX_RING_SIZE - 1) &
                (IWL_RX_RING_SIZE - 1));

    return (struct iwl_rx_packet *)buf;
}

/* ------------------------------------------------------------------ *
 *  Bringing it all up
 * ------------------------------------------------------------------ */

bool iwl_trans_start(iwl_trans_t *trans)
{
    if (!trans->present)
        return false;

    if (!iwl_fw_load(&trans->fw))
        return false;

    if (!iwl_prepare_card(trans))
        goto fail;

    /* Clear the bit that asks the device to keep its state across a reset.
     * Whatever it was preserving belongs to whoever had it last, not to us. */
    iwl_clear_bit(trans, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_PERSIST_MODE);

    /* Then reset it, so that what follows starts from a known state rather
     * than from wherever the previous attempt left off. */
    if (!iwl_sw_reset(trans)) {
        kputs("iwlwifi: the adapter would not come back after a reset\n");
        goto fail;
    }

    /* Mask every interrupt.  This driver polls; the status register still
     * records what happened, which is all the polling needs. */
    iwl_write32(trans, CSR_INT_MASK, 0);
    iwl_write32(trans, CSR_INT, 0xFFFFFFFF);
    iwl_write32(trans, CSR_FH_INT_STATUS, 0xFFFFFFFF);

    if (!iwl_apm_init(trans))
        goto fail;

    if (!iwl_alloc_rx(trans))
        goto fail_memory;

    trans->keep_warm = dma_alloc(4096, 4096);
    trans->scheduler = dma_alloc(1024, 1024);
    if (!trans->keep_warm.virt || !trans->scheduler.virt)
        goto fail_memory;

    /* Only the queues that are used are allocated: thirty-one rings, each
     * with its buffers, is more memory than this machine should spend on
     * queues that carry nothing. */
    if (!iwl_alloc_tx_queue(&trans->queue[IWL_CMD_QUEUE]) ||
        !iwl_alloc_tx_queue(&trans->queue[IWL_DATA_QUEUE]) ||
        !iwl_alloc_tx_queue(&trans->queue[IWL_MGMT_QUEUE]))
        goto fail_memory;

    if (!iwl_configure_rings(trans))
        goto fail;

    /* Clear the radio-switch handshake before asking the device to do
     * anything.  Until these are clear it considers commands blocked, and a
     * blocked device configures happily and then does nothing -- which looks
     * exactly like a broken DMA engine and was mistaken for one here.
     *
     * Cleared twice, as every driver for this hardware does, because the
     * device can set them again between the two writes. */
    iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR,
                CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    iwl_write32(trans, CSR_INT, 0xFFFFFFFF);
    iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    iwl_write32(trans, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);

    /* Turn bus mastering on again.
     *
     * Resetting the device clears its configuration space, and bus mastering
     * lives there -- so the enable done when the device was found has been
     * undone by the reset above.  Without it the DMA engine accepts every
     * instruction and moves nothing, because the device is no longer allowed
     * to touch memory at all.  Nothing reports this: there is no error and no
     * fault, the transfer simply never happens.
     *
     * This cost most of an evening, and the thing that found it was the host
     * complaining that it had to restore the device's BARs after the guest
     * reset it -- if the BARs needed restoring, so did everything else in
     * that register. */
    pci_enable_bus_master(&trans->pci);

    {
        uint32_t cmd = pci_read32(trans->pci.bus, trans->pci.slot,
                                  trans->pci.func, 0x04);

        kprintf("iwlwifi: pci command 0x%x before loading (%s bus master)\n",
                cmd & 0xFFFF, (cmd & 0x4) ? "with" : "WITHOUT");
    }

    kputs("iwlwifi: loading firmware\n");

    /* Let the transmit-finished interrupt through while the firmware is being
     * pushed in.  Nothing here takes the interrupt -- this driver polls -- but
     * a status bit that is masked may never be latched at all, and the whole
     * of the load is paced by watching for that bit. */
    iwl_write32(trans, CSR_INT_MASK, CSR_INT_BIT_FH_TX);

    bool loaded = iwl_load_image(trans, &trans->fw.regular);

    iwl_write32(trans, CSR_INT_MASK, 0);

    if (!loaded) {
        kputs("iwlwifi: the firmware would not load\n");
        goto fail;
    }

    /* Release the device to run what was just put into it. */
    iwl_write32(trans, CSR_RESET, 0);

    /* The firmware says it is running by raising one bit.  On a device where
     * the load went wrong this never arrives, and there is nothing to read
     * that says why -- which is why every step above reports its own
     * failure rather than leaving it to this one. */
    uint64_t deadline = time_now_ms() + IWL_ALIVE_TIMEOUT_MS;
    bool alive = false;

    while (time_now_ms() < deadline) {
        if (iwl_read32(trans, CSR_INT) & CSR_INT_BIT_ALIVE) {
            kputs("iwlwifi: the firmware raised its alive interrupt\n");
            alive = true;
            break;
        }

        /* The firmware also announces itself by putting a notification in
         * the receive ring, and that is the more useful of the two: it means
         * the ring is working, which everything after this depends on. */
        struct iwl_rx_packet *pkt = iwl_rx_next(trans);

        if (pkt) {
            kprintf("iwlwifi: the firmware sent notification 0x%x/0x%x, "
                    "%u bytes -- the receive ring is working\n",
                    pkt->hdr.group_id, pkt->hdr.cmd, IWL_RX_PACKET_LEN(pkt));
            alive = true;
            break;
        }
        io_wait();
    }

    if (!alive) {
        /* Everything the device might use to say something, so that the next
         * person starts from evidence rather than from the beginning. */
        kprintf("iwlwifi: the firmware never reported itself alive\n"
                "iwlwifi:   CSR_INT      0x%x\n"
                "iwlwifi:   FH_INT       0x%x\n"
                "iwlwifi:   GP_CNTRL     0x%x\n"
                "iwlwifi:   UCODE_GP1    0x%x\n"
                "iwlwifi:   load status  0x%x\n"
                "iwlwifi:   rx write ptr 0x%x\n",
                iwl_read32(trans, CSR_INT),
                iwl_read32(trans, CSR_FH_INT_STATUS),
                iwl_read32(trans, CSR_GP_CNTRL),
                iwl_read32(trans, CSR_UCODE_DRV_GP1),
                iwl_read32(trans, FH_UCODE_LOAD_STATUS),
                trans->rx_status.virt
                    ? *(volatile uint32_t *)trans->rx_status.virt : 0);

        kputs("iwlwifi: the firmware is in the device but has not started; "
              "see docs/wireless.md\n");
        goto fail;
    }

    iwl_write32(trans, CSR_INT, CSR_INT_BIT_ALIVE);
    trans->firmware_running = true;

    kputs("iwlwifi: firmware is running\n");

    /* Drain whatever the firmware has to say for itself.
     *
     * The alive interrupt only proves the firmware started.  The
     * notification behind it comes through the receive ring, and whether
     * that ring works is the question everything after this depends on --
     * every command is answered through it.  Better to find out here, where
     * it can be said plainly, than to have the first command time out for
     * reasons that look like the command's fault. */
    {
        uint64_t drain = time_now_ms() + 500;
        int      seen = 0;

        while (time_now_ms() < drain) {
            struct iwl_rx_packet *pkt = iwl_rx_next(trans);

            if (pkt) {
                kprintf("iwlwifi:   notification 0x%x/0x%x, %u bytes\n",
                        pkt->hdr.group_id, pkt->hdr.cmd,
                        IWL_RX_PACKET_LEN(pkt));
                seen++;
                continue;
            }
            io_wait();
        }

        kprintf("iwlwifi: the receive ring delivered %d notification(s) -- "
                "%s\n", seen,
                seen ? "it is working" : "IT IS NOT WORKING");
    }

    return true;

fail_memory:
    kputs("iwlwifi: not enough memory for the adapter's rings\n");
fail:
    iwl_free_rings(trans);
    iwl_fw_free(&trans->fw);
    iwl_apm_stop(trans);
    return false;
}

/* Unpack a hardware address out of the two registers holding it.
 *
 * The bytes come out reversed within each register, which is why this is a
 * function rather than a memcpy: the first four bytes are the first register
 * read big-end first, and the last two are the bottom half of the second. */
static void iwl_unpack_mac(uint32_t addr0, uint32_t addr1, uint8_t *out)
{
    out[0] = (uint8_t)(addr0 >> 24);
    out[1] = (uint8_t)(addr0 >> 16);
    out[2] = (uint8_t)(addr0 >> 8);
    out[3] = (uint8_t)(addr0);
    out[4] = (uint8_t)(addr1 >> 8);
    out[5] = (uint8_t)(addr1);
}

/* An address is usable if it is not all zeroes, not all ones, and not a
 * multicast address -- a device that has never been programmed reads back as
 * one of those. */
static bool iwl_mac_valid(const uint8_t *mac)
{
    uint8_t all_or = 0, all_and = 0xFF;

    for (int i = 0; i < 6; i++) {
        all_or  |= mac[i];
        all_and &= mac[i];
    }

    return all_or != 0x00 && all_and != 0xFF && !(mac[0] & 1);
}

bool iwl_read_mac_address(iwl_trans_t *trans)
{
    /* What the board maker fused in comes first; the chip's own programmed
     * copy is the fallback for a board that had none fused. */
    iwl_unpack_mac(iwl_read32(trans, CSR_MAC_ADDR0_STRAP),
                   iwl_read32(trans, CSR_MAC_ADDR1_STRAP), trans->mac);

    if (!iwl_mac_valid(trans->mac))
        iwl_unpack_mac(iwl_read32(trans, CSR_MAC_ADDR0_OTP),
                       iwl_read32(trans, CSR_MAC_ADDR1_OTP), trans->mac);

    if (!iwl_mac_valid(trans->mac))
        return false;

    kprintf("iwlwifi: hardware address %x:%x:%x:%x:%x:%x\n",
            trans->mac[0], trans->mac[1], trans->mac[2],
            trans->mac[3], trans->mac[4], trans->mac[5]);
    return true;
}

void iwl_trans_stop(iwl_trans_t *trans)
{
    if (!trans->present)
        return;

    iwl_write32(trans, CSR_INT_MASK, 0);
    trans->firmware_running = false;

    iwl_free_rings(trans);
    iwl_fw_free(&trans->fw);
    iwl_apm_stop(trans);
}
