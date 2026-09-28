/* rtw.c - Realtek wireless.
 *
 * A Realtek Wi-Fi chip has a small processor on it - an 8051 in the older
 * parts, something larger in the newer ones - and it does nothing until the
 * driver has pushed the vendor's firmware into it.  The handover is unlike
 * Intel's: instead of writing into the device's memory through an address
 * register, the driver writes whole pages into a window at a fixed address and
 * tells the chip which page each one is, and instead of a ring of commands
 * there are four mailboxes of eight bytes each.
 *
 * That mailbox interface is the reason this family is worth writing out.  It
 * is small, it has not changed in a decade, and everything the driver needs to
 * do - set the channel, join a network, install a key, send a frame - goes
 * through it.
 *
 * The firmware is Realtek's and is not redistributable, which is why no
 * operating system ships it.  What an operating system provides is this code
 * and somewhere to look for the file.
 *
 * ---------------------------------------------------------------------------
 * On testing.  The firmware header, the paged download, the mailbox protocol
 * and the receive path are exercised on every run of the self-test, against a
 * model of the device that plays the part of the firmware.  The driver code
 * that runs against it is the same code that would run against a card.
 *
 * What that cannot establish is that the register offsets match Realtek's
 * silicon.  Those come from documentation and are unverified.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "firmware.h"
#include "rtw89.h"
#include "rtw89_radio_tables.h"
#include "rtw89_radio_gain.h"
#include "rtw89_radio_power.h"
#include "wifi.h"
#include "net.h"
#include "rtw.h"

void ieee80211_attach(wifi_device_t *dev);
void ieee80211_receive(wifi_device_t *dev, u8 *frame, int len, s8 signal_dbm);

/* ------------------------------------------------------------------ state */

typedef struct {
    volatile u8  *regs;
    wifi_device_t dev;
    firmware_t    fw;
    bool          modelled;

    rtw_fw_header_t header;
    bool            header_read;
    bool            firmware_running;

    /* The BE generation tunes by writing the radio's own registers; the
     * earlier ones ask their firmware to do it.  Two different things through
     * one entry point, so which one this card is has to be known. */
    bool            is_be;

    /* The BE generation's rings, once its firmware is running. */
    rtw89_channel_t mailbox;
    rtw89_data_t    data;
    rtw89_rx_path_t rx;
    bool            mailbox_ready;
    bool            data_ready;
    bool            rx_ready;
    bool            calibrated;
    bool            board_data_valid;
    u8              board_rf[0x240];
    u8              board_phycap[0x38];
    bool            radio_resources_valid;
    rtw89_radio_resources_t radio_resources; /* Views into retained c->fw. */
    rtw89_radio_gain_t radio_gain;
    /* Firmware CH12 DMA backing belongs to this card for its whole lifetime.
     * A timed-out card may still read it; another card must never reuse it. */
    u8             *fwdl_ring_memory;
    u8             *fwdl_packet_memory;
    u64             fwdl_ring_phys;
    u64             fwdl_packet_phys;
    rtw89_ring_t    fwdl_ring;
    bool            fwdl_ring_attached;
    bool            fwdl_ring_poisoned;

    /* Which mailbox the next command goes in.  They are used in turn so that
     * a command can be sent while the previous one is still being read. */
    u8            next_box;
    u8            sequence;
} rtw_t;

#define MAX_CARDS 2
static rtw_t cards[MAX_CARDS];
static int   card_count;

void rtw_model_sync(void);

/* --------------------------------------------------------------- registers */

static inline u8 rd8(rtw_t *c, u32 off) {
    if (c->modelled) rtw_model_sync();
    return c->regs ? *(volatile u8 *)(c->regs + off) : 0xFF;
}
static inline u16 rd16(rtw_t *c, u32 off) {
    if (c->modelled) rtw_model_sync();
    return c->regs ? *(volatile u16 *)(c->regs + off) : 0xFFFF;
}
static inline u32 rd32(rtw_t *c, u32 off) {
    if (c->modelled) rtw_model_sync();
    return c->regs ? *(volatile u32 *)(c->regs + off) : 0xFFFFFFFFu;
}
static inline void wr8(rtw_t *c, u32 off, u8 v) {
    if (c->regs) *(volatile u8 *)(c->regs + off) = v;
}
static inline void wr16(rtw_t *c, u32 off, u16 v) {
    if (c->regs) *(volatile u16 *)(c->regs + off) = v;
}
static inline void wr32(rtw_t *c, u32 off, u32 v) {
    if (c->regs) *(volatile u32 *)(c->regs + off) = v;
}

/* ------------------------------------------------------------ the firmware */

static inline u16 le16(const void *p) {
    const u8 *b = p;
    return (u16)((u16)b[0] | ((u16)b[1] << 8));
}
static inline u32 le32(const void *p) {
    const u8 *b = p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

/* A Realtek firmware file is a fixed header followed by the code itself.  The
 * signature says which chip it is for, and a file for the wrong chip loads
 * perfectly happily and then does nothing - so it is checked. */
bool rtw_parse_firmware(const u8 *data, size_t size, rtw_fw_header_t *out) {
    memset(out, 0, sizeof *out);

    if (size < RTW_FW_HEADER_SIZE + 4) {
        kwarn("rtw", "the firmware file is too short to be one (%zu bytes)", size);
        return false;
    }

    out->signature = le16(data);
    out->category = data[2];
    out->function = data[3];
    out->version = le16(data + 4);
    out->subversion = data[6];
    out->subindex = data[7];
    out->month = data[16];
    out->date = data[17];
    out->hour = data[18];
    out->minute = data[19];
    out->header_size = le16(data + 20);
    out->body_size = le32(data + 24);

    /* The signature is the chip the file was built for.  Every one of these
     * has shipped in a card somebody might have. */
    switch (out->signature) {
    case RTW_SIG_8188E: case RTW_SIG_8192E: case RTW_SIG_8723B:
    case RTW_SIG_8812:  case RTW_SIG_8821:  case RTW_SIG_8822B:
    case RTW_SIG_8822C: case RTW_SIG_8821C: case RTW_SIG_8723D:
        break;
    default:
        kwarn("rtw", "signature %04x is not a chip this driver knows",
              out->signature);
        return false;
    }

    /* Older files put the code straight after a 32-byte header; newer ones
     * declare the header's own size, and it is 64. */
    u32 header = out->header_size ? out->header_size : RTW_FW_HEADER_SIZE;
    if (header != 32 && header != 64) header = RTW_FW_HEADER_SIZE;
    if (header >= size) {
        kwarn("rtw", "the header claims %u bytes and the file is %zu", header, size);
        return false;
    }

    out->body = data + header;
    out->body_size = (u32)(size - header);
    return true;
}

/* --------------------------------------------------------- the download */

/* The chip's own processor is held in reset while its memory is written. */
static void hold_cpu(rtw_t *c) {
    u16 enable = rd16(c, REG_SYS_FUNC_EN);
    wr16(c, REG_SYS_FUNC_EN, (u16)(enable & ~FEN_CPUEN));
}

static void release_cpu(rtw_t *c) {
    u16 enable = rd16(c, REG_SYS_FUNC_EN);
    wr16(c, REG_SYS_FUNC_EN, (u16)(enable | FEN_CPUEN));
}

/* One page of firmware.  The page number goes in a register and the bytes go
 * into a fixed window; the chip works out where they belong from the number.
 * This is the whole of the transfer protocol. */
static void write_page(rtw_t *c, int page, const u8 *data, u32 length) {
    u8 control = rd8(c, REG_MCUFWDL + 2);
    control = (u8)((control & ~0x07) | (page & 0x07));
    wr8(c, REG_MCUFWDL + 2, control);

    for (u32 i = 0; i < length; i++)
        wr8(c, REG_FW_START_ADDRESS + i, data[i]);

    /* A page shorter than the window is padded, because the chip reads the
     * whole window whatever it was told. */
    for (u32 i = length; i < RTW_FW_PAGE_SIZE; i++)
        wr8(c, REG_FW_START_ADDRESS + i, 0);
}

static bool download_firmware(rtw_t *c) {
    wifi_device_t *dev = &c->dev;
    const rtw_fw_header_t *fw = &c->header;

    hold_cpu(c);

    /* Turn the download path on, and clear the checksum report from any
     * previous attempt - it is write-one-to-clear and a stale one would be
     * read back as success. */
    u32 control = rd32(c, REG_MCUFWDL);
    control |= MCUFWDL_EN;
    control &= ~MCUFWDL_RDY;
    wr32(c, REG_MCUFWDL, control);
    wr8(c, REG_MCUFWDL, (u8)(rd8(c, REG_MCUFWDL) | FWDL_CHKSUM_RPT));

    u32 pages = fw->body_size / RTW_FW_PAGE_SIZE;
    u32 remainder = fw->body_size % RTW_FW_PAGE_SIZE;

    for (u32 p = 0; p < pages; p++)
        write_page(c, (int)p, fw->body + p * RTW_FW_PAGE_SIZE, RTW_FW_PAGE_SIZE);
    if (remainder)
        write_page(c, (int)pages, fw->body + pages * RTW_FW_PAGE_SIZE, remainder);

    /* Off again, and let the processor go. */
    control = rd32(c, REG_MCUFWDL);
    control &= ~MCUFWDL_EN;
    wr32(c, REG_MCUFWDL, control);

    release_cpu(c);

    /* The firmware sets a bit when it has started.  Until it does, a command
     * sent to it goes into a mailbox nothing is reading. */
    for (int i = 0; i < 200; i++) {
        if (rd32(c, REG_MCUFWDL) & WINTINI_RDY) {
            kinfo("rtw", "%s: firmware %04x version %u.%u started, %u bytes in "
                         "%u page(s)",
                  dev->name, fw->signature, fw->version, fw->subversion,
                  fw->body_size, pages + (remainder ? 1 : 0));
            c->firmware_running = true;
            return true;
        }
        timer_mdelay(1);
    }

    kwarn("rtw", "%s: the firmware was written but never reported itself ready",
          dev->name);
    return false;
}

/* ------------------------------------------------------------- the mailbox */

/* A command to the firmware goes into one of four mailboxes: the payload in
 * the low four bytes and the extension in the high four, written last because
 * writing the low half is what the chip watches for.
 *
 * Which of the four is used matters only for throughput; the firmware reads
 * whichever has something in it.  Using them in turn means a second command
 * does not have to wait for the first to be picked up. */
static bool send_h2c(rtw_t *c, u8 cmd, const u8 *payload, int len) {
    if (!c->firmware_running) return false;
    if (len > RTW_H2C_PAYLOAD) return false;

    u8 box = c->next_box;
    c->next_box = (u8)((c->next_box + 1) & 3);

    /* Wait for the box to be free.  The chip clears its bit when it has read
     * the previous command out. */
    for (int i = 0; i < 100; i++) {
        if (!(rd16(c, REG_HMETFR) & (1u << box))) break;
        timer_udelay(100);
    }
    if (rd16(c, REG_HMETFR) & (1u << box)) {
        kwarn("rtw", "%s: mailbox %u is still full", c->dev.name, box);
        return false;
    }

    u8 message[RTW_H2C_BOX_SIZE];
    memset(message, 0, sizeof message);
    message[0] = cmd;
    if (payload && len) memcpy(message + 1, payload, (size_t)len);

    u32 base = REG_HMEBOX0 + (u32)box * 4;
    u32 ext_base = REG_HMEBOX0_EXT + (u32)box * 4;

    /* The extension half first: writing the low half is the doorbell, so
     * anything the firmware needs alongside it has to already be there. */
    wr32(c, ext_base, le32(message + 4));
    wr32(c, base, le32(message));
    return true;
}

/* -------------------------------------------------------- what comes back */

/* The firmware answers through a second set of registers - one event register
 * and a small buffer.  A byte count of zero means nothing has arrived. */
static void drain_c2h(rtw_t *c) {
    for (int guard = 0; guard < 8; guard++) {
        if (rd8(c, REG_C2HEVT_CLEAR) != C2H_EVT_READY) return;  /* nothing waiting */

        u8 id = rd8(c, REG_C2HEVT_MSG);
        u8 length = rd8(c, REG_C2HEVT_MSG + 1);
        if (length > RTW_C2H_MAX) length = RTW_C2H_MAX;

        u8 body[RTW_C2H_MAX];
        for (u8 i = 0; i < length; i++) body[i] = rd8(c, REG_C2HEVT_MSG + 2 + i);

        switch (id) {
        case C2H_MSG_RX_FRAME: {
            /* A frame does not fit in fourteen bytes, so the event says how
             * long it is and where to find it; the frame itself is in the
             * window.  The first byte is the signal strength. */
            if (length < 3) break;
            int frame_len = body[1] | (body[2] << 8);
            if (frame_len <= 0 || frame_len > RTW_TX_WINDOW) break;

            static u8 frame[RTW_TX_WINDOW];
            for (int i = 0; i < frame_len; i++)
                frame[i] = rd8(c, REG_TX_BUFFER + (u32)i);
            ieee80211_receive(&c->dev, frame, frame_len, -(s8)body[0]);
            break;
        }
        case C2H_MSG_TX_REPORT:
            break;
        default:
            kdebug("rtw", "%s: event %02x, %u bytes", c->dev.name, id, length);
            break;
        }

        /* Telling the chip the buffer is free again is what lets the next one
         * arrive. */
        wr8(c, REG_C2HEVT_CLEAR, C2H_EVT_EMPTY);
    }
}

/* --------------------------------------------------------------- the driver */

static bool rtw_start(wifi_device_t *dev) {
    rtw_t *c = dev->ctx;
    if (dev->unsupported_generation && !c->is_be) return false;

    if (c->is_be) {
        /* Wi-Fi toggle must never feed a BE device through the legacy 8051
         * firmware/mailbox registers. Boot owns BE initialization for now. */
        if (!dev->radio_up)
            kwarn("rtw", "%s: BE radio initialization is not complete; legacy restart refused",
                  dev->name);
        return dev->radio_up;
    }

    /* A model has no firmware file to be given, so it gets an image built to
     * Realtek's own format.  The whole path then runs: the header is read, the
     * pages go across the window in order, and the processor is released. */
    if (c->modelled && !dev->firmware_present && !c->header_read) {
        static u8 image[RTW_FW_PAGE_SIZE * 3];
        size_t len = rtw_build_test_firmware(image, sizeof image);
        if (len && rtw_parse_firmware(image, len, &c->header)) {
            c->header_read = true;
            kinfo("rtw", "%s: no firmware file, so an image built to the same "
                         "format is being loaded into the model", dev->name);
        }
    }

    if (!c->header_read) {
        if (!dev->firmware_present) {
            kwarn("rtw", "%s: %s is not present, so the radio cannot start",
                  dev->name, dev->firmware_name);
            kwarn("rtw", "%s: `firmware import <path>` copies it in; it is in "
                         "the linux-firmware package", dev->name);
            return false;
        }
        if (!c->fw.data && !firmware_load(dev->firmware_name, &c->fw)) return false;
        if (!rtw_parse_firmware(c->fw.data, c->fw.size, &c->header)) return false;
        c->header_read = true;
    }

    if (!c->regs) {
        kwarn("rtw", "%s: the firmware is readable but the device is not mapped",
              dev->name);
        return false;
    }

    if (!download_firmware(c)) return false;

    /* The chip's own address, out of its EEPROM.  It is readable before the
     * firmware runs as well, but reading it afterwards checks that the
     * firmware did not disturb it. */
    for (int i = 0; i < ETH_ALEN; i++)
        dev->mac.addr[i] = rd8(c, REG_MACID + i);

    /* Tell the firmware the host is up and what to do with what it receives. */
    u8 media_status[RTW_H2C_PAYLOAD] = { 0 };
    media_status[0] = 1;                       /* connected: no, but listening */
    send_h2c(c, H2C_SET_MEDIA_STATUS, media_status, 1);

    dev->radio_up = true;
    char mac[24];
    mac_format(&dev->mac, mac, sizeof mac);
    kinfo("rtw", "%s: the radio is up, %s%s", dev->name, mac,
          c->modelled ? " (model)" : "");
    return true;
}

static void rtw_stop(wifi_device_t *dev) {
    rtw_t *c = dev->ctx;
    if (c->is_be || dev->unsupported_generation) {
        /* Never use the 8051 stop bit on BE silicon.  Quiesce PCIe transport
         * and retain all DMA pages even if it times out: completion may still
         * arrive after the caller has stopped using the radio. */
        if (c->is_be && c->regs && c->fwdl_ring_attached &&
            !rtw89_dma_quiesce(c->regs, dev->model))
            kwarn("rtw", "%s: BE DMA did not quiesce; backing retained",
                  dev->model);
        c->fwdl_ring_poisoned = true;
        c->firmware_running = false;
        c->mailbox_ready = false;
        c->data_ready = false;
        c->rx_ready = false;
        c->calibrated = false;
        dev->radio_up = false;
        return;
    }
    if (c->regs) hold_cpu(c);
    firmware_free(&c->fw);
    c->header_read = false;
    c->firmware_running = false;
    dev->radio_up = false;
}

static bool rtw_set_channel(wifi_device_t *dev, u8 channel) {
    rtw_t *c = dev->ctx;

    /* RF tuning alone is not a working channel switch. Do not let a UI scan
     * bypass MAC, BB/RF initialization and calibrated power-limit setup. */
    if (c->is_be) {
        if (!c->regs || !dev->radio_up || !c->calibrated) return false;
        if (!rtw89_set_channel(c->regs, channel,
                               rtw89_band_of_channel(channel), false)) return false;
        dev->channel = channel;
        return true;
    }

    if (!c->firmware_running) return false;

    u8 payload[RTW_H2C_PAYLOAD] = { 0 };
    payload[0] = channel;
    payload[1] = 0;                            /* twenty megahertz */
    if (!send_h2c(c, H2C_SET_CHANNEL, payload, 2)) return false;
    dev->channel = channel;
    return true;
}

/* A frame goes into the transmit window and is announced with a command
 * naming its length.  Larger parts have a descriptor ring; this window works
 * on every part in the family and is what a management frame uses anyway. */
static int rtw_transmit(wifi_device_t *dev, const void *frame, int len) {
    rtw_t *c = dev->ctx;

    /* The BE generation puts a frame on a ring with a descriptor in front of
     * it.  The earlier parts copy it into a window on the card and then ask
     * the firmware to send what is in the window - two genuinely different
     * mechanisms, not two spellings of one. */
    if (c->is_be) {
        if (!dev->radio_up || !c->calibrated || !c->data_ready ||
            !frame || len <= 0) return -1;
        const u8 *a1 = frame;                 /* the recipient, in the header */
        bool broadcast = len >= 10 && (a1[4] & 1);
        return rtw89_data_transmit(c->regs, &c->data, frame, len, broadcast);
    }

    if (!c->firmware_running) return -1;
    if (len <= 0 || len > RTW_TX_WINDOW) return -1;

    const u8 *bytes = frame;
    for (int i = 0; i < len; i++) wr8(c, REG_TX_BUFFER + (u32)i, bytes[i]);

    u8 payload[RTW_H2C_PAYLOAD] = { 0 };
    payload[0] = (u8)len;
    payload[1] = (u8)(len >> 8);
    payload[2] = c->sequence++;
    if (!send_h2c(c, H2C_TRANSMIT, payload, 3)) return -1;
    return len;
}

/* One frame off the card, handed to the stack above.
 *
 * ieee80211_receive is where scanning, association and the handshake already
 * live, so nothing about them is repeated here - this is only the join. */
static void deliver_to_stack(void *ctx, const u8 *frame, u32 len,
                             s8 signal_dbm) {
    ieee80211_receive((wifi_device_t *)ctx, (u8 *)frame, (int)len, signal_dbm);
}

static void rtw_poll(wifi_device_t *dev) {
    rtw_t *c = dev->ctx;

    if (c->is_be) {
        if (c->rx_ready) rtw89_rx_poll(c->regs, &c->rx, deliver_to_stack, dev);
        return;
    }

    if (c->firmware_running) drain_c2h(c);
}

/* The chip decrypts in hardware once it has the key, which is why this is
 * worth telling it rather than leaving to the software path. */
static bool rtw_set_key(wifi_device_t *dev, const u8 *key, int len, bool pairwise) {
    rtw_t *c = dev->ctx;
    if (c->is_be) return false; /* BE CAM programming is not the legacy mailbox. */
    if (!c->firmware_running || len != 16) return false;

    u8 payload[RTW_H2C_PAYLOAD] = { 0 };
    payload[0] = pairwise ? 1 : 0;
    payload[1] = 4;                            /* CCMP */
    if (!send_h2c(c, H2C_SET_KEY, payload, 2)) return false;

    /* The key itself goes into the chip's key table rather than a mailbox:
     * eight bytes will not hold sixteen. */
    for (int i = 0; i < len; i++) wr8(c, REG_CAM_WRITE + (u32)i, key[i]);
    wr32(c, REG_CAM_CMD, CAM_CMD_POLLING | CAM_CMD_WRITE | (pairwise ? 4 : 0));
    return true;
}

static const wifi_driver_t rtw_driver_ops = {
    "rtw", rtw_start, rtw_stop, rtw_set_channel,
    rtw_transmit, rtw_poll, rtw_set_key,
};


/* Push the image into a BE-generation card.
 *
 * Everything here was built and checked separately - the request header, the
 * descriptors, the ring, the doorbell, the download's own cutting of sections
 * into pieces - and this is the first thing that uses them on a card rather
 * than against a model.
 *
 * What it proves, on real silicon, is the thing no model can: whether this
 * particular processor accepts this particular image.  The answer is a status
 * the card holds, and the whole reason for coming this far is to get it into
 * the log where somebody can read it.
 */
typedef struct {
    volatile u8 *regs;
    const char *who;
    rtw89_ring_t ring;
    u8  *packet;
    u64  packet_phys;
    int  sent;
    bool failed;
    bool expect_fwdl_path; /* only the firmware-header H2C changes ROM phase */
} be_push_t;

/* One request, out through the ring. */
static bool be_send(void *ctx, const u8 *packet, u32 len, bool fwdl) {
    be_push_t *p = ctx;
    enum { BE_FWCMD_DESC_BYTES = 24, BE_FWCMD_MAX_BYTES = 2028 };
    if (p->failed || !len || len > BE_FWCMD_MAX_BYTES) return false;

    /* RTL8922A's CH12 fetches a BE short RX descriptor before the H2C or
     * raw firmware bytes. Linux marks the two phases as packet types 13 and
     * 14 respectively; without this prefix the card parses the first four
     * firmware bytes as a descriptor and never sees the intended request. */
    memset(p->packet, 0, BE_FWCMD_DESC_BYTES);
    u32 descriptor = (len & BE_RXD_RPKT_LEN_MASK) |
                     ((fwdl ? 14u : 13u) << 24);
    p->packet[0] = (u8)descriptor;
    p->packet[1] = (u8)(descriptor >> 8);
    p->packet[2] = (u8)(descriptor >> 16);
    p->packet[3] = (u8)(descriptor >> 24);
    memcpy(p->packet + BE_FWCMD_DESC_BYTES, packet, len);

    if (!rtw89_ring_add(&p->ring, p->packet_phys,
                        len + BE_FWCMD_DESC_BYTES, true)) {
        /* Full: the card has not kept up.  Waiting for it needs the completion
         * path, which is the next piece; until then this is a refusal rather
         * than a wait, and it says so. */
        p->failed = true;
        return false;
    }

    rtw89_ring_doorbell(p->regs, &p->ring, R_BE_CH12_TXBD_IDX);
    p->sent++;

    /* The card reads the descriptor and then the packet, and the buffer is
     * reused for the next piece - so it has to have finished with this one.
     * A ring per piece would avoid the wait; one buffer and a wait is the
     * simpler thing that is correct, and a download happens once. */
    for (int spent = 0; spent < 100; spent++) {
        u16 completed = rtw89_ring_card_index(p->regs, R_BE_CH12_TXBD_IDX);
        if (completed >= p->ring.slots) {
            p->failed = true;
            return false;
        }
        if (completed == p->ring.host_index) {
            /* The hardware has fetched the only outstanding descriptor.
             * Retire it in the software ring too, otherwise ring_free sees
             * all 127 entries as occupied after a long firmware image. */
            p->ring.card_index = completed;
            if (!fwdl && p->expect_fwdl_path) {
                /* The ROM switches from H2C to the raw firmware path only
                 * after it consumes the image header. Linux waits for this
                 * second handshake before streaming any section bytes. */
                if (!rtw89_fwdl_path_ready(p->regs, false, p->who)) {
                    p->failed = true;
                    return false;
                }
                *(volatile u32 *)(p->regs + R_BE_HALT_H2C_CTRL) = 0;
                *(volatile u32 *)(p->regs + R_BE_HALT_C2H_CTRL) = 0;
            }
            return true;
        }
        timer_mdelay(1);
    }

    p->failed = true;
    return false;
}

static bool be_download_firmware(volatile u8 *regs, wifi_device_t *dev,
                                 const u8 *image, size_t image_size,
                                 rtw89_fw_info_t *info, bool bbmcu0) {
    rtw_t *card = dev->ctx;
    be_push_t push;
    if (!card || card->fwdl_ring_poisoned) return false;

    /* Linux fw_download_suit waits before EACH header, including BBMCU0.
     * NORMAL's completion does not imply that the next H2C phase is open.
     * RTL8922A additionally supplies the ROM allocator address for NORMAL. */
    if (!bbmcu0)
        *(volatile u32 *)(regs + R_BE_SECURE_BOOT_MALLOC_INFO) = 0x20248000u;
    if (!rtw89_fwdl_path_ready(regs, true, dev->model)) return false;

    if (!card->fwdl_ring_memory)
        card->fwdl_ring_memory = dma_alloc_pages(1, &card->fwdl_ring_phys);
    if (!card->fwdl_packet_memory)
        card->fwdl_packet_memory = dma_alloc_pages(2, &card->fwdl_packet_phys);
    if (!card->fwdl_ring_memory || !card->fwdl_packet_memory) {
        kwarn("rtw", "%s: no memory the card could be pointed at, so the image "
                     "was not pushed", dev->model);
        return false;
    }

    memset(&push, 0, sizeof push);
    push.regs = regs;
    push.who = dev->model;
    push.packet = card->fwdl_packet_memory;
    push.packet_phys = card->fwdl_packet_phys;
    push.expect_fwdl_path = true;
    if (card->fwdl_ring_attached) {
        push.ring = card->fwdl_ring;
    } else {
        push.ring.bd = card->fwdl_ring_memory;
        push.ring.bd_phys = card->fwdl_ring_phys;
        push.ring.slots = 128;
        memset(card->fwdl_ring_memory, 0, 128 * RTW89_PCI_BD_BYTES);
        if (!rtw89_ring_attach(regs, &push.ring, R_BE_CH12_TXBD_DESA_L,
                               R_BE_CH12_TXBD_DESA_H, R_BE_CH12_TXBD_NUM,
                               R_BE_CH12_TXBD_IDX)) {
            kwarn("rtw", "%s: the card would not take a ring", dev->model);
            card->fwdl_ring_poisoned = true;
            return false;
        }
        card->fwdl_ring_attached = true;
    }

    if (!rtw89_fw_download(image, image_size, info, be_send, &push)) {
        card->fwdl_ring_poisoned = true;
        kwarn("rtw", "%s: the image was not handed over whole (%d request(s) "
                     "went)", dev->model, push.sent);
        return false;
    }
    card->fwdl_ring = push.ring;

    /* And whether it will run what it was given. */
    if (bbmcu0) {
        if (!rtw89_fw_wait_bb0_ready(regs, 2000, dev->model)) {
            card->fwdl_ring_poisoned = true;
            return false;
        }
    } else if (!rtw89_fw_wait_ready(regs, 2000, dev->model)) {
        card->fwdl_ring_poisoned = true;
        return false;
    }

    kinfo("rtw", "%s: %s firmware download accepted (%d requests); runtime not yet verified",
          dev->model, bbmcu0?"BBMCU0":"NORMAL", push.sent);
    return true;
}

/* Which generation this part belongs to.
 *
 * Realtek's wireless parts fall into three families that share a vendor number
 * and almost nothing else.  The older ones - the 8188, 8192, 8812 and 8821 -
 * are what this driver was written from: an 8051 core, firmware pushed in
 * pages through a fixed window, four mailboxes.  The 8822 onwards replaced all
 * of that, and the 8852 and 8922 replaced it again for Wi-Fi 6 and 7.
 *
 * The registers did not move out of the way when that happened.  The addresses
 * this driver writes to still exist on a newer part and still accept writes -
 * they simply mean something else.  So running this sequence against a Wi-Fi 7
 * card is not a driver that fails: it is a driver that writes an unrelated
 * pattern into a working card's control registers, and the best outcome is
 * that nothing happens.
 *
 * A card of a generation this was not written for is therefore reported and
 * left alone.  Somebody looking at the machine sees the card, its name and
 * whether its firmware is present - which is the useful part - and nothing
 * pokes at it.
 */
/* Which of the unsupported parts belong to the newer family this system now
 * has a start-up sequence for.  Separate from the check below because the two
 * questions are different: one is "was this driver written for it", the other
 * is "is there another driver here that was". */
static bool generation_is_be(u16 device) {
    switch (device) {
    case 0x8922:                  /* RTL8922AE, Wi-Fi 7             */
        return true;
    default:
        return false;
    }
}

static bool generation_is_supported(u16 device) {
    /* Named the other way round deliberately: the parts this was written
     * against are the ones it has been driven against, and the self-test
     * drives an 8822 - so a list of what is allowed would have to be kept in
     * step with the test or it would refuse the very card it is verified on.
     * What is listed here is what is known to be different. */
    switch (device) {
    case 0x8852: case 0xA85A:     /* RTL8852AE, Wi-Fi 6             */
    case 0xB852: case 0xB85B:     /* RTL8852BE                      */
    case 0xC852:                  /* RTL8852CE, Wi-Fi 6E            */
    case 0x8851:                  /* RTL8851BE                      */
    case 0x8922:                  /* RTL8922AE, Wi-Fi 7             */
        return false;
    default:
        return true;
    }
}

/* ------------------------------------------------------------- bringing up */

/* Admit every mandatory radio resource before starting DMA. Executable
 * images do not include BB, RF, gain or regulatory power tables. Retain the
 * package allocation while any of the resulting views are in card state. */
static bool be_prepare_radio(const firmware_t *image, u8 cv, u8 rfe,
                              rtw89_radio_resources_t *resources,
                              rtw89_radio_gain_t *gain) {
    if (!image || !rtw89_radio_resources_load(image->data, image->size,
                                              cv, rfe, resources)) return false;
    if (!rtw89_radio_gain_decode(&resources->gain, rfe, cv, gain)) return false;
    for (unsigned i = 0; i < 9; ++i)
        if (!rtw89_radio_power_walk(&resources->power[i], NULL, NULL)) return false;
    return true;
}

static bool bring_up(rtw_t *c, volatile u8 *regs, size_t reg_bytes,
                     u16 pci_device, bool modelled) {
    memset(c, 0, sizeof *c);
    c->regs = regs;
    c->modelled = modelled;

    /* Say how far the mapping goes before anything reads a register.  The
     * radio's memory-mapped interface is around 0x2f000, far above everything
     * else here, and on a card whose window is smaller those accesses would
     * land outside it. */
    rtw89_set_register_window((u32)reg_bytes);

    wifi_device_t *dev = &c->dev;
    if (!wifi_identify(0x10EC, pci_device, dev)) return false;

    c->is_be = generation_is_be(pci_device);

    firmware_declare(dev->firmware_name, "the linux-firmware package");
    dev->firmware_present = firmware_present(dev->firmware_name, NULL);

    snprintf(dev->name, sizeof dev->name, "wlan%d", wifi_count());
    dev->driver = &rtw_driver_ops;
    dev->ctx = c;

    wifi_register(dev);
    ieee80211_attach(dev);

    if (!generation_is_supported(pci_device)) {
        dev->unsupported_generation = true;
        /* Registered so it is reported, and not started.  See the note above:
         * the sequence below was written for a different generation and the
         * registers it writes to exist on this one meaning other things. */
        /* The newer family has its own start-up sequence now - Realtek's own,
         * transcribed rather than remembered, and checked step by step
         * against a model at boot.  It is run here.
         *
         * What that does and does not achieve is worth being exact about,
         * because "the Wi-Fi driver works" is the kind of claim that is easy
         * to make from a card that merely stopped saying no.  After this the
         * MAC is out of reset and its blocks are running.  There is still no
         * firmware in it, no command interface, no radio calibration and no
         * association - so the card does not carry traffic and this does not
         * pretend it does. */
        if (generation_is_be(pci_device)) {
            /* The last thing this sequence writes sits at 0x10000, so a
             * window that does not reach it would take the write somewhere
             * else entirely - into whatever the next mapping happens to be.
             * Refusing is the only safe answer; there is nothing to fall back
             * to. */
            if (reg_bytes < RTW89_REG_BYTES) {
                kwarn("rtw", "%s presents only %u bytes of registers and its "
                             "start-up sequence reaches %u; it is left alone "
                             "rather than written past the end of",
                      dev->model, (unsigned)reg_bytes,
                      (unsigned)RTW89_REG_BYTES);
                return true;
            }

            /* Linux reads the cut from the AX-named system register even for
             * BE parts.  It chooses which member of an MFW package can run on
             * this exact silicon, so reading it cannot be deferred until after
             * the package has already been selected. */
            u8 hardware_cv;
            if (!rtw89_chip_cv(regs, &hardware_cv)) {
                kwarn("rtw", "%s did not return a valid hardware cut; firmware "
                             "selection is stopped rather than guessed",
                      dev->model);
                return true;
            }

            if (!rtw89_power_on(regs, dev->model)) {
                kwarn("rtw", "%s would not come out of reset; it is left "
                             "alone", dev->model);
                return true;
            }

            dev->powered_on = true;
            if (!rtw89_read_pci_mac(regs, dev->mac.addr)) {
                kwarn("rtw", "%s: hardware MAC address is invalid; startup stopped", dev->model);
                return true;
            }
            dev->net.mac = dev->mac;
            /* The per-board RF front-end, crystal trim and country fields are
             * not firmware defaults. Read the RTL8922A's own physical/logical
             * efuse before any RF table is selected or any scan transmits. */
            u8 *physical = kmalloc(0x1300);
            if (physical) {
                c->board_data_valid =
                    rtw89_efuse_read(regs, hardware_cv, 0, physical, 0x1300) &&
                    rtw89_efuse_logical(physical, 0x1300, 1, 0,
                                        c->board_rf, sizeof c->board_rf) &&
                    rtw89_efuse_read(regs, hardware_cv, 0x1700,
                                     c->board_phycap, sizeof c->board_phycap);
                kfree(physical);
            }
            if (!c->board_data_valid || c->board_rf[0xca] == 0xff) {
                c->board_data_valid = false;
                kwarn("rtw", "%s: board RF/trim efuse is unavailable; radio startup stopped", dev->model);
                return true;
            }
            kinfo("rtw", "%s: board RFE=%u crystal=%u; RF/phycap maps read",
                  dev->model, c->board_rf[0xca], c->board_rf[0xb9]);
            kinfo("rtw", "%s is out of reset: its packet engine and first "
                         "radio block are enabled", dev->model);

            /* Validate executable images, board resources and security profile
             * before handing any firmware bytes to the bus download path. */
            if (!dev->firmware_present) {
                kinfo("rtw", "its firmware (%s) is not here, so there is "
                             "nothing to check", dev->firmware_name);
                return true;
            }

            firmware_t image;
            if (!firmware_load(dev->firmware_name, &image)) {
                kwarn("rtw", "%s is listed as present but would not load",
                      dev->firmware_name);
                return true;
            }

            rtw89_fw_image_t selected;
            if (!rtw89_select_firmware(image.data, image.size, hardware_cv,
                                       RTW89_FW_NORMAL, &selected)) {
                firmware_free(&image);
                return true;
            }
            if (selected.from_container)
                kinfo("rtw", "selected normal firmware cut %u for hardware cut %u "
                             "from its MFW container (%u bytes)",
                      selected.cv, hardware_cv, (unsigned)selected.size);

            rtw89_fw_info_t info;
            bool readable = rtw89_parse_firmware(selected.data, selected.size,
                                                 &info);
            if (readable)
                kinfo("rtw", "its firmware is version %u.%u.%u.%u built "
                             "%u-%02u-%02u, %d sections, %u bytes to download",
                      info.major, info.minor, info.subversion, info.subindex,
                      info.year, info.month, info.date,
                      info.section_count, info.payload_bytes);
            if (!readable) { firmware_free(&image); return true; }

            rtw89_radio_resources_t resources;
            if (!be_prepare_radio(&image, hardware_cv, c->board_rf[0xca],
                                   &resources, &c->radio_gain)) {
                kwarn("rtw", "%s: board-specific radio resources invalid or unsupported; nothing downloaded",
                      dev->model);
                firmware_free(&image);
                return true;
            }

            /* Profile is read even for an image without a signature pool:
             * secure silicon must not receive a non-secure-only image. */
            rtw89_fw_security_t security;
            if (!rtw89_fw_security_read(regs, hardware_cv, &security) ||
                !rtw89_fw_apply_security(selected.data, selected.size,
                                         &security, &info)) {
                kwarn("rtw", "%s: firmware cut %u has no validated matching "
                             "security profile/signature; nothing downloaded",
                      dev->model, selected.cv);
                firmware_free(&image);
                return true;
            }

            /* RTL8922A has one BB MCU.  Its executable is an appended
             * element, not another NORMAL MFW suit; start with the same cut
             * policy as Linux and reject a missing/malformed second image
             * before touching the bus download path. */
            const u8 *bb_image = NULL;
            size_t bb_size = 0;
            rtw89_fw_info_t bb_info;
            if (!rtw89_fw_element_select(image.data, image.size, hardware_cv,
                                         0, &bb_image, &bb_size) ||
                !rtw89_parse_firmware(bb_image, bb_size, &bb_info) ||
                (bb_info.needs_security_profile &&
                 !rtw89_fw_apply_security(bb_image, bb_size, &security,
                                          &bb_info))) {
                kwarn("rtw", "%s: matching BBMCU0 firmware is absent or invalid; "
                             "nothing downloaded", dev->model);
                firmware_free(&image);
                return true;
            }

            /* The bus-side engine, before the processor: the download the
             * next piece will do goes through it. */
            if (!rtw89_fwdl_preinit(regs, dev->model) ||
                !rtw89_dma_reset(regs, 256, dev->model)) {
                kwarn("rtw", "%s: its DMA engine would not restart, so "
                              "nothing could be handed to it", dev->model);
                firmware_free(&image);
                return true;
            }

            if (!rtw89_fwdl_start_cpu(regs, 0, true, dev->model)) {
                firmware_free(&image);
                return true;
            }

            u8 st = rtw89_fwdl_status(regs);
            kinfo("rtw", "%s: its processor is running and the download path "
                         "is open - it reports \"%s\"",
                  dev->model, rtw89_fwdl_status_name(st));

            /* Retain CH12 and its DMA backing across both processors' images.
             * The card can still own these pages after a failed download. */
            if (be_download_firmware(regs, dev, selected.data, selected.size,
                                     &info, false) &&
                be_download_firmware(regs, dev, bb_image, bb_size,
                                     &bb_info, true)) {
                /* Linux gives FreeRTOS a short final interval after BB MCU
                 * completion before checking the running WCPU once more. */
                timer_mdelay(5);
                if (!rtw89_fw_wait_running(regs, 2000, dev->model)) {
                    firmware_free(&image);
                    return true;
                }
                c->firmware_running = true;
                /* DLFW quotas and CH12 are not a running MAC/RX setup. Do not
                 * replace its live ring with the old eight-slot mailbox or
                 * calibrate before BB/RF tables and power limits are applied. */
                c->fw = image;
                memset(&image, 0, sizeof image);
                c->radio_resources = resources;
                c->radio_resources_valid = true;
                kinfo("rtw", "%s: NORMAL+BBMCU firmware ready; radio tables admitted (gain rows=%u)",
                      dev->model, c->radio_gain.loaded_rows);
                kwarn("rtw", "%s: MAC runtime/RX and calibrated regulatory power setup incomplete; transmission remains disabled",
                      dev->model);
            } else {
                kwarn("rtw", "%s: firmware download incomplete; MAC/RX, radio calibration and network joining remain unavailable",
                      dev->model);
            }

            firmware_free(&image);
            return true;
        }

        kwarn("rtw", "%s is a generation this driver was not written for, so "
                     "it is listed and not driven - driving it would mean "
                     "writing another part's start-up sequence into it",
              dev->model);
        if (dev->firmware_present)
            kinfo("rtw", "its firmware (%s) is here, so what is missing is the "
                         "driver for this generation rather than the firmware",
                  dev->firmware_name);
        return true;
    }

    rtw_start(dev);
    return true;
}

/* Bring up one candidate PCI device if it is a Realtek Wi-Fi part this driver
 * drives.  bring_up() calls wifi_identify(), which rejects everything that is
 * not a known Realtek Wi-Fi device ID - including Realtek's ETHERNET parts,
 * which share vendor 0x10EC and are iterated here too - so scanning widely and
 * letting the ID table be the filter is safe. */
static void rtw_try(pci_dev_t *pci) {
    if (pci->vendor != 0x10EC) return;
    if (card_count >= MAX_CARDS) return;

    /* Only a known Realtek Wi-Fi device ID.  Checked BEFORE enabling memory or
     * mapping a BAR, so a Realtek ETHERNET part (same vendor, e.g. the RTL8125,
     * now reached because the scan is class-agnostic) is left completely
     * untouched for its own driver. */
    wifi_device_t probe;
    memset(&probe, 0, sizeof probe);
    if (!wifi_identify(0x10EC, pci->device, &probe)) return;

    volatile u8 *regs = NULL;
    size_t mapped_bytes = 0;
    if (pci->bar[2] && !pci->bar_is_io[2]) {
        /* Realtek parts put their registers in the third region. */
        pci_enable_memory(pci);
        pci_enable_bus_master(pci);
        /* When the card did not say, map enough for the radio's memory-mapped
         * interface rather than only the low registers - 0x4000 covers
         * everything this driver touches EXCEPT tuning. */
        size_t len = pci->bar_size[2] ? (size_t)pci->bar_size[2] : 0x40000;
        if (len > 0x100000) len = 0x100000;
        mapped_bytes = len;
        regs = vmm_map_mmio(pci->bar[2], len);
    }

    if (bring_up(&cards[card_count], regs, mapped_bytes, pci->device, false)) {
        pci_claim(pci, "rtw89");
        card_count++;
    }
}

void rtw_init(void) {
    /* Match by vendor + device ID across whatever class the card reports, the
     * way Linux's rtw89 does (PCI_DEVICE is class-agnostic).  The old
     * pci_find(0x02, 0x80, ...) demanded class 02 / subclass 80 EXACTLY - but a
     * Realtek Wi-Fi part can report subclass 00, or sit under the wireless
     * class 0D, and then it was silently never claimed and the radio was simply
     * dead with no error.  Scan network (02) AND wireless (0D), any subclass. */
    static const u8 classes[2] = { 0x02, 0x0D };
    for (int c = 0; c < 2; c++) {
        pci_dev_t *pci = NULL;
        while ((pci = pci_find(classes[c], 0xFF, 0xFF, pci)) != NULL)
            rtw_try(pci);
    }
}

volatile u8 *rtw_model_attach(void);

/* Bring the driver up against a model of the hardware, so the download, the
 * mailbox protocol and the receive path all run. */
bool rtw_attach_model(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return cards[i].dev.radio_up;
    if (card_count >= MAX_CARDS) return false;

    volatile u8 *regs = rtw_model_attach();
    if (!regs) return false;

    int before = card_count;
    if (bring_up(&cards[card_count], regs, RTW89_REG_BYTES, 0xB822, true))
        card_count++;
    return card_count > before;
}

wifi_device_t *rtw_model_device(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return &cards[i].dev;
    return NULL;
}

/* A firmware image built to Realtek's own format, so the model has something
 * real to be given and the header reader has something real to read. */
size_t rtw_build_test_firmware(u8 *out, size_t cap) {
    if (cap < RTW_FW_HEADER_SIZE + 4096) return 0;
    memset(out, 0, cap);

    out[0] = (u8)(RTW_SIG_8822B & 0xFF);
    out[1] = (u8)(RTW_SIG_8822B >> 8);
    out[2] = 0;                                /* category */
    out[3] = 1;                                /* function */
    out[4] = 27; out[5] = 0;                   /* version 27 */
    out[6] = 4;                                /* subversion */
    out[7] = 0;
    out[16] = 8; out[17] = 26;                 /* built on the twenty-sixth */
    out[18] = 12; out[19] = 0;
    out[20] = RTW_FW_HEADER_SIZE; out[21] = 0;

    /* Two pages and a bit, so the paging and the short last page both run. */
    size_t body = RTW_FW_PAGE_SIZE * 2 + 500;
    for (size_t i = 0; i < body; i++)
        out[RTW_FW_HEADER_SIZE + i] = (u8)(i * 7 + 3);

    return RTW_FW_HEADER_SIZE + body;
}

/* ------------------------------------------------------------------- tests */

/* Drive the driver against a model of the hardware: the header, the paged
 * download, the mailbox protocol and a frame going out and coming back.
 *
 * Returns the number of checks that failed. */
bool      rtw_model_firmware_running(void);
int       rtw_model_pages(void);
const u8 *rtw_model_image(void);
u8        rtw_model_channel(void);

int rtw_drive_test(void) {
    /* The header reader first, on its own: a file for the wrong chip and a
     * file that is not one at all both have to be refused, and that is what
     * goes wrong most often in practice. */
    int failures = 0;
    rtw_fw_header_t header;

    static u8 image[RTW_FW_PAGE_SIZE * 3];
    size_t len = rtw_build_test_firmware(image, sizeof image);

    if (!rtw_parse_firmware(image, len, &header)) {
        kerr("rtw-test", "a well-formed firmware header was refused");
        failures++;
    } else if (header.signature != RTW_SIG_8822B || header.version != 27 ||
               header.subversion != 4) {
        kerr("rtw-test", "the header came out as %04x version %u.%u",
             header.signature, header.version, header.subversion);
        failures++;
    } else if (header.body_size != len - RTW_FW_HEADER_SIZE) {
        kerr("rtw-test", "the body came out as %u bytes, expected %zu",
             header.body_size, len - RTW_FW_HEADER_SIZE);
        failures++;
    }

    u8 wrong[64];
    memcpy(wrong, image, sizeof wrong);
    wrong[0] = 0x34; wrong[1] = 0x12;
    if (rtw_parse_firmware(wrong, sizeof wrong, &header)) {
        kerr("rtw-test", "a firmware file for another chip was accepted");
        failures++;
    }

    static const u8 nonsense[8] = { 0xDE, 0xAD };
    if (rtw_parse_firmware(nonsense, sizeof nonsense, &header)) {
        kerr("rtw-test", "a file that is not firmware was accepted");
        failures++;
    }

    if (!rtw_attach_model()) {
        kwarn("rtw-test", "the Realtek model could not be brought up");
        return failures + 1;
    }

    wifi_device_t *dev = rtw_model_device();
    if (!dev || !dev->radio_up) {
        kerr("rtw-test", "the radio did not come up");
        return failures + 1;
    }

    /* Every byte of the image has to be where it belongs in the chip's memory.
     * Comparing the whole thing rather than a count is what catches two pages
     * arriving in the wrong order, which a count cannot see. */
    const u8 *body = image + RTW_FW_HEADER_SIZE;
    u32 body_len = (u32)(len - RTW_FW_HEADER_SIZE);
    int expected_pages = (int)((body_len + RTW_FW_PAGE_SIZE - 1) / RTW_FW_PAGE_SIZE);

    if (rtw_model_pages() != expected_pages) {
        kerr("rtw-test", "%d page(s) arrived, expected %d",
             rtw_model_pages(), expected_pages);
        failures++;
    } else {
        const u8 *got = rtw_model_image();
        u32 total = (u32)expected_pages * RTW_FW_PAGE_SIZE;
        u32 wrong = 0;
        for (u32 i = 0; i < total; i++) {
            u8 want = i < body_len ? body[i] : 0;   /* the last page is padded */
            if (got[i] != want) wrong++;
        }
        if (wrong) {
            kerr("rtw-test", "%u byte(s) of the firmware arrived altered", wrong);
            failures++;
        } else {
            kinfo("rtw-test", "%d page(s), %u bytes, arrived exactly as sent",
                  expected_pages, body_len);
        }
    }

    static const u8 expected_mac[ETH_ALEN] = { 0x00, 0xe0, 0x4c, 0x11, 0x22, 0x33 };
    if (memcmp(dev->mac.addr, expected_mac, ETH_ALEN)) {
        char got[24];
        mac_format(&dev->mac, got, sizeof got);
        kerr("rtw-test", "the chip's address came back as %s", got);
        failures++;
    }

    /* A scan drives the mailbox: a channel command, a probe request through
     * the transmit window, and a probe response back through the event
     * register into the 802.11 layer. */
    wifi_scan(dev, 2000);

    const wifi_network_t *found = NULL;
    for (int i = 0; i < dev->scan_count; i++)
        if (!strcmp(dev->scan[i].ssid, "kestrel-test")) found = &dev->scan[i];

    if (!found) {
        kerr("rtw-test", "the scan found nothing through the Realtek driver");
        failures++;
    } else if (found->security != WIFI_SECURITY_WPA2) {
        kerr("rtw-test", "the security came back as %s",
             wifi_security_name(found->security));
        failures++;
    } else {
        kinfo("rtw-test", "a probe request went out through the mailbox and "
                          "\"%s\" came back through the event register",
              found->ssid);
    }

    if (!failures)
        kinfo("rtw-test", "the Realtek driver's header, download and mailbox "
                          "are all correct");
    return failures;
}
