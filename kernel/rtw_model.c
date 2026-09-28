/* rtw_model.c - a model of a Realtek Wi-Fi chip, and of its firmware.
 *
 * It answers the registers the Realtek driver writes, accepts the firmware
 * being paged into it, and then plays the part of that firmware: reading the
 * mailboxes, acting on what it finds, and answering through the event
 * register.
 *
 * The driver that runs against it is the same driver that would run against a
 * card.  Nothing in it is switched off for the model.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: the firmware header is read correctly, every page of
 * the image reaches the chip in the right order with the right contents, the
 * processor is held and released around the transfer, the mailbox protocol
 * works including the busy bits, and a frame goes out and comes back.
 *
 * What it cannot establish: that the register offsets match Realtek's silicon.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "time.h"
#include "klog.h"
#include "net.h"
#include "wifi.h"
#include "rtw.h"

#define MODEL_SSID    "kestrel-test"
#define MODEL_CHANNEL 6

static const u8 model_mac[ETH_ALEN] = { 0x00, 0xe0, 0x4c, 0x11, 0x22, 0x33 };
static const u8 model_bssid[ETH_ALEN] = { 0x02, 0x00, 0x5e, 0xaa, 0xbb, 0xcc };

/* The register window.  A Realtek chip's is small; the transmit buffer at the
 * far end of it is what decides the size. */
#define REG_BYTES 0x8800   /* the firmware window at 0x1000, the transmit
                            * window at 0x8000, and everything below them */
static u8 register_space[REG_BYTES] __attribute__((aligned(4096)));

typedef struct {
    bool present;
    bool cpu_held;
    bool download_enabled;
    bool firmware_running;

    /* What arrived, and whether it arrived intact.  The image is kept rather
     * than summarised, because "the pages are all there but two of them are
     * swapped" is exactly the failure worth catching. */
    u8   loaded[RTW_FW_PAGE_SIZE * 8];
    int  highest_page;
    bool saw_any_page;

    u8   channel;
    bool answered_probe;
    bool key_installed;

    /* What the driver last left in each mailbox. */
    u32  box_low[4], box_high[4];
} model_t;

static model_t model;

static u8  reg8(u32 off)  { return off < REG_BYTES ? register_space[off] : 0xFF; }
static u16 reg16(u32 off) { return off + 2 <= REG_BYTES ? *(volatile u16 *)(register_space + off) : 0xFFFF; }
static u32 reg32(u32 off) { return off + 4 <= REG_BYTES ? *(volatile u32 *)(register_space + off) : 0xFFFFFFFFu; }
static void set8(u32 off, u8 v)   { if (off < REG_BYTES) register_space[off] = v; }
static void set16(u32 off, u16 v) { if (off + 2 <= REG_BYTES) *(volatile u16 *)(register_space + off) = v; }
static void set32(u32 off, u32 v) { if (off + 4 <= REG_BYTES) *(volatile u32 *)(register_space + off) = v; }

/* --------------------------------------------------------- what it says back */

/* There is one event buffer and the firmware may have several things to say -
 * a transmit report and then a frame that arrived because of it - so they
 * queue behind it and go out one at a time as the host empties it.  A real
 * chip does the same; a model that dropped the second would hide exactly the
 * bug that causes. */
#define PENDING_MAX 8

typedef struct { u8 id; u8 length; u8 body[RTW_C2H_MAX]; } pending_t;
static pending_t pending[PENDING_MAX];
static int pending_head, pending_tail;

static void queue_event(u8 id, const u8 *body, int length) {
    int next = (pending_tail + 1) % PENDING_MAX;
    if (next == pending_head) return;              /* full */
    if (length > RTW_C2H_MAX) length = RTW_C2H_MAX;
    pending[pending_tail].id = id;
    pending[pending_tail].length = (u8)length;
    if (body && length) memcpy(pending[pending_tail].body, body, (size_t)length);
    pending_tail = next;
}

/* Move the next queued message into the buffer, if the host has finished with
 * the last one. */
static void push_event(void) {
    if (pending_head == pending_tail) return;
    if (reg8(REG_C2HEVT_CLEAR) == C2H_EVT_READY) return;   /* still full */

    pending_t *e = &pending[pending_head];
    pending_head = (pending_head + 1) % PENDING_MAX;

    set8(REG_C2HEVT_MSG, e->id);
    set8(REG_C2HEVT_MSG + 1, e->length);
    for (int i = 0; i < e->length; i++) set8(REG_C2HEVT_MSG + 2 + (u32)i, e->body[i]);
    set8(REG_C2HEVT_CLEAR, C2H_EVT_READY);
}

static void deliver(u8 id, const u8 *body, int length) {
    queue_event(id, body, length);
    push_event();
}

/* ------------------------------------------------------------ 802.11 frames */

static void put16le(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

void rtw_model_deliver_frame(const u8 *frame, int len, s8 signal);

/* The frame the driver will read out of the window when it takes the event
 * announcing it.  It waits here until that event reaches the front of the
 * queue, so nothing queued in between can overwrite the window. */
static u8  waiting_frame[RTW_TX_WINDOW];
static int waiting_frame_len;

/* A probe response.  It has to fit the event buffer, which is small - so this
 * sends the part a scan actually reads: the header, the fixed fields, the
 * name and the security element, with the optional elements left out. */
static void answer_probe(void) {
    u8 frame[96];
    memset(frame, 0, sizeof frame);

    put16le(frame, (u16)(0 << 2 | 5 << 4));       /* management, probe response */
    memset(frame + 4, 0xFF, ETH_ALEN);
    memcpy(frame + 10, model_bssid, ETH_ALEN);
    memcpy(frame + 16, model_bssid, ETH_ALEN);

    u8 *p = frame + 24;
    memset(p, 0, 8); p += 8;
    put16le(p, 100); p += 2;
    put16le(p, 0x0011); p += 2;                   /* privacy is on */

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

    /* The event buffer is far smaller than a frame, so this is where a real
     * chip would use its receive ring instead.  The model puts the frame in
     * host-visible memory the same way and reports its length. */
    rtw_model_deliver_frame(frame, (int)(p - frame), -47);
}

/* A frame is longer than the event buffer, so it comes up the same way it goes
 * down: through the window, with an event saying how much is there. */
void rtw_model_deliver_frame(const u8 *frame, int len, s8 signal) {
    if (len <= 0 || len > RTW_TX_WINDOW) return;

    /* Held until its event reaches the front of the queue. */
    memcpy(waiting_frame, frame, (size_t)len);
    waiting_frame_len = len;

    u8 body[4];
    body[0] = (u8)-signal;
    body[1] = (u8)len;
    body[2] = (u8)(len >> 8);
    body[3] = 0;
    deliver(C2H_MSG_RX_FRAME, body, 4);
}



/* ------------------------------------------------------------- the mailbox */

static void handle_command(const u8 *message) {
    u8 cmd = message[0];
    const u8 *payload = message + 1;

    switch (cmd) {
    case H2C_SET_MEDIA_STATUS:
        break;

    case H2C_SET_CHANNEL:
        model.channel = payload[0];
        break;

    case H2C_SET_KEY:
        model.key_installed = true;
        break;

    case H2C_TRANSMIT: {
        int len = payload[0] | (payload[1] << 8);
        if (len <= 0 || len > RTW_TX_WINDOW) break;

        /* Read the frame back out of the window the driver wrote it into -
         * which is the check that it got there. */
        u8 frame[RTW_TX_WINDOW];
        for (int i = 0; i < len; i++) frame[i] = reg8(REG_TX_BUFFER + (u32)i);

        u16 control = (u16)(frame[0] | (frame[1] << 8));
        u8 type = (u8)((control >> 2) & 3);
        u8 subtype = (u8)((control >> 4) & 0xF);

        u8 report[2] = { payload[2], 1 };
        deliver(C2H_MSG_TX_REPORT, report, 2);

        if (type == 0 && subtype == 4) {          /* a probe request */
            model.answered_probe = true;
            answer_probe();
        }
        break;
    }

    default:
        kdebug("rtw-model", "command %02x", cmd);
        break;
    }
}

/* ------------------------------------------------------------ watching the bus */

/* The window's contents belong to whichever page the driver last named, so
 * copying them across on every observation puts each page where it belongs
 * however often the model happens to look.  A chip does this continuously;
 * doing it on observation is the same thing at a coarser grain, and it means
 * the model never has to guess when a page finished. */
static void absorb_window(void) {
    u8 page = (u8)(reg8(REG_MCUFWDL + 2) & 0x07);
    if ((u32)(page + 1) * RTW_FW_PAGE_SIZE > sizeof model.loaded) return;

    for (u32 i = 0; i < RTW_FW_PAGE_SIZE; i++)
        model.loaded[page * RTW_FW_PAGE_SIZE + i] = reg8(REG_FW_START_ADDRESS + i);

    if (page > model.highest_page) model.highest_page = page;
    model.saw_any_page = true;
}

static u16 previous_func_en;
static u32 previous_fwdl;
static u8  previous_clear;

void rtw_model_sync(void) {
    if (!model.present) return;

    /* The processor being held and released around the transfer. */
    u16 func = reg16(REG_SYS_FUNC_EN);
    if (func != previous_func_en) {
        bool was_held = model.cpu_held;
        previous_func_en = func;
        model.cpu_held = !(func & FEN_CPUEN);

        if (was_held && !model.cpu_held && model.saw_any_page) {
            /* Released with an image in it: the firmware starts. */
            model.firmware_running = true;
            set32(REG_MCUFWDL, reg32(REG_MCUFWDL) | WINTINI_RDY);
            for (int i = 0; i < ETH_ALEN; i++) set8(REG_MACID + (u32)i, model_mac[i]);
            kinfo("rtw-model", "the firmware started; %d page(s) arrived",
                  model.highest_page + 1);
        }
    }

    /* The download path being switched on and off. */
    u32 fwdl = reg32(REG_MCUFWDL);
    if (fwdl != previous_fwdl) {
        bool was_enabled = model.download_enabled;
        previous_fwdl = fwdl;
        model.download_enabled = (fwdl & MCUFWDL_EN) != 0;
        if (!was_enabled && model.download_enabled) {
            memset(model.loaded, 0, sizeof model.loaded);
            model.highest_page = 0;
            model.saw_any_page = false;
        }
        if (was_enabled && !model.download_enabled) {
            /* The last page is still in the window when the path is switched
             * off. */
            absorb_window();
        }
    }

    /* Whatever is in the window belongs to the page the driver last named. */
    if (model.download_enabled) absorb_window();

    /* An event buffer the driver has finished with: the next queued message
     * can go into it. */
    if (reg8(REG_C2HEVT_CLEAR) != C2H_EVT_READY) {
        if (waiting_frame_len && pending_head != pending_tail &&
            pending[pending_head].id == C2H_MSG_RX_FRAME) {
            /* The frame goes into the window just before the event announcing
             * it, so nothing queued in between can overwrite it. */
            for (int i = 0; i < waiting_frame_len; i++)
                set8(REG_TX_BUFFER + (u32)i, waiting_frame[i]);
            waiting_frame_len = 0;
        }
        push_event();
    }

    /* The mailboxes.  Writing the low half is the doorbell, so a change there
     * is a command; the extension half was already in place. */
    for (int box = 0; box < 4; box++) {
        u32 low = reg32(REG_HMEBOX0 + (u32)box * 4);
        u32 high = reg32(REG_HMEBOX0_EXT + (u32)box * 4);
        if (low == model.box_low[box] && high == model.box_high[box]) continue;
        model.box_low[box] = low;
        model.box_high[box] = high;
        if (!low) continue;

        u8 message[RTW_H2C_BOX_SIZE];
        for (int i = 0; i < 4; i++) message[i] = (u8)(low >> (8 * i));
        for (int i = 0; i < 4; i++) message[4 + i] = (u8)(high >> (8 * i));

        if (model.firmware_running) {
            /* Busy while it is being read, which is what the driver waits
             * on. */
            set16(REG_HMETFR, (u16)(reg16(REG_HMETFR) | (1u << box)));
            handle_command(message);
            set16(REG_HMETFR, (u16)(reg16(REG_HMETFR) & ~(1u << box)));
        }
    }
}

/* ------------------------------------------------------------- bringing up */

volatile u8 *rtw_model_attach(void) {
    if (model.present) return register_space;

    memset(register_space, 0, sizeof register_space);
    memset(&model, 0, sizeof model);
    model.present = true;
    model.cpu_held = false;
    model.channel = MODEL_CHANNEL;

    previous_func_en = 0;
    previous_fwdl = 0;
    previous_clear = 0;

    /* The processor is on at power-up, which is why the driver holds it. */
    set16(REG_SYS_FUNC_EN, FEN_CPUEN);
    previous_func_en = FEN_CPUEN;

    kinfo("rtw-model", "no Realtek card present; a model is standing in so the "
                       "driver runs");
    return register_space;
}

bool rtw_model_firmware_running(void) { return model.firmware_running; }
int  rtw_model_pages(void) { return model.saw_any_page ? model.highest_page + 1 : 0; }
bool rtw_model_key_installed(void) { return model.key_installed; }
u8   rtw_model_channel(void) { return model.channel; }

/* What the chip actually ended up holding, so the caller can compare it with
 * what it meant to send. */
const u8 *rtw_model_image(void) { return model.loaded; }
