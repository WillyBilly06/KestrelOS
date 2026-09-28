/* rtw89.h - the registers a Wi-Fi 6E/7 Realtek part is started through.
 *
 * The 8852 and 8922 parts share a register map that has nothing to do with
 * the one in rtw.c.  That is the whole reason this file exists: the older
 * driver's addresses still exist on these chips and still accept writes, they
 * simply mean something else - so starting a Wi-Fi 7 card with the older
 * sequence is not a driver that fails, it is a driver that writes an
 * unrelated pattern into a working card's control registers.
 *
 * Every address and bit below is transcribed from Realtek's own published
 * driver (reg.h and rtw8922a.c in the rtw89 tree), not remembered.  A bit in
 * the wrong place here does not produce an error; it produces a card that
 * never reports ready, which looks identical to a card that is broken.
 */
#ifndef KESTREL_RTW89_H
#define KESTREL_RTW89_H

#include "kernel.h"

/* --------------------------------------------------------- system power */
#define R_BE_SYS_ISO_CTRL              0x0000
#define B_BE_PWC_EV2EF_S               (1u << 14)
#define B_BE_PWC_EV2EF_B               (1u << 15)
#define B_BE_ISO_EB2CORE               (1u << 8)

#define R_BE_SYS_PW_CTRL               0x0004
#define B_BE_APFM_SWLPS                (1u << 10)
#define B_BE_AFSM_WLSUS_EN             (1u << 11)
#define B_BE_AFSM_PCIE_SUS_EN          (1u << 12)
#define B_BE_APDM_HPDN                 (1u << 15)
#define B_BE_EN_WLON                   (1u << 16)
#define B_BE_RDY_SYSPWR                (1u << 17)
#define B_BE_DIS_WLBT_PDNSUSEN_SOPC    (1u << 18)
#define B_BE_APFN_ONMAC                (1u << 8)

#define R_BE_ANAPAR_POW_MAC            0x0016   /* eight bits wide */
#define B_BE_POW_PC_LDO_PORT0          (1u << 2)
#define B_BE_POW_PC_LDO_PORT1          (1u << 3)

#define R_BE_SYS_ADIE_PAD_PWR_CTRL     0x0018
#define B_BE_SYM_PADPDN_WL_RFC0_1P3    (1u << 5)
#define B_BE_SYM_PADPDN_WL_RFC1_1P3    (1u << 6)

#define R_BE_SYS_SDIO_CTRL             0x0070
#define B_BE_PCIE_FORCE_IBX_EN         (1u << 12)

#define R_BE_HCI_OPT_CTRL              0x0074
#define B_BE_HAXIDMA_IO_EN             (1u << 24)
#define B_BE_HAXIDMA_BACKUP_RESTORE_ST (1u << 26)
#define B_BE_HAXIDMA_IO_ST             (1u << 27)
#define B_BE_HCI_WLAN_IO_EN            (1u << 28)
#define B_BE_HCI_WLAN_IO_ST            (1u << 31)

#define R_BE_FEN_RST_ENABLE            0x0084
#define B_BE_FEN_BBPLAT_RSTB           (1u << 0)
#define B_BE_FEN_BB_IP_RSTN            (1u << 1)
#define B_BE_R_SYM_ISO_ADDA_P02PP      (1u << 20)
#define B_BE_R_SYM_ISO_ADDA_P12PP      (1u << 21)

#define R_BE_PLATFORM_ENABLE           0x0088   /* eight bits wide */
#define B_BE_PLATFORM_EN               (1u << 0)

#define R_BE_WLLPS_CTRL                0x0090
#define B_BE_DIS_WLBT_LPSEN_LOPC       (1u << 1)

#define R_BE_WLRESUME_CTRL             0x0094
#define B_BE_LPSROP_CMAC0              (1u << 12)
#define B_BE_LPSROP_CMAC1              (1u << 13)

#define R_BE_PMC_DBG_CTRL2             0x00CC
#define B_BE_SYSON_DIS_PMCR_BE_WRMSK   (1u << 2)

/* The cut/version field keeps its AX-generation address on BE parts. */
#define R_AX_SYS_CFG1                  0x00F0
#define B_AX_CHIP_VER_MASK             0x0000F000u
#define B_AX_CHIP_VER_SHIFT            12

#define R_BE_AFE_ON_CTRL1              0x0244
#define B_BE_REG_CK_MON_CK960M_EN      (1u << 28)

/* ------------------------------------------------ the crystal's side band
 *
 * A second register file reached one byte at a time through a mailbox: an
 * address, a value, a mask and a mode go into one word, a poll bit is set, and
 * the hardware clears it when the byte has landed.
 */
#define R_BE_WLAN_XTAL_SI_CTRL         0x0270
#define B_BE_WL_XTAL_SI_ADDR_SHIFT     0        /* 7:0   */
#define B_BE_WL_XTAL_SI_DATA_SHIFT     8        /* 15:8  */
#define B_BE_WL_XTAL_SI_BITMASK_SHIFT  16       /* 23:16 */
#define B_BE_WL_XTAL_SI_MODE_SHIFT     24       /* 25:24 */
#define B_BE_WL_XTAL_SI_CHIPID_SHIFT   28       /* 30:28 */
#define B_BE_WL_XTAL_SI_CMD_POLL       (1u << 31)
#define XTAL_SI_NORMAL_WRITE           0x00

#define XTAL_SI_XREF_RF1               0x2D
#define XTAL_SI_XREF_RF2               0x2E
#define XTAL_SI_WL_RFC_S0              0x80
#define XTAL_SI_WL_RFC_S1              0x81
#define XTAL_SI_ANAPAR_WL              0x90
#define XTAL_SI_PLL                    0xE0
#define XTAL_SI_PLL_1                  0xE1

/* ------------------------------------------------------- the MAC blocks */
#define R_BE_DMAC_FUNC_EN              0x8400
#define B_BE_DLE_DATACPUIO_EN          (1u << 8)
#define B_BE_P_AXIDMA_EN               (1u << 9)
#define B_BE_LTR_CTL_EN                (1u << 7)
#define B_BE_PLRLS_EN                  (1u << 10)
#define B_BE_DMAC_MLO_EN               (1u << 11)
#define B_BE_H_AXIDMA_EN               (1u << 14)
#define B_BE_MAC_SEC_EN                (1u << 16)
#define B_BE_BBRPT_EN                  (1u << 17)
#define B_BE_DISPATCHER_EN             (1u << 18)
#define B_BE_DLE_CPUIO_EN              (1u << 19)
#define B_BE_PKT_IN_EN                 (1u << 20)
#define B_BE_DMAC_TBL_EN               (1u << 21)
#define B_BE_PKT_BUF_EN                (1u << 22)
#define B_BE_DLE_PLE_EN                (1u << 23)
#define B_BE_STA_SCH_EN                (1u << 24)
#define B_BE_TXPKT_CTRL_EN             (1u << 25)
#define B_BE_DLE_WDE_EN                (1u << 26)
#define B_BE_WD_RLS_EN                 (1u << 27)
#define B_BE_MPDU_PROC_EN              (1u << 28)
#define B_BE_DMAC_FUNC_EN_BIT          (1u << 29)
#define B_BE_MAC_FUNC_EN               (1u << 30)

#define R_BE_CMAC_SHARE_FUNC_EN        0x0E000
#define B_BE_BTCOEX_EN                 (1u << 0)
#define B_BE_ADDRSRCH_EN               (1u << 1)
#define B_BE_RESPBA_EN                 (1u << 2)
#define B_BE_CMAC_SHARE_EN             (1u << 30)

#define R_BE_CMAC_FUNC_EN              0x10000
#define B_BE_RMAC_EN                   (1u << 0)
#define B_BE_TMAC_EN                   (1u << 1)
#define B_BE_SCHEDULER_EN              (1u << 2)
#define B_BE_PTCLTOP_EN                (1u << 3)
#define B_BE_CMAC_DMA_EN               (1u << 4)
#define B_BE_PHYINTF_EN                (1u << 5)
#define B_BE_SIGB_EN                   (1u << 6)
#define B_BE_RESP_PKTCTL_EN            (1u << 7)
#define B_BE_TXTIME_EN                 (1u << 8)
#define B_BE_CMAC_RXEN                 (1u << 28)
#define B_BE_CMAC_TXEN                 (1u << 29)
#define B_BE_CMAC_EN                   (1u << 30)

/* The register window this part presents.  CMAC sits at 0x10000, so the
 * window has to reach past it. */
/* Enough to reach the radio's second interface, which is memory mapped at
 * 0x2e060 and 0x2f060 rather than reached through the serial one. */
/* Enough to reach the signal processor's own window at 0x30000, which is
 * the highest thing this driver touches. */
#define RTW89_REG_BYTES                0x40000

/* ------------------------------------------------------- the DMA engine
 *
 * The card moves packets by reading and writing rings in host memory, and the
 * engine that does it sits on the bus side of the chip with its own register
 * block.  It has to be stopped, its channel pointers cleared, and started
 * again before anything can be handed to it - and stopping it means waiting
 * for it to finish, not merely asking.
 *
 * Fifteen transmit channels and two receive ones, and the pointers of every
 * one of them are cleared through a single register with a bit each.
 */
#define R_BE_HAXI_INIT_CFG1            0xB000
#define B_BE_TXDMA_EN                  (1u << 4)
#define B_BE_RXDMA_EN                  (1u << 5)
#define B_BE_STOP_AXI_MST              (1u << 7)

#define R_BE_HAXI_DMA_STOP1            0xB010
#define B_BE_STOP_WPDMA                (1u << 31)

#define R_BE_TXBD_RWPTR_CLR1           0xB014
#define R_BE_RXBD_RWPTR_CLR1_V1        0xB018
#define B_BE_CLR_RXQ0_IDX              (1u << 0)
#define B_BE_CLR_RPQ0_IDX              (1u << 1)

#define R_BE_HAXI_DMA_BUSY1            0xB01C
#define B_BE_RXQ0_BUSY_V1              (1u << 16)
#define B_BE_RPQ0_BUSY_V1              (1u << 17)

#define R_BE_HAXI_MST_WDT_TIMEOUT_SEL  0xB02C
#define B_BE_HAXI_MST_WDT_TIMEOUT_MASK 0x1Fu        /* 4:0 */

#define R_BE_RXQ0_RXBD_IDX_V1          0xB160      /* sixteen bits wide */
#define R_BE_RPQ0_RXBD_IDX_V1          0xB164

/* The fifteen transmit channels, and the mask that clears or watches them
 * all.  Written out rather than as a range, because "channels 0 to 14" is a
 * fact about this chip and not a loop bound to be guessed at. */
#define BE_TX_CHANNELS                 15
#define BE_ALL_TX_CHANNELS             0x7FFFu     /* bits 0..14 */

/* Stop the engine, clear every channel's pointers, and start it again.
 * `rx_ring_slots` is how many entries each receive ring has; the card is told
 * one less, because the index it wants is of the last slot rather than the
 * count. */
bool rtw89_dma_reset(volatile u8 *regs, u16 rx_ring_slots, const char *who);
bool rtw89_dma_quiesce(volatile u8 *regs, const char *who);
/* Establish the BE PCIe firmware-download DLE and CH12-only flow control. */
bool rtw89_fwdl_preinit(volatile u8 *regs, const char *who);

/* Whether every channel has finished what it was doing. */
bool rtw89_dma_idle(volatile u8 *regs);

/* ------------------------------------------------ starting the card's CPU
 *
 * The firmware is executed by a processor on the card, and that processor has
 * to be started in download mode before anything can be pushed into it: told
 * which of the four processors are being loaded, given a reason for booting,
 * held in reset, released, and then polled until it says a path is open.
 *
 * The debug registers are cleared first, and read first.  Realtek's driver
 * complains when they are not already empty, because a value left in them is
 * the previous life of a card that fell over rather than stopped - which is
 * worth knowing before deciding a download failed for some new reason.
 */
#define R_BE_SYS_CLK_CTRL              0x0008
#define B_BE_CPU_CLK_EN                (1u << 14)

#define R_BE_HALT_H2C_CTRL             0x0160
#define R_BE_HALT_C2H_CTRL             0x0164
#define R_BE_HALT_H2C                  0x0168
#define R_BE_HALT_C2H                  0x016C
#define R_BE_SYS_CFG5                  0x0170
#define B_BE_WDT_WAKE_USB_EN           (1u << 9)
#define B_BE_WDT_WAKE_PCIE_EN          (1u << 10)

#define R_BE_HISR0                     0x01A4
#define B_BE_HALT_C2H_INT              (1u << 21)

#define R_BE_WCPU_FW_CTRL              0x01E0
#define B_BE_RUN_ENV_MASK              (3u << 30)
#define R_BE_SECURE_BOOT_MALLOC_INFO    0x0184
#define R_BE_DCPU_PLATFORM_ENABLE      0x0888
#define B_BE_DCPU_PLATFORM_EN          (1u << 0)
#define B_BE_DLFW_PATH_RDY             (1u << 0)
#define B_BE_H2C_PATH_RDY              (1u << 1)
#define B_BE_WCPU_ROM_CUT_GET          (1u << 8)
#define B_BE_WLANCPU_FWDL_EN           (1u << 9)
#define B_BE_DATACPU_FWDL_EN           (1u << 10)
#define B_BE_BBMCU0_FWDL_EN            (1u << 11)
#define B_BE_BBMCU1_FWDL_EN            (1u << 12)
#define B_BE_WDT_PLT_RST_EN            (1u << 17)
#define B_BE_WCPU_FWDL_STATUS_SHIFT    26            /* 29:26 */
#define B_BE_WCPU_FWDL_STATUS_MASK     0xFu

#define R_BE_BOOT_REASON               0x01E6        /* sixteen bits wide */
#define B_BE_BOOT_REASON_MASK          0x7u          /* 2:0 */

#define R_BE_UDM1                      0x01F4
#define R_BE_UDM0                      0x01F0
#define R_BE_UDM2                      0x01F8

/* Two more bits of the platform register, which the power-on sequence already
 * uses for something else. */
#define B_BE_WCPU_EN                   (1u << 1)
#define B_BE_HOLD_AFTER_RESET          (1u << 11)

/* What the card says about a download in progress. */
#define RTW89_FWDL_INITIAL_STATE       0
#define RTW89_FWDL_FWDL_ONGOING        1
#define RTW89_FWDL_CHECKSUM_FAIL       2
#define RTW89_FWDL_SECURITY_FAIL       3
#define RTW89_FWDL_CV_NOT_MATCH        4
/* 5 is reserved and unused.  It is written down because leaving it out is
 * what put the two below it one place low - they are only ever compared
 * against each other here, so nothing broke, but a constant that disagrees
 * with the source is a constant that will be trusted later. */
#define RTW89_FWDL_RSVD0               5
#define RTW89_FWDL_WCPU_FWDL_RDY       6
#define RTW89_FWDL_WCPU_FW_INIT_RDY    7

/* Start the card's processor with the download path open.  `include_bb` also
 * loads the baseband's processor, which the 8922 needs. */
bool rtw89_fwdl_start_cpu(volatile u8 *regs, u8 boot_reason, bool include_bb,
                          const char *who);

/* Wait for a path to open: the download one, or the command one. */
bool rtw89_fwdl_path_ready(volatile u8 *regs, bool h2c, const char *who);
bool rtw89_fw_wait_running(volatile u8 *regs, int ms, const char *who);

/* Where the download has got to, as one of the RTW89_FWDL_ values above. */
u8 rtw89_fwdl_status(volatile u8 *regs);

/* ------------------------------------------------- reaching the radio itself
 *
 * The radio is not on the register bus.  It sits behind a small serial
 * interface, and reaching one of its registers means writing an address into
 * one place, asking for a read, and waiting for a different register to say
 * the answer is there.  Realtek calls that interface HWSI.
 *
 * Two paths, because the chip has two radios - one per antenna chain - and
 * they have separate windows onto their own serial interface.
 *
 * Everything below is from Linux's rtw89, which is dual licensed GPL-2.0 or
 * BSD-3-Clause; the second is what makes it usable here.  Copyright (c) 2023
 * Realtek Corporation.
 */
#define RTW89_RF_PATH_A            0
#define RTW89_RF_PATH_B            1
#define RTW89_RF_PATHS             2

/* Linux accesses these through phy_read/write32, hence the BE PHY base is
 * part of their MMIO address just as it is for the direct RF interface. */
#define R_HWSI_ADD(path)           (RTW89_PHY_CR_BASE + ((path) ? 0x2BDCu : 0x2ADCu))
#define R_HWSI_DATA(path)          (RTW89_PHY_CR_BASE + ((path) ? 0x2BE0u : 0x2AE0u))
#define R_HWSI_VAL(path)           (RTW89_PHY_CR_BASE + ((path) ? 0x2D24u : 0x2C24u))

#define B_HWSI_ADD_CTL_MASK        0x7u          /* 2:0   */
#define B_HWSI_ADD_POLL_MASK       0x3u          /* 1:0   */
#define B_HWSI_ADD_RD              (1u << 2)
#define B_HWSI_ADD_MASK            0xFF0u        /* 11:4  */
#define B_HWSI_ADD_SHIFT           4

#define B_HWSI_DATA_ADDR_MASK      0xFFu         /* 7:0   */
#define B_HWSI_DATA_VAL_MASK       0x0FFFFF00u   /* 27:8  */
#define B_HWSI_DATA_VAL_SHIFT      8

#define B_HWSI_VAL_BUSY            (1u << 29)
#define B_HWSI_VAL_RDONE           (1u << 31)

/* Which of the radio's two interfaces an address means.
 *
 * This is not part of the address.  Bit 16 selects how the request travels:
 * clear, it goes through the serial interface above; set, the register is
 * plainly memory mapped and is read and written like any other.
 *
 * Both interfaces carry registers with the SAME low address - 0x18 and
 * 0x10018 are both "register 0x18" and are two different registers.  Masking
 * the address to eight bits, which is what the serial interface's address
 * field holds, sends both to the same place: the second write lands on top of
 * the first, half the radio is never tuned, and nothing reports an error.
 */
#define RTW89_RF_ADDR_ADSEL_MASK   0x00010000u   /* bit 16 */

/* Where the memory-mapped one lives: the control registers' own base, plus a
 * base per path, plus the register's address scaled to a word. */
#define RTW89_PHY_CR_BASE          0x20000u
#define RTW89_RF_BASE(path)        ((path) ? 0xF000u : 0xE000u)
#define RTW89_RF_DIRECT(path, addr) \
    (RTW89_PHY_CR_BASE + RTW89_RF_BASE(path) + (((addr) & 0xFFu) << 2))

/* How much of the card is actually mapped.
 *
 * Told rather than assumed, because the radio's memory-mapped interface sits
 * far above everything else this driver touches - roughly 0x2f000, where the
 * rest lives below 0x10400.  A card whose window is smaller than that is a
 * card where those writes would land outside the mapping entirely, on whatever
 * happens to follow it.  Refusing is the only safe answer and it needs the
 * length to know.
 */
void rtw89_set_register_window(u32 bytes);

#define RTW89_RF_MASK              0xFFFFFu      /* a radio register is 20 bits */
#define RTW89_RF_INVALID           0xFFFFFFFFu

/* ------------------------------------------- frames, in and out ----------
 *
 * A frame does not go to the card on its own.  It is preceded by a descriptor
 * the card reads first, which says how long the frame is, which queue it
 * belongs to, what rate to send it at and whether it is encrypted.
 *
 * The descriptor is two halves.  The body is always present; the info half is
 * optional and is what carries retry limits and RTS.  Both are eight words,
 * so a described frame costs 64 bytes in front of it.
 */
#define RTW89_TXWD_BODY_BYTES      32
#define RTW89_TXWD_INFO_BYTES      32
#define RTW89_TXD_BYTES            (RTW89_TXWD_BODY_BYTES + RTW89_TXWD_INFO_BYTES)

#define BE_TXD_BODY0_WD_PAGE       0x00000080u   /* bit 7  */
#define BE_TXD_BODY0_HDR_LLC_LEN   0x0000F800u   /* 15:11 */
#define BE_TXD_BODY0_CH_DMA        0x000F0000u   /* 19:16 */
#define BE_TXD_BODY0_WDINFO_EN     0x00400000u   /* bit 22 */
#define BE_TXD_BODY0_WP_OFFSET_V1  0x0F000000u   /* 27:24 */

#define BE_TXD_BODY1_SEC_TYPE      0x0000F000u   /* 15:12 */
#define BE_TXD_BODY1_SEC_KEYID     0x00030000u   /* 17:16 */
#define BE_TXD_BODY1_ADDR_INFO_NUM 0xFC000000u   /* 31:26 */

#define BE_TXD_BODY2_TXPKTSIZE     0x00003FFFu   /* 13:0  */
#define BE_TXD_BODY2_AGG_EN        0x00004000u   /* bit 14 */
#define BE_TXD_BODY2_BK            0x00008000u   /* bit 15 */
#define BE_TXD_BODY2_QSEL          0x007E0000u   /* 22:17 */
#define BE_TXD_BODY2_TID_IND       0x00800000u   /* bit 23 */
#define BE_TXD_BODY2_MACID         0xFF000000u   /* 31:24 */

#define BE_TXD_BODY3_WIFI_SEQ      0x00000FFFu   /* 11:0  */
#define BE_TXD_BODY3_MLO_FLAG      0x00001000u   /* bit 12 */
#define BE_TXD_BODY3_IS_MLD_SW_EN  0x00002000u   /* bit 13 */

#define BE_TXD_BODY6_UPD_WLAN_HDR  0x00002000u   /* bit 13 */

#define BE_TXD_BODY7_DATA_ER       0x00000400u   /* bit 10 */
#define BE_TXD_BODY7_DATA_BW_ER    0x00000800u   /* bit 11 */
#define BE_TXD_BODY7_GI_LTF        0x0000E000u   /* 15:13 */
#define BE_TXD_BODY7_DATARATE      0x0FFF0000u   /* 27:16 */
#define BE_TXD_BODY7_DATA_BW       0x70000000u   /* 30:28 */
#define BE_TXD_BODY7_USERATE_SEL   0x80000000u   /* bit 31 */

#define BE_TXD_INFO0_MULTIPORT_ID  0x00000070u   /* 6:4   */
#define BE_TXD_INFO0_DISDATAFB     0x00000400u   /* bit 10 */
#define BE_TXD_INFO0_DATA_LDPC     0x00000800u   /* bit 11 */
#define BE_TXD_INFO0_DATA_STBC     0x00001000u   /* bit 12 */
#define BE_TXD_INFO0_DATA_TXCNT_LMT     0x003F0000u   /* 21:16 */
#define BE_TXD_INFO0_DATA_TXCNT_LMT_SEL 0x00400000u   /* bit 22 */

#define BE_TXD_INFO1_MAX_AGG_NUM   0x000000FFu   /* 7:0   */
#define BE_TXD_INFO1_A_CTRL_BSR    0x00004000u   /* bit 14 */
#define BE_TXD_INFO1_DATA_RTY_LOWEST_RATE 0x0FFF0000u  /* 27:16 */
#define BE_TXD_INFO1_SW_DEFINE     0xF0000000u   /* 31:28 */

#define BE_TXD_INFO2_SEC_CAM_IDX   0x000000FFu   /* 7:0   */
#define BE_TXD_INFO2_FORCE_KEY_EN  0x00000100u   /* bit 8  */
#define BE_TXD_INFO2_AMPDU_DENSITY 0x001C0000u   /* 20:18 */
#define BE_TXD_INFO2_SPE_RPT_V1    0x40000000u   /* bit 30 */

#define BE_TXD_INFO4_RTS_EN        0x08000000u   /* bit 27 */
#define BE_TXD_INFO4_HW_RTS_EN     0x80000000u   /* bit 31 */

/* Which queue a management frame belongs to, and which ring carries that
 * queue.  These are two different numbers for the same thing and the card
 * checks that they agree - the descriptor names the queue, the doorbell names
 * the ring. */
#define RTW89_TX_QSEL_B0_MGMT      0x12
#define RTW89_TXCH_CH8             8

#define R_BE_CH8_TXBD_NUM          0xB040
#define R_BE_CH8_TXBD_IDX          0xB120
#define R_BE_CH8_TXBD_DESA_L       0xB240
#define R_BE_CH8_TXBD_DESA_H       0xB244

/* And the other direction.  The card writes a descriptor in front of every
 * frame it received, and what follows the descriptor is not the frame: there
 * can be a radio report, a driver area and a header-conversion area between
 * them, each with its own unit of measurement. */
#define RTW89_RXD_SHORT_BYTES      24
#define RTW89_RXD_LONG_BYTES       40

#define BE_RXD_RPKT_LEN_MASK       0x00003FFFu   /* 13:0  */
#define BE_RXD_SHIFT_MASK          0x0000C000u   /* 15:14, 2-byte units  */
#define BE_RXD_DRV_INFO_SZ_MASK    0x000C0000u   /* 19:18, 8-byte units  */
#define BE_RXD_HDR_CNV_SZ_MASK     0x00300000u   /* 21:20, 16-byte units */
#define BE_RXD_PHY_RPT_SZ_MASK     0x00C00000u   /* 23:22, 8-byte units  */
#define BE_RXD_RPKT_TYPE_MASK      0x3F000000u   /* 29:24 */
#define BE_RXD_BB_SEL              0x40000000u   /* bit 30 */
#define BE_RXD_LONG_RXD            0x80000000u   /* bit 31 */

#define BE_RXD_MAC_ID_MASK         0x000000FFu   /* 7:0   */
#define BE_RXD_TYPE_MASK           0x00000C00u   /* 11:10 */

#define BE_RXD_SEC_TYPE_MASK       0x0000000Fu   /* 3:0   */
#define BE_RXD_CRC32_ERR           0x00000040u   /* bit 6  */
#define BE_RXD_ICV_ERR             0x00000080u   /* bit 7  */
#define BE_RXD_HW_DEC              0x00000100u   /* bit 8  */
#define BE_RXD_SW_DEC              0x00000200u   /* bit 9  */
#define BE_RXD_A1_MATCH            0x00000400u   /* bit 10 */
#define BE_RXD_AMPDU               0x00000800u   /* bit 11 */

/* How strong the signal was, from the radio report that sits between the
 * descriptor and the frame.  The card reports it in half-decibel steps above
 * a floor, so it is a small positive number until it is converted. */
#define BE_RXD_PHY_RSSI            0x00000FFFu   /* 11:0  */
#define RTW89_PHY_RPT_BYTES        8
#define RSSI_FACTOR                1
#define MAX_RSSI                   110

/* What the card puts in the type field.  Only the first is a frame off the
 * air; the rest are the card talking about itself, and handing one of those
 * up as if it were a packet is how a receive path corrupts a network stack. */
#define RTW89_RX_TYPE_WIFI         0
#define RTW89_RX_TYPE_PPDU_STAT    1
#define RTW89_RX_TYPE_C2H          10

/* What the driver needs from one incoming descriptor. */
typedef struct {
    u32  frame_bytes;      /* the frame itself                        */
    u32  frame_at;         /* where it starts, past all the preamble  */
    u8   type;             /* RTW89_RX_TYPE_*                         */
    bool crc_error;
    bool icv_error;
    bool decrypted;        /* the card did it                         */
    bool addressed_to_us;
    u8   mac_id;

    /* Only meaningful when the card included a radio report; without one
     * there is nothing to convert and this says so rather than reporting a
     * confident zero, which would read as an unusually strong signal. */
    bool signal_known;
    s8   signal_dbm;
} rtw89_rxd_t;

/* Read one.  False when the descriptor does not fit in what arrived, or
 * describes a frame that does not. */
bool rtw89_rxd_parse(const u8 *buffer, u32 arrived, rtw89_rxd_t *out);

/* What a frame needs said about it before the card will send it. */
typedef struct {
    u16  bytes;            /* the frame, not counting the descriptor  */
    u8   qsel;             /* which queue                             */
    u8   channel;          /* which ring - and it must match qsel     */
    u16  sequence;
    u8   mac_id;
    u16  rate;             /* only used when fixed_rate is set        */
    bool fixed_rate;
    bool broadcast;        /* no RTS for these                        */
    u8   retries;          /* 0 leaves the card's own limit alone     */
} rtw89_txd_t;

/* Write the 64-byte descriptor a frame is sent behind.  Returns how many
 * bytes it wrote, or 0 if it refused. */
u32 rtw89_txd_fill(u8 *out, u32 capacity, const rtw89_txd_t *how);

/* ------------------------------------- the signal processor's registers ---
 *
 * Three windows, one card.  The low registers are the MAC's, the baseband's
 * are 0x20000 higher, and the processor that runs the baseband has its own at
 * 0x30000.  Same bus, same BAR, three bases - and an address written without
 * its base lands in the MAC, which answers.
 */
#define RTW89_BBMCU_BASE           0x30000u

#define R_BBCLK                    0x0000
#define B_CLK_640M                 0x00000004u   /* bit 2  */

#define R_TXFCTR                   0x627C
#define B_TXFCTR_THD               0x000FFC00u   /* 19:10 */
#define R_TXSCALE                  0x6284
#define B_TXFCTR_EN                0x00080000u   /* bit 19 */

#define R_SC_CORNER                0x6B70
#define B_SC_CORNER                0x000007FFu   /* 10:0  */

#define R_SLOPE                    0x6B6C
#define B_SLOPE_A                  0x00003FFFu   /* 13:0  */
#define B_SLOPE_B                  0x0FFFC000u   /* 27:14 */
#define B_EHT_RATE_TH              0xF0000000u   /* 31:28 */

#define R_MAG_A                    0x6BF4
#define B_MGA_AEND                 0xFF000000u   /* 31:24 */
#define R_MAG_AB                   0x6BF8
#define B_MAG_AB                   0x00FFFFFFu   /* 23:0  */
#define B_BY_SLOPE                 0xFF000000u   /* 31:24 */

#define R_BEDGE                    0x6BFC
#define B_HE_RATE_TH               0x78000000u   /* 30:27 */
#define B_EHT_MCS14                0x80000000u   /* bit 31 */
#define R_BEDGE2                   0x6C00
#define B_HT_VHT_TH                0x00000FFFu   /* 11:0  */
#define B_EHT_MCS15                0x80000000u   /* bit 31 */
#define R_BEDGE3                   0x6C04
#define B_EHTTB_EN                 0x00008000u   /* bit 15 */
#define B_HEERSU_EN                0x00080000u   /* bit 19 */
#define B_HEMU_EN                  0x00200000u   /* bit 21 */
#define B_TB_EN                    0x00800000u   /* bit 23 */
#define R_SU_PUNC                  0x6C08
#define B_SU_PUNC_EN               0x00000002u   /* bit 1  */
#define R_BEDGE5                   0x6C10
#define B_PWROFST_COMP             0x00100000u   /* bit 20 */
#define B_HWGEN_EN                 0x02000000u   /* bit 25 */

#define R_UDP_COEEF                0x0CBC
#define B_UDP_COEEF                0x00080000u   /* bit 19 */

/* Finish bringing the baseband up: hand its processor the handful of registers
 * it boots against, then set the receiver's thresholds.
 *
 * Called after rtw89_baseband_reset, which is the half that holds it down. */
bool rtw89_baseband_configure(volatile u8 *regs, int phy);

/* ------------------------------------------------ tuning the radio -------
 *
 * Which channel the card listens on lives in one radio register, held twice:
 * once per path, and each path holds it at two addresses that have to agree.
 * Four registers for one number, and all four are read before any is written.
 *
 * The band and the channel number are separate fields and both must be set:
 * channel 36 with the band left at 2 GHz is not "nearly right", it is a
 * different frequency entirely.
 */
#define RR_CFGCH                   0x18
#define RR_CFGCH_V1                0x10018

#define RR_CFGCH_CH                0x000000FFu   /* 7:0   the channel number */
#define RR_CFGCH_BAND0             0x00000300u   /* 9:8                      */
#define RR_CFGCH_BW_V2             0x00001C00u   /* 12:10 how wide           */
#define RR_CFGCH_BAND1             0x00030000u   /* 17:16                    */

/* The band is written into two fields, and they do not hold the same number -
 * 6 GHz is 3 in one and 0 in the other, which is also 2.4 GHz's value there.
 * Neither field identifies the band on its own. */
#define CFGCH_BAND1_2G             0
#define CFGCH_BAND1_5G             1
#define CFGCH_BAND1_6G             3
#define CFGCH_BAND0_2G             0
#define CFGCH_BAND0_5G             1
#define CFGCH_BAND0_6G             0

#define CFGCH_BW_V2_20M            0
#define CFGCH_BW_V2_40M            1
#define CFGCH_BW_V2_80M            2
#define CFGCH_BW_V2_160M           3

/* The lookup table this chip's first revision needs written after a retune. */
#define RR_LUTWA                   0x33
#define RR_LUTWD1                  0x3e
#define RR_LUTWD0                  0x3f
#define RR_LUTWE                   0xef

/* Which band a channel number belongs to.
 *
 * 2.4 GHz and 5 GHz never share a number, so this is exact for them.  6 GHz
 * does share - its channel 36 is not 5 GHz's channel 36 - so a caller that
 * means 6 GHz has to say so rather than pass a number and hope. */
typedef enum {
    RTW89_BAND_2G = 0,
    RTW89_BAND_5G = 1,
    RTW89_BAND_6G = 2,
} rtw89_band_t;

rtw89_band_t rtw89_band_of_channel(u8 channel);

/* Tune both paths to a channel.  20 MHz wide, which is what a card associates
 * at before anything negotiates wider. */
bool rtw89_set_channel(volatile u8 *regs, u8 channel, rtw89_band_t band,
                       bool first_revision);

/* Read one of the radio's registers.  Returns RTW89_RF_INVALID when the
 * interface never answered, which is a different thing from a register that
 * happens to read as all ones. */
u32 rtw89_rf_read(volatile u8 *regs, int path, u32 addr);

/* Write one.  Returns false when the interface would not go idle to take it. */
bool rtw89_rf_write(volatile u8 *regs, int path, u32 addr, u32 data);

/* Wait for the card's processor to say the firmware it was given is running.
 *
 * This is the step between "the bytes were sent" and "the card is usable", and
 * the two are not the same thing: the processor verifies what it was given
 * before it runs it, and a checksum or a signature that does not match leaves
 * it stopped with the reason in a register.  Waiting for a timeout and then
 * carrying on regardless is how a driver ends up talking to a card that never
 * started, where every later failure is a mystery with no connection to the
 * download that actually failed.
 *
 * Returns false and names the reason.  `ms` bounds the wait. */
bool rtw89_fw_wait_ready(volatile u8 *regs, int ms, const char *who);
bool rtw89_fw_wait_bb0_ready(volatile u8 *regs, int ms, const char *who);
const char *rtw89_fwdl_status_name(u8 status);

/* ------------------------------------------------- the firmware container
 *
 * A newer Realtek firmware file is a twelve-word header, one four-word
 * descriptor per section, and then the sections' bytes laid end to end.  The
 * fields are packed into those words rather than given a byte each, so every
 * one of them is a shift and a mask - and a mask one bit wide in the wrong
 * place gives a section length that is plausible and wrong, which is a
 * download that silently corrupts the card's memory.
 *
 * Field positions are from fw.h in Realtek's own tree.
 */
#define RTW89_FW_HDR_V1_WORDS      12
#define RTW89_FW_HDR_V1_BYTES      (RTW89_FW_HDR_V1_WORDS * 4)
#define RTW89_FW_SECTION_V1_BYTES  16
#define RTW89_FW_MAX_SECTIONS      16
#define FWDL_SECTION_CHKSUM_LEN    8
#define FWDL_SECURITY_SECTION_TYPE 9
#define FWDL_SECURITY_SIGLEN       512
#define FWDL_SECURITY_CHKSUM_LEN   8
#define FW_HDR_V1_W6_DSP_CHKSUM    (1u << 24)

/* linux-firmware packages several images in one MFW file.  These are file
 * records, not download-section descriptors. */
#define RTW89_MFW_SIG              0xFF
#define RTW89_MFW_HEADER_BYTES     16
#define RTW89_MFW_ENTRY_BYTES      16
#define RTW89_FW_NORMAL            1
#define RTW89_FW_WOWLAN            3
#define RTW89_FW_LOGFMT            0xFF

/* w1 */
#define FW_HDR_V1_W1_MAJOR_SHIFT   0    /* 7:0   */
#define FW_HDR_V1_W1_MINOR_SHIFT   8    /* 15:8  */
#define FW_HDR_V1_W1_SUBVER_SHIFT  16   /* 23:16 */
#define FW_HDR_V1_W1_SUBIDX_SHIFT  24   /* 31:24 */
/* w3 */
#define FW_HDR_V1_W3_HDR_VER_SHIFT 24   /* 31:24 */
/* w4 */
#define FW_HDR_V1_W4_MONTH_SHIFT   0    /* 7:0   */
#define FW_HDR_V1_W4_DATE_SHIFT    8    /* 15:8  */
/* w5 */
#define FW_HDR_V1_W5_YEAR_MASK     0xFFFFu       /* 15:0  */
#define FW_HDR_V1_W5_HDR_SIZE_SHIFT 16           /* 31:16 */
/* w6 */
#define FW_HDR_V1_W6_SEC_NUM_SHIFT 8    /* 15:8  */
/* w7 */
#define FW_HDR_V1_W7_DYN_HDR       (1u << 16)

/* section w1 */
#define FWSEC_V1_W1_SIZE_MASK      0x00FFFFFFu   /* 23:0  */
#define FWSEC_V1_W1_TYPE_SHIFT     24            /* 27:24 */
#define FWSEC_V1_W1_TYPE_MASK      0xFu
#define FWSEC_V1_W1_CHECKSUM       (1u << 28)
#define FWSEC_V1_W1_REDL           (1u << 29)

typedef struct {
    u32 download_address;
    u32 length;                  /* the checksum, when present, included */
    u32 mssc_length;             /* signature pool after a security section */
    u32 key_offset, key_length;  /* validated signature in the original file */
    u8  type;
    u8  mssc;
    bool redownload;
} rtw89_fw_section_t;

typedef struct {
    u8  major, minor, subversion, subindex;
    u16 year;
    u8  month, date;
    u8  header_version;
    u32 header_length;           /* where the sections' bytes begin        */
    u32 dynamic_header_length;   /* host-only tail not sent to the card    */
    u32 part_size;               /* maximum bytes in one section request   */
    int section_count;
    rtw89_fw_section_t sections[RTW89_FW_MAX_SECTIONS];
    u32 payload_bytes;           /* bytes actually downloaded              */
    u32 file_bytes;              /* payload plus security signature pools  */
    bool needs_security_profile; /* MSSC key choice requires hardware efuse */
    bool security_validated;
} rtw89_fw_info_t;

typedef struct {
    bool valid, secure_boot;
    u8 device_type, customer, key;
    u32 selector;
} rtw89_fw_security_t;

bool rtw89_fw_security_decode(const u8 bytes[4], rtw89_fw_security_t *out);
bool rtw89_efuse_read(volatile u8 *regs, u8 cv, u32 address, u8 *bytes, u32 length);
bool rtw89_efuse_logical(const u8 *physical, size_t length, u8 page,
                        u32 offset, u8 *logical, u32 count);
bool rtw89_read_pci_mac(volatile u8 *regs, u8 mac[6]);
bool rtw89_fw_security_read(volatile u8 *regs, u8 cv,
                           rtw89_fw_security_t *out);
bool rtw89_fw_apply_security(const u8 *fw, size_t size,
                            const rtw89_fw_security_t *profile,
                            rtw89_fw_info_t *info);

typedef struct {
    const u8 *data;
    size_t size;
    u8 cv;
    u8 type;
    bool from_container;
} rtw89_fw_image_t;

/* Read the container.  Returns false and says which check failed; nothing is
 * written to the card on the strength of a header that did not add up. */
bool rtw89_parse_firmware(const u8 *data, size_t size, rtw89_fw_info_t *out);
bool rtw89_select_firmware(const u8 *data, size_t size, u8 hardware_cv,
                           u8 type, rtw89_fw_image_t *out);
bool rtw89_chip_cv(volatile u8 *regs, u8 *cv);

/* ------------------------------------------------ talking to the firmware
 *
 * Everything the host asks the card's processor to do goes in a packet with an
 * eight-byte header on the front: two words saying what kind of request this
 * is, which part of the firmware handles it, and how long the whole thing is.
 *
 * The bit positions below were read out of Linux's rtw89 driver rather than
 * remembered, and that was not a formality.  Written from memory, the category
 * and class fields would have come out at bits 4:3 and 7:5 - which is where an
 * earlier generation of this chip family put them.  They are 1:0 and 7:2 here.
 * A header with those two fields in the wrong place is still eight bytes long
 * and still gets sent; the card simply files the request under something else.
 */
#define H2C_HEADER_LEN             8

/* word 0 */
#define H2C_HDR_CAT_SHIFT          0            /* 1:0   */
#define H2C_HDR_CAT_MASK           0x3u
#define H2C_HDR_CLASS_SHIFT        2            /* 7:2   */
#define H2C_HDR_CLASS_MASK         0x3Fu
#define H2C_HDR_FUNC_SHIFT         8            /* 15:8  */
#define H2C_HDR_FUNC_MASK          0xFFu
#define H2C_HDR_DEL_TYPE_SHIFT     16           /* 19:16 */
#define H2C_HDR_DEL_TYPE_MASK      0xFu
#define H2C_HDR_SEQ_SHIFT          24           /* 31:24 */
#define H2C_HDR_SEQ_MASK           0xFFu

/* word 1 */
#define H2C_HDR_TOTAL_LEN_MASK     0x3FFFu      /* 13:0  */
#define H2C_HDR_REC_ACK            (1u << 14)
#define H2C_HDR_DONE_ACK           (1u << 15)

/* Who handles a firmware download, and which part of it. */
#define H2C_CAT_MAC                0x1
#define H2C_CL_MAC_FWDL            0x3
#define H2C_FUNC_MAC_FWHDR_DL      0x0

/* Build the eight bytes that go in front of a request.  `payload_len` is the
 * length of what follows, not of the whole packet - the header adds its own
 * length in, which is the sort of off-by-eight that produces a packet the card
 * reads the wrong amount of. */
void rtw89_h2c_header(u8 out[H2C_HEADER_LEN], u8 cat, u8 cls, u8 func,
                      u8 del_type, u8 seq, u32 payload_len,
                      bool rec_ack, bool done_ack);

/* ------------------------------------------------- pushing the firmware in
 *
 * The card's processor has no firmware of its own worth the name; what makes
 * it a Wi-Fi card is downloaded into it every time the machine starts.  That
 * happens in two stages and the order is not negotiable: the container's own
 * header goes first, so the processor knows how much is coming and where each
 * piece belongs, and then the sections' bytes follow.
 *
 * A section is longer than one request can carry, so it is cut into pieces.
 * The size below is the one Realtek's driver uses; it is not a round number
 * and it is not arbitrary - a larger piece does not fit the request length
 * field's fourteen bits once the header is added.
 */
#define FWDL_SECTION_PER_PKT_LEN   2020
#define FWDL_SECTION_MAX_NUM       10
#define FWDL_SECTION_CHKSUM_LEN    8

/* The firmware header is an H2C command; following section chunks are raw
 * FWDL packets on the same PCI channel, selected by the BE short descriptor.
 * Returning false stops the download before an incomplete image can run. */
typedef bool (*rtw89_h2c_send_fn)(void *ctx, const u8 *packet, u32 len,
                                 bool fwdl);

/* Build and hand over every request a download consists of, in order.
 * `fw` is the whole container and `info` what reading it produced. */
bool rtw89_fw_download(const u8 *fw, size_t size, const rtw89_fw_info_t *info,
                       rtw89_h2c_send_fn send, void *ctx);

/* ------------------------------------------- setting the packet engine up
 *
 * After the firmware runs, the parts of the chip that move packets have to be
 * told how to behave.  Three of them are short enough to be here; the rest -
 * the memory quotas, the flow control, the preload - are tables and are not.
 *
 * The scheduler decides which station is served next, and has an initialisation
 * that reports when it has finished.  The security engine encrypts and
 * decrypts in hardware, and each kind of frame is switched on separately - a
 * driver that enables the engine and forgets broadcast decryption receives
 * everything except the frames a network sends to everyone.  The MPDU
 * processor is what appends and checks the parts of a frame the hardware owns.
 *
 * Every constant below was diffed against the source with tools/verify_rtw89.py
 * rather than read across.
 */
#define R_BE_SS_CTRL                   0xA310
#define B_BE_SS_EN                     (1u << 0)
#define B_BE_BAND1_TRIG_EN             (1u << 9)
#define B_BE_BAND_TRIG_EN              (1u << 28)
#define B_BE_WARM_INIT                 (1u << 29)
#define B_BE_SS_INIT_DONE              (1u << 31)
#define TRXCFG_WAIT_CNT                2000u

#define R_BE_SEC_ENG_CTRL              0x9D00
#define B_BE_SEC_TX_ENC                (1u << 0)
#define B_BE_SEC_RX_DEC                (1u << 1)
#define B_BE_BC_DEC                    (1u << 2)
#define B_BE_MC_DEC                    (1u << 3)
#define B_BE_UC_MGNT_DEC               (1u << 4)
#define B_BE_BMC_MGNT_DEC              (1u << 5)
#define B_BE_CLK_EN_WEP_TKIP           (1u << 8)
#define B_BE_CLK_EN_WAPI               (1u << 9)
#define B_BE_CLK_EN_CGCMP              (1u << 10)
#define B_BE_SEC_PRE_ENQUE_TX          (1u << 11)

#define R_BE_SEC_MPDU_PROC             0x9D04
#define B_BE_APPEND_MIC                (1u << 0)
#define B_BE_APPEND_ICV                (1u << 1)

#define R_BE_MPDU_PROC                 0x9C00
#define B_BE_APPEND_FCS                (1u << 0)

/* Linux v6.17 reg.h, mpdu_proc_init_be() in mac_be.c. */
#define R_BE_CUT_AMSDU_CTRL            0x9C94
#define TRXCFG_MPDU_PROC_CUT_CTRL      0x010E05F0u
#define R_BE_HDR_SHCUT_SETTING         0x9B00
#define B_BE_TX_HW_SEQ_EN              (1u << 0)
#define B_BE_TX_HW_ACK_POLICY_EN       (1u << 1)
#define B_BE_TX_MAC_MPDU_PROC_EN        (1u << 2)
#define B_BE_TX_ADDR_MLD_TO_LIK         (1u << 4)
#define R_BE_RX_HDRTRNS                0x9CC0
#define TRXCFG_MPDU_PROC_RX_HDR_CONV    0u
#define R_BE_DISP_FWD_WLAN_0           0x8938
#define B_BE_FWD_WLAN_CPU_TYPE_0_DATA_MASK 0x03u
#define B_BE_FWD_WLAN_CPU_TYPE_0_MNG_MASK  0x0Cu
#define B_BE_FWD_WLAN_CPU_TYPE_0_CTL_MASK  0x30u
#define B_BE_FWD_WLAN_CPU_TYPE_1_MASK      0xC0u

/* -------------------------------------------- how the card's memory is cut
 *
 * A Wi-Fi chip holds packets in memory of its own while they are queued, being
 * encrypted, or waiting for the air to be free.  That memory is divided before
 * anything can use it, and the division is a set of numbers rather than a
 * calculation: how large a page is, how many pages there are, and how much is
 * set aside for things that are not packets.
 *
 * The numbers below are for an RTL8922A on PCIe carrying one link, which is
 * what a machine joining a network is.
 *
 * They are checked rather than trusted.  Every one was read across from
 * somewhere else, and the failure they produce is not a compile error - it is
 * a card that accepts the division, runs, and drops packets under load because
 * the pages it was told about are not the pages it has.  Adding them up and
 * comparing with the memory the chip says it has catches that here.
 */
#define RTW89_8922A_FIFO_BYTES         589824u

/* The queue engine: how packets are tracked. */
#define RTW89_WDE_PAGE_BYTES           64u
#define RTW89_WDE_LINKED_PAGES         3328u
#define RTW89_WDE_UNLINKED_PAGES       0u

/* The payload engine: where the packets themselves sit. */
#define RTW89_PLE_PAGE_BYTES           128u
#define RTW89_PLE_LINKED_PAGES         2688u
#define RTW89_PLE_UNLINKED_PAGES       240u

/* And what is set aside for neither. */
#define RTW89_DLE_RESERVED_BYTES       2048u

/* Do the numbers describe the memory the chip actually has?  Says what it
 * found when they do not. */
bool rtw89_memory_split_adds_up(void);

/* The registers the division is written into.
 *
 * Two engines, configured the same way: which page size, where their region
 * starts, and how many pages they have.  The start is written in units of
 * eight kilobytes rather than bytes, which is the sort of thing that produces
 * a region eight thousand times too far along if it is missed.
 */
#define R_BE_DMAC_FUNC_EN              0x8400
#define B_BE_DLE_PLE_EN                (1u << 23)
#define B_BE_DLE_WDE_EN                (1u << 26)

#define R_BE_DMAC_CLK_EN               0x8404
#define B_BE_DLE_PLE_CLK_EN            (1u << 23)
#define B_BE_DLE_WDE_CLK_EN            (1u << 26)

#define R_BE_WDE_PKTBUF_CFG            0x8C08
#define B_BE_WDE_PAGE_SEL_MASK         0x3u          /* 1:0   */
#define B_BE_WDE_START_BOUND_MASK      0x7F00u       /* 14:8  */
#define B_BE_WDE_FREE_PAGE_NUM_MASK    0x1FFF0000u   /* 28:16 */

#define R_BE_PLE_PKTBUF_CFG            0x9008
#define R_BE_WDE_QTA0_CFG              0x8C40
#define R_BE_PLE_QTA0_CFG              0x9040
/* BE uses the shared AX-named manager status registers (Linux mac_be.c). */
#define R_AX_WDE_INI_STATUS            0x8D00
#define R_AX_PLE_INI_STATUS            0x9100
#define RTW89_DLE_INIT_READY           3u
#define B_BE_PLE_PAGE_SEL_MASK         0x3u          /* 1:0   */
#define B_BE_PLE_START_BOUND_MASK      0x7F00u       /* 14:8  */
#define B_BE_PLE_FREE_PAGE_NUM_MASK    0x1FFF0000u   /* 28:16 */

/* Which page size, as the register spells it.  The queue engine cannot do 256
 * bytes and the payload engine cannot do 64; each refuses rather than choosing
 * something else. */
#define WDE_PAGE_SEL_64                0u
#define WDE_PAGE_SEL_128               1u
#define PLE_PAGE_SEL_128               1u

#define DLE_BOUND_UNIT                 (8u * 1024u)

/* Where the payload region starts, which is after the queue region. */
#define RTW89_PLE_START_OFFSET         212992u

/* Divide the card's memory the way the numbers above say. */
bool rtw89_dle_init(volatile u8 *regs);

/* ------------------------------------------------ keeping the host in step
 *
 * The card's memory is finite and the host can hand it packets faster than the
 * air carries them away.  Flow control is how the card says stop: each channel
 * is given a reserve of pages it may always use, the rest are shared, and a
 * channel is called full when its reserve is gone.
 *
 * Getting this wrong does not fail, it stalls.  A reserve larger than the
 * memory means a channel is never full and the host keeps handing over packets
 * that have nowhere to go; a reserve of nothing means it is always full and
 * nothing is ever sent.  Neither says anything about itself.
 *
 * The numbers are for an RTL8922A on PCIe: two pages reserved for the ordinary
 * channels, thirty-two for the one commands travel on, and 3302 shared.
 */
#define R_BE_HCI_FC_CTRL               0xB700
#define B_BE_HCI_FC_MODE_MASK          0x6u          /* 2:1   */
#define B_BE_HCI_FC_WD_FULL_COND_MASK  0x30u         /* 5:4   */
#define B_BE_HCI_FC_CH12_FULL_COND_MASK 0xC00u       /* 11:10 */
#define B_BE_HCI_FC_EN                (1u << 0)
#define B_BE_HCI_FC_CH12_EN            (1u << 3)
#define B_BE_HCI_FC_WP_CH07_FULL_COND_MASK 0xC0u
#define B_BE_HCI_FC_WP_CH811_FULL_COND_MASK 0x300u
#define R_BE_CH0_PAGE_CTRL            0xB718
#define R_BE_CH0_PAGE_INFO            0xB750
#define R_BE_PUB_PAGE_CTRL1           0xB790
#define R_BE_PUB_PAGE_INFO1           0xB79C
#define R_BE_PUB_PAGE_INFO2           0xB7A0
#define R_BE_PUB_PAGE_INFO3           0xB78C
#define R_BE_WP_PAGE_CTRL2            0xB7A8
#define R_BE_WP_PAGE_INFO1            0xB7AC

#define R_BE_CH_PAGE_CTRL              0xB704
#define B_BE_PREC_PAGE_CH011_V1_MASK   0x3Fu         /* 5:0   */
#define B_BE_PREC_PAGE_CH12_V1_MASK    0x3F0000u     /* 21:16 */

#define R_BE_PUB_PAGE_CTRL2            0xB794
#define B_BE_PUBPG_ALL_MASK            0x1FFFu       /* 12:0  */

#define R_BE_WP_PAGE_CTRL1             0xB7A4
#define B_BE_PREC_PAGE_WP_CH07_MASK    0x1FFu        /* 8:0   */
#define B_BE_PREC_PAGE_WP_CH811_MASK   0x1FF0000u    /* 24:16 */

/* How much each kind of channel keeps for itself, and what is shared. */
#define RTW89_HFC_CH011_RESERVE        2u
#define RTW89_HFC_H2C_RESERVE          32u
#define RTW89_HFC_PUBLIC_PAGES         3302u
/* Zero, not one.  This was written as one from memory and the source says
 * RTW89_HCIFC_POH = 0 - caught by eye rather than by the checker, because it
 * is an enum and the checker only read #defines until now. */
#define RTW89_HCIFC_POH                0u    /* packets held by the host */

/* Who the tracking pages belong to.
 *
 * The pages that track packets are split between the host, the card's
 * processor, the path packets arrive on, and the processor's own traffic.
 * These four are a real division - they add up to the pages that exist - which
 * makes them checkable, and distinguishes them from the flow-control numbers
 * above, which are thresholds INSIDE the host's share rather than shares of
 * their own.
 *
 * Confusing the two is easy and produces a check that fires on correct data.
 * It did: adding the reserves to the shared pool came to more pages than the
 * card has, and the arithmetic was wrong rather than the numbers.
 */
#define RTW89_WDE_QT_HOST              3302u
#define RTW89_WDE_QT_CARD_CPU          6u
#define RTW89_WDE_QT_PACKET_IN         0u
#define RTW89_WDE_QT_CPU_IO            20u

/* Tell the card how to hold the host back. */
bool rtw89_flow_control_init(volatile u8 *regs);

/* ---------------------------------------------- fetching a packet early
 *
 * A radio cannot wait for memory.  When its turn to transmit arrives it has
 * microseconds, so the beginning of a packet is fetched into the transmit path
 * BEFORE the turn comes - preloaded - and the rest follows while the first part
 * is already going out.
 *
 * How much is fetched is a compromise with nothing to do with correctness:
 * too little and the radio catches up with the fetch and the frame is cut
 * short; too much and memory is spent holding packets that may never be sent.
 * The numbers are the chip's own.
 */
#define R_BE_TXPKTCTL_B0_PRELD_CFG0    0x9F48
#define B_BE_B0_PRELD_CAM_G0ENTNUM_MASK 0x1Fu        /* 4:0   */
#define B_BE_B0_PRELD_CAM_G1ENTNUM_MASK 0x1F00u      /* 12:8  */
#define B_BE_B0_PRELD_USEMAXSZ_MASK    0x03FF0000u   /* 25:16 */
#define B_BE_B0_PRELD_FEN              (1u << 31)

#define R_BE_TXPKTCTL_B0_PRELD_CFG1    0x9F4C
#define B_BE_B0_PRELD_NXT_RSVMINSZ_MASK 0xFFu        /* 7:0   */
#define B_BE_B0_PRELD_NXT_TXENDWIN_MASK 0xF00u       /* 11:8  */

#define PRELD_B0_ENT_NUM               10u
#define PRELD_AMSDU_SIZE               52u
#define PRELD_NEXT_WND                 1u
#define PRELD_B0_ACQ_ENT_NUM_8922A     8u
#define PRELD_MISCQ_ENT_NUM_8922A      2u

/* Set the transmit path to fetch the start of a packet before its turn. */
bool rtw89_preload_init(volatile u8 *regs);

/* ------------------------------------------- the regions kept back for use
 *
 * Not all of the packet memory holds packets.  A stretch past the free pages
 * is kept for things the hardware needs somewhere to put: a table describing
 * each packet it is sending, the channel measurements a receiver reports back,
 * and the timing exchanges used to measure distance.
 *
 * They sit one after another starting where the free pages end, so each one's
 * place is the sum of everything before it.  Getting that sum wrong does not
 * overlap harmlessly - two regions sharing pages means one overwrites the
 * other, and what is lost is whichever the hardware wrote second.
 */
#define RTW89_RSVD_MPDU_INFO_PAGES     2u
#define RTW89_RSVD_B0_CSI_PAGES        107u
#define RTW89_RSVD_B1_CSI_PAGES        107u
#define RTW89_RSVD_B0_LMR_PAGES        6u
#define RTW89_RSVD_B1_LMR_PAGES        6u
#define RTW89_RSVD_B0_FTM_PAGES        6u
#define RTW89_RSVD_B1_FTM_PAGES        6u

/* Where the first of them starts: immediately after the pages that hold
 * packets. */
#define RTW89_RSVD_FIRST_PAGE          RTW89_PLE_LINKED_PAGES

#define R_BE_TXPKTCTL_MPDUINFO_CFG     0x9F10
#define B_BE_MPDUINFO_B1_BADDR_MASK    0x3Fu         /* 5:0   */
#define B_BE_MPDUINFO_PKTID_MASK       0x0FFF0000u   /* 27:16 */
#define B_BE_MPDUINFO_FEN              (1u << 31)
#define MPDU_INFO_B1_OFST              18u

/* Tell the transmit path where its packet-description table lives. */
bool rtw89_txpktctl_init(volatile u8 *regs);

/* ------------------------------------------------------ the transmit side
 *
 * Two settings that are about timing rather than data.
 *
 * The first stops the card treating a keep-alive frame as traffic for the
 * purpose of the fairness rules - it is not carrying anything, and counting it
 * makes the card believe it has used a turn it did not.
 *
 * The second is how long the radio waits between the training symbols at the
 * start of a frame and the data after them, in microseconds.  These are the
 * chip's own numbers and there is nothing to derive them from; too short and
 * the receiver is still settling when the data begins.
 */
#define R_BE_TB_PPDU_CTRL              0x1080C
#define B_BE_QOSNULL_UPD_MUEDCA_EN     (1u << 3)

#define R_BE_WMTX_TCR_BE_4             0x10E2C
#define B_BE_EHT_HE_PPDU_2XLTF_ZLD_USTIMER_MASK 0x001F0000u   /* 20:16 */
#define B_BE_EHT_HE_PPDU_4XLTF_ZLD_USTIMER_MASK 0x1F000000u   /* 28:24 */

#define RTW89_ZLD_USTIMER_4XLTF        0x12u
#define RTW89_ZLD_USTIMER_2XLTF        0x0Eu

/* Set the transmit side's timing. */
bool rtw89_tmac_init(volatile u8 *regs, int mac);

/* ------------------------------------------------------- the receive side
 *
 * How long the receiver waits before deciding a frame it started hearing is
 * not going to finish, and the largest frame it will accept.
 *
 * The timeouts are what stop a burst of noise holding the receiver open: it
 * hears the start of something, waits, and gives up.  Too long and every burst
 * of interference costs the time it would have taken to receive a real frame.
 *
 * The maximum length is worked out rather than written down, because it is the
 * smallest of three things: the pages this radio has for receiving, what the
 * standard permits, and the field it has to fit in.
 */
#define R_BE_RCR                       0x11400
#define B_BE_BUSY_CHKSN                (1u << 15)

#define R_BE_DLK_PROTECT_CTL           0x11402
#define B_BE_RX_DLK_DATA_TIME_MASK     0xF0u         /* 7:4   */
#define B_BE_RX_DLK_CCA_TIME_MASK      0xFF00u       /* 15:8  */
#define TRXCFG_RMAC_DATA_TO            15u
#define TRXCFG_RMAC_CCA_TO             32u

#define R_BE_PLCP_HDR_FLTR             0x11404
#define B_BE_VHT_SU_SIGB_CRC_CHK       (1u << 4)

#define R_BE_RX_FLTR_OPT               0x11420
#define B_BE_RX_MPDU_MAX_LEN_MASK      0x3F0000u     /* 21:16 */

#define PLD_RLS_MAX_PG                 127u
#define RX_MAX_LEN_UNIT                512u
#define RX_SPEC_MAX_LEN                (11454u + RX_MAX_LEN_UNIT)

/* Set the receive side's timeouts and its largest acceptable frame. */
bool rtw89_rmac_init(volatile u8 *regs, int mac);

/* ------------------------------------------------- bringing the radio's
 *                                                    signal processor up
 *
 * Between the packet engine and the aerial sits the baseband: the part that
 * turns bytes into a waveform.  It has its own processor and its own reset,
 * and it comes out of reset in a particular order - the whole block held down,
 * then the platform released, then the block, and only then is its processor
 * told it may boot.
 *
 * The order is not a formality.  Releasing the processor before the block it
 * runs in is released means it starts executing against registers that are
 * still held at zero, and what it does then is not defined by anything.
 */
#define R_BE_FEN_RST_ENABLE            0x0084
#define B_BE_FEN_BBPLAT_RSTB           (1u << 0)
#define B_BE_FEN_BB_IP_RSTN            (1u << 1)
#define B_BE_BOOT_RDY0                 (1u << 2)
#define B_BE_FEN_BB1PLAT_RSTB          (1u << 8)
#define B_BE_FEN_BB1_IP_RSTN           (1u << 9)
#define B_BE_BOOT_RDY1                 (1u << 10)

#define R_BE_MEM_PWR_CTRL              0x00D0
#define B_BE_MEM_BBMCU0_DS_V1          (1u << 17)

#define R_BE_DMAC_SYS_CR32B            0x842C
#define B_BE_DMAC_BB_PHY0_MASK         0x0000FFFFu   /* 15:0  */
#define B_BE_DMAC_BB_PHY1_MASK         0xFFFF0000u   /* 31:16 */
#define RTW89_DMAC_BB_SETTING          0x7FF9u

/* Bring one radio's signal processor out of reset. */
bool rtw89_baseband_reset(volatile u8 *regs, int phy);

/* Multi-link operation - the thing Wi-Fi 7 is for.
 *
 * A Wi-Fi 7 radio can hold links on more than one band at once and move
 * traffic between them, and the table that tracks which link a station is on
 * has to be built before any of that.  Building it is a pulse: a bit set and
 * cleared, and then the chip says when the table is there.
 *
 * The pulse is why the bit is cleared immediately rather than left set.  Left
 * set, the table is held in reset and the wait below never ends - which reads
 * as a chip that does not support multi-link rather than a driver that asked
 * for the table and never let go. */
#define R_BE_MLO_INIT_CTL              0xA114
#define B_BE_MLO_HW_CHGLINK_EN         (1u << 10)
#define B_BE_MLO_TABLE_REINIT          (1u << 23)
#define B_BE_MLO_TABLE_INIT_DONE       (1u << 31)

#define R_BE_CMAC_SHARE_ACQCHK_CFG_0   0x0E010
#define B_BE_R_MACID_ACQ_CHK_EN        (1u << 0)

/* Build the link table and switch multi-link on. */
bool rtw89_mlo_init(volatile u8 *regs);

/* Start the scheduler and wait for it to say it is ready. */
bool rtw89_sched_init(volatile u8 *regs);
/* Switch on encryption and decryption, for every kind of frame. */
bool rtw89_security_init(volatile u8 *regs);
/* And the part of a frame the hardware appends. */
bool rtw89_mpdu_init(volatile u8 *regs);

/* ------------------------------------------------ what the card passes up
 *
 * A radio hears every frame in the air, and most of them are not for this
 * machine.  Three filters decide what reaches the host, one per kind of frame:
 * management (beacons, and the exchange that joins a network), control (the
 * acknowledgements and requests that hardware handles for itself), and data.
 *
 * Both ways of getting this wrong are quiet.  Everything dropped is a radio
 * that hears nothing and looks broken; everything accepted is every frame from
 * every network nearby arriving to be sorted out in software, which works and
 * is slow in a way nothing points at.
 *
 * The chip has two of these blocks, one per radio.  The second sits 0x4000
 * further on - which is not stated anywhere as a stride, but all three of the
 * registers below are published for both and all three differ by exactly that.
 */
#define R_BE_CTRL_FLTR             0x11424
#define R_BE_MGNT_FLTR             0x11428
#define R_BE_DATA_FLTR             0x1142C
#define RTW89_MAC_STRIDE           0x4000
#define RTW89_MACS                 2

#define RX_FLTR_ACCEPT             0xFFFFu
#define RX_FLTR_DROP               0x0000u

typedef enum {
    RTW89_FRAME_MGMT = 0,
    RTW89_FRAME_CTRL,
    RTW89_FRAME_DATA,
} rtw89_frame_kind_t;

/* Say whether one kind of frame reaches the host on one of the radios. */
bool rtw89_rx_filter_set(volatile u8 *regs, int mac, rtw89_frame_kind_t kind,
                         bool accept);

/* What a machine joining a network needs: the frames that carry the joining
 * and the frames that carry the traffic, and not the ones the hardware answers
 * by itself. */
bool rtw89_rx_filter_station(volatile u8 *regs, int mac);

/* -------------------------------------------------- what comes back up
 *
 * The receiving ring runs the other way round.  The host puts EMPTY buffers in
 * it and the card fills them, so a descriptor here describes somewhere to put
 * a packet rather than a packet to send.
 *
 * In front of each packet the card writes one word about it, and three things
 * in that word matter:
 *
 *   How much it wrote, which is not how big the buffer was.
 *
 *   Whether this is the start of a packet and whether it is the end.  A packet
 *   larger than one buffer arrives in pieces, and a driver that assumes every
 *   buffer holds a whole packet delivers the first fragment of each and throws
 *   the rest away - which looks like a network that only carries small frames.
 *
 *   A tag, which counts up.  The card puts the next one on each packet, so a
 *   tag that skips is a packet that was lost between the card and here - and
 *   without checking it, that loss is invisible and shows up much later as
 *   traffic that does not add up.
 */
#define RTW89_PCI_RXBD_BYTES       8
#define RTW89_RX_TAG_MAX           0x1FFF

typedef struct {
    u32  bytes;       /* what the card actually wrote          */
    u16  tag;         /* its place in the card's own sequence  */
    bool first;       /* the start of a packet                 */
    bool last;        /* and the end of one                    */
} rtw89_rx_info_t;

/* Describe a buffer for the card to fill.  `size` is how much room there is,
 * not how much will arrive. */
bool rtw89_rx_bd_write(u8 *slot, u64 phys, u32 size);

/* Take apart the word the card writes in front of a packet. */
bool rtw89_rx_parse(const u8 *buffer, u32 arrived, rtw89_rx_info_t *out);

/* Is this the tag that should have come next?  Advances `expected` when it is,
 * and says how many were missed when it is not. */
bool rtw89_rx_tag_ok(u16 *expected, u16 got, int *missed);

/* ---------------------------------------------------- the ring to the card
 *
 * A request does not reach the card by being written to a register.  It is
 * left in memory the card can read, and a descriptor pointing at it is put
 * into a ring both sides walk round: the host adds at one end, the card takes
 * from the other, and each tells the other where it has got to by an index.
 *
 * The descriptor is eight bytes and its layout is fixed by the hardware:
 *
 *     length   16 bits   how many bytes the request is
 *     option   16 bits   flags, of which one says this is the last piece
 *     address  32 bits   where those bytes are, as the card addresses them
 *
 * Thirty-two bits of address, which is the constraint that matters: a request
 * placed above four gigabytes cannot be pointed at, and nothing in the
 * hardware complains - the top bits are simply gone and the card fetches from
 * whatever is at the truncated address.
 */
#define RTW89_PCI_BD_BYTES         8
#define RTW89_PCI_TXBD_OPTION_LS   (1u << 14)   /* the last piece of a request */

typedef struct {
    u8  *bd;             /* the descriptors, in memory the card can read */
    u64  bd_phys;
    u16  slots;
    u16  host_index;     /* where the host will add next                 */
    u16  card_index;     /* where the card has taken up to               */
} rtw89_ring_t;

/* Write one descriptor into a ring slot.  Returns false rather than writing
 * an address the card cannot reach or a length that does not fit. */
bool rtw89_bd_write(u8 *slot, u64 phys, u32 len, bool last);

/* Add a request to the ring.  Returns false when the ring is full, which is
 * not an error - it means waiting for the card to catch up. */
bool rtw89_ring_add(rtw89_ring_t *r, u64 phys, u32 len, bool last);

/* How many slots are free.  One is always left empty: a ring whose two
 * indexes are equal is empty, so filling the last slot would make a full ring
 * indistinguishable from an empty one. */
u16 rtw89_ring_free(const rtw89_ring_t *r);

/* ---------------------------------------- where the card is told to look
 *
 * A ring in memory is not a ring the card knows about.  It has to be handed
 * over: the address in two halves because the register pair is thirty-two bits
 * each, the number of slots, and then - every time work is added - the index
 * the host has reached, which is what actually makes the card go and fetch.
 *
 * These are the BE-generation addresses, which are not the ones the older
 * parts use; the same channel has a register at 0x1160 on an AX part and
 * 0xB260 here.  Channel twelve is the one firmware commands travel on, which
 * is why it is the one named: it is the channel the download uses.
 */
#define R_BE_CH12_TXBD_DESA_L          0xB260
#define R_BE_CH12_TXBD_DESA_H          0xB264
#define R_BE_CH12_TXBD_NUM             0xB048
#define R_BE_CH12_TXBD_IDX             0xB130

/* And the same four for the ring the card fills.  These are in the driver's
 * PCI header rather than its register header, which is why they took looking
 * for: the ring registers belong to the bus interface rather than to the MAC. */
/* One register, two indexes.
 *
 * The host's position lives in the low twelve bits and the card's in bits 27
 * to 16, and each side writes only its own half.  Writing the whole word - as
 * the obvious reading of "the index register" suggests - wipes out the card's
 * half with whatever the host thought, and then the host reads back its own
 * number and concludes the card has caught up.  Everything appears to work and
 * nothing is ever received. */
#define BD_HOST_IDX_MASK               0x00000FFFu   /* 11:0  */
#define BD_CARD_IDX_MASK               0x0FFF0000u   /* 27:16 */
#define BD_CARD_IDX_SHIFT              16

#define R_BE_RXQ0_RXBD_NUM             0xB050
#define R_BE_RXQ0_RXBD_DESA_L          0xB300
#define R_BE_RXQ0_RXBD_DESA_H          0xB304

/* ------------------------------------------------- what the card says back
 *
 * The card answers on its own ring, and its replies carry the same header the
 * requests do: a category, a class, a function, and a length.  That symmetry
 * is real and worth relying on - the two were checked against the driver's
 * source separately and came out identical.
 *
 * The length in that header is of the WHOLE reply, this header included, and
 * it is a number the card chose.  Everything below treats it as such: a reply
 * claiming to be shorter than its own header, or longer than the bytes that
 * actually arrived, is refused rather than believed.  A card that has crashed
 * writes whatever was in memory, and a parser that trusts a length field is
 * how that becomes a read off the end of the buffer.
 */
typedef struct {
    u8  category;
    u8  cls;
    u8  func;
    const u8 *payload;
    u32 payload_len;
} rtw89_c2h_t;

/* Take a reply apart.  `received` is how many bytes actually arrived, which is
 * not the same as how many the reply says it is. */
bool rtw89_c2h_parse(const u8 *packet, u32 received, rtw89_c2h_t *out);

/* Tell the card where a ring is and how big.  Done once, before anything is
 * put in it. */
bool rtw89_ring_attach(volatile u8 *regs, rtw89_ring_t *r,
                       u32 desa_lo_reg, u32 desa_hi_reg,
                       u32 num_reg, u32 idx_reg);

/* And the doorbell: the host's index, written after the descriptors are in
 * memory and not before. */
void rtw89_ring_doorbell(volatile u8 *regs, const rtw89_ring_t *r,
                         u32 idx_reg);

/* How far the card has got, from the half of the index register it owns. */
u16 rtw89_ring_card_index(volatile u8 *regs, u32 idx_reg);

/* --------------------------------------------- asking, and being answered
 *
 * Everything above is a piece: a header, a descriptor, a ring, a way of
 * reading what comes back.  This is the join - one request going out the way
 * it really goes out, and its answer arriving the way answers really arrive.
 *
 * Pieces that each work are not the same as pieces that fit.  Every one of the
 * layers under this was checked on its own and none of them had ever been used
 * together, which is exactly the arrangement where the header is right, the
 * descriptor is right, the doorbell is right, and the card receives nothing
 * because the packet was left somewhere it does not look.
 */
typedef struct {
    rtw89_ring_t tx;              /* requests going down                    */
    rtw89_ring_t rx;              /* buffers waiting to be filled           */

    u8  *tx_buffer;               /* where a request is assembled           */
    u64  tx_buffer_phys;
    u8  *rx_buffer;               /* where an answer lands                  */
    u64  rx_buffer_phys;
    u32  rx_buffer_size;

    u8   sequence;                /* so an answer can be matched to a call  */
    u16  expected_tag;
} rtw89_channel_t;

/* Send one request and wait for its answer.  `reply` receives the payload of
 * the answer, without the header.  Returns the payload length, or -1.
 *
 * The answer does not always come back under the class and function that were
 * asked.  Most of the card's commands reply in kind, but the radio
 * calibrations are asked for on one class and reported on another - so what to
 * wait for is said separately rather than assumed. */
int rtw89_ask_as(volatile u8 *regs, rtw89_channel_t *ch,
                 u8 cat, u8 cls, u8 func,
                 u8 reply_cls, u8 reply_func,
                 const void *payload, u16 len, void *reply, u32 reply_cap,
                 int timeout_ms);

/* The common case: a command to the MAC, answered in kind. */
int rtw89_ask(volatile u8 *regs, rtw89_channel_t *ch, u8 cls, u8 func,
              const void *payload, u16 len, void *reply, u32 reply_cap,
              int timeout_ms);

/* ------------------------------------------------- calibrating the radio --
 *
 * The measurements a radio needs before it can send or receive accurately:
 * the converters' own offsets, and the receiver's standing DC level.  Both
 * drift with temperature and neither can be computed - they have to be
 * measured against the silicon.
 *
 * On this part the host does not do them.  It ASKS, and the card's own
 * processor runs them and reports back - which is why what would otherwise be
 * the largest and least verifiable part of a wireless driver is a handful of
 * mailbox exchanges here.
 */
#define H2C_CAT_OUTSRC             0x2
#define H2C_CL_OUTSRC_RF_FW_RFK    0xb

#define H2C_FUNC_RFK_TSSI_OFFLOAD    0x0
#define H2C_FUNC_RFK_IQK_OFFLOAD     0x1
#define H2C_FUNC_RFK_DPK_OFFLOAD     0x3
#define H2C_FUNC_RFK_TXGAPK_OFFLOAD  0x4
#define H2C_FUNC_RFK_DACK_OFFLOAD    0x5
#define H2C_FUNC_RFK_RXDCK_OFFLOAD   0x6
#define H2C_FUNC_RFK_PRE_NOTIFY      0x8

/* And what it answers on, which is not what it was asked on. */
#define RTW89_PHY_C2H_RFK_REPORT             0x9
#define RTW89_PHY_C2H_RFK_REPORT_FUNC_STATE  0

/* How it says it went.  "Started" is not "finished" and is reported as its
 * own thing - a driver that treats any answer as success carries on with an
 * uncalibrated radio. */
#define RTW89_RFK_STATE_START        0x0
#define RTW89_RFK_STATE_OK           0x1
#define RTW89_RFK_STATE_FAIL         0x2
#define RTW89_RFK_STATE_TIMEOUT      0x3
#define RTW89_RFK_STATE_H2C_CMD_ERR  0x4

/* Both chains at once. */
#define RTW89_RF_PATH_AB           0x3

const char *rtw89_rfk_state_name(u8 state);

/* Ask for one calibration and wait for the card to report on it.  Returns the
 * state the card reported, or -1 if it never answered. */
int rtw89_rfk_ask(volatile u8 *regs, rtw89_channel_t *ch, u8 func,
                  const void *payload, u16 len, int timeout_ms);

/* The two the radio needs before it is usable, in order. */
bool rtw89_rfk_calibrate(volatile u8 *regs, rtw89_channel_t *ch, u8 phy,
                         u8 band, u8 bandwidth, u8 channel);



/* --------------------------------------------- frames on their own ring ---
 *
 * The mailbox above carries requests to the card's processor.  This carries
 * frames to the air, which is a different ring, a different queue and a
 * different descriptor - and the same doorbell mechanism underneath.
 *
 * One buffer holds the descriptor and the frame together, because that is how
 * the card reads them: the descriptor says how far past itself the frame
 * begins, and the two travel as one region.
 */
typedef struct {
    rtw89_ring_t tx;
    u8  *buffer;                  /* descriptor and frame, in that order    */
    u64  buffer_phys;
    u32  buffer_size;
    u16  sequence;                /* the number the card puts in the frame  */
    int  sent;
} rtw89_data_t;

/* Point the card at a ring for outgoing frames. */
bool rtw89_data_attach(volatile u8 *regs, rtw89_data_t *d,
                       u8 *ring, u64 ring_phys, int slots,
                       u8 *buffer, u64 buffer_phys, u32 buffer_size);

/* Send one 802.11 frame.  Returns the number of bytes taken, or -1. */
int rtw89_data_transmit(volatile u8 *regs, rtw89_data_t *d,
                        const void *frame, int len, bool broadcast);

/* --------------------------------------------- frames coming the other way
 *
 * Receiving is not the sending path reversed.  Here the driver hands the card
 * empty buffers and the card decides when to fill them, so the buffers have to
 * be posted BEFORE anything arrives - a card with nowhere to put a frame drops
 * it and says nothing.
 *
 * More than one buffer, for the same reason: frames arrive in bursts, and a
 * single buffer means every frame after the first is lost while the driver is
 * still looking at the one before it.
 */
#define RTW89_RX_BUFFERS           16
#define RTW89_RX_BUFFER_BYTES      2048

typedef struct {
    rtw89_ring_t rx;
    u8  *buffer[RTW89_RX_BUFFERS];
    u64  buffer_phys[RTW89_RX_BUFFERS];
    u32  buffer_size;
    int  count;
    u16  next;                    /* the one the card fills next            */
    u16  expected_tag;

    int  frames;                  /* handed up                              */
    int  crc_errors;
    int  not_frames;              /* the card talking about itself          */
    int  unreadable;
} rtw89_rx_path_t;

/* Hand the card a ring and a pool of buffers.  `pool` must hold
 * count * each bytes, and be memory the card can reach. */
bool rtw89_rx_attach(volatile u8 *regs, rtw89_rx_path_t *r,
                     u8 *ring, u64 ring_phys,
                     u8 *pool, u64 pool_phys, int count, u32 each);

/* Take whatever has arrived, calling `deliver` for each 802.11 frame.
 * Returns how many frames were handed up. */
int rtw89_rx_poll(volatile u8 *regs, rtw89_rx_path_t *r,
                  void (*deliver)(void *ctx, const u8 *frame, u32 len,
                                  s8 signal_dbm),
                  void *ctx);

/* Bring a BE-generation part out of reset.  Returns false with a reason
 * logged; the register that would not answer is named. */
bool rtw89_power_on(volatile u8 *regs, const char *who);

/* The model, and the checks that drive the sequence against it. */
volatile u8 *rtw89_model_attach(void);
void         rtw89_model_detach(void);
int          rtw89_model_xtal_writes(void);
void         rtw89_model_last_xtal(u8 *addr, u8 *value, u8 *mask);
bool         rtw89_model_powered(void);
bool         rtw89_model_cpu_running(void);
bool         rtw89_model_debug_cleared(void);
int          rtw89_selftest(void);
bool         rtw89_model_dma_started(void);
/* Make the model answer the way a card does once it has an image: started, or
 * refused with the card's own status number. */
void         rtw89_model_firmware_started(void);
void         rtw89_model_firmware_refused(u8 card_number);
u16          rtw89_model_rx_ring_slots(void);

#endif /* KESTREL_RTW89_H */
