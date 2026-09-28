/* nv_dp_model.c - a monitor on the end of a cable that is not there.
 *
 * The DisplayPort driver next door cannot be run on anything available: there
 * is no NVIDIA card, and if there were, its outputs would go to whatever the
 * host had plugged in.  So this stands in for both halves - the card's AUX
 * engine and the monitor on the far end of the pair.
 *
 * It is deliberately awkward, in the specific ways real monitors are awkward,
 * because a model that answers everything immediately and correctly proves
 * only that the driver can talk to a model:
 *
 *   It refuses a transaction started without the pad being requested first.
 *   Sharing that pad is a real constraint and a driver that skips the
 *   handshake works right up until something else on the card wants it.
 *
 *   It defers.  A monitor that is busy says "ask me again", as many times as
 *   it likes, and a driver that reads that as a failure works on the monitor
 *   it was written against and no other.
 *
 *   It returns fewer bytes than were asked for.  The reply carries a count and
 *   it is allowed to be short; a driver that believes its own request instead
 *   reads bytes nobody sent.
 *
 *   Its cable will not carry 8.1 Gbps.  Clock recovery never completes at that
 *   rate however hard the transmitter is driven, which is exactly what a long
 *   or cheap cable does - so the driver has to notice it has run out of
 *   voltage swing and fall back rather than loop.  At 5.4 it works, but only
 *   once the swing and pre-emphasis have been raised to what it asked for.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: the AUX transaction sequence including the pad
 * handshake, defer retries and short transfers; the training state machine
 * through clock recovery and channel equalisation; the adjustment loop; both
 * of the escape conditions; and the fall back to a rate that works.
 *
 * What it cannot establish: anything electrical.  Whether a real cable at a
 * real voltage swing carries a real signal is not a question a model can
 * answer, and this one says the driver's logic is right, not that a picture
 * would appear.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

/* What this cable needs before the monitor can read it, and what it cannot do
 * at all. */
#define NEED_SWING        2
#define NEED_PREEMPHASIS  1
#define CABLE_TOO_SLOW_FOR DP_RATE_8_10

static u8 model_edid[128];

static struct {
    bool present;
    int  channel;
    volatile u8 *window;

    bool granted;
    bool saw_unrequested;

    /* The monitor's own registers, as far as they are used. */
    u8   link_bw;
    u8   lane_count;       /* low bits; the top bit is enhanced framing     */
    u8   pattern;
    u8   lane_set[4];
    u8   power;

    /* Where an emulated I2C transaction has got to. */
    u8   i2c_address;
    u8   i2c_offset;
    bool i2c_open;
    bool i2c_short_done;    /* the one deliberately short transfer          */

    /* How far training has got, and how hard the driver has had to work. */
    int  transactions;
    int  defers;
    int  pattern_changes;
    int  deferred_this_read;
    bool cr_done;
    bool eq_done;
    u8   trained_rate, trained_lanes;
    bool trained;

    /* How many times the swing has been asked for at this rate, so a rate the
     * cable cannot carry escalates rather than sitting still. */
    u8   asking_swing;
} dp;

static u32 reg_get(u32 offset) {
    if (!dp.window) return 0;
    return *(volatile u32 *)(dp.window + offset);
}

static void reg_set(u32 offset, u32 value) {
    if (!dp.window) return;
    *(volatile u32 *)(dp.window + offset) = value;
}

/* ------------------------------------------------------------ the monitor */

static void build_edid(void) {
    u8 *e = model_edid;
    memset(e, 0, 128);

    static const u8 header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(e, header, 8);

    u16 maker = (u16)((('K' - 'A' + 1) << 10) | (('S' - 'A' + 1) << 5) |
                      ('T' - 'A' + 1));
    e[8] = (u8)(maker >> 8);
    e[9] = (u8)maker;
    e[10] = 0x02; e[11] = 0x50;
    e[16] = 12; e[17] = 35;
    e[18] = 1; e[19] = 4;
    e[20] = 0xB5;                    /* digital, 10 bits, DisplayPort        */
    e[21] = 70; e[22] = 39;
    e[24] = 0x06;

    /* 3840 by 2160 at sixty: 594 MHz, which needs more than four lanes at 2.7
     * can carry and less than four at 5.4 - so which rate the driver lands on
     * is a fact rather than a preference. */
    u16 clock = (u16)(594000 / 10);
    u32 h_active = 3840, h_blank = 560;
    u32 v_active = 2160, v_blank = 90;

    u8 *d = e + 54;
    d[0] = (u8)clock; d[1] = (u8)(clock >> 8);
    d[2] = (u8)h_active;
    d[3] = (u8)h_blank;
    d[4] = (u8)(((h_active >> 8) << 4) | (h_blank >> 8));
    d[5] = (u8)v_active;
    d[6] = (u8)v_blank;
    d[7] = (u8)(((v_active >> 8) << 4) | (v_blank >> 8));
    d[8] = 176; d[9] = 88; d[10] = 0x18;
    d[17] = 0x1E;

    d = e + 72;
    d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0xFC; d[4] = 0;
    memcpy(d + 5, "Kestrel DP", 10);
    d[15] = 0x0A; d[16] = 0x20; d[17] = 0x20;

    u8 sum = 0;
    for (int i = 0; i < 127; i++) sum = (u8)(sum + e[i]);
    e[127] = (u8)(0x100 - sum);
}

/* How many lanes are live, and what each one has been driven to. */
static int active_lanes(void) {
    int n = dp.lane_count & 0x1F;
    return n > 4 ? 4 : n;
}

static bool drive_is_enough(void) {
    int n = active_lanes();
    if (!n) return false;
    for (int i = 0; i < n; i++)
        if ((dp.lane_set[i] & 0x3) < NEED_SWING) return false;
    return true;
}

static bool preemphasis_is_enough(void) {
    int n = active_lanes();
    for (int i = 0; i < n; i++)
        if (((dp.lane_set[i] >> 3) & 0x3) < NEED_PREEMPHASIS) return false;
    return true;
}

/* The status registers, worked out afresh every time they are read - which is
 * what makes this a model rather than a table. */
static u8 lane_status(int lane) {
    u8 out = 0;
    if (lane >= active_lanes()) return 0;

    /* Nothing happens at all until a training pattern is running. */
    if (dp.pattern == 0) return 0;

    /* This cable will not carry the top rate no matter how hard it is driven,
     * so clock recovery never completes there. */
    if (dp.link_bw == CABLE_TOO_SLOW_FOR) return 0;

    if (dp.pattern >= 1 && drive_is_enough()) out |= DPCD_LANE_CR_DONE;

    if ((out & DPCD_LANE_CR_DONE) && dp.pattern >= 2 && preemphasis_is_enough())
        out |= DPCD_LANE_EQ_DONE | DPCD_LANE_SYMBOL_LOCKED;

    return out;
}

static u8 adjust_for(int lane) {
    (void)lane;
    /* At a rate this cable can carry, ask for what is actually needed.  At the
     * rate it cannot, keep asking for more - which is what a monitor that is
     * seeing nothing does, and is what walks the driver up to the maximum
     * swing so it can tell it has run out. */
    u8 swing, pre;

    if (dp.link_bw == CABLE_TOO_SLOW_FOR) {
        swing = dp.asking_swing;
        pre = 0;
    } else {
        swing = NEED_SWING;
        pre = NEED_PREEMPHASIS;
    }

    if (swing > 3) swing = 3;
    if (swing + pre > 3) pre = (u8)(3 - swing);
    return (u8)(swing | (pre << 2));
}

static u8 dpcd_read_byte(u32 address) {
    switch (address) {
    case DPCD_REV:            return 0x14;          /* DisplayPort 1.4      */
    case DPCD_MAX_LINK_RATE:  return DP_RATE_8_10;
    case DPCD_MAX_LANE_COUNT:
        return (u8)(4 | DPCD_ENHANCED_FRAME_CAP | DPCD_TPS3_SUPPORTED);
    case DPCD_MAX_DOWNSPREAD:  return 0x01;
    case DPCD_TRAINING_AUX_RD_INTERVAL: return 0x00;   /* the default wait  */
    case DPCD_LINK_BW_SET:     return dp.link_bw;
    case DPCD_LANE_COUNT_SET:  return dp.lane_count;
    case DPCD_TRAINING_PATTERN_SET: return dp.pattern;
    case DPCD_SINK_COUNT:      return 1;
    case DPCD_LANE0_1_STATUS:
        return (u8)(lane_status(0) | (lane_status(1) << 4));
    case DPCD_LANE2_3_STATUS:
        return (u8)(lane_status(2) | (lane_status(3) << 4));
    case DPCD_LANE_ALIGN_STATUS: {
        int n = active_lanes();
        bool all = n > 0;
        for (int i = 0; i < n; i++)
            if (!(lane_status(i) & DPCD_LANE_EQ_DONE)) all = false;
        return all ? DPCD_INTERLANE_ALIGN_DONE : 0;
    }
    case DPCD_ADJUST_REQUEST_LANE0_1:
        return (u8)(adjust_for(0) | (adjust_for(1) << 4));
    case DPCD_ADJUST_REQUEST_LANE2_3:
        return (u8)(adjust_for(2) | (adjust_for(3) << 4));
    case DPCD_SET_POWER:       return dp.power;
    default:
        if (address >= DPCD_TRAINING_LANE0_SET &&
            address < DPCD_TRAINING_LANE0_SET + 4)
            return dp.lane_set[address - DPCD_TRAINING_LANE0_SET];
        return 0;
    }
}

static void dpcd_write_byte(u32 address, u8 value) {
    switch (address) {
    case DPCD_LINK_BW_SET:
        if (value != dp.link_bw) dp.asking_swing = 0;
        dp.link_bw = value;
        return;
    case DPCD_LANE_COUNT_SET:
        dp.lane_count = value;
        return;
    case DPCD_TRAINING_PATTERN_SET:
        if ((value & 0x1F) != dp.pattern) dp.pattern_changes++;
        dp.pattern = (u8)(value & 0x1F);

        /* Training ending with the pattern switched off and the link at a
         * usable setting is what "trained" means. */
        if (dp.pattern == 0 && dp.cr_done && dp.eq_done) {
            dp.trained = true;
            dp.trained_rate = dp.link_bw;
            dp.trained_lanes = (u8)active_lanes();
        }
        if (dp.pattern == 0) { dp.cr_done = dp.eq_done = false; }
        return;
    case DPCD_SET_POWER:
        dp.power = value;
        return;
    default:
        if (address >= DPCD_TRAINING_LANE0_SET &&
            address < DPCD_TRAINING_LANE0_SET + 4) {
            dp.lane_set[address - DPCD_TRAINING_LANE0_SET] = value;
            /* Every time the driver pushes a new setting at a rate this cable
             * cannot carry, ask for one more step - so it walks to the top and
             * finds out it has run out. */
            if (dp.link_bw == CABLE_TOO_SLOW_FOR && dp.asking_swing < 3)
                dp.asking_swing++;
        }
        return;
    }
}

/* Recording where training has got to, so that switching the pattern off can
 * tell whether it finished or was abandoned. */
static void note_progress(void) {
    int n = active_lanes();
    if (!n) return;

    bool cr = true, eq = true;
    for (int i = 0; i < n; i++) {
        u8 s = lane_status(i);
        if (!(s & DPCD_LANE_CR_DONE)) cr = false;
        if (!(s & DPCD_LANE_EQ_DONE)) eq = false;
    }
    if (cr) dp.cr_done = true;
    if (cr && eq) dp.eq_done = true;
}

/* ------------------------------------------------------------ the AUX engine */

static void do_transaction(u32 ctrl) {
    u32 base = (u32)dp.channel * 0x50;
    u32 address = reg_get(NV_AUX_ADDR(dp.channel));
    u8  type = (u8)((ctrl >> NV_AUX_CTRL_TYPE_SHIFT) & 0xF);
    u32 length_field = ctrl & 0x1FF;
    int length = (length_field == 0x100) ? 0 : (int)(length_field & 0xFF) + 1;

    dp.transactions++;

    /* Starting one without asking for the pad first. */
    if (!dp.granted) {
        dp.saw_unrequested = true;
        reg_set(NV_AUX_STAT(dp.channel), NV_AUX_STAT_SINK | NV_AUX_STAT_ERROR);
        return;
    }

    u32 stat = NV_AUX_STAT_SINK;
    u8 reply = NV_AUX_ACK;
    u8 out[16];
    memset(out, 0, sizeof out);
    int produced = 0;

    /* A monitor is entitled to be busy.  The first native read of a fresh
     * conversation is deferred once, which is enough to make the driver's
     * retry path run for real. */
    if (type == NV_AUX_NATIVE_READ && dp.deferred_this_read == 0) {
        dp.deferred_this_read = 1;
        dp.defers++;
        reg_set(NV_AUX_STAT(dp.channel),
                NV_AUX_STAT_SINK | ((u32)NV_AUX_DEFER << NV_AUX_STAT_REPLY_SHIFT));
        return;
    }

    switch (type) {
    case NV_AUX_NATIVE_READ:
        note_progress();
        for (int i = 0; i < length; i++) out[i] = dpcd_read_byte(address + i);
        produced = length;
        break;

    case NV_AUX_NATIVE_WRITE: {
        u32 words[4];
        for (int i = 0; i < 4; i++)
            words[i] = reg_get(NV_AUX_DATA_WRITE(dp.channel, i * 4));
        for (int i = 0; i < length; i++)
            dpcd_write_byte(address + i, (u8)(words[i / 4] >> (8 * (i % 4))));
        note_progress();
        produced = length;
        break;
    }

    case NV_AUX_I2C_WRITE:
    case NV_AUX_I2C_WRITE_MOT: {
        if ((address & 0x7F) != 0x50) { reply = NV_AUX_I2C_NACK; break; }
        u32 word = reg_get(NV_AUX_DATA_WRITE(dp.channel, 0));
        if (length >= 1) dp.i2c_offset = (u8)word;
        dp.i2c_open = (type == NV_AUX_I2C_WRITE_MOT);
        produced = length;
        break;
    }

    case NV_AUX_I2C_READ:
    case NV_AUX_I2C_READ_MOT: {
        if ((address & 0x7F) != 0x50) { reply = NV_AUX_I2C_NACK; break; }

        int give = length;
        /* Once, give fewer bytes than were asked for.  It is legal, it
         * happens, and a driver that does not read the count walks off the end
         * of what it was actually sent. */
        if (!dp.i2c_short_done && give > 8) { give = 8; dp.i2c_short_done = true; }

        for (int i = 0; i < give; i++)
            out[i] = model_edid[(dp.i2c_offset + i) & 0x7F];
        dp.i2c_offset = (u8)(dp.i2c_offset + give);
        dp.i2c_open = (type == NV_AUX_I2C_READ_MOT);
        produced = give;
        break;
    }

    default:
        reply = NV_AUX_NACK;
        break;
    }

    /* The next read gets one defer again only after a write has happened, so a
     * long run of reads is not made pointlessly slow. */
    if (type == NV_AUX_NATIVE_WRITE) dp.deferred_this_read = 0;

    if (reply == NV_AUX_ACK && produced > 0 && (type & 1)) {
        u32 words[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < produced && i < 16; i++)
            words[i / 4] |= (u32)out[i] << (8 * (i % 4));
        for (int i = 0; i < 4; i++)
            reg_set(NV_AUX_DATA_READ(dp.channel, i * 4), words[i]);
    }

    stat |= (u32)reply << NV_AUX_STAT_REPLY_SHIFT;
    stat |= (u32)(produced & 0x1F);
    reg_set(NV_AUX_STAT(dp.channel), stat);
    (void)base;
}

void nv_dp_model_write(u32 offset, u32 value) {
    if (!dp.present) return;

    /* The AUX engine exists on every channel; the monitor is on one of them.
     * A driver has to be able to ask an empty connector and be told nothing is
     * there, which is not the same as the pad refusing to be granted. */
    int channel = -1;
    for (int i = 0; i < 8; i++)
        if (offset == (u32)NV_AUX_CTRL(i)) { channel = i; break; }
    if (channel < 0) return;

    /* A transaction is looked for first, because the request bits stay set for
     * as long as the pad is held - so testing them first would swallow every
     * transaction made while holding it, which is all of them. */
    if (value & NV_AUX_CTRL_TRANSACT) {
        if (channel == dp.channel) {
            do_transaction(value);
        } else {
            /* Nothing plugged in here.  The transaction still completes; it
             * just says so. */
            reg_set(NV_AUX_STAT(channel), 0);
        }

        u32 now = reg_get(offset);
        reg_set(offset, now & ~NV_AUX_CTRL_TRANSACT);
        return;
    }

    if ((value & NV_AUX_CTRL_REQ_MASK) == NV_AUX_CTRL_REQUEST) {
        if (channel == dp.channel) dp.granted = true;
        reg_set(offset, (value & ~NV_AUX_CTRL_REP_MASK) | NV_AUX_CTRL_GRANTED);
        return;
    }

    if (!(value & NV_AUX_CTRL_REQ_MASK)) {
        if (channel == dp.channel) dp.granted = false;
        reg_set(offset, value & ~NV_AUX_CTRL_REP_MASK);
        return;
    }
}

void nv_dp_model_read(u32 offset) { (void)offset; }

/* --------------------------------------------------------------- bringing up */

void nv_dp_model_attach(int channel) {
    nv_card_t *c = nv_model_card();
    if (!c || !c->regs) return;

    memset(&dp, 0, sizeof dp);
    dp.present = true;
    dp.channel = channel;
    dp.window = c->regs;

    build_edid();

    /* A connector with a monitor plugged into it, and the pad idle. */
    reg_set(NV_AUX_CTRL(channel), 0);
    reg_set(NV_AUX_STAT(channel), NV_AUX_STAT_SINK);

    /* Every other channel reads back as nothing plugged in, which is the
     * normal state of most of a card's connectors and is what the driver has
     * to cope with without complaining. */
    for (int i = 0; i < 8; i++) {
        if (i == channel) continue;
        reg_set(NV_AUX_CTRL(i), 0);
        reg_set(NV_AUX_STAT(i), 0);
    }
}

void nv_dp_model_detach(void) { dp.present = false; }

int  nv_dp_model_transactions(void) { return dp.transactions; }
int  nv_dp_model_defers(void)       { return dp.defers; }
bool nv_dp_model_trained(void)      { return dp.trained; }
u8   nv_dp_model_trained_rate(void) { return dp.trained_rate; }
u8   nv_dp_model_trained_lanes(void){ return dp.trained_lanes; }
int  nv_dp_model_pattern_changes(void) { return dp.pattern_changes; }
bool nv_dp_model_saw_unrequested(void) { return dp.saw_unrequested; }
const u8 *nv_dp_model_edid(void) { return model_edid; }

/* ------------------------------------------------------------------- test */

int nv_dp_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) {
        kinfo("nv-dp", "no card to try this on");
        return 0;
    }

    const int channel = 3;
    nv_dp_model_attach(channel);

    static nv_dp_link_t link;
    memset(&link, 0, sizeof link);
    link.channel = channel;
    link.sor = 1;
    link.link = 0;

    /* What the monitor says it can do. */
    if (!nv_dp_read_caps(c, &link)) {
        kerr("nv-dp", "the monitor's capabilities were not read");
        failures++;
    } else if (link.rev != 0x14 || link.max_lanes != 4 ||
               link.max_rate != DP_RATE_8_10 || !link.tps3 ||
               !link.enhanced_framing) {
        kerr("nv-dp", "the capabilities came out as rev %02x, %u lanes, rate "
                      "%02x", link.rev, link.max_lanes, link.max_rate);
        failures++;
    }

    /* Its identification block, over the emulated bus on the same pair - and
     * through a transfer the monitor deliberately cuts short. */
    u8 edid[128];
    if (!nv_dp_read_edid(c, &link, edid)) {
        kerr("nv-dp", "the identification block was not read over AUX");
        failures++;
    } else if (memcmp(edid, nv_dp_model_edid(), 128)) {
        kerr("nv-dp", "the identification block came back altered");
        failures++;
    } else {
        u32 pixel_khz = ((u32)edid[54] | ((u32)edid[55] << 8)) * 10;
        u32 width = edid[56] | ((u32)(edid[58] & 0xF0) << 4);
        u32 height = edid[59] | ((u32)(edid[61] & 0xF0) << 4);
        kinfo("nv-dp", "the monitor answered over AUX: %ux%u, %u.%u MHz",
              width, height, pixel_khz / 1000, (pixel_khz % 1000) / 100);
    }

    /* A defer has to have happened, or the retry path was never run. */
    if (nv_dp_model_defers() == 0) {
        kerr("nv-dp", "the monitor never deferred, so coping with one was "
                      "never tested");
        failures++;
    }

    /* And the training itself.  3840 by 2160 at sixty needs 594 MHz of pixels,
     * which at 24 bits each is more than four lanes at 2.7 Gbps carry and less
     * than four at 5.4 - and this cable will not do 8.1 at all.  So there is
     * exactly one answer, and getting there means noticing the top rate is
     * hopeless and dropping to the next. */
    if (!nv_dp_train(c, &link, 594000, 24)) {
        kerr("nv-dp", "the link would not train");
        failures++;
    } else if (link.rate != DP_RATE_5_40 || link.lanes != 4) {
        kerr("nv-dp", "it trained at rate %02x on %u lanes, expected 5.4 Gbps "
                      "on 4", link.rate, link.lanes);
        failures++;
    } else if (link.fallbacks == 0) {
        kerr("nv-dp", "it reached 5.4 Gbps without ever failing at 8.1, so the "
                      "fallback was not what got it there");
        failures++;
    } else if (link.swing[0] < NEED_SWING ||
               link.preemphasis[0] < NEED_PREEMPHASIS) {
        kerr("nv-dp", "it trained without raising the drive: swing %u, "
                      "pre-emphasis %u", link.swing[0], link.preemphasis[0]);
        failures++;
    } else if (!nv_dp_model_trained()) {
        kerr("nv-dp", "the driver believes the link is trained and the monitor "
                      "does not");
        failures++;
    } else {
        kinfo("nv-dp", "the link trained: %u lanes at 5.4 Gbps after failing "
                       "at 8.1, %d attempt(s), %d clock-recovery and %d "
                       "equalisation rounds, swing %u pre-emphasis %u",
              link.lanes, link.attempts, link.cr_loops, link.eq_loops,
              link.swing[0], link.preemphasis[0]);
    }

    /* The pad handshake was honoured throughout. */
    if (nv_dp_model_saw_unrequested()) {
        kerr("nv-dp", "a transaction was started without requesting the pad");
        failures++;
    }

    /* A connector with nothing in it has to come back as nothing plugged in
     * rather than as a monitor that will not answer. */
    {
        static nv_dp_link_t empty;
        memset(&empty, 0, sizeof empty);
        empty.channel = 5;
        u8 byte = 0, one = 1;
        int reply = nv_aux_transfer(c, empty.channel, NV_AUX_NATIVE_READ,
                                    DPCD_REV, &byte, &one);
        if (reply != -2) {
            kerr("nv-dp", "an empty connector answered with %d", reply);
            failures++;
        }
    }

    /* And a mode nothing this monitor supports has to be refused rather than
     * trained badly: 7680 by 4320 at sixty needs far more than four lanes at
     * 8.1 Gbps carry even if the cable could do it. */
    {
        static nv_dp_link_t big;
        memcpy(&big, &link, sizeof big);
        big.trained = false;
        if (nv_dp_train(c, &big, 2376000, 24)) {
            kerr("nv-dp", "a mode too big for the link was trained anyway");
            failures++;
        }
    }

    int transactions = nv_dp_model_transactions();
    nv_dp_model_detach();

    if (!failures)
        kinfo("nv-dp", "DisplayPort works: %d AUX transactions, the monitor "
                       "read over the pair, a deferred reply and a short "
                       "transfer both handled, and the link trained by "
                       "negotiation", transactions);
    return failures;
}
