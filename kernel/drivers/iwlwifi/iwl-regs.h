/* Registers of the Intel 9000-series wireless adapter.
 *
 * ---------------------------------------------------------------------------
 * Derived from Linux, and used under BSD-3-Clause
 *
 * The register addresses and bit values in this file, and the order in which
 * iwl-trans.c programs them, were taken from the iwlwifi driver in the Linux
 * kernel -- principally drivers/net/wireless/intel/iwlwifi/iwl-fh.h,
 * iwl-prph.h and pcie/gen1_2/trans.c.
 *
 *   Copyright (C) 2005-2014, 2018-2026 Intel Corporation
 *   Copyright (C) 2013-2015 Intel Mobile Communications GmbH
 *   Copyright (C) 2015-2017 Intel Deutschland GmbH
 *
 * Those files are dual licensed, "GPL-2.0 OR BSD-3-Clause".  WOS takes the
 * BSD-3-Clause option, which requires that the notice above and the
 * disclaimer that goes with it be kept -- see docs/wireless.md and the
 * README, where the same is recorded.
 *
 * This is worth being exact about.  Register addresses are facts about a
 * piece of silicon and could in principle have been found by other means, but
 * they were not: they were read out of somebody else's source, and several of
 * the sequences here are that source's sequence rather than an independent
 * reconstruction of it.  Saying so is both the licence's requirement and the
 * honest description of where this came from.
 * ---------------------------------------------------------------------------
 *
 * There are three ways to reach a register on this device and it is worth
 * being clear about which is which, because they look alike in code and are
 * not interchangeable:
 *
 *   CSR    the control and status registers, at the bottom of the memory
 *          window the PCI base address register points at.  These are the
 *          only ones readable on a device that has not been powered up, and
 *          they are how it is powered up.
 *
 *   PRPH   "peripheral" registers, inside the device rather than on the bus.
 *          They are not mapped anywhere; reaching one means writing its
 *          address into a window register and then reading a data register,
 *          which is why every access is two or three bus transactions and why
 *          the device must be awake first.
 *
 *   FH     the flow handler -- the DMA engine.  These live in the same memory
 *          window as the CSRs, higher up, and are what the rings are
 *          programmed through.
 *
 * The values here come from the published register description of this
 * hardware generation.  They have been stable across the 2000, 6000, 7000,
 * 8000 and 9000 series, which is some comfort, but none of them has been
 * verified against this adapter.
 */
#ifndef WOS_IWL_REGS_H
#define WOS_IWL_REGS_H

/* ------------------------------------------------------------------ *
 *  Control and status registers
 * ------------------------------------------------------------------ */

#define CSR_HW_IF_CONFIG_REG    0x000
#define CSR_INT_COALESCING      0x004
#define CSR_INT                 0x008
#define CSR_INT_MASK            0x00c
#define CSR_FH_INT_STATUS       0x010
#define CSR_GPIO_IN             0x018
#define CSR_RESET               0x020
#define CSR_GP_CNTRL            0x024
#define CSR_HW_REV              0x028
#define CSR_EEPROM_REG          0x02c
#define CSR_EEPROM_GP           0x030
#define CSR_OTP_GP_REG          0x034
#define CSR_GIO_REG             0x03c
#define CSR_GP_UCODE_REG        0x048
#define CSR_GP_DRIVER_REG       0x050
#define CSR_UCODE_DRV_GP1       0x054
#define CSR_UCODE_DRV_GP1_SET   0x058
#define CSR_UCODE_DRV_GP1_CLR   0x05c
#define CSR_UCODE_DRV_GP2       0x060
#define CSR_MBOX_SET_REG        0x088
#define CSR_LED_REG             0x094
#define CSR_HW_RF_ID            0x09c
#define CSR_DRAM_INT_TBL_REG    0x0a0
#define CSR_MAC_SHADOW_REG_CTRL 0x0a8
/* The adapter's own hardware address, in the configuration registers rather
 * than behind a firmware command.  There are two copies: the one the board
 * maker fused in, and the one in the chip's own one-time-programmable memory,
 * used when the board maker fused nothing.  0x380 is where this family keeps
 * them; other families put them elsewhere. */
#define CSR_MAC_ADDR_BASE       0x380
#define CSR_MAC_ADDR0_OTP       (CSR_MAC_ADDR_BASE + 0x00)
#define CSR_MAC_ADDR1_OTP       (CSR_MAC_ADDR_BASE + 0x04)
#define CSR_MAC_ADDR0_STRAP     (CSR_MAC_ADDR_BASE + 0x08)
#define CSR_MAC_ADDR1_STRAP     (CSR_MAC_ADDR_BASE + 0x0c)

#define CSR_GIO_CHICKEN_BITS    0x100
#define CSR_ANA_PLL_CFG         0x20c
#define CSR_HW_REV_WA_REG       0x22c
#define CSR_MONITOR_STATUS_REG  0x228
#define CSR_DBG_HPET_MEM_REG    0x240
#define CSR_DBG_LINK_PWR_MGMT_REG 0x250

/* The flow handler's wait threshold, set to its maximum.  This is a documented
 * workaround for a hardware error the device raises under load. */
#define CSR_DBG_HPET_MEM_REG_VAL 0xFFFF0000

/* CSR_HW_IF_CONFIG_REG.  The interesting half of this register is the
 * handshake that takes ownership of the device from whatever had it before --
 * on a CNVi part that may be the firmware the system booted with. */
#define CSR_HW_IF_CONFIG_MSK_MAC_DASH     0x00000003
#define CSR_HW_IF_CONFIG_MSK_MAC_STEP     0x0000000c
#define CSR_HW_IF_CONFIG_BIT_MAC_SI       0x00000100
#define CSR_HW_IF_CONFIG_BIT_RADIO_SI     0x00000200
#define CSR_HW_IF_CONFIG_MSK_PHY_TYPE     0x00000c00
#define CSR_HW_IF_CONFIG_MSK_PHY_DASH     0x00003000
#define CSR_HW_IF_CONFIG_MSK_PHY_STEP     0x0000c000
/* Let an interrupt from the management bus pull the PCI Express link up out
 * of its low-power state.  Without this the device can be asked to wake and
 * simply not hear the request, because the link it would answer over is
 * asleep. */
#define CSR_HW_IF_CONFIG_BIT_HAP_WAKE_L1A 0x00080000
#define CSR_HW_IF_CONFIG_BIT_NIC_READY    0x00400000
#define CSR_HW_IF_CONFIG_BIT_NIC_PREPARE_DONE 0x02000000
#define CSR_HW_IF_CONFIG_PREPARE          0x08000000
#define CSR_HW_IF_CONFIG_ENABLE_PME       0x10000000
#define CSR_HW_IF_CONFIG_PERSIST_MODE     0x40000000

/* CSR_GP_CNTRL.  This is the register the whole power-up turns on: asking for
 * access to the device's clock, and waiting for it to say the clock is
 * running.  Nothing else works until that bit comes back. */
#define CSR_GP_CNTRL_MAC_CLOCK_READY      0x00000001
#define CSR_GP_CNTRL_INIT_DONE            0x00000004
#define CSR_GP_CNTRL_MAC_ACCESS_REQ       0x00000008
#define CSR_GP_CNTRL_GOING_TO_SLEEP       0x00000010
#define CSR_GP_CNTRL_XTAL_ON              0x00000400
#define CSR_GP_CNTRL_MAC_POWER_SAVE       0x04000000
#define CSR_GP_CNTRL_HW_RF_KILL_SW        0x08000000

/* CSR_RESET */
#define CSR_RESET_NEVO_RESET              0x00000001
#define CSR_RESET_FORCE_NMI               0x00000002
#define CSR_RESET_SW_RESET                0x00000080
#define CSR_RESET_MASTER_DISABLED         0x00000100
#define CSR_RESET_STOP_MASTER             0x00000200
#define CSR_RESET_LINK_PWR_MGMT_DISABLED  0x80000000

/* CSR_INT -- what the device is telling us.  The driver polls these rather
 * than taking an interrupt: WOS has no PCI interrupt routing for this device,
 * and every path through the wireless code is already a polling loop with a
 * deadline.  Masking every source and reading the status register gives the
 * same information a handler would have had. */
#define CSR_INT_BIT_ALIVE        0x00000001  /* firmware is running       */
#define CSR_INT_BIT_WAKEUP       0x00000004
#define CSR_INT_BIT_SW_RX        0x00000008  /* a command response        */
#define CSR_INT_BIT_RF_KILL      0x00000080  /* the radio switch moved    */
#define CSR_INT_BIT_CT_KILL      0x00000100  /* too hot                   */
#define CSR_INT_BIT_SW_ERR       0x02000000  /* firmware fell over        */
#define CSR_INT_BIT_SCD          0x00040000
#define CSR_INT_BIT_FH_TX        0x00080000
#define CSR_INT_BIT_RX_PERIODIC  0x10000000
#define CSR_INT_BIT_HW_ERR       0x20000000
#define CSR_INT_BIT_FH_RX        0x80000000  /* a frame arrived           */

#define CSR_INI_SET_MASK (CSR_INT_BIT_FH_RX | CSR_INT_BIT_HW_ERR | \
                          CSR_INT_BIT_FH_TX | CSR_INT_BIT_SW_ERR | \
                          CSR_INT_BIT_RF_KILL | CSR_INT_BIT_SW_RX | \
                          CSR_INT_BIT_WAKEUP | CSR_INT_BIT_ALIVE | \
                          CSR_INT_BIT_RX_PERIODIC)

/* CSR_GIO_REG / CSR_GIO_CHICKEN_BITS */
#define CSR_GIO_REG_VAL_L0S_ENABLED       0x00000002
#define CSR_GIO_CHICKEN_L1A_NO_L0S_RX     0x00800000
#define CSR_GIO_CHICKEN_DIS_L0S_TIMER     0x00100000

/* CSR_UCODE_DRV_GP1.  These are the driver's half of a handshake with the
 * device about the radio switch, and they matter more than they look: while
 * the device believes commands are blocked it accepts configuration and
 * performs no work, which from the outside is indistinguishable from a DMA
 * engine that does not function.  They are cleared through the _CLR alias,
 * which clears exactly the bits written to it. */
#define CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP   0x00000001
#define CSR_UCODE_SW_BIT_RFKILL           0x00000002
#define CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED 0x00000004

/* CSR_DRAM_INT_TBL_REG: the interrupt-cause table, which this driver does not
 * use, but whose enable bits must be right for the device to run at all. */
#define CSR_DRAM_INT_TBL_ENABLE           0x80000000
#define CSR_DRAM_INIT_TBL_WRITE_POINTER   0x10000000
#define CSR_DRAM_INIT_TBL_WRAP_CHECK      0x20000000

/* CSR_MBOX_SET_REG */
#define CSR_MBOX_SET_REG_OS_ALIVE         0x00000020

/* ------------------------------------------------------------------ *
 *  The indirect windows
 * ------------------------------------------------------------------ */

#define HBUS_TARG_MEM_RADDR     0x40c
#define HBUS_TARG_MEM_WADDR     0x410
#define HBUS_TARG_MEM_WDAT      0x418
#define HBUS_TARG_MEM_RDAT      0x41c
#define HBUS_TARG_PRPH_WADDR    0x444
#define HBUS_TARG_PRPH_RADDR    0x448
#define HBUS_TARG_PRPH_WDAT     0x44c
#define HBUS_TARG_PRPH_RDAT     0x450
#define HBUS_TARG_WRPTR         0x460

/* The two high bits of a peripheral address select a three-byte access.
 * Every driver for this hardware writes them and none explains them. */
#define HBUS_PRPH_ACCESS_MASK   0x03000000

/* ------------------------------------------------------------------ *
 *  Peripheral registers
 * ------------------------------------------------------------------ */

/* Take the device's processors out of reset, so that they can accept the
 * firmware about to be sent to them.
 *
 * Until this is written the DMA engine can be configured and enabled and will
 * carry nothing: the transfer is addressed to a processor that is held in
 * reset and not listening.  From the host side that is indistinguishable from
 * a DMA engine that does not work, and it cost this driver an evening.
 *
 * Note the address is the same as APMG_PS_CTRL_REG below.  That is not a
 * mistake: the older families have a power-management register there, and the
 * 8000 series onwards -- which have no APMG block at all -- put the processor
 * reset control at the same offset. */
#define RELEASE_CPU_RESET       0x300C
#define RELEASE_CPU_RESET_BIT   0x01000000

/* Where the driver tells the firmware how much of itself has arrived: a bit
 * per section, in the low half for the first processor and the high half for
 * the second. */
#define FH_UCODE_LOAD_STATUS    0x1AF0

/* Sections loading into this window need an extra bit set first, because the
 * address does not otherwise fit the space the DMA engine addresses. */
#define LMPM_CHICK                     0xA01FF8
#define LMPM_CHICK_EXTENDED_ADDR_SPACE 0x00000001
#define IWL_FW_MEM_EXTENDED_START      0x40000
#define IWL_FW_MEM_EXTENDED_END        0x57FFF

#define APMG_CLK_CTRL_REG       0x3000
#define APMG_CLK_EN_REG         0x3004
#define APMG_CLK_DIS_REG        0x3008
#define APMG_PS_CTRL_REG        0x300c
#define APMG_PCIDEV_STT_REG     0x3010
#define APMG_RTC_INT_STT_REG    0x3014
#define APMG_RTC_INT_MSK_REG    0x3018
#define APMG_DIGITAL_SVR_REG    0x3058
#define APMG_ANALOG_SVR_REG     0x306c

#define APMG_CLK_VAL_DMA_CLK_RQT      0x00000200
#define APMG_CLK_VAL_BSM_CLK_RQT      0x00000800
#define APMG_PS_CTRL_VAL_RESET_REQ    0x04000000
#define APMG_PS_CTRL_MSK_PWR_SRC      0x03000000
#define APMG_PS_CTRL_VAL_PWR_SRC_VMAIN 0x00000000
#define APMG_PCIDEV_STT_VAL_PERSIST_DIS 0x00000200
#define APMG_PCIDEV_STT_VAL_L1_ACT_DIS  0x00000800

/* The scheduler: the block that decides which queue gets the air next. */
#define SCD_BASE                    0xa02c00
#define SCD_SRAM_BASE_ADDR          (SCD_BASE + 0x0)
#define SCD_DRAM_BASE_ADDR          (SCD_BASE + 0x8)
#define SCD_AIT                     (SCD_BASE + 0x18)
#define SCD_TXFACT                  (SCD_BASE + 0x10)
#define SCD_ACTIVE                  (SCD_BASE + 0x24)
#define SCD_QUEUECHAIN_SEL          (SCD_BASE + 0xe8)
#define SCD_CHAINEXT_EN             (SCD_BASE + 0x244)
#define SCD_AGGR_SEL                (SCD_BASE + 0x248)
#define SCD_INTERRUPT_MASK          (SCD_BASE + 0x108)
#define SCD_EN_CTRL                 (SCD_BASE + 0x254)
#define SCD_QUEUE_RDPTR(q)          (SCD_BASE + 0x68 + (q) * 4)
#define SCD_QUEUE_WRPTR(q)          (SCD_BASE + 0x18 + (q) * 4)
#define SCD_QUEUE_STATUS_BITS(q)    (SCD_BASE + 0x10c + (q) * 4)

#define SCD_QUEUE_STTS_REG_POS_ACTIVE   0
#define SCD_QUEUE_STTS_REG_POS_TXF      1
#define SCD_QUEUE_STTS_REG_POS_WSL      5
#define SCD_QUEUE_STTS_REG_MSK          0x017f0000

/* Where the firmware is told to look for things, once it is running. */
#define RFH_Q0_FRBDCB_BA_LSB        0xa08000
#define RFH_Q_FRBDCB_BA_LSB(q)      (RFH_Q0_FRBDCB_BA_LSB + (q) * 8)
#define RFH_Q0_FRBDCB_WIDX          0xa08080
#define RFH_Q_FRBDCB_WIDX(q)        (RFH_Q0_FRBDCB_WIDX + (q) * 4)
#define RFH_Q0_FRBDCB_RIDX          0xa080c0
#define RFH_Q_FRBDCB_RIDX(q)        (RFH_Q0_FRBDCB_RIDX + (q) * 4)
#define RFH_Q0_URBDCB_BA_LSB        0xa08100
#define RFH_Q_URBDCB_BA_LSB(q)      (RFH_Q0_URBDCB_BA_LSB + (q) * 8)
#define RFH_Q0_URBD_STTS_WPTR_LSB   0xa08200
#define RFH_Q_URBD_STTS_WPTR_LSB(q) (RFH_Q0_URBD_STTS_WPTR_LSB + (q) * 8)
#define RFH_GEN_CFG                 0xa09800
#define RFH_RXF_DMA_CFG             0xa09820
#define RFH_RXF_RXQ_ACTIVE          0xa0980c

/* The free list's write index, which unlike the rest of the RFH registers is
 * an ordinary memory-mapped word rather than one behind the window.  Handing
 * buffers to the device means writing this one. */
#define RFH_Q0_FRBDCB_WIDX_TRG         0x1C80
#define RFH_Q_FRBDCB_WIDX_TRG(q)       (RFH_Q0_FRBDCB_WIDX_TRG + (q) * 4)

#define RFH_Q0_URBDCB_WIDX             0xa08180
#define RFH_Q_URBDCB_WIDX(q)           (RFH_Q0_URBDCB_WIDX + (q) * 4)

#define RFH_GEN_CFG_SERVICE_DMA_SNOOP  0x00000001
#define RFH_GEN_CFG_RFH_DMA_SNOOP      0x00000002
#define RFH_GEN_CFG_RB_CHUNK_SIZE_128  0x00000010
#define RFH_GEN_CFG_DEFAULT_RXQ_NUM    0x00000F00

#define RFH_DMA_EN_ENABLE_VAL          0x80000000
#define RFH_RXF_DMA_RB_SIZE_4K         0x00040000
#define RFH_RXF_DMA_RBDCB_SIZE_512     0x00900000
#define RFH_RXF_DMA_MIN_RB_4_8         0x03000000
#define RFH_RXF_DMA_DROP_TOO_LARGE     0x04000000

/* ------------------------------------------------------------------ *
 *  The flow handler -- the DMA engine
 * ------------------------------------------------------------------ */

#define FH_MEM_LOWER_BOUND          0x1000

/* Where each transmit queue's ring is */
#define FH_MEM_CBBC_0_15_LOWER_BOUND 0x9d0
#define FH_MEM_CBBC_QUEUE(q)         (FH_MEM_CBBC_0_15_LOWER_BOUND + (q) * 4)

/* The receive side, on the older path used while firmware is loading */
#define FH_MEM_RSCSR_LOWER_BOUND      (FH_MEM_LOWER_BOUND + 0xbc0)
#define FH_RSCSR_CHNL0_STTS_WPTR_REG  (FH_MEM_RSCSR_LOWER_BOUND + 0x000)
#define FH_RSCSR_CHNL0_RBDCB_BASE_REG (FH_MEM_RSCSR_LOWER_BOUND + 0x004)
#define FH_RSCSR_CHNL0_WPTR           (FH_MEM_RSCSR_LOWER_BOUND + 0x008)

#define FH_MEM_RCSR_LOWER_BOUND       (FH_MEM_LOWER_BOUND + 0xc00)
#define FH_MEM_RCSR_CHNL0_CONFIG_REG  (FH_MEM_RCSR_LOWER_BOUND + 0x000)
#define FH_MEM_RCSR_CHNL0_RBDCB_WPTR  (FH_MEM_RCSR_LOWER_BOUND + 0x008)

#define FH_MEM_RSSR_LOWER_BOUND       (FH_MEM_LOWER_BOUND + 0xc40)
#define FH_MEM_RSSR_SHARED_CTRL_REG   (FH_MEM_RSSR_LOWER_BOUND + 0x000)
#define FH_MEM_RSSR_RX_STATUS_REG     (FH_MEM_RSSR_LOWER_BOUND + 0x004)

#define FH_RSSR_CHNL0_RX_STATUS_CHNL_IDLE 0x01000000

/* Transmit control */
#define FH_TCSR_LOWER_BOUND           (FH_MEM_LOWER_BOUND + 0xd00)
#define FH_TCSR_CHNL_TX_CONFIG_REG(c) (FH_TCSR_LOWER_BOUND + 0x20 * (c))
#define FH_TCSR_CHNL_TX_CREDIT_REG(c) (FH_TCSR_LOWER_BOUND + 0x20 * (c) + 0x4)
#define FH_TCSR_CHNL_TX_BUF_STS_REG(c) (FH_TCSR_LOWER_BOUND + 0x20 * (c) + 0x8)

#define FH_TCSR_TX_CONFIG_DMA_PAUSE          0x00000000
#define FH_TCSR_TX_CONFIG_DMA_ENABLE         0x80000000
#define FH_TCSR_TX_CONFIG_CREDIT_DISABLE     0x00000008
#define FH_TCSR_TX_CONFIG_CIRQ_HOST_ENDTFD   0x00100000

#define FH_TCSR_CHNL_TX_BUF_STS_TB_NUM_POS   20
#define FH_TCSR_CHNL_TX_BUF_STS_TB_IDX_POS   12
#define FH_TCSR_CHNL_TX_BUF_STS_TFDB_VALID   0x00000003

#define FH_TSSR_LOWER_BOUND           (FH_MEM_LOWER_BOUND + 0xea0)
#define FH_TSSR_TX_MSG_CONFIG_REG     (FH_TSSR_LOWER_BOUND + 0x008)
#define FH_TSSR_TX_STATUS_REG         (FH_TSSR_LOWER_BOUND + 0x010)

/* The channel used to push firmware into the device before it is running. */
#define FH_SRVC_CHNL                  9
#define FH_SRVC_LOWER_BOUND           (FH_MEM_LOWER_BOUND + 0x9c8)
#define FH_SRVC_CHNL_SRAM_ADDR_REG(c) (FH_SRVC_LOWER_BOUND + ((c) - 9) * 0x4)

#define FH_TFDIB_LOWER_BOUND          (FH_MEM_LOWER_BOUND + 0x900)
#define FH_TFDIB_CTRL0_REG(c)         (FH_TFDIB_LOWER_BOUND + 0x8 * (c))
#define FH_TFDIB_CTRL1_REG(c)         (FH_TFDIB_LOWER_BOUND + 0x8 * (c) + 0x4)
#define FH_TFDIB_CTRL1_ADDR_BITSHIFT  28

/* Keep-warm: a page the device touches to stop its memory controller from
 * powering down mid-transfer.  It is never read by anybody. */
#define FH_KW_MEM_ADDR_REG            (FH_MEM_LOWER_BOUND + 0x97c)

#define FH_MEM_TB_MAX_LENGTH          0x00020000

/* CSR_FH_INT_STATUS bits, for spotting that a firmware chunk landed. */
#define FH_INT_TX_CHNL0               0x00010000
#define FH_INT_RX_CHNL0               0x01000000

/* ------------------------------------------------------------------ *
 *  Ring geometry
 * ------------------------------------------------------------------ */

/* Both rings are power-of-two sized because the hardware wraps their indices
 * by masking rather than by comparing. */
#define IWL_TX_RING_SIZE   256
#define IWL_RX_RING_SIZE   256

/* The buffer each received frame lands in.  4 KiB is the size the receive
 * DMA is configured for above, and one frame never needs more. */
#define IWL_RX_BUF_SIZE    4096

/* A transmit descriptor: a small header and up to this many scatter-gather
 * entries.  Two is all this driver uses -- the command header, and the body
 * that follows it. */
#define IWL_NUM_TBS        20

/* The queues.  The firmware assigns meaning to the numbers: the last one is
 * where host commands go, and the low ones carry traffic. */
#define IWL_CMD_QUEUE      9
#define IWL_DATA_QUEUE     0
#define IWL_MGMT_QUEUE     4
#define IWL_NUM_QUEUES     31

#endif /* WOS_IWL_REGS_H */
