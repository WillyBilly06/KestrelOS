/* ath9k_model.c - a model of the hardware, so the driver is run rather than
 * merely compiled.
 *
 * No virtual machine emulates a Wi-Fi card: VMware, QEMU and VirtualBox all
 * present wired Ethernet to the guest whatever the host is connected to.  That
 * leaves a driver in an unhappy position - written, plausible, and never once
 * executed.  So this answers the registers the driver writes and walks the
 * descriptor chains it builds, and the driver runs against it unchanged.
 *
 * Be precise about what this establishes, because it is easy to overstate.
 *
 * It does exercise: the reset sequence, the EEPROM read, the descriptor chain
 * construction and its circular linking, the DMA addresses handed over, the
 * transmit path from a frame down to a queue's enable bit, the receive path
 * from a filled descriptor up into the 802.11 layer, the ownership handshake in
 * the status words, ring wrap-around, and the whole stack above the driver
 * running over a device that behaves like a radio.
 *
 * It does not establish: that the register offsets in ath9k.h match real
 * silicon, or that the synthesiser sequence tunes a real radio.  Those two are
 * from documentation and remain unverified.  A model written from the same
 * understanding as the driver cannot check that understanding against the
 * world - only against itself.
 *
 * What it is worth: every class of bug that lives in ring management, DMA
 * addressing, descriptor formats and state machines - which is where driver
 * bugs actually live - is reachable here.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "wifi.h"
#include "ath9k.h"
#include "crypto.h"

/* The model behaves like one access point beaconing a WPA2 network, so the
 * whole join can be driven through the real driver. */
#define MODEL_SSID       "kestrel-test"
#define MODEL_PASSPHRASE "kestreltest"
#define MODEL_CHANNEL    6

/* The register file, in ordinary memory.  The driver's reads and writes land
 * here instead of on a bus. */
#define REG_SPAN 0x10000

typedef struct {
    u8   regs[REG_SPAN];

    bool present;
    bool rx_enabled;
    bool phy_active;
    u32  rx_descriptor;        /* where the receive chain starts   */
    u32  rx_current;           /* which descriptor is next to fill */

    mac_t station;             /* what the driver programmed       */
    mac_t bssid;
    u8    channel;

    /* The access point's side of things, including its half of the key
     * exchange - so a join driven through the real driver goes all the way to
     * an agreed key rather than stopping at association. */
    mac_t ap;
    u64   last_beacon_ms;
    bool  authenticated;
    bool  associated;
    bool  have_pmk;
    u8    pmk[32];
    u8    anonce[32];
    u8    snonce[32];
    u8    ptk[48];

    /* Frames the model wants to deliver, queued so a reply is never built
     * inside the receive path that provoked it. */
    u8    pending[4][512];
    int   pending_len[4];
    int   pending_count;
} ath_model_t;

static ath_model_t model;

/* ------------------------------------------------------------- registers */

static inline u32 reg_get(u32 off) {
    return *(u32 *)(model.regs + off);
}
static inline void reg_set(u32 off, u32 v) {
    *(u32 *)(model.regs + off) = v;
}

/* Reading a physical address the driver handed over.  The model runs in the
 * same address space, so the direct map turns it back into a pointer - which
 * is exactly what a card's bus master does, by different means. */
static void *phys(u32 address) {
    return phys_to_virt((u64)address);
}

/* --------------------------------------------------------------- EEPROM */

/* The words the driver asks for.  Only the address is modelled; the
 * calibration data is not, because nothing here uses it. */
static u16 eeprom_word(u32 address) {
    static const u8 mac[ETH_ALEN] = { 0x00, 0x03, 0x7F, 0x11, 0x22, 0x33 };

    if (address >= AR_EEPROM_MAC_WORD && address < AR_EEPROM_MAC_WORD + 3) {
        int i = (int)(address - AR_EEPROM_MAC_WORD);
        return (u16)((mac[i * 2] << 8) | mac[i * 2 + 1]);
    }
    return 0;
}

/* ------------------------------------------------------- 802.11 the model
 * sends
 *
 * Enough of an access point to complete a join: a beacon, an authentication
 * reply and an association reply.  The key exchange is the test adapter's job
 * and is not repeated here; what this file is for is the descriptor path.
 */

static inline void put16le(void *p, u16 v) {
    u8 *b = p;
    b[0] = (u8)v; b[1] = (u8)(v >> 8);
}
static inline u16 get16le(const void *p) {
    const u8 *b = p;
    return (u16)(b[0] | (b[1] << 8));
}

static void queue_frame(const u8 *frame, int len) {
    if (model.pending_count >= 4 || len > 512) return;
    memcpy(model.pending[model.pending_count], frame, (size_t)len);
    model.pending_len[model.pending_count] = len;
    model.pending_count++;
}

static void build_beacon(void) {
    u8 frame[128];
    memset(frame, 0, sizeof frame);

    put16le(frame, (u16)(0 << 2 | 8 << 4));
    memset(frame + 4, 0xFF, ETH_ALEN);
    memcpy(frame + 10, model.ap.addr, ETH_ALEN);
    memcpy(frame + 16, model.ap.addr, ETH_ALEN);

    u8 *p = frame + 24;
    memset(p, 0, 8); p += 8;
    put16le(p, 100); p += 2;
    put16le(p, 0x0011); p += 2;             /* privacy is on */

    *p++ = 0;
    *p++ = (u8)strlen(MODEL_SSID);
    memcpy(p, MODEL_SSID, strlen(MODEL_SSID));
    p += strlen(MODEL_SSID);

    *p++ = 3; *p++ = 1; *p++ = MODEL_CHANNEL;

    static const u8 rsn[] = {
        48, 20, 0x01, 0x00,
        0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
        0x00, 0x00,
    };
    memcpy(p, rsn, sizeof rsn);
    p += sizeof rsn;

    queue_frame(frame, (int)(p - frame));
}

/* Message one of the four-way exchange: the access point's random number,
 * carried in a data frame because there is no key yet to protect it with. */
static void build_message_one(void) {
    u8 frame[160];
    memset(frame, 0, sizeof frame);

    put16le(frame, (u16)(2 << 2 | 0 << 4 | 0x0200));    /* data, from the AP */
    memcpy(frame + 4, model.station.addr, ETH_ALEN);
    memcpy(frame + 10, model.ap.addr, ETH_ALEN);
    memcpy(frame + 16, model.ap.addr, ETH_ALEN);

    u8 *p = frame + 24;
    p[0] = 0xAA; p[1] = 0xAA; p[2] = 0x03;
    p[3] = 0x00; p[4] = 0x00; p[5] = 0x00;
    p[6] = 0x88; p[7] = 0x8E;                            /* EAPOL */
    p += 8;

    p[0] = 2; p[1] = 3;                                  /* version, key */
    p[2] = 0; p[3] = 95;

    u8 *body = p + 4;
    memset(body, 0, 95);
    body[0] = 2;                                         /* RSN descriptor */
    body[1] = 0x00; body[2] = 0x8A;    /* pairwise, ack, key version two */
    body[3] = 0; body[4] = 16;                           /* CCMP key length */
    body[12] = 1;                                        /* a replay counter */
    memcpy(body + 13, model.anonce, 32);

    queue_frame(frame, (int)(body + 95 - frame));
}

/* What the driver sent, seen from the other side. */
static void frame_from_driver(const u8 *frame, int len) {
    if (len < 24) return;

    u16 fc = get16le(frame);
    u8 type = (u8)((fc >> 2) & 3), subtype = (u8)((fc >> 4) & 0xF);

    if (type == 0 && subtype == 4) {            /* a probe request */
        build_beacon();
        return;
    }

    if (type == 0 && subtype == 11) {           /* authentication */
        u8 reply[32];
        memset(reply, 0, sizeof reply);
        put16le(reply, (u16)(0 << 2 | 11 << 4));
        memcpy(reply + 4, model.station.addr, ETH_ALEN);
        memcpy(reply + 10, model.ap.addr, ETH_ALEN);
        memcpy(reply + 16, model.ap.addr, ETH_ALEN);
        put16le(reply + 24, 0);
        put16le(reply + 26, 2);
        put16le(reply + 28, 0);
        queue_frame(reply, 30);
        model.authenticated = true;
        return;
    }

    if (type == 0 && subtype == 0) {            /* association request */
        u8 reply[32];
        memset(reply, 0, sizeof reply);
        put16le(reply, (u16)(0 << 2 | 1 << 4));
        memcpy(reply + 4, model.station.addr, ETH_ALEN);
        memcpy(reply + 10, model.ap.addr, ETH_ALEN);
        memcpy(reply + 16, model.ap.addr, ETH_ALEN);
        put16le(reply + 24, 0x0011);
        put16le(reply + 26, 0);
        put16le(reply + 28, 1);
        queue_frame(reply, 30);
        model.associated = true;

        /* Association done, so the key exchange begins: the access point sends
         * its random number and waits for the station's. */
        build_message_one();
        return;
    }

    if (type == 0 && subtype == 12) {           /* the station left */
        model.authenticated = model.associated = false;
        model.pending_count = 0;
        return;
    }

    /* A data frame carrying the key exchange.  The access point derives the
     * same session key from its own side and checks the station's proof, which
     * is the only way to know both ends agree. */
    if (type == 2 && len > 24 + 8 + 99) {
        const u8 *snap = frame + 24;
        if (snap[6] != 0x88 || snap[7] != 0x8E) return;

        const u8 *eapol = snap + 8;
        const u8 *body = eapol + 4;
        u16 key_info = (u16)((body[1] << 8) | body[2]);

        /* Message two carries a proof and does not ask for one back. */
        if (!(key_info & 0x0100) || (key_info & 0x0080)) return;

        memcpy(model.snonce, body + 13, 32);
        wpa_derive_ptk(model.pmk, &model.station, &model.ap,
                       model.snonce, model.anonce, model.ptk);

        u16 key_data_len = (u16)((body[93] << 8) | body[94]);
        int total = 4 + 95 + key_data_len;
        if (total > len - 24 - 8) return;

        static u8 copy[512];
        if (total > (int)sizeof copy) return;
        memcpy(copy, eapol, (size_t)total);
        memset(copy + 4 + 77, 0, 16);

        u8 expected[SHA1_SIZE];
        hmac_sha1(model.ptk, 16, copy, (size_t)total, expected);

        if (memcmp(expected, body + 77, 16) != 0) {
            kwarn("ath9k-model", "the station's proof did not verify - it does "
                                 "not have the right passphrase");
            return;
        }

        kinfo("ath9k-model", "the station's proof verified; both sides derived "
                             "the same key");

        /* Message three: the access point's own proof and the instruction to
         * install the key. */
        u8 reply[192];
        memset(reply, 0, sizeof reply);
        put16le(reply, (u16)(2 << 2 | 0 << 4 | 0x0200));
        memcpy(reply + 4, model.station.addr, ETH_ALEN);
        memcpy(reply + 10, model.ap.addr, ETH_ALEN);
        memcpy(reply + 16, model.ap.addr, ETH_ALEN);

        u8 *p = reply + 24;
        p[0] = 0xAA; p[1] = 0xAA; p[2] = 0x03;
        p[3] = 0x00; p[4] = 0x00; p[5] = 0x00;
        p[6] = 0x88; p[7] = 0x8E;
        p += 8;

        u8 *out_eapol = p;
        out_eapol[0] = 2; out_eapol[1] = 3;
        out_eapol[2] = 0; out_eapol[3] = 95;

        u8 *out_body = out_eapol + 4;
        memset(out_body, 0, 95);
        out_body[0] = 2;
        u16 info = 0x0002 | 0x0008 | 0x0040 | 0x0080 | 0x0100 | 0x0200;
        out_body[1] = (u8)(info >> 8);
        out_body[2] = (u8)info;
        out_body[3] = 0; out_body[4] = 16;
        out_body[12] = 2;
        memcpy(out_body + 13, model.anonce, 32);

        u8 mic[SHA1_SIZE];
        hmac_sha1(model.ptk, 16, out_eapol, (size_t)(4 + 95), mic);
        memcpy(out_body + 77, mic, 16);

        queue_frame(reply, (int)(out_body + 95 - reply));
    }
}

/* ------------------------------------------------------- the transmit path */

/* The driver has pointed a queue at a descriptor and set its enable bit; walk
 * the chain the way the hardware would, taking each frame and marking the
 * descriptor done. */
static void run_transmit_queue(int queue) {
    u32 address = reg_get(AR_QTXDP(queue));
    if (!address) return;

    for (int guard = 0; guard < 16; guard++) {
        ath_desc_t *d = phys(address);

        /* A descriptor whose done bit is already set is one the driver has not
         * handed over; the hardware would stop there. */
        if (d->status0 & ATH_TXS0_DONE) break;

        int len = (int)(d->control0 & ATH_TXC0_LENGTH);
        if (len > 0 && len <= 2048 && d->buffer) {
            const u8 *frame = phys(d->buffer);
            frame_from_driver(frame, len);
        }

        d->status0 = ATH_TXS0_DONE | ATH_TXS0_OK;
        d->status1 = 0;

        /* One frame per enable, which is what the driver does. */
        break;
    }

    reg_set(AR_Q_TXE, 0);
}

/* -------------------------------------------------------- the receive path */

/* Put one frame into the next descriptor the driver gave us, exactly as the
 * hardware would: fill the buffer, write the length and status, and move on
 * along the chain. */
static bool deliver(const u8 *frame, int len) {
    if (!model.rx_enabled || !model.rx_current) return false;

    ath_desc_t *d = phys(model.rx_current);

    /* A descriptor whose done bit is set has not been given back yet. */
    if (d->status0 & ATH_RXS0_DONE) return false;

    int room = (int)(d->control1 & ATH_RXC1_BUF_LEN);
    if (len > room) len = room;
    if (!d->buffer) return false;

    memcpy(phys(d->buffer), frame, (size_t)len);

    d->status0 = ATH_RXS0_DONE | ATH_RXS0_OK |
                 (((u32)len << ATH_RXS0_LENGTH_S) & ATH_RXS0_LENGTH);
    /* Signal strength above the noise floor: a strong nearby access point. */
    d->status1 = 53;

    model.rx_current = d->link;
    return true;
}

static void scan_for_writes(void);

/* Called by the driver before every register read.  On silicon a write acts at
 * once; here it acts the next time the driver looks, which is close enough that
 * no path can tell the difference - and it means a sequence like the EEPROM
 * read, which never polls, still works. */
void ath_model_sync(void) {
    if (!model.present) return;
    scan_for_writes();
}

/* Called from the driver's poll, before it looks at the descriptors: this is
 * where the model does whatever the passage of time would have caused. */
void ath_model_tick(void) {
    if (!model.present) return;
    scan_for_writes();

    /* Beacon about ten times a second, as an access point does - and only
     * while the driver is listening on the right channel, so that a scan
     * finding it on channel six is a real result. */
    if (model.phy_active && model.channel == MODEL_CHANNEL &&
        g_uptime_ms - model.last_beacon_ms >= 100) {
        model.last_beacon_ms = g_uptime_ms;
        build_beacon();
    }

    while (model.pending_count > 0) {
        if (!deliver(model.pending[0], model.pending_len[0])) break;

        /* Shuffle the rest down; there are never more than a few. */
        for (int i = 1; i < model.pending_count; i++) {
            memcpy(model.pending[i - 1], model.pending[i],
                   (size_t)model.pending_len[i]);
            model.pending_len[i - 1] = model.pending_len[i];
        }
        model.pending_count--;
    }
}

/* ------------------------------------------------- the register interface */

/* What a write to each register means.  Anything not named here is stored and
 * read back, which is what an unimplemented register does anyway. */
static void on_write(u32 off, u32 value) {
    switch (off) {
    case AR_RC:
        if (value == 0) {
            /* Out of reset: the revision becomes readable and the state is
             * whatever a freshly reset card has. */
            reg_set(AR_SREV, (0x00Bu << AR_SREV_VERSION_S) | 0x2);
            model.rx_enabled = false;
            model.phy_active = false;
            model.authenticated = model.associated = false;
            model.pending_count = 0;
        }
        break;

    case AR_EEPROM_CMD:
        if (value & AR_EEPROM_CMD_READ) {
            u32 address = reg_get(AR_EEPROM_ADDR);
            reg_set(AR_EEPROM_DATA, eeprom_word(address));
            reg_set(AR_EEPROM_STATUS, AR_EEPROM_STATUS_DONE);
        }
        break;

    case AR_CR:
        if (value & AR_CR_RXE) {
            model.rx_enabled = true;
            if (!model.rx_current) model.rx_current = model.rx_descriptor;
        }
        if (value & AR_CR_RXD) model.rx_enabled = false;
        break;

    case AR_RXDP:
        model.rx_descriptor = value;
        model.rx_current = value;
        break;

    case AR_PHY_ACTIVE:
        model.phy_active = (value & AR_PHY_ACTIVE_EN) != 0;
        break;

    case AR_PHY_RFBUS_REQ:
        /* Grant the radio bus straight away; a real card takes a moment. */
        reg_set(AR_PHY_RFBUS_GRANT, value ? 1u : 0u);
        break;

    case AR_PHY_SYNTH_CONTROL: {
        /* Undo the encoding the driver applied, so the model knows which
         * channel it is on - which is what makes a scan meaningful. */
        u32 synth = (value >> 17) & 0x7FF;
        u32 mhz = 2300u + (synth * 25u) / 10u;
        model.channel = (mhz >= 2412 && mhz <= 2484)
                      ? (u8)((mhz == 2484) ? 14 : ((mhz - 2412) / 5 + 1)) : 0;
        break;
    }

    case AR_STA_ID0:
        model.station.addr[0] = (u8)value;
        model.station.addr[1] = (u8)(value >> 8);
        model.station.addr[2] = (u8)(value >> 16);
        model.station.addr[3] = (u8)(value >> 24);
        break;
    case AR_STA_ID1:
        model.station.addr[4] = (u8)value;
        model.station.addr[5] = (u8)(value >> 8);
        break;

    case AR_BSS_ID0:
        model.bssid.addr[0] = (u8)value;
        model.bssid.addr[1] = (u8)(value >> 8);
        model.bssid.addr[2] = (u8)(value >> 16);
        model.bssid.addr[3] = (u8)(value >> 24);
        break;
    case AR_BSS_ID1:
        model.bssid.addr[4] = (u8)value;
        model.bssid.addr[5] = (u8)(value >> 8);
        break;

    case AR_Q_TXE:
        for (int q = 0; q < 10; q++)
            if (value & (1u << q)) run_transmit_queue(q);
        break;

    default:
        break;
    }
}

/* The driver reaches the model through ordinary memory, so its writes have to
 * be noticed.  Rather than trapping every store - which would mean unmapping
 * the page and taking a fault per access - the model watches the registers it
 * cares about each time the driver polls, plus at the two moments a write must
 * take effect immediately.
 *
 * That is a real difference from hardware and worth naming: on silicon a write
 * acts at once, and here it acts at the next poll.  Every path the driver takes
 * tolerates that, because a real card's response is asynchronous anyway. */
static u32 watched[] = {
    AR_RC, AR_EEPROM_CMD, AR_CR, AR_RXDP, AR_PHY_ACTIVE,
    AR_PHY_RFBUS_REQ, AR_PHY_SYNTH_CONTROL,
    AR_STA_ID0, AR_STA_ID1, AR_BSS_ID0, AR_BSS_ID1, AR_Q_TXE,
};
static u32 previous[ARRAY_LEN(watched)];

/* Registers that mean "do this now" rather than "remember this".  Real
 * hardware clears them once it has acted, and so does this - which is also what
 * makes the same command written twice in a row register as two commands
 * rather than one. */
static bool is_command_register(u32 off) {
    return off == AR_EEPROM_CMD || off == AR_Q_TXE || off == AR_CR;
}

static void scan_for_writes(void) {
    for (size_t i = 0; i < ARRAY_LEN(watched); i++) {
        u32 now = reg_get(watched[i]);
        if (now == previous[i]) continue;

        on_write(watched[i], now);

        if (is_command_register(watched[i]) && now != 0) {
            reg_set(watched[i], 0);
            previous[i] = 0;
        } else {
            previous[i] = now;
        }
    }
}

/* ---------------------------------------------------------------- attach */

/* What the model's network is called, so the self-test does not have to repeat
 * the constants. */
const char *ath_model_ssid(void)       { return MODEL_SSID; }
const char *ath_model_passphrase(void) { return MODEL_PASSPHRASE; }
u8          ath_model_channel(void)    { return MODEL_CHANNEL; }

bool ath_model_attach(u32 *span, volatile u8 **regs_out) {
    memset(&model, 0, sizeof model);

    static const u8 ap[ETH_ALEN] = { 0x02, 0x41, 0x54, 0x48, 0x00, 0xAA };
    memcpy(model.ap.addr, ap, ETH_ALEN);
    model.present = true;

    for (int i = 0; i < 32; i++) model.anonce[i] = (u8)(0xA0 + i);

    /* The access point knows the passphrase, so it derives the same shared key
     * the station will - which is what makes the check meaningful. */
    pbkdf2_sha1(MODEL_PASSPHRASE, (const u8 *)MODEL_SSID, strlen(MODEL_SSID),
                4096, model.pmk, sizeof model.pmk);
    model.have_pmk = true;

    /* Answer the revision before the driver has reset anything, so a read
     * during probe does not look like a dead card. */
    reg_set(AR_SREV, (0x00Bu << AR_SREV_VERSION_S) | 0x2);

    for (size_t i = 0; i < ARRAY_LEN(watched); i++)
        previous[i] = reg_get(watched[i]);

    if (span) *span = REG_SPAN;
    if (regs_out) *regs_out = model.regs;

    kinfo("ath9k", "no Atheros card present; a hardware model is standing in "
                   "so the driver runs");
    return true;
}

