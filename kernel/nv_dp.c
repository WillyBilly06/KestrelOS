/* nv_dp.c - DisplayPort: talking to the monitor, and getting the link to work.
 *
 * Every output on a modern NVIDIA card is DisplayPort or HDMI, and the
 * DisplayPort ones do not work at all until the link has been trained.  That
 * is the part people are surprised by: the cable is not a wire that carries
 * pixels, it is four differential pairs whose voltage swing and pre-emphasis
 * have to be negotiated with the monitor, one adjustment at a time, until the
 * monitor says it can read them.  Until that conversation finishes the screen
 * is black no matter what the display engine is doing.
 *
 * The conversation happens over a fifth pair called AUX, which carries
 * addressed transactions into a register space inside the monitor.  Everything
 * a driver needs is in there: what the monitor can do, what it is seeing on
 * each lane right now, and what it would like changed.  The monitor's
 * identification block is read through the same pair, over an emulated I2C
 * bus, because the two-wire bus it used to sit on no longer exists.
 *
 * There are four things here that a driver gets wrong once each:
 *
 *   A defer is not a failure.  A monitor that is busy answers "ask me again",
 *   and it is entitled to do that many times in a row.  Treating it as an
 *   error produces a driver that works on the monitor it was written against
 *   and fails on slower ones.
 *
 *   The reply says how many bytes were actually transferred, and it is allowed
 *   to be fewer than were asked for.  Believing the request rather than the
 *   reply reads uninitialised bytes and calls them data.
 *
 *   Clock recovery has two separate escape conditions and both are required:
 *   five attempts at the same voltage swing, and the swing reaching its
 *   maximum.  A loop with only a counter takes far too long to give up on a
 *   cable that cannot carry the rate; a loop with only the maximum check
 *   spins forever on one that oscillates.
 *
 *   When training fails, the answer is to try again slower - fewer lanes, or a
 *   lower rate - not to give up.  A long or thin cable that cannot do 8.1 Gbps
 *   on four lanes will usually do 5.4 on four, and a driver that does not fall
 *   back reports a dead monitor that works perfectly with any other driver.
 *
 * ---------------------------------------------------------------------------
 * What this establishes and what it cannot.  The register protocol is the one
 * Maxwell 2 introduced and every card since has kept, and the monitor-side
 * addresses are the DisplayPort specification's.  Run against the model in
 * nv_dp_model.c, this proves the transaction sequence, the defer and
 * short-transfer handling, the training state machine, the adjustment loop and
 * the fallback.  It cannot prove anything electrical: whether a real cable at
 * a real swing carries a real signal is not a thing a model can answer.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

static void nv_mask32(nv_card_t *c, u32 offset, u32 mask, u32 value) {
    u32 was = nv_rd32(c, offset);
    nv_wr32(c, offset, (was & ~mask) | (value & mask));
}

/* ------------------------------------------------------------ transactions
 *
 * The pad has to be requested before a transaction and released after.  It is
 * shared with other things on the card, so a driver that takes it and does not
 * give it back works until something else wants it - which is a fault that
 * shows up much later and somewhere else.
 */
static bool aux_begin(nv_card_t *c, int channel) {
    u32 ctrl = 0;

    /* Anything already in flight has to finish first. */
    for (int i = 0; i < 1000; i++) {
        ctrl = nv_rd32(c, NV_AUX_CTRL(channel));
        if (!(ctrl & NV_AUX_CTRL_BUSY_MASK)) break;
        timer_udelay(1);
    }
    if (ctrl & NV_AUX_CTRL_BUSY_MASK) {
        kwarn("nv-dp", "the AUX pad on channel %d never went idle (%08x)",
              channel, ctrl);
        return false;
    }

    nv_mask32(c, NV_AUX_CTRL(channel), NV_AUX_CTRL_REQ_MASK,
              NV_AUX_CTRL_REQUEST);

    for (int i = 0; i < 1000; i++) {
        ctrl = nv_rd32(c, NV_AUX_CTRL(channel));
        if ((ctrl & NV_AUX_CTRL_REP_MASK) == NV_AUX_CTRL_GRANTED) return true;
        timer_udelay(1);
    }

    kwarn("nv-dp", "the AUX pad on channel %d was never granted (%08x)",
          channel, ctrl);
    nv_mask32(c, NV_AUX_CTRL(channel), 0x00710000u, 0);
    return false;
}

static void aux_end(nv_card_t *c, int channel) {
    nv_mask32(c, NV_AUX_CTRL(channel), 0x00710000u, 0);
}

/* One transaction.  Returns the reply nibble, or a negative number if it never
 * completed.  On a read, *size comes back as the number of bytes the monitor
 * actually sent. */
int nv_aux_transfer(nv_card_t *c, int channel, u8 type, u32 address,
                    u8 *data, u8 *size) {
    if (!size || *size > 16) return -1;

    if (!aux_begin(c, channel)) return -1;

    u32 stat = nv_rd32(c, NV_AUX_STAT(channel));
    if (!(stat & NV_AUX_STAT_SINK)) {
        /* Nothing plugged in.  Not an error worth a warning - it is the normal
         * state of most of a card's connectors. */
        aux_end(c, channel);
        return -2;
    }

    bool writing = !(type & 1);

    if (writing) {
        /* The payload goes out as four words whether it is four bytes or
         * sixteen; the length in the control register is what decides how many
         * are actually sent. */
        u32 out[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < *size && i < 16; i++)
            out[i / 4] |= (u32)data[i] << (8 * (i % 4));
        for (int i = 0; i < 4; i++)
            nv_wr32(c, NV_AUX_DATA_WRITE(channel, i * 4), out[i]);
    }

    u32 ctrl = nv_rd32(c, NV_AUX_CTRL(channel));
    ctrl &= ~NV_AUX_CTRL_FIELDS;
    ctrl |= (u32)type << NV_AUX_CTRL_TYPE_SHIFT;
    /* A length of zero is how an address-only transaction is asked for, and it
     * is encoded as a value that cannot be confused with a length of one. */
    ctrl |= *size ? (u32)(*size - 1) : 0x00000100u;

    nv_wr32(c, NV_AUX_ADDR(channel), address);

    int reply = NV_AUX_NACK;
    int deferred = 0;

    for (int attempt = 0; attempt < 32; attempt++) {
        nv_wr32(c, NV_AUX_CTRL(channel), NV_AUX_CTRL_RESET | ctrl);
        nv_wr32(c, NV_AUX_CTRL(channel), ctrl);
        if (attempt) timer_udelay(400);

        nv_wr32(c, NV_AUX_CTRL(channel), NV_AUX_CTRL_TRANSACT | ctrl);

        bool finished = false;
        for (int i = 0; i < 2000; i++) {
            u32 now = nv_rd32(c, NV_AUX_CTRL(channel));
            if (!(now & NV_AUX_CTRL_TRANSACT)) { finished = true; break; }
            timer_udelay(1);
        }
        if (!finished) {
            kwarn("nv-dp", "an AUX transaction on channel %d never completed",
                  channel);
            aux_end(c, channel);
            return -1;
        }

        stat = nv_rd32(c, NV_AUX_STAT(channel));
        if (stat & NV_AUX_STAT_TIMEOUT) { aux_end(c, channel); return -3; }
        if (stat & NV_AUX_STAT_ERROR)   { aux_end(c, channel); return -4; }

        reply = (int)((stat & NV_AUX_STAT_REPLY_MASK) >> NV_AUX_STAT_REPLY_SHIFT);

        /* "Ask me again" - which a monitor is entitled to say repeatedly. */
        if (reply == NV_AUX_DEFER || reply == NV_AUX_I2C_DEFER) {
            deferred++;
            continue;
        }
        break;
    }

    if (!writing) {
        u32 in[4];
        for (int i = 0; i < 4; i++)
            in[i] = nv_rd32(c, NV_AUX_DATA_READ(channel, i * 4));

        /* However many the monitor says it sent, not however many were asked
         * for.  The two are allowed to differ. */
        u8 got = (u8)(stat & NV_AUX_STAT_COUNT_MASK);
        if (got > *size) got = *size;
        for (int i = 0; i < got; i++)
            data[i] = (u8)(in[i / 4] >> (8 * (i % 4)));
        *size = got;
    }

    aux_end(c, channel);
    (void)deferred;
    return reply;
}

/* ---------------------------------------------------------- the monitor's
 *                                                             own registers */

bool nv_dpcd_read(nv_card_t *c, nv_dp_link_t *l, u32 address, u8 *out, int len) {
    while (len > 0) {
        u8 chunk = (u8)(len > 16 ? 16 : len);
        int reply = nv_aux_transfer(c, l->channel, NV_AUX_NATIVE_READ, address,
                                    out, &chunk);
        if (reply != NV_AUX_ACK) return false;

        /* However many came back, which may be fewer than were asked for - and
         * if it is none, saying so beats looping forever asking again. */
        if (chunk == 0) return false;

        address += chunk;
        out += chunk;
        len -= chunk;
    }
    return true;
}

bool nv_dpcd_write(nv_card_t *c, nv_dp_link_t *l, u32 address, const u8 *in,
                   int len) {
    u8 scratch[16];
    while (len > 0) {
        u8 chunk = (u8)(len > 16 ? 16 : len);
        memcpy(scratch, in, chunk);
        int reply = nv_aux_transfer(c, l->channel, NV_AUX_NATIVE_WRITE, address,
                                    scratch, &chunk);
        if (reply != NV_AUX_ACK) return false;
        address += chunk;
        in += chunk;
        len -= chunk;
    }
    return true;
}

/* -------------------------------------------------------- what it can do */

u32 nv_dp_rate_khz(u8 rate) {
    switch (rate) {
    case DP_RATE_1_62: return 1620000;
    case DP_RATE_2_70: return 2700000;
    case DP_RATE_5_40: return 5400000;
    case DP_RATE_8_10: return 8100000;
    default:           return 0;
    }
}

bool nv_dp_read_caps(nv_card_t *c, nv_dp_link_t *l) {
    u8 dpcd[16];
    if (!nv_dpcd_read(c, l, DPCD_REV, dpcd, sizeof dpcd)) {
        kwarn("nv-dp", "channel %d did not answer; nothing is plugged in or it "
                       "is not DisplayPort", l->channel);
        return false;
    }

    l->rev = dpcd[DPCD_REV];
    l->max_rate = dpcd[DPCD_MAX_LINK_RATE];
    l->max_lanes = dpcd[DPCD_MAX_LANE_COUNT] & 0x1F;
    l->enhanced_framing = (dpcd[DPCD_MAX_LANE_COUNT] & DPCD_ENHANCED_FRAME_CAP) != 0;
    l->tps3 = (dpcd[DPCD_MAX_LANE_COUNT] & DPCD_TPS3_SUPPORTED) != 0;

    /* How long to wait between asking the monitor how it is getting on.  Zero
     * means the specification's default of 100 microseconds; anything else is
     * in units of four milliseconds. */
    u8 interval = dpcd[DPCD_TRAINING_AUX_RD_INTERVAL] & 0x7F;
    l->aux_rd_interval_us = interval ? interval * 4000 : 100;

    if (!nv_dp_rate_khz(l->max_rate) || !l->max_lanes || l->max_lanes > 4) {
        kwarn("nv-dp", "channel %d claims rate %02x and %u lanes, which is not "
                       "a link", l->channel, l->max_rate, l->max_lanes);
        return false;
    }

    kinfo("nv-dp", "channel %d: DisplayPort %u.%u, up to %u lanes at %u.%u Gbps"
                   "%s%s",
          l->channel, l->rev >> 4, l->rev & 0xF, l->max_lanes,
          nv_dp_rate_khz(l->max_rate) / 1000000,
          (nv_dp_rate_khz(l->max_rate) / 100000) % 10,
          l->enhanced_framing ? ", enhanced framing" : "",
          l->tps3 ? ", pattern 3" : "");
    return true;
}

/* The identification block, over the emulated I2C bus.  The address pointer is
 * set with a write that says the transaction continues, then the bytes are
 * read; ending the first transaction instead would reset the pointer. */
bool nv_dp_read_edid(nv_card_t *c, nv_dp_link_t *l, u8 out[128]) {
    u8 offset = 0;
    u8 one = 1;

    int reply = nv_aux_transfer(c, l->channel, NV_AUX_I2C_WRITE_MOT, 0x50,
                                &offset, &one);
    if (reply != NV_AUX_ACK) {
        kwarn("nv-dp", "the monitor on channel %d would not take the address",
              l->channel);
        return false;
    }

    int got = 0;
    while (got < 128) {
        u8 chunk = (u8)(128 - got > 16 ? 16 : 128 - got);
        bool last = (got + chunk) >= 128;
        reply = nv_aux_transfer(c, l->channel,
                                last ? NV_AUX_I2C_READ : NV_AUX_I2C_READ_MOT,
                                0x50, out + got, &chunk);
        if (reply != NV_AUX_ACK || chunk == 0) {
            kwarn("nv-dp", "the monitor stopped after %d bytes", got);
            return false;
        }
        got += chunk;
    }

    static const u8 header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (memcmp(out, header, 8)) {
        kwarn("nv-dp", "what came back is not an identification block");
        return false;
    }

    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (u8)(sum + out[i]);
    if (sum) {
        kwarn("nv-dp", "the identification block does not check out");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- the card's
 *                                                                side */

static void source_link(nv_card_t *c, nv_dp_link_t *l, u8 rate, u8 lanes,
                        bool enhanced) {
    u32 soff = (u32)l->sor * NV_PDISP_SOR_STRIDE;
    u32 loff = soff + (u32)l->link * NV_PDISP_SOR_LINK_STRIDE;

    /* What the serialiser clocks at. */
    nv_mask32(c, NV_PDISP_SOR_CLK_CNTL + soff, 0x007C0000u, (u32)rate << 18);

    /* Which lanes are switched on, and whether the framing is the one that
     * wastes fewer symbols. */
    u32 dpctrl = ((1u << lanes) - 1u) << 16;
    if (enhanced) dpctrl |= 0x00004000u;
    nv_mask32(c, NV_PDISP_SOR_DP_LINKCTL + loff, 0x401F4000u, dpctrl);
}

static void source_pattern(nv_card_t *c, nv_dp_link_t *l, int pattern) {
    u32 soff = (u32)l->sor * NV_PDISP_SOR_STRIDE;
    nv_mask32(c, NV_PDISP_SOR_DP_PADCTL + soff, 0x0F0F0F0Fu,
              0x01010101u * (u32)pattern);
}

static void source_drive(nv_card_t *c, nv_dp_link_t *l, int lane, u8 swing,
                         u8 preemphasis) {
    u32 loff = (u32)l->sor * NV_PDISP_SOR_STRIDE +
               (u32)l->link * NV_PDISP_SOR_LINK_STRIDE;
    int shift = lane * 8;

    nv_mask32(c, NV_PDISP_SOR_DP_DRIVE + loff, 0xFFu << shift,
              (u32)swing << shift);
    nv_mask32(c, NV_PDISP_SOR_DP_PREEMPH + loff, 0xFFu << shift,
              (u32)preemphasis << shift);
}

/* ------------------------------------------------------------------ training
 *
 * Two phases, and they are not interchangeable.  The first gets each lane's
 * clock recovered; the second gets the symbols readable.  Losing clock
 * recovery during the second phase means the settings that got there are no
 * longer working, and the right answer is to start again slower rather than to
 * keep adjusting.
 */

/* Push the current swing and pre-emphasis to both ends. */
static bool set_drive(nv_card_t *c, nv_dp_link_t *l) {
    u8 lanes[4];

    for (int i = 0; i < l->lanes; i++) {
        source_drive(c, l, i, l->swing[i], l->preemphasis[i]);

        lanes[i] = (u8)(l->swing[i] | (l->preemphasis[i] << 3));
        /* The monitor has to be told when a lane has nothing left to give, or
         * it will keep asking for more and the loop below will never end. */
        if (l->swing[i] >= 3) lanes[i] |= DPCD_MAX_SWING_REACHED;
        if (l->preemphasis[i] >= 3) lanes[i] |= DPCD_MAX_PRE_EMPHASIS_REACHED;
    }

    return nv_dpcd_write(c, l, DPCD_TRAINING_LANE0_SET, lanes, l->lanes);
}

/* Take what the monitor asked for.  Returns true if anything changed. */
static bool take_adjustment(nv_dp_link_t *l, const u8 *adjust) {
    bool changed = false;

    for (int i = 0; i < l->lanes; i++) {
        u8 byte = adjust[i / 2];
        u8 swing = (u8)((byte >> ((i & 1) * 4)) & 0x3);
        u8 pre = (u8)((byte >> (((i & 1) * 4) + 2)) & 0x3);

        /* The two are not independent: the sum of the two levels cannot exceed
         * three, and a monitor asking for more than that is asking for
         * something the transmitter cannot produce. */
        if (swing + pre > 3) pre = (u8)(3 - swing);

        if (swing != l->swing[i] || pre != l->preemphasis[i]) changed = true;
        l->swing[i] = swing;
        l->preemphasis[i] = pre;
    }
    return changed;
}

static bool clock_recovered(const nv_dp_link_t *l, const u8 *status) {
    for (int i = 0; i < l->lanes; i++) {
        u8 lane = (u8)((status[i / 2] >> ((i & 1) * 4)) & 0xF);
        if (!(lane & DPCD_LANE_CR_DONE)) return false;
    }
    return true;
}

static bool channel_equalised(const nv_dp_link_t *l, const u8 *status) {
    if (!(status[2] & DPCD_INTERLANE_ALIGN_DONE)) return false;

    for (int i = 0; i < l->lanes; i++) {
        u8 lane = (u8)((status[i / 2] >> ((i & 1) * 4)) & 0xF);
        u8 want = DPCD_LANE_CR_DONE | DPCD_LANE_EQ_DONE | DPCD_LANE_SYMBOL_LOCKED;
        if ((lane & want) != want) return false;
    }
    return true;
}

static bool at_maximum_swing(const nv_dp_link_t *l) {
    for (int i = 0; i < l->lanes; i++)
        if (l->swing[i] < 3) return false;
    return true;
}

static void set_pattern(nv_card_t *c, nv_dp_link_t *l, int pattern) {
    source_pattern(c, l, pattern);

    u8 value = (u8)pattern;
    /* Scrambling is off during training and back on when it ends, and the two
     * ends have to agree about when. */
    if (pattern) value |= DPCD_SCRAMBLING_DISABLE;
    nv_dpcd_write(c, l, DPCD_TRAINING_PATTERN_SET, &value, 1);
}

/* One attempt at one rate and lane count. */
static bool train_once(nv_card_t *c, nv_dp_link_t *l) {
    l->attempts++;

    for (int i = 0; i < 4; i++) { l->swing[i] = 0; l->preemphasis[i] = 0; }

    source_link(c, l, l->rate, l->lanes, l->enhanced_framing);

    u8 bw = l->rate;
    u8 count = (u8)(l->lanes | (l->enhanced_framing ? 0x80 : 0));
    if (!nv_dpcd_write(c, l, DPCD_LINK_BW_SET, &bw, 1)) return false;
    if (!nv_dpcd_write(c, l, DPCD_LANE_COUNT_SET, &count, 1)) return false;

    /* ---- clock recovery ---- */
    set_pattern(c, l, 1);
    if (!set_drive(c, l)) return false;

    int same_swing = 0;
    u8 previous_swing = l->swing[0];
    bool recovered = false;

    for (int loop = 0; loop < 10; loop++) {
        l->cr_loops++;
        timer_udelay(l->aux_rd_interval_us);

        u8 status[3];
        if (!nv_dpcd_read(c, l, DPCD_LANE0_1_STATUS, status, 3)) return false;

        if (clock_recovered(l, status)) { recovered = true; break; }

        /* Both escape conditions.  Either one alone leaves a loop that either
         * gives up far too slowly or never gives up at all. */
        if (at_maximum_swing(l)) break;
        if (l->swing[0] == previous_swing) {
            if (++same_swing >= 5) break;
        } else {
            same_swing = 0;
            previous_swing = l->swing[0];
        }

        u8 adjust[2];
        if (!nv_dpcd_read(c, l, DPCD_ADJUST_REQUEST_LANE0_1, adjust, 2)) return false;
        take_adjustment(l, adjust);
        if (!set_drive(c, l)) return false;
    }

    if (!recovered) {
        set_pattern(c, l, 0);
        return false;
    }

    /* ---- channel equalisation ---- */
    set_pattern(c, l, l->tps3 ? 3 : 2);

    for (int loop = 0; loop < 5; loop++) {
        l->eq_loops++;
        timer_udelay(l->aux_rd_interval_us);

        u8 status[3];
        if (!nv_dpcd_read(c, l, DPCD_LANE0_1_STATUS, status, 3)) return false;

        /* Losing clock recovery here means the settings that got this far have
         * stopped working, and no further adjustment will bring them back. */
        if (!clock_recovered(l, status)) break;

        if (channel_equalised(l, status)) {
            /* Training off, scrambling back on, and the link is live. */
            set_pattern(c, l, 0);
            l->trained = true;
            return true;
        }

        u8 adjust[2];
        if (!nv_dpcd_read(c, l, DPCD_ADJUST_REQUEST_LANE0_1, adjust, 2)) return false;
        take_adjustment(l, adjust);
        if (!set_drive(c, l)) return false;
    }

    set_pattern(c, l, 0);
    return false;
}

bool nv_dp_train(nv_card_t *c, nv_dp_link_t *l, u32 pixel_khz,
                 int bits_per_pixel) {
    static const u8 rates[] = { DP_RATE_8_10, DP_RATE_5_40, DP_RATE_2_70,
                                DP_RATE_1_62 };

    l->trained = false;
    l->attempts = l->cr_loops = l->eq_loops = l->fallbacks = 0;

    /* Wake it up.  A monitor in its low-power state answers AUX and does
     * nothing else, and training it there fails in a way that looks like a bad
     * cable. */
    u8 power = 1;
    nv_dpcd_write(c, l, DPCD_SET_POWER, &power, 1);
    timer_udelay(2000);

    /* What the picture needs, in kilobits per second, against what a
     * combination carries: eight tenths of the serial rate, because the data is
     * 8b/10b coded, times the number of lanes. */
    u64 needed = (u64)pixel_khz * (u32)bits_per_pixel;

    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        if (nv_dp_rate_khz(rates[r]) > nv_dp_rate_khz(l->max_rate)) continue;

        for (int lanes = l->max_lanes; lanes >= 1; lanes >>= 1) {
            u64 carries = (u64)nv_dp_rate_khz(rates[r]) * (u32)lanes * 8 / 10;
            if (carries < needed) continue;

            l->rate = rates[r];
            l->lanes = (u8)lanes;

            if (train_once(c, l)) {
                kinfo("nv-dp", "channel %d trained: %u lanes at %u.%u Gbps, "
                               "swing %u pre-emphasis %u, %d attempt(s)",
                      l->channel, l->lanes,
                      nv_dp_rate_khz(l->rate) / 1000000,
                      (nv_dp_rate_khz(l->rate) / 100000) % 10,
                      l->swing[0], l->preemphasis[0], l->attempts);
                return true;
            }

            l->fallbacks++;
            kinfo("nv-dp", "channel %d would not train at %u lanes and %u.%u "
                           "Gbps; trying slower", l->channel, l->lanes,
                  nv_dp_rate_khz(l->rate) / 1000000,
                  (nv_dp_rate_khz(l->rate) / 100000) % 10);
        }
    }

    kwarn("nv-dp", "channel %d would not train at any rate this monitor and "
                   "this mode have in common", l->channel);
    return false;
}
