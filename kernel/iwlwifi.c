/* iwlwifi.c - Intel wireless.
 *
 * An Intel Wi-Fi chip comes up with nothing in it.  Everything it does is done
 * by microcode that the driver has to push across first, and from then on the
 * driver talks to that microcode rather than to the radio.  So this file is
 * mostly about the handover: recognising the firmware file, taking it apart,
 * preparing the device, writing the sections into its memory, and letting it
 * run.
 *
 * The firmware itself is Intel's and they do not permit anyone else to
 * redistribute it - which is why no operating system ships it, Linux included.
 * What an operating system provides is this code and somewhere to look for the
 * file; the user supplies the file.  `firmware import` is that.
 *
 * Once the microcode is running the conversation changes shape: instead of
 * registers, the driver and the microcode exchange messages through two rings
 * in host memory that the device reads and writes by DMA.  That half is here
 * too - the queues, the handshake that says the microcode has started, the
 * command path, and the receive path that dispatches what comes back.
 *
 * ---------------------------------------------------------------------------
 * On testing.  The container format, the ring mechanics, the handshake and the
 * command path are all exercised on every run of the self-test, against a model
 * of the device that plays the part of the microcode: it reads the descriptors
 * this driver builds, follows them into host memory, and answers through the
 * receive ring.  The driver code that runs against it is the same code that
 * would run against a card.
 *
 * What that cannot establish is that the register offsets match Intel's
 * silicon, or that the payload of each command matches the firmware's idea of
 * it - those structures are versioned by the firmware API and change between
 * releases.  The ring mechanics do not; they have been the same shape across
 * every generation of the family, which is why they are worth getting right
 * even without a card to try them on.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "firmware.h"
#include "wifi.h"
#include "iwl_cmd.h"
#include "net.h"

void ieee80211_attach(wifi_device_t *dev);
void ieee80211_receive(wifi_device_t *dev, u8 *frame, int len, s8 signal_dbm);

/* ------------------------------------------------------- the container
 *
 * A .ucode file is a small header followed by a run of type-length-value
 * records.  Most carry metadata; a few carry the actual microcode, each with
 * the address inside the device it has to be written to.
 */

#define IWL_TLV_UCODE_MAGIC 0x0a4c5749u      /* "IWL\n" */

typedef struct {
    u32 zero;              /* zero, which is how this format is recognised */
    u32 magic;
    u8  human_readable[64];
    u32 version;
    u32 build;
    u64 ignore;
    /* the records follow */
} __attribute__((packed)) iwl_ucode_header_t;

typedef struct {
    u32 type;
    u32 length;
    /* `length` bytes follow, padded out to a multiple of four */
} __attribute__((packed)) iwl_tlv_t;

/* The record types this driver acts on.  Anything else is stepped over, which
 * is what lets a newer file with records this code has never heard of still
 * load - and Intel add records with every generation. */
#define IWL_TLV_INST          1     /* runtime instructions        */
#define IWL_TLV_DATA          2     /* runtime data                */
#define IWL_TLV_INIT          3     /* initialisation instructions */
#define IWL_TLV_INIT_DATA     4
#define IWL_TLV_SEC_RT       19     /* a runtime section, with an address */
#define IWL_TLV_SEC_INIT     20
#define IWL_TLV_SEC_WOWLAN   21
#define IWL_TLV_API_CHANGES  29
#define IWL_TLV_CAPABILITIES 30
#define IWL_TLV_FW_VERSION   36

#define IWL_MAX_SECTIONS 16

typedef struct {
    u32         destination;    /* where in the device it goes */
    const u8   *data;
    u32         length;
    u32         type;
} iwl_section_t;

typedef struct {
    char          name[64];
    u32           version;
    u32           build;
    iwl_section_t section[IWL_MAX_SECTIONS];
    int           sections;
    u32           total_bytes;
} iwl_image_t;

/* The register definitions and the message shapes are in iwl_cmd.h, which is
 * shared with the model of the device that the self-test drives this against. */

/* ------------------------------------------------------------------ state */

/* One transmit queue: a ring of descriptors, and one buffer per slot for the
 * command or frame the descriptor points at. */
typedef struct {
    iwl_tfd_t *tfd;
    u64        tfd_phys;
    u8        *buffer;          /* IWL_TFD_QUEUE_SIZE * IWL_CMD_BUFFER */
    u64        buffer_phys;
    u16        write;
    u16        read;
} iwl_queue_t;

/* The receive ring: a list of page addresses the device fills, and a status
 * block it writes its progress into. */
typedef struct {
    u32             *bd;        /* the page list, addresses shifted right by 8 */
    u64              bd_phys;
    u8              *pages;
    u64              pages_phys;
    iwl_rb_status_t *status;
    u64              status_phys;
    u16              read;
} iwl_rx_t;

typedef struct {
    volatile u8  *regs;
    wifi_device_t dev;
    firmware_t    fw;
    iwl_image_t   image;
    bool          parsed;

    /* The rings, and where the conversation with the microcode has got to. */
    iwl_queue_t   queue[IWL_MAX_QUEUES];
    iwl_rx_t      rx;
    bool          rings_ready;
    bool          alive;
    u16           next_sequence;

    /* What the microcode said about itself. */
    iwl_alive_t   alive_info;

    /* A response being waited for, and where to put it.  A command with no
     * payload in its answer is still waited for, so the flag is separate from
     * whether there is anywhere to put one. */
    bool          waiting;
    u16           waiting_for;
    u8           *response;
    int           response_cap;
    int           response_len;
    bool          response_seen;

    /* Set when the device is a model rather than silicon. */
    bool          modelled;
} iwl_t;

#define MAX_CARDS 2
static iwl_t cards[MAX_CARDS];
static int   card_count;

void iwl_model_sync(void);

static inline u32 rd(iwl_t *c, u32 off) {
    /* Reading is when the driver observes the device.  Against a model that is
     * also when the model has to catch up with whatever was written since. */
    if (c->modelled) iwl_model_sync();
    return c->regs ? *(volatile u32 *)(c->regs + off) : 0xFFFFFFFFu;
}
static inline void wr(iwl_t *c, u32 off, u32 v) {
    if (c->regs) *(volatile u32 *)(c->regs + off) = v;
}

/* ------------------------------------------------------------- the parser */

static inline u32 le32(const void *p) {
    const u8 *b = p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

/* Take a firmware image apart into the sections that have to be written into
 * the device, in the order they have to be written.
 *
 * Every bound is checked against the file's actual length: a firmware file is
 * data from outside this system, and a length field that runs off the end must
 * be rejected rather than followed. */
bool iwl_parse_firmware(const u8 *data, size_t size, iwl_image_t *out) {
    memset(out, 0, sizeof *out);

    if (size < sizeof(iwl_ucode_header_t)) {
        kwarn("iwlwifi", "the firmware file is too short to be one");
        return false;
    }

    const iwl_ucode_header_t *header = (const void *)data;
    if (le32(&header->zero) != 0 || le32(&header->magic) != IWL_TLV_UCODE_MAGIC) {
        kwarn("iwlwifi", "that file is not an Intel firmware image");
        return false;
    }

    out->version = le32(&header->version);
    out->build = le32(&header->build);

    /* The readable name is not guaranteed to be terminated. */
    size_t n = 0;
    while (n < sizeof header->human_readable - 1 &&
           n < sizeof out->name - 1 &&
           header->human_readable[n]) {
        out->name[n] = (char)header->human_readable[n];
        n++;
    }
    out->name[n] = 0;

    size_t offset = sizeof(iwl_ucode_header_t);
    int records = 0;

    while (offset + sizeof(iwl_tlv_t) <= size) {
        const iwl_tlv_t *tlv = (const void *)(data + offset);
        u32 type = le32(&tlv->type);
        u32 length = le32(&tlv->length);

        offset += sizeof(iwl_tlv_t);
        if (length > size - offset) {
            kwarn("iwlwifi", "a record claims %u bytes but only %zu remain",
                  length, size - offset);
            return false;
        }

        const u8 *payload = data + offset;

        switch (type) {
        /* The older layout: the type says where it goes. */
        case IWL_TLV_INST:
        case IWL_TLV_DATA:
        case IWL_TLV_INIT:
        case IWL_TLV_INIT_DATA:
            if (out->sections < IWL_MAX_SECTIONS && length) {
                iwl_section_t *s = &out->section[out->sections++];
                /* Instructions go to the instruction memory, data to the data
                 * memory; the driver knows the two base addresses. */
                s->destination = (type == IWL_TLV_INST || type == IWL_TLV_INIT)
                               ? 0x00000000u : 0x00800000u;
                s->data = payload;
                s->length = length;
                s->type = type;
                out->total_bytes += length;
            }
            break;

        /* The newer layout: the first four bytes are the address. */
        case IWL_TLV_SEC_RT:
        case IWL_TLV_SEC_INIT:
        case IWL_TLV_SEC_WOWLAN:
            if (length < 4) break;
            if (out->sections < IWL_MAX_SECTIONS) {
                iwl_section_t *s = &out->section[out->sections++];
                s->destination = le32(payload);
                s->data = payload + 4;
                s->length = length - 4;
                s->type = type;
                out->total_bytes += s->length;
            }
            break;

        case IWL_TLV_FW_VERSION:
            if (length >= 4) out->version = le32(payload);
            break;

        default:
            /* A record this driver has no use for.  Stepping over it is what
             * lets a file from a newer generation still load. */
            break;
        }

        /* Records are padded out to a multiple of four. */
        offset += (length + 3u) & ~3u;
        if (++records > 256) break;            /* a malformed file, not a real one */
    }

    if (!out->sections) {
        kwarn("iwlwifi", "the firmware file contains no loadable sections");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- the device */

/* Tell the device the host is here and wait for it to say it is ready. */
static bool prepare_nic(iwl_t *c) {
    if (rd(c, CSR_HW_IF_CONFIG) & CSR_HW_IF_NIC_READY) return true;

    wr(c, CSR_HW_IF_CONFIG, rd(c, CSR_HW_IF_CONFIG) | CSR_HW_IF_PREPARE);

    for (int i = 0; i < 150; i++) {
        if (rd(c, CSR_HW_IF_CONFIG) & CSR_HW_IF_NIC_READY) return true;
        timer_mdelay(1);
    }
    return false;
}

/* The device's clocks have to be running before its memory can be written, and
 * access has to be held for as long as the writing goes on. */
static bool claim_access(iwl_t *c) {
    wr(c, CSR_GP_CNTRL, rd(c, CSR_GP_CNTRL) | CSR_GP_MAC_ACCESS_REQ);

    for (int i = 0; i < 15000; i++) {
        if (rd(c, CSR_GP_CNTRL) & CSR_GP_MAC_CLOCK_READY) return true;
        timer_udelay(10);
    }
    return false;
}

static void release_access(iwl_t *c) {
    wr(c, CSR_GP_CNTRL, rd(c, CSR_GP_CNTRL) & ~CSR_GP_MAC_ACCESS_REQ);
}

static void stop_device(iwl_t *c) {
    wr(c, CSR_INT_MASK, 0);
    wr(c, CSR_INT, 0xFFFFFFFF);

    /* Stop the bus master and wait for it to say it has stopped, so a reset
     * cannot happen while a transfer is in flight. */
    wr(c, CSR_RESET, CSR_RESET_STOP_MASTER);
    for (int i = 0; i < 100; i++) {
        if (rd(c, CSR_RESET) & CSR_RESET_MASTER_DISABLED) break;
        timer_udelay(10);
    }

    wr(c, CSR_RESET, CSR_RESET_SW);
    timer_mdelay(5);
}

/* Write one section into the device's memory, a word at a time through the
 * address and data registers.  Newer parts have a DMA path that is faster;
 * this one works on every part in the family. */
static bool write_section(iwl_t *c, const iwl_section_t *s) {
    if (s->length % 4) {
        kwarn("iwlwifi", "a section is %u bytes, which is not a whole number "
                         "of words", s->length);
        return false;
    }

    void iwl_model_note_write(u32 words);

    wr(c, HBUS_TARG_MEM_WADDR, s->destination);
    for (u32 offset = 0; offset < s->length; offset += 4)
        wr(c, HBUS_TARG_MEM_WDAT, le32(s->data + offset));
    if (c->modelled) iwl_model_note_write(s->length / 4);

    return true;
}

/* Get the device into the state where its memory can be written: awake, reset,
 * and with its clocks running.  The rings are programmed between this and the
 * firmware being written, because a reset clears the registers that hold
 * them. */
static bool reset_device(iwl_t *c) {
    if (!prepare_nic(c)) {
        kwarn("iwlwifi", "%s: the device did not become ready", c->dev.name);
        return false;
    }
    stop_device(c);
    return true;
}

static bool load_firmware(iwl_t *c) {
    wifi_device_t *dev = &c->dev;

    if (!claim_access(c)) {
        kwarn("iwlwifi", "%s: the device's clocks did not start", dev->name);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < c->image.sections && ok; i++)
        ok = write_section(c, &c->image.section[i]);

    release_access(c);

    if (!ok) return false;

    /* Releasing the reset is what starts the microcode running.  It answers
     * with an alive notification through the receive ring, which is why the
     * rings were programmed before this point rather than after. */
    wr(c, CSR_RESET, 0);

    kinfo("iwlwifi", "%s: %d section(s), %u bytes written into the device",
          dev->name, c->image.sections, c->image.total_bytes);
    return true;
}


/* ------------------------------------------------------------------ queues
 *
 * Everything below this point is the conversation with the microcode rather
 * than with the radio.  Two rings carry it: descriptors going out, pages
 * coming back, and a doorbell register for each direction.
 */

/* Take down a ring's memory.  Called on any failure part-way through setting
 * them up, so it has to cope with a half-built one. */
static void free_rings(iwl_t *c) {
    for (int q = 0; q < IWL_MAX_QUEUES; q++) {
        iwl_queue_t *queue = &c->queue[q];
        if (queue->tfd)
            dma_free_pages(queue->tfd,
                           (IWL_TFD_QUEUE_SIZE * sizeof(iwl_tfd_t) + PAGE_SIZE - 1) / PAGE_SIZE);
        if (queue->buffer)
            dma_free_pages(queue->buffer,
                           (IWL_TFD_QUEUE_SIZE * IWL_CMD_BUFFER + PAGE_SIZE - 1) / PAGE_SIZE);
        memset(queue, 0, sizeof *queue);
    }
    if (c->rx.bd)
        dma_free_pages(c->rx.bd, (IWL_RX_QUEUE_SIZE * 4 + PAGE_SIZE - 1) / PAGE_SIZE);
    if (c->rx.pages)
        dma_free_pages(c->rx.pages, IWL_RX_QUEUE_SIZE);
    if (c->rx.status)
        dma_free_pages(c->rx.status, 1);
    memset(&c->rx, 0, sizeof c->rx);
    c->rings_ready = false;
}

static bool setup_rings(iwl_t *c) {
    u64 phys;

    for (int q = 0; q < IWL_MAX_QUEUES; q++) {
        iwl_queue_t *queue = &c->queue[q];

        size_t tfd_pages = (IWL_TFD_QUEUE_SIZE * sizeof(iwl_tfd_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        queue->tfd = dma_alloc_pages(tfd_pages, &phys);
        if (!queue->tfd) { free_rings(c); return false; }
        queue->tfd_phys = phys;
        memset(queue->tfd, 0, tfd_pages * PAGE_SIZE);

        size_t buf_pages = (IWL_TFD_QUEUE_SIZE * IWL_CMD_BUFFER + PAGE_SIZE - 1) / PAGE_SIZE;
        queue->buffer = dma_alloc_pages(buf_pages, &phys);
        if (!queue->buffer) { free_rings(c); return false; }
        queue->buffer_phys = phys;
        memset(queue->buffer, 0, buf_pages * PAGE_SIZE);

        queue->write = queue->read = 0;
    }

    c->rx.bd = dma_alloc_pages((IWL_RX_QUEUE_SIZE * 4 + PAGE_SIZE - 1) / PAGE_SIZE, &phys);
    if (!c->rx.bd) { free_rings(c); return false; }
    c->rx.bd_phys = phys;

    c->rx.pages = dma_alloc_pages(IWL_RX_QUEUE_SIZE, &phys);
    if (!c->rx.pages) { free_rings(c); return false; }
    c->rx.pages_phys = phys;
    memset(c->rx.pages, 0, IWL_RX_QUEUE_SIZE * IWL_RX_PAGE_SIZE);

    c->rx.status = dma_alloc_pages(1, &phys);
    if (!c->rx.status) { free_rings(c); return false; }
    c->rx.status_phys = phys;
    memset(c->rx.status, 0, sizeof *c->rx.status);

    /* The page list holds addresses shifted right by eight, which is how a
     * thirty-six bit address fits in a thirty-two bit word.  Every page is
     * offered to the device up front. */
    for (int i = 0; i < IWL_RX_QUEUE_SIZE; i++)
        c->rx.bd[i] = (u32)((c->rx.pages_phys + (u64)i * IWL_RX_PAGE_SIZE) >> 8);
    c->rx.read = 0;

    c->rings_ready = true;
    return true;
}

/* Tell the device where the rings are.  This has to happen after the microcode
 * has started, because a reset clears every one of these. */
static void program_rings(iwl_t *c) {
    /* Receive first: the status block, the page list, and how many pages are
     * available.  The write pointer is left eight short of the end so the
     * device can never catch its own tail. */
    wr(c, FH_MEM_RCSR_CHNL0_CONFIG, 0);
    wr(c, FH_RSCSR_CHNL0_STTS_WPTR, (u32)(c->rx.status_phys >> 4));
    wr(c, FH_RSCSR_CHNL0_RBDCB_BASE, (u32)(c->rx.bd_phys >> 8));
    wr(c, FH_RSCSR_CHNL0_WPTR, (IWL_RX_QUEUE_SIZE - 8) & ~7u);
    wr(c, FH_MEM_RCSR_CHNL0_CONFIG, FH_RCSR_RX_CONFIG_ENABLE);

    /* Then each transmit queue: where its descriptors are, and enable the
     * channel that services it. */
    for (int q = 0; q < IWL_MAX_QUEUES; q++) {
        int hw_queue = (q == 0) ? IWL_DATA_QUEUE : IWL_CMD_QUEUE;
        wr(c, FH_MEM_CBBC_QUEUE_BASE + hw_queue * 4, (u32)(c->queue[q].tfd_phys >> 8));
        wr(c, FH_TCSR_CHNL_TX_CONFIG + hw_queue * 0x20, FH_TCSR_TX_CONFIG_ENABLE);
        wr(c, HBUS_TARG_WRPTR, (u32)(hw_queue << 8));
    }
}

/* Which hardware queue this driver's queue index maps to. */
static int hw_queue_of(int index) { return index == 1 ? IWL_CMD_QUEUE : IWL_DATA_QUEUE; }

/* Put one buffer on a descriptor, splitting the address the way the hardware
 * expects: thirty-two bits in one field, the top four bits packed beside the
 * length in the next. */
static void add_buffer(iwl_tfd_t *tfd, u64 phys, u16 len) {
    if (tfd->num_tbs >= IWL_NUM_OF_TBS) return;
    iwl_tfd_tb_t *tb = &tfd->tbs[tfd->num_tbs++];
    tb->lo = (u32)phys;
    tb->hi_n_len = (u16)(((phys >> 32) & 0xF) | (len << 4));
}

/* Hand one command or frame to the device.  Returns the ring slot it went
 * into, which is what the sequence number is built from. */
static int enqueue(iwl_t *c, int index, const void *data, int len) {
    iwl_queue_t *q = &c->queue[index];
    if (len > IWL_CMD_BUFFER) {
        kwarn("iwlwifi", "a %d byte message will not fit a queue slot", len);
        return -1;
    }

    u16 slot = q->write & (IWL_TFD_QUEUE_SIZE - 1);
    u8 *buffer = q->buffer + (size_t)slot * IWL_CMD_BUFFER;
    memcpy(buffer, data, (size_t)len);

    iwl_tfd_t *tfd = &q->tfd[slot];
    memset(tfd, 0, sizeof *tfd);
    add_buffer(tfd, q->buffer_phys + (u64)slot * IWL_CMD_BUFFER, (u16)len);

    q->write = (u16)((q->write + 1) & (IWL_TFD_QUEUE_SIZE - 1));

    /* The doorbell: the queue in the high byte, the new write pointer in the
     * low one.  Writing it is what tells the device to look. */
    wr(c, HBUS_TARG_WRPTR, (u32)((hw_queue_of(index) << 8) | q->write));
    return slot;
}

/* ------------------------------------------------------------ the receiving */

static void handle_packet(iwl_t *c, const iwl_rx_packet_t *packet, int length);

/* Walk whatever the device has left in the receive ring since last time.
 * Returns how many packets were taken. */
static int drain_rx(iwl_t *c) {
    if (!c->rings_ready) return 0;
    if (c->modelled) iwl_model_sync();

    int taken = 0;
    u16 closed = c->rx.status->closed_rb_num & (IWL_RX_QUEUE_SIZE - 1);

    while (c->rx.read != closed && taken < IWL_RX_QUEUE_SIZE) {
        const u8 *page = c->rx.pages + (size_t)c->rx.read * IWL_RX_PAGE_SIZE;
        const iwl_rx_packet_t *packet = (const void *)page;

        u32 length = packet->len_n_flags & 0x3FFF;
        if (length >= sizeof(iwl_cmd_header_t) && length <= IWL_RX_PAGE_SIZE)
            handle_packet(c, packet, (int)(length - sizeof(iwl_cmd_header_t)));

        c->rx.read = (u16)((c->rx.read + 1) & (IWL_RX_QUEUE_SIZE - 1));
        taken++;

        /* The page is free again, so it goes back on the list. */
        wr(c, FH_RSCSR_CHNL0_WPTR, (u32)((c->rx.read - 8) & (IWL_RX_QUEUE_SIZE - 1) & ~7u));
    }
    return taken;
}

/* ------------------------------------------------------------ the commands */

/* Send one command and, if `response` is given, wait for the answer that
 * carries the same sequence number.
 *
 * Pairing on the sequence number rather than on the next thing to arrive is
 * what makes this safe: notifications the microcode sends of its own accord -
 * a received frame, a scan finishing - turn up in the middle of a command's
 * round trip all the time, and they are dispatched rather than mistaken for
 * the answer. */
static int send_command(iwl_t *c, u8 group, u8 cmd, const void *payload, int payload_len,
                        void *response, int response_cap, int timeout_ms) {
    if (!c->rings_ready) return -1;

    u8 message[IWL_CMD_BUFFER];
    if (payload_len + (int)sizeof(iwl_cmd_header_t) > (int)sizeof message) return -1;

    iwl_cmd_header_t *hdr = (void *)message;
    hdr->cmd = cmd;
    hdr->group_id = group;

    iwl_queue_t *q = &c->queue[1];                 /* commands go down queue 1 */
    u16 sequence = IWL_MAKE_SEQ(IWL_CMD_QUEUE, q->write);
    hdr->sequence = sequence;

    if (payload && payload_len)
        memcpy(message + sizeof *hdr, payload, (size_t)payload_len);

    c->waiting = true;
    c->waiting_for = sequence;
    c->response = response;
    c->response_cap = response_cap;
    c->response_len = 0;
    c->response_seen = false;

    if (enqueue(c, 1, message, (int)sizeof *hdr + payload_len) < 0) {
        c->waiting = false;
        c->response = NULL;
        return -1;
    }

    if (!timeout_ms) { c->waiting = false; c->response = NULL; return 0; }

    for (int waited = 0; waited < timeout_ms; waited++) {
        drain_rx(c);
        if (c->response_seen) {
            int len = c->response_len;
            c->waiting = false;
            c->response = NULL;
            return len;
        }
        timer_mdelay(1);
    }

    c->waiting = false;
    c->response = NULL;
    kwarn("iwlwifi", "command %02x/%02x was not answered within %d ms",
          group, cmd, timeout_ms);
    return -1;
}

/* --------------------------------------------------------- what comes back */

static void handle_packet(iwl_t *c, const iwl_rx_packet_t *packet, int length) {
    u8 cmd = packet->hdr.cmd;

    /* An answer to something that was asked for. */
    if (c->waiting && packet->hdr.sequence == c->waiting_for) {
        int copy = 0;
        if (c->response) {
            copy = length < c->response_cap ? length : c->response_cap;
            if (copy > 0) memcpy(c->response, packet->data, (size_t)copy);
        }
        c->response_len = copy;
        c->response_seen = true;
        return;
    }

    switch (cmd) {
    case IWL_CMD_ALIVE:
        if (length >= (int)sizeof(iwl_alive_t)) {
            memcpy(&c->alive_info, packet->data, sizeof c->alive_info);
            c->alive = (c->alive_info.status == IWL_ALIVE_STATUS_OK);
        }
        break;

    case IWL_CMD_RX_MPDU: {
        /* A received frame.  The microcode puts its own descriptor in front of
         * the frame; the 802.11 layer wants only what was on the air. */
        int offset = (int)sizeof(u32) * 2;         /* the microcode's descriptor */
        if (length > offset)
            ieee80211_receive(&c->dev, (u8 *)packet->data + offset, length - offset, -50);
        break;
    }

    case IWL_CMD_SCAN_COMPLETE_UMAC:
        kdebug("iwlwifi", "%s: the scan finished", c->dev.name);
        break;

    case IWL_CMD_TX_RESPONSE:
        /* A frame the driver sent has been dealt with; the slot is free. */
        c->queue[0].read = IWL_SEQ_TO_INDEX(packet->hdr.sequence);
        break;

    default:
        kdebug("iwlwifi", "%s: notification %02x, %d bytes", c->dev.name, cmd, length);
        break;
    }
}

/* -------------------------------------------------------- the alive handshake */

/* After the reset is released the microcode starts, and the first thing it
 * does is say so.  Everything else has to wait for that: a command sent before
 * it lands in a queue nothing is reading. */
static bool wait_for_alive(iwl_t *c, int timeout_ms) {
    c->alive = false;
    for (int waited = 0; waited < timeout_ms; waited++) {
        u32 interrupts = rd(c, CSR_INT);
        if (interrupts & (CSR_INT_BIT_SW_ERR | CSR_INT_BIT_HW_ERR)) {
            kwarn("iwlwifi", "%s: the microcode reported an error while starting",
                  c->dev.name);
            return false;
        }
        drain_rx(c);
        if (c->alive) return true;
        timer_mdelay(1);
    }
    return false;
}

/* --------------------------------------------------------------- the driver */

static size_t build_test_image(u8 *image, size_t cap);

static bool iwl_start(wifi_device_t *dev) {
    iwl_t *c = dev->ctx;

    /* A model has no firmware file to be given, so it gets an image built to
     * the same format.  The load path then runs in full: the container is
     * parsed, the sections are written into the device word by word, and the
     * reset is released - exactly as it would be with Intel's own file. */
    if (c->modelled && !dev->firmware_present && !c->parsed) {
        static u8 image[1024];
        size_t len = build_test_image(image, sizeof image);
        if (len && iwl_parse_firmware(image, len, &c->image)) {
            c->parsed = true;
            kinfo("iwlwifi", "%s: no firmware file, so an image built to the "
                             "same format is being loaded into the model",
                  dev->name);
        }
    }

    if (!dev->firmware_present && !c->parsed) {
        kwarn("iwlwifi", "%s: %s is not present, so the radio cannot start",
              dev->name, dev->firmware_name);
        kwarn("iwlwifi", "%s: `firmware import <path>` copies it in; it is in "
                         "the linux-firmware package", dev->name);
        return false;
    }

    if (!c->parsed && !c->fw.data && !firmware_load(dev->firmware_name, &c->fw))
        return false;

    if (!c->parsed) {
        if (!iwl_parse_firmware(c->fw.data, c->fw.size, &c->image)) return false;
        c->parsed = true;
        kinfo("iwlwifi", "%s: firmware \"%s\", version %u.%u, %d section(s)",
              dev->name, c->image.name,
              c->image.version >> 24, (c->image.version >> 16) & 0xFF,
              c->image.sections);
    }

    if (!c->regs) {
        kwarn("iwlwifi", "%s: the firmware is readable but the device is not "
                         "mapped", dev->name);
        return false;
    }

    /* The order matters and is not obvious: reset the device first, then
     * program the rings, then write the microcode and let it go.  Programming
     * the rings after the reset is what keeps them - a reset clears every
     * register that holds one - and programming them before the microcode
     * starts is what means the first thing it says has somewhere to land. */
    if (!reset_device(c)) return false;

    if (!c->rings_ready && !setup_rings(c)) {
        kwarn("iwlwifi", "%s: no memory for the queues", dev->name);
        return false;
    }
    program_rings(c);

    if (!load_firmware(c)) return false;

    if (!wait_for_alive(c, 2000)) {
        kwarn("iwlwifi", "%s: the microcode did not report itself alive", dev->name);
        return false;
    }
    kinfo("iwlwifi", "%s: microcode alive, version %u.%u, API %u.%u",
          dev->name, c->alive_info.ucode_major, c->alive_info.ucode_minor,
          c->alive_info.api_major, c->alive_info.api_minor);

    /* The radio's own configuration, which the microcode needs before it will
     * transmit anything.  What goes in it is firmware-API-versioned; what is
     * sent here is the shape common to the generations this driver knows. */
    struct {
        u32 phy_cfg;
        u32 calib_control_flow;
        u32 calib_control_event;
    } phy = { 0, 0xFFFFFFFF, 0xFFFFFFFF };
    if (send_command(c, 0, IWL_CMD_PHY_CONFIGURATION, &phy, sizeof phy, NULL, 0, 500) < 0)
        kwarn("iwlwifi", "%s: the radio configuration was not acknowledged", dev->name);

    /* The device's own address lives in its non-volatile memory, which is read
     * through the microcode rather than directly. */
    struct { u16 offset, length, type, op; } nvm = { 0, 8, 0 /* HW section */, 0 /* read */ };
    u8 answer[64];
    int n = send_command(c, 0, IWL_CMD_NVM_ACCESS, &nvm, sizeof nvm, answer, sizeof answer, 500);
    if (n >= (int)sizeof(iwl_nvm_access_resp_t) + ETH_ALEN) {
        const iwl_nvm_access_resp_t *resp = (const void *)answer;
        if (resp->status == 0) memcpy(dev->mac.addr, resp->data, ETH_ALEN);
    }
    if (!dev->mac.addr[0] && !dev->mac.addr[1] && !dev->mac.addr[2])
        kwarn("iwlwifi", "%s: the device's address could not be read", dev->name);

    dev->radio_up = true;
    char mac[24];
    mac_format(&dev->mac, mac, sizeof mac);
    kinfo("iwlwifi", "%s: the radio is up, %s%s", dev->name, mac,
          c->modelled ? " (model)" : "");
    return true;
}

static void iwl_stop(wifi_device_t *dev) {
    iwl_t *c = dev->ctx;
    if (c->regs) stop_device(c);
    free_rings(c);
    firmware_free(&c->fw);
    c->parsed = false;
    c->alive = false;
    dev->radio_up = false;
}

/* Tuning is a command like any other: the microcode owns the synthesiser. */
static bool iwl_set_channel(wifi_device_t *dev, u8 channel) {
    iwl_t *c = dev->ctx;
    dev->channel = channel;
    if (!c->alive) return false;

    struct {
        u32 id_and_action;
        u32 apply_time;
        u8  band;              /* 1 is 2.4 GHz, 0 is 5 GHz */
        u8  channel;
        u8  width;
        u8  ctrl_pos;
        u32 rxchain_info;
        u32 txchain_info;
    } ctx = {
        .id_and_action = 1,               /* context zero, add-or-modify */
        .band = channel <= 14 ? 1 : 0,
        .channel = channel,
        .width = 0,                       /* twenty megahertz */
        .rxchain_info = 0x3,
        .txchain_info = 0x3,
    };
    return send_command(c, 0, IWL_CMD_PHY_CONTEXT, &ctx, sizeof ctx, NULL, 0, 200) >= 0;
}

/* A frame to send goes down the data queue with the microcode's transmit
 * command in front of it.  The two travel in one buffer so the descriptor only
 * has to point at one place. */
static int iwl_transmit(wifi_device_t *dev, const void *frame, int len) {
    iwl_t *c = dev->ctx;
    if (!c->alive) return -1;
    if (len <= 0 || len > IWL_CMD_BUFFER - 64) return -1;

    struct {
        iwl_cmd_header_t hdr;
        u16 len;
        u16 offload_assist;
        u32 tx_flags;
        u32 rate_n_flags;
        u8  sta_id;
        u8  sec_ctl;
        u8  initial_rate_index;
        u8  reserved;
        u32 life_time;
    } __attribute__((packed)) tx;

    memset(&tx, 0, sizeof tx);
    tx.hdr.cmd = IWL_CMD_TX;
    tx.hdr.group_id = 0;
    tx.hdr.sequence = IWL_MAKE_SEQ(IWL_DATA_QUEUE, c->queue[0].write);
    tx.len = (u16)len;
    tx.tx_flags = 0x00000008;             /* the microcode picks the rate */
    tx.sta_id = 0;
    tx.life_time = 0xFFFFFFFF;            /* no expiry */

    u8 message[IWL_CMD_BUFFER];
    memcpy(message, &tx, sizeof tx);
    memcpy(message + sizeof tx, frame, (size_t)len);

    if (enqueue(c, 0, message, (int)sizeof tx + len) < 0) return -1;
    return len;
}

static void iwl_poll(wifi_device_t *dev) {
    iwl_t *c = dev->ctx;
    if (c->rings_ready) drain_rx(c);
}

static const wifi_driver_t iwl_driver = {
    "iwlwifi", iwl_start, iwl_stop, iwl_set_channel,
    iwl_transmit, iwl_poll, NULL,
};

/* Everything one card needs, whether it is silicon or a model. */
static bool bring_up(iwl_t *c, volatile u8 *regs, u16 pci_device, bool modelled) {
    memset(c, 0, sizeof *c);
    c->regs = regs;
    c->modelled = modelled;

    wifi_device_t *dev = &c->dev;
    if (!wifi_identify(0x8086, pci_device, dev)) return false;

    firmware_declare(dev->firmware_name, "the linux-firmware package");
    dev->firmware_present = firmware_present(dev->firmware_name, NULL);

    snprintf(dev->name, sizeof dev->name, "wlan%d", wifi_count());
    dev->driver = &iwl_driver;
    dev->ctx = c;

    wifi_register(dev);
    ieee80211_attach(dev);
    iwl_start(dev);
    return true;
}

void iwlwifi_init(void) {
    pci_dev_t *pci = NULL;

    while ((pci = pci_find(0x02, 0x80, 0xFF, pci)) != NULL) {
        if (pci->vendor != 0x8086) continue;
        if (card_count >= MAX_CARDS) break;

        volatile u8 *regs = NULL;
        if (pci->bar[0] && !pci->bar_is_io[0]) {
            pci_enable_memory(pci);
            pci_enable_bus_master(pci);
            size_t len = pci->bar_size[0] ? (size_t)pci->bar_size[0] : 0x2000;
            if (len > 0x100000) len = 0x100000;
            regs = vmm_map_mmio(pci->bar[0], len);
        }

        if (bring_up(&cards[card_count], regs, pci->device, false)) {
            pci_claim(pci, "iwlwifi");
            card_count++;
        }
    }
}

volatile u8 *iwl_model_attach(void);

/* Bring the driver up against a model of the hardware.  This is not a
 * substitute for a card - it cannot confirm a single register offset - but it
 * does run the driver's own code over its own rings, which is where the bugs
 * that can be found without a card actually are. */
bool iwlwifi_attach_model(void) {
    /* Already standing in: the same one is used again rather than a second
     * interface appearing for the same imaginary card. */
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return cards[i].dev.radio_up;

    if (card_count >= MAX_CARDS) return false;

    volatile u8 *regs = iwl_model_attach();
    if (!regs) return false;

    /* An 8260, which is the generation this driver's register set matches. */
    int before = card_count;
    if (bring_up(&cards[card_count], regs, 0x24F3, true)) card_count++;
    return card_count > before;
}

bool iwlwifi_is_modelled(void) { return card_count > 0 && cards[0].modelled; }

/* The device the model is behind, so the self-test can drive it. */
wifi_device_t *iwlwifi_model_device(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return &cards[i].dev;
    return NULL;
}


/* ------------------------------------------------------------------- tests */

/* Build an image in Intel's own container format and check the parser takes it
 * apart correctly.  The format and everything that reads it are entirely
 * checkable without hardware, and they are where a firmware file that will not
 * load usually goes wrong - a record whose length runs off the end, a section
 * whose address is misread, a newer file carrying records this code has never
 * seen.  All three are exercised here.
 *
 * Returns the number of checks that failed. */
/* Build an image in Intel's own container format.  Used by the parser test
 * below, and by the model - which has no real firmware to be given and would
 * otherwise never exercise the load path at all. */
static size_t build_test_image(u8 *image, size_t cap) {
    size_t offset = 0;
    if (cap < 512) return 0;
    memset(image, 0, cap);

    /* The header. */
    image[0] = image[1] = image[2] = image[3] = 0;          /* the zero word */
    image[4] = 0x49; image[5] = 0x57; image[6] = 0x4C; image[7] = 0x0A;
    memcpy(image + 8, "kestrel test image", 18);
    image[72] = 0x11; image[73] = 0x22; image[74] = 0x33; image[75] = 0x44;
    offset = sizeof(iwl_ucode_header_t);

    /* A record this parser has never heard of, which must be stepped over
     * rather than stopping the load - Intel add these every generation. */
    u32 unknown_type = 9999;
    memcpy(image + offset, &unknown_type, 4);
    u32 unknown_len = 6;
    memcpy(image + offset + 4, &unknown_len, 4);
    memcpy(image + offset + 8, "ignore", 6);
    offset += 8 + ((unknown_len + 3) & ~3u);

    /* A runtime section, with its destination in the first four bytes. */
    u32 sec_type = IWL_TLV_SEC_RT;
    u32 sec_len = 4 + 16;
    memcpy(image + offset, &sec_type, 4);
    memcpy(image + offset + 4, &sec_len, 4);
    u32 destination = 0x00805000;
    memcpy(image + offset + 8, &destination, 4);
    for (int i = 0; i < 16; i++) image[offset + 12 + i] = (u8)(0xC0 + i);
    offset += 8 + sec_len;

    /* An instruction section in the older layout. */
    u32 inst_type = IWL_TLV_INST;
    u32 inst_len = 8;
    memcpy(image + offset, &inst_type, 4);
    memcpy(image + offset + 4, &inst_len, 4);
    for (int i = 0; i < 8; i++) image[offset + 8 + i] = (u8)(0xE0 + i);
    offset += 8 + inst_len;

    return offset;
}

int iwl_selftest(void) {
    static u8 image[1024];
    int failures = 0;
    size_t offset = build_test_image(image, sizeof image);

    iwl_image_t parsed;
    if (!iwl_parse_firmware(image, offset, &parsed)) {
        kerr("iwlwifi", "a well-formed image failed to parse");
        return 1;
    }

    if (parsed.sections != 2) {
        kerr("iwlwifi", "found %d sections, expected 2", parsed.sections);
        failures++;
    }
    if (strcmp(parsed.name, "kestrel test image")) {
        kerr("iwlwifi", "the name came out as \"%s\"", parsed.name);
        failures++;
    }
    if (parsed.sections > 0 && parsed.section[0].destination != 0x00805000) {
        kerr("iwlwifi", "the section address came out as %08x, expected 805000",
             parsed.section[0].destination);
        failures++;
    }
    if (parsed.sections > 0 &&
        (parsed.section[0].length != 16 || parsed.section[0].data[0] != 0xC0)) {
        kerr("iwlwifi", "the section contents are wrong");
        failures++;
    }
    if (parsed.total_bytes != 24) {
        kerr("iwlwifi", "the total came out as %u bytes, expected 24",
             parsed.total_bytes);
        failures++;
    }

    /* A record whose length runs past the end of the file must be refused: a
     * firmware image is data from outside this system, and following a bad
     * length would read whatever happened to be after it. */
    static u8 truncated[128];
    memcpy(truncated, image, sizeof(iwl_ucode_header_t));
    size_t bad = sizeof(iwl_ucode_header_t);
    u32 bad_type = IWL_TLV_SEC_RT, bad_len = 0x10000;
    memcpy(truncated + bad, &bad_type, 4);
    memcpy(truncated + bad + 4, &bad_len, 4);

    iwl_image_t rejected;
    if (iwl_parse_firmware(truncated, bad + 8 + 16, &rejected)) {
        kerr("iwlwifi", "an image with a length running off the end was accepted");
        failures++;
    }

    /* Something that is not a firmware image at all. */
    static const u8 nonsense[64] = { 0xDE, 0xAD, 0xBE, 0xEF };
    if (iwl_parse_firmware(nonsense, sizeof nonsense, &rejected)) {
        kerr("iwlwifi", "a file that is not firmware was accepted");
        failures++;
    }

    if (!failures)
        kinfo("iwlwifi", "the firmware container format is read correctly, "
                         "including a record type this driver does not know "
                         "and a length that runs off the end");
    return failures;
}
