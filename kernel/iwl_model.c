/* iwl_model.c - a model of an Intel Wi-Fi device, and of its microcode.
 *
 * No virtual machine emulates a Wi-Fi card, so a driver written for one has
 * nowhere to run.  This stands in: it answers the registers the Intel driver
 * writes, accepts the microcode being pushed into it, and then plays the part
 * of that microcode - reading the descriptors the driver builds, following
 * them into host memory, and answering through the receive ring.
 *
 * The driver that runs against it is the same driver that would run against a
 * card.  Nothing in it is switched off or stubbed for the model; the only
 * thing it knows is which side of the bus it is talking to, and that only so
 * it can let the model catch up before reading a register.
 *
 * ---------------------------------------------------------------------------
 * What this establishes, and what it does not.  It exercises the descriptor
 * layout, the DMA addressing, the ring wrap-around, the doorbell protocol, the
 * sequence numbers that pair a response with its command, and the whole path
 * from a scan request down through the queues and back up into the 802.11
 * layer.  Those are the parts where driver bugs actually live.
 *
 * It cannot establish that the register offsets match Intel's silicon, nor
 * that the payload of each command matches the firmware's idea of it: a model
 * written from the same understanding as the driver checks that understanding
 * against itself, not against the world.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "net.h"
#include "wifi.h"
#include "iwl_cmd.h"

#define MODEL_SSID     "kestrel-test"
#define MODEL_CHANNEL  6

static const u8 model_mac[ETH_ALEN] = { 0x00, 0x21, 0x6a, 0x44, 0x55, 0x66 };
static const u8 model_bssid[ETH_ALEN] = { 0x02, 0x00, 0x5e, 0xaa, 0xbb, 0xcc };

/* --------------------------------------------------------------- the device */

typedef struct {
    bool present;
    volatile u8 *regs;          /* the registers the driver writes */

    bool nic_ready;
    bool clocks_running;
    bool master_stopped;
    bool microcode_running;

    /* What the driver wrote into the device's own memory. */
    u32  mem_write_address;
    u32  bytes_loaded;

    /* Where the rings are, as the driver told it. */
    u64  rx_bd_phys;
    u64  rx_status_phys;
    u64  queue_phys[32];
    u16  queue_read[32];        /* how far the model has consumed each queue */
    u16  rx_write;              /* where the next packet goes */
    bool rx_enabled;

    /* The channel the driver last asked for. */
    u8   channel;
    bool answered_probe;

    /* What the device is trying to tell the driver.  A driver acknowledges an
     * interrupt by writing its bit back, which clears it - so the register
     * cannot simply hold whatever was last written to it. */
    u32  interrupt_status;
} model_t;

static model_t model;

/* The registers the driver reads live in ordinary memory here; a write by the
 * driver is picked up on its next read. */
#define REG_BYTES 0x2000
static u8 register_space[REG_BYTES] __attribute__((aligned(4096)));

static u32 reg_get(u32 off) {
    return off + 4 <= REG_BYTES ? *(volatile u32 *)(register_space + off) : 0;
}
static void reg_set(u32 off, u32 v) {
    if (off + 4 <= REG_BYTES) *(volatile u32 *)(register_space + off) = v;
}

/* ------------------------------------------------------------- the receiving
 *
 * Putting a message where the driver will find it means writing it into the
 * next free page of the receive ring and moving the status block on, which is
 * exactly what the device does. */
/* Raise an interrupt, which on this device means setting a bit the driver will
 * see and then acknowledge. */
static void raise_interrupt(u32 bits) {
    model.interrupt_status |= bits;
    reg_set(CSR_INT, model.interrupt_status);
}

static void deliver(u8 cmd, u16 sequence, const void *payload, int length) {
    if (!model.rx_bd_phys || !model.rx_status_phys) {
        kwarn("iwl-model", "a message was ready before the driver had "
                           "programmed the receive ring");
        return;
    }

    u32 *bd = phys_to_virt(model.rx_bd_phys);
    iwl_rb_status_t *status = phys_to_virt(model.rx_status_phys);

    u16 slot = model.rx_write & (IWL_RX_QUEUE_SIZE - 1);
    u64 page_phys = (u64)bd[slot] << 8;
    if (!page_phys) return;

    u8 *page = phys_to_virt(page_phys);
    iwl_rx_packet_t *packet = (void *)page;

    int total = (int)sizeof(iwl_cmd_header_t) + length;
    if (total > IWL_RX_PAGE_SIZE) return;

    packet->len_n_flags = (u32)total;
    packet->hdr.cmd = cmd;
    packet->hdr.group_id = 0;
    packet->hdr.sequence = sequence;
    if (payload && length) memcpy(packet->data, payload, (size_t)length);

    model.rx_write = (u16)((model.rx_write + 1) & (IWL_RX_QUEUE_SIZE - 1));
    status->closed_rb_num = model.rx_write;
    raise_interrupt(CSR_INT_BIT_FH_RX);
}

/* ------------------------------------------------------------ 802.11 frames */

static void put16le(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

/* A probe response, which is what a scan is looking for.  It carries the same
 * information a beacon does: the name, the channel, and the security the
 * network expects. */
static void answer_probe(void) {
    u8 frame[160];
    memset(frame, 0, sizeof frame);

    /* The microcode puts a small descriptor of its own in front of every
     * received frame; the driver steps over it. */
    u8 message[192];
    memset(message, 0, sizeof message);

    put16le(frame, (u16)(0 << 2 | 5 << 4));       /* management, probe response */
    memset(frame + 4, 0xFF, ETH_ALEN);
    memcpy(frame + 10, model_bssid, ETH_ALEN);
    memcpy(frame + 16, model_bssid, ETH_ALEN);

    u8 *p = frame + 24;
    memset(p, 0, 8); p += 8;                      /* the timestamp */
    put16le(p, 100); p += 2;                      /* the beacon interval */
    put16le(p, 0x0011); p += 2;                   /* privacy is on */

    *p++ = 0;                                     /* the network's name */
    *p++ = (u8)strlen(MODEL_SSID);
    memcpy(p, MODEL_SSID, strlen(MODEL_SSID));
    p += strlen(MODEL_SSID);

    *p++ = 3; *p++ = 1; *p++ = MODEL_CHANNEL;     /* which channel it is on */

    /* WPA2 with CCMP, which is what the security element says. */
    static const u8 rsn[] = {
        48, 20, 0x01, 0x00,
        0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
        0x00, 0x00,
    };
    memcpy(p, rsn, sizeof rsn);
    p += sizeof rsn;

    int frame_len = (int)(p - frame);
    int offset = 8;                               /* the microcode's descriptor */
    memcpy(message + offset, frame, (size_t)frame_len);

    deliver(IWL_CMD_RX_MPDU, 0, message, offset + frame_len);
}

/* ------------------------------------------------------------- the commands */

static void handle_command(const u8 *message, int length) {
    if (length < (int)sizeof(iwl_cmd_header_t)) return;
    const iwl_cmd_header_t *hdr = (const void *)message;
    const u8 *payload = message + sizeof *hdr;
    int payload_len = length - (int)sizeof *hdr;

    switch (hdr->cmd) {
    case IWL_CMD_PHY_CONFIGURATION:
        /* Acknowledged by echoing the header back, which is what a command
         * with no answer of its own gets. */
        deliver(IWL_CMD_PHY_CONFIGURATION, hdr->sequence, NULL, 0);
        break;

    case IWL_CMD_NVM_ACCESS: {
        /* The device's address, out of its non-volatile memory. */
        u8 answer[sizeof(iwl_nvm_access_resp_t) + ETH_ALEN];
        iwl_nvm_access_resp_t *resp = (void *)answer;
        resp->offset = 0;
        resp->length = ETH_ALEN;
        resp->type = 0;
        resp->status = 0;
        memcpy(resp->data, model_mac, ETH_ALEN);
        deliver(IWL_CMD_NVM_ACCESS, hdr->sequence, answer, (int)sizeof answer);
        break;
    }

    case IWL_CMD_PHY_CONTEXT:
        if (payload_len >= 10) model.channel = payload[9];
        deliver(IWL_CMD_PHY_CONTEXT, hdr->sequence, NULL, 0);
        break;

    case IWL_CMD_ECHO:
        deliver(IWL_CMD_ECHO, hdr->sequence, payload, payload_len);
        break;

    default:
        /* Anything else is acknowledged so a driver waiting on it does not
         * stall, and noted so a missing case is visible. */
        kdebug("iwl-model", "command %02x, %d bytes of payload",
               hdr->cmd, payload_len);
        deliver(hdr->cmd, hdr->sequence, NULL, 0);
        break;
    }
}

/* A frame the driver asked to transmit.  Only the ones that expect an answer
 * get one: a probe request is answered with a probe response, which is what
 * makes a scan find anything. */
static void handle_transmit(const u8 *message, int length) {
    /* The transmit command sits in front of the frame; its length field says
     * how much of what follows is the frame itself. */
    if (length < 24) return;
    int header_len = length;
    u16 frame_len = 0;
    memcpy(&frame_len, message + sizeof(iwl_cmd_header_t), 2);
    if (frame_len == 0 || frame_len > length) return;
    header_len = length - frame_len;
    if (header_len < (int)sizeof(iwl_cmd_header_t)) return;

    const u8 *frame = message + header_len;
    u16 control = (u16)(frame[0] | (frame[1] << 8));
    u8 type = (u8)((control >> 2) & 3);
    u8 subtype = (u8)((control >> 4) & 0xF);

    const iwl_cmd_header_t *hdr = (const void *)message;

    /* The microcode reports what happened to every frame. */
    u8 status[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
    deliver(IWL_CMD_TX_RESPONSE, hdr->sequence, status, (int)sizeof status);

    if (type == 0 && subtype == 4) {              /* a probe request */
        model.answered_probe = true;
        answer_probe();
    }
}

/* -------------------------------------------------------- watching the bus */

/* One transmit queue's worth of new descriptors, followed into host memory and
 * acted on.  This is the model reading exactly what the driver wrote. */
static void run_queue(int hw_queue, u16 write) {
    u64 tfd_phys = model.queue_phys[hw_queue & 31];
    if (!tfd_phys) return;

    iwl_tfd_t *ring = phys_to_virt(tfd_phys);
    u16 *read = &model.queue_read[hw_queue & 31];

    int guard = 0;
    while (*read != write && guard++ < IWL_TFD_QUEUE_SIZE) {
        iwl_tfd_t *tfd = &ring[*read & (IWL_TFD_QUEUE_SIZE - 1)];

        if (tfd->num_tbs >= 1) {
            /* Reassemble the address the driver split across two fields. */
            const iwl_tfd_tb_t *tb = &tfd->tbs[0];
            u64 phys = (u64)tb->lo | ((u64)(tb->hi_n_len & 0xF) << 32);
            int length = tb->hi_n_len >> 4;

            if (phys && length > 0 && length <= IWL_CMD_BUFFER) {
                const u8 *message = phys_to_virt(phys);
                if (hw_queue == IWL_CMD_QUEUE) handle_command(message, length);
                else handle_transmit(message, length);
            }
        }
        *read = (u16)((*read + 1) & (IWL_TFD_QUEUE_SIZE - 1));
    }
}

/* Registers whose value means "do this" rather than "remember this". */
static u32 previous_reset, previous_doorbell, previous_config, previous_gp;
static u32 previous_rx_config;

void iwl_model_sync(void) {
    if (!model.present) return;

    /* Acknowledgements first.  A driver clears an interrupt by writing its bit
     * back, so a value in this register that is not the current status is the
     * driver acknowledging whatever bits it set - including the "clear
     * everything" write of all ones that every driver does at start-up. */
    u32 written = reg_get(CSR_INT);
    if (written != model.interrupt_status) {
        model.interrupt_status &= ~written;
        reg_set(CSR_INT, model.interrupt_status);
    }

    /* The host says it is here; the device says it is ready. */
    u32 config = reg_get(CSR_HW_IF_CONFIG);
    if (config != previous_config) {
        previous_config = config;
        if (config & CSR_HW_IF_PREPARE) {
            model.nic_ready = true;
            reg_set(CSR_HW_IF_CONFIG, config | CSR_HW_IF_NIC_READY);
        }
    }

    /* Access to the device's memory needs its clocks running. */
    u32 gp = reg_get(CSR_GP_CNTRL);
    if (gp != previous_gp) {
        previous_gp = gp;
        model.clocks_running = (gp & CSR_GP_MAC_ACCESS_REQ) != 0;
        if (model.clocks_running) reg_set(CSR_GP_CNTRL, gp | CSR_GP_MAC_CLOCK_READY);
        else reg_set(CSR_GP_CNTRL, gp & ~CSR_GP_MAC_CLOCK_READY);
    }

    /* The reset register: stopping the bus master, and then the release that
     * starts the microcode. */
    u32 reset = reg_get(CSR_RESET);
    if (reset != previous_reset) {
        bool was_reset = (previous_reset & CSR_RESET_SW) != 0;
        previous_reset = reset;

        if (reset & CSR_RESET_STOP_MASTER) {
            model.master_stopped = true;
            reg_set(CSR_RESET, reset | CSR_RESET_MASTER_DISABLED);
        }
        if (reset & CSR_RESET_SW) {
            model.microcode_running = false;
            model.bytes_loaded = 0;
        }
        if (was_reset && !(reset & CSR_RESET_SW)) {
            /* Released: the microcode starts, and the first thing it does is
             * say so. */
            model.microcode_running = true;
            iwl_alive_t alive;
            memset(&alive, 0, sizeof alive);
            alive.status = IWL_ALIVE_STATUS_OK;
            alive.ucode_major = 9;
            alive.ucode_minor = 3;
            alive.api_major = 50;
            alive.api_minor = 0;
            alive.timestamp = (u32)g_uptime_ms;
            deliver(IWL_CMD_ALIVE, 0, &alive, (int)sizeof alive);
            raise_interrupt(CSR_INT_BIT_ALIVE);
            kinfo("iwl-model", "the microcode started; %u bytes had been loaded",
                  model.bytes_loaded);
        }
    }

    /* Writing into the device's own memory: an address register followed by a
     * run of writes to the data register.  Only the count matters here. */
    u32 waddr = reg_get(HBUS_TARG_MEM_WADDR);
    if (waddr != model.mem_write_address) {
        model.mem_write_address = waddr;
    }

    /* Where the rings are. */
    u32 rx_bd = reg_get(FH_RSCSR_CHNL0_RBDCB_BASE);
    if (rx_bd) model.rx_bd_phys = (u64)rx_bd << 8;
    u32 rx_status = reg_get(FH_RSCSR_CHNL0_STTS_WPTR);
    if (rx_status) model.rx_status_phys = (u64)rx_status << 4;

    u32 rx_config = reg_get(FH_MEM_RCSR_CHNL0_CONFIG);
    if (rx_config != previous_rx_config) {
        previous_rx_config = rx_config;
        model.rx_enabled = (rx_config & FH_RCSR_RX_CONFIG_ENABLE) != 0;
    }

    for (int q = 0; q < 32; q++) {
        u32 base = reg_get(FH_MEM_CBBC_QUEUE_BASE + q * 4);
        if (base) model.queue_phys[q] = (u64)base << 8;
    }

    /* The doorbell: which queue, and how far the driver has filled it. */
    u32 doorbell = reg_get(HBUS_TARG_WRPTR);
    if (doorbell != previous_doorbell) {
        previous_doorbell = doorbell;
        if (model.microcode_running)
            run_queue((int)((doorbell >> 8) & 0x1F), (u16)(doorbell & 0xFF));
    }
}

/* ------------------------------------------------------------- bringing up */

/* Called when no Intel card was found.  Returns the register space the driver
 * should use, or NULL if a model is not wanted. */
volatile u8 *iwl_model_attach(void) {
    if (model.present) return model.regs;

    memset(register_space, 0, sizeof register_space);
    memset(&model, 0, sizeof model);
    model.present = true;
    model.regs = register_space;
    model.channel = MODEL_CHANNEL;

    /* The hardware revision a driver reads to know what it is talking to. */
    reg_set(CSR_HW_REV, 0x00000210);

    previous_reset = previous_doorbell = previous_config = previous_gp = 0;
    previous_rx_config = 0;

    kinfo("iwl-model", "no Intel card present; a model is standing in so the "
                       "driver runs");
    return model.regs;
}

bool iwl_model_running(void) { return model.present && model.microcode_running; }
bool iwl_model_answered_probe(void) { return model.answered_probe; }
u32  iwl_model_bytes_loaded(void) { return model.bytes_loaded; }

/* Counting the microcode as it arrives.  The driver writes it one word at a
 * time through the data register, which is a write the model cannot see any
 * other way. */
void iwl_model_note_write(u32 count) { model.bytes_loaded += count * 4; }
