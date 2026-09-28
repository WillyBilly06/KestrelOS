/* mt76.c - MediaTek wireless.
 *
 * MediaTek's parts differ from the other two in a way that shows up
 * immediately: there is no window and no mailbox, only rings.  The firmware
 * goes across in the same descriptors that later carry commands and frames,
 * and the microcontroller answers on a ring of its own.
 *
 * There are also two firmware files rather than one.  The patch goes first and
 * fixes the read-only code already on the chip; the main image follows.  They
 * are not the same shape - the patch has a header at the front with its fields
 * the wrong way round for this processor, and the main image puts its table at
 * the END of the file, so it has to be read backwards.  Getting either wrong
 * produces a chip that accepts everything and then does nothing.
 *
 * The firmware is MediaTek's and is not redistributable, which is why no
 * operating system ships it.
 *
 * ---------------------------------------------------------------------------
 * On testing.  Both container formats, the ring mechanics, the message
 * framing, the download sequence and the receive path are exercised on every
 * run of the self-test, against a model of the device that plays the part of
 * the microcontroller.  The driver code that runs against it is the same code
 * that would run against a card.
 *
 * What that cannot establish is that the register offsets match MediaTek's
 * silicon, nor that the payload of each command matches the firmware's idea of
 * it.  Those come from documentation and change between generations.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "firmware.h"
#include "wifi.h"
#include "net.h"
#include "mt76.h"

void ieee80211_attach(wifi_device_t *dev);
void ieee80211_receive(wifi_device_t *dev, u8 *frame, int len, s8 signal_dbm);
void mt76_model_sync(void);

/* ------------------------------------------------------------------ state */

typedef struct {
    mt76_desc_t *desc;
    u64          desc_phys;
    u8          *buffer;
    u64          buffer_phys;
    u16          cpu_index;      /* how far the driver has filled it */
    u16          dma_index;      /* how far the chip has got         */
} mt76_ring_t;

typedef struct {
    volatile u8  *regs;
    wifi_device_t dev;
    firmware_t    patch_fw, main_fw;
    bool          modelled;

    mt76_patch_t    patch;
    mt76_firmware_t firmware;
    bool            parsed;
    bool            running;

    mt76_ring_t   tx[2];
    mt76_ring_t   rx[2];
    bool          rings_ready;

    u8            sequence;
    /* A response being waited for. */
    bool          waiting;
    u8            waiting_seq;
    bool          answered;
    u8            answer[64];
    int           answer_len;
} mt76_t;

#define MAX_CARDS 2
static mt76_t cards[MAX_CARDS];
static int    card_count;

static inline u32 rd(mt76_t *c, u32 off) {
    if (c->modelled) mt76_model_sync();
    return c->regs ? *(volatile u32 *)(c->regs + off) : 0xFFFFFFFFu;
}
static inline void wr(mt76_t *c, u32 off, u32 v) {
    if (c->regs) *(volatile u32 *)(c->regs + off) = v;
}

/* ------------------------------------------------------------- the readers */

static inline u32 le32(const void *p) {
    const u8 *b = p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}
/* The patch header's fields are the other way round from everything else on
 * this processor, which is the single commonest way to misread it. */
static inline u32 be32(const void *p) {
    const u8 *b = p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

/* ------------------------------------------------------------ the patch file */

bool mt76_parse_patch(const u8 *data, size_t size, mt76_patch_t *out) {
    memset(out, 0, sizeof *out);

    if (size < MT76_PATCH_HEADER_SIZE + 16) {
        kwarn("mt76", "the patch file is too short to be one (%zu bytes)", size);
        return false;
    }

    memcpy(out->build_date, data, 15);
    out->build_date[15] = 0;
    memcpy(out->platform, data + 16, 4);
    out->platform[4] = 0;
    out->hw_sw_version = be32(data + 20);
    out->patch_version = be32(data + 24);

    /* The platform string is what says this is a patch at all, and which chip
     * it is for.  Every MediaTek part writes it. */
    if (out->platform[0] != 'M' || out->platform[1] != 'T') {
        kwarn("mt76", "\"%s\" is not a MediaTek patch platform", out->platform);
        return false;
    }

    u32 section_count = be32(data + 32);
    if (section_count == 0 || section_count > MT76_MAX_SECTIONS) {
        kwarn("mt76", "the patch claims %u sections", section_count);
        return false;
    }

    /* Each section says where it goes and how long it is; the data itself is
     * at an offset from the start of the file. */
    size_t at = MT76_PATCH_HEADER_SIZE;
    for (u32 i = 0; i < section_count; i++) {
        if (at + 16 > size) {
            kwarn("mt76", "the patch's section table runs off the end");
            return false;
        }
        u32 offset = be32(data + at);
        u32 address = be32(data + at + 4);
        u32 length = be32(data + at + 8);
        u32 mode = be32(data + at + 12);
        at += 16;

        if (offset > size || length > size - offset) {
            kwarn("mt76", "a patch section claims %u bytes at %u and the file "
                          "is %zu", length, offset, size);
            return false;
        }

        mt76_section_t *s = &out->section[out->sections++];
        s->address = address;
        s->length = length;
        s->mode = mode;
        s->data = data + offset;
    }
    return true;
}

/* ------------------------------------------------------- the main firmware
 *
 * The table is at the end of the file rather than the start, so the trailer is
 * found by counting backwards from the last byte.  A file truncated by a byte
 * therefore reads as complete nonsense rather than as a short file, which is
 * why every offset below is checked.
 */
#define MT76_TRAILER_SIZE 36
#define MT76_REGION_SIZE  40

bool mt76_parse_firmware(const u8 *data, size_t size, mt76_firmware_t *out) {
    memset(out, 0, sizeof *out);

    if (size < MT76_TRAILER_SIZE + MT76_REGION_SIZE) {
        kwarn("mt76", "the firmware file is too short to be one (%zu bytes)", size);
        return false;
    }

    const u8 *trailer = data + size - MT76_TRAILER_SIZE;
    out->chip_id = trailer[0];
    out->eco_code = trailer[1];
    out->regions = trailer[2];
    out->format_version = trailer[3];
    memcpy(out->version, trailer + 7, 10);
    out->version[10] = 0;
    memcpy(out->build_date, trailer + 17, 15);
    out->build_date[15] = 0;

    if (out->regions == 0 || out->regions > MT76_MAX_SECTIONS) {
        kwarn("mt76", "the firmware claims %u regions", out->regions);
        return false;
    }

    size_t table = (size_t)out->regions * MT76_REGION_SIZE;
    if (table + MT76_TRAILER_SIZE > size) {
        kwarn("mt76", "%u regions will not fit in a %zu byte file",
              out->regions, size);
        return false;
    }

    const u8 *region = data + size - MT76_TRAILER_SIZE - table;
    size_t offset = 0;

    for (u8 i = 0; i < out->regions; i++) {
        u32 address = le32(region + 16);
        u32 length = le32(region + 20);
        region += MT76_REGION_SIZE;

        if (length > size || offset > size - length) {
            kwarn("mt76", "region %u claims %u bytes and only %zu remain",
                  i, length, size - offset);
            return false;
        }

        mt76_section_t *s = &out->section[out->sections++];
        s->address = address;
        s->length = length;
        s->mode = 0;
        s->data = data + offset;
        offset += length;
    }
    return true;
}

/* --------------------------------------------------------------- the rings */

static void free_rings(mt76_t *c) {
    for (int i = 0; i < 2; i++) {
        if (c->tx[i].desc)
            dma_free_pages(c->tx[i].desc,
                           (MT76_RING_SIZE * sizeof(mt76_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE);
        if (c->tx[i].buffer)
            dma_free_pages(c->tx[i].buffer, (MT76_RING_SIZE * MT76_BUFFER_SIZE) / PAGE_SIZE);
        if (c->rx[i].desc)
            dma_free_pages(c->rx[i].desc,
                           (MT76_RING_SIZE * sizeof(mt76_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE);
        if (c->rx[i].buffer)
            dma_free_pages(c->rx[i].buffer, (MT76_RING_SIZE * MT76_BUFFER_SIZE) / PAGE_SIZE);
        memset(&c->tx[i], 0, sizeof c->tx[i]);
        memset(&c->rx[i], 0, sizeof c->rx[i]);
    }
    c->rings_ready = false;
}

static bool alloc_ring(mt76_ring_t *ring) {
    u64 phys;
    size_t desc_pages = (MT76_RING_SIZE * sizeof(mt76_desc_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    ring->desc = dma_alloc_pages(desc_pages, &phys);
    if (!ring->desc) return false;
    ring->desc_phys = phys;
    memset(ring->desc, 0, desc_pages * PAGE_SIZE);

    ring->buffer = dma_alloc_pages((MT76_RING_SIZE * MT76_BUFFER_SIZE) / PAGE_SIZE, &phys);
    if (!ring->buffer) return false;
    ring->buffer_phys = phys;
    memset(ring->buffer, 0, MT76_RING_SIZE * MT76_BUFFER_SIZE);

    ring->cpu_index = 0;
    ring->dma_index = 0;
    return true;
}

static bool setup_rings(mt76_t *c) {
    for (int i = 0; i < 2; i++) {
        if (!alloc_ring(&c->tx[i]) || !alloc_ring(&c->rx[i])) {
            free_rings(c);
            return false;
        }
    }

    /* A receive descriptor is armed by pointing it at a buffer; the chip
     * fills it and clears the length.  Every one is armed up front. */
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < MT76_RING_SIZE; i++) {
            mt76_desc_t *d = &c->rx[r].desc[i];
            d->buf0 = (u32)(c->rx[r].buffer_phys + (u64)i * MT76_BUFFER_SIZE);
            d->ctrl = MT76_BUFFER_SIZE << MT76_CTRL_LEN0_SHIFT;
            d->buf1 = 0;
            d->info = 0;
        }
        c->rx[r].cpu_index = MT76_RING_SIZE - 1;
    }

    c->rings_ready = true;
    return true;
}

static u32 ring_reg(u32 base, int index, u32 field) {
    return base + (u32)index * MT_RING_STRIDE + field;
}

static void program_rings(mt76_t *c) {
    wr(c, MT_WFDMA_GLO_CFG, 0);

    for (int i = 0; i < 2; i++) {
        wr(c, ring_reg(MT_TX_RING_BASE, i, MT_RING_BASE_OFF), (u32)c->tx[i].desc_phys);
        wr(c, ring_reg(MT_TX_RING_BASE, i, MT_RING_COUNT_OFF), MT76_RING_SIZE);
        wr(c, ring_reg(MT_TX_RING_BASE, i, MT_RING_CPU_IDX_OFF), 0);

        wr(c, ring_reg(MT_RX_RING_BASE, i, MT_RING_BASE_OFF), (u32)c->rx[i].desc_phys);
        wr(c, ring_reg(MT_RX_RING_BASE, i, MT_RING_COUNT_OFF), MT76_RING_SIZE);
        wr(c, ring_reg(MT_RX_RING_BASE, i, MT_RING_CPU_IDX_OFF), c->rx[i].cpu_index);
    }

    wr(c, MT_WFDMA_GLO_CFG, MT_GLO_CFG_TX_DMA_EN | MT_GLO_CFG_RX_DMA_EN);
}

/* ------------------------------------------------------------ the messages */

static void handle_event(mt76_t *c, const u8 *message, int length);

static int drain_rx(mt76_t *c, int ring_index) {
    if (!c->rings_ready) return 0;
    if (c->modelled) mt76_model_sync();

    mt76_ring_t *ring = &c->rx[ring_index];
    int taken = 0;

    while (taken < MT76_RING_SIZE) {
        mt76_desc_t *d = &ring->desc[ring->dma_index];
        if (!(d->ctrl & MT76_CTRL_DMA_DONE)) break;

        int length = (int)((d->ctrl >> MT76_CTRL_LEN0_SHIFT) & 0x3FFF);
        const u8 *buffer = ring->buffer + (size_t)ring->dma_index * MT76_BUFFER_SIZE;
        if (length > 0 && length <= MT76_BUFFER_SIZE)
            handle_event(c, buffer, length);

        /* Arm it again for the next one. */
        d->ctrl = MT76_BUFFER_SIZE << MT76_CTRL_LEN0_SHIFT;
        ring->cpu_index = ring->dma_index;
        ring->dma_index = (u16)((ring->dma_index + 1) % MT76_RING_SIZE);
        wr(c, ring_reg(MT_RX_RING_BASE, ring_index, MT_RING_CPU_IDX_OFF), ring->cpu_index);
        taken++;
    }
    return taken;
}

/* Put one message on a transmit ring.  The descriptor names the buffer and
 * says how long it is; moving the index is what tells the chip to look. */
static bool enqueue(mt76_t *c, int ring_index, const void *data, int length) {
    if (!c->rings_ready || length <= 0 || length > MT76_BUFFER_SIZE) return false;

    mt76_ring_t *ring = &c->tx[ring_index];
    u16 slot = ring->cpu_index;

    memcpy(ring->buffer + (size_t)slot * MT76_BUFFER_SIZE, data, (size_t)length);

    mt76_desc_t *d = &ring->desc[slot];
    d->buf0 = (u32)(ring->buffer_phys + (u64)slot * MT76_BUFFER_SIZE);
    d->ctrl = ((u32)length << MT76_CTRL_LEN0_SHIFT) | MT76_CTRL_LAST0;
    d->buf1 = 0;
    d->info = 0;

    ring->cpu_index = (u16)((slot + 1) % MT76_RING_SIZE);
    wr(c, ring_reg(MT_TX_RING_BASE, ring_index, MT_RING_CPU_IDX_OFF), ring->cpu_index);
    return true;
}

/* A command to the microcontroller, and the answer if one is wanted. */
static int send_mcu(mt76_t *c, u8 cid, const void *payload, int payload_len,
                    void *answer, int answer_cap, int timeout_ms) {
    u8 message[MT76_BUFFER_SIZE];
    int total = (int)sizeof(mt76_mcu_txd_t) + payload_len;
    if (total > (int)sizeof message) return -1;

    memset(message, 0, (size_t)total);
    mt76_mcu_txd_t *txd = (void *)message;
    txd->length = (u16)total;
    txd->cid = cid;
    txd->pkt_type = MT76_PKT_TYPE_CMD;
    txd->set_query = 1;
    txd->seq = ++c->sequence ? c->sequence : ++c->sequence;   /* never zero */
    if (payload && payload_len)
        memcpy(message + sizeof *txd, payload, (size_t)payload_len);

    c->waiting = true;
    c->waiting_seq = txd->seq;
    c->answered = false;
    c->answer_len = 0;

    if (!enqueue(c, MT_TX_RING_MCU, message, total)) {
        c->waiting = false;
        return -1;
    }

    if (!timeout_ms) { c->waiting = false; return 0; }

    for (int waited = 0; waited < timeout_ms; waited++) {
        drain_rx(c, MT_RX_RING_EVENT);
        if (c->answered) {
            int len = c->answer_len < answer_cap ? c->answer_len : answer_cap;
            if (answer && len > 0) memcpy(answer, c->answer, (size_t)len);
            c->waiting = false;
            return c->answer_len;
        }
        timer_mdelay(1);
    }

    c->waiting = false;
    kwarn("mt76", "command %02x was not answered within %d ms", cid, timeout_ms);
    return -1;
}

static void handle_event(mt76_t *c, const u8 *message, int length) {
    if (length < (int)sizeof(mt76_mcu_rxd_t)) return;
    const mt76_mcu_rxd_t *rxd = (const void *)message;
    const u8 *body = message + sizeof *rxd;
    int body_len = length - (int)sizeof *rxd;

    if (c->waiting && rxd->seq == c->waiting_seq) {
        c->answer_len = body_len < (int)sizeof c->answer ? body_len : (int)sizeof c->answer;
        if (c->answer_len > 0) memcpy(c->answer, body, (size_t)c->answer_len);
        c->answered = true;
        return;
    }

    if (rxd->eid == MCU_EVENT_RX_FRAME && body_len > 1)
        ieee80211_receive(&c->dev, (u8 *)body + 1, body_len - 1, -(s8)body[0]);
}

/* ------------------------------------------------------------ the download */

/* Each section is announced, sent as a run of scatter messages, and then the
 * chip is told to start it.  The announcement is what tells the chip where the
 * bytes belong; the scatter messages carry no address of their own. */
static bool send_section(mt76_t *c, const mt76_section_t *s, u8 start_command) {
    struct {
        u32 address;
        u32 length;
        u32 mode;
    } request = { s->address, s->length, s->mode };

    if (send_mcu(c, MCU_CMD_TARGET_ADDRESS_LEN_REQ, &request, sizeof request,
                 NULL, 0, 500) < 0) {
        kwarn("mt76", "the chip refused a section at %08x", s->address);
        return false;
    }

    u32 sent = 0;
    while (sent < s->length) {
        u32 chunk = s->length - sent;
        if (chunk > MT76_BUFFER_SIZE - sizeof(mt76_mcu_txd_t))
            chunk = MT76_BUFFER_SIZE - sizeof(mt76_mcu_txd_t);
        if (send_mcu(c, MCU_CMD_FW_SCATTER, s->data + sent, (int)chunk, NULL, 0, 0) < 0)
            return false;
        sent += chunk;
    }

    struct { u32 check_crc; u32 reserved; } start = { 0, 0 };
    return send_mcu(c, start_command, &start, sizeof start, NULL, 0, 1000) >= 0;
}

static bool download(mt76_t *c) {
    wifi_device_t *dev = &c->dev;

    /* The patch is shared: another driver may already have loaded it, and the
     * semaphore is how the chip says so.  Loading it twice does not work. */
    struct { u32 operation; } sem = { PATCH_SEM_GET };
    u8 answer[16];
    int n = send_mcu(c, MCU_CMD_PATCH_SEM_CONTROL, &sem, sizeof sem,
                     answer, sizeof answer, 500);
    u32 state = n >= 4 ? le32(answer) : PATCH_NOT_DL_SEM_SUCCESS;

    if (state == PATCH_IS_DL) {
        kinfo("mt76", "%s: the patch is already loaded", dev->name);
    } else {
        for (int i = 0; i < c->patch.sections; i++)
            if (!send_section(c, &c->patch.section[i], MCU_CMD_PATCH_FINISH_REQ))
                return false;
        kinfo("mt76", "%s: patch \"%s\" version %u, %d section(s)",
              dev->name, c->patch.build_date, c->patch.patch_version,
              c->patch.sections);
    }

    sem.operation = PATCH_SEM_RELEASE;
    send_mcu(c, MCU_CMD_PATCH_SEM_CONTROL, &sem, sizeof sem, NULL, 0, 500);

    /* Then the main image, region by region. */
    for (int i = 0; i < c->firmware.sections; i++)
        if (!send_section(c, &c->firmware.section[i], MCU_CMD_FW_START_REQ))
            return false;

    /* The chip reports which state it is in; anything but the running one
     * means it took the bytes and did not like them. */
    for (int i = 0; i < 500; i++) {
        u32 misc = rd(c, MT_TOP_MISC2);
        if ((misc & MT_TOP_MISC2_FW_STATE_MASK) == MT_FW_STATE_NORMAL_TRX) {
            kinfo("mt76", "%s: firmware \"%s\" running, %d region(s)",
                  dev->name, c->firmware.version, c->firmware.sections);
            c->running = true;
            return true;
        }
        timer_mdelay(1);
    }

    kwarn("mt76", "%s: the firmware was sent but the chip never reported itself "
                  "running", dev->name);
    return false;
}

/* --------------------------------------------------------------- the driver */

static bool mt76_start(wifi_device_t *dev) {
    mt76_t *c = dev->ctx;

    /* A model has no firmware files to be given, so it gets images built to
     * the same two formats.  The whole path then runs. */
    if (c->modelled && !c->parsed) {
        static u8 patch_image[2048], main_image[4096];
        size_t patch_len = mt76_build_test_patch(patch_image, sizeof patch_image);
        size_t main_len = mt76_build_test_firmware(main_image, sizeof main_image);
        if (patch_len && main_len &&
            mt76_parse_patch(patch_image, patch_len, &c->patch) &&
            mt76_parse_firmware(main_image, main_len, &c->firmware)) {
            c->parsed = true;
            kinfo("mt76", "%s: no firmware files, so images built to the same "
                          "formats are being loaded into the model", dev->name);
        }
    }

    if (!c->parsed) {
        if (!dev->firmware_present) {
            kwarn("mt76", "%s: %s is not present, so the radio cannot start",
                  dev->name, dev->firmware_name);
            kwarn("mt76", "%s: `firmware import <path>` copies it in; it is in "
                          "the linux-firmware package", dev->name);
            return false;
        }
        if (!firmware_load(dev->firmware_name, &c->main_fw)) return false;
        if (!mt76_parse_firmware(c->main_fw.data, c->main_fw.size, &c->firmware))
            return false;
        /* The patch travels in a second file beside it, named the same way
         * with "_ram_code" replaced by "_patch". */
        char patch_name[64];
        size_t n = 0;
        const char *from = dev->firmware_name;
        for (size_t i = 0; from[i] && n + 8 < sizeof patch_name; i++) {
            if (from[i] == '_' && !strncmp(from + i, "_ram_code", 9)) {
                memcpy(patch_name + n, "_patch", 6);
                n += 6;
                i += 8;
                continue;
            }
            patch_name[n++] = from[i];
        }
        patch_name[n] = 0;
        if (firmware_present(patch_name, NULL) &&
            firmware_load(patch_name, &c->patch_fw))
            mt76_parse_patch(c->patch_fw.data, c->patch_fw.size, &c->patch);
        c->parsed = true;
    }

    if (!c->regs) {
        kwarn("mt76", "%s: the firmware is readable but the device is not mapped",
              dev->name);
        return false;
    }

    if (!c->rings_ready && !setup_rings(c)) {
        kwarn("mt76", "%s: no memory for the rings", dev->name);
        return false;
    }
    program_rings(c);

    if (!download(c)) return false;

    /* The chip's own address, asked for through the register access command
     * rather than read directly - which is how everything above the firmware
     * reaches the hardware on this family. */
    struct { u32 address; u32 value; } access = { 0x0000a000, 0 };
    u8 answer[16];
    int n = send_mcu(c, MCU_CMD_INIT_ACCESS_REG, &access, sizeof access,
                     answer, sizeof answer, 500);
    if (n >= ETH_ALEN) memcpy(dev->mac.addr, answer, ETH_ALEN);

    dev->radio_up = true;
    char mac[24];
    mac_format(&dev->mac, mac, sizeof mac);
    kinfo("mt76", "%s: the radio is up, %s%s", dev->name, mac,
          c->modelled ? " (model)" : "");
    return true;
}

static void mt76_stop(wifi_device_t *dev) {
    mt76_t *c = dev->ctx;
    if (c->regs) wr(c, MT_WFDMA_GLO_CFG, 0);
    free_rings(c);
    firmware_free(&c->patch_fw);
    firmware_free(&c->main_fw);
    c->parsed = false;
    c->running = false;
    dev->radio_up = false;
}

static bool mt76_set_channel(wifi_device_t *dev, u8 channel) {
    mt76_t *c = dev->ctx;
    dev->channel = channel;
    if (!c->running) return false;

    struct {
        u8  control_channel;
        u8  center_channel;
        u8  bandwidth;
        u8  tx_streams;
        u8  rx_streams;
        u8  band;
        u16 reserved;
    } request = {
        .control_channel = channel,
        .center_channel = channel,
        .bandwidth = 0,
        .tx_streams = 2,
        .rx_streams = 2,
        .band = channel <= 14 ? 0 : 1,
    };
    return send_mcu(c, MCU_EXT_CMD_CHANNEL_SWITCH, &request, sizeof request,
                    NULL, 0, 300) >= 0;
}

static int mt76_transmit(wifi_device_t *dev, const void *frame, int len) {
    mt76_t *c = dev->ctx;
    if (!c->running) return -1;
    if (len <= 0 || len > MT76_BUFFER_SIZE - 32) return -1;

    /* A frame carries a short descriptor of its own in front of it, which is
     * what tells the chip which queue and which rate to use. */
    u8 message[MT76_BUFFER_SIZE];
    memset(message, 0, 32);
    message[0] = (u8)len;
    message[1] = (u8)(len >> 8);
    message[2] = 0;                     /* the queue */
    message[3] = c->sequence++;
    memcpy(message + 32, frame, (size_t)len);

    if (!enqueue(c, MT_TX_RING_DATA, message, 32 + len)) return -1;
    return len;
}

static void mt76_poll(wifi_device_t *dev) {
    mt76_t *c = dev->ctx;
    if (!c->rings_ready) return;
    drain_rx(c, MT_RX_RING_EVENT);
    drain_rx(c, MT_RX_RING_DATA);
}

static const wifi_driver_t mt76_driver_ops = {
    "mt76", mt76_start, mt76_stop, mt76_set_channel,
    mt76_transmit, mt76_poll, NULL,
};

/* ------------------------------------------------------------- bringing up */

static bool bring_up(mt76_t *c, volatile u8 *regs, u16 pci_device, bool modelled) {
    memset(c, 0, sizeof *c);
    c->regs = regs;
    c->modelled = modelled;

    wifi_device_t *dev = &c->dev;
    if (!wifi_identify(0x14C3, pci_device, dev)) return false;

    firmware_declare(dev->firmware_name, "the linux-firmware package");
    dev->firmware_present = firmware_present(dev->firmware_name, NULL);

    snprintf(dev->name, sizeof dev->name, "wlan%d", wifi_count());
    dev->driver = &mt76_driver_ops;
    dev->ctx = c;

    wifi_register(dev);
    ieee80211_attach(dev);
    mt76_start(dev);
    return true;
}

void mt76_init(void) {
    pci_dev_t *pci = NULL;

    while ((pci = pci_find(0x02, 0x80, 0xFF, pci)) != NULL) {
        if (pci->vendor != 0x14C3) continue;
        if (card_count >= MAX_CARDS) break;

        volatile u8 *regs = NULL;
        if (pci->bar[0] && !pci->bar_is_io[0]) {
            pci_enable_memory(pci);
            pci_enable_bus_master(pci);
            size_t len = pci->bar_size[0] ? (size_t)pci->bar_size[0] : 0x100000;
            if (len > 0x200000) len = 0x200000;
            regs = vmm_map_mmio(pci->bar[0], len);
        }

        if (bring_up(&cards[card_count], regs, pci->device, false)) {
            pci_claim(pci, "mt76");
            card_count++;
        }
    }
}

volatile u8 *mt76_model_attach(void);

bool mt76_attach_model(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return cards[i].dev.radio_up;
    if (card_count >= MAX_CARDS) return false;

    volatile u8 *regs = mt76_model_attach();
    if (!regs) return false;

    int before = card_count;
    if (bring_up(&cards[card_count], regs, 0x7961, true)) card_count++;
    return card_count > before;
}

wifi_device_t *mt76_model_device(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return &cards[i].dev;
    return NULL;
}

/* ------------------------------------------------------- images to test with */

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void put_le32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

size_t mt76_build_test_patch(u8 *out, size_t cap) {
    if (cap < 1024) return 0;
    memset(out, 0, cap);

    memcpy(out, "20260826123000", 14);
    memcpy(out + 16, "MT79", 4);
    put_be32(out + 20, 0x79610001);
    put_be32(out + 24, 0x00000005);
    put_be32(out + 32, 2);                       /* two sections */

    size_t table = MT76_PATCH_HEADER_SIZE;
    size_t data = table + 2 * 16;

    put_be32(out + table, (u32)data);            /* offset */
    put_be32(out + table + 4, 0x00100000);       /* where it goes */
    put_be32(out + table + 8, 128);              /* how long */
    put_be32(out + table + 12, 0);

    put_be32(out + table + 16, (u32)(data + 128));
    put_be32(out + table + 20, 0x00110000);
    put_be32(out + table + 24, 64);
    put_be32(out + table + 28, 0);

    for (size_t i = 0; i < 192; i++) out[data + i] = (u8)(i * 5 + 1);
    return data + 192;
}

size_t mt76_build_test_firmware(u8 *out, size_t cap) {
    if (cap < 2048) return 0;
    memset(out, 0, cap);

    /* Two regions, their bytes first, then the table, then the trailer. */
    size_t body = 600;
    for (size_t i = 0; i < body; i++) out[i] = (u8)(i * 11 + 7);

    u8 *region = out + body;
    put_le32(region + 16, 0x00200000);
    put_le32(region + 20, 400);
    put_le32(region + MT76_REGION_SIZE + 16, 0x00210000);
    put_le32(region + MT76_REGION_SIZE + 20, 200);

    u8 *trailer = region + 2 * MT76_REGION_SIZE;
    trailer[0] = 0x61;                           /* chip id */
    trailer[1] = 0;
    trailer[2] = 2;                              /* two regions */
    trailer[3] = 1;
    memcpy(trailer + 7, "1.5.0.0\0\0\0", 10);
    memcpy(trailer + 17, "20260826123000", 14);

    return body + 2 * MT76_REGION_SIZE + MT76_TRAILER_SIZE;
}

/* ------------------------------------------------------------------- tests */

bool      mt76_model_firmware_running(void);
int       mt76_model_sections(void);
int       mt76_model_scatter_messages(void);
u32       mt76_model_last_address(void);
u32       mt76_model_last_length(void);
const u8 *mt76_model_last_section(void);

/* Drive the driver against a model of the hardware: both container formats,
 * the rings, the download sequence and a scan.
 *
 * Returns the number of checks that failed. */
int mt76_drive_test(void) {
    int failures = 0;

    /* The two formats first, on their own.  They are the part most likely to
     * be misread - one is big-endian, the other keeps its table at the end -
     * and a misread produces a chip that takes everything and does nothing. */
    static u8 patch_image[2048], main_image[4096];
    size_t patch_len = mt76_build_test_patch(patch_image, sizeof patch_image);
    size_t main_len = mt76_build_test_firmware(main_image, sizeof main_image);

    mt76_patch_t patch;
    if (!mt76_parse_patch(patch_image, patch_len, &patch)) {
        kerr("mt76-test", "a well-formed patch was refused");
        failures++;
    } else if (patch.sections != 2 || patch.section[0].address != 0x00100000 ||
               patch.section[0].length != 128 || patch.section[1].length != 64) {
        kerr("mt76-test", "the patch's sections came out wrong: %d section(s), "
                          "first at %08x for %u bytes",
             patch.sections, patch.section[0].address, patch.section[0].length);
        failures++;
    } else if (patch.patch_version != 5) {
        kerr("mt76-test", "the patch version came out as %u, expected 5 - the "
                          "header's fields are the other way round",
             patch.patch_version);
        failures++;
    }

    mt76_firmware_t firmware;
    if (!mt76_parse_firmware(main_image, main_len, &firmware)) {
        kerr("mt76-test", "a well-formed firmware image was refused");
        failures++;
    } else if (firmware.sections != 2 || firmware.section[0].address != 0x00200000 ||
               firmware.section[0].length != 400 || firmware.section[1].length != 200) {
        kerr("mt76-test", "the firmware's regions came out wrong: %d region(s)",
             firmware.sections);
        failures++;
    } else if (strcmp(firmware.version, "1.5.0.0")) {
        kerr("mt76-test", "the firmware version came out as \"%s\"", firmware.version);
        failures++;
    }

    /* A trailer claiming more regions than the file can hold has to be
     * refused: it is what a truncated download looks like. */
    static u8 truncated[512];
    memcpy(truncated, main_image, sizeof truncated);
    truncated[sizeof truncated - 36 + 2] = 200;      /* two hundred regions */
    if (mt76_parse_firmware(truncated, sizeof truncated, &firmware)) {
        kerr("mt76-test", "a firmware image claiming impossible regions was "
                          "accepted");
        failures++;
    }

    static const u8 nonsense[8] = { 0xDE, 0xAD };
    if (mt76_parse_patch(nonsense, sizeof nonsense, &patch)) {
        kerr("mt76-test", "a file that is not a patch was accepted");
        failures++;
    }

    if (!mt76_attach_model()) {
        kwarn("mt76-test", "the MediaTek model could not be brought up");
        return failures + 1;
    }

    wifi_device_t *dev = mt76_model_device();
    if (!dev || !dev->radio_up) {
        kerr("mt76-test", "the radio did not come up");
        return failures + 1;
    }

    /* Four sections in all - two of patch and two of firmware - each announced
     * and then scattered.  The last one is compared byte for byte with what it
     * was built from. */
    if (mt76_model_sections() != 4) {
        kerr("mt76-test", "%d section(s) completed, expected 4",
             mt76_model_sections());
        failures++;
    } else if (mt76_model_last_address() != 0x00210000 ||
               mt76_model_last_length() != 200) {
        kerr("mt76-test", "the last region arrived as %u bytes at %08x",
             mt76_model_last_length(), mt76_model_last_address());
        failures++;
    } else if (memcmp(mt76_model_last_section(), main_image + 400, 200)) {
        kerr("mt76-test", "the last region arrived altered");
        failures++;
    } else {
        kinfo("mt76-test", "4 section(s) across %d scatter message(s), the last "
                           "one byte for byte as sent",
              mt76_model_scatter_messages());
    }

    static const u8 expected_mac[ETH_ALEN] = { 0x00, 0x0c, 0xe7, 0x77, 0x88, 0x99 };
    if (memcmp(dev->mac.addr, expected_mac, ETH_ALEN)) {
        char got[24];
        mac_format(&dev->mac, got, sizeof got);
        kerr("mt76-test", "the chip's address came back as %s", got);
        failures++;
    }

    wifi_scan(dev, 2000);

    const wifi_network_t *found = NULL;
    for (int i = 0; i < dev->scan_count; i++)
        if (!strcmp(dev->scan[i].ssid, "kestrel-test")) found = &dev->scan[i];

    if (!found) {
        kerr("mt76-test", "the scan found nothing through the MediaTek driver");
        failures++;
    } else if (found->security != WIFI_SECURITY_WPA2) {
        kerr("mt76-test", "the security came back as %s",
             wifi_security_name(found->security));
        failures++;
    } else {
        kinfo("mt76-test", "a probe request went out through the data ring and "
                           "\"%s\" came back through the event ring", found->ssid);
    }

    if (!failures)
        kinfo("mt76-test", "the MediaTek driver's containers, rings and "
                           "download sequence are all correct");
    return failures;
}
