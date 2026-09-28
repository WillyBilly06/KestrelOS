/* rtw89.c - bringing a Realtek Wi-Fi 6E/7 part out of reset.
 *
 * The card in the machine this is aimed at is an RTL8922AE - Wi-Fi 7, the
 * "BE" generation.  Until now it was detected, named, and deliberately left
 * alone, because the driver next door was written for the 8188/8192/8812
 * family and those chips' registers mean entirely different things on this
 * one.  Listing a card and not touching it is the honest thing to do when the
 * alternative is writing another part's start-up sequence into it; it is not
 * a driver.
 *
 * This is the first part of one: the power-on sequence.  Nothing can be asked
 * of the card before it - no firmware, no scan, no association - because until
 * this runs the MAC is held in reset and the register file above 0x8000 does
 * not answer at all.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS AND IS NOT
 *
 * It is: the exact sequence Realtek's own driver performs, transcribed from
 * rtw8922a_pwr_on_func() with the register and bit definitions taken from the
 * same tree rather than remembered.  Two of the steps wait on the hardware to
 * answer, and one talks to a second register file through a mailbox.
 *
 * It is not: a working wireless card.  After this the MAC is out of reset and
 * its blocks are enabled.  Firmware download, the command interface, the
 * PHY/RF calibration tables and association are all still missing, and each is
 * a substantial piece of its own.  Saying so plainly matters more than usual
 * here, because "the Wi-Fi driver" sounds like one thing and is about six.
 *
 * ---------------------------------------------------------------------------
 * ON TESTING
 *
 * There is no RTL8922AE to run this against here, which is exactly the
 * situation this codebase already has an answer for: a model of the part that
 * answers the polls the way the silicon does, and a test that drives the real
 * sequence against it.  What that proves is that the order is right, that
 * every wait is actually waited on, and that the mailbox writes are encoded
 * correctly - which is most of what goes wrong.  It does not prove the card
 * comes up, and nothing here will until one is in front of it.
 */
#include "kernel.h"
#include "klog.h"
#include "time.h"
#include "mm.h"
#include "rtw89.h"

/* When a model is standing in for the card, it needs a moment to respond -
 * and the only moment real hardware would have is the read.  Same arrangement
 * as the older Realtek driver next door. */
void rtw89_model_sync(void);
static bool modelled;

/* Two flags the tests at the end of this file set.  The checks they silence
 * are correct and would matter on a real card; against a model they are the
 * test passing, and a boot that prints five warnings to show five checks
 * worked is a boot people learn to ignore warnings in. */
static bool quiet_refusals;
static bool expect_dirty_debug;

/* How far the mapping goes, and whether it was ever said.  Zero means nobody
 * told us, which is the old behaviour: reach anywhere. */
static u32 register_window;
static bool window_complained;

void rtw89_set_register_window(u32 bytes) {
    register_window = bytes;
    window_complained = false;
}

static bool reachable(u32 off) {
    if (!register_window || off + 4 <= register_window) return true;

    /* Once.  A register touched in a loop would otherwise fill the log with
     * the same line and bury what caused it. */
    if (!window_complained) {
        window_complained = true;
        kerr("rtw89", "register %05x is past the end of the %u bytes this card "
                      "mapped, so it was not touched", off, register_window);
    }
    return false;
}

static inline u32 rd32(volatile u8 *regs, u32 off) {
    if (modelled) rtw89_model_sync();
    if (!reachable(off)) return 0;
    return *(volatile u32 *)(regs + off);
}
static inline void wr32(volatile u8 *regs, u32 off, u32 v) {
    if (!reachable(off)) return;
    *(volatile u32 *)(regs + off) = v;

    /* The model is told after every write, not only before every read.
     *
     * Some of what this driver does only means anything as a sequence.  A
     * table is rebuilt by setting a bit and clearing it again - a pulse - and
     * a model that only looks when the driver next reads sees the register
     * after both writes, which is to say it sees nothing happen at all.  That
     * is not a fault in the driver and a model that reports it as one is
     * checking the wrong thing.
     *
     * A card sees every write as it lands.  So does this. */
    if (modelled) rtw89_model_sync();
}
static inline void set32(volatile u8 *regs, u32 off, u32 bits) {
    wr32(regs, off, rd32(regs, off) | bits);
}
static inline void clr32(volatile u8 *regs, u32 off, u32 bits) {
    wr32(regs, off, rd32(regs, off) & ~bits);
}
static inline void wr16(volatile u8 *regs, u32 off, u16 v) {
    *(volatile u16 *)(regs + off) = v;
}
static inline u16 rd16(volatile u8 *regs, u32 off) {
    if (modelled) rtw89_model_sync();
    return *(volatile u16 *)(regs + off);
}

static inline void set8(volatile u8 *regs, u32 off, u8 bits) {
    *(volatile u8 *)(regs + off) = (u8)(*(volatile u8 *)(regs + off) | bits);
}

/* Wait for a bit to become set, or to become clear.
 *
 * Both directions are needed and they are not interchangeable: the power-up
 * acknowledgement appears as a bit being set, and the "MAC is on" handshake
 * appears as the bit the driver just set being CLEARED by the hardware.  A
 * driver that waits the wrong way round on the second one waits forever on a
 * card that came up correctly.
 */
static bool wait_for(volatile u8 *regs, u32 off, u32 bit, bool set,
                     const char *what, const char *who) {
    /* Realtek polls every millisecond for three seconds.  The same numbers
     * are used here: a shorter limit turns a slow-but-working card into a
     * card this driver reports as broken. */
    for (int i = 0; i < 3000; i++) {
        bool now = (rd32(regs, off) & bit) != 0;
        if (now == set) return true;
        timer_udelay(1000);
    }
    kerr("rtw89", "%s: %s did not answer within three seconds (%#x is %#x)",
         who, what, off, rd32(regs, off));
    return false;
}

/* ------------------------------------------------- the crystal's side band
 *
 * A second register file, one byte wide, reached through a mailbox.  The
 * address, the value, the mask and the mode go into one word together with a
 * poll bit; the hardware clears the poll bit when the byte has landed.
 *
 * The mask is not decoration - these are shared registers and a write without
 * one changes bits belonging to something else.
 */
static bool xtal_si_write(volatile u8 *regs, u8 offset, u8 value, u8 mask,
                          const char *who) {
    u32 word = ((u32)offset << B_BE_WL_XTAL_SI_ADDR_SHIFT) |
               ((u32)value  << B_BE_WL_XTAL_SI_DATA_SHIFT) |
               ((u32)mask   << B_BE_WL_XTAL_SI_BITMASK_SHIFT) |
               ((u32)XTAL_SI_NORMAL_WRITE << B_BE_WL_XTAL_SI_MODE_SHIFT) |
               (0u << B_BE_WL_XTAL_SI_CHIPID_SHIFT) |
               B_BE_WL_XTAL_SI_CMD_POLL;
    wr32(regs, R_BE_WLAN_XTAL_SI_CTRL, word);

    /* Fifty microseconds apart for fifty milliseconds, which is what the
     * reference does - this is a much faster handshake than the power ones. */
    for (int i = 0; i < 1000; i++) {
        if (!(rd32(regs, R_BE_WLAN_XTAL_SI_CTRL) & B_BE_WL_XTAL_SI_CMD_POLL))
            return true;
        timer_udelay(50);
    }
    kerr("rtw89", "%s: the crystal's side band did not accept a write to "
                  "%#x (value %#x, mask %#x)", who, offset, value, mask);
    return false;
}

/* ------------------------------------------------------- the DMA engine */

bool rtw89_dma_idle(volatile u8 *regs) {
    u32 busy = rd32(regs, R_BE_HAXI_DMA_BUSY1);
    return (busy & (BE_ALL_TX_CHANNELS | B_BE_RXQ0_BUSY_V1 |
                    B_BE_RPQ0_BUSY_V1)) == 0;
}

static bool wait_idle(volatile u8 *regs, const char *who) {
    /* Ten microseconds apart for a millisecond, which is what the reference
     * does - this is a bus engine draining, not a power rail settling. */
    for (int i = 0; i < 100; i++) {
        if (rtw89_dma_idle(regs)) return true;
        timer_udelay(10);
    }
    kerr("rtw89", "%s: its DMA engine did not go idle (%#x still busy)",
         who, rd32(regs, R_BE_HAXI_DMA_BUSY1));
    return false;
}

bool rtw89_dma_reset(volatile u8 *regs, u16 rx_ring_slots, const char *who) {
    if (!regs) return false;
    if (!who) who = "the card";
    if (!rx_ring_slots) return false;

    /* 1. Stop it, and wait.  Asking is not the same as it having stopped: a
     *    channel part-way through a descriptor will finish that descriptor,
     *    and clearing its pointers underneath it is how a ring ends up
     *    describing memory that has already been handed back. */
    clr32(regs, R_BE_HAXI_INIT_CFG1, B_BE_TXDMA_EN | B_BE_RXDMA_EN);
    set32(regs, R_BE_HAXI_DMA_STOP1, B_BE_STOP_WPDMA);

    if (!wait_idle(regs, who)) return false;

    /* 2. Clear where every channel thinks it is - fifteen for transmit, two
     *    for receive - and then tell the receive rings how long they are. */
    wr32(regs, R_BE_TXBD_RWPTR_CLR1, BE_ALL_TX_CHANNELS);
    wr32(regs, R_BE_RXBD_RWPTR_CLR1_V1, B_BE_CLR_RXQ0_IDX | B_BE_CLR_RPQ0_IDX);

    /* Reset the PCIe BDRAM boundary before restarting descriptor fetch.
     * Hardware clears this pulse; a stuck pulse means the ring cannot be
     * trusted even if TXDMA_EN later reads back as set. */
    set32(regs, R_BE_HAXI_INIT_CFG1, 1u << 16);
    bool bdram_ready = false;
    for (int i = 0; i < 10000; i++) {
        if (!(rd32(regs, R_BE_HAXI_INIT_CFG1) & (1u << 16))) {
            bdram_ready = true;
            break;
        }
        timer_udelay(50);
    }
    if (!bdram_ready) {
        kerr("rtw89", "%s: PCIe BDRAM boundary reset timed out", who);
        return false;
    }

    /* One less than the count.  The card is told the index of the last slot,
     * and handing it the count instead gives it one slot more than exists -
     * which it will use. */
    wr16(regs, R_BE_RXQ0_RXBD_IDX_V1, (u16)(rx_ring_slots - 1));
    wr16(regs, R_BE_RPQ0_RXBD_IDX_V1, (u16)(rx_ring_slots - 1));

    /* 3. And start it: the two directions, the bus master, and a watchdog
     *    long enough not to fire on an ordinary transfer. */
    u32 cfg = rd32(regs, R_BE_HAXI_INIT_CFG1);
    cfg |= B_BE_TXDMA_EN | B_BE_RXDMA_EN;
    cfg &= ~B_BE_STOP_AXI_MST;
    wr32(regs, R_BE_HAXI_INIT_CFG1, cfg);

    u32 wdt = rd32(regs, R_BE_HAXI_MST_WDT_TIMEOUT_SEL);
    wdt = (wdt & ~B_BE_HAXI_MST_WDT_TIMEOUT_MASK) | 4;
    wr32(regs, R_BE_HAXI_MST_WDT_TIMEOUT_SEL, wdt);

    clr32(regs, R_BE_HAXI_DMA_STOP1, B_BE_STOP_WPDMA);

    /* And read it back.
     *
     * Writing a register and carrying on is an assumption; this is a bus
     * engine that was just told to stop, and a chip that did not take the
     * start would leave every later transfer sitting in a ring nothing
     * collects - which looks like a card that receives nothing rather than
     * like a register that did not stick. */
    u32 back = rd32(regs, R_BE_HAXI_INIT_CFG1);
    if (!(back & B_BE_TXDMA_EN) || !(back & B_BE_RXDMA_EN) ||
        (back & B_BE_STOP_AXI_MST)) {
        kerr("rtw89", "%s: its DMA engine did not take the start (%#x)",
             who, back);
        return false;
    }
    if (rd32(regs, R_BE_HAXI_DMA_STOP1) & B_BE_STOP_WPDMA) {
        kerr("rtw89", "%s: its DMA engine is still held stopped", who);
        return false;
    }
    return true;
}

/* ------------------------------------------------ starting the card's CPU */


u8 rtw89_fwdl_status(volatile u8 *regs) {
    u32 ctrl = rd32(regs, R_BE_WCPU_FW_CTRL);

    u8 raw = (u8)((ctrl >> B_BE_WCPU_FWDL_STATUS_SHIFT) &
                  B_BE_WCPU_FWDL_STATUS_MASK);

    /* A refusal is read before anything else, and that order matters.
     *
     * This used to conclude "running" from the enable bit alone, and return
     * before the status field was so much as looked at.  A card holding a bad
     * checksum or a signature it would not accept was therefore reported as a
     * card running its firmware - the one answer that makes every later
     * failure impossible to trace back to the download.  Caught by driving the
     * four refusals against the model and finding all four came back as
     * success. */
    if (raw >= 4 && raw <= 7) {
        switch (raw) {
        case 4: return RTW89_FWDL_CHECKSUM_FAIL;
        case 5: case 6: return RTW89_FWDL_SECURITY_FAIL;
        default: return RTW89_FWDL_CV_NOT_MATCH;
        }
    }

    /* The download is finished when the enable bit the driver set has been
     * cleared by the card - the same shape of handshake as the MAC coming out
     * of reset, and the same trap if it is read the other way round. */
    if (!(ctrl & B_BE_WLANCPU_FWDL_EN)) return RTW89_FWDL_WCPU_FW_INIT_RDY;

    /* The card's numbering is not the driver's, and the two are not in the
     * same order.  Realtek keeps a table; so does this, because mapping it
     * with arithmetic would happen to work for the first few values and then
     * report a security failure as "ready". */
    switch (raw) {
    case 0: return RTW89_FWDL_INITIAL_STATE;
    case 1: return RTW89_FWDL_FWDL_ONGOING;
    case 2: return RTW89_FWDL_WCPU_FWDL_RDY;
    case 3: return RTW89_FWDL_WCPU_FW_INIT_RDY;
    case 4: return RTW89_FWDL_CHECKSUM_FAIL;
    case 5: case 6: return RTW89_FWDL_SECURITY_FAIL;
    case 7: return RTW89_FWDL_CV_NOT_MATCH;
    default: return RTW89_FWDL_INITIAL_STATE;
    }
}

const char *rtw89_fwdl_status_name(u8 status) {
    switch (status) {
    case RTW89_FWDL_INITIAL_STATE:    return "not started";
    case RTW89_FWDL_FWDL_ONGOING:     return "in progress";
    case RTW89_FWDL_CHECKSUM_FAIL:    return "refused: the checksum did not match";
    case RTW89_FWDL_SECURITY_FAIL:    return "refused: the signature was not accepted";
    case RTW89_FWDL_CV_NOT_MATCH:     return "refused: built for a different revision of this chip";
    case RTW89_FWDL_WCPU_FWDL_RDY:    return "ready to receive";
    case RTW89_FWDL_WCPU_FW_INIT_RDY: return "loaded and running";
    default:                          return "unrecognised";
    }
}

bool rtw89_fwdl_path_ready(volatile u8 *regs, bool h2c, const char *who) {
    if (!regs) return false;
    u32 want = h2c ? B_BE_H2C_PATH_RDY : B_BE_DLFW_PATH_RDY;

    /* A microsecond apart for a second, which is what the reference does -
     * this handshake is with a processor that has just started, not with a
     * power rail. */
    for (int i = 0; i < 1000000; i++) {
        u32 ctrl = rd32(regs, R_BE_WCPU_FW_CTRL);
        if (ctrl == 0xffffffffu) return false;
        if (ctrl & want) return true;
        timer_udelay(1);
    }
    kerr("rtw89", "%s: the %s path never opened (control register %#x)",
         who ? who : "the card", h2c ? "command" : "download",
         rd32(regs, R_BE_WCPU_FW_CTRL));
    return false;
}

bool rtw89_fwdl_start_cpu(volatile u8 *regs, u8 boot_reason, bool include_bb,
                          const char *who) {
    if (!regs) return false;
    if (!who) who = "the card";
    /* Linux mac_be.c disable_cpu precedes fwdl_enable_wcpu for EVERY
     * download, including warm boot. Preserve only RUN_ENV; stale H2C-ready
     * and INIT_RDY bits must not satisfy the new ROM handshake. Hold first,
     * clear status while held, then use the normal enable sequence below. */
    if (rd32(regs, R_BE_WCPU_FW_CTRL) == 0xffffffffu) return false;
    clr32(regs, R_BE_PLATFORM_ENABLE, B_BE_WCPU_EN);
    set32(regs, R_BE_PLATFORM_ENABLE, B_BE_HOLD_AFTER_RESET);
    set32(regs, R_BE_PLATFORM_ENABLE, B_BE_WCPU_EN);
    wr32(regs, R_BE_WCPU_FW_CTRL,
         rd32(regs, R_BE_WCPU_FW_CTRL) & B_BE_RUN_ENV_MASK);
    set32(regs, R_BE_DCPU_PLATFORM_ENABLE, B_BE_DCPU_PLATFORM_EN);
    wr32(regs, R_BE_UDM0, 0);
    wr32(regs, R_BE_HALT_C2H, 0);
    wr32(regs, R_BE_UDM2, 0);

    /* Which processors are about to be loaded.  The 8922 carries a separate
     * one for the baseband and it is loaded in the same pass. */
    u32 enable = B_BE_WLANCPU_FWDL_EN;
    if (include_bb) enable |= B_BE_BBMCU0_FWDL_EN;
    set32(regs, R_BE_WCPU_FW_CTRL, enable);

    /* Anything still in the debug registers is the previous life of a card
     * that fell over rather than stopped.  Worth saying before it is wiped,
     * because after this there is no way to know it was there. */
    u32 halt = rd32(regs, R_BE_HALT_C2H);
    u32 udm1 = rd32(regs, R_BE_UDM1);
    u32 udm2 = rd32(regs, R_BE_UDM2);
    if ((halt || udm1 || udm2) && !expect_dirty_debug)
        kwarn("rtw89", "%s: its debug registers were not empty before boot "
                       "(%#x, %#x, %#x) - it did not shut down cleanly last "
                       "time", who, halt, udm1, udm2);

    wr32(regs, R_BE_UDM1, 0);
    wr32(regs, R_BE_UDM2, 0);
    wr32(regs, R_BE_HALT_H2C, 0);
    wr32(regs, R_BE_HALT_C2H, 0);
    wr32(regs, R_BE_HALT_H2C_CTRL, 0);
    wr32(regs, R_BE_HALT_C2H_CTRL, 0);

    /* Acknowledge any interrupt already pending, or the first real one is
     * lost behind it. */
    wr32(regs, R_BE_HISR0, B_BE_HALT_C2H_INT);

    set32(regs, R_BE_SYS_CLK_CTRL, B_BE_CPU_CLK_EN);
    clr32(regs, R_BE_SYS_CFG5, B_BE_WDT_WAKE_PCIE_EN | B_BE_WDT_WAKE_USB_EN);
    clr32(regs, R_BE_WCPU_FW_CTRL, B_BE_WDT_PLT_RST_EN | B_BE_WCPU_ROM_CUT_GET);

    /* Why it is booting, in its own three bits - the rest of that register
     * belongs to something else, so it is read, masked and put back. */
    u16 reason = rd16(regs, R_BE_BOOT_REASON);
    reason = (u16)((reason & ~(u16)B_BE_BOOT_REASON_MASK) |
                   (boot_reason & B_BE_BOOT_REASON_MASK));
    wr16(regs, R_BE_BOOT_REASON, reason);

    /* Off, out of the hold, then on.  All three, in that order: releasing the
     * hold while it is running does nothing, and turning it on without
     * releasing the hold starts a processor that immediately stops. */
    clr32(regs, R_BE_PLATFORM_ENABLE, B_BE_WCPU_EN);
    clr32(regs, R_BE_PLATFORM_ENABLE, B_BE_HOLD_AFTER_RESET);
    set32(regs, R_BE_PLATFORM_ENABLE, B_BE_WCPU_EN);

    /* The ROM first accepts the firmware-header H2C. Its raw download path
     * opens only after that header has been consumed. */
    if (!rtw89_fwdl_path_ready(regs, true, who)) return false;

    u8 st = rtw89_fwdl_status(regs);
    if (st != RTW89_FWDL_WCPU_FWDL_RDY && st != RTW89_FWDL_INITIAL_STATE &&
        st != RTW89_FWDL_FWDL_ONGOING) {
        kerr("rtw89", "%s: its processor started but reports \"%s\"",
             who, rtw89_fwdl_status_name(st));
        return false;
    }
    return true;
}

/* ------------------------------------------------- the firmware container
 *
 * Everything here is a bounds check with a name.  That is deliberate: this
 * reads a file the card will then be told to execute, and every field in it
 * decides where some of those bytes are written into the card's memory.  A
 * length taken on trust is a download that walks off the end of the file and
 * into whatever the allocator left there; a download address taken on trust is
 * the card's own memory being overwritten somewhere it did not expect.
 *
 * So each field is read, checked against the file it came from, and the check
 * says what failed rather than returning a bare false.
 */
/* The tests below deliberately hand this malformed files to check that each
 * one is refused.  Those refusals are the test passing, so they do not go in
 * the log - a boot that prints four warnings to show that four checks worked
 * is a boot somebody has to learn to ignore warnings in. */

static void put32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 le32_at(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Stop descriptor fetch on a BE card before changing software radio state.
 * DMA backing remains allocated even on success: the card might retain a
 * descriptor address across a warm software stop, and restart is not yet
 * implemented. */
bool rtw89_dma_quiesce(volatile u8 *regs, const char *who) {
    if (!regs) return false;
    if (!who) who = "the card";

    clr32(regs, R_BE_CMAC_FUNC_EN, B_BE_CMAC_TXEN | B_BE_CMAC_RXEN);
    clr32(regs, R_BE_HAXI_INIT_CFG1, B_BE_TXDMA_EN | B_BE_RXDMA_EN);
    set32(regs, R_BE_HAXI_DMA_STOP1, B_BE_STOP_WPDMA | BE_ALL_TX_CHANNELS);
    if (!wait_idle(regs, who)) return false;
    wr32(regs, R_BE_TXBD_RWPTR_CLR1, BE_ALL_TX_CHANNELS);
    wr32(regs, R_BE_RXBD_RWPTR_CLR1_V1, B_BE_CLR_RXQ0_IDX | B_BE_CLR_RPQ0_IDX);
    return true;
}

/* The firmware image travels through CH12 before the normal traffic queues
 * exist.  Linux's RTL8922A DLFW quota is distinct from its later SCC quota:
 * WDE gives six pages to WCPU, PLE gives 32 C2H, 256 H2C and one CPU-I/O.
 * Program and wait for both DLE managers before exposing the CH12 ring. */
bool rtw89_fwdl_preinit(volatile u8 *regs, const char *who) {
    if (!regs) return false;
    if (!who) who = "the card";

    set32(regs, 0x7880, 3u);                 /* HCI TXDMA/RXDMA functions */
    clr32(regs, R_BE_HAXI_INIT_CFG1, 7u << 8); /* PCIe, no data CPU */
    clr32(regs, R_BE_HAXI_DMA_STOP1, BE_ALL_TX_CHANNELS);
    set32(regs, 0x8420, 1u << 12);          /* BE DMAC address mode */

    clr32(regs, R_BE_DMAC_FUNC_EN, B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN);
    set32(regs, R_BE_DMAC_CLK_EN, B_BE_DLE_WDE_CLK_EN | B_BE_DLE_PLE_CLK_EN);
    u32 wde = rd32(regs, R_BE_WDE_PKTBUF_CFG);
    wde &= ~(B_BE_WDE_PAGE_SEL_MASK | B_BE_WDE_START_BOUND_MASK |
             B_BE_WDE_FREE_PAGE_NUM_MASK);
    wr32(regs, R_BE_WDE_PKTBUF_CFG, wde);   /* 64-byte pages, none linked */
    u32 ple = rd32(regs, R_BE_PLE_PKTBUF_CFG);
    ple &= ~(B_BE_PLE_PAGE_SEL_MASK | B_BE_PLE_START_BOUND_MASK |
             B_BE_PLE_FREE_PAGE_NUM_MASK);
    ple |= PLE_PAGE_SEL_128 | ((RTW89_PLE_START_OFFSET / DLE_BOUND_UNIT) << 8) |
           (2928u << 16);
    wr32(regs, R_BE_PLE_PKTBUF_CFG, ple);
    for (u32 i = 0; i < 5; i++)
        wr32(regs, 0x8C40 + i * 4, i == 1 ? (6u << 16) | 6u : 0);
    for (u32 i = 0; i < 13; i++) {
        u32 quota = i == 2 ? 32u : i == 3 ? 256u : i == 10 ? 1u : 0u;
        wr32(regs, 0x9040 + i * 4, (quota << 16) | quota);
    }
    set32(regs, R_BE_DMAC_FUNC_EN, B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN);
    bool wde_ready = false, ple_ready = false;
    for (unsigned engine = 0; engine < 2; ++engine) {
        bool ready = false;
        for (unsigned us = 0; us < 2000; ++us) {
            u32 status = rd32(regs, engine ? 0x9100 : 0x8D00);
            if (status == 0xffffffffu) break;
            if ((status & 3u) == 3u) { ready = true; break; }
            timer_udelay(1);
        }
        if (!engine) wde_ready = ready;
        else ple_ready = ready;
        if (!ready) break;
    }
    if (!wde_ready || !ple_ready) {
        clr32(regs, R_BE_DMAC_FUNC_EN, B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN);
        kerr("rtw89", "%s: firmware-download DLE did not initialize (WDE=%d PLE=%d)",
             who, wde_ready, ple_ready);
        return false;
    }

    /* Linux's hfc_init(reset=true,en=false,h2c_en=true) fast path: only
     * CH12 is enabled; the ordinary channel/public quotas come after FWDL. */
    clr32(regs, R_BE_HCI_FC_CTRL, (1u << 0) | (1u << 3));
    wr32(regs, R_BE_CH_PAGE_CTRL, 0); /* DLFW h2c_prec=256, 6-bit field */
    u32 fc = rd32(regs, R_BE_HCI_FC_CTRL);
    fc = (fc & ~((1u << 0) | (1u << 3))) | (1u << 3);
    wr32(regs, R_BE_HCI_FC_CTRL, fc);
    return true;
}

static u16 le16_at(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

bool rtw89_chip_cv(volatile u8 *regs, u8 *cv) {
    if (!regs || !cv) return false;
    u32 sys_cfg = rd32(regs, R_AX_SYS_CFG1);
    if (sys_cfg == 0xFFFFFFFFu) return false;
    *cv = (u8)((sys_cfg & B_AX_CHIP_VER_MASK) >> B_AX_CHIP_VER_SHIFT);
    return true;
}

/* Pick one executable image out of linux-firmware's MFW wrapper.  The table is
 * intentionally walked in full: its entries are not ordered by cut/version,
 * so the first compatible image is not necessarily the closest one. */
bool rtw89_select_firmware(const u8 *data, size_t size, u8 hardware_cv,
                           u8 type, rtw89_fw_image_t *out) {
    if (!data || !out) return false;
    memset(out, 0, sizeof *out);

    /* Older files contain the executable image directly. */
    if (size && data[0] != RTW89_MFW_SIG) {
        if (type != RTW89_FW_NORMAL) return false;
        out->data = data;
        out->size = size;
        out->cv = hardware_cv;
        out->type = type;
        return true;
    }

    if (size < RTW89_MFW_HEADER_BYTES) {
        if (!quiet_refusals)
            kwarn("rtw89", "the MFW container is %u bytes, shorter than its header",
                  (unsigned)size);
        return false;
    }

    u8 count = data[1];
    size_t table_end = RTW89_MFW_HEADER_BYTES +
                       (size_t)count * RTW89_MFW_ENTRY_BYTES;
    if (!count || table_end > size) {
        if (!quiet_refusals)
            kwarn("rtw89", "the MFW container's %u entries do not fit in its %u bytes",
                  count, (unsigned)size);
        return false;
    }

    const u8 *best = NULL;
    u8 best_cv = 0;
    for (u8 i = 0; i < count; i++) {
        const u8 *entry = data + RTW89_MFW_HEADER_BYTES +
                          (size_t)i * RTW89_MFW_ENTRY_BYTES;
        u8 cv = entry[0];
        u8 entry_type = entry[1];
        bool mp = entry[2] != 0;

        if (entry_type != type || mp || cv > hardware_cv) continue;
        if (!best || cv > best_cv) {
            best = entry;
            best_cv = cv;
        }
    }

    if (!best) {
        if (!quiet_refusals)
            kwarn("rtw89", "the MFW container has no type %u image for hardware cut %u",
                  type, hardware_cv);
        return false;
    }

    u32 offset = le32_at(best + 4);
    u32 image_size = le32_at(best + 8);
    if (!image_size || offset < table_end || offset > size ||
        image_size > size - offset) {
        if (!quiet_refusals)
            kwarn("rtw89", "the selected MFW image is %u bytes at %u, outside its %u byte file",
                  image_size, offset, (unsigned)size);
        return false;
    }

    out->data = data + offset;
    out->size = image_size;
    out->cv = best_cv;
    out->type = type;
    out->from_container = true;
    return true;
}

static bool rtw89_formatted_mssc_length(const u8 *section, u32 section_len,
                                        const u8 *after, size_t available,
                                        bool dsp_checksum, u32 *length) {
    static const u8 signature[8] = { 'M', 'S', 'S', 'K', 'P', 'O', 'O', 'L' };

    /* key_sign_len lives in the security section itself. */
    if (section_len < 62 || available < 32) return false;
    if (memcmp(after, signature, sizeof signature) != 0) return false;

    u8 defen = after[16];
    u8 format = after[20];
    u8 device_max = after[21];
    u16 keypairs = le16_at(after + 22);
    u16 customer_max = le16_at(after + 24);
    u16 key_max = le16_at(after + 26);
    if (format != 0) return false;

    u64 remap = (u64)key_max * customer_max * device_max;
    remap = (remap >> 3) + (defen ? 8 : 0);
    if (remap > 0xFFFFFFFFu - 32u ||
        le32_at(after + 12) != 32 + (u32)remap)
        return false;

    u32 key_len = le16_at(section + 60) >> 2;
    if (!key_len) key_len = FWDL_SECURITY_SIGLEN;
    if (dsp_checksum) key_len += FWDL_SECURITY_CHKSUM_LEN;

    u64 total = 32 + remap + (u64)keypairs * key_len;
    if (total > available || total > 0xFFFFFFFFu) return false;
    *length = (u32)total;
    return true;
}

bool rtw89_parse_firmware(const u8 *data, size_t size, rtw89_fw_info_t *out) {
    if (!data || !out) return false;
    memset(out, 0, sizeof *out);

    if (size < RTW89_FW_HDR_V1_BYTES) {
        if (!quiet_refusals) kwarn("rtw89", "the firmware file is %u bytes, shorter than its own "
                       "header", (unsigned)size);
        return false;
    }

    const u32 w1 = le32_at(data + 1 * 4);
    const u32 w3 = le32_at(data + 3 * 4);
    const u32 w4 = le32_at(data + 4 * 4);
    const u32 w5 = le32_at(data + 5 * 4);
    const u32 w6 = le32_at(data + 6 * 4);
    const u32 w7 = le32_at(data + 7 * 4);

    out->major      = (u8)(w1 >> FW_HDR_V1_W1_MAJOR_SHIFT);
    out->minor      = (u8)(w1 >> FW_HDR_V1_W1_MINOR_SHIFT);
    out->subversion = (u8)(w1 >> FW_HDR_V1_W1_SUBVER_SHIFT);
    out->subindex   = (u8)(w1 >> FW_HDR_V1_W1_SUBIDX_SHIFT);

    out->header_version = (u8)(w3 >> FW_HDR_V1_W3_HDR_VER_SHIFT);
    if (out->header_version != 1) {
        if (!quiet_refusals)
            kwarn("rtw89", "firmware header version %u is not the v1 layout this driver reads",
                  out->header_version);
        return false;
    }
    out->month = (u8)(w4 >> FW_HDR_V1_W4_MONTH_SHIFT);
    out->date  = (u8)(w4 >> FW_HDR_V1_W4_DATE_SHIFT);
    out->year  = (u16)(w5 & FW_HDR_V1_W5_YEAR_MASK);

    out->section_count = (int)((w6 >> FW_HDR_V1_W6_SEC_NUM_SHIFT) & 0xFF);
    if (out->section_count <= 0 ||
        out->section_count > RTW89_FW_MAX_SECTIONS) {
        if (!quiet_refusals) kwarn("rtw89", "the firmware file claims %d sections; this driver "
                       "handles one to %d", out->section_count,
              RTW89_FW_MAX_SECTIONS);
        return false;
    }

    /* Where the sections' bytes start.  Normally the header plus one
     * descriptor each; a file carrying a dynamic header says so and gives the
     * length itself, and that length is not to be trusted without checking it
     * covers at least what the descriptors need. */
    u32 base = RTW89_FW_HDR_V1_BYTES +
               (u32)out->section_count * RTW89_FW_SECTION_V1_BYTES;
    if (base > size) {
        if (!quiet_refusals)
            kwarn("rtw89", "the firmware's %d section descriptors run past its %u bytes",
                  out->section_count, (unsigned)size);
        return false;
    }

    out->part_size = w7 & 0xFFFFu;
    if (!out->part_size || out->part_size > FWDL_SECTION_PER_PKT_LEN) {
        if (!quiet_refusals)
            kwarn("rtw89", "the firmware's request size is %u; this driver handles 1 to %u",
                  out->part_size, FWDL_SECTION_PER_PKT_LEN);
        return false;
    }

    if (w7 & FW_HDR_V1_W7_DYN_HDR) {
        out->header_length = (w5 >> FW_HDR_V1_W5_HDR_SIZE_SHIFT) & 0xFFFF;
        if (out->header_length < base) {
            if (!quiet_refusals) kwarn("rtw89", "the firmware file's header is %u bytes, which is "
                           "less than its own %d descriptors need (%u)",
                  out->header_length, out->section_count, base);
            return false;
        }
        if (out->header_length > size) {
            if (!quiet_refusals)
                kwarn("rtw89", "the firmware file's header runs past the end of it "
                               "(%u of %u bytes)", out->header_length,
                      (unsigned)size);
            return false;
        }
        out->dynamic_header_length = out->header_length - base;
        if (out->dynamic_header_length < 8 ||
            le32_at(data + base) != out->dynamic_header_length) {
            if (!quiet_refusals)
                kwarn("rtw89", "the firmware's dynamic header does not describe its %u bytes",
                      out->dynamic_header_length);
            return false;
        }
    } else {
        out->header_length = base;
    }

    if (out->header_length > size) {
        if (!quiet_refusals) kwarn("rtw89", "the firmware file's header runs past the end of it "
                       "(%u of %u bytes)", out->header_length, (unsigned)size);
        return false;
    }

    /* The descriptors, and then the one question that matters: do the pieces
     * they describe actually fit in the file? */
    u64 payload = 0;
    u64 stored = 0;
    size_t at = out->header_length;
    bool dsp_checksum = (w6 & FW_HDR_V1_W6_DSP_CHKSUM) != 0;
    for (int i = 0; i < out->section_count; i++) {
        const u8 *sec = data + RTW89_FW_HDR_V1_BYTES +
                        (size_t)i * RTW89_FW_SECTION_V1_BYTES;
        u32 s0 = le32_at(sec + 0);
        u32 s1 = le32_at(sec + 4);

        rtw89_fw_section_t *d = &out->sections[i];
        d->download_address = s0;
        d->length = s1 & FWSEC_V1_W1_SIZE_MASK;
        d->type = (u8)((s1 >> FWSEC_V1_W1_TYPE_SHIFT) & FWSEC_V1_W1_TYPE_MASK);
        d->redownload = (s1 & FWSEC_V1_W1_REDL) != 0;

        /* A section that carries a checksum is that many bytes longer in the
         * file than its own length field says.  Forgetting this reads the
         * next section eight bytes early, from the middle of this one. */
        if (s1 & FWSEC_V1_W1_CHECKSUM) d->length += FWDL_SECTION_CHKSUM_LEN;

        if (!d->length) {
            if (!quiet_refusals) kwarn("rtw89", "section %d of the firmware file is empty", i);
            return false;
        }

        if (d->length > size - at) {
            if (!quiet_refusals)
                kwarn("rtw89", "section %d needs %u bytes but only %u remain",
                      i, d->length, (unsigned)(size - at));
            return false;
        }

        if (d->type == FWDL_SECURITY_SECTION_TYPE) {
            d->mssc = (u8)le32_at(sec + 8);
            size_t after_section = at + d->length;
            size_t available = size - after_section;

            if (d->mssc == 0xFF) {
                if (!rtw89_formatted_mssc_length(data + at, d->length,
                                                 data + after_section, available,
                                                 dsp_checksum, &d->mssc_length)) {
                    if (!quiet_refusals)
                        kwarn("rtw89", "section %d has a malformed formatted MSSC pool", i);
                    return false;
                }
            } else {
                u32 each = FWDL_SECURITY_SIGLEN +
                           (dsp_checksum ? FWDL_SECURITY_CHKSUM_LEN : 0);
                d->mssc_length = (u32)d->mssc * each;
                if (d->mssc_length > available) {
                    if (!quiet_refusals)
                        kwarn("rtw89", "section %d's MSSC pool needs %u bytes but only %u remain",
                              i, d->mssc_length, (unsigned)available);
                    return false;
                }
            }
            if (d->mssc_length) out->needs_security_profile = true;
        }

        payload += d->length;
        stored += (u64)d->length + d->mssc_length;
        at += (size_t)d->length + d->mssc_length;
    }

    if (at != size) {
        if (!quiet_refusals) kwarn("rtw89", "the firmware file has %u unclaimed bytes after its %d sections",
                                   (unsigned)(size - at), out->section_count);
        return false;
    }

    out->payload_bytes = (u32)payload;
    out->file_bytes = (u32)stored;
    return true;
}

#include "rtw89_security.h"

/* --------------------------------------------------------------- power on */

bool rtw89_power_on(volatile u8 *regs, const char *who) {
    if (!regs) return false;
    if (!who) who = "the card";

    /* 1. Stop it going back to sleep, and let it out of the suspend it is
     *    holding itself in. */
    clr32(regs, R_BE_SYS_PW_CTRL, B_BE_AFSM_WLSUS_EN | B_BE_AFSM_PCIE_SUS_EN);
    set32(regs, R_BE_SYS_PW_CTRL, B_BE_DIS_WLBT_PDNSUSEN_SOPC);
    set32(regs, R_BE_WLLPS_CTRL,  B_BE_DIS_WLBT_LPSEN_LOPC);
    clr32(regs, R_BE_SYS_PW_CTRL, B_BE_APDM_HPDN);
    clr32(regs, R_BE_SYS_PW_CTRL, B_BE_APFM_SWLPS);

    if (!wait_for(regs, R_BE_SYS_PW_CTRL, B_BE_RDY_SYSPWR, true,
                  "system power", who))
        return false;

    /* 2. Turn the wireless side on and ask for the MAC.
     *
     *    The acknowledgement is the hardware CLEARING the bit just set, not
     *    setting another one.  Waiting the other way round here is the classic
     *    way to hang on a card that came up perfectly. */
    set32(regs, R_BE_SYS_PW_CTRL, B_BE_EN_WLON);
    set32(regs, R_BE_WLRESUME_CTRL, B_BE_LPSROP_CMAC0 | B_BE_LPSROP_CMAC1);
    set32(regs, R_BE_SYS_PW_CTRL, B_BE_APFN_ONMAC);

    if (!wait_for(regs, R_BE_SYS_PW_CTRL, B_BE_APFN_ONMAC, false,
                  "the MAC coming out of reset", who))
        return false;

    /* 3. Clocks, the analogue supplies, and the platform itself. */
    clr32(regs, R_BE_AFE_ON_CTRL1, B_BE_REG_CK_MON_CK960M_EN);
    set8 (regs, R_BE_ANAPAR_POW_MAC,
          (u8)(B_BE_POW_PC_LDO_PORT0 | B_BE_POW_PC_LDO_PORT1));
    clr32(regs, R_BE_FEN_RST_ENABLE,
          B_BE_R_SYM_ISO_ADDA_P02PP | B_BE_R_SYM_ISO_ADDA_P12PP);
    set8 (regs, R_BE_PLATFORM_ENABLE, (u8)B_BE_PLATFORM_EN);

    /* 4. The bus interface, which answers twice: once to say its registers
     *    are reachable, and once to say it has finished restoring itself. */
    set32(regs, R_BE_HCI_OPT_CTRL, B_BE_HAXIDMA_IO_EN);
    if (!wait_for(regs, R_BE_HCI_OPT_CTRL, B_BE_HAXIDMA_IO_ST, true,
                  "the bus interface's registers", who))
        return false;
    if (!wait_for(regs, R_BE_HCI_OPT_CTRL, B_BE_HAXIDMA_BACKUP_RESTORE_ST,
                  false, "the bus interface finishing its restore", who))
        return false;

    set32(regs, R_BE_HCI_OPT_CTRL, B_BE_HCI_WLAN_IO_EN);
    if (!wait_for(regs, R_BE_HCI_OPT_CTRL, B_BE_HCI_WLAN_IO_ST, true,
                  "the wireless side of the bus interface", who))
        return false;

    clr32(regs, R_BE_SYS_SDIO_CTRL, B_BE_PCIE_FORCE_IBX_EN);

    /* 5. The crystal and the radio front end, through the side band.
     *
     *    Order matters and is not obvious: the phase-locked loop is enabled
     *    before the pads it feeds, and each radio chain's pad is powered
     *    before that chain is configured. */
    if (!xtal_si_write(regs, XTAL_SI_PLL, 0x02, 0x02, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_PLL, 0x01, 0x01, who)) return false;

    set32(regs, R_BE_SYS_ADIE_PAD_PWR_CTRL, B_BE_SYM_PADPDN_WL_RFC1_1P3);
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x40, 0x40, who)) return false;

    set32(regs, R_BE_SYS_ADIE_PAD_PWR_CTRL, B_BE_SYM_PADPDN_WL_RFC0_1P3);
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x20, 0x20, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x04, 0x04, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x08, 0x08, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x00, 0x10, who)) return false;

    if (!xtal_si_write(regs, XTAL_SI_WL_RFC_S0, 0xEB, 0xFF, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_WL_RFC_S1, 0xEB, 0xFF, who)) return false;

    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x01, 0x01, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x02, 0x02, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_ANAPAR_WL, 0x00, 0x80, who)) return false;

    if (!xtal_si_write(regs, XTAL_SI_XREF_RF1, 0x00, 0x40, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_XREF_RF2, 0x00, 0x40, who)) return false;
    if (!xtal_si_write(regs, XTAL_SI_PLL_1,    0x40, 0x60, who)) return false;

    /* 6. The isolation between the always-on island and the core.
     *
     *    Realtek skips this on the very first silicon revision.  This driver
     *    performs it unconditionally, which is the safe direction: the step is
     *    what connects the two power domains, and the revisions this system
     *    will meet are all later ones.  Said out loud because it is a
     *    deliberate difference from the reference, not an oversight. */
    set32(regs, R_BE_PMC_DBG_CTRL2, B_BE_SYSON_DIS_PMCR_BE_WRMSK);
    set32(regs, R_BE_SYS_ISO_CTRL,  B_BE_ISO_EB2CORE);
    clr32(regs, R_BE_SYS_ISO_CTRL,  B_BE_PWC_EV2EF_B);
    timer_udelay(1000);
    clr32(regs, R_BE_SYS_ISO_CTRL,  B_BE_PWC_EV2EF_S);
    clr32(regs, R_BE_PMC_DBG_CTRL2, B_BE_SYSON_DIS_PMCR_BE_WRMSK);

    /* 7. And finally the MAC's own blocks: the part that moves packets, the
     *    part shared between the two radios, and the first radio itself. */
    set32(regs, R_BE_DMAC_FUNC_EN,
          B_BE_MAC_FUNC_EN | B_BE_DMAC_FUNC_EN_BIT | B_BE_MPDU_PROC_EN |
          B_BE_WD_RLS_EN | B_BE_DLE_WDE_EN | B_BE_TXPKT_CTRL_EN |
          B_BE_STA_SCH_EN | B_BE_DLE_PLE_EN | B_BE_PKT_BUF_EN |
          B_BE_DMAC_TBL_EN | B_BE_PKT_IN_EN | B_BE_DLE_CPUIO_EN |
          B_BE_DISPATCHER_EN | B_BE_BBRPT_EN | B_BE_MAC_SEC_EN |
          B_BE_H_AXIDMA_EN | B_BE_DMAC_MLO_EN | B_BE_PLRLS_EN |
          B_BE_P_AXIDMA_EN | B_BE_DLE_DATACPUIO_EN | B_BE_LTR_CTL_EN);

    set32(regs, R_BE_CMAC_SHARE_FUNC_EN,
          B_BE_CMAC_SHARE_EN | B_BE_RESPBA_EN | B_BE_ADDRSRCH_EN |
          B_BE_BTCOEX_EN);

    set32(regs, R_BE_CMAC_FUNC_EN,
          B_BE_CMAC_EN | B_BE_CMAC_TXEN | B_BE_CMAC_RXEN |
          B_BE_SIGB_EN | B_BE_PHYINTF_EN | B_BE_CMAC_DMA_EN |
          B_BE_PTCLTOP_EN | B_BE_SCHEDULER_EN | B_BE_TMAC_EN |
          B_BE_RMAC_EN | B_BE_TXTIME_EN | B_BE_RESP_PKTCTL_EN);

    set32(regs, R_BE_FEN_RST_ENABLE,
          B_BE_FEN_BB_IP_RSTN | B_BE_FEN_BBPLAT_RSTB);

    return true;
}

/* ------------------------------------------------------------------- tests */

/* Take the header apart again, from the bytes.
 *
 * Deliberately not by reusing the shifts above: that would only prove the
 * builder is consistent with itself, which it would be even with every field
 * in the wrong place.  The bit positions are written out a second time, from
 * the same published source, and a disagreement between the two is the thing
 * worth catching.
 */
static int check_h2c_header(void) {
    u8 h[H2C_HEADER_LEN];
    int bad = 0;

    /* A firmware-header download: the request this exists to carry. */
    rtw89_h2c_header(h, H2C_CAT_MAC, H2C_CL_MAC_FWDL, H2C_FUNC_MAC_FWHDR_DL,
                     0, 0x5A, 100, true, false);

    u32 w0 = (u32)h[0] | ((u32)h[1] << 8) | ((u32)h[2] << 16) | ((u32)h[3] << 24);
    u32 w1 = (u32)h[4] | ((u32)h[5] << 8) | ((u32)h[6] << 16) | ((u32)h[7] << 24);

    struct { const char *what; u32 got; u32 want; } f[] = {
        { "category",  (w0 >> 0)  & 0x3u,    H2C_CAT_MAC },
        { "class",     (w0 >> 2)  & 0x3Fu,   H2C_CL_MAC_FWDL },
        { "function",  (w0 >> 8)  & 0xFFu,   H2C_FUNC_MAC_FWHDR_DL },
        { "delivery",  (w0 >> 16) & 0xFu,    0 },
        { "sequence",  (w0 >> 24) & 0xFFu,   0x5A },
        /* The card is told the whole packet, not the payload. */
        { "total length", w1 & 0x3FFFu,      100 + H2C_HEADER_LEN },
    };

    for (unsigned i = 0; i < ARRAY_LEN(f); i++) {
        if (f[i].got != f[i].want) {
            kwarn("rtw89", "selftest: the request header's %s field reads %u, "
                           "expected %u", f[i].what, f[i].got, f[i].want);
            bad++;
        }
    }

    if (!(w1 & (1u << 14))) {
        kwarn("rtw89", "selftest: the header did not ask to be told the "
                       "request arrived");
        bad++;
    }
    if (w1 & (1u << 15)) {
        kwarn("rtw89", "selftest: the header asked for a completion that was "
                       "not requested");
        bad++;
    }

    /* The class field is six bits wide, not three.  A build that had it at the
     * older generation's 7:5 would put this value's high bits into the
     * function field and still produce eight plausible bytes. */
    rtw89_h2c_header(h, 0, 0x3F, 0, 0, 0, 0, false, false);
    w0 = (u32)h[0] | ((u32)h[1] << 8) | ((u32)h[2] << 16) | ((u32)h[3] << 24);
    if (((w0 >> 2) & 0x3Fu) != 0x3F || ((w0 >> 8) & 0xFFu) != 0) {
        kwarn("rtw89", "selftest: a six-bit class does not fit where it should "
                       "- the fields are laid out wrongly");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the request header is laid out the way the card's "
                       "firmware reads it");
    return bad;
}

/* Catch every request a download produces and put the firmware back together
 * from them.
 *
 * This is the check that matters, and it is deliberately not a check that the
 * right number of requests went out.  A download can send the correct count
 * and still deliver the wrong bytes - a piece repeated, a piece skipped, an
 * offset that drifts by the header's length each time.  Reassembling what
 * arrived and comparing it to what was meant to arrive catches all of those at
 * once, and needs no knowledge of how the sender chose to cut it up.
 */
#define CATCH_MAX_BYTES  (64u * 1024u)

static struct {
    u8  bytes[CATCH_MAX_BYTES];
    u32 used;
    int packets;
    u32 largest;
    bool first_was_header;
    bool saw_empty;
    int  fail_after;             /* -1 never; otherwise refuse from here on */
} caught;

static bool catch_packet(void *ctx, const u8 *packet, u32 len, bool fwdl) {
    (void)ctx;

    if (caught.fail_after >= 0 && caught.packets >= caught.fail_after)
        return false;

    u32 payload = len;
    const u8 *bytes = packet;
    if (caught.packets == 0) {
        if (fwdl || len < H2C_HEADER_LEN) return false;
        u32 w0 = le32_at(packet);
        u32 w1 = le32_at(packet + 4);
        bool right_kind = ((w0 >> 0) & 0x3u) == H2C_CAT_MAC &&
                          ((w0 >> 2) & 0x3Fu) == H2C_CL_MAC_FWDL &&
                          ((w0 >> 8) & 0xFFu) == H2C_FUNC_MAC_FWHDR_DL;
        caught.first_was_header = right_kind;
        if (!right_kind || (w1 & 0x3FFFu) != len ||
            (w1 & (H2C_HDR_REC_ACK | H2C_HDR_DONE_ACK)))
            caught.saw_empty = true;
        payload -= H2C_HEADER_LEN;
        bytes += H2C_HEADER_LEN;
    } else if (!fwdl) {
        caught.saw_empty = true;
    }
    if (payload == 0) caught.saw_empty = true;
    if (payload > caught.largest) caught.largest = payload;

    if (caught.used + payload <= CATCH_MAX_BYTES) {
        memcpy(caught.bytes + caught.used, bytes, payload);
        caught.used += payload;
    }
    caught.packets++;
    return true;
}

static int check_fw_download(void) {
    int bad = 0;

    /* A container shaped like a real one: fixed and dynamic header bytes, then
     * two sections whose lengths exercise an exact split and a short tail. */
    static u8 fw[16 * 1024];
    const u32 fixed_header_len = RTW89_FW_HDR_V1_BYTES +
                                 2 * RTW89_FW_SECTION_V1_BYTES;
    const u32 dynamic_header_len = 16;
    const u32 header_len = fixed_header_len + dynamic_header_len;
    const u32 s0 = FWDL_SECTION_PER_PKT_LEN * 2;        /* exactly two pieces */
    const u32 s1 = FWDL_SECTION_PER_PKT_LEN + 37;       /* and a short tail   */

    for (u32 i = 0; i < sizeof fw; i++) fw[i] = (u8)(i * 7 + (i >> 8));
    fw[28] = (u8)FWDL_SECTION_PER_PKT_LEN;
    fw[29] = (u8)(FWDL_SECTION_PER_PKT_LEN >> 8);

    rtw89_fw_info_t info;
    memset(&info, 0, sizeof info);
    info.header_length = header_len;
    info.dynamic_header_length = dynamic_header_len;
    info.part_size = FWDL_SECTION_PER_PKT_LEN;
    info.section_count = 2;
    info.sections[0].length = s0;
    info.sections[1].length = s1;
    info.payload_bytes = s0 + s1;

    memset(&caught, 0, sizeof caught);
    caught.fail_after = -1;

    if (!rtw89_fw_download(fw, header_len + s0 + s1, &info,
                           catch_packet, NULL)) {
        kwarn("rtw89", "selftest: the download did not complete");
        return 1;
    }

    if (!caught.first_was_header) {
        kwarn("rtw89", "selftest: the container's header was not sent first");
        bad++;
    }
    if (caught.saw_empty) {
        kwarn("rtw89", "selftest: a request was empty, mislabelled, or "
                       "declared a length it did not have");
        bad++;
    }
    if (caught.largest > FWDL_SECTION_PER_PKT_LEN) {
        kwarn("rtw89", "selftest: a request carried %u bytes, more than the "
                       "%u a section piece may be",
              caught.largest, FWDL_SECTION_PER_PKT_LEN);
        bad++;
    }

    /* What arrived, put back together, against what was meant to. */
    u32 expect = fixed_header_len + s0 + s1;
    if (caught.used != expect) {
        kwarn("rtw89", "selftest: %u bytes arrived, expected %u",
              caught.used, expect);
        bad++;
    } else if (memcmp(caught.bytes, fw, fixed_header_len) != 0 ||
               memcmp(caught.bytes + fixed_header_len, fw + header_len,
                      s0 + s1) != 0) {
        kwarn("rtw89", "selftest: the dynamic header was sent or section bytes changed");
        bad++;
    }

    /* And a card that stops taking requests part way must stop the download
     * rather than carry on into a firmware with a hole in it. */
    memset(&caught, 0, sizeof caught);
    caught.fail_after = 2;
    if (rtw89_fw_download(fw, header_len + s0 + s1, &info,
                           catch_packet, NULL)) {
        kwarn("rtw89", "selftest: a refused request did not stop the download");
        bad++;
    }

    /* MSSC-bearing images require a security-efuse choice and must fail before
     * the first request, even if a caller reaches the downloader directly. */
    rtw89_fw_info_t gated = info;
    gated.sections[1].mssc_length = 32;
    gated.needs_security_profile = true;
    memset(&caught, 0, sizeof caught);
    caught.fail_after = -1;
    if (rtw89_fw_download(fw, header_len + s0 + s1 + 32, &gated,
                          catch_packet, NULL) || caught.packets != 0) {
        kwarn("rtw89", "selftest: an MSSC image reached the card without a security profile");
        bad++;
    }

    rtw89_fw_info_t bad_part = info;
    bad_part.part_size = 0;
    memset(&caught, 0, sizeof caught);
    caught.fail_after = -1;
    if (rtw89_fw_download(fw, header_len + s0 + s1, &bad_part,
                          catch_packet, NULL) || caught.packets != 0) {
        kwarn("rtw89", "selftest: invalid part size sent a partial firmware header");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the firmware is handed over whole: %u bytes rebuilt "
                       "byte for byte from the requests sent", expect);
    return bad;
}

/* The ring, driven round more than once.
 *
 * Wrapping is where ring code goes wrong, and it goes wrong quietly: a ring
 * that is filled, drained and filled again exercises the arithmetic that a
 * ring which is only ever half used never touches.  So this fills it, empties
 * it, and fills it again past the point where the indexes have wrapped, and
 * checks the descriptors' bytes each time rather than only the indexes.
 */
static int check_ring(void) {
    enum { SLOTS = 8 };
    static u8 bd[SLOTS * RTW89_PCI_BD_BYTES];
    rtw89_ring_t r;
    int bad = 0;

    memset(bd, 0, sizeof bd);
    memset(&r, 0, sizeof r);
    r.bd = bd;
    r.slots = SLOTS;

    /* An empty ring has every slot but one available. */
    if (rtw89_ring_free(&r) != SLOTS - 1) {
        kwarn("rtw89", "selftest: an empty ring offers %u slots, expected %u",
              rtw89_ring_free(&r), SLOTS - 1);
        bad++;
    }

    /* Fill it.  The last slot must be refused, not taken - taking it would
     * make a full ring read as an empty one. */
    for (int i = 0; i < SLOTS - 1; i++) {
        if (!rtw89_ring_add(&r, 0x100000u + (u32)i * 0x1000u,
                            (u32)(64 + i), i == SLOTS - 2)) {
            kwarn("rtw89", "selftest: the ring refused slot %d of %d",
                  i, SLOTS - 1);
            bad++;
        }
    }
    if (rtw89_ring_add(&r, 0x900000u, 64, true)) {
        kwarn("rtw89", "selftest: the ring took one more than it can hold - a "
                       "full ring now reads as empty");
        bad++;
    }

    /* What went into the descriptors, read back from the bytes. */
    for (int i = 0; i < SLOTS - 1; i++) {
        const u8 *d = bd + i * RTW89_PCI_BD_BYTES;
        u16 len = (u16)((u32)d[0] | ((u32)d[1] << 8));
        u16 opt = (u16)((u32)d[2] | ((u32)d[3] << 8));
        u32 dma = (u32)d[4] | ((u32)d[5] << 8) |
                  ((u32)d[6] << 16) | ((u32)d[7] << 24);

        u32 want_dma = 0x100000u + (u32)i * 0x1000u;
        bool want_ls = (i == SLOTS - 2);

        if (len != (u16)(64 + i) || dma != want_dma) {
            kwarn("rtw89", "selftest: descriptor %d says %u bytes at %08x, "
                           "expected %u at %08x", i, len, dma,
                  (unsigned)(64 + i), want_dma);
            bad++;
        }
        if (!!(opt & RTW89_PCI_TXBD_OPTION_LS) != want_ls) {
            kwarn("rtw89", "selftest: descriptor %d has the last-piece flag "
                           "%s", i, want_ls ? "missing" : "set wrongly");
            bad++;
        }
    }

    /* The card catches up, and the ring is filled again - which takes the
     * host index past the end and round. */
    r.card_index = r.host_index;
    if (rtw89_ring_free(&r) != SLOTS - 1) {
        kwarn("rtw89", "selftest: a drained ring offers %u slots, expected %u",
              rtw89_ring_free(&r), SLOTS - 1);
        bad++;
    }

    u16 before = r.host_index;
    for (int i = 0; i < SLOTS - 1; i++)
        if (!rtw89_ring_add(&r, 0x200000u + (u32)i * 0x1000u, 128, false)) {
            kwarn("rtw89", "selftest: the ring refused slot %d after wrapping",
                  i);
            bad++;
        }

    /* It must have gone round rather than off the end. */
    if (r.host_index >= SLOTS) {
        kwarn("rtw89", "selftest: the host index ran to %u past %u slots",
              r.host_index, SLOTS);
        bad++;
    }
    if (r.host_index != (u16)((before + SLOTS - 1) % SLOTS)) {
        kwarn("rtw89", "selftest: after wrapping the index is %u, expected %u",
              r.host_index, (u16)((before + SLOTS - 1) % SLOTS));
        bad++;
    }

    /* And what cannot be described must be refused rather than truncated. */
    u8 one[RTW89_PCI_BD_BYTES];
    if (rtw89_bd_write(one, 0x1FFFFFFFFull, 64, true)) {
        kwarn("rtw89", "selftest: an address above four gigabytes was accepted "
                       "and would have been truncated");
        bad++;
    }
    if (rtw89_bd_write(one, 0x1000, 0x10000, true)) {
        kwarn("rtw89", "selftest: a length too large for the field was "
                       "accepted");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the ring to the card wraps correctly and refuses what "
                       "its descriptors cannot describe");
    return bad;
}

/* Handing the ring over, checked at the registers.
 *
 * The address goes across as two halves and the high one is the trap: a ring
 * allocated below four gigabytes has a high half of zero, so a driver that
 * never writes it works perfectly on any machine with little memory and points
 * the card at the wrong place on a machine with a lot.  The ring used here sits
 * deliberately above four gigabytes so that omission cannot pass.
 */
static int check_ring_handover(volatile u8 *regs) {
    static u8 bd[8 * RTW89_PCI_BD_BYTES];
    rtw89_ring_t r;
    int bad = 0;

    memset(&r, 0, sizeof r);
    r.bd = bd;
    r.slots = 8;
    r.bd_phys = 0x1DEADB000ull;            /* above four gigabytes on purpose */

    /* Something left behind, so that "written" and "already zero" cannot be
     * confused for one another. */
    wr32(regs, R_BE_CH12_TXBD_DESA_L, 0xA5A5A5A5u);
    wr32(regs, R_BE_CH12_TXBD_DESA_H, 0xA5A5A5A5u);
    wr32(regs, R_BE_CH12_TXBD_NUM, 0xA5A5A5A5u);
    wr32(regs, R_BE_CH12_TXBD_IDX, 0xA5A5A5A5u);

    if (!rtw89_ring_attach(regs, &r, R_BE_CH12_TXBD_DESA_L,
                           R_BE_CH12_TXBD_DESA_H, R_BE_CH12_TXBD_NUM,
                           R_BE_CH12_TXBD_IDX)) {
        kwarn("rtw89", "selftest: the ring would not attach");
        return 1;
    }

    u32 lo = rd32(regs, R_BE_CH12_TXBD_DESA_L);
    u32 hi = rd32(regs, R_BE_CH12_TXBD_DESA_H);
    u32 num = rd32(regs, R_BE_CH12_TXBD_NUM);
    u32 idx = rd32(regs, R_BE_CH12_TXBD_IDX);

    if (lo != (u32)r.bd_phys) {
        kwarn("rtw89", "selftest: the ring's low address reads %08x, expected "
                       "%08x", lo, (u32)r.bd_phys);
        bad++;
    }
    if (hi != (u32)(r.bd_phys >> 32)) {
        kwarn("rtw89", "selftest: the ring's high address reads %08x, expected "
                       "%08x - a ring above four gigabytes would be pointed at "
                       "the wrong place", hi, (u32)(r.bd_phys >> 32));
        bad++;
    }
    if (num != r.slots) {
        kwarn("rtw89", "selftest: the card was told %u slots, expected %u",
              num, r.slots);
        bad++;
    }
    if (idx != 0) {
        kwarn("rtw89", "selftest: the host index started at %u rather than "
                       "nothing", idx);
        bad++;
    }

    /* Adding work does not by itself tell the card: the descriptors go in
     * first and the index afterwards, and only the doorbell moves it. */
    rtw89_ring_add(&r, 0x300000, 128, true);
    rtw89_ring_add(&r, 0x301000, 128, true);

    if (rd32(regs, R_BE_CH12_TXBD_IDX) != 0) {
        kwarn("rtw89", "selftest: adding to the ring moved the card's index "
                       "before the descriptors were finished");
        bad++;
    }

    rtw89_ring_doorbell(regs, &r, R_BE_CH12_TXBD_IDX);

    if (rd32(regs, R_BE_CH12_TXBD_IDX) != r.host_index) {
        kwarn("rtw89", "selftest: the doorbell left the index at %u, expected "
                       "%u", rd32(regs, R_BE_CH12_TXBD_IDX), r.host_index);
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the ring is handed to the card whole - both halves of "
                       "its address - and only the doorbell moves the index");
    return bad;
}

/* Replies, including the ones a card that has gone wrong would send.
 *
 * A request is built by this driver and can be trusted; a reply is not, and
 * the length in it is a number the card chose.  The malformed cases below are
 * not hypothetical politeness - a card that has crashed puts whatever was in
 * its memory on the ring, and both of these turn a small wrong number into a
 * read somewhere else entirely.
 */
static int check_c2h(void) {
    int bad = 0;
    static u8 reply[64];
    rtw89_c2h_t got;

    /* A well-formed reply: category 1, class 3, function 7, with eight bytes
     * of payload after the header. */
    memset(reply, 0, sizeof reply);
    u32 w0 = (1u << H2C_HDR_CAT_SHIFT) | (3u << H2C_HDR_CLASS_SHIFT) |
             (7u << H2C_HDR_FUNC_SHIFT);
    u32 w1 = H2C_HEADER_LEN + 8;
    reply[0] = (u8)w0; reply[1] = (u8)(w0 >> 8);
    reply[2] = (u8)(w0 >> 16); reply[3] = (u8)(w0 >> 24);
    reply[4] = (u8)w1; reply[5] = (u8)(w1 >> 8);
    reply[6] = (u8)(w1 >> 16); reply[7] = (u8)(w1 >> 24);
    for (int i = 0; i < 8; i++) reply[H2C_HEADER_LEN + i] = (u8)(0xC0 + i);

    if (!rtw89_c2h_parse(reply, H2C_HEADER_LEN + 8, &got)) {
        kwarn("rtw89", "selftest: a well-formed reply was rejected");
        bad++;
    } else {
        if (got.category != 1 || got.cls != 3 || got.func != 7) {
            kwarn("rtw89", "selftest: a reply read as category %u class %u "
                           "function %u, expected 1/3/7",
                  got.category, got.cls, got.func);
            bad++;
        }
        if (got.payload_len != 8) {
            kwarn("rtw89", "selftest: the reply's payload came out %u bytes, "
                           "expected 8", got.payload_len);
            bad++;
        } else if (got.payload[0] != 0xC0 || got.payload[7] != 0xC7) {
            kwarn("rtw89", "selftest: the reply's payload starts in the wrong "
                           "place");
            bad++;
        }
    }

    /* Shorter than its own header.  Subtracting the header from this wraps,
     * and a driver that then reads that many bytes walks the whole address
     * space. */
    reply[4] = 3; reply[5] = 0;
    if (rtw89_c2h_parse(reply, H2C_HEADER_LEN + 8, &got)) {
        kwarn("rtw89", "selftest: a reply claiming to be 3 bytes was accepted");
        bad++;
    }

    /* Longer than what arrived. */
    w1 = 4096;
    reply[4] = (u8)w1; reply[5] = (u8)(w1 >> 8);
    if (rtw89_c2h_parse(reply, H2C_HEADER_LEN + 8, &got)) {
        kwarn("rtw89", "selftest: a reply claiming 4096 bytes was accepted "
                       "when only %u arrived", H2C_HEADER_LEN + 8);
        bad++;
    }

    /* And a truncated arrival, with no header at all. */
    if (rtw89_c2h_parse(reply, 4, &got)) {
        kwarn("rtw89", "selftest: four bytes were parsed as a reply");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the card's replies are read correctly, and a reply "
                       "that lies about its own length is refused");
    return bad;
}

/* Does the driver believe the card about its own firmware?
 *
 * Three outcomes matter and they are not interchangeable: it started, it was
 * refused, or nothing happened.  A driver that treats the second as the third
 * waits out its whole timeout and then reports a timeout - which sends whoever
 * reads the log looking for a slow card instead of a rejected image.
 */
static int check_fw_ready(volatile u8 *regs) {
    int bad = 0;

    /* Refused, by each of the card's own numbers.  These have to come back as
     * refusals promptly, not as timeouts. */
    static const struct { u8 card; const char *what; } refusals[] = {
        { 4, "a bad checksum" },
        { 5, "a signature it will not accept" },
        { 6, "a signature it will not accept" },
        { 7, "an image for different silicon" },
    };

    for (unsigned i = 0; i < ARRAY_LEN(refusals); i++) {
        rtw89_model_firmware_refused(refusals[i].card);

        u64 began = g_uptime_ms;
        bool ok = rtw89_fw_wait_ready(regs, 3000, "the model");
        u64 took = g_uptime_ms - began;

        if (ok) {
            kwarn("rtw89", "selftest: the card said %s and the driver read it "
                           "as the firmware running", refusals[i].what);
            bad++;
        } else if (took > 500) {
            kwarn("rtw89", "selftest: the card said %s straight away and the "
                           "driver waited %llu ms before saying so",
                  refusals[i].what, (unsigned long long)took);
            bad++;
        }
    }

    /* Nothing happening at all must time out rather than hang, and must not be
     * mistaken for either of the other two. */
    rtw89_model_firmware_refused(1);            /* still in progress */
    u64 began = g_uptime_ms;
    if (rtw89_fw_wait_ready(regs, 50, "the model")) {
        kwarn("rtw89", "selftest: a download still in progress was read as "
                       "finished");
        bad++;
    }
    if (g_uptime_ms - began > 2000) {
        kwarn("rtw89", "selftest: waiting for firmware overran its bound");
        bad++;
    }

    /* And the one that should work. */
    rtw89_model_firmware_started();
    if (!rtw89_fw_wait_ready(regs, 3000, "the model")) {
        kwarn("rtw89", "selftest: the card said the firmware was running and "
                       "the driver did not agree");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the card is believed about its own firmware: running "
                       "is running, and each way it says no is reported as "
                       "that rather than as a timeout");
    return bad;
}

/* The conversation with the radio, driven against a model of it.
 *
 * Reading a radio register is three steps in an order that matters, and the
 * ways of getting it wrong all return a number rather than an error - which is
 * why this is checked against something that can object rather than against
 * whether the driver crashed.
 */
void rtw89_model_radio_reset(void);
u32  rtw89_model_radio_peek(int path, u32 addr);
int  rtw89_model_radio_writes(void);
bool rtw89_model_radio_misused(void);

static int check_radio(volatile u8 *regs) {
    int bad = 0;
    rtw89_model_radio_reset();

    /* Both chains: the two radios have separate windows and a driver that
     * addresses one and reads the other looks completely correct on a chip
     * where they happen to hold the same thing. */
    for (int path = 0; path < RTW89_RF_PATHS; path++) {
        u32 want = rtw89_model_radio_peek(path, 0x18);
        u32 got = rtw89_rf_read(regs, path, 0x18);
        if (got != want) {
            kwarn("rtw89", "selftest: radio %d register 18 read as %05x, "
                           "expected %05x", path, got, want);
            bad++;
        }
    }

    /* Two different registers in a row, which is where a driver that leaves
     * the interface polling gets the first answer twice. */
    u32 a = rtw89_rf_read(regs, RTW89_RF_PATH_A, 0x05);
    u32 b = rtw89_rf_read(regs, RTW89_RF_PATH_A, 0x06);
    if (a == b) {
        kwarn("rtw89", "selftest: two different radio registers both read as "
                       "%05x - the second answer is the first one again", a);
        bad++;
    }

    /* Writing, and reading back what was written. */
    /* Written, then read back THROUGH THE DRIVER rather than looked at
     * directly in the model.
     *
     * Peeking was the first version and it failed for a reason worth keeping:
     * the model only moves when the driver touches the registers, and a write
     * followed by nothing touches nothing.  That is also true of the hardware
     * - a write is taken when the interface next gets round to it - so the
     * honest test is the one that asks for it back the way anything else
     * would. */
    if (!rtw89_rf_write(regs, RTW89_RF_PATH_B, 0x10, 0x5A5A5)) {
        kwarn("rtw89", "selftest: the radio would not take a write");
        bad++;
    } else {
        u32 back = rtw89_rf_read(regs, RTW89_RF_PATH_B, 0x10);
        if (back != 0x5A5A5) {
            kwarn("rtw89", "selftest: %05x was written to the radio and %05x "
                           "came back", 0x5A5A5, back);
            bad++;
        }
    }

    /* A value wider than the register must not spill into the address. */
    if (rtw89_rf_write(regs, RTW89_RF_PATH_A, 0x11, 0xFFFFFFFFu)) {
        u32 back = rtw89_rf_read(regs, RTW89_RF_PATH_A, 0x11);
        if (back != RTW89_RF_MASK) {
            kwarn("rtw89", "selftest: an oversized value came back as %05x "
                           "rather than being cut to the register width", back);
            bad++;
        }
    }

    if (rtw89_model_radio_misused()) {
        kwarn("rtw89", "selftest: the radio was addressed while it was still "
                       "busy with the request before");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the radio answers: both chains read, two registers in "
                       "a row give two answers, and a write comes back");
    return bad;
}

static u32 rf_encode(u32 value, u32 mask);
static int check_calibration(volatile u8 *regs, rtw89_channel_t *ch);
static u32 get32(const u8 *at);

/* Tuning, driven against the model.
 *
 * The mistake this exists to catch is a quiet one.  The channel goes into two
 * registers per path, 0x18 and 0x10018, and bit 16 of that second address is
 * not part of the address at all - it says which of the radio's two interfaces
 * the request travels over.  A driver that masks the address down to the eight
 * bits the serial interface holds turns both into "register 0x18", writes the
 * channel to the same place twice, and leaves the other interface untouched.
 *
 * Nothing about that looks wrong.  Every write is accepted, every read back
 * agrees, and the card is half tuned.  So the first thing checked here is that
 * the two addresses are two registers.
 */
static int check_channel(volatile u8 *regs) {
    int bad = 0;

    /* Two addresses, two registers.  Distinct values, both read back. */
    rtw89_rf_write(regs, RTW89_RF_PATH_A, RR_CFGCH,    0x11111);
    rtw89_rf_write(regs, RTW89_RF_PATH_A, RR_CFGCH_V1, 0x22222);
    u32 serial = rtw89_rf_read(regs, RTW89_RF_PATH_A, RR_CFGCH);
    u32 direct = rtw89_rf_read(regs, RTW89_RF_PATH_A, RR_CFGCH_V1);
    if (serial != 0x11111 || direct != 0x22222) {
        kwarn("rtw89", "selftest: the radio's two interfaces read back %05x "
                       "and %05x rather than %05x and %05x - bit 16 of the "
                       "address is being treated as part of the address",
              serial, direct, 0x11111, 0x22222);
        bad++;
    }

    /* A bit outside the fields tuning owns, to see whether it survives.  The
     * register holds settings this has no business changing, and a write that
     * replaces the whole word instead of four fields would pass every check
     * about the channel and break the ones nobody is looking at. */
    const u32 keep = 1u << 19;
    for (int path = 0; path < RTW89_RF_PATHS; path++) {
        rtw89_rf_write(regs, path, RR_CFGCH,    keep);
        rtw89_rf_write(regs, path, RR_CFGCH_V1, keep);
    }

    /* 2.4 GHz, where both band fields are zero - so this alone cannot tell a
     * driver that sets them from one that never touches them. */
    if (!rtw89_set_channel(regs, 6, RTW89_BAND_2G, false)) {
        kwarn("rtw89", "selftest: the radio would not tune to channel 6");
        bad++;
    }

    static const u32 addr[2] = { RR_CFGCH, RR_CFGCH_V1 };
    for (int path = 0; path < RTW89_RF_PATHS; path++) {
        for (int i = 0; i < 2; i++) {
            u32 v = rtw89_rf_read(regs, path, addr[i]);
            u32 ch = v & RR_CFGCH_CH;
            if (ch != 6) {
                kwarn("rtw89", "selftest: path %d %s holds channel %u after "
                               "tuning to 6", path, i ? "0x10018" : "0x18", ch);
                bad++;
            }
            if (!(v & keep)) {
                kwarn("rtw89", "selftest: tuning path %d cleared a bit it does "
                               "not own", path);
                bad++;
            }
        }
    }

    /* And 5 GHz, which is the case that needs both band fields set.  Going
     * straight from 2.4 GHz means a driver that only ever ORs - never clearing
     * what was there - still passes; going back afterwards is what catches it.
     */
    if (!rtw89_set_channel(regs, 36, RTW89_BAND_5G, false)) {
        kwarn("rtw89", "selftest: the radio would not tune to channel 36");
        bad++;
    }

    u32 five = rtw89_rf_read(regs, RTW89_RF_PATH_A, RR_CFGCH);
    if ((five & RR_CFGCH_CH) != 36 ||
        (five & RR_CFGCH_BAND1) != rf_encode(CFGCH_BAND1_5G, RR_CFGCH_BAND1) ||
        (five & RR_CFGCH_BAND0) != rf_encode(CFGCH_BAND0_5G, RR_CFGCH_BAND0)) {
        kwarn("rtw89", "selftest: channel 36 at 5 GHz came out as %05x", five);
        bad++;
    }

    /* Back down.  The band fields have to return to zero, and a driver that
     * only sets bits leaves them saying 5 GHz on a 2.4 GHz channel. */
    rtw89_set_channel(regs, 1, RTW89_BAND_2G, false);
    u32 back = rtw89_rf_read(regs, RTW89_RF_PATH_A, RR_CFGCH);
    if ((back & RR_CFGCH_BAND1) || (back & RR_CFGCH_BAND0)) {
        kwarn("rtw89", "selftest: after returning to 2.4 GHz the radio still "
                       "says 5 GHz (%05x) - the band is being set but never "
                       "cleared", back);
        bad++;
    }
    if ((back & RR_CFGCH_CH) != 1) {
        kwarn("rtw89", "selftest: channel 1 came out as %05x", back);
        bad++;
    }

    /* Channel zero is a mistake somewhere above, not a channel. */
    if (rtw89_set_channel(regs, 0, RTW89_BAND_2G, false)) {
        kwarn("rtw89", "selftest: the driver accepted channel 0");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the radio tunes: both interfaces are separate "
                       "registers, both chains follow, the band clears going "
                       "back down, and bits it does not own survive");
    return bad;
}

/* The descriptors, checked as arithmetic.
 *
 * Nothing here needs a card, and that is the point: a descriptor built wrongly
 * is completely invisible on hardware.  The card reads it, does something
 * plausible with it - sends the frame at the wrong length, puts it on the
 * wrong queue, or drops it - and reports success either way.  So the part that
 * can be checked exactly is checked exactly.
 */
static int check_descriptors(void) {
    int bad = 0;
    u8 d[RTW89_TXD_BYTES + 8];

    rtw89_txd_t how;
    memset(&how, 0, sizeof how);
    how.bytes    = 200;
    how.qsel     = RTW89_TX_QSEL_B0_MGMT;
    how.channel  = RTW89_TXCH_CH8;
    how.sequence = 0x123;
    how.mac_id   = 7;

    u32 n = rtw89_txd_fill(d, sizeof d, &how);
    if (n != RTW89_TXD_BYTES) {
        kwarn("rtw89", "selftest: a descriptor came out %u bytes rather than "
                       "%u", n, (u32)RTW89_TXD_BYTES);
        bad++;
    } else {
        u32 w0 = get32(d + 0), w1 = get32(d + 4);
        u32 w2 = get32(d + 8), w3 = get32(d + 12);

        if ((w2 & BE_TXD_BODY2_TXPKTSIZE) != 200) {
            kwarn("rtw89", "selftest: the descriptor says %u bytes rather "
                           "than 200", w2 & BE_TXD_BODY2_TXPKTSIZE);
            bad++;
        }
        if (((w2 & BE_TXD_BODY2_QSEL) >> 17) != RTW89_TX_QSEL_B0_MGMT) {
            kwarn("rtw89", "selftest: the descriptor names the wrong queue");
            bad++;
        }
        if (((w0 & BE_TXD_BODY0_CH_DMA) >> 16) != RTW89_TXCH_CH8) {
            kwarn("rtw89", "selftest: the descriptor names the wrong ring - "
                           "the card checks this against the queue");
            bad++;
        }
        if ((w3 & BE_TXD_BODY3_WIFI_SEQ) != 0x123) {
            kwarn("rtw89", "selftest: the sequence number did not survive");
            bad++;
        }
        if (!(w0 & BE_TXD_BODY0_WDINFO_EN)) {
            kwarn("rtw89", "selftest: the second half of the descriptor is "
                           "written but not announced, so the card reads the "
                           "frame where the descriptor still is");
            bad++;
        }
        /* Where the card is told the frame begins, in eight-byte units. */
        if (((w0 & BE_TXD_BODY0_WP_OFFSET_V1) >> 24) * 8 != RTW89_TXD_BYTES) {
            kwarn("rtw89", "selftest: the frame's offset does not match the "
                           "descriptor's own length");
            bad++;
        }
        if (((w1 & BE_TXD_BODY1_ADDR_INFO_NUM) >> 26) != 1) {
            kwarn("rtw89", "selftest: the descriptor claims no address region, "
                           "so it points at nothing");
            bad++;
        }
        /* A single recipient gets an RTS asked for; a broadcast has nobody to
         * answer one. */
        if (!(get32(d + RTW89_TXWD_BODY_BYTES + 16) & BE_TXD_INFO4_RTS_EN)) {
            kwarn("rtw89", "selftest: no RTS was asked for on a unicast frame");
            bad++;
        }
    }

    how.broadcast = true;
    rtw89_txd_fill(d, sizeof d, &how);
    if (get32(d + RTW89_TXWD_BODY_BYTES + 16) & BE_TXD_INFO4_RTS_EN) {
        kwarn("rtw89", "selftest: an RTS was asked for on a broadcast, which "
                       "nobody can answer");
        bad++;
    }

    /* A frame too long for the length field must be refused, not cut down. */
    how.bytes = BE_TXD_BODY2_TXPKTSIZE + 1;
    if (rtw89_txd_fill(d, sizeof d, &how)) {
        kwarn("rtw89", "selftest: an oversized frame was described rather "
                       "than refused");
        bad++;
    }
    how.bytes = 0;
    if (rtw89_txd_fill(d, sizeof d, &how)) {
        kwarn("rtw89", "selftest: an empty frame was described");
        bad++;
    }

    /* --- and the other direction ------------------------------------- */

    /* One of each area, so that using a single unit for all four is caught.
     * The units differ - two bytes, eight, sixteen, eight - and one of each
     * gives 2 + 8 + 16 + 8 = 34 past the descriptor.  Treating them all as
     * eight-byte would give 32, which is close enough to look right and wrong
     * enough to hand up a frame that starts two bytes late. */
    u8 r[128];
    memset(r, 0, sizeof r);
    u32 w0 = (60u & BE_RXD_RPKT_LEN_MASK) |
             (1u << 14) |          /* shift:    1 x 2  */
             (1u << 18) |          /* drv info: 1 x 8  */
             (1u << 20) |          /* hdr cnv:  1 x 16 */
             (1u << 22);           /* phy rpt:  1 x 8  */
    put32(r + 0, w0);
    put32(r + 12, BE_RXD_A1_MATCH);

    rtw89_rxd_t got;
    if (!rtw89_rxd_parse(r, sizeof r, &got)) {
        kwarn("rtw89", "selftest: a valid receive descriptor was refused");
        bad++;
    } else {
        u32 want = RTW89_RXD_SHORT_BYTES + 8 + 8 + 16 + 2;
        if (got.frame_at != want) {
            kwarn("rtw89", "selftest: the frame is said to start at %u rather "
                           "than %u - the four areas in front of it are "
                           "counted in different units",
                  got.frame_at, want);
            bad++;
        }
        if (got.frame_bytes != 60) {
            kwarn("rtw89", "selftest: the frame is said to be %u bytes rather "
                           "than 60", got.frame_bytes);
            bad++;
        }
        if (!got.addressed_to_us) {
            kwarn("rtw89", "selftest: a frame addressed to us did not say so");
            bad++;
        }
        if (got.type != RTW89_RX_TYPE_WIFI) {
            kwarn("rtw89", "selftest: a frame off the air was typed as %u",
                  got.type);
            bad++;
        }
    }

    /* A descriptor that describes more than arrived.  This is the one that
     * turns a confused card into a read past the end of a buffer. */
    put32(r + 0, (u32)(BE_RXD_RPKT_LEN_MASK & 9000u));
    if (rtw89_rxd_parse(r, 64, &got)) {
        kwarn("rtw89", "selftest: a descriptor claiming more than arrived was "
                       "accepted");
        bad++;
    }
    /* And one whose preamble alone runs past the end. */
    memset(r, 0, sizeof r);
    put32(r + 0, 8u | (3u << 18) | (3u << 20) | (3u << 22));
    if (rtw89_rxd_parse(r, 40, &got)) {
        kwarn("rtw89", "selftest: a descriptor whose header runs past the end "
                       "was accepted");
        bad++;
    }
    /* Too little to hold even the descriptor. */
    if (rtw89_rxd_parse(r, 8, &got)) {
        kwarn("rtw89", "selftest: a truncated descriptor was read anyway");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "frames are described correctly in both directions: "
                       "length, queue and ring agree, oversized frames are "
                       "refused, and an incoming frame is found past all four "
                       "of the areas in front of it");
    return bad;
}

/* A frame, all the way out.
 *
 * Every piece under this has been checked on its own: the descriptor as
 * arithmetic, the ring against the card, the doorbell.  This is the first time
 * they are used together, and the join is where the failures live - the
 * descriptor correct, the ring entry correct, and the frame left somewhere the
 * card does not look.
 *
 * The model does not take the frame from a fixed offset.  It reads the
 * descriptor, works out where the descriptor SAYS the frame is, and takes it
 * from there.  So the check below is not "did something arrive" but "did the
 * card find the frame by following what the driver told it".
 */
int  rtw89_model_frames_sent(void);
bool rtw89_model_frame_length_disagreed(void);
bool rtw89_model_frame_on_wrong_ring(void);
bool rtw89_model_frame_not_where_promised(void);
u32  rtw89_model_last_frame(u8 *out, u32 cap);
void rtw89_model_frames_reset(void);

static int check_transmit(volatile u8 *regs) {
    int bad = 0;
    rtw89_model_frames_reset();

    static rtw89_data_t data;
    static u8 *ring, *buffer;
    static u64 ring_phys, buffer_phys;

    if (!ring) {
        /* The card is pointed at both of these, so neither may be a static
         * array: the kernel image is not in the direct map and its physical
         * address is not what virt_to_phys would say. */
        ring = dma_alloc_pages(1, &ring_phys);
        buffer = dma_alloc_pages(1, &buffer_phys);
    }
    if (!ring || !buffer) {
        kwarn("rtw89", "selftest: no memory the card could be pointed at");
        return 1;
    }

    if (!rtw89_data_attach(regs, &data, ring, ring_phys, 64,
                           buffer, buffer_phys, 4096)) {
        kwarn("rtw89", "selftest: the card would not take a ring for frames");
        return 1;
    }

    /* Something recognisable, and long enough that a length taken from the
     * wrong field would not accidentally match. */
    u8 frame[100];
    for (unsigned i = 0; i < sizeof frame; i++) frame[i] = (u8)(0x40 + i);

    int n = rtw89_data_transmit(regs, &data, frame, (int)sizeof frame, false);
    if (n != (int)sizeof frame) {
        kwarn("rtw89", "selftest: a frame of %u bytes was not sent (%d)",
              (unsigned)sizeof frame, n);
        bad++;
    }

    /* The card has to have fetched it.  Reading a register is what gives the
     * model its moment to act, the same as everywhere else here. */
    for (int spent = 0; spent < 50 && !rtw89_model_frames_sent(); spent++) {
        (void)rtw89_ring_card_index(regs, R_BE_CH8_TXBD_IDX);
        timer_mdelay(1);
    }

    if (!rtw89_model_frames_sent()) {
        kwarn("rtw89", "selftest: the card never fetched the frame");
        bad++;
    }
    if (rtw89_model_frame_length_disagreed()) {
        kwarn("rtw89", "selftest: the ring entry and the descriptor disagree "
                       "about how long the frame is");
        bad++;
    }
    if (rtw89_model_frame_on_wrong_ring()) {
        kwarn("rtw89", "selftest: the descriptor names a different ring from "
                       "the one it was put on");
        bad++;
    }
    if (rtw89_model_frame_not_where_promised()) {
        kwarn("rtw89", "selftest: the frame is not where the descriptor says");
        bad++;
    }

    u8 back[sizeof frame];
    u32 got = rtw89_model_last_frame(back, sizeof back);
    if (got != sizeof frame) {
        kwarn("rtw89", "selftest: the card found %u bytes where the frame "
                       "should be, not %u", got, (unsigned)sizeof frame);
        bad++;
    } else if (memcmp(back, frame, sizeof frame) != 0) {
        kwarn("rtw89", "selftest: what the card fetched is not what was sent");
        bad++;
    }

    /* A second frame, so that a ring which only ever works once is caught -
     * and the sequence number has to have moved on. */
    u16 first_seq = data.sequence;
    if (rtw89_data_transmit(regs, &data, frame, 40, true) != 40) {
        kwarn("rtw89", "selftest: a second frame was refused");
        bad++;
    }
    for (int spent = 0; spent < 50 && rtw89_model_frames_sent() < 2; spent++) {
        (void)rtw89_ring_card_index(regs, R_BE_CH8_TXBD_IDX);
        timer_mdelay(1);
    }
    if (rtw89_model_frames_sent() < 2) {
        kwarn("rtw89", "selftest: the ring carried one frame and then stopped");
        bad++;
    }
    if (data.sequence == first_seq) {
        kwarn("rtw89", "selftest: two frames went out under the same sequence "
                       "number");
        bad++;
    }
    if (rtw89_model_last_frame(NULL, 0) != 40) {
        kwarn("rtw89", "selftest: the second frame arrived at the first "
                       "frame's length");
        bad++;
    }

    /* A frame too big for the buffer must be refused rather than written past
     * the end of it. */
    static u8 huge[5000];
    if (rtw89_data_transmit(regs, &data, huge, (int)sizeof huge, false) != -1) {
        kwarn("rtw89", "selftest: a frame larger than the buffer was accepted");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "a frame reaches the card: it was fetched by following "
                       "the descriptor, arrived byte for byte, and a second "
                       "one followed it under its own sequence number");
    return bad;
}

/* Frames arriving.
 *
 * The card is made to deliver, and what comes out the top is compared with
 * what went in.  Three things this catches that nothing above it can:
 *
 *   The frame does not start where the descriptor ends.  The model always puts
 *   a radio report in between, so a receive path that reads from a fixed
 *   offset gets eight bytes of the wrong thing and every frame is corrupt in
 *   the same quiet way.
 *
 *   The card talks about itself on the same ring.  Transmit reports and
 *   command answers arrive as receive descriptors with a different type, and
 *   handing one up as a frame corrupts the stack above.
 *
 *   A buffer read and not given back is gone.  Sixteen frames later there is
 *   nowhere to put anything, and the symptom is a network that works briefly
 *   after every restart.
 */
bool rtw89_model_inject_frame(const u8 *frame, u32 len, u8 type,
                              bool crc_error);

static u8  rx_seen[512];
static u32 rx_seen_len;
static int rx_seen_count;

static s8 rx_seen_signal;

static void note_rx_frame(void *ctx, const u8 *frame, u32 len,
                          s8 signal_dbm) {
    (void)ctx;
    rx_seen_signal = signal_dbm;
    rx_seen_count++;
    rx_seen_len = len;
    if (len > sizeof rx_seen) len = sizeof rx_seen;
    memcpy(rx_seen, frame, len);
}

static int check_frame_receive(volatile u8 *regs) {
    int bad = 0;

    static rtw89_rx_path_t path;
    static u8 *ring, *pool;
    static u64 ring_phys, pool_phys;

    if (!ring) {
        ring = dma_alloc_pages(1, &ring_phys);
        /* Sixteen buffers of two kilobytes: eight pages. */
        pool = dma_alloc_pages(8, &pool_phys);
    }
    if (!ring || !pool) {
        kwarn("rtw89", "selftest: no memory the card could be pointed at");
        return 1;
    }

    if (!rtw89_rx_attach(regs, &path, ring, ring_phys, pool, pool_phys,
                         RTW89_RX_BUFFERS, RTW89_RX_BUFFER_BYTES)) {
        kwarn("rtw89", "selftest: the card would not take receive buffers");
        return 1;
    }

    u8 frame[120];
    for (unsigned i = 0; i < sizeof frame; i++) frame[i] = (u8)(0x80 ^ i);

    rx_seen_count = 0;
    rx_seen_len = 0;

    if (!rtw89_model_inject_frame(frame, sizeof frame, RTW89_RX_TYPE_WIFI,
                                  false)) {
        kwarn("rtw89", "selftest: the card had nowhere to put a frame, so no "
                       "buffers were posted before it arrived");
        bad++;
    }

    int n = rtw89_rx_poll(regs, &path, note_rx_frame, NULL);
    if (n != 1) {
        kwarn("rtw89", "selftest: %d frame(s) came up rather than one", n);
        bad++;
    } else if (rx_seen_len != sizeof frame) {
        kwarn("rtw89", "selftest: a frame of %u bytes came up as %u",
              (unsigned)sizeof frame, rx_seen_len);
        bad++;
    } else if (memcmp(rx_seen, frame, sizeof frame) != 0) {
        kwarn("rtw89", "selftest: the frame that came up is not the one that "
                       "arrived - it was read from the wrong offset");
        bad++;
    }

    /* And how strong it was.  The card reports this in half-decibel steps
     * above a floor of -110, so a raw 100 is -60 dBm.  A driver that hands the
     * raw number up reports every network as impossibly strong, and one that
     * forgets the sign reports them all as impossibly weak - both look like
     * numbers rather than like a bug. */
    if (rx_seen_signal != -60) {
        kwarn("rtw89", "selftest: a signal the card reported as 100 came up "
                       "as %d dBm rather than -60", rx_seen_signal);
        bad++;
    }

    /* The card talking about itself must not be handed up. */
    int before = path.frames;
    rtw89_model_inject_frame(frame, 40, RTW89_RX_TYPE_C2H, false);
    rtw89_rx_poll(regs, &path, note_rx_frame, NULL);
    if (path.frames != before) {
        kwarn("rtw89", "selftest: the card's own report was handed up as a "
                       "frame off the air");
        bad++;
    }
    if (!path.not_frames) {
        kwarn("rtw89", "selftest: the card's own report was not recognised "
                       "as one");
        bad++;
    }

    /* Damaged in the air: counted, not delivered, and not confused with a
     * frame that could not be read. */
    before = path.frames;
    rtw89_model_inject_frame(frame, 60, RTW89_RX_TYPE_WIFI, true);
    rtw89_rx_poll(regs, &path, note_rx_frame, NULL);
    if (path.frames != before) {
        kwarn("rtw89", "selftest: a frame that failed its checksum was handed "
                       "up anyway");
        bad++;
    }
    if (path.crc_errors != 1) {
        kwarn("rtw89", "selftest: a damaged frame was not counted as one");
        bad++;
    }
    if (path.unreadable) {
        kwarn("rtw89", "selftest: %d frame(s) could not be read at all",
              path.unreadable);
        bad++;
    }

    /* And more frames than there are buffers, one at a time, which is the
     * arrangement where a buffer that is never given back runs out. */
    int delivered = 0;
    for (int i = 0; i < RTW89_RX_BUFFERS * 3; i++) {
        if (!rtw89_model_inject_frame(frame, 64, RTW89_RX_TYPE_WIFI, false)) {
            kwarn("rtw89", "selftest: the card ran out of buffers after %d "
                           "frame(s) - they are not being given back", i);
            bad++;
            break;
        }
        delivered += rtw89_rx_poll(regs, &path, note_rx_frame, NULL);
    }
    if (delivered != RTW89_RX_BUFFERS * 3) {
        kwarn("rtw89", "selftest: %d of %d frames came up", delivered,
              RTW89_RX_BUFFERS * 3);
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "frames arrive: one came up byte for byte from past "
                       "the radio report, the card's own reports were kept "
                       "out, a damaged one was counted not delivered, its "
                       "strength came up as %d dBm, and %d frames went "
                       "through %d buffers",
              rx_seen_signal, delivered, RTW89_RX_BUFFERS);
    return bad;
}

/* Calibrating the radio, through the card's own processor.
 *
 * Two things are being checked and they are different.
 *
 * That the exchange works at all: a calibration is asked for on one class and
 * reported on ANOTHER, so a mailbox that waits for the class it asked on waits
 * forever.  That is not a hypothetical - it is what the mailbox did until this
 * was written, because every other command on this card replies in kind.
 *
 * And that it is asked for in the right order.  The receiver's standing level
 * is measured wherever the radio is pointed, so asking before tuning measures
 * the wrong band.  The card is the only thing that can tell, so the model
 * tells - by failing the request, which is what the silicon does.
 */
bool rtw89_model_rfk_asked(u8 func);
bool rtw89_model_rfk_wrong_channel(void);

static int check_calibration(volatile u8 *regs, rtw89_channel_t *ch) {
    int bad = 0;

    /* Asked before the radio is tuned anywhere.  This must be reported as a
     * failure, not quietly accepted. */
    rtw89_set_channel(regs, 0, RTW89_BAND_2G, false);      /* refused: no tune */
    u8 early[9] = { 9, 0, 0, RTW89_RF_PATH_AB,
                    RTW89_BAND_5G, 0, 149, 0, 0 };
    int state = rtw89_rfk_ask(regs, ch, H2C_FUNC_RFK_RXDCK_OFFLOAD,
                              early, sizeof early, 500);
    if (state == RTW89_RFK_STATE_OK) {
        kwarn("rtw89", "selftest: the receiver was calibrated for a channel "
                       "the radio is not on, and reported success");
        bad++;
    }

    /* Now in the right order: tune, then calibrate for where it is. */
    if (!rtw89_set_channel(regs, 36, RTW89_BAND_5G, false)) {
        kwarn("rtw89", "selftest: the radio would not tune before calibration");
        return bad + 1;
    }

    if (!rtw89_rfk_calibrate(regs, ch, 0, RTW89_BAND_5G, 0, 36)) {
        kwarn("rtw89", "selftest: the radio would not calibrate");
        bad++;
    }

    if (!rtw89_model_rfk_asked(H2C_FUNC_RFK_DACK_OFFLOAD)) {
        kwarn("rtw89", "selftest: the converters were never calibrated");
        bad++;
    }
    if (!rtw89_model_rfk_asked(H2C_FUNC_RFK_RXDCK_OFFLOAD)) {
        kwarn("rtw89", "selftest: the receiver was never calibrated");
        bad++;
    }

    /* Each way the card can say no has to reach the log as that, rather than
     * as a timeout - a driver that reports every refusal as "no answer" sends
     * people looking at the wrong thing. */
    if (rtw89_rfk_state_name(RTW89_RFK_STATE_START) ==
        rtw89_rfk_state_name(RTW89_RFK_STATE_OK)) {
        kwarn("rtw89", "selftest: a calibration that started and one that "
                       "finished are reported the same way");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the radio calibrates: the card ran its converter and "
                       "receiver measurements and reported them done, and a "
                       "request for a channel it was not tuned to was refused");
    return bad;
}

/* Bits of the word the card writes in front of each packet. */
#define RXBD_WRITE_SIZE_MASK   0x3FFFu        /* 13:0  */
#define RXBD_LS                (1u << 14)
#define RXBD_FS                (1u << 15)
#define RXBD_TAG_SHIFT         16             /* 28:16 */
#define RXBD_TAG_MASK          0x1FFFu

/* Packets coming back, including the ones a confused card would send.
 *
 * Everything the word in front of a packet says is a number the card chose,
 * and the two that matter both turn a small wrong value into a read past the
 * end of a buffer.  The tag is different: getting it wrong loses packets
 * silently, which is worse than a crash because nothing points at it.
 */
static int check_receive(void) {
    int bad = 0;
    static u8 buf[64];

    /* One whole packet: first and last both set, twenty bytes, tag 7. */
    u32 w = 20u | RXBD_FS | RXBD_LS | (7u << RXBD_TAG_SHIFT);
    buf[0] = (u8)w; buf[1] = (u8)(w >> 8);
    buf[2] = (u8)(w >> 16); buf[3] = (u8)(w >> 24);

    rtw89_rx_info_t got;
    if (!rtw89_rx_parse(buf, 4 + 20, &got)) {
        kwarn("rtw89", "selftest: a well-formed packet was not accepted");
        bad++;
    } else if (got.bytes != 20 || !got.first || !got.last || got.tag != 7) {
        kwarn("rtw89", "selftest: a packet read as %u bytes, first %d, last "
                       "%d, tag %u - expected 20, 1, 1, 7",
              got.bytes, got.first, got.last, got.tag);
        bad++;
    }

    /* Claiming more than arrived. */
    if (rtw89_rx_parse(buf, 8, &got)) {
        kwarn("rtw89", "selftest: a packet claiming 20 bytes was accepted when "
                       "only 4 arrived");
        bad++;
    }
    /* And a word that did not arrive whole. */
    if (rtw89_rx_parse(buf, 2, &got)) {
        kwarn("rtw89", "selftest: two bytes were read as a packet header");
        bad++;
    }

    /* A fragment: the start of a packet but not the end.  A driver that
     * ignores these delivers the first piece of every large packet and drops
     * the rest, which looks like a link that only carries small frames. */
    w = 40u | RXBD_FS | (8u << RXBD_TAG_SHIFT);
    buf[0] = (u8)w; buf[1] = (u8)(w >> 8);
    buf[2] = (u8)(w >> 16); buf[3] = (u8)(w >> 24);
    if (!rtw89_rx_parse(buf, 4 + 40, &got) || got.first || !got.last) {
        /* first should be true, last false */
    }
    if (!got.first || got.last) {
        kwarn("rtw89", "selftest: a fragment read as first %d last %d, "
                       "expected 1 and 0", got.first, got.last);
        bad++;
    }

    /* The tag, in sequence and out of it. */
    u16 expected = 0;
    int missed = 0;
    rtw89_rx_tag_ok(&expected, 5, &missed);          /* the first sets the run */
    if (!rtw89_rx_tag_ok(&expected, 6, &missed) || missed) {
        kwarn("rtw89", "selftest: the next tag in sequence was reported as a "
                       "loss of %d", missed);
        bad++;
    }
    if (rtw89_rx_tag_ok(&expected, 10, &missed)) {
        kwarn("rtw89", "selftest: a tag that skipped three was accepted as in "
                       "sequence");
        bad++;
    } else if (missed != 3) {
        kwarn("rtw89", "selftest: a skip of three was counted as %d", missed);
        bad++;
    }
    /* And it carries on from what arrived rather than reporting every packet
     * after a loss as another loss. */
    if (!rtw89_rx_tag_ok(&expected, 11, &missed) || missed) {
        kwarn("rtw89", "selftest: after one loss the next packet was also "
                       "reported lost");
        bad++;
    }

    /* Posting a buffer the card cannot reach must be refused. */
    u8 slot[RTW89_PCI_RXBD_BYTES];
    if (rtw89_rx_bd_write(slot, 0x1FFFFFFFFull, 2048)) {
        kwarn("rtw89", "selftest: a receive buffer above four gigabytes was "
                       "accepted");
        bad++;
    }
    if (!rtw89_rx_bd_write(slot, 0x200000, 2048)) {
        kwarn("rtw89", "selftest: a good receive buffer was refused");
        bad++;
    } else {
        u16 size = (u16)((u32)slot[0] | ((u32)slot[1] << 8));
        u32 dma = (u32)slot[4] | ((u32)slot[5] << 8) |
                  ((u32)slot[6] << 16) | ((u32)slot[7] << 24);
        if (size != 2048 || dma != 0x200000) {
            kwarn("rtw89", "selftest: a receive descriptor says %u bytes at "
                           "%08x, expected 2048 at 200000", size, dma);
            bad++;
        }
    }

    if (!bad)
        kinfo("rtw89", "packets coming back are read correctly: a fragment is "
                       "known from a whole packet, a lost one is counted, and "
                       "a packet that lies about its length is refused");
    return bad;
}

/* What the card is told to pass up, checked at the registers.
 *
 * The check that matters is the second radio.  Its filters sit 0x4000 further
 * on, and a driver that writes both radios to the same place configures the
 * first one twice - which works perfectly on a machine using one radio and
 * fails only on the machines that need both.
 */
static int check_rx_filter(volatile u8 *regs) {
    int bad = 0;

    /* Poisoned first, so "written" and "already right" cannot be confused. */
    for (int mac = 0; mac < RTW89_MACS; mac++) {
        u32 base = (u32)mac * RTW89_MAC_STRIDE;
        wr32(regs, R_BE_MGNT_FLTR + base, 0xA5A5A5A5u);
        wr32(regs, R_BE_CTRL_FLTR + base, 0xA5A5A5A5u);
        wr32(regs, R_BE_DATA_FLTR + base, 0xA5A5A5A5u);
    }

    for (int mac = 0; mac < RTW89_MACS; mac++) {
        if (!rtw89_rx_filter_station(regs, mac)) {
            kwarn("rtw89", "selftest: radio %d would not take a filter", mac);
            bad++;
            continue;
        }

        u32 base = (u32)mac * RTW89_MAC_STRIDE;
        struct { const char *what; u32 reg; u32 want; } f[] = {
            { "management", R_BE_MGNT_FLTR + base, RX_FLTR_ACCEPT },
            { "data",       R_BE_DATA_FLTR + base, RX_FLTR_ACCEPT },
            { "control",    R_BE_CTRL_FLTR + base, RX_FLTR_DROP },
        };
        for (unsigned i = 0; i < ARRAY_LEN(f); i++) {
            u32 got = rd32(regs, f[i].reg);
            if (got != f[i].want) {
                kwarn("rtw89", "selftest: radio %d %s filter reads %08x, "
                               "expected %04x", mac, f[i].what, got, f[i].want);
                bad++;
            }
        }
    }

    /* And the two radios must not be the same registers.  Setting one and
     * looking at the other is the only way to catch a stride of zero. */
    wr32(regs, R_BE_DATA_FLTR + RTW89_MAC_STRIDE, 0x1234u);
    if (rd32(regs, R_BE_DATA_FLTR) == 0x1234u) {
        kwarn("rtw89", "selftest: the two radios share one set of filters, so "
                       "only the first is ever configured");
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "the card is told what to pass up: joining and traffic "
                       "accepted, hardware chatter dropped, on both radios "
                       "separately");
    return bad;
}

/* One request, all the way there and back.
 *
 * Thirteen things were checked on their own before this and none of them had
 * ever been used together.  That is the arrangement where every piece is
 * correct and nothing works: the header right, the descriptor right, the
 * doorbell right, and the request sitting somewhere the card does not look.
 *
 * So this is the join, driven against a model that goes and FETCHES what the
 * descriptor points at rather than watching registers - because a model that
 * only watches registers cannot tell a request that was placed correctly from
 * one that was merely described correctly.
 */
int  rtw89_model_requests_fetched(void);
bool rtw89_model_answered_into_nothing(void);
void rtw89_model_channel_reset(void);

static int check_command_channel(volatile u8 *regs) {
    enum { SLOTS = 8, BUF = 512 };
    static rtw89_channel_t ch;
    int bad = 0;

    /* Memory the card can be pointed at, which a static array is not.
     *
     * The first version of this used static arrays and virt_to_phys on them,
     * and got a physical address that was not theirs: virt_to_phys subtracts
     * the direct map's base, and a static array lives in the kernel image
     * rather than the direct map.  The card - and the model, faithfully - then
     * fetched from an address belonging to nothing, which presented as a card
     * that was asked something and never answered. */
    static u8 *tx_bd, *rx_bd, *tx_buf, *rx_buf;
    static u64 tx_bd_phys, rx_bd_phys, tx_buf_phys, rx_buf_phys;

    if (!tx_bd) {
        tx_bd = dma_alloc_pages(1, &tx_bd_phys);
        rx_bd = dma_alloc_pages(1, &rx_bd_phys);
        tx_buf = dma_alloc_pages(1, &tx_buf_phys);
        rx_buf = dma_alloc_pages(1, &rx_buf_phys);
    }
    if (!tx_bd || !rx_bd || !tx_buf || !rx_buf) {
        kwarn("rtw89", "selftest: no memory the card could be pointed at");
        return 0;
    }

    rtw89_model_channel_reset();
    memset(&ch, 0, sizeof ch);
    memset(tx_bd, 0, SLOTS * RTW89_PCI_BD_BYTES);
    memset(rx_bd, 0, SLOTS * RTW89_PCI_BD_BYTES);

    ch.tx.bd = tx_bd;   ch.tx.slots = SLOTS;   ch.tx.bd_phys = tx_bd_phys;
    ch.rx.bd = rx_bd;   ch.rx.slots = SLOTS;   ch.rx.bd_phys = rx_bd_phys;

    ch.tx_buffer = tx_buf;  ch.tx_buffer_phys = tx_buf_phys;
    ch.rx_buffer = rx_buf;  ch.rx_buffer_phys = rx_buf_phys;
    ch.rx_buffer_size = BUF;

    rtw89_ring_attach(regs, &ch.tx, R_BE_CH12_TXBD_DESA_L,
                      R_BE_CH12_TXBD_DESA_H, R_BE_CH12_TXBD_NUM,
                      R_BE_CH12_TXBD_IDX);
    rtw89_ring_attach(regs, &ch.rx, R_BE_RXQ0_RXBD_DESA_L,
                      R_BE_RXQ0_RXBD_DESA_H, R_BE_RXQ0_RXBD_NUM,
                      R_BE_RXQ0_RXBD_IDX_V1);

    const u8 question[4] = { 0x11, 0x22, 0x33, 0x44 };
    u8 answer[16];
    int n = rtw89_ask(regs, &ch, H2C_CL_MAC_FWDL, 0x02, question,
                      sizeof question, answer, sizeof answer, 200);

    if (n < 0) {
        kwarn("rtw89", "selftest: the card was asked something and did not "
                       "answer");
        return 1;
    }

    if (!rtw89_model_requests_fetched()) {
        kwarn("rtw89", "selftest: an answer arrived but the card never "
                       "fetched the request - so it answered something else");
        bad++;
    }
    if (rtw89_model_answered_into_nothing()) {
        kwarn("rtw89", "selftest: the request was asked before anywhere was "
                       "posted for the answer, so a real card would have "
                       "dropped it");
        bad++;
    }
    if (n != 4 || answer[0] != 0xC0 || answer[3] != 0x01) {
        kwarn("rtw89", "selftest: the answer came back as %d bytes starting "
                       "%02x, expected 4 starting c0", n, n > 0 ? answer[0] : 0);
        bad++;
    }

    /* A second one, because the first exchange leaves both rings part way
     * round and an index that only works from zero is a driver that answers
     * once. */
    n = rtw89_ask(regs, &ch, H2C_CL_MAC_FWDL, 0x02, question, sizeof question,
                  answer, sizeof answer, 200);
    if (n != 4) {
        kwarn("rtw89", "selftest: a second request came back as %d - the rings "
                       "only work from their starting position", n);
        bad++;
    }

    if (!bad)
        kinfo("rtw89", "a request reaches the card and its answer comes back: "
                       "the header, the descriptor, the doorbell and the "
                       "receiving ring all working together, twice");

    /* And the calibrations, which use this same channel - so they run here,
     * while it is set up, rather than being handed a second one. */
    bad += check_calibration(regs, &ch);
    return bad;
}

/* The packet engine, set up against the model. */
static int check_engine(volatile u8 *regs) {
    int bad = 0;

    if (!rtw89_sched_init(regs)) {
        kwarn("rtw89", "selftest: the scheduler would not start");
        bad++;
    } else {
        u32 ss = rd32(regs, R_BE_SS_CTRL);
        if (!(ss & B_BE_WARM_INIT)) {
            kwarn("rtw89", "selftest: the scheduler started but was not told "
                           "to keep its state");
            bad++;
        }
        if (ss & (B_BE_BAND_TRIG_EN | B_BE_BAND1_TRIG_EN)) {
            kwarn("rtw89", "selftest: the scheduler's band triggers were left "
                           "on");
            bad++;
        }
    }

    rtw89_security_init(regs);
    u32 sec = rd32(regs, R_BE_SEC_ENG_CTRL);
    /* Every kind, not most of them.  Broadcast is the one that is easy to
     * leave out and the one whose absence stops a network being joined at
     * all. */
    u32 needed = B_BE_SEC_TX_ENC | B_BE_SEC_RX_DEC | B_BE_BC_DEC | B_BE_MC_DEC |
                 B_BE_UC_MGNT_DEC | B_BE_BMC_MGNT_DEC;
    if ((sec & needed) != needed) {
        kwarn("rtw89", "selftest: the security engine is missing %08x of what "
                       "it needs", needed & ~sec);
        bad++;
    }
    if ((rd32(regs, R_BE_SEC_MPDU_PROC) & (B_BE_APPEND_ICV | B_BE_APPEND_MIC))
        != (B_BE_APPEND_ICV | B_BE_APPEND_MIC)) {
        kwarn("rtw89", "selftest: what encryption adds to a frame is not being "
                       "appended");
        bad++;
    }

    /* Flow control, read back - every field shares a register here too. */
    if (!rtw89_flow_control_init(regs)) {
        kwarn("rtw89", "selftest: flow control would not be configured");
        bad++;
    } else {
        u32 pages = rd32(regs, R_BE_CH_PAGE_CTRL);
        u32 shared = rd32(regs, R_BE_PUB_PAGE_CTRL2) & B_BE_PUBPG_ALL_MASK;
        if ((pages & B_BE_PREC_PAGE_CH011_V1_MASK) != RTW89_HFC_CH011_RESERVE ||
            ((pages & B_BE_PREC_PAGE_CH12_V1_MASK) >> 16) != RTW89_HFC_H2C_RESERVE) {
            kwarn("rtw89", "selftest: the per-channel reserves read back wrong");
            bad++;
        }
        if (shared != RTW89_HFC_PUBLIC_PAGES) {
            kwarn("rtw89", "selftest: %u shared pages, expected %u",
                  shared, (unsigned)RTW89_HFC_PUBLIC_PAGES);
            bad++;
        }
        /* The division of the tracking pages adds up to the pages there are.
         *
         * The first version of this check added the flow-control reserves to
         * the shared pool and complained that they came to more pages than
         * exist.  They are not shares - they are thresholds inside the host's
         * share - so the check was wrong and the numbers were right.  This is
         * the division that really is one. */
        u32 split = RTW89_WDE_QT_HOST + RTW89_WDE_QT_CARD_CPU +
                    RTW89_WDE_QT_PACKET_IN + RTW89_WDE_QT_CPU_IO;
        if (split != RTW89_WDE_LINKED_PAGES) {
            kwarn("rtw89", "selftest: the tracking pages divide into %u and "
                           "there are %u", split,
                  (unsigned)RTW89_WDE_LINKED_PAGES);
            bad++;
        }
        if (RTW89_HFC_PUBLIC_PAGES > RTW89_WDE_QT_HOST) {
            kwarn("rtw89", "selftest: flow control shares out %u pages of the "
                           "host's %u", (unsigned)RTW89_HFC_PUBLIC_PAGES,
                  (unsigned)RTW89_WDE_QT_HOST);
            bad++;
        }
    }

    /* The transmit side's timing, on both radios. */
    for (int mac = 0; mac < RTW89_MACS; mac++) {
        u32 at = (u32)mac * RTW89_MAC_STRIDE;
        wr32(regs, R_BE_TB_PPDU_CTRL + at, 0xFFFFFFFFu);   /* poisoned */
        wr32(regs, R_BE_WMTX_TCR_BE_4 + at, 0);

        if (!rtw89_tmac_init(regs, mac)) { bad++; continue; }

        if (!rtw89_rmac_init(regs, mac)) { bad++; continue; }

        /* The signal processor, and that it was left with its block released
         * rather than held down. */
        wr32(regs, R_BE_FEN_RST_ENABLE, 0);
        wr32(regs, R_BE_MEM_PWR_CTRL, B_BE_MEM_BBMCU0_DS_V1);
        if (!rtw89_baseband_reset(regs, mac)) { bad++; continue; }

        u32 fen = rd32(regs, R_BE_FEN_RST_ENABLE);
        u32 block = mac ? B_BE_FEN_BB1_IP_RSTN : B_BE_FEN_BB_IP_RSTN;
        if (!(fen & block)) {
            kwarn("rtw89", "selftest: radio %d's signal processor was left "
                           "held in reset", mac);
            bad++;
        }
        if (rd32(regs, R_BE_MEM_PWR_CTRL) & B_BE_MEM_BBMCU0_DS_V1) {
            kwarn("rtw89", "selftest: its memory was left in the low-power "
                           "state, so it would boot into memory that is not "
                           "there");
            bad++;
        }

        /* And the other half: the registers its processor boots against, and
         * the receiver's thresholds. */
        if (!rtw89_baseband_configure(regs, mac)) { bad++; continue; }

        /* The table has to have reached the processor's own window rather than
         * the MAC's.  Three windows share one mapping, and an address written
         * without its base lands in the MAC - which accepts it, so the only
         * way to tell is to look where the value should be. */
        if (rd32(regs, RTW89_BBMCU_BASE + 0x6808) != 0x76543210) {
            kwarn("rtw89", "selftest: radio %d's signal processor was not "
                           "handed its boot registers", mac);
            bad++;
        }
        /* 0x6800 and 0x6820 are each written twice; the LAST value is the one
         * that releases it.  A loop that stopped early, or a table that had
         * been tidied, leaves the first. */
        if (rd32(regs, RTW89_BBMCU_BASE + 0x6800) != 0xC0000FFF ||
            rd32(regs, RTW89_BBMCU_BASE + 0x6820) != 0xFFFFFFFFu) {
            kwarn("rtw89", "selftest: radio %d's processor was left holding "
                           "the value that keeps it down", mac);
            bad++;
        }

        /* A threshold the receiver needs, in the baseband's window. */
        u32 he = (rd32(regs, RTW89_PHY_CR_BASE + R_BEDGE) & B_HE_RATE_TH) >> 27;
        if (he != 0xA) {
            kwarn("rtw89", "selftest: radio %d's receive threshold reads %x "
                           "rather than a", mac, he);
            bad++;
        }
        if (!(rd32(regs, RTW89_PHY_CR_BASE + R_BBCLK) & B_CLK_640M)) {
            kwarn("rtw89", "selftest: radio %d's baseband clock was not "
                           "started", mac);
            bad++;
        }

        u32 units = (rd32(regs, R_BE_RX_FLTR_OPT + at) &
                     B_BE_RX_MPDU_MAX_LEN_MASK) >> 16;
        /* It has to fit six bits, and it has to be more than nothing - a
         * maximum of zero is a radio that accepts no frame at all. */
        if (!units || units > (B_BE_RX_MPDU_MAX_LEN_MASK >> 16)) {
            kwarn("rtw89", "selftest: radio %d accepts frames up to %u units, "
                           "which is not a usable maximum", mac, units);
            bad++;
        }

        if (rd32(regs, R_BE_TB_PPDU_CTRL + at) & B_BE_QOSNULL_UPD_MUEDCA_EN) {
            kwarn("rtw89", "selftest: radio %d still counts keep-alive frames "
                           "against its share of the air", mac);
            bad++;
        }
        u32 t = rd32(regs, R_BE_WMTX_TCR_BE_4 + at);
        if (((t & B_BE_EHT_HE_PPDU_4XLTF_ZLD_USTIMER_MASK) >> 24) !=
                RTW89_ZLD_USTIMER_4XLTF ||
            ((t & B_BE_EHT_HE_PPDU_2XLTF_ZLD_USTIMER_MASK) >> 16) !=
                RTW89_ZLD_USTIMER_2XLTF) {
            kwarn("rtw89", "selftest: radio %d's frame timing reads %08x",
                  mac, t);
            bad++;
        }
    }

    /* Where the packet-description table lives, and that the regions kept
     * back do not run into the memory that is not there. */
    if (!rtw89_txpktctl_init(regs)) {
        kwarn("rtw89", "selftest: the packet description table was not placed");
        bad++;
    } else {
        u32 cfg = rd32(regs, R_BE_TXPKTCTL_MPDUINFO_CFG);
        if (((cfg & B_BE_MPDUINFO_PKTID_MASK) >> 16) != RTW89_RSVD_FIRST_PAGE) {
            kwarn("rtw89", "selftest: the description table reads as starting "
                           "at page %u, expected %u",
                  (cfg & B_BE_MPDUINFO_PKTID_MASK) >> 16,
                  (unsigned)RTW89_RSVD_FIRST_PAGE);
            bad++;
        }
        if (!(cfg & B_BE_MPDUINFO_FEN)) {
            kwarn("rtw89", "selftest: the description table was placed and "
                           "left switched off");
            bad++;
        }

        /* Every region kept back, laid end to end, must fit in the pages that
         * exist beyond the free ones.  They overlap silently otherwise, and
         * what is lost is whichever the hardware wrote second. */
        u32 kept = RTW89_RSVD_MPDU_INFO_PAGES +
                   RTW89_RSVD_B0_CSI_PAGES + RTW89_RSVD_B1_CSI_PAGES +
                   RTW89_RSVD_B0_LMR_PAGES + RTW89_RSVD_B1_LMR_PAGES +
                   RTW89_RSVD_B0_FTM_PAGES + RTW89_RSVD_B1_FTM_PAGES;
        u32 spare = RTW89_PLE_UNLINKED_PAGES;
        if (kept > spare) {
            kwarn("rtw89", "selftest: %u pages are kept back and only %u exist "
                           "beyond the free ones", kept, spare);
            bad++;
        }
    }

    /* Fetching ahead, and the switch left on afterwards. */
    if (!rtw89_preload_init(regs)) {
        kwarn("rtw89", "selftest: the transmit path was not set to fetch "
                       "ahead");
        bad++;
    } else {
        u32 cfg0 = rd32(regs, R_BE_TXPKTCTL_B0_PRELD_CFG0);
        u32 most = PRELD_B0_ENT_NUM * PRELD_AMSDU_SIZE;
        if (!(cfg0 & B_BE_B0_PRELD_FEN)) {
            kwarn("rtw89", "selftest: fetching ahead was configured and left "
                           "switched off");
            bad++;
        }
        if (((cfg0 & B_BE_B0_PRELD_USEMAXSZ_MASK) >> 16) != most) {
            kwarn("rtw89", "selftest: how much may be held reads %u, expected "
                           "%u", (cfg0 & B_BE_B0_PRELD_USEMAXSZ_MASK) >> 16,
                  most);
            bad++;
        }
        /* It must fit the field, or the size silently becomes a smaller one
         * and every frame is cut short. */
        if (most > (B_BE_B0_PRELD_USEMAXSZ_MASK >> 16)) {
            kwarn("rtw89", "selftest: %u does not fit the field that carries "
                           "it", most);
            bad++;
        }
    }

    /* Multi-link, which is what a Wi-Fi 7 part is for. */
    if (!rtw89_mlo_init(regs)) {
        kwarn("rtw89", "selftest: the link table was not built");
        bad++;
    } else {
        if (!(rd32(regs, R_BE_SS_CTRL) & B_BE_MLO_HW_CHGLINK_EN)) {
            kwarn("rtw89", "selftest: the table was built and the hardware was "
                           "not told it may change links");
            bad++;
        }
        if (!(rd32(regs, R_BE_CMAC_SHARE_ACQCHK_CFG_0) &
              B_BE_R_MACID_ACQ_CHK_EN)) {
            kwarn("rtw89", "selftest: station-id checking was left off");
            bad++;
        }
    }

    /* And that the numbers dividing the card's memory are the right numbers,
     * which is arithmetic and needs no card at all. */
    if (!rtw89_memory_split_adds_up()) bad++;

    /* Then the division written into the card, and read back.
     *
     * Read back because every field here shares a register with another and is
     * written by masking: a mask one bit wide in the wrong place leaves the
     * value looking right in the variable and wrong in the card. */
    if (!rtw89_dle_init(regs)) {
        kwarn("rtw89", "selftest: the card's memory was not divided");
        bad++;
    } else {
        u32 wde = rd32(regs, R_BE_WDE_PKTBUF_CFG);
        u32 ple = rd32(regs, R_BE_PLE_PKTBUF_CFG);

        struct { const char *what; u32 got; u32 want; } f[] = {
            { "the tracking page size",
              wde & B_BE_WDE_PAGE_SEL_MASK, WDE_PAGE_SEL_64 },
            { "how many pages track",
              (wde & B_BE_WDE_FREE_PAGE_NUM_MASK) >> 16, RTW89_WDE_LINKED_PAGES },
            { "the packet page size",
              ple & B_BE_PLE_PAGE_SEL_MASK, PLE_PAGE_SEL_128 },
            { "how many pages hold packets",
              (ple & B_BE_PLE_FREE_PAGE_NUM_MASK) >> 16, RTW89_PLE_LINKED_PAGES },
            { "where the packets start",
              (ple & B_BE_PLE_START_BOUND_MASK) >> 8,
              RTW89_PLE_START_OFFSET / DLE_BOUND_UNIT },
        };
        for (unsigned i = 0; i < ARRAY_LEN(f); i++)
            if (f[i].got != f[i].want) {
                kwarn("rtw89", "selftest: %s reads %u, expected %u",
                      f[i].what, f[i].got, f[i].want);
                bad++;
            }

        /* And both engines running afterwards, not left switched off. */
        u32 en = rd32(regs, R_BE_DMAC_FUNC_EN);
        if ((en & (B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN)) !=
            (B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN)) {
            kwarn("rtw89", "selftest: the memory was divided and the engines "
                           "were left off");
            bad++;
        }
    }

    rtw89_mpdu_init(regs);
    if (!(rd32(regs, R_BE_MPDU_PROC) & B_BE_APPEND_FCS)) {
        kwarn("rtw89", "selftest: the frame check sequence is not appended, so "
                       "every frame sent would be discarded by whoever "
                       "received it");
        bad++;
    }

    if (!bad)
        /* Kept short on purpose.  The previous version of this line named all
         * five things and ran past the log's line length, which truncates
         * without saying so - the check looking for the end of it then failed
         * on a system where everything had worked. */
        kinfo("rtw89", "the packet engine is set up: memory divided, "
                       "scheduler waited for, links tabled, frames encrypted "
                       "and checksummed");
    return bad;
}

int rtw89_selftest(void) {
    kinfo("rtw89", "BEGIN simulated self-tests; deliberately malformed inputs below must be rejected, not accepted");
    /* The whole self-test is one long series of deliberately-bad inputs handed
     * to the driver to prove it refuses them.  Each refusal is a real error in
     * ordinary use and is logged as one - but here it is the expected outcome,
     * so the refusal logging is quieted for the duration.  It leaked before as a
     * screen full of red "errors" that were the test passing, which buried the
     * one line that would have said something was actually wrong. */
    quiet_refusals = true;

    int header_faults = check_h2c_header();
    int failures = header_faults + check_fw_download() + check_ring() +
                   check_c2h();

    volatile u8 *regs = rtw89_model_attach();
    if (!regs) {
        kwarn("rtw89", "selftest: no memory for the model");
        quiet_refusals = false;
        kerr("rtw89", "END simulated self-tests: model allocation failed");
        return 1;
    }
    modelled = true;

    /* The ring's registers, before the power sequence disturbs them - this
     * only needs somewhere to write and read back, not a card that is up. */
    failures += check_ring_handover(regs);
    failures += check_fw_ready(regs);
    failures += check_radio(regs);
    failures += check_channel(regs);
    failures += check_descriptors();
    failures += check_transmit(regs);
    failures += check_frame_receive(regs);
    failures += check_receive();
    failures += check_rx_filter(regs);
    failures += check_command_channel(regs);
    failures += check_engine(regs);

    if (!rtw89_power_on(regs, "the model")) {
        kwarn("rtw89", "selftest: the power-on sequence did not complete "
                       "against a model that answers every wait");
        failures++;
    }

    /* The processor, and the download path it opens. */
    expect_dirty_debug = true;
    bool started = rtw89_fwdl_start_cpu(regs, 0, true, "the model");
    expect_dirty_debug = false;
    if (!started) {
        kwarn("rtw89", "the card's processor would not start against a model "
                       "that answers every step");
        failures++;
    }
    if (!rtw89_model_cpu_running()) {
        kwarn("rtw89", "selftest: the processor was enabled without being "
                       "released from its hold first - on silicon it would "
                       "start and stop again");
        failures++;
    }
    if (!rtw89_model_debug_cleared()) {
        kwarn("rtw89", "selftest: the card's debug registers were not all "
                       "cleared before boot");
        failures++;
    }
    {
        u8 st = rtw89_fwdl_status(regs);
        if (st != RTW89_FWDL_WCPU_FWDL_RDY) {
            kwarn("rtw89", "selftest: the download state read back as \"%s\", "
                           "expected \"ready to receive\" - the card's own "
                           "numbering is not the driver's and has to be mapped",
                  rtw89_fwdl_status_name(st));
            failures++;
        }
    }

    if (!rtw89_model_powered()) {
        kwarn("rtw89", "selftest: the sequence finished but the model does "
                       "not consider the part powered - a step is missing or "
                       "out of order");
        failures++;
    }

    /* Fifteen writes go through the crystal's side band - two to the
     * phase-locked loop, ten to the analogue block and the two radio chains,
     * two to the references, one back to the loop.  Counting them catches a
     * mailbox that silently accepted nothing, which from the driver's side
     * looks exactly like one that worked.
     *
     * The number is counted from the sequence rather than remembered: it was
     * written as sixteen first, and the model was right and the test wrong. */
    int writes = rtw89_model_xtal_writes();
    if (writes != 15) {
        kwarn("rtw89", "selftest: %d writes reached the crystal's side band, "
                       "expected 15", writes);
        failures++;
    }

    /* And the last of them, in full.  A count alone passes a driver that sent
     * fifteen copies of the wrong thing; this pins the encoding - address,
     * value and mask each in their own byte of the word. */
    u8 addr = 0, value = 0, mask = 0;
    rtw89_model_last_xtal(&addr, &value, &mask);
    if (addr != XTAL_SI_PLL_1 || value != 0x40 || mask != 0x60) {
        kwarn("rtw89", "selftest: the last side-band write came out "
                       "address %#x value %#x mask %#x, expected %#x/%#x/%#x",
              addr, value, mask, XTAL_SI_PLL_1, 0x40, 0x60);
        failures++;
    }

    /* ---- the DMA engine --------------------------------------------- */
    if (!rtw89_dma_reset(regs, 256, "the model")) {
        kwarn("rtw89", "selftest: the DMA engine would not restart against a "
                       "model that answers");
        failures++;
    }
    if (!rtw89_model_dma_started()) {
        kwarn("rtw89", "selftest: the engine was started without being stopped "
                       "and drained first - on silicon that clears a channel's "
                       "pointers underneath a transfer still in flight");
        failures++;
    }
    if (rtw89_model_rx_ring_slots() != 255) {
        kwarn("rtw89", "selftest: the receive rings were told %u, expected 255 "
                       "- the card wants the index of the last slot, not the "
                       "count", rtw89_model_rx_ring_slots());
        failures++;
    }

    modelled = false;
    rtw89_model_detach();

    /* ---- the firmware container ------------------------------------- */

    /* The outer MFW table is distinct from the executable header.  Entries are
     * deliberately out of version order, with an MP build in front, to prove
     * selection is by type and closest compatible production cut. */
    static u8 mfw[192];
    memset(mfw, 0, sizeof mfw);
    mfw[0] = RTW89_MFW_SIG;
    mfw[1] = 4;
    /* cut 1 normal MP -- must not win */
    mfw[16] = 1; mfw[17] = RTW89_FW_NORMAL; mfw[18] = 1;
    put32(mfw + 20, 80); put32(mfw + 24, 16);
    /* cut 2 normal production -- newer than cut 1 hardware */
    mfw[32] = 2; mfw[33] = RTW89_FW_NORMAL;
    put32(mfw + 36, 96); put32(mfw + 40, 16);
    /* cut 0 normal production -- the expected fallback */
    mfw[48] = 0; mfw[49] = RTW89_FW_NORMAL;
    put32(mfw + 52, 112); put32(mfw + 56, 16);
    /* right cut, wrong type */
    mfw[64] = 1; mfw[65] = RTW89_FW_WOWLAN;
    put32(mfw + 68, 128); put32(mfw + 72, 16);
    mfw[112] = 0xA5;

    rtw89_fw_image_t chosen;
    if (!rtw89_select_firmware(mfw, sizeof mfw, 1, RTW89_FW_NORMAL,
                               &chosen) ||
        !chosen.from_container || chosen.cv != 0 || chosen.data != mfw + 112 ||
        chosen.size != 16 || chosen.data[0] != 0xA5) {
        kwarn("rtw89", "selftest: MFW selection did not choose the closest "
                       "compatible production image");
        failures++;
    }
    /* Make the cut-1 entry production and prove it now supersedes cut 0. */
    mfw[18] = 0;
    mfw[80] = 0x5A;
    if (!rtw89_select_firmware(mfw, sizeof mfw, 1, RTW89_FW_NORMAL,
                               &chosen) ||
        chosen.cv != 1 || chosen.data != mfw + 80 || chosen.data[0] != 0x5A) {
        kwarn("rtw89", "selftest: MFW selection did not prefer the exact cut");
        failures++;
    }
    quiet_refusals = true;
    put32(mfw + 20, sizeof mfw - 4);
    put32(mfw + 24, 16);
    if (rtw89_select_firmware(mfw, sizeof mfw, 1, RTW89_FW_NORMAL,
                              &chosen)) {
        kwarn("rtw89", "selftest: an MFW image outside its container was accepted");
        failures++;
    }
    quiet_refusals = false;

    /* A file in Realtek's own v1 format, built here so the reader has
     * something real to read rather than a buffer shaped like what it
     * expects.  The dynamic header is host-only, and the security section's
     * two MSSC signatures occupy file space but are not download payload. */
    static u8 image[2048];
    memset(image, 0, sizeof image);

    const int sections = 2;
    const u32 base = RTW89_FW_HDR_V1_BYTES +
                     sections * RTW89_FW_SECTION_V1_BYTES;
    const u32 dynamic = 8;
    const u32 header_length = base + dynamic;

    /* w1: version 3.4.5.6 */
    put32(image + 1 * 4, 3u | (4u << 8) | (5u << 16) | (6u << 24));
    /* w3: header version 1 */
    put32(image + 3 * 4, 1u << FW_HDR_V1_W3_HDR_VER_SHIFT);
    /* w4: the eleventh of August */
    put32(image + 4 * 4, 8u | (11u << 8));
    /* w5: 2026, with the complete header size in the high half */
    put32(image + 5 * 4, 2026u | (header_length << FW_HDR_V1_W5_HDR_SIZE_SHIFT));
    /* w6: two sections and checksums on each legacy MSSC signature */
    put32(image + 6 * 4, ((u32)sections << FW_HDR_V1_W6_SEC_NUM_SHIFT) |
                         FW_HDR_V1_W6_DSP_CHKSUM);
    put32(image + 7 * 4, FWDL_SECTION_PER_PKT_LEN | FW_HDR_V1_W7_DYN_HDR);

    /* Section 0: 64 bytes to 0x20000, no checksum. */
    put32(image + RTW89_FW_HDR_V1_BYTES + 0, 0x00020000u);
    put32(image + RTW89_FW_HDR_V1_BYTES + 4, 64u);
    /* Section 1: 32 bytes plus checksum to 0x30000, followed by two MSSCs. */
    put32(image + RTW89_FW_HDR_V1_BYTES + 16 + 0, 0x00030000u);
    put32(image + RTW89_FW_HDR_V1_BYTES + 16 + 4,
          32u | (FWDL_SECURITY_SECTION_TYPE << FWSEC_V1_W1_TYPE_SHIFT) |
          FWSEC_V1_W1_CHECKSUM | FWSEC_V1_W1_REDL);
    put32(image + RTW89_FW_HDR_V1_BYTES + 16 + 8, 2);

    /* The dynamic-header header describes itself. */
    put32(image + base, dynamic);

    /* 64 + (32 + 8) bytes downloaded; 2 * (512 + 8) bytes only stored. */
    const u32 payload = 64 + 32 + FWDL_SECTION_CHKSUM_LEN;
    const u32 mssc_bytes = 2 * (FWDL_SECURITY_SIGLEN +
                                FWDL_SECURITY_CHKSUM_LEN);
    const size_t image_len = header_length + payload + mssc_bytes;

    rtw89_fw_info_t fw;
    if (!rtw89_parse_firmware(image, image_len, &fw)) {
        kwarn("rtw89", "selftest: a well formed firmware container was "
                       "refused");
        failures++;
    } else {
        if (fw.major != 3 || fw.minor != 4 || fw.subversion != 5 ||
            fw.subindex != 6) {
            kwarn("rtw89", "selftest: the version read back as %u.%u.%u.%u, "
                           "expected 3.4.5.6",
                  fw.major, fw.minor, fw.subversion, fw.subindex);
            failures++;
        }
        if (fw.year != 2026 || fw.month != 8 || fw.date != 11) {
            kwarn("rtw89", "selftest: the build date read back as %u-%u-%u",
                  fw.year, fw.month, fw.date);
            failures++;
        }
        if (fw.header_version != 1) {
            kwarn("rtw89", "selftest: the header version read back as %u",
                  fw.header_version);
            failures++;
        }
        if (fw.section_count != 2 || fw.header_length != header_length ||
            fw.dynamic_header_length != dynamic) {
            kwarn("rtw89", "selftest: %d sections after a %u-byte header, "
                           "expected 2 after %u",
                  fw.section_count, fw.header_length, header_length);
            failures++;
        }
        if (fw.sections[0].download_address != 0x00020000u ||
            fw.sections[0].length != 64) {
            kwarn("rtw89", "selftest: the first section came out %u bytes to "
                           "%#x", fw.sections[0].length,
                  fw.sections[0].download_address);
            failures++;
        }
        /* The one that is easy to get wrong: a section with a checksum takes
         * eight more bytes in the file than its length field says. */
        if (fw.sections[1].length != 32 + FWDL_SECTION_CHKSUM_LEN) {
            kwarn("rtw89", "selftest: a section carrying a checksum came out "
                           "%u bytes, expected %u", fw.sections[1].length,
                  32 + FWDL_SECTION_CHKSUM_LEN);
            failures++;
        }
        if (!fw.sections[1].redownload) {
            kwarn("rtw89", "selftest: the redownload flag was not read");
            failures++;
        }
        if (fw.sections[1].type != FWDL_SECURITY_SECTION_TYPE ||
            fw.sections[1].mssc != 2 ||
            fw.sections[1].mssc_length != mssc_bytes) {
            kwarn("rtw89", "selftest: the security section's MSSC pool was "
                           "not measured correctly");
            failures++;
        }
        if (fw.payload_bytes != payload) {
            kwarn("rtw89", "selftest: the sections add up to %u bytes, "
                           "expected %u", fw.payload_bytes, payload);
            failures++;
        }
        if (fw.file_bytes != payload + mssc_bytes) {
            kwarn("rtw89", "selftest: firmware storage was counted as %u bytes, "
                           "expected %u", fw.file_bytes, payload + mssc_bytes);
            failures++;
        }
        if (!fw.needs_security_profile) {
            kwarn("rtw89", "selftest: an MSSC-bearing image was not marked as "
                           "requiring a hardware security profile");
            failures++;
        }
    }

    /* And the refusals.  Each of these is a file that would otherwise be
     * downloaded into the card off the end of the buffer holding it. */
    quiet_refusals = true;
    if (rtw89_parse_firmware(image, RTW89_FW_HDR_V1_BYTES - 1, &fw)) {
        kwarn("rtw89", "selftest: a file shorter than its header was accepted");
        failures++;
    }
    if (rtw89_parse_firmware(image, image_len - 1, &fw)) {
        kwarn("rtw89", "selftest: a file one byte short of its own sections "
                       "was accepted");
        failures++;
    }
    {
        /* More sections than there is room to describe. */
        static u8 wrong[2048];
        memcpy(wrong, image, sizeof wrong);
        put32(wrong + 6 * 4, 200u << FW_HDR_V1_W6_SEC_NUM_SHIFT);
        if (rtw89_parse_firmware(wrong, image_len, &fw)) {
            kwarn("rtw89", "selftest: a file claiming 200 sections was "
                           "accepted");
            failures++;
        }
    }
    {
        /* A section of no length, which would make the download a no-op that
         * looks like it worked. */
        static u8 wrong[2048];
        memcpy(wrong, image, sizeof wrong);
        put32(wrong + RTW89_FW_HDR_V1_BYTES + 4, 0);
        if (rtw89_parse_firmware(wrong, image_len, &fw)) {
            kwarn("rtw89", "selftest: a file with an empty section was "
                           "accepted");
            failures++;
        }
    }

    quiet_refusals = false;

    if (!failures)
        kinfo("rtw89", "a Wi-Fi 7 part's power-on sequence runs correctly "
                       "against a model of it, in order and with every wait "
                       "waited on; its firmware container is read correctly "
                       "and four malformed ones are refused; its processor "
                       "starts in the right order with the download path "
                       "open, and its DMA engine is drained before its "
                       "channels are cleared");
    if (failures)
        kerr("rtw89", "END simulated self-tests: %d failures", failures);
    else
        kinfo("rtw89", "END simulated self-tests: PASS; expected input refusals are not a physical radio failure");
    return failures;
}

/* ==================================================== talking to the firmware
 *
 * The header that goes in front of every request to the card's processor.
 *
 * Two words.  The first says what the request is - a category, a class within
 * it, and a function within that - plus a sequence number the card echoes back
 * so an answer can be matched to what it answers.  The second carries the
 * length of the whole packet, this header included, and two bits asking to be
 * told the request arrived and that it finished.
 *
 * Written little-endian by hand rather than by casting a structure over the
 * buffer.  The card's byte order is fixed and this kernel's is not something
 * the packet should depend on, and a structure would also invite the compiler
 * to pad between the two words - which would move every field after it.
 */
void rtw89_h2c_header(u8 out[H2C_HEADER_LEN], u8 cat, u8 cls, u8 func,
                      u8 del_type, u8 seq, u32 payload_len,
                      bool rec_ack, bool done_ack) {
    u32 w0 = ((u32)(cat      & H2C_HDR_CAT_MASK)      << H2C_HDR_CAT_SHIFT)      |
             ((u32)(cls      & H2C_HDR_CLASS_MASK)    << H2C_HDR_CLASS_SHIFT)    |
             ((u32)(func     & H2C_HDR_FUNC_MASK)     << H2C_HDR_FUNC_SHIFT)     |
             ((u32)(del_type & H2C_HDR_DEL_TYPE_MASK) << H2C_HDR_DEL_TYPE_SHIFT) |
             ((u32)(seq      & H2C_HDR_SEQ_MASK)      << H2C_HDR_SEQ_SHIFT);

    /* The length the card is told is the whole packet.  Passing the payload's
     * length here would have the card stop eight bytes early on every request,
     * which on a firmware download truncates the firmware. */
    u32 total = payload_len + H2C_HEADER_LEN;
    u32 w1 = total & H2C_HDR_TOTAL_LEN_MASK;
    if (rec_ack)  w1 |= H2C_HDR_REC_ACK;
    if (done_ack) w1 |= H2C_HDR_DONE_ACK;

    out[0] = (u8)w0;        out[1] = (u8)(w0 >> 8);
    out[2] = (u8)(w0 >> 16); out[3] = (u8)(w0 >> 24);
    out[4] = (u8)w1;        out[5] = (u8)(w1 >> 8);
    out[6] = (u8)(w1 >> 16); out[7] = (u8)(w1 >> 24);
}

/* ------------------------------------------------------- the download itself
 *
 * The container's header first, then every section's bytes, cut into pieces
 * small enough to be carried.
 *
 * Two things here are easy to get wrong and neither announces itself:
 *
 *   The header that is sent is not the whole header.  A newer container can
 *   carry a dynamic extension after the fixed part, and that extension is for
 *   the host to read, not the card - so what goes across is the header's
 *   length minus whatever the extension took.  Sending the extension too gives
 *   the processor a header longer than it expects and it stops reading in the
 *   middle of a field.
 *
 *   The pieces are cut by length, not by section.  A section that divides
 *   evenly by the piece size sends no short final piece, and a driver that
 *   assumes there is always one sends an empty request at the end of it.
 */
bool rtw89_fw_download(const u8 *fw, size_t size, const rtw89_fw_info_t *info,
                       rtw89_h2c_send_fn send, void *ctx) {
    if (!fw || !info || !send || size > 0xFFFFFFFFu) return false;

    /* Revalidate the complete file map before sending the first byte.  The
     * parser normally established this, but firmware memory and its length are
     * separate arguments here; a mismatch must not produce a partial download
     * followed by a late bounds failure. */
    if (info->section_count <= 0 ||
        info->section_count > RTW89_FW_MAX_SECTIONS)
        return false;
    u32 fixed_header_len = RTW89_FW_HDR_V1_BYTES +
                           (u32)info->section_count * RTW89_FW_SECTION_V1_BYTES;
    if (info->header_length < fixed_header_len ||
        info->header_length > size ||
        info->dynamic_header_length != info->header_length - fixed_header_len ||
        fixed_header_len > FWDL_SECTION_PER_PKT_LEN)
        return false;
    u32 part_size = info->part_size;
    if (!part_size || part_size > FWDL_SECTION_PER_PKT_LEN) {
        kerr("rtw89", "the firmware's request size %u is outside the safe limit",
             part_size);
        return false;
    }

    /* An MSSC pool is not data that may simply be omitted on every chip.
     * Secure-boot silicon needs one signature selected from it and substituted
     * into the security section.  Until the caller supplies that validated
     * selection, reject it here as well as at bring-up so no alternate caller
     * can accidentally hand the unpatched image to the card. */
    if (info->needs_security_profile && !info->security_validated) return false;
    size_t file_at = info->header_length;
    u64 payload = 0;
    for (int i = 0; i < info->section_count; i++) {
        u32 section = info->sections[i].length;
        u32 mssc = info->sections[i].mssc_length;
        if (!section || section > size - file_at || mssc > size - file_at - section)
            return false;
        u32 key_at = info->sections[i].key_offset;
        u32 key_len = info->sections[i].key_length;
        if (mssc) {
            size_t pool_at = file_at + section;
            if (!info->security_validated ||
                info->sections[i].type != FWDL_SECURITY_SECTION_TYPE)
                return false;
            /* Non-secure silicon legitimately sends the section's original
             * signature and skips the host-side pool. Secure silicon must
             * have both a selected offset and length, validated by the
             * security-profile helper before this point. */
            if (key_at || key_len) {
                if (!key_at || !key_len || key_len > section ||
                    key_at < pool_at || key_at > size ||
                    key_len > size - key_at || key_len > mssc ||
                    key_at - pool_at > mssc - key_len)
                    return false;
            }
        } else if (key_at || key_len) {
            return false;
        }
        file_at += (size_t)section + mssc;
        payload += section;
    }
    if (file_at != size || payload != info->payload_bytes) return false;

    static u8 packet[H2C_HEADER_LEN + FWDL_SECTION_PER_PKT_LEN];
    /* --- the fixed header and descriptors, without the host-only dynamic
     * extension.  Section data still begins after the complete header. ----- */
    u32 header_len = fixed_header_len;

    rtw89_h2c_header(packet, H2C_CAT_MAC, H2C_CL_MAC_FWDL,
                     H2C_FUNC_MAC_FWHDR_DL, 0, 0, header_len, false, false);
    memcpy(packet + H2C_HEADER_LEN, fw, header_len);
    /* Linux's v1 download advertises the transport's fixed 2020-byte chunk
     * size, independent of the value in the stored image header. */
    packet[H2C_HEADER_LEN + 7 * 4] = (u8)FWDL_SECTION_PER_PKT_LEN;
    packet[H2C_HEADER_LEN + 7 * 4 + 1] =
        (u8)(FWDL_SECTION_PER_PKT_LEN >> 8);

    if (!send(ctx, packet, H2C_HEADER_LEN + header_len, false)) {
        kerr("rtw89", "the card would not take the firmware's header");
        return false;
    }

    /* --- and the sections, in the order the header listed them ----------- */
    u32 at = info->header_length;          /* the bytes begin after it      */
    for (int i = 0; i < info->section_count; i++) {
        u32 left = info->sections[i].length;

        if (at > size || left > size - at ||
            info->sections[i].mssc_length > size - at - left) {
            kerr("rtw89", "section %d runs %u bytes past the end of the file",
                 i, left);
            return false;
        }

        u32 section_at = at;
        u32 section_len = left;
        u32 key_at = info->sections[i].key_offset;
        u32 key_len = info->sections[i].key_length;
        while (left) {
            u32 piece = left > FWDL_SECTION_PER_PKT_LEN ?
                        FWDL_SECTION_PER_PKT_LEN : left;
            memcpy(packet, fw + at, piece);
            if (key_len) {
                /* The chosen signature replaces the security section's tail,
                 * not the host-side MSSC pool nor an extra transmitted chunk. */
                u32 key_dst = section_at + section_len - key_len;
                u32 overlap_start = at > key_dst ? at : key_dst;
                u32 overlap_end = at + piece < key_dst + key_len ?
                                  at + piece : key_dst + key_len;
                if (overlap_start < overlap_end)
                    memcpy(packet + overlap_start - at,
                           fw + key_at + overlap_start - key_dst,
                           overlap_end - overlap_start);
            }

            if (!send(ctx, packet, piece, true)) {
                if (!quiet_refusals)
                    kerr("rtw89", "the card stopped taking section %d with %u "
                                  "bytes still to go", i, left);
                return false;
            }

            at += piece;
            left -= piece;
        }

        /* MSSC signature pools live in the file after a security section but
         * are host-side choices, not another section to execute. */
        at += info->sections[i].mssc_length;
    }

    if (at != size) {
        kerr("rtw89", "%u bytes remain after the firmware's last section",
             (u32)(size - at));
        return false;
    }

    kinfo("rtw89", "%u bytes of firmware handed over as one H2C header and raw FWDL sections",
          info->payload_bytes);
    return true;
}

/* ------------------------------------------------------------------ the ring
 *
 * One descriptor, written the way the hardware reads it: little-endian, eight
 * bytes, no padding.  Written byte by byte rather than through a structure
 * because a structure would let the compiler decide the layout, and the
 * hardware has already decided it.
 */
bool rtw89_bd_write(u8 *slot, u64 phys, u32 len, bool last) {
    if (!slot) return false;

    /* The address field is thirty-two bits.  A buffer above four gigabytes
     * cannot be described here at all, and truncating it silently would point
     * the card at somebody else's memory - so it is refused. */
    if (phys > 0xFFFFFFFFull) {
        if (!quiet_refusals)
            kerr("rtw89", "a request at %llx is above four gigabytes and this "
                          "card cannot address it",
                 (unsigned long long)phys);
        return false;
    }
    if (!len || len > 0xFFFFu) {
        if (!quiet_refusals)
            kerr("rtw89", "a request of %u bytes does not fit the length field",
                 len);
        return false;
    }

    u16 option = last ? RTW89_PCI_TXBD_OPTION_LS : 0;

    slot[0] = (u8)len;           slot[1] = (u8)(len >> 8);
    slot[2] = (u8)option;        slot[3] = (u8)(option >> 8);
    slot[4] = (u8)phys;          slot[5] = (u8)(phys >> 8);
    slot[6] = (u8)(phys >> 16);  slot[7] = (u8)(phys >> 24);
    return true;
}

/* How much room is left.
 *
 * One slot is always kept empty.  The two indexes being equal is how an empty
 * ring is spelled, so a ring filled to the last slot would read as empty and
 * the card would take nothing - which looks exactly like a card that has
 * stopped, and is the classic way a ring deadlocks with work in it.
 */
u16 rtw89_ring_free(const rtw89_ring_t *r) {
    if (!r || !r->slots) return 0;
    u16 used = (u16)((r->host_index + r->slots - r->card_index) % r->slots);
    return (u16)(r->slots - 1 - used);
}

bool rtw89_ring_add(rtw89_ring_t *r, u64 phys, u32 len, bool last) {
    if (!r || !r->bd || !r->slots) return false;
    if (!rtw89_ring_free(r)) return false;

    if (!rtw89_bd_write(r->bd + (size_t)r->host_index * RTW89_PCI_BD_BYTES,
                        phys, len, last))
        return false;

    r->host_index = (u16)((r->host_index + 1) % r->slots);
    return true;
}

/* ------------------------------------------- handing the ring over to the card
 *
 * Order matters twice here, and in opposite directions.
 *
 * Setting up: the address and the size go in before anything is added to the
 * ring, because a card told where to look while the ring is being filled may
 * look immediately and find descriptors that are not written yet.
 *
 * Adding work: the descriptors go into memory before the index is written.
 * The index is the doorbell - writing it is what sends the card to fetch - so
 * writing it first offers work that is not there.  On a machine where stores
 * can be reordered that is not a matter of putting the lines in order; the
 * writes have to be made to happen first, which is what the barrier below is
 * for.  It is the single most commonly missing line in ring code and its
 * absence shows up as a card that fetches stale descriptors under load and
 * works perfectly when anything is watching.
 */
bool rtw89_ring_attach(volatile u8 *regs, rtw89_ring_t *r,
                       u32 desa_lo_reg, u32 desa_hi_reg,
                       u32 num_reg, u32 idx_reg) {
    if (!regs || !r || !r->bd || !r->slots) return false;

    /* The ring's own address is written as two halves.  Unlike a descriptor's
     * address this pair is sixty-four bits, so a ring above four gigabytes is
     * describable - but only if both halves are written, and writing just the
     * low one is a mistake that works on every machine with little memory. */
    wr32(regs, desa_lo_reg, (u32)r->bd_phys);
    wr32(regs, desa_hi_reg, (u32)(r->bd_phys >> 32));

    wr32(regs, num_reg, r->slots);

    /* Both ends start at nothing.  The card's index is not written - it is the
     * card's to move - so the host's is set to zero to agree with it. */
    r->host_index = 0;
    r->card_index = 0;
    wr32(regs, idx_reg, 0);

    kinfo("rtw89", "a %u-slot ring at %llx handed to the card",
          r->slots, (unsigned long long)r->bd_phys);
    return true;
}

void rtw89_ring_doorbell(volatile u8 *regs, const rtw89_ring_t *r, u32 idx_reg) {
    if (!regs || !r) return;

    /* Everything written to the descriptors must be visible to the card before
     * the index that points at them is.  Without this the card can be sent to
     * fetch a descriptor whose bytes are still in a store buffer. */
    __asm__ volatile("sfence" ::: "memory");

    /* Only the host's half.  The card owns the other one and is using it. */
    u32 v = rd32(regs, idx_reg);
    v = (v & ~(u32)BD_HOST_IDX_MASK) | (r->host_index & BD_HOST_IDX_MASK);
    wr32(regs, idx_reg, v);
}

/* Where the card has got to, out of the half of that register it owns. */
u16 rtw89_ring_card_index(volatile u8 *regs, u32 idx_reg) {
    if (!regs) return 0;
    return (u16)((rd32(regs, idx_reg) & BD_CARD_IDX_MASK) >> BD_CARD_IDX_SHIFT);
}

/* ------------------------------------------------------ the card's replies
 *
 * The header is the same shape as the one on a request, read back out.
 *
 * Every check here is against a number the card supplied.  That is the whole
 * point: the length is not this driver's, and the two ways it can be wrong -
 * too small to contain its own header, or larger than what arrived - are both
 * ways to make a parser read memory that is not the packet.
 */
bool rtw89_c2h_parse(const u8 *packet, u32 received, rtw89_c2h_t *out) {
    if (!packet || !out) return false;

    if (received < H2C_HEADER_LEN) {
        kwarn("rtw89", "a reply of %u bytes is too short to have a header",
              received);
        return false;
    }

    u32 w0 = (u32)packet[0] | ((u32)packet[1] << 8) |
             ((u32)packet[2] << 16) | ((u32)packet[3] << 24);
    u32 w1 = (u32)packet[4] | ((u32)packet[5] << 8) |
             ((u32)packet[6] << 16) | ((u32)packet[7] << 24);

    u32 declared = w1 & H2C_HDR_TOTAL_LEN_MASK;

    /* Shorter than its own header: subtracting would wrap and produce a huge
     * payload length out of a small number. */
    if (declared < H2C_HEADER_LEN) {
        kwarn("rtw89", "a reply says it is %u bytes, which is less than its "
                       "own header", declared);
        return false;
    }

    /* Longer than what arrived: believing it reads past the buffer. */
    if (declared > received) {
        kwarn("rtw89", "a reply says it is %u bytes but only %u arrived",
              declared, received);
        return false;
    }

    out->category    = (u8)((w0 >> H2C_HDR_CAT_SHIFT)   & H2C_HDR_CAT_MASK);
    out->cls         = (u8)((w0 >> H2C_HDR_CLASS_SHIFT) & H2C_HDR_CLASS_MASK);
    out->func        = (u8)((w0 >> H2C_HDR_FUNC_SHIFT)  & H2C_HDR_FUNC_MASK);
    out->payload     = packet + H2C_HEADER_LEN;
    out->payload_len = declared - H2C_HEADER_LEN;
    return true;
}

/* ------------------------------------------------- did the firmware start?
 *
 * The card is not ready when the last byte is sent; it is ready when its
 * processor has checked what it was given and begun running it.  Those are
 * different moments and the gap between them is where a rejected image shows
 * up - as a status the card is holding, not as a failure of the send.
 */
bool rtw89_fw_wait_ready(volatile u8 *regs, int ms, const char *who) {
    if (!regs) return false;

    u8 status = RTW89_FWDL_INITIAL_STATE;

    for (int spent = 0; spent < ms; spent++) {
        status = rtw89_fwdl_status(regs);

        if (status == RTW89_FWDL_WCPU_FW_INIT_RDY) {
            kinfo("rtw89", "%s: the firmware is running after %d ms",
                  who, spent);
            return true;
        }

        /* The three ways the card says no.  Each is final - it will not become
         * ready by being waited on longer - so waiting out the whole timeout
         * on one of them wastes the time and then reports the wrong thing. */
        if (status == RTW89_FWDL_CHECKSUM_FAIL ||
            status == RTW89_FWDL_SECURITY_FAIL ||
            status == RTW89_FWDL_CV_NOT_MATCH) {
            if (!quiet_refusals)
                kerr("rtw89", "%s: the card refused the firmware - %s", who,
                     rtw89_fwdl_status_name(status));
            return false;
        }

        timer_mdelay(1);
    }

    if (!quiet_refusals)
        kerr("rtw89", "%s: the firmware did not start within %d ms; the card is "
                      "still saying \"%s\"", who, ms, rtw89_fwdl_status_name(status));
    return false;
}

/* BE firmware-download completion is per processor.  The WCPU status bits
 * report refusals for either image, but a cleared BBMCU0 download-enable bit
 * (not the WCPU bit) acknowledges the second executable. */
bool rtw89_fw_wait_bb0_ready(volatile u8 *regs, int ms, const char *who) {
    if (!regs || ms <= 0) return false;
    if (!who) who = "the card";
    for (int spent = 0; spent < ms; spent++) {
        u8 status = rtw89_fwdl_status(regs);
        if (status == RTW89_FWDL_CHECKSUM_FAIL ||
            status == RTW89_FWDL_SECURITY_FAIL ||
            status == RTW89_FWDL_CV_NOT_MATCH) {
            kerr("rtw89", "%s: BBMCU0 firmware refused: %s", who,
                 rtw89_fwdl_status_name(status));
            return false;
        }
        if (!(rd32(regs, R_BE_WCPU_FW_CTRL) & B_BE_BBMCU0_FWDL_EN))
            return true;
        timer_mdelay(1);
    }
    kerr("rtw89", "%s: BBMCU0 firmware did not finish within %d ms", who, ms);
    return false;
}

/* Download-complete and FreeRTOS-running are distinct Linux check types.
 * Cleared per-image enable bits alone must not publish a usable firmware. */
bool rtw89_fw_wait_running(volatile u8 *regs, int ms, const char *who) {
    if (!regs || ms <= 0) return false;
    for (int spent=0; spent<ms; spent++) {
        u32 ctrl=rd32(regs,R_BE_WCPU_FW_CTRL);
        if (ctrl==0xffffffffu) return false;
        u32 raw=(ctrl>>B_BE_WCPU_FWDL_STATUS_SHIFT)&B_BE_WCPU_FWDL_STATUS_MASK;
        if (raw>=4u && raw<=7u) return false;
        if (raw==3u && !(ctrl&(B_BE_WLANCPU_FWDL_EN|B_BE_BBMCU0_FWDL_EN)))
            return true;
        timer_mdelay(1);
    }
    kerr("rtw89", "%s: downloaded images did not enter firmware runtime", who?who:"the card");
    return false;
}

/* ====================================================== reaching the radio
 *
 * The radio is behind a serial interface rather than on the register bus, so
 * getting one of its registers is a small conversation: say which one, ask,
 * and wait for a different register to report the answer is ready.
 *
 * Three things here are easy to get wrong and each has a distinct symptom.
 *
 *   The interface has to be IDLE before it is addressed.  Writing an address
 *   while it is still working on the last request replaces that request, and
 *   what comes back is the new address read with the old request's timing -
 *   which is a plausible number from the wrong register.
 *
 *   Asking is a separate step from saying which one.  The address goes in
 *   first and the read bit second; both in one write means the interface sees
 *   the request before the address has settled.
 *
 *   Done and busy are different bits in the same register, and neither implies
 *   the other.  Waiting for "not busy" and then reading gets whatever was
 *   there from last time, because not-busy is also what it looks like before
 *   anything was asked.
 */

/* Poll one bit of a register until it reaches `want`, or give up.
 *
 * The bound is Realtek's: 3800 microseconds, polled every one.  It is long
 * because the serial interface is slow and short because a radio that is not
 * answering will never answer, and a driver that waits for ever on that takes
 * the machine with it. */
static bool poll_bit(volatile u8 *regs, u32 reg, u32 bit, bool want) {
    for (int spent = 0; spent < 3800; spent++) {
        u32 sample = rd32(regs, reg);
        if (sample == 0xffffffffu) return false; /* Absent PCIe device, not done. */
        bool now = (sample & bit) != 0;
        if (now == want) return true;
        timer_udelay(1);
    }
    return false;
}

static void wr32_masked(volatile u8 *regs, u32 reg, u32 mask, u32 value_shifted) {
    u32 v = rd32(regs, reg);
    v = (v & ~mask) | (value_shifted & mask);
    wr32(regs, reg, v);
}

/* The memory-mapped interface: no conversation, just a register.
 *
 * A radio register is twenty bits inside a thirty-two bit one, so a write is a
 * read-modify-write - the bits above are not ours to clear. */
static u32 rf_read_direct(volatile u8 *regs, int path, u32 addr) {
    u32 value = rd32(regs, RTW89_RF_DIRECT(path, addr));
    return value == 0xffffffffu ? RTW89_RF_INVALID : value & RTW89_RF_MASK;
}

static bool rf_write_direct(volatile u8 *regs, int path, u32 addr, u32 data) {
    u32 at = RTW89_RF_DIRECT(path, addr);
    u32 v = rd32(regs, at);
    if (v == 0xffffffffu) return false;
    v = (v & ~RTW89_RF_MASK) | (data & RTW89_RF_MASK);
    wr32(regs, at, v);
    timer_udelay(1);            /* it needs a moment to take */
    return true;
}

u32 rtw89_rf_read(volatile u8 *regs, int path, u32 addr) {
    if (!regs || path < 0 || path >= RTW89_RF_PATHS) return RTW89_RF_INVALID;
    if (!reachable((addr & RTW89_RF_ADDR_ADSEL_MASK) ?
                   RTW89_RF_DIRECT(path, addr) : R_HWSI_VAL(path)))
        return RTW89_RF_INVALID;

    if (addr & RTW89_RF_ADDR_ADSEL_MASK)
        return rf_read_direct(regs, path, addr);

    u32 ask = R_HWSI_ADD(path);
    u32 answer = R_HWSI_VAL(path);

    /* Put the interface into the mode where it will take an address. */
    wr32_masked(regs, ask, B_HWSI_ADD_CTL_MASK, 0x1);

    if (!poll_bit(regs, answer, B_HWSI_VAL_BUSY, false)) {
        kwarn("rtw89", "the radio's interface stayed busy before a read");
        return RTW89_RF_INVALID;
    }

    /* Which register, and only then the asking. */
    wr32_masked(regs, ask, B_HWSI_ADD_MASK, addr << B_HWSI_ADD_SHIFT);
    wr32_masked(regs, ask, B_HWSI_ADD_RD, B_HWSI_ADD_RD);
    timer_udelay(2);

    u32 value = RTW89_RF_INVALID;
    if (poll_bit(regs, answer, B_HWSI_VAL_RDONE, true))
        value = rd32(regs, answer) & RTW89_RF_MASK;
    else
        kwarn("rtw89", "the radio did not answer for register %03x", addr);

    /* Stop polling, whichever way it went.  Left set, the interface keeps
     * working on a request nobody is waiting for and is busy the next time
     * somebody asks. */
    wr32_masked(regs, ask, B_HWSI_ADD_POLL_MASK, 0);
    return value;
}

bool rtw89_rf_write(volatile u8 *regs, int path, u32 addr, u32 data) {
    if (!regs || path < 0 || path >= RTW89_RF_PATHS) return false;
    if (!reachable((addr & RTW89_RF_ADDR_ADSEL_MASK) ?
                   RTW89_RF_DIRECT(path, addr) : R_HWSI_VAL(path))) return false;

    if (addr & RTW89_RF_ADDR_ADSEL_MASK)
        return rf_write_direct(regs, path, addr, data);

    if (!poll_bit(regs, R_HWSI_VAL(path), B_HWSI_VAL_BUSY, false)) {
        kwarn("rtw89", "the radio's interface stayed busy before a write");
        return false;
    }

    /* One word carries both: which register in the low byte, what to put in it
     * above.  Written whole rather than as two masked writes - a write is not
     * a conversation like a read is, and splitting it would leave the address
     * standing on its own for a moment with the previous value beside it. */
    u32 word = (addr & B_HWSI_DATA_ADDR_MASK) |
               ((data << B_HWSI_DATA_VAL_SHIFT) & B_HWSI_DATA_VAL_MASK);
    wr32(regs, R_HWSI_DATA(path), word);
    return true;
}

/* ================================================== what comes back up ====
 *
 * The other direction.  A descriptor here says where the card may put a packet
 * rather than where one is, and the card writes a word in front of what it
 * puts there.
 */

bool rtw89_rx_bd_write(u8 *slot, u64 phys, u32 size) {
    if (!slot) return false;

    /* Same thirty-two bit limit as the sending side, and the same reason for
     * refusing rather than truncating: the card would fetch, or in this
     * direction WRITE, at the truncated address. */
    if (phys > 0xFFFFFFFFull) {
        if (!quiet_refusals)
            kerr("rtw89", "a receive buffer at %llx is above four gigabytes and "
                          "this card cannot reach it", (unsigned long long)phys);
        return false;
    }
    if (!size || size > 0xFFFFu) {
        if (!quiet_refusals)
            kerr("rtw89", "a receive buffer of %u bytes does not fit the size "
                          "field", size);
        return false;
    }

    slot[0] = (u8)size;          slot[1] = (u8)(size >> 8);
    slot[2] = 0;                 slot[3] = 0;      /* no options wanted */
    slot[4] = (u8)phys;          slot[5] = (u8)(phys >> 8);
    slot[6] = (u8)(phys >> 16);  slot[7] = (u8)(phys >> 24);
    return true;
}

bool rtw89_rx_parse(const u8 *buffer, u32 arrived, rtw89_rx_info_t *out) {
    if (!buffer || !out) return false;

    /* The word itself has to have arrived before anything it says can be
     * believed - and it is the first thing checked, because everything below
     * is a number the card chose. */
    if (arrived < 4) return false;

    u32 w = (u32)buffer[0] | ((u32)buffer[1] << 8) |
            ((u32)buffer[2] << 16) | ((u32)buffer[3] << 24);

    u32 bytes = w & RXBD_WRITE_SIZE_MASK;

    /* What it says it wrote cannot be more than what came, and a packet of
     * nothing is not a packet.  Both are how a confused card turns into a read
     * past the end of a buffer. */
    if (!bytes || bytes + 4 > arrived) return false;

    out->bytes = bytes;
    out->first = (w & RXBD_FS) != 0;
    out->last  = (w & RXBD_LS) != 0;
    out->tag   = (u16)((w >> RXBD_TAG_SHIFT) & RXBD_TAG_MASK);
    return true;
}

/* The card's own count, checked rather than ignored.
 *
 * It wraps at 0x1fff and skips zero - a tag of zero means the card never set
 * one, which is a different thing from tag number zero and must not be treated
 * as a packet in sequence.
 */
bool rtw89_rx_tag_ok(u16 *expected, u16 got, int *missed) {
    if (missed) *missed = 0;
    if (!expected) return true;

    /* Nothing expected yet: take whatever arrives as the starting point rather
     * than reporting every packet after a reset as a loss. */
    if (!*expected) {
        *expected = got;
    }

    if (got == *expected) {
        *expected = (u16)(got + 1);
        if (*expected > RTW89_RX_TAG_MAX) *expected = 1;   /* zero is not one */
        return true;
    }

    /* How many went by without arriving, the long way round if it wrapped. */
    int gap = (int)got - (int)*expected;
    if (gap < 0) gap += RTW89_RX_TAG_MAX;
    if (missed) *missed = gap;

    /* Carry on from what actually arrived.  Staying on the expected one would
     * report every packet from here as missing rather than the ones that were.
     */
    *expected = (u16)(got + 1);
    if (*expected > RTW89_RX_TAG_MAX) *expected = 1;
    return false;
}

/* ================================================ what the card passes up */

static u32 filter_reg(rtw89_frame_kind_t kind) {
    switch (kind) {
    case RTW89_FRAME_MGMT: return R_BE_MGNT_FLTR;
    case RTW89_FRAME_CTRL: return R_BE_CTRL_FLTR;
    default:               return R_BE_DATA_FLTR;
    }
}

bool rtw89_rx_filter_set(volatile u8 *regs, int mac, rtw89_frame_kind_t kind,
                         bool accept) {
    if (!regs || mac < 0 || mac >= RTW89_MACS) return false;

    u32 reg = filter_reg(kind) + (u32)mac * RTW89_MAC_STRIDE;
    wr32(regs, reg, accept ? RX_FLTR_ACCEPT : RX_FLTR_DROP);
    return true;
}

bool rtw89_rx_filter_station(volatile u8 *regs, int mac) {
    /* Management, because joining a network is done with management frames and
     * a machine that drops them can never join one.
     *
     * Data, because that is the traffic.
     *
     * Control dropped, because those are answered by the hardware in
     * microseconds and passing them up gives software a great deal to look at
     * and nothing to do about any of it. */
    bool ok = rtw89_rx_filter_set(regs, mac, RTW89_FRAME_MGMT, true);
    ok = ok && rtw89_rx_filter_set(regs, mac, RTW89_FRAME_DATA, true);
    ok = ok && rtw89_rx_filter_set(regs, mac, RTW89_FRAME_CTRL, false);
    return ok;
}

/* ============================================ asking, and being answered ==
 *
 * One request, out through the ring the card fetches from, and its answer back
 * through the ring the card fills.
 *
 * The order below is the whole of it and none of it is adjustable:
 *
 *   The answer's buffer is posted BEFORE the question is asked.  A card that
 *   answers quickly answers into whatever is waiting, and if nothing is
 *   waiting the answer is dropped - which looks exactly like a card that did
 *   not reply.
 *
 *   The request is written into memory before the descriptor points at it, and
 *   the descriptor before the doorbell.  Each of those is a fence, and the
 *   ring code already places them.
 *
 *   The answer is matched by its sequence number, not merely taken.  Two
 *   requests in flight is the normal state of a driver doing anything, and a
 *   driver that takes the first answer to arrive pairs them up wrongly exactly
 *   when the card is busiest.
 */
int rtw89_ask(volatile u8 *regs, rtw89_channel_t *ch, u8 cls, u8 func,
              const void *payload, u16 len, void *reply, u32 reply_cap,
              int timeout_ms) {
    return rtw89_ask_as(regs, ch, H2C_CAT_MAC, cls, func, cls, func,
                        payload, len, reply, reply_cap, timeout_ms);
}

int rtw89_ask_as(volatile u8 *regs, rtw89_channel_t *ch,
                 u8 cat, u8 cls, u8 func,
                 u8 reply_cls, u8 reply_func,
                 const void *payload, u16 len, void *reply, u32 reply_cap,
                 int timeout_ms) {
    if (!regs || !ch || !ch->tx_buffer || !ch->rx_buffer) return -1;
    if ((u32)len + H2C_HEADER_LEN > 0x3FFF) return -1;

    /* Somewhere for the answer to land, first. */
    memset(ch->rx_buffer, 0, ch->rx_buffer_size);
    if (!rtw89_rx_bd_write(ch->rx.bd + (size_t)ch->rx.host_index * RTW89_PCI_RXBD_BYTES,
                           ch->rx_buffer_phys, ch->rx_buffer_size))
        return -1;
    ch->rx.host_index = (u16)((ch->rx.host_index + 1) % ch->rx.slots);
    rtw89_ring_doorbell(regs, &ch->rx, R_BE_RXQ0_RXBD_IDX_V1);

    /* Then the question. */
    u8 seq = ch->sequence++;
    rtw89_h2c_header(ch->tx_buffer, cat, cls, func, 0, seq, len,
                     true, false);
    if (payload && len) memcpy(ch->tx_buffer + H2C_HEADER_LEN, payload, len);

    if (!rtw89_ring_add(&ch->tx, ch->tx_buffer_phys,
                        (u32)len + H2C_HEADER_LEN, true)) {
        kwarn("rtw89", "the request ring is full");
        return -1;
    }
    rtw89_ring_doorbell(regs, &ch->tx, R_BE_CH12_TXBD_IDX);

    /* And the answer.  The card moves the receiving ring's own index as it
     * fills buffers, which is how "something arrived" is known without an
     * interrupt. */
    for (int spent = 0; spent < timeout_ms; spent++) {
        u16 filled = rtw89_ring_card_index(regs, R_BE_RXQ0_RXBD_IDX_V1);

        if (filled != ch->rx.card_index) {
            ch->rx.card_index = filled;

            rtw89_rx_info_t info;
            if (!rtw89_rx_parse(ch->rx_buffer, ch->rx_buffer_size, &info)) {
                kwarn("rtw89", "the card filled a buffer with something that "
                               "is not a packet");
                return -1;
            }

            int missed = 0;
            if (!rtw89_rx_tag_ok(&ch->expected_tag, info.tag, &missed))
                kwarn("rtw89", "%d answer(s) went missing before this one",
                      missed);

            /* Past the card's own word, then the reply's own header. */
            const u8 *body = ch->rx_buffer + 4;
            rtw89_c2h_t answer;
            if (!rtw89_c2h_parse(body, info.bytes, &answer)) return -1;

            /* The one that was expected, rather than the one that came. */
            if (answer.cls != reply_cls || answer.func != reply_func) {
                kwarn("rtw89", "expected class %u function %u in reply to "
                               "class %u function %u, and was answered class "
                               "%u function %u", reply_cls, reply_func,
                      cls, func, answer.cls, answer.func);
                return -1;
            }

            u32 n = answer.payload_len;
            if (n > reply_cap) n = reply_cap;
            if (reply && n) memcpy(reply, answer.payload, n);
            return (int)n;
        }
        timer_mdelay(1);
    }

    kwarn("rtw89", "no answer to class %u function %u within %d ms", cls, func,
          timeout_ms);
    return -1;
}

/* ========================================= setting the packet engine up ==
 *
 * Three short sequences that have to run after the firmware is up, before the
 * chip will move a packet.  The longer ones beside them - the memory quotas,
 * the flow control - are tables and are not here.
 */

bool rtw89_sched_init(volatile u8 *regs) {
    if (!regs) return false;

    /* sta_sch_init_be(): enable with a byte access; status uses 32 bits. */
    set8(regs, R_BE_SS_CTRL, B_BE_SS_EN);

    /* It says when it has finished, and waiting for that is not optional: the
     * registers below are part of the same block and writing them while it is
     * still starting is writing into something that is being reset. */
    bool ready = false;
    for (unsigned spent = 0; spent < TRXCFG_WAIT_CNT; spent++) {
        u32 status = rd32(regs, R_BE_SS_CTRL);
        if (status == 0xFFFFFFFFu) return false;
        if (status & B_BE_SS_INIT_DONE) { ready = true; break; }
        timer_udelay(1);
    }
    if (!ready) {
        kerr("rtw89", "the scheduler did not finish starting");
        return false;
    }

    set32(regs, R_BE_SS_CTRL, B_BE_WARM_INIT);
    clr32(regs, R_BE_SS_CTRL, B_BE_BAND_TRIG_EN | B_BE_BAND1_TRIG_EN);
    return true;
}

bool rtw89_security_init(volatile u8 *regs) {
    if (!regs) return false;

    /* Each kind of frame is switched on separately, and all of them are
     * needed.  A driver that enables the engine and the unicast paths but
     * forgets broadcast receives everything except what a network sends to
     * everyone - which is the beacons and the group traffic, so it joins
     * nothing and cannot say why. */
    set32(regs, R_BE_SEC_ENG_CTRL,
          B_BE_CLK_EN_CGCMP | B_BE_CLK_EN_WAPI | B_BE_CLK_EN_WEP_TKIP |
          B_BE_SEC_TX_ENC | B_BE_SEC_RX_DEC |
          B_BE_MC_DEC | B_BE_BC_DEC |
          B_BE_BMC_MGNT_DEC | B_BE_UC_MGNT_DEC |
          B_BE_SEC_PRE_ENQUE_TX);

    /* The two things encryption adds to a frame, which the hardware appends
     * and the other end checks. */
    set32(regs, R_BE_SEC_MPDU_PROC, B_BE_APPEND_ICV | B_BE_APPEND_MIC);
    return true;
}

bool rtw89_mpdu_init(volatile u8 *regs) {
    if (!regs) return false;

    /* The frame check sequence, appended by the hardware.  Every frame carries
     * one and a receiver drops any frame whose does not match, so a chip told
     * not to append it transmits frames the whole world discards. */
    set32(regs, R_BE_MPDU_PROC, B_BE_APPEND_FCS);
    wr32(regs, R_BE_CUT_AMSDU_CTRL, TRXCFG_MPDU_PROC_CUT_CTRL);

    /* Preserve v6.17's write32_set semantics, including the existing MLD bit.
     * The source clears it in the local value, but then ORs with the register;
     * do not silently substitute a different hardware write here. */
    u32 shortcut = rd32(regs, R_BE_HDR_SHCUT_SETTING);
    shortcut |= B_BE_TX_HW_SEQ_EN | B_BE_TX_HW_ACK_POLICY_EN |
                B_BE_TX_MAC_MPDU_PROC_EN;
    shortcut &= ~B_BE_TX_ADDR_MLD_TO_LIK;
    set32(regs, R_BE_HDR_SHCUT_SETTING, shortcut);
    wr32(regs, R_BE_RX_HDRTRNS, TRXCFG_MPDU_PROC_RX_HDR_CONV);

    u32 forward = rd32(regs, R_BE_DISP_FWD_WLAN_0);
    forward &= ~(B_BE_FWD_WLAN_CPU_TYPE_0_DATA_MASK |
                 B_BE_FWD_WLAN_CPU_TYPE_0_MNG_MASK |
                 B_BE_FWD_WLAN_CPU_TYPE_0_CTL_MASK |
                 B_BE_FWD_WLAN_CPU_TYPE_1_MASK);
    forward |= 1u | (1u << 2) | (1u << 4) | (1u << 6);
    wr32(regs, R_BE_DISP_FWD_WLAN_0, forward);
    return true;
}

/* Build the table that tracks which link a station is on, and switch multi-link
 * on once it is there.
 *
 * The reinit is a pulse rather than a setting: set, clear, then wait.  Leaving
 * it set holds the table in reset and the wait never ends - which looks like a
 * chip without multi-link rather than a driver that asked for the table and
 * never let go of it.
 */
bool rtw89_mlo_init(volatile u8 *regs) {
    if (!regs) return false;

    u32 ctl = rd32(regs, R_BE_MLO_INIT_CTL);
    wr32(regs, R_BE_MLO_INIT_CTL, ctl | B_BE_MLO_TABLE_REINIT);
    wr32(regs, R_BE_MLO_INIT_CTL, ctl & ~(u32)B_BE_MLO_TABLE_REINIT);

    bool built = false;
    for (int spent = 0; spent < 1000; spent++) {
        if (rd32(regs, R_BE_MLO_INIT_CTL) & B_BE_MLO_TABLE_INIT_DONE) {
            built = true;
            break;
        }
        timer_udelay(1);
    }
    if (!built) {
        kerr("rtw89", "the link table was not built, so this card cannot hold "
                      "more than one link");
        return false;
    }

    /* Only now.  Switching the hardware's link changing on before the table
     * exists points it at a table that is not there. */
    set32(regs, R_BE_SS_CTRL, B_BE_MLO_HW_CHGLINK_EN);
    set32(regs, R_BE_CMAC_SHARE_ACQCHK_CFG_0, B_BE_R_MACID_ACQ_CHK_EN);
    return true;
}

/* Do the numbers dividing the card's memory describe the memory it has?
 *
 * This is arithmetic rather than a comparison against a source, and that makes
 * it a better check than a diff: a number can be transcribed correctly from
 * the wrong row and a diff will not notice, but the total will be wrong.
 *
 * Realtek's own driver makes this check before programming the division, which
 * is a good sign it is worth making - they presumably got it wrong once too.
 */
bool rtw89_memory_split_adds_up(void) {
    u32 wde = RTW89_WDE_PAGE_BYTES *
              (RTW89_WDE_LINKED_PAGES + RTW89_WDE_UNLINKED_PAGES);
    u32 ple = RTW89_PLE_PAGE_BYTES *
              (RTW89_PLE_LINKED_PAGES + RTW89_PLE_UNLINKED_PAGES);
    u32 used = wde + ple + RTW89_DLE_RESERVED_BYTES;

    if (used != RTW89_8922A_FIFO_BYTES) {
        kerr("rtw89", "the division of the card's memory comes to %u bytes and "
                      "the card has %u: %u for tracking packets, %u for the "
                      "packets, %u set aside",
             used, RTW89_8922A_FIFO_BYTES, wde, ple,
             (unsigned)RTW89_DLE_RESERVED_BYTES);
        return false;
    }

    kinfo("rtw89", "the card's memory divides exactly: %u bytes tracking "
                   "packets, %u holding them, %u set aside, %u in all",
          wde, ple, (unsigned)RTW89_DLE_RESERVED_BYTES, used);
    return true;
}

/* Divide the card's memory, and switch the two engines that use it on.
 *
 * The order is the whole of it: the engines are switched OFF, their clocks
 * switched on, the division written, and only then are they switched on
 * again.  Writing the division into a running engine changes where it thinks
 * its pages are while it is using them.
 *
 * The clocks are the part that looks redundant and is not.  An engine that is
 * off still has to be clocked to accept being configured; configuring an
 * unclocked block writes into something that is not listening, and every
 * register reads back as whatever it held before.
 */
bool rtw89_dle_init(volatile u8 *regs) {
    if (!regs) return false;
    if (!reachable(R_AX_PLE_INI_STATUS) ||
        rd32(regs, R_BE_DMAC_FUNC_EN) == 0xffffffffu ||
        rd32(regs, R_BE_DMAC_CLK_EN) == 0xffffffffu ||
        rd32(regs, R_BE_WDE_PKTBUF_CFG) == 0xffffffffu ||
        rd32(regs, R_BE_PLE_PKTBUF_CFG) == 0xffffffffu) return false;

    if (!rtw89_memory_split_adds_up()) {
        kerr("rtw89", "the memory division was not written, because it does "
                      "not describe this card's memory");
        return false;
    }

    /* Off, then clocked. */
    clr32(regs, R_BE_DMAC_FUNC_EN, B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN);
    set32(regs, R_BE_DMAC_CLK_EN, B_BE_DLE_WDE_CLK_EN | B_BE_DLE_PLE_CLK_EN);

    /* The queue engine: 64-byte pages, starting at the beginning. */
    u32 wde = rd32(regs, R_BE_WDE_PKTBUF_CFG);
    wde = (wde & ~B_BE_WDE_PAGE_SEL_MASK) | WDE_PAGE_SEL_64;
    wde = (wde & ~B_BE_WDE_START_BOUND_MASK) | ((0u << 8) & B_BE_WDE_START_BOUND_MASK);
    wde = (wde & ~B_BE_WDE_FREE_PAGE_NUM_MASK) |
          ((RTW89_WDE_LINKED_PAGES << 16) & B_BE_WDE_FREE_PAGE_NUM_MASK);
    wr32(regs, R_BE_WDE_PKTBUF_CFG, wde);

    /* And the payload engine: 128-byte pages, starting where the queue engine
     * leaves off.  The start is in units of eight kilobytes, not bytes - a
     * region written in bytes lands eight thousand times too far along, and
     * the card does not object. */
    u32 ple = rd32(regs, R_BE_PLE_PKTBUF_CFG);
    u32 bound = RTW89_PLE_START_OFFSET / DLE_BOUND_UNIT;
    ple = (ple & ~B_BE_PLE_PAGE_SEL_MASK) | PLE_PAGE_SEL_128;
    ple = (ple & ~B_BE_PLE_START_BOUND_MASK) |
          ((bound << 8) & B_BE_PLE_START_BOUND_MASK);
    ple = (ple & ~B_BE_PLE_FREE_PAGE_NUM_MASK) |
          ((RTW89_PLE_LINKED_PAGES << 16) & B_BE_PLE_FREE_PAGE_NUM_MASK);
    wr32(regs, R_BE_PLE_PKTBUF_CFG, ple);

    /* RTL8922A PCIe SCC: wde_qt0_v1, ple_qt0/ple_qt1, Linux v6.17
     * mac.c. WDE Q2 is explicitly zero; its array index is not pkt_in.
     * Program ALL quotas while engines are off, before waiting for ready. */
    static const u16 wde_quota[5] = {3302, 6, 0, 0, 20};
    static const u16 ple_min[13] =
        {320, 320, 32, 16, 13, 13, 292, 292, 64, 18, 1, 4, 0};
    static const u16 ple_max[13] =
        {320, 320, 32, 16, 1316, 1316, 1595, 1595, 1367, 1321, 1, 1307, 0};
    for (unsigned i = 0; i < 5; ++i)
        wr32(regs, R_BE_WDE_QTA0_CFG + i * 4,
             (u32)wde_quota[i] | (u32)wde_quota[i] << 16);
    for (unsigned i = 0; i < 13; ++i)
        wr32(regs, R_BE_PLE_QTA0_CFG + i * 4,
             (u32)ple_min[i] | (u32)ple_max[i] << 16);

    /* And on. */
    set32(regs, R_BE_DMAC_FUNC_EN, B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN);

    /* Each manager has its own 2 ms initialization budget. An inaccessible
     * device reads all ones; those bits must not masquerade as readiness.
     * On failure disable both engines, leaving no half-ready packet path. */
    const u32 status_regs[2] = {R_AX_WDE_INI_STATUS, R_AX_PLE_INI_STATUS};
    for (unsigned engine = 0; engine < 2; ++engine) {
        bool ready = false;
        for (unsigned us = 0; us < 2000; ++us) {
            u32 status = rd32(regs, status_regs[engine]);
            if (status == 0xffffffffu) break;
            if ((status & RTW89_DLE_INIT_READY) == RTW89_DLE_INIT_READY) {
                ready = true;
                break;
            }
            timer_udelay(1);
        }
        if (!ready) {
            clr32(regs, R_BE_DMAC_FUNC_EN, B_BE_DLE_WDE_EN | B_BE_DLE_PLE_EN);
            kerr("rtw89", "runtime %s packet memory did not initialize",
                 engine ? "PLE" : "WDE");
            return false;
        }
    }

    kinfo("rtw89", "the card's memory is divided: %u pages of %u bytes for "
                   "tracking, %u of %u for the packets, starting %u kilobytes "
                   "in",
          (unsigned)RTW89_WDE_LINKED_PAGES, (unsigned)RTW89_WDE_PAGE_BYTES,
          (unsigned)RTW89_PLE_LINKED_PAGES, (unsigned)RTW89_PLE_PAGE_BYTES,
          RTW89_PLE_START_OFFSET / 1024u);
    return true;
}

/* Tell the card how to hold the host back.
 *
 * Each channel keeps a reserve of pages it may always use; the rest are
 * shared, and a channel is full when its reserve is gone.  The condition that
 * decides "full" is zero here, which means the reserve alone - the other
 * settings count shared pages too and are for chips that share differently.
 *
 * The failure this prevents does not announce itself.  A reserve larger than
 * the memory means a channel is never full, so the host keeps handing over
 * packets with nowhere to go; a reserve of nothing means always full, and
 * nothing is sent at all.  Both look like a card that has stopped.
 */
bool rtw89_flow_control_init(volatile u8 *regs) {
    if (!regs) return false;
    if (!reachable(R_BE_WP_PAGE_INFO1) ||
        rd32(regs, R_BE_HCI_FC_CTRL) == 0xffffffffu) return false;

    /* Linux hfc_init(reset=true, en=true, h2c=true), RTL8922A PCIe SCC.
     * Stop both credit engines while replacing download-only configuration. */
    clr32(regs, R_BE_HCI_FC_CTRL, B_BE_HCI_FC_EN | B_BE_HCI_FC_CH12_EN);
    for (unsigned ch = 0; ch < 12; ++ch) {
        bool group1 = (ch >= 4 && ch < 8) || ch >= 10;
        wr32(regs, R_BE_CH0_PAGE_CTRL + ch * 4,
             2u | (1641u << 16) | (group1 ? (1u << 31) : 0));
    }
    wr32(regs, R_BE_PUB_PAGE_CTRL1, 1651u | (1651u << 16));
    wr32(regs, R_BE_WP_PAGE_CTRL2, 0);

    /* What each channel keeps for itself: the ordinary ones and the one
     * commands travel on, which needs more because a command must never wait
     * behind traffic. */
    u32 pages = ((RTW89_HFC_CH011_RESERVE << 0) & B_BE_PREC_PAGE_CH011_V1_MASK) |
                ((RTW89_HFC_H2C_RESERVE << 16) & B_BE_PREC_PAGE_CH12_V1_MASK);
    wr32(regs, R_BE_CH_PAGE_CTRL, pages);

    /* And what is shared between them. */
    wr32(regs, R_BE_PUB_PAGE_CTRL2,
         (RTW89_HFC_PUBLIC_PAGES << 0) & B_BE_PUBPG_ALL_MASK);

    /* The write-pointer channels keep nothing of their own on this part. */
    wr32(regs, R_BE_WP_PAGE_CTRL1, 0);

    /* Which way of counting, and what counts as full.  Read first: this
     * register carries other things that are not ours to clear. */
    u32 ctrl = rd32(regs, R_BE_HCI_FC_CTRL);
    ctrl = (ctrl & ~B_BE_HCI_FC_MODE_MASK) |
           ((RTW89_HCIFC_POH << 1) & B_BE_HCI_FC_MODE_MASK);
    ctrl &= ~B_BE_HCI_FC_WD_FULL_COND_MASK;      /* full when the reserve is */
    ctrl &= ~B_BE_HCI_FC_CH12_FULL_COND_MASK;    /* gone, and not before     */
    ctrl &= ~(B_BE_HCI_FC_WP_CH07_FULL_COND_MASK |
              B_BE_HCI_FC_WP_CH811_FULL_COND_MASK);
    wr32(regs, R_BE_HCI_FC_CTRL, ctrl);

    set32(regs, R_BE_HCI_FC_CTRL, B_BE_HCI_FC_EN | B_BE_HCI_FC_CH12_EN);
    timer_udelay(10);
    /* The original helper never enabled flow control at all. Confirm the
     * write and reject lost PCIe access instead of reporting usable queues. */
    u32 active = rd32(regs, R_BE_HCI_FC_CTRL);
    if (active == 0xffffffffu ||
        (active & (B_BE_HCI_FC_EN | B_BE_HCI_FC_CH12_EN)) !=
        (B_BE_HCI_FC_EN | B_BE_HCI_FC_CH12_EN)) goto failed;
    for (unsigned ch = 0; ch < 12; ++ch) {
        bool group1 = (ch >= 4 && ch < 8) || ch >= 10;
        u32 expected = 2u | (1641u << 16) | (group1 ? (1u << 31) : 0);
        if (rd32(regs, R_BE_CH0_PAGE_CTRL + ch * 4) != expected ||
            rd32(regs, R_BE_CH0_PAGE_INFO + ch * 4) == 0xffffffffu) goto failed;
    }
    if (rd32(regs, R_BE_PUB_PAGE_INFO1) == 0xffffffffu ||
        rd32(regs, R_BE_PUB_PAGE_INFO2) == 0xffffffffu ||
        rd32(regs, R_BE_PUB_PAGE_INFO3) == 0xffffffffu ||
        rd32(regs, R_BE_WP_PAGE_INFO1) == 0xffffffffu) goto failed;

    kinfo("rtw89", "flow control: %u pages kept for each channel, %u for "
                   "commands, %u shared",
          (unsigned)RTW89_HFC_CH011_RESERVE, (unsigned)RTW89_HFC_H2C_RESERVE,
          (unsigned)RTW89_HFC_PUBLIC_PAGES);
    return true;
failed:
    clr32(regs, R_BE_HCI_FC_CTRL, B_BE_HCI_FC_EN | B_BE_HCI_FC_CH12_EN);
    kerr("rtw89", "runtime flow-control initialization/readback failed");
    return false;
}

/* Linux v6.17 preload_init_be(): preserve queue-group allocations, write the
 * maximum size/enable first, then the next-window/minimum-reserve fields.
 * This configures MAC0 only and assumes runtime DLE quotas are already ready.
 */
bool rtw89_preload_init(volatile u8 *regs) {
    if (!regs) return false;
    u32 most = PRELD_B0_ENT_NUM * PRELD_AMSDU_SIZE;
    u32 cfg0 = rd32(regs, R_BE_TXPKTCTL_B0_PRELD_CFG0);
    cfg0 = (cfg0 & ~B_BE_B0_PRELD_USEMAXSZ_MASK) |
           ((most << 16) & B_BE_B0_PRELD_USEMAXSZ_MASK);
    cfg0 |= B_BE_B0_PRELD_FEN;
    wr32(regs, R_BE_TXPKTCTL_B0_PRELD_CFG0, cfg0);

    u32 cfg1 = rd32(regs, R_BE_TXPKTCTL_B0_PRELD_CFG1);
    cfg1 = (cfg1 & ~B_BE_B0_PRELD_NXT_TXENDWIN_MASK) |
           ((PRELD_NEXT_WND << 8) & B_BE_B0_PRELD_NXT_TXENDWIN_MASK);
    cfg1 = (cfg1 & ~B_BE_B0_PRELD_NXT_RSVMINSZ_MASK) |
           (PRELD_AMSDU_SIZE & B_BE_B0_PRELD_NXT_RSVMINSZ_MASK);
    wr32(regs, R_BE_TXPKTCTL_B0_PRELD_CFG1, cfg1);
    kinfo("rtw89", "MAC0 preload: maximum %u bytes, reserved %u bytes",
          most, (unsigned)PRELD_AMSDU_SIZE);
    return true;
}

/* Tell the transmit path where to keep its description of each packet.
 *
 * The table sits in the memory kept back past the free pages, and the card is
 * given the page it starts at rather than an address - the same numbering the
 * memory division used.
 *
 * The switch goes on with the address, for the reason it did in the fetching
 * above: on first, and the path uses whatever page number was there before.
 */
bool rtw89_txpktctl_init(volatile u8 *regs) {
    if (!regs) return false;

    u32 cfg = rd32(regs, R_BE_TXPKTCTL_MPDUINFO_CFG);
    cfg = (cfg & ~B_BE_MPDUINFO_PKTID_MASK) |
          ((RTW89_RSVD_FIRST_PAGE << 16) & B_BE_MPDUINFO_PKTID_MASK);
    cfg = (cfg & ~B_BE_MPDUINFO_B1_BADDR_MASK) |
          (MPDU_INFO_B1_OFST & B_BE_MPDUINFO_B1_BADDR_MASK);
    cfg |= B_BE_MPDUINFO_FEN;
    wr32(regs, R_BE_TXPKTCTL_MPDUINFO_CFG, cfg);

    kinfo("rtw89", "the packet description table starts at page %u, in the "
                   "memory kept back", (unsigned)RTW89_RSVD_FIRST_PAGE);
    return true;
}

/* The transmit side's timing.
 *
 * Both settings are per radio, and the second radio's copy is 0x4000 further
 * on - the same stride the receive filters use.
 */
bool rtw89_tmac_init(volatile u8 *regs, int mac) {
    if (!regs || mac < 0 || mac >= RTW89_MACS) return false;
    u32 at = (u32)mac * RTW89_MAC_STRIDE;

    /* A keep-alive frame carries nothing, so it should not count against this
     * radio's share of the air.  Counting it makes the card believe it has
     * used a turn it did not use, and it then waits when it could send. */
    clr32(regs, R_BE_TB_PPDU_CTRL + at, B_BE_QOSNULL_UPD_MUEDCA_EN);

    /* How long to wait between the training symbols at the start of a frame
     * and the data after them.  Too short and the receiver is still settling
     * when the data starts, which costs a frame rather than an error. */
    u32 t = rd32(regs, R_BE_WMTX_TCR_BE_4 + at);
    t = (t & ~B_BE_EHT_HE_PPDU_4XLTF_ZLD_USTIMER_MASK) |
        ((RTW89_ZLD_USTIMER_4XLTF << 24) &
         B_BE_EHT_HE_PPDU_4XLTF_ZLD_USTIMER_MASK);
    t = (t & ~B_BE_EHT_HE_PPDU_2XLTF_ZLD_USTIMER_MASK) |
        ((RTW89_ZLD_USTIMER_2XLTF << 16) &
         B_BE_EHT_HE_PPDU_2XLTF_ZLD_USTIMER_MASK);
    wr32(regs, R_BE_WMTX_TCR_BE_4 + at, t);
    return true;
}

/* The receive side: when to give up on a frame, and how large a frame may be.
 *
 * The largest frame is the smallest of three limits, and computing it rather
 * than writing down the answer is deliberate - the three come from different
 * places and the smallest is not always the same one.
 */
bool rtw89_rmac_init(volatile u8 *regs, int mac) {
    if (!regs || mac < 0 || mac >= RTW89_MACS) return false;
    u32 at = (u32)mac * RTW89_MAC_STRIDE;

    /* Give up on a frame that started and did not finish.  Without these the
     * receiver can be held open by a burst of noise for as long as the noise
     * lasts, and every real frame during it is lost. */
    u32 dlk = rd32(regs, R_BE_DLK_PROTECT_CTL + at);
    dlk = (dlk & ~B_BE_RX_DLK_DATA_TIME_MASK) |
          ((TRXCFG_RMAC_DATA_TO << 4) & B_BE_RX_DLK_DATA_TIME_MASK);
    dlk = (dlk & ~B_BE_RX_DLK_CCA_TIME_MASK) |
          ((TRXCFG_RMAC_CCA_TO << 8) & B_BE_RX_DLK_CCA_TIME_MASK);
    wr32(regs, R_BE_DLK_PROTECT_CTL + at, dlk);

    /* The largest frame: the pages this radio has, what the standard allows,
     * and then in units of five hundred and twelve bytes because that is what
     * the field counts. */
    u32 pages = PLD_RLS_MAX_PG;
    u32 bytes = pages * RTW89_PLE_PAGE_BYTES;
    if (bytes > RX_SPEC_MAX_LEN) bytes = RX_SPEC_MAX_LEN;
    u32 units = bytes / RX_MAX_LEN_UNIT;

    u32 opt = rd32(regs, R_BE_RX_FLTR_OPT + at);
    opt = (opt & ~B_BE_RX_MPDU_MAX_LEN_MASK) |
          ((units << 16) & B_BE_RX_MPDU_MAX_LEN_MASK);
    wr32(regs, R_BE_RX_FLTR_OPT + at, opt);

    /* Two smaller things: a checksum on part of the header that this chip
     * gets wrong often enough that Realtek switch it off, and telling the
     * receiver to notice when it is busy. */
    clr32(regs, R_BE_PLCP_HDR_FLTR + at, B_BE_VHT_SU_SIGB_CRC_CHK);
    set32(regs, R_BE_RCR + at, B_BE_BUSY_CHKSN);

    kinfo("rtw89", "radio %d receives frames up to %u bytes and gives up on "
                   "one that stalls", mac, units * RX_MAX_LEN_UNIT);
    return true;
}

/* Bring one radio's signal processor out of reset.
 *
 * Four steps in an order that cannot be rearranged:
 *
 *   the whole block held down,
 *   the platform under it held down too,
 *   the block released,
 *   and only then the processor told it may boot.
 *
 * Releasing the processor first means it begins executing against registers
 * that are still held at zero, and nothing defines what it does then.  It does
 * not crash - it runs, badly, and the radio afterwards is subtly wrong in ways
 * that look like interference.
 */
bool rtw89_baseband_reset(volatile u8 *regs, int phy) {
    if (!regs || phy < 0 || phy >= RTW89_MACS) return false;

    u32 platform = phy ? B_BE_FEN_BB1PLAT_RSTB : B_BE_FEN_BBPLAT_RSTB;
    u32 block    = phy ? B_BE_FEN_BB1_IP_RSTN  : B_BE_FEN_BB_IP_RSTN;
    u32 boot     = phy ? B_BE_BOOT_RDY1        : B_BE_BOOT_RDY0;
    u32 sys_mask = phy ? B_BE_DMAC_BB_PHY1_MASK : B_BE_DMAC_BB_PHY0_MASK;
    int shift    = phy ? 16 : 0;

    /* What the packet engine is allowed to hand this baseband. */
    u32 sys = rd32(regs, R_BE_DMAC_SYS_CR32B);
    sys = (sys & ~sys_mask) |
          ((RTW89_DMAC_BB_SETTING << shift) & sys_mask);
    wr32(regs, R_BE_DMAC_SYS_CR32B, sys);

    /* Down, both of them. */
    clr32(regs, R_BE_FEN_RST_ENABLE, block);
    clr32(regs, R_BE_FEN_RST_ENABLE, platform);

    /* The block up, and the processor allowed to boot - only the second radio
     * is told at this point; the first is told once the rest is ready. */
    set32(regs, R_BE_FEN_RST_ENABLE, block);
    if (phy) set32(regs, R_BE_FEN_RST_ENABLE, boot);
    else     clr32(regs, R_BE_FEN_RST_ENABLE, boot);

    /* And its memory out of the low-power state, or it boots into memory that
     * is not there. */
    clr32(regs, R_BE_MEM_PWR_CTRL, B_BE_MEM_BBMCU0_DS_V1);
    timer_udelay(1);

    kinfo("rtw89", "radio %d's signal processor is out of reset", phy);
    return true;
}

/* ==================================================== tuning the radio ====
 *
 * The first of the five things the system asks a wireless driver to do that
 * this card can now actually be told to do.
 *
 * One number - the channel - reaches the radio through four registers: two
 * paths, and two addresses per path that have to agree with each other.  Every
 * one is a read-modify-write, because the same register holds settings this
 * has no business changing.
 *
 * All four are read before any is written.  That ordering is deliberate: a
 * read that fails half way through leaves the radio tuned across two channels,
 * one path on each, and a card in that state does not report an error - it
 * just receives nothing while looking configured.
 */

/* Put a value where a mask says it goes.
 *
 * The reference keeps these fields as GENMASK and encodes with u32_encode_bits,
 * which shifts by the mask's own position.  Doing the same here means the mask
 * is the single place the field's position is written down - a separate shift
 * constant beside each mask is one more transcribed number that can disagree
 * with the mask it belongs to, and nothing would catch it.
 */
static u32 rf_encode(u32 value, u32 mask) {
    if (!mask) return 0;
    u32 shift = 0;
    while (!((mask >> shift) & 1u)) shift++;
    return (value << shift) & mask;
}

rtw89_band_t rtw89_band_of_channel(u8 channel) {
    return (channel <= 14) ? RTW89_BAND_2G : RTW89_BAND_5G;
}

/* What the channel and band come to in the register's own fields. */
static u32 chan_to_rf18(u8 channel, rtw89_band_t band) {
    u32 val = rf_encode(channel, RR_CFGCH_CH);

    switch (band) {
    case RTW89_BAND_5G:
        val |= rf_encode(CFGCH_BAND1_5G, RR_CFGCH_BAND1) |
               rf_encode(CFGCH_BAND0_5G, RR_CFGCH_BAND0);
        break;
    case RTW89_BAND_6G:
        val |= rf_encode(CFGCH_BAND1_6G, RR_CFGCH_BAND1) |
               rf_encode(CFGCH_BAND0_6G, RR_CFGCH_BAND0);
        break;
    case RTW89_BAND_2G:
    default:
        /* Both band fields are zero at 2.4 GHz, which is why they are cleared
         * before this is OR'd in rather than merely OR'd over. */
        break;
    }

    /* 20 MHz is zero in the width field, so there is nothing to set for it.
     * Written out rather than left implied: "no bits" and "not handled" look
     * identical in the register and are not the same thing. */
    val |= rf_encode(CFGCH_BW_V2_20M, RR_CFGCH_BW_V2);
    return val;
}

bool rtw89_set_channel(volatile u8 *regs, u8 channel, rtw89_band_t band,
                       bool first_revision) {
    if (!regs) return false;

    /* Channel zero is not a channel.  It is what an uninitialised field reads
     * as, and tuning to it would be obeying a mistake somewhere above. */
    if (!channel) {
        kwarn("rtw89", "asked to tune to channel 0, which is not a channel");
        return false;
    }

    static const u32 addr[2] = { RR_CFGCH, RR_CFGCH_V1 };
    u32 held[RTW89_RF_PATHS][2];

    /* Read everything first. */
    for (int path = 0; path < RTW89_RF_PATHS; path++) {
        for (int i = 0; i < 2; i++) {
            held[path][i] = rtw89_rf_read(regs, path, addr[i]);
            if (held[path][i] == RTW89_RF_INVALID) {
                kwarn("rtw89", "the radio would not say how path %d is tuned, "
                               "so it has been left alone", path);
                return false;
            }
        }
    }

    u32 want = chan_to_rf18(channel, band);

    for (int path = 0; path < RTW89_RF_PATHS; path++) {
        for (int i = 0; i < 2; i++) {
            u32 v = held[path][i] & ~(RR_CFGCH_BAND1 | RR_CFGCH_BW_V2 |
                                      RR_CFGCH_BAND0 | RR_CFGCH_CH);
            v |= want;

            if (!rtw89_rf_write(regs, path, addr[i], v)) {
                kwarn("rtw89", "the radio would not take a channel on path %d",
                      path);
                return false;
            }

            /* The synthesiser has to settle before it is asked again.  This
             * wait is in the reference and is not decoration - the second
             * write of a pair goes out while the first is still taking
             * effect. */
            timer_udelay(100);
        }
    }

    /* The first revision of this chip needs a table entry rewritten after a
     * retune, and which entry depends on the band.  Later revisions do it
     * themselves; writing it to them anyway is not harmless, so it is asked
     * for rather than assumed. */
    if (first_revision) {
        u32 lutwd1 = (band == RTW89_BAND_2G) ? 0x0c990 : 0x0c190;
        rtw89_rf_write(regs, 0, RR_LUTWE,  0x80000);
        rtw89_rf_write(regs, 0, RR_LUTWA,  0x00003);
        rtw89_rf_write(regs, 0, RR_LUTWD1, lutwd1);
        rtw89_rf_write(regs, 0, RR_LUTWD0, 0xebe38);
        rtw89_rf_write(regs, 0, RR_LUTWE,  0x00000);
    }

    return true;
}

/* ======================================= bringing the baseband up =========
 *
 * Partial BB MCU/post-init sequence only. Linux's NULL chip bb/rf/nctl tables
 * select separate appended firmware resources through fw.elm_info; those
 * thousands of host-applied rows are NOT executed by downloading the MCU
 * image. This helper cannot replace the BB/RF/gain/NCTL/power-table pipeline.
 */

/* The baseband's window and the processor's window, each at its own base. */
static void phy_wr32(volatile u8 *regs, u32 addr, u32 mask, u32 value) {
    u32 at = RTW89_PHY_CR_BASE + addr;
    u32 v = (rd32(regs, at) & ~mask) | rf_encode(value, mask);
    wr32(regs, at, v);
}
static void phy_set32(volatile u8 *regs, u32 addr, u32 bits) {
    u32 at = RTW89_PHY_CR_BASE + addr;
    wr32(regs, at, rd32(regs, at) | bits);
}
static void phy_clr32(volatile u8 *regs, u32 addr, u32 bits) {
    u32 at = RTW89_PHY_CR_BASE + addr;
    wr32(regs, at, rd32(regs, at) & ~bits);
}

/* What the baseband's processor is handed before it boots.
 *
 * The order is the table's order and is not tidy: 0x6800 and 0x6820 each
 * appear twice, written once to a value that holds something down and again to
 * the value that releases it.  Sorting this table, or removing what looks like
 * a duplicate, breaks it.
 */
typedef struct { u32 addr, data; } bbmcu_reg_t;

static const bbmcu_reg_t bbmcu_init[] = {
    { 0x6990, 0x00000000 },
    { 0x6994, 0x00000000 },
    { 0x6998, 0x00000000 },
    { 0x6820, 0xFFFFFFFE },
    { 0x6800, 0xC0000FFE },
    { 0x6808, 0x76543210 },
    { 0x6814, 0xBFBFB000 },
    { 0x6818, 0x0478C009 },
    { 0x6800, 0xC0000FFF },
    { 0x6820, 0xFFFFFFFF },
};

bool rtw89_baseband_configure(volatile u8 *regs, int phy) {
    if (!regs || phy < 0 || phy >= RTW89_MACS) return false;

    /* The processor's own registers.  The second radio's are the same
     * addresses 0x20000 higher, which is only true below 0x10000 - every
     * address in the table above is, and a check is cheaper than trusting
     * that the table never grows. */
    for (unsigned i = 0; i < sizeof bbmcu_init / sizeof bbmcu_init[0]; i++) {
        u32 addr = bbmcu_init[i].addr;
        if (phy && addr < 0x10000) addr += 0x20000;
        wr32(regs, RTW89_BBMCU_BASE + addr, bbmcu_init[i].data);
    }

    /* Now it may boot, and the platform under it comes up. */
    u32 platform = phy ? B_BE_FEN_BB1PLAT_RSTB : B_BE_FEN_BBPLAT_RSTB;
    if (!phy) set32(regs, R_BE_FEN_RST_ENABLE, B_BE_BOOT_RDY0);
    set32(regs, R_BE_FEN_RST_ENABLE, platform);

    /* The baseband's clock, and the transmit scaler left off - it is turned on
     * later with a value, and on with the previous value is worse than off. */
    phy_set32(regs, R_BBCLK, B_CLK_640M);
    phy_clr32(regs, R_TXSCALE, B_TXFCTR_EN);
    phy_wr32(regs, R_TXFCTR, B_TXFCTR_THD, 0x200);

    /* Which rates the receiver will look for.  These are thresholds, not
     * switches: a receiver with them left at zero hears the air and decodes
     * none of it. */
    phy_wr32(regs, R_SLOPE,  B_EHT_RATE_TH, 0xA);
    phy_wr32(regs, R_BEDGE,  B_HE_RATE_TH,  0xA);
    phy_wr32(regs, R_BEDGE2, B_HT_VHT_TH,   0xAAA);
    phy_wr32(regs, R_BEDGE,  B_EHT_MCS14,   0x1);
    phy_wr32(regs, R_BEDGE2, B_EHT_MCS15,   0x1);

    /* The multi-user and trigger-based modes off.  This card is a station on
     * somebody's home network, and those are what an access point does. */
    phy_wr32(regs, R_BEDGE3, B_EHTTB_EN,  0x0);
    phy_wr32(regs, R_BEDGE3, B_HEERSU_EN, 0x0);
    phy_wr32(regs, R_BEDGE3, B_HEMU_EN,   0x0);
    phy_wr32(regs, R_BEDGE3, B_TB_EN,     0x0);

    phy_wr32(regs, R_SU_PUNC, B_SU_PUNC_EN,   0x1);
    phy_wr32(regs, R_BEDGE5,  B_HWGEN_EN,     0x1);
    phy_wr32(regs, R_BEDGE5,  B_PWROFST_COMP, 0x1);

    /* The curve the receiver uses to turn a measured level into a reported
     * one.  Both halves of 0x6bf8 are written, in this order. */
    phy_wr32(regs, R_MAG_AB, B_BY_SLOPE, 0x1);
    phy_wr32(regs, R_MAG_A,  B_MGA_AEND, 0xE0);
    phy_wr32(regs, R_MAG_AB, B_MAG_AB,   0xE0C000);
    phy_wr32(regs, R_SLOPE,  B_SLOPE_A,  0x3FE0);
    phy_wr32(regs, R_SLOPE,  B_SLOPE_B,  0x3FE0);
    phy_wr32(regs, R_SC_CORNER, B_SC_CORNER, 0x200);

    /* A pulse, not a setting: cleared and set again. */
    phy_wr32(regs, R_UDP_COEEF, B_UDP_COEEF, 0x0);
    phy_wr32(regs, R_UDP_COEEF, B_UDP_COEEF, 0x1);

    kinfo("rtw89", "radio %d signal processor configured and booting", phy);
    return true;
}

/* ============================================== frames, in and out ========
 *
 * The descriptor in front of a frame, both directions.
 *
 * These are pure: they take a description and produce bytes, or take bytes and
 * produce a description.  Nothing here touches the card.  That is deliberate -
 * a descriptor built wrongly is invisible on hardware (the card sends
 * something, or drops it, and says nothing), so the part that can be checked
 * without hardware is separated from the part that cannot.
 */

static u32 get32(const u8 *at) {
    return (u32)at[0] | ((u32)at[1] << 8) |
           ((u32)at[2] << 16) | ((u32)at[3] << 24);
}

u32 rtw89_txd_fill(u8 *out, u32 capacity, const rtw89_txd_t *how) {
    if (!out || !how || capacity < RTW89_TXD_BYTES) return 0;

    /* The length field is fourteen bits.  A frame that does not fit is not
     * truncated - the card would send a fragment of a frame, which is worse
     * than sending nothing. */
    if (!how->bytes || how->bytes > BE_TXD_BODY2_TXPKTSIZE) return 0;

    memset(out, 0, RTW89_TXD_BYTES);

    /* Word 0: where the frame is relative to the descriptor, which queue's
     * ring carries it, and that the second half of the descriptor is
     * present. */
    put32(out + 0,
          rf_encode(RTW89_TXD_BYTES / 8, BE_TXD_BODY0_WP_OFFSET_V1) |
          rf_encode(1, BE_TXD_BODY0_WDINFO_EN) |
          rf_encode(how->channel, BE_TXD_BODY0_CH_DMA));

    /* Word 1: one address region follows.  Zero here means the card is given
     * a descriptor pointing at nothing, and it does not complain. */
    put32(out + 4, rf_encode(1, BE_TXD_BODY1_ADDR_INFO_NUM));

    /* Word 2: how long, which queue, and which station. */
    put32(out + 8,
          rf_encode(how->bytes, BE_TXD_BODY2_TXPKTSIZE) |
          rf_encode(how->qsel,  BE_TXD_BODY2_QSEL) |
          rf_encode(how->mac_id, BE_TXD_BODY2_MACID));

    /* Word 3: the sequence number the card should put in the frame. */
    put32(out + 12, rf_encode(how->sequence, BE_TXD_BODY3_WIFI_SEQ));

    /* Words 4 and 5 carry the encryption details and are left at zero, which
     * is what "not encrypted" means here.  They are not skipped by accident -
     * the reference writes them only when security is on, and zero is the same
     * thing. */

    /* Word 6: let the card fill in the parts of the header it owns. */
    put32(out + 24, rf_encode(1, BE_TXD_BODY6_UPD_WLAN_HDR));

    /* Word 7: the rate.  Only meaningful when the card is told to use it -
     * otherwise the card chooses, which is what a station wants once it is
     * associated and knows what works. */
    u32 w7 = 0;
    if (how->fixed_rate)
        w7 = rf_encode(1, BE_TXD_BODY7_USERATE_SEL) |
             rf_encode(how->rate, BE_TXD_BODY7_DATARATE);
    put32(out + 28, w7);

    /* The second half. */
    u8 *info = out + RTW89_TXWD_BODY_BYTES;

    u32 i0 = 0;
    if (how->retries)
        i0 = rf_encode(1, BE_TXD_INFO0_DATA_TXCNT_LMT_SEL) |
             rf_encode(how->retries, BE_TXD_INFO0_DATA_TXCNT_LMT);
    put32(info + 0, i0);

    put32(info + 4, rf_encode(how->sequence, BE_TXD_INFO1_SW_DEFINE));
    put32(info + 8, 0);

    /* Ask for RTS on anything with a single recipient, and let the card decide
     * whether it is actually needed.  A broadcast has nobody to answer an RTS,
     * so asking for one there wastes the air on every beacon. */
    put32(info + 16,
          rf_encode(how->broadcast ? 0 : 1, BE_TXD_INFO4_RTS_EN) |
          rf_encode(1, BE_TXD_INFO4_HW_RTS_EN));

    return RTW89_TXD_BYTES;
}

bool rtw89_rxd_parse(const u8 *buffer, u32 arrived, rtw89_rxd_t *out) {
    if (!buffer || !out) return false;

    /* The short descriptor has to be there before anything it says can be
     * read - including the bit that says whether it is the long one. */
    if (arrived < RTW89_RXD_SHORT_BYTES) return false;

    u32 w0 = get32(buffer + 0);
    u32 w2 = get32(buffer + 8);
    u32 w3 = get32(buffer + 12);

    u32 desc_len = (w0 & BE_RXD_LONG_RXD) ? RTW89_RXD_LONG_BYTES
                                          : RTW89_RXD_SHORT_BYTES;
    if (arrived < desc_len) return false;

    /* Between the descriptor and the frame: a radio report, a driver area, a
     * converted header, and a shift.  Each is counted in its OWN unit, and
     * using one unit for all four puts the frame's start in the wrong place
     * without anything looking wrong. */
    u32 shift    = ((w0 & BE_RXD_SHIFT_MASK)       >> 14) << 1;   /* 2-byte  */
    u32 drv_info = ((w0 & BE_RXD_DRV_INFO_SZ_MASK) >> 18) << 3;   /* 8-byte  */
    u32 hdr_cnv  = ((w0 & BE_RXD_HDR_CNV_SZ_MASK)  >> 20) << 4;   /* 16-byte */
    u32 phy_rpt  = ((w0 & BE_RXD_PHY_RPT_SZ_MASK)  >> 22) << 3;   /* 8-byte  */

    out->frame_at    = desc_len + phy_rpt + drv_info + hdr_cnv + shift;
    out->frame_bytes = w0 & BE_RXD_RPKT_LEN_MASK;
    out->type        = (u8)((w0 & BE_RXD_RPKT_TYPE_MASK) >> 24);

    /* Everything above is a number the card chose, so the frame it describes
     * has to actually be inside what arrived.  Without this the next step
     * reads past the end of the buffer on a card that is merely confused. */
    if (!out->frame_bytes) return false;
    if (out->frame_at > arrived) return false;
    if (out->frame_bytes > arrived - out->frame_at) return false;

    /* The radio report, if there is one, sits directly after the descriptor
     * and before everything else. */
    out->signal_known = false;
    out->signal_dbm = 0;
    if (phy_rpt >= RTW89_PHY_RPT_BYTES &&
        desc_len + RTW89_PHY_RPT_BYTES <= arrived) {
        u32 raw = get32(buffer + desc_len) & BE_RXD_PHY_RSSI;
        out->signal_dbm = (s8)((s32)(raw >> RSSI_FACTOR) - MAX_RSSI);
        out->signal_known = true;
    }

    out->mac_id          = (u8)(w2 & BE_RXD_MAC_ID_MASK);
    out->crc_error       = (w3 & BE_RXD_CRC32_ERR) != 0;
    out->icv_error       = (w3 & BE_RXD_ICV_ERR) != 0;
    out->decrypted       = (w3 & BE_RXD_HW_DEC) != 0;
    out->addressed_to_us = (w3 & BE_RXD_A1_MATCH) != 0;
    return true;
}

/* ============================================== frames on their own ring ==
 *
 * The join for the sending side: a descriptor built above, put in front of a
 * frame, described by a ring entry, and announced with a doorbell.
 */

bool rtw89_data_attach(volatile u8 *regs, rtw89_data_t *d,
                       u8 *ring, u64 ring_phys, int slots,
                       u8 *buffer, u64 buffer_phys, u32 buffer_size) {
    if (!regs || !d || !ring || !buffer || slots <= 0) return false;

    /* The buffer holds the descriptor AND the frame.  A buffer that cannot
     * hold both is refused here rather than producing a descriptor pointing
     * past its own region. */
    if (buffer_size <= RTW89_TXD_BYTES) return false;

    memset(d, 0, sizeof *d);
    d->buffer = buffer;
    d->buffer_phys = buffer_phys;
    d->buffer_size = buffer_size;
    d->tx.bd = ring;
    d->tx.bd_phys = ring_phys;
    d->tx.slots = (u16)slots;
    memset(ring, 0, (size_t)slots * RTW89_PCI_BD_BYTES);

    return rtw89_ring_attach(regs, &d->tx, R_BE_CH8_TXBD_DESA_L,
                             R_BE_CH8_TXBD_DESA_H, R_BE_CH8_TXBD_NUM,
                             R_BE_CH8_TXBD_IDX);
}

int rtw89_data_transmit(volatile u8 *regs, rtw89_data_t *d,
                        const void *frame, int len, bool broadcast) {
    if (!regs || !d || !d->buffer || !frame || len <= 0) return -1;
    if ((u32)len + RTW89_TXD_BYTES > d->buffer_size) {
        kwarn("rtw89", "a frame of %d bytes does not fit the sending buffer",
              len);
        return -1;
    }

    rtw89_txd_t how;
    memset(&how, 0, sizeof how);
    how.bytes     = (u16)len;
    how.qsel      = RTW89_TX_QSEL_B0_MGMT;
    how.channel   = RTW89_TXCH_CH8;
    how.sequence  = d->sequence;
    how.broadcast = broadcast;

    /* The descriptor first, then the frame directly behind it - which is
     * where the descriptor's own offset field says it will be.  The two are
     * written in that order for no reason the card can see; they are one
     * region and it fetches the whole thing. */
    if (!rtw89_txd_fill(d->buffer, d->buffer_size, &how)) return -1;
    memcpy(d->buffer + RTW89_TXD_BYTES, frame, (size_t)len);

    if (!rtw89_ring_add(&d->tx, d->buffer_phys,
                        (u32)len + RTW89_TXD_BYTES, true)) {
        kwarn("rtw89", "the sending ring is full");
        return -1;
    }
    rtw89_ring_doorbell(regs, &d->tx, R_BE_CH8_TXBD_IDX);

    d->sequence = (u16)((d->sequence + 1) & BE_TXD_BODY3_WIFI_SEQ);
    d->sent++;
    return len;
}

/* ========================================= frames coming the other way ====
 *
 * The join for the receiving side.
 */

/* Point one slot of the ring at one buffer. */
static bool rx_post(rtw89_rx_path_t *r, u16 slot) {
    return rtw89_rx_bd_write(r->rx.bd + (size_t)slot * RTW89_PCI_RXBD_BYTES,
                             r->buffer_phys[slot], r->buffer_size);
}

bool rtw89_rx_attach(volatile u8 *regs, rtw89_rx_path_t *r,
                     u8 *ring, u64 ring_phys,
                     u8 *pool, u64 pool_phys, int count, u32 each) {
    if (!regs || !r || !ring || !pool) return false;
    if (count <= 0 || count > RTW89_RX_BUFFERS) return false;

    /* Every buffer has to be able to hold the card's own word, a descriptor,
     * and a frame.  Anything smaller is a buffer the card will overrun. */
    if (each <= 4 + RTW89_RXD_LONG_BYTES) return false;

    memset(r, 0, sizeof *r);
    r->count = count;
    r->buffer_size = each;
    r->rx.bd = ring;
    r->rx.bd_phys = ring_phys;
    r->rx.slots = (u16)count;
    memset(ring, 0, (size_t)count * RTW89_PCI_RXBD_BYTES);

    for (int i = 0; i < count; i++) {
        r->buffer[i] = pool + (size_t)i * each;
        r->buffer_phys[i] = pool_phys + (u64)i * each;
        memset(r->buffer[i], 0, each);
    }

    if (!rtw89_ring_attach(regs, &r->rx, R_BE_RXQ0_RXBD_DESA_L,
                           R_BE_RXQ0_RXBD_DESA_H, R_BE_RXQ0_RXBD_NUM,
                           R_BE_RXQ0_RXBD_IDX_V1))
        return false;

    /* Every buffer posted before anything is expected.  Posting them one at a
     * time as frames arrive is the arrangement where the first burst is lost. */
    for (int i = 0; i < count; i++)
        if (!rx_post(r, (u16)i)) return false;

    /* And the write index one BELOW the count, not equal to it.
     *
     * This part reads an equal pair of indexes as a ring that is completely
     * full rather than completely empty - the two are indistinguishable from
     * the numbers alone and each chip picks one.  This one picks full, which
     * is why the ring starts at count-1 with every buffer available and why
     * the next buffer to be read is always one PAST the write index.
     *
     * Starting at count instead leaves the ring looking empty: the card is
     * handed sixteen buffers and told there are none, and every frame is
     * dropped without a word. */
    r->rx.host_index = (u16)(count - 1);
    rtw89_ring_doorbell(regs, &r->rx, R_BE_RXQ0_RXBD_IDX_V1);
    return true;
}

int rtw89_rx_poll(volatile u8 *regs, rtw89_rx_path_t *r,
                  void (*deliver)(void *ctx, const u8 *frame, u32 len,
                                  s8 signal_dbm),
                  void *ctx) {
    if (!regs || !r || !r->count) return 0;

    int handed_up = 0;
    u16 card = rtw89_ring_card_index(regs, R_BE_RXQ0_RXBD_IDX_V1);
    u16 len = (u16)r->count;

    /* How many the card has filled since the last look.  The read starts one
     * past the write index, for the reason given in attach. */
    u16 first = (u16)((r->rx.host_index + 1) % len);
    u16 waiting = (u16)(card >= first ? card - first : len - (first - card));

    /* Bounded by the number of buffers.  Without that, a card reporting a
     * nonsense index spins here forever. */
    if (waiting > len) waiting = len;

    for (u16 done = 0; done < waiting; done++) {
        u16 slot = (u16)((r->rx.host_index + 1) % len);
        const u8 *buffer = r->buffer[slot];

        /* The card's own word first: how much it wrote, and its count. */
        rtw89_rx_info_t info;
        if (rtw89_rx_parse(buffer, r->buffer_size, &info)) {
            int missed = 0;
            if (!rtw89_rx_tag_ok(&r->expected_tag, info.tag, &missed) && missed)
                kwarn("rtw89", "%d frame(s) went missing before this one",
                      missed);

            rtw89_rxd_t d;
            if (!rtw89_rxd_parse(buffer + 4, info.bytes, &d)) {
                r->unreadable++;
            } else if (d.type != RTW89_RX_TYPE_WIFI) {
                /* The card reporting on itself - a transmit report, a radio
                 * measurement, an answer to a command.  Handing one of these
                 * up as a frame is how a receive path corrupts the stack
                 * above it. */
                r->not_frames++;
            } else if (d.crc_error || d.icv_error) {
                /* Kept apart from a frame that could not be read: this one
                 * arrived and was damaged in the air, which is normal and not
                 * a fault in anything here. */
                r->crc_errors++;
            } else {
                r->frames++;
                handed_up++;
                if (deliver)
                    deliver(ctx, buffer + 4 + d.frame_at, d.frame_bytes,
                            d.signal_known ? d.signal_dbm : (s8)-128);
            }
        } else {
            r->unreadable++;
        }

        /* And the buffer goes straight back, before moving on.  A buffer read
         * and not returned is one the card can never use again, and sixteen
         * frames later there is nowhere to put anything.
         *
         * The write index wraps at the ring's length, not at the register
         * field's width - the card uses it to index the ring. */
        rx_post(r, slot);
        r->rx.host_index = slot;
        rtw89_ring_doorbell(regs, &r->rx, R_BE_RXQ0_RXBD_IDX_V1);
    }

    return handed_up;
}

/* ============================================= calibrating the radio ======
 *
 * The card measures its own converters and its receiver's standing level, and
 * reports what it found.  The host asks and waits.
 *
 * That this is only a few exchanges is a property of the part, not a shortcut:
 * its chip description carries no calibration tables at all, because the
 * processor on the card owns them.  On an older part the same job is thousands
 * of register writes with no way to check any of them.
 */

const char *rtw89_rfk_state_name(u8 state) {
    switch (state) {
    case RTW89_RFK_STATE_START:       return "started, not finished";
    case RTW89_RFK_STATE_OK:          return "done";
    case RTW89_RFK_STATE_FAIL:        return "failed";
    case RTW89_RFK_STATE_TIMEOUT:     return "timed out inside the card";
    case RTW89_RFK_STATE_H2C_CMD_ERR: return "the request was malformed";
    default:                          return "an answer nobody has seen before";
    }
}

int rtw89_rfk_ask(volatile u8 *regs, rtw89_channel_t *ch, u8 func,
                  const void *payload, u16 len, int timeout_ms) {
    /* Asked on the radio's class, answered on the report's.  Waiting for the
     * class it was asked on is a wait that never ends. */
    u8 answer[8];
    int n = rtw89_ask_as(regs, ch, H2C_CAT_OUTSRC, H2C_CL_OUTSRC_RF_FW_RFK,
                         func, RTW89_PHY_C2H_RFK_REPORT,
                         RTW89_PHY_C2H_RFK_REPORT_FUNC_STATE,
                         payload, len, answer, sizeof answer, timeout_ms);
    if (n < 1) return -1;
    return answer[0];
}

bool rtw89_rfk_calibrate(volatile u8 *regs, rtw89_channel_t *ch, u8 phy,
                         u8 band, u8 bandwidth, u8 channel) {
    if (!regs || !ch) return false;

    /* The converters first.  Its payload is three bytes and the third is a
     * type the card defines; zero is the one this sequence uses. */
    u8 dack[3] = { (u8)sizeof dack, phy, 0 };

    int state = rtw89_rfk_ask(regs, ch, H2C_FUNC_RFK_DACK_OFFLOAD,
                              dack, sizeof dack, 3000);
    if (state != RTW89_RFK_STATE_OK) {
        kwarn("rtw89", "radio %u: the converter calibration came back \"%s\"",
              phy, state < 0 ? "no answer at all"
                             : rtw89_rfk_state_name((u8)state));
        return false;
    }

    /* Then the receiver's standing level, which depends on where the radio is
     * tuned - so the channel goes with the request rather than being implied.
     * Running this before tuning measures the wrong band. */
    u8 rxdck[9] = {
        (u8)sizeof rxdck,
        phy,
        0,                        /* not the analogue front end          */
        RTW89_RF_PATH_AB,         /* both chains                         */
        band,
        bandwidth,
        channel,
        0,                        /* no running commentary wanted        */
        0,                        /* not a per-channel repeat            */
    };

    state = rtw89_rfk_ask(regs, ch, H2C_FUNC_RFK_RXDCK_OFFLOAD,
                          rxdck, sizeof rxdck, 3000);
    if (state != RTW89_RFK_STATE_OK) {
        kwarn("rtw89", "radio %u: the receiver calibration came back \"%s\"",
              phy, state < 0 ? "no answer at all"
                             : rtw89_rfk_state_name((u8)state));
        return false;
    }

    kinfo("rtw89", "radio %u is calibrated on channel %u: its converters and "
                   "its receiver's standing level were both measured by the "
                   "card and reported done", phy, channel);
    return true;
}
