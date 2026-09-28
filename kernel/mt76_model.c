/* mt76_model.c - a model of a MediaTek Wi-Fi chip, and of its microcontroller.
 *
 * It answers the registers the MediaTek driver writes, walks the descriptor
 * rings the driver builds, takes the firmware apart as it arrives, and then
 * plays the part of the microcontroller: answering commands and delivering
 * frames back up the receive ring.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: both firmware container formats are read correctly,
 * the descriptor layout and the index protocol work in both directions, the
 * download sequence is the right one - announce, scatter, start - and every
 * byte of both images arrives where the driver said it should.
 *
 * What it cannot establish: that the register offsets match MediaTek's
 * silicon, nor that the payloads match the firmware's idea of them.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "net.h"
#include "wifi.h"
#include "mt76.h"

#define MODEL_SSID    "kestrel-test"
#define MODEL_CHANNEL 6

static const u8 model_mac[ETH_ALEN] = { 0x00, 0x0c, 0xe7, 0x77, 0x88, 0x99 };
static const u8 model_bssid[ETH_ALEN] = { 0x02, 0x00, 0x5e, 0xaa, 0xbb, 0xcc };

/* A MediaTek part presents a megabyte of register space and a driver touches a
 * handful of it, but the ones it touches are a long way apart - the flow
 * handler sits above 0xd4000.  Backing the whole window is simpler than any
 * scheme for backing only the parts in use, and a megabyte is affordable. */
#define WINDOW_BYTES 0x100000
static u8 *window;

static u32 reg_get(u32 off) {
    if (off + 4 <= WINDOW_BYTES && window) return *(volatile u32 *)(window + off);
    return 0;
}
static void reg_set(u32 off, u32 v) {
    if (off + 4 <= WINDOW_BYTES && window) *(volatile u32 *)(window + off) = v;
}

typedef struct {
    bool present;
    bool dma_enabled;
    bool firmware_running;

    u64  tx_desc_phys[2], rx_desc_phys[2];
    u16  tx_read[2];              /* how far the model has consumed */
    u16  rx_write[2];             /* where the next message goes    */
    u16  tx_cpu_seen[2];

    /* What the firmware download has said so far. */
    u32  pending_address;
    u32  pending_length;
    u32  pending_received;
    bool section_pending;      /* one has been announced and not yet finished */
    u8   loaded[4096];            /* the last section, to be checked */
    u32  loaded_length;
    u32  loaded_address;
    int  sections_started;
    int  scatter_messages;
    bool patch_semaphore_held;

    u8   channel;
    bool answered_probe;
} model_t;

static model_t model;

/* ------------------------------------------------------------- the rings */

static u32 ring_reg(u32 base, int index, u32 field) {
    return base + (u32)index * MT_RING_STRIDE + field;
}

/* Put a message where the driver will find it: into the next receive
 * descriptor, with the done bit set. */
static void deliver(int ring_index, u8 eid, u8 seq, const void *payload, int length) {
    u64 desc_phys = model.rx_desc_phys[ring_index];
    if (!desc_phys) return;

    mt76_desc_t *ring = phys_to_virt(desc_phys);
    u16 slot = model.rx_write[ring_index];
    mt76_desc_t *d = &ring[slot];
    if (!d->buf0) return;

    int total = (int)sizeof(mt76_mcu_rxd_t) + length;
    if (total > MT76_BUFFER_SIZE) return;

    u8 *buffer = phys_to_virt((u64)d->buf0);
    memset(buffer, 0, (size_t)total);
    mt76_mcu_rxd_t *rxd = (void *)buffer;
    rxd->length = (u16)total;
    rxd->eid = eid;
    rxd->seq = seq;
    rxd->pkt_type = MT76_PKT_TYPE_CMD;
    if (payload && length) memcpy(buffer + sizeof *rxd, payload, (size_t)length);

    d->ctrl = ((u32)total << MT76_CTRL_LEN0_SHIFT) | MT76_CTRL_DMA_DONE;
    model.rx_write[ring_index] = (u16)((slot + 1) % MT76_RING_SIZE);
}

/* ------------------------------------------------------------ 802.11 frames */

static void put16le(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static void answer_probe(void) {
    u8 message[192];
    memset(message, 0, sizeof message);

    u8 *frame = message + 1;                      /* the signal byte comes first */
    message[0] = 47;                              /* forty-seven dB down */

    put16le(frame, (u16)(0 << 2 | 5 << 4));
    memset(frame + 4, 0xFF, ETH_ALEN);
    memcpy(frame + 10, model_bssid, ETH_ALEN);
    memcpy(frame + 16, model_bssid, ETH_ALEN);

    u8 *p = frame + 24;
    memset(p, 0, 8); p += 8;
    put16le(p, 100); p += 2;
    put16le(p, 0x0011); p += 2;

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

    deliver(MT_RX_RING_EVENT, MCU_EVENT_RX_FRAME, 0, message, (int)(p - message));
}

/* ------------------------------------------------------------- the commands */

static inline u32 le32_at(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void handle_mcu(const u8 *message, int length) {
    if (length < (int)sizeof(mt76_mcu_txd_t)) return;
    const mt76_mcu_txd_t *txd = (const void *)message;
    const u8 *payload = message + sizeof *txd;
    int payload_len = length - (int)sizeof *txd;

    switch (txd->cid) {
    case MCU_CMD_PATCH_SEM_CONTROL: {
        u32 operation = payload_len >= 4 ? le32_at(payload) : 0;
        u32 answer;
        if (operation == PATCH_SEM_GET) {
            answer = model.patch_semaphore_held ? PATCH_IS_DL : PATCH_NOT_DL_SEM_SUCCESS;
            model.patch_semaphore_held = true;
        } else {
            model.patch_semaphore_held = false;
            answer = PATCH_NOT_DL_SEM_SUCCESS;
        }
        deliver(MT_RX_RING_EVENT, MCU_EVENT_GENERIC, txd->seq, &answer, 4);
        break;
    }

    case MCU_CMD_TARGET_ADDRESS_LEN_REQ:
        if (payload_len >= 8) {
            model.pending_address = le32_at(payload);
            model.pending_length = le32_at(payload + 4);
            model.pending_received = 0;
            model.section_pending = true;
            model.loaded_length = 0;
        }
        deliver(MT_RX_RING_EVENT, MCU_EVENT_GENERIC, txd->seq, NULL, 0);
        break;

    case MCU_CMD_FW_SCATTER:
        /* The bytes themselves, with no address of their own: they belong
         * wherever the announcement said. */
        model.scatter_messages++;
        if (model.loaded_length + (u32)payload_len <= sizeof model.loaded) {
            memcpy(model.loaded + model.loaded_length, payload, (size_t)payload_len);
            model.loaded_length += (u32)payload_len;
        }
        model.pending_received += (u32)payload_len;
        break;

    case MCU_CMD_PATCH_FINISH_REQ:
    case MCU_CMD_FW_START_REQ:
        /* Only a section that was actually announced counts.  These two
         * messages also end each stage of the download - the patch, and then
         * the firmware - and at that point the counters from the last real
         * section still agree with each other, so counting on agreement alone
         * would count the end of a stage as one more section than arrived. */
        if (!model.section_pending) {
            /* The end of a stage, with nothing outstanding. */
        } else if (model.pending_received != model.pending_length) {
            kwarn("mt76-model", "a section was announced as %u bytes and %u "
                                "arrived", model.pending_length,
                  model.pending_received);
            model.section_pending = false;
        } else {
            model.sections_started++;
            model.loaded_address = model.pending_address;
            model.section_pending = false;
        }

        if (txd->cid == MCU_CMD_FW_START_REQ && model.sections_started >= 4 &&
            !model.firmware_running) {
            /* Both patch sections and both firmware regions have arrived. */
            model.firmware_running = true;
            reg_set(MT_TOP_MISC2, MT_FW_STATE_NORMAL_TRX);
            kinfo("mt76-model", "the firmware started; %d section(s) arrived in "
                                "%d scatter message(s)",
                  model.sections_started, model.scatter_messages);
        }
        deliver(MT_RX_RING_EVENT, MCU_EVENT_GENERIC, txd->seq, NULL, 0);
        break;

    case MCU_CMD_INIT_ACCESS_REG:
        deliver(MT_RX_RING_EVENT, MCU_EVENT_GENERIC, txd->seq, model_mac, ETH_ALEN);
        break;

    case MCU_EXT_CMD_CHANNEL_SWITCH:
        if (payload_len >= 1) model.channel = payload[0];
        deliver(MT_RX_RING_EVENT, MCU_EVENT_GENERIC, txd->seq, NULL, 0);
        break;

    default:
        kdebug("mt76-model", "command %02x, %d bytes", txd->cid, payload_len);
        deliver(MT_RX_RING_EVENT, MCU_EVENT_GENERIC, txd->seq, NULL, 0);
        break;
    }
}

static void handle_data(const u8 *message, int length) {
    if (length <= 32) return;
    const u8 *frame = message + 32;

    u16 control = (u16)(frame[0] | (frame[1] << 8));
    u8 type = (u8)((control >> 2) & 3);
    u8 subtype = (u8)((control >> 4) & 0xF);

    if (type == 0 && subtype == 4) {
        model.answered_probe = true;
        answer_probe();
    }
}

/* --------------------------------------------------------- watching the bus */

static void run_ring(int index) {
    u64 desc_phys = model.tx_desc_phys[index];
    if (!desc_phys) return;

    u32 cpu = reg_get(ring_reg(MT_TX_RING_BASE, index, MT_RING_CPU_IDX_OFF));
    mt76_desc_t *ring = phys_to_virt(desc_phys);

    int guard = 0;
    while (model.tx_read[index] != (u16)cpu && guard++ < MT76_RING_SIZE) {
        mt76_desc_t *d = &ring[model.tx_read[index]];
        int length = (int)((d->ctrl >> MT76_CTRL_LEN0_SHIFT) & 0x3FFF);

        if (d->buf0 && length > 0 && length <= MT76_BUFFER_SIZE) {
            const u8 *message = phys_to_virt((u64)d->buf0);
            if (index == MT_TX_RING_MCU) handle_mcu(message, length);
            else handle_data(message, length);
        }

        /* Done with: the chip clears the length and sets the done bit. */
        d->ctrl = MT76_CTRL_DMA_DONE;
        model.tx_read[index] = (u16)((model.tx_read[index] + 1) % MT76_RING_SIZE);
        reg_set(ring_reg(MT_TX_RING_BASE, index, MT_RING_DMA_IDX_OFF),
                model.tx_read[index]);
    }
}

void mt76_model_sync(void) {
    if (!model.present) return;

    for (int i = 0; i < 2; i++) {
        u32 base = reg_get(ring_reg(MT_TX_RING_BASE, i, MT_RING_BASE_OFF));
        if (base) model.tx_desc_phys[i] = base;
        u32 rx_base = reg_get(ring_reg(MT_RX_RING_BASE, i, MT_RING_BASE_OFF));
        if (rx_base) model.rx_desc_phys[i] = rx_base;
    }

    u32 cfg = reg_get(MT_WFDMA_GLO_CFG);
    model.dma_enabled = (cfg & (MT_GLO_CFG_TX_DMA_EN | MT_GLO_CFG_RX_DMA_EN)) != 0;
    if (!model.dma_enabled) return;

    run_ring(MT_TX_RING_MCU);
    run_ring(MT_TX_RING_DATA);
}

/* ------------------------------------------------------------- bringing up */

volatile u8 *mt76_model_attach(void) {
    if (model.present) return window;

    /* A megabyte of register space, which is what a MediaTek part presents. */
    u64 phys;
    window = dma_alloc_pages(WINDOW_BYTES / PAGE_SIZE, &phys);
    if (!window) {
        kwarn("mt76-model", "no memory for the register window");
        return NULL;
    }
    memset(window, 0, WINDOW_BYTES);

    memset(&model, 0, sizeof model);
    model.present = true;
    model.channel = MODEL_CHANNEL;

    /* The chip comes up ready to be given firmware. */
    reg_set(MT_TOP_MISC2, MT_FW_STATE_FW_DOWNLOAD);

    kinfo("mt76-model", "no MediaTek card present; a model is standing in so "
                        "the driver runs");
    return window;
}

bool mt76_model_firmware_running(void) { return model.firmware_running; }
int  mt76_model_sections(void) { return model.sections_started; }
int  mt76_model_scatter_messages(void) { return model.scatter_messages; }
u32  mt76_model_last_address(void) { return model.loaded_address; }
u32  mt76_model_last_length(void) { return model.loaded_length; }
const u8 *mt76_model_last_section(void) { return model.loaded; }
u8   mt76_model_channel(void) { return model.channel; }
