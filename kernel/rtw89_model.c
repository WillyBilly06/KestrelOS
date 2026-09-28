/* rtw89_model.c - a Wi-Fi 7 Realtek part, in software, for the start-up
 * sequence to be driven against.
 *
 * There is no RTL8922AE on the machine this is built on, and the sequence in
 * rtw89.c cannot be checked by reading it: every mistake it can make - a wait
 * in the wrong direction, two steps swapped, a bit dropped from a mask - looks
 * exactly the same from outside, which is a card that never reports ready.
 *
 * So the part is modelled.  Not all of it: only the handful of registers the
 * power-on sequence touches, and only the behaviour that sequence depends on.
 * What that is worth is precise - it proves the driver writes the right things
 * in the right order and waits for the right answers.  It cannot prove the
 * silicon agrees, because the model was written from the same source the
 * driver was, and nothing here has met the hardware.
 *
 * The model is deliberately strict.  It refuses to advance when a step is
 * missing or out of order rather than being generous about it - a forgiving
 * model passes a driver that a real card would hang on, which is worse than
 * no model at all.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "rtw89.h"

static u8  *space;
static bool attached;

/* What the part has been told, in order. */
static bool system_power_ready;
static bool mac_on;
static bool bus_io_ready;
static bool bus_wlan_ready;
static int  xtal_writes;

/* The processor on the card, once the power sequence has finished with it. */
static bool cpu_running;
static bool download_path_open;
static bool debug_dirty;          /* left non-empty on purpose, once */

/* The bus-side engine, which has to be stopped and drained before its
 * channel pointers may be touched. */
static bool dma_stopped;
static bool dma_drained;
static bool dma_started;
static bool dma_cleared_while_busy;
static u16  rx_ring_slots;

/* The last thing written through the crystal's side band, so a test can see
 * that the encoding came out the way the hardware would read it. */
static u8 last_xtal_addr, last_xtal_value, last_xtal_mask;

static void radio_sync(void);
static void serve_request(void);
static void serve_frames(void);
static u16 model_frame_taken;
static u16 model_tx_taken, model_rx_taken;
static u16 model_rx_free;
static void rx_accounting(void);
static void rx_took_one(u16 len);
static u16 model_rx_tag = 1;
static int sched_settling;
static bool mlo_pulsed;
static int mlo_settling;

static u32 rd(u32 off) {
    return off + 4 <= RTW89_REG_BYTES ? *(volatile u32 *)(space + off) : 0;
}
static void wr(u32 off, u32 v) {
    if (off + 4 <= RTW89_REG_BYTES) *(volatile u32 *)(space + off) = v;
}

/* Model the two independent packet-memory ready signals. Do not acknowledge
 * merely because functions are enabled: that hid the missing runtime quotas.
 * Numeric expectations are the Linux v6.17 RTL8922A PCIe layouts. */
static void packet_memory_sync(void) {
    const u32 enabled = (1u << 26) | (1u << 23);
    bool clocks = (rd(0x8404) & enabled) == enabled;
    bool running = (rd(0x8400) & enabled) == enabled;
    u32 wde_cfg = rd(0x8c08), ple_cfg = rd(0x9008);
    unsigned pages = (wde_cfg >> 16) & 0x1fff;
    bool runtime = pages == 3328;
    bool geometry = !(wde_cfg & 0x7f03) &&
        (ple_cfg & 0x7f03) == (26u << 8 | 1u) &&
        ((ple_cfg >> 16) & 0x1fff) == (runtime ? 2688u : 2928u) &&
        (runtime || pages == 0);
    static const u16 wde[5] = {3302, 6, 0, 0, 20};
    static const u16 pmin[13] = {320,320,32,16,13,13,292,292,64,18,1,4,0};
    static const u16 pmax[13] = {320,320,32,16,1316,1316,1595,1595,1367,1321,1,1307,0};
    bool wq = true, pq = true;
    for (unsigned i = 0; i < 5; ++i) {
        u32 q = runtime ? wde[i] : (i == 1 ? 6u : 0u);
        if (rd(0x8c40 + 4 * i) != (q | q << 16)) wq = false;
    }
    for (unsigned i = 0; i < 13; ++i) {
        u32 q = i == 2 ? 32u : i == 3 ? 256u : i == 10 ? 1u : 0u;
        u32 expected = runtime ? (u32)pmin[i] | (u32)pmax[i] << 16 : q | q << 16;
        if (rd(0x9040 + 4 * i) != expected) pq = false;
    }
    wr(0x8d00, clocks && running && geometry && wq ? 3u : 0u);
    wr(0x9100, clocks && running && geometry && pq ? 3u : 0u);
}

/* Called from the driver's read path, which is the only moment the hardware
 * would have to respond in. */
void rtw89_model_sync(void) {
    if (!attached) return;
    packet_memory_sync();

    u32 pw = rd(R_BE_SYS_PW_CTRL);

    /* The part reports its power good once it has been told to stop
     * suspending itself.  All four conditions, because a driver that clears
     * three of them and polls is a driver that would hang on real silicon. */
    if (!system_power_ready &&
        !(pw & B_BE_AFSM_WLSUS_EN) && !(pw & B_BE_AFSM_PCIE_SUS_EN) &&
        !(pw & B_BE_APDM_HPDN) && !(pw & B_BE_APFM_SWLPS) &&
        (pw & B_BE_DIS_WLBT_PDNSUSEN_SOPC)) {
        system_power_ready = true;
        wr(R_BE_SYS_PW_CTRL, pw | B_BE_RDY_SYSPWR);
    }

    /* Asking for the MAC is acknowledged by the request bit being cleared -
     * and only after the power is good and the wireless side is on.  A driver
     * that asks too early gets no answer, which is the point. */
    pw = rd(R_BE_SYS_PW_CTRL);
    if (system_power_ready && !mac_on &&
        (pw & B_BE_EN_WLON) && (pw & B_BE_APFN_ONMAC)) {
        mac_on = true;
        wr(R_BE_SYS_PW_CTRL, pw & ~B_BE_APFN_ONMAC);
    }

    /* The bus interface answers in two stages, and only once the MAC is up. */
    u32 hci = rd(R_BE_HCI_OPT_CTRL);
    if (mac_on && !bus_io_ready && (hci & B_BE_HAXIDMA_IO_EN)) {
        bus_io_ready = true;
        /* Its registers are reachable, and the restore it does on the way up
         * has finished - that second one is reported by a bit going away. */
        hci |= B_BE_HAXIDMA_IO_ST;
        hci &= ~B_BE_HAXIDMA_BACKUP_RESTORE_ST;
        wr(R_BE_HCI_OPT_CTRL, hci);
    }

    hci = rd(R_BE_HCI_OPT_CTRL);
    if (bus_io_ready && !bus_wlan_ready && (hci & B_BE_HCI_WLAN_IO_EN)) {
        bus_wlan_ready = true;
        wr(R_BE_HCI_OPT_CTRL, hci | B_BE_HCI_WLAN_IO_ST);
    }

    /* The processor.
     *
     * It only starts when it has been taken out of reset in the right order -
     * disabled, released from the hold, then enabled.  A model that starts it
     * on the enable alone would pass a driver that skips the release, which is
     * a processor that starts and immediately stops on real silicon. */
    u32 plat = rd(R_BE_PLATFORM_ENABLE);
    if (!(plat & B_BE_WCPU_EN) || (plat & B_BE_HOLD_AFTER_RESET)) {
        cpu_running = false;
        download_path_open = false;
    }
    if (!cpu_running && bus_wlan_ready &&
        (plat & B_BE_WCPU_EN) && !(plat & B_BE_HOLD_AFTER_RESET) &&
        (rd(R_BE_SYS_CLK_CTRL) & B_BE_CPU_CLK_EN)) {
        cpu_running = true;
    }

    /* The ROM first opens its H2C path for the image header. The raw DLFW
     * path opens only after that header is consumed. This model does not
     * simulate the physical CH12 transfer, so it stops at the first gate. */
    if (cpu_running && !download_path_open) {
        u32 ctrl = rd(R_BE_WCPU_FW_CTRL);
        if (ctrl & B_BE_WLANCPU_FWDL_EN) {
            download_path_open = true;
            ctrl |= B_BE_H2C_PATH_RDY;
            ctrl &= ~((u32)B_BE_WCPU_FWDL_STATUS_MASK <<
                      B_BE_WCPU_FWDL_STATUS_SHIFT);
            /* 2 is the card's own number for "ready to receive"; the driver
             * has to map it, and mapping it wrong is how a security failure
             * gets read as success. */
            ctrl |= (u32)2 << B_BE_WCPU_FWDL_STATUS_SHIFT;
            wr(R_BE_WCPU_FW_CTRL, ctrl);
        }
    }

    /* The bus-side DMA engine.
     *
     * It reports itself busy for a moment after being told to stop, exactly as
     * the silicon does: a channel part-way through a descriptor finishes it.
     * A driver that clears the channel pointers without waiting is caught
     * here rather than on a card. */
    {
        u32 cfg = rd(R_BE_HAXI_INIT_CFG1);
        u32 stop = rd(R_BE_HAXI_DMA_STOP1);
        /* PCIe BDRAM boundary reset is a self-clearing hardware pulse. */
        if (cfg & (1u << 16)) {
            cfg &= ~(1u << 16);
            wr(R_BE_HAXI_INIT_CFG1, cfg);
        }

        bool asked_to_stop = !(cfg & (B_BE_TXDMA_EN | B_BE_RXDMA_EN)) &&
                             (stop & B_BE_STOP_WPDMA);

        if (asked_to_stop && !dma_stopped) {
            dma_stopped = true;
            /* Still finishing: busy for one more look. */
            wr(R_BE_HAXI_DMA_BUSY1, BE_ALL_TX_CHANNELS);
        } else if (dma_stopped && !dma_drained) {
            dma_drained = true;
            wr(R_BE_HAXI_DMA_BUSY1, 0);
        }

        if (rd(R_BE_TXBD_RWPTR_CLR1) && !dma_drained)
            dma_cleared_while_busy = true;

        if (dma_drained && (cfg & B_BE_TXDMA_EN) && (cfg & B_BE_RXDMA_EN) &&
            !(cfg & B_BE_STOP_AXI_MST) && !(rd(R_BE_HAXI_DMA_STOP1) & B_BE_STOP_WPDMA)) {
            dma_started = true;
            rx_ring_slots = *(volatile u16 *)(space + R_BE_RXQ0_RXBD_IDX_V1);
        }
    }

    radio_sync();
    serve_request();
    serve_frames();

    /* The link table is built a moment after being asked for, and only if the
     * asking was a PULSE.  A driver that sets the reinit bit and leaves it set
     * is holding the table in reset, and this model holds it there too rather
     * than reporting a table that a real chip would not have built. */
    {
        u32 mlo = rd(R_BE_MLO_INIT_CTL);
        if (mlo & B_BE_MLO_TABLE_REINIT) {
            mlo_pulsed = true;
            mlo_settling = 0;
            wr(R_BE_MLO_INIT_CTL, mlo & ~(u32)B_BE_MLO_TABLE_INIT_DONE);
        } else if (mlo_pulsed && !(mlo & B_BE_MLO_TABLE_INIT_DONE)) {
            if (++mlo_settling >= 3)
                wr(R_BE_MLO_INIT_CTL, mlo | B_BE_MLO_TABLE_INIT_DONE);
        }
    }

    /* The scheduler reports that it has finished starting, a moment after
     * being told to.  Immediately would let a driver that never waits pass,
     * and that driver then writes the rest of the block while it is still
     * resetting. */
    {
        u32 ss = rd(R_BE_SS_CTRL);
        if ((ss & B_BE_SS_EN) && !(ss & B_BE_SS_INIT_DONE)) {
            if (++sched_settling >= 3)
                wr(R_BE_SS_CTRL, ss | B_BE_SS_INIT_DONE);
        }
    }

    /* The crystal's side band: take the byte and drop the poll bit. */
    u32 xt = rd(R_BE_WLAN_XTAL_SI_CTRL);
    if (xt & B_BE_WL_XTAL_SI_CMD_POLL) {
        last_xtal_addr  = (u8)(xt >> B_BE_WL_XTAL_SI_ADDR_SHIFT);
        last_xtal_value = (u8)(xt >> B_BE_WL_XTAL_SI_DATA_SHIFT);
        last_xtal_mask  = (u8)(xt >> B_BE_WL_XTAL_SI_BITMASK_SHIFT);
        xtal_writes++;
        wr(R_BE_WLAN_XTAL_SI_CTRL, xt & ~B_BE_WL_XTAL_SI_CMD_POLL);
    }
}

volatile u8 *rtw89_model_attach(void) {
    if (attached) return space;

    space = kzalloc(RTW89_REG_BYTES);
    if (!space) return NULL;

    /* How the part comes out of a cold start: suspending itself, MAC held in
     * reset, the bus interface not yet reachable.  Starting from zero would
     * let a driver that skips the first step appear to work. */
    wr(R_BE_SYS_PW_CTRL, B_BE_AFSM_WLSUS_EN | B_BE_AFSM_PCIE_SUS_EN |
                         B_BE_APDM_HPDN | B_BE_APFM_SWLPS);
    wr(R_BE_HCI_OPT_CTRL, B_BE_HAXIDMA_BACKUP_RESTORE_ST);

    /* Something left over from a card that fell over rather than stopped, so
     * the driver's check for it is exercised rather than merely present. */
    wr(R_BE_UDM1, 0xDEADBEEF);
    debug_dirty = true;

    system_power_ready = mac_on = bus_io_ready = bus_wlan_ready = false;
    cpu_running = download_path_open = false;
    dma_stopped = dma_drained = dma_started = false;
    dma_cleared_while_busy = false;
    rx_ring_slots = 0;
    xtal_writes = 0;
    attached = true;
    return space;
}

void rtw89_model_detach(void) {
    if (!attached) return;
    kfree(space);
    space = NULL;
    attached = false;
}

int rtw89_model_xtal_writes(void) { return xtal_writes; }

/* Started, and only counted as started if it was stopped and drained first -
 * and never if a channel's pointers were cleared while it was still busy. */
bool rtw89_model_dma_started(void) {
    return dma_started && !dma_cleared_while_busy;
}

u16 rtw89_model_rx_ring_slots(void) { return rx_ring_slots; }

bool rtw89_model_cpu_running(void) { return cpu_running; }

bool rtw89_model_debug_cleared(void) {
    if (!attached) return false;
    /* The driver is expected to have read the leftovers, said so, and then
     * cleared every one of them. */
    return debug_dirty && !rd(R_BE_UDM1) && !rd(R_BE_UDM2) &&
           !rd(R_BE_HALT_H2C) && !rd(R_BE_HALT_C2H) &&
           !rd(R_BE_HALT_H2C_CTRL) && !rd(R_BE_HALT_C2H_CTRL);
}

void rtw89_model_last_xtal(u8 *addr, u8 *value, u8 *mask) {
    if (addr)  *addr  = last_xtal_addr;
    if (value) *value = last_xtal_value;
    if (mask)  *mask  = last_xtal_mask;
}

/* Whether the part would now be out of reset with its blocks running.
 *
 * Every stage of the handshake, and the three enables at the end checked for
 * their exact contents - which is what catches a bit dropped from one of those
 * long masks.  A missing bit there disables one block of the MAC and nothing
 * says so; the card simply does not work in one particular way later on.
 */
bool rtw89_model_powered(void) {
    if (!attached) return false;
    if (!system_power_ready || !mac_on || !bus_io_ready || !bus_wlan_ready)
        return false;

    const u32 want_dmac =
        B_BE_MAC_FUNC_EN | B_BE_DMAC_FUNC_EN_BIT | B_BE_MPDU_PROC_EN |
        B_BE_WD_RLS_EN | B_BE_DLE_WDE_EN | B_BE_TXPKT_CTRL_EN |
        B_BE_STA_SCH_EN | B_BE_DLE_PLE_EN | B_BE_PKT_BUF_EN |
        B_BE_DMAC_TBL_EN | B_BE_PKT_IN_EN | B_BE_DLE_CPUIO_EN |
        B_BE_DISPATCHER_EN | B_BE_BBRPT_EN | B_BE_MAC_SEC_EN |
        B_BE_H_AXIDMA_EN | B_BE_DMAC_MLO_EN | B_BE_PLRLS_EN |
        B_BE_P_AXIDMA_EN | B_BE_DLE_DATACPUIO_EN | B_BE_LTR_CTL_EN;

    const u32 want_share =
        B_BE_CMAC_SHARE_EN | B_BE_RESPBA_EN | B_BE_ADDRSRCH_EN | B_BE_BTCOEX_EN;

    const u32 want_cmac =
        B_BE_CMAC_EN | B_BE_CMAC_TXEN | B_BE_CMAC_RXEN | B_BE_SIGB_EN |
        B_BE_PHYINTF_EN | B_BE_CMAC_DMA_EN | B_BE_PTCLTOP_EN |
        B_BE_SCHEDULER_EN | B_BE_TMAC_EN | B_BE_RMAC_EN | B_BE_TXTIME_EN |
        B_BE_RESP_PKTCTL_EN;

    if ((rd(R_BE_DMAC_FUNC_EN) & want_dmac) != want_dmac) {
        kwarn("rtw89", "model: the packet engine's enables came out %#x, "
                       "expected %#x", rd(R_BE_DMAC_FUNC_EN), want_dmac);
        return false;
    }
    if ((rd(R_BE_CMAC_SHARE_FUNC_EN) & want_share) != want_share) {
        kwarn("rtw89", "model: the shared radio block's enables came out %#x, "
                       "expected %#x", rd(R_BE_CMAC_SHARE_FUNC_EN), want_share);
        return false;
    }
    if ((rd(R_BE_CMAC_FUNC_EN) & want_cmac) != want_cmac) {
        kwarn("rtw89", "model: the radio block's enables came out %#x, "
                       "expected %#x", rd(R_BE_CMAC_FUNC_EN), want_cmac);
        return false;
    }

    /* The baseband is let out of reset last. */
    u32 fen = rd(R_BE_FEN_RST_ENABLE);
    if (!(fen & B_BE_FEN_BB_IP_RSTN) || !(fen & B_BE_FEN_BBPLAT_RSTB)) {
        kwarn("rtw89", "model: the baseband was left in reset (%#x)", fen);
        return false;
    }

    /* And the platform was enabled - one byte, easy to write to the wrong
     * width and then to be surprised that nothing happened. */
    if (!(*(volatile u8 *)(space + R_BE_PLATFORM_ENABLE) & B_BE_PLATFORM_EN)) {
        kwarn("rtw89", "model: the platform was never enabled");
        return false;
    }

    return true;
}

/* ------------------------------------------- what the card does with an image
 *
 * The firmware itself does not travel through these registers - it goes into
 * the card over the bus - so this model cannot watch it arrive and cannot
 * decide on its own whether to accept it.  What it can do is be told which
 * answer the card is giving, and then give it the way the silicon does: in the
 * status field of the control register, in the card's OWN numbering.
 *
 * That numbering is the point.  The driver has to map the card's numbers to
 * its own, they are not in the same order, and mapping them with arithmetic
 * works for the first few and then reports a security failure as "ready".
 * These take the card's number so a wrong mapping is caught here.
 */
static void set_card_status(u8 card_number) {
    u32 ctrl = rd(R_BE_WCPU_FW_CTRL);
    ctrl &= ~((u32)B_BE_WCPU_FWDL_STATUS_MASK << B_BE_WCPU_FWDL_STATUS_SHIFT);
    ctrl |= (u32)(card_number & B_BE_WCPU_FWDL_STATUS_MASK)
            << B_BE_WCPU_FWDL_STATUS_SHIFT;
    wr(R_BE_WCPU_FW_CTRL, ctrl);
}

/* The image was checked and is running.  The card clears the bit the driver
 * set to open the download, which is the same shape of handshake as the MAC
 * coming out of reset. */
void rtw89_model_firmware_started(void) {
    u32 ctrl = rd(R_BE_WCPU_FW_CTRL);
    ctrl &= ~(u32)B_BE_WLANCPU_FWDL_EN;
    wr(R_BE_WCPU_FW_CTRL, ctrl);
    set_card_status(3);
}

/* And the ways it says no, by the card's own number: 4 a bad checksum, 5 and 6
 * a signature it will not accept, 7 an image for different silicon. */
void rtw89_model_firmware_refused(u8 card_number) {
    /* The download stays open.  A card that rejected an image has not finished
     * with it, and clearing the bit here would let a driver that only looks at
     * the bit pass this by accident - which is exactly the bug this was
     * written to catch. */
    u32 ctrl = rd(R_BE_WCPU_FW_CTRL);
    ctrl |= B_BE_WLANCPU_FWDL_EN;
    wr(R_BE_WCPU_FW_CTRL, ctrl);
    set_card_status(card_number);
}

/* ================================================== the radio, in software
 *
 * The radio behind the serial interface, modelled so the conversation with it
 * can be checked rather than assumed.
 *
 * Written to be AWKWARD in the same ways the hardware is, because a model that
 * answers immediately would let all three of the mistakes below pass:
 *
 *   It reports itself busy for a moment after a request, so a driver that
 *   addresses it without waiting is caught.
 *
 *   It clears "done" when a new address arrives and sets it only after the
 *   read has been asked for.  A driver that waits for the wrong bit, or reads
 *   before asking, gets the previous answer - which is the failure that looks
 *   like a radio returning stale values rather than like a driver bug.
 *
 *   Its registers hold twenty bits.  A driver that writes more, or reads
 *   without masking, disagrees with it.
 */
/* The address field is eight bits, so all 256 can be named - and the driver
 * does name one above 64: the lookup-table enable at 0xef.  A model that holds
 * fewer silently drops those writes and reports nothing. */
#define RF_REGISTERS   256

static struct {
    u32  reg[RTW89_RF_PATHS][RF_REGISTERS];
    u32  addressed[RTW89_RF_PATHS];
    bool asked[RTW89_RF_PATHS];
    bool rd_was[RTW89_RF_PATHS];
    int  busy_left[RTW89_RF_PATHS];
    bool read_before_asking;
    bool addressed_while_busy;
    int  writes;
} radio;

void rtw89_model_radio_reset(void) {
    memset(&radio, 0, sizeof radio);
    /* Something recognisable in each, so a read that returns zero is not
     * mistaken for a read that worked. */
    for (int p = 0; p < RTW89_RF_PATHS; p++)
        for (int r = 0; r < RF_REGISTERS; r++)
            radio.reg[p][r] = (u32)(0xA0000 | (p << 12) | r);
}

/* Called from the model's sync, so the interface changes state as the driver
 * touches it rather than only when it is next read. */
static void radio_sync(void) {
    for (int p = 0; p < RTW89_RF_PATHS; p++) {
        u32 ask = R_HWSI_ADD(p);
        u32 answer = R_HWSI_VAL(p);

        u32 a = rd(ask);
        u32 v = rd(answer);

        /* Working through a previous request. */
        if (radio.busy_left[p] > 0) {
            radio.busy_left[p]--;
            if (radio.busy_left[p] == 0) v &= ~(u32)B_HWSI_VAL_BUSY;
            wr(answer, v);
            continue;
        }

        u32 addr = (a & B_HWSI_ADD_MASK) >> B_HWSI_ADD_SHIFT;

        /* A new address while still busy is the driver getting ahead. */
        if (addr != radio.addressed[p]) {
            if (v & B_HWSI_VAL_BUSY) radio.addressed_while_busy = true;
            radio.addressed[p] = addr;
            radio.asked[p] = false;
            v &= ~(u32)B_HWSI_VAL_RDONE;     /* the old answer is not this one */
            wr(answer, v);
        }

        /* Asking is an EDGE, not a level.
         *
         * Every read starts by writing 1 into the low three bits of the
         * address register, which clears the read bit, and only then sets it
         * again.  That rising edge is the request.
         *
         * Waiting for the address to change instead - which is what this did -
         * makes reading the same register twice return the first answer the
         * second time, because nothing tells the model a second question was
         * asked.  That is a model that reports a correct driver as broken, and
         * it did: tuning reads a register the caller had just read, got the
         * stale answer, and wrote it back.  The driver was right and matches
         * the reference line for line.
         */
        bool rd_now = (a & B_HWSI_ADD_RD) != 0;
        if (rd_now && !radio.rd_was[p]) {
            radio.asked[p] = true;
            /* The answer, and only now. */
            u32 value = addr < RF_REGISTERS ? radio.reg[p][addr] : 0;
            wr(answer, (value & RTW89_RF_MASK) | B_HWSI_VAL_RDONE);
        }
        radio.rd_was[p] = rd_now;

        /* A write: the address and the value arrive together in one word. */
        u32 d = rd(R_HWSI_DATA(p));
        if (d) {
            u32 waddr = d & B_HWSI_DATA_ADDR_MASK;
            u32 wval = (d & B_HWSI_DATA_VAL_MASK) >> B_HWSI_DATA_VAL_SHIFT;
            if (waddr < RF_REGISTERS) radio.reg[p][waddr] = wval & RTW89_RF_MASK;
            radio.writes++;
            wr(R_HWSI_DATA(p), 0);
            /* Busy for a moment afterwards, as the real one is. */
            radio.busy_left[p] = 2;
            wr(R_HWSI_VAL(p), rd(R_HWSI_VAL(p)) | B_HWSI_VAL_BUSY);
        }
    }
}

u32  rtw89_model_radio_peek(int path, u32 addr) {
    if (path < 0 || path >= RTW89_RF_PATHS || addr >= RF_REGISTERS) return 0;
    return radio.reg[path][addr];
}
int  rtw89_model_radio_writes(void) { return radio.writes; }
bool rtw89_model_radio_misused(void) {
    return radio.read_before_asking || radio.addressed_while_busy;
}

/* ============================================ a card that fetches and fills
 *
 * Everything modelled above answers register writes.  This does the other
 * thing a card does: it goes and gets what the descriptors point at, and
 * writes back into memory the host set aside.
 *
 * That is what makes it able to catch the mistakes the register-level models
 * cannot see - a request assembled correctly but left somewhere the descriptor
 * does not point at, an answer written before anywhere was posted to receive
 * it, a length in the descriptor that does not match the packet.
 */
static bool answered_before_posted;
static int  requests_fetched;
static bool rfk_asked[16];
static bool rfk_wrong_channel;

/* Whether the card could really reach what it has been pointed at.
 *
 * A real card would issue the read regardless and the machine would answer
 * however its chipset answers - which is not something a model can imitate,
 * and following the address here means dereferencing whatever number happens
 * to be in a register.  One test deliberately leaves 0x1deadb000 in a ring
 * register to prove the driver writes the high half of an address, and a model
 * that follows it faults on a driver that is behaving correctly.
 *
 * So it is bounded and reported instead.  Being pointed at memory that does
 * not exist is a genuine driver fault - it is what a ring left over from
 * something else looks like - and it is worth naming rather than crashing on.
 */
static bool pointed_at_nothing;

static bool card_can_reach(u64 phys, u32 len) {
    u64 top = pmm_total_bytes();
    if (!top) return false;
    if (phys < PAGE_SIZE) return false;          /* the first page is nobody's */
    if (phys + (u64)len < phys) return false;    /* wrapped */
    if (phys + (u64)len > top) return false;
    return true;
}

bool rtw89_model_pointed_at_nothing(void) { return pointed_at_nothing; }

/* Read a descriptor out of a ring the host handed over. */
static bool read_bd(u64 ring_phys, u16 index, u32 *len, u32 *addr, u16 *opt) {
    u64 at = ring_phys + (u64)index * RTW89_PCI_BD_BYTES;
    if (!card_can_reach(at, RTW89_PCI_BD_BYTES)) {
        pointed_at_nothing = true;
        if (len) *len = 0;
        if (addr) *addr = 0;
        if (opt) *opt = 0;
        return false;
    }

    const u8 *bd = (const u8 *)phys_to_virt(ring_phys) +
                   (size_t)index * RTW89_PCI_BD_BYTES;
    if (len)  *len  = (u32)bd[0] | ((u32)bd[1] << 8);
    if (opt)  *opt  = (u16)((u32)bd[2] | ((u32)bd[3] << 8));
    if (addr) *addr = (u32)bd[4] | ((u32)bd[5] << 8) |
                      ((u32)bd[6] << 16) | ((u32)bd[7] << 24);
    return true;
}

/* The card's side of one exchange, run when the host rings the doorbell. */
static void serve_request(void) {
    u64 tx_ring = ((u64)rd(R_BE_CH12_TXBD_DESA_H) << 32) |
                  rd(R_BE_CH12_TXBD_DESA_L);
    u64 rx_ring = ((u64)rd(R_BE_RXQ0_RXBD_DESA_H) << 32) |
                  rd(R_BE_RXQ0_RXBD_DESA_L);
    if (!tx_ring || !rx_ring) return;

    u16 tx_head = (u16)(rd(R_BE_CH12_TXBD_IDX) & BD_HOST_IDX_MASK);
    if (tx_head == model_tx_taken) return;            /* nothing new */

    u32 len = 0, addr = 0;
    u16 opt = 0;
    read_bd(tx_ring, model_tx_taken, &len, &addr, &opt);
    model_tx_taken = (u16)(tx_head);

    if (!addr || !len) return;
    if (!card_can_reach((u64)addr, len)) { pointed_at_nothing = true; return; }
    requests_fetched++;

    /* What the host actually asked. */
    const u8 *req = (const u8 *)phys_to_virt((u64)addr);
    u32 w0 = (u32)req[0] | ((u32)req[1] << 8) |
             ((u32)req[2] << 16) | ((u32)req[3] << 24);
    u8 cat = (u8)(w0 & 0x3);
    u8 cls = (u8)((w0 >> 2) & 0x3F);
    u8 func = (u8)((w0 >> 8) & 0xFF);

    /* A radio calibration is asked for on one class and reported on another,
     * so the reply is not simply the request echoed back. */
    bool is_rfk = (cat == H2C_CAT_OUTSRC && cls == H2C_CL_OUTSRC_RF_FW_RFK);

    /* Somewhere to put the answer.  If the host has posted nothing, a real
     * card drops the answer - and a driver that asks before posting would
     * otherwise pass here and fail on hardware. */
    rx_accounting();
    if (!model_rx_free) { answered_before_posted = true; return; }

    u32 rx_size = 0, rx_addr = 0;
    read_bd(rx_ring, model_rx_taken, &rx_size, &rx_addr, NULL);
    if (!rx_addr || rx_size < 32) return;
    if (!card_can_reach((u64)rx_addr, rx_size)) {
        pointed_at_nothing = true;
        return;
    }

    /* The answer: the card's own word, then a reply, then a payload the test
     * can recognise. */
    u8 *out = (u8 *)phys_to_virt((u64)rx_addr);

    static const u8 ordinary_body[4] = { 0xC0, 0xFF, 0xEE, 0x01 };
    u8 rfk_body[2] = { RTW89_RFK_STATE_OK, 1 };

    const u8 *answer_body = ordinary_body;
    u32 answer_bytes = (u32)sizeof ordinary_body;
    u8 reply_cls = cls, reply_func = func;

    if (is_rfk) {
        reply_cls = RTW89_PHY_C2H_RFK_REPORT;
        reply_func = RTW89_PHY_C2H_RFK_REPORT_FUNC_STATE;
        answer_body = rfk_body;
        answer_bytes = (u32)sizeof rfk_body;
        rfk_asked[func & 0xF] = true;

        /* The receiver's standing level is measured where the radio is
         * pointed.  Asking for it before the radio has been tuned - or for a
         * different channel from the one it is on - measures the wrong band,
         * and the card is the only thing that can tell.
         *
         * A real part reports this as a failure rather than a refusal, which
         * is worth imitating: it is the difference between a driver that
         * notices and one that carries on with a radio calibrated for
         * somewhere else. */
        if (func == H2C_FUNC_RFK_RXDCK_OFFLOAD) {
            u8 asked_ch = (len > H2C_HEADER_LEN + 6)
                          ? req[H2C_HEADER_LEN + 6] : 0;
            u8 tuned_ch = (u8)(radio.reg[0][0x18] & 0xFF);
            if (!tuned_ch || asked_ch != tuned_ch) {
                rfk_wrong_channel = true;
                rfk_body[0] = RTW89_RFK_STATE_FAIL;
            }
        }
    }

    u32 reply_len = H2C_HEADER_LEN + answer_bytes;

    u32 info = reply_len | (1u << 14) | (1u << 15) |
               ((u32)model_rx_tag << 16);
    out[0] = (u8)info;        out[1] = (u8)(info >> 8);
    out[2] = (u8)(info >> 16); out[3] = (u8)(info >> 24);

    u8 *body = out + 4;
    u32 r0 = ((u32)1 & 0x3) | (((u32)reply_cls & 0x3F) << 2) |
             (((u32)reply_func) << 8);
    u32 r1 = reply_len;
    body[0] = (u8)r0; body[1] = (u8)(r0 >> 8);
    body[2] = (u8)(r0 >> 16); body[3] = (u8)(r0 >> 24);
    body[4] = (u8)r1; body[5] = (u8)(r1 >> 8);
    body[6] = (u8)(r1 >> 16); body[7] = (u8)(r1 >> 24);
    memcpy(body + H2C_HEADER_LEN, answer_body, answer_bytes);

    model_rx_tag++;
    if (model_rx_tag > RTW89_RX_TAG_MAX) model_rx_tag = 1;

    /* And say a buffer was filled, which is how the host learns without an
     * interrupt. */
    /* Into the card's half of the register, leaving the host's alone - which
     * is what the silicon does, and what a driver writing the whole word would
     * destroy. */
    rx_took_one((u16)(rd(R_BE_RXQ0_RXBD_NUM) & 0xFFFFu));
    u32 idx = rd(R_BE_RXQ0_RXBD_IDX_V1);
    idx = (idx & ~(u32)BD_CARD_IDX_MASK) |
          (((u32)model_rx_taken << BD_CARD_IDX_SHIFT) & BD_CARD_IDX_MASK);
    wr(R_BE_RXQ0_RXBD_IDX_V1, idx);
}

/* What the card knows about the receive ring.
 *
 * Two indexes that are equal mean the ring is full here, so availability
 * cannot be read off them.  The card tracks it instead: it starts with every
 * buffer when the host points it at a ring, gains one every time the host
 * moves its index on, and loses one every time it fills a buffer.
 */
static u32 last_rx_ring_base;
static u16 last_host_index;

static void rx_accounting(void) {
    u32 base = rd(R_BE_RXQ0_RXBD_DESA_L);
    u16 len = (u16)(rd(R_BE_RXQ0_RXBD_NUM) & 0xFFFFu);
    if (!base || !len) return;

    u16 host = (u16)(rd(R_BE_RXQ0_RXBD_IDX_V1) & BD_HOST_IDX_MASK);

    /* A new ring: everything in it is available, and nothing about the old
     * one carries over. */
    if (base != last_rx_ring_base) {
        last_rx_ring_base = base;
        last_host_index = host;
        model_rx_taken = 0;
        model_rx_free = len;
        return;
    }

    if (host != last_host_index) {
        u16 added = (u16)(host >= last_host_index
                          ? host - last_host_index
                          : len - (last_host_index - host));
        model_rx_free = (u16)(model_rx_free + added);
        if (model_rx_free > len) model_rx_free = len;
        last_host_index = host;
    }
}

static void rx_took_one(u16 len) {
    if (model_rx_free) model_rx_free--;
    model_rx_taken = (u16)((model_rx_taken + 1) % (len ? len : 1));
}

int  rtw89_model_requests_fetched(void) { return requests_fetched; }
bool rtw89_model_rfk_asked(u8 func) { return rfk_asked[func & 0xF]; }
bool rtw89_model_rfk_wrong_channel(void) { return rfk_wrong_channel; }
bool rtw89_model_answered_into_nothing(void) { return answered_before_posted; }
void rtw89_model_channel_reset(void) {
    pointed_at_nothing = false;
    last_rx_ring_base = 0;
    model_rx_free = 0;
    answered_before_posted = false;
    requests_fetched = 0;
    model_tx_taken = 0;
    model_rx_taken = 0;
    model_rx_tag = 1;
}

/* ==================================== a card that fetches frames ==========
 *
 * The sending ring, served the way the card serves it: take the descriptor,
 * read what it points at, and read the TRANSMIT descriptor that sits in front
 * of the frame - then find the frame where that descriptor says it is.
 *
 * Everything here is worked out from the raw bytes at hand-computed offsets
 * rather than through the driver's own structures.  Sharing them would mean a
 * descriptor laid out wrongly is laid out wrongly on both sides and agrees
 * with itself, which is the failure this whole file exists to avoid.
 */
static int  frames_sent;
static bool frame_length_disagreed;
static bool frame_on_wrong_ring;
static bool frame_not_where_promised;
static u8   last_frame[256];
static u32  last_frame_len;

static void serve_frames(void) {
    u64 ring = ((u64)rd(R_BE_CH8_TXBD_DESA_H) << 32) | rd(R_BE_CH8_TXBD_DESA_L);
    if (!ring) return;

    u16 head = (u16)(rd(R_BE_CH8_TXBD_IDX) & 0xFFFF);
    if (head == model_frame_taken) return;

    u32 region_len = 0, addr = 0;
    u16 opt = 0;
    read_bd(ring, model_frame_taken, &region_len, &addr, &opt);
    model_frame_taken = head;

    if (!addr || region_len <= 64) return;
    if (!card_can_reach((u64)addr, region_len)) {
        pointed_at_nothing = true;
        return;
    }
    frames_sent++;

    const u8 *txd = (const u8 *)phys_to_virt((u64)addr);

    u32 w0 = (u32)txd[0] | ((u32)txd[1] << 8) |
             ((u32)txd[2] << 16) | ((u32)txd[3] << 24);
    u32 w2 = (u32)txd[8] | ((u32)txd[9] << 8) |
             ((u32)txd[10] << 16) | ((u32)txd[11] << 24);

    u32 said_bytes = w2 & 0x3FFFu;                 /* 13:0  */
    u32 said_ring  = (w0 >> 16) & 0xFu;            /* 19:16 */
    u32 said_at    = ((w0 >> 24) & 0xFu) * 8;      /* 27:24, eight-byte units */

    /* The ring entry says how long the whole region is; the descriptor says
     * how long the frame is.  They have to agree about the descriptor sitting
     * in front of the frame - and a driver that puts the frame's length in the
     * ring entry, or the region's length in the descriptor, passes every other
     * check and sends a truncated or over-long frame. */
    if (region_len != said_bytes + said_at) frame_length_disagreed = true;

    /* The descriptor names its own ring, and the card checks it. */
    if (said_ring != RTW89_TXCH_CH8) frame_on_wrong_ring = true;

    if (said_at + said_bytes > region_len) {
        frame_not_where_promised = true;
        return;
    }

    /* And the frame itself, taken from where the descriptor said - not from a
     * fixed offset, so a descriptor that lies about it is caught. */
    u32 n = said_bytes;
    if (n > sizeof last_frame) n = sizeof last_frame;
    memcpy(last_frame, txd + said_at, n);
    last_frame_len = n;
}

int  rtw89_model_frames_sent(void) { return frames_sent; }
bool rtw89_model_frame_length_disagreed(void) { return frame_length_disagreed; }
bool rtw89_model_frame_on_wrong_ring(void) { return frame_on_wrong_ring; }
bool rtw89_model_frame_not_where_promised(void) { return frame_not_where_promised; }

u32 rtw89_model_last_frame(u8 *out, u32 cap) {
    u32 n = last_frame_len < cap ? last_frame_len : cap;
    if (out && n) memcpy(out, last_frame, n);
    return last_frame_len;
}

void rtw89_model_frames_reset(void) {
    frames_sent = 0;
    frame_length_disagreed = false;
    frame_on_wrong_ring = false;
    frame_not_where_promised = false;
    last_frame_len = 0;
    model_frame_taken = 0;
}

/* ================================== a card that delivers frames ===========
 *
 * The card putting something into a buffer the host posted.
 *
 * The layout written here is built from raw offsets rather than the driver's
 * structures, on purpose: a descriptor the driver lays out wrongly would be
 * laid out wrongly here too if they shared a definition, and the two would
 * agree with each other while disagreeing with the silicon.
 *
 * A radio report is always included.  It is optional on real hardware, but
 * including it means the frame does NOT start immediately after the
 * descriptor - so a receive path that assumes it does is caught here instead
 * of on the air.
 */
#define MODEL_PHY_RPT_BYTES  8

bool rtw89_model_inject_frame(const u8 *frame, u32 len, u8 type,
                              bool crc_error) {
    if (!attached || !frame || !len) return false;

    u64 rx_ring = ((u64)rd(R_BE_RXQ0_RXBD_DESA_H) << 32) |
                  rd(R_BE_RXQ0_RXBD_DESA_L);
    if (!rx_ring) return false;

    /* Only into a buffer the host actually posted.
     *
     * Counted rather than compared.  An equal pair of indexes means the ring
     * is FULL on this part, so the two states that matter - every buffer
     * available and none - look identical from the numbers, and the card is
     * the side that knows which.  It knows because it has been watching the
     * host's index move. */
    rx_accounting();
    if (!model_rx_free) return false;

    u32 size = 0, addr = 0;
    read_bd(rx_ring, model_rx_taken, &size, &addr, NULL);

    u32 total = 4 + RTW89_RXD_SHORT_BYTES + MODEL_PHY_RPT_BYTES + len;
    if (!addr || size < total) return false;
    if (!card_can_reach((u64)addr, total)) {
        pointed_at_nothing = true;
        return false;
    }

    u8 *out = (u8 *)phys_to_virt((u64)addr);
    memset(out, 0, total);

    /* The card's own word: everything after it, and its running count. */
    u32 after = total - 4;
    u32 info = (after & 0x3FFFu) | (1u << 14) | (1u << 15) |
               (((u32)model_rx_tag & 0x1FFFu) << 16);
    out[0] = (u8)info;         out[1] = (u8)(info >> 8);
    out[2] = (u8)(info >> 16); out[3] = (u8)(info >> 24);

    /* The receive descriptor.  Word 0 carries the frame's length, what sits
     * between this and the frame, and what kind of thing it is. */
    u8 *d = out + 4;
    u32 w0 = (len & 0x3FFFu) |
             ((u32)(MODEL_PHY_RPT_BYTES / 8) << 22) |   /* 23:22, 8-byte */
             (((u32)type & 0x3Fu) << 24);               /* 29:24         */
    d[0] = (u8)w0;         d[1] = (u8)(w0 >> 8);
    d[2] = (u8)(w0 >> 16); d[3] = (u8)(w0 >> 24);

    /* Word 3: whether it survived the air, and whether it was for us. */
    u32 w3 = (1u << 10);                                /* A1 matched     */
    if (crc_error) w3 |= (1u << 6);
    d[12] = (u8)w3;         d[13] = (u8)(w3 >> 8);
    d[14] = (u8)(w3 >> 16); d[15] = (u8)(w3 >> 24);

    /* The radio report.  A known value, so the conversion into decibels can
     * be checked rather than merely happening: 100 raw is (100 >> 1) - 110,
     * which is -60 dBm - an ordinary indoor signal. */
    u8 *rpt = out + 4 + RTW89_RXD_SHORT_BYTES;
    u32 r0 = 100u & BE_RXD_PHY_RSSI;
    rpt[0] = (u8)r0;         rpt[1] = (u8)(r0 >> 8);
    rpt[2] = (u8)(r0 >> 16); rpt[3] = (u8)(r0 >> 24);

    /* Then the frame behind it. */
    memcpy(out + 4 + RTW89_RXD_SHORT_BYTES + MODEL_PHY_RPT_BYTES, frame, len);

    model_rx_tag++;
    if (model_rx_tag > RTW89_RX_TAG_MAX) model_rx_tag = 1;

    /* And the card's half of the index, so the host learns without an
     * interrupt. */
    rx_took_one((u16)(rd(R_BE_RXQ0_RXBD_NUM) & 0xFFFFu));
    u32 idx = rd(R_BE_RXQ0_RXBD_IDX_V1);
    idx = (idx & ~(u32)BD_CARD_IDX_MASK) |
          (((u32)model_rx_taken << BD_CARD_IDX_SHIFT) & BD_CARD_IDX_MASK);
    wr(R_BE_RXQ0_RXBD_IDX_V1, idx);
    return true;
}
