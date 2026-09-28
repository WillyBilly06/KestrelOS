/* amd_model.c - an AMD graphics card that is not there.
 *
 * Nothing this system can be run on has one: VMware, QEMU and VirtualBox all
 * present their own display adapter, and the machine this was written on has
 * its card claimed by the host.  So without something standing in, the driver
 * next door could be written and never once executed - and code that has never
 * run is not a driver, it is a description of one.
 *
 * This is that something.  It is not a stub.  It holds the same state a card
 * holds and it is deliberately strict about the order things happen in:
 *
 *   It carries a real ATOM BIOS image - option ROM header, ROM header, master
 *   data table, GPIO table, object table, records - built to AMD's layout and
 *   read out of its memory the same way a driver reads a card's, one dword at
 *   a time through an index/data pair.
 *
 *   It carries a real discovery table, checksummed, so a driver that reads it
 *   wrongly gets a checksum failure rather than plausible nonsense.
 *
 *   Its power processor refuses a message whose argument was written after the
 *   message, because a real one would have read the previous argument.  That
 *   is the ordering bug this protocol invites, and a model that tolerates it
 *   proves nothing.
 *
 *   Its security processor refuses the firmware image, because nothing anybody
 *   outside AMD can produce is signed by AMD.  A model that pretended
 *   otherwise would be reporting a capability that does not exist.
 *
 *   There is a monitor on one connector, and it answers the two wires the
 *   driver bit-bangs - start conditions, bit order, acknowledgements, repeated
 *   start - with a real identification block.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: that the discovery table is walked correctly, that the
 * ATOM tables are followed and bounds-checked, that the connector list and the
 * pin assignments come out right, that the two-wire protocol is driven well
 * enough for a device on the far end to answer, that the mailbox protocols are
 * driven in the right order, and that the packet ring is built with counts that
 * match what was written.
 *
 * What it cannot establish: that these register numbers are the ones AMD's
 * silicon uses.  They come from AMD's published headers, but a model written
 * from the same headers as the driver checks the driver against that reading,
 * not against the world.  That distinction is stated wherever this is reported.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "amd.h"

/* A Navi 31 - the chip in a Radeon RX 7900 XTX - because it is recent enough
 * to have everything the modern path needs: a discovery table, both
 * co-processors, and no fixed register layout to fall back on. */
#define MODEL_DEVICE      0x744C
#define MODEL_REVISION    0xC8
#define MODEL_VRAM_MB     24576u
#define MODEL_VRAM        ((u64)MODEL_VRAM_MB << 20)
#define MODEL_TEMP_C      61
#define MODEL_GFX_MHZ     2400
#define MODEL_MEM_MHZ     2500
#define MODEL_SMU_VERSION 0x004D5301u        /* 77.83.1                      */

/* Where the discovery table says the blocks are.  Dword offsets, chosen to sit
 * inside the window below so that every one of them is genuinely reachable. */
#define BASE_GC     0x00002000u
#define BASE_DCI    0x00002400u
#define BASE_SDMA0  0x00001260u
#define BASE_MP0    0x00016000u
#define BASE_MP1    0x00016400u
#define BASE_I2C    0x00001000u              /* the GPIO pins for the wires   */

#define WINDOW_BYTES 0x100000                /* one megabyte of registers     */

/* The two parts of video memory this model actually backs.  A card has
 * gigabytes; a driver bringing one up touches these two places. */
#define ATOM_BYTES      (64 * 1024)
#define DISCOVERY_BYTES (16 * 1024)

#define MONITOR_LINE 2                       /* the first DisplayPort         */

static u8 *window;
static u8 atom_image[ATOM_BYTES];
static u8 discovery[DISCOVERY_BYTES];

/* ------------------------------------------------------------------- state */

enum { I2C_IDLE, I2C_RX, I2C_ACK, I2C_TX, I2C_TX_ACK };

typedef struct {
    bool driver_scl_low, driver_sda_low;
    bool device_sda_low;
    bool prev_scl, prev_sda;

    int  state;
    u8   shift;
    int  bits;
    bool first_byte;
    bool reading;
    bool expect_offset;
    u8   tx;
    int  tx_bits;
    u8   offset;
    bool master_acked;

    /* What actually happened on this bus, so a test can say where a transfer
     * stopped rather than only that it did. */
    int  starts, stops, acks, bytes_out;
} i2c_bus_t;

static struct {
    bool present;

    /* The index/data pair, mid-transaction. */
    u64  vram_pointer;
    u32  index_hi;
    bool index_is_vram;
    u32  register_pointer;

    /* The power processor. */
    bool smu_arg_fresh;      /* the argument was written since the last clear */
    int  smu_messages;
    int  smu_out_of_order;
    u32  smu_last_message;

    /* The security processor. */
    bool psp_arg_fresh;
    int  psp_loads;
    int  psp_rejections;
    bool sos_running;

    /* The packet ring. */
    u32 *ring;
    u32  ring_words;
    volatile u32 *ring_rptr;
    volatile u64 *ring_fence;
    u32  ring_read;
    u32  ring_doorbell;
    int  ring_packets;
    u32  ring_total_words;
    u32  ring_last_opcode;
    int  ring_errors;

    i2c_bus_t i2c[AMD_MAX_I2C_BUSES];
} model;

static u8 monitor_edid[128];

static u32 reg_get(u32 offset) {
    if (!window || offset + 4 > WINDOW_BYTES) return 0;
    return *(volatile u32 *)(window + offset);
}

static void reg_set(u32 offset, u32 value) {
    if (!window || offset + 4 > WINDOW_BYTES) return;
    *(volatile u32 *)(window + offset) = value;
}

/* ------------------------------------------------------------ the monitor */

static void build_edid(void) {
    u8 *e = monitor_edid;
    memset(e, 0, 128);

    static const u8 header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(e, header, 8);

    /* "KES" packed five bits per letter, which is how a manufacturer
     * identifier is stored. */
    u16 maker = (u16)((('K' - 'A' + 1) << 10) | (('E' - 'A' + 1) << 5) |
                      ('S' - 'A' + 1));
    e[8] = (u8)(maker >> 8);
    e[9] = (u8)maker;
    e[10] = 0x01; e[11] = 0x40;      /* product code                        */
    e[16] = 32; e[17] = 34;          /* week and year of manufacture        */
    e[18] = 1; e[19] = 4;            /* EDID 1.4                            */
    e[20] = 0xA0;                    /* digital input, 8 bits per colour    */
    e[21] = 70; e[22] = 39;          /* 70 by 39 centimetres                */
    e[24] = 0x06;

    /* The preferred timing: 3840 by 2160 at sixty. */
    u32 pixel_khz = 594000;          /* 594 MHz                             */
    u16 clock = (u16)(pixel_khz / 10);
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
    d[8] = 176; d[9] = 88; d[10] = 0x18;    /* sync offsets and widths      */
    d[11] = 0; d[12] = 0; d[13] = 0;
    d[14] = 0; d[15] = 0; d[16] = 0;
    d[17] = 0x1E;

    /* The name. */
    d = e + 72;
    d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0xFC; d[4] = 0;
    memcpy(d + 5, "Kestrel UHD", 11);
    d[16] = 0x0A; d[17] = 0x20;

    u8 sum = 0;
    for (int i = 0; i < 127; i++) sum = (u8)(sum + e[i]);
    e[127] = (u8)(0x100 - sum);
}

/* ------------------------------------------------------------- the ATOM BIOS
 *
 * Built to AMD's layout, because a model that lays it out any other way tests
 * the driver against a fiction.  Every offset below is one the driver will
 * follow, and every pointer is written where the structure definition says the
 * pointer lives.
 */
#define ROM_HEADER_AT   0x0100
#define VERSION_AT      0x0200
#define MASTER_AT       0x0300
#define GPIO_I2C_AT     0x0400
#define OBJECT_AT       0x0600
#define CONNECTOR_AT    0x0700
#define RECORDS_AT      0x0800

#define MODEL_CONNECTORS 4
#define MODEL_I2C_LINES  4

static void put16(u8 *at, u16 value) { at[0] = (u8)value; at[1] = (u8)(value >> 8); }

static void build_atom(void) {
    u8 *b = atom_image;
    memset(b, 0, ATOM_BYTES);

    /* The option ROM header every PCI expansion ROM starts with. */
    put16(b + 0, AMD_ATOM_BIOS_MAGIC);
    b[2] = ATOM_BYTES / 512;
    memcpy(b + AMD_ATOM_ATI_MAGIC_PTR, AMD_ATOM_ATI_MAGIC, 10);
    put16(b + AMD_ATOM_ROM_TABLE_PTR, ROM_HEADER_AT);

    /* The ROM header, whose whole purpose is to say where the two lists of
     * tables are. */
    u8 *h = b + ROM_HEADER_AT;
    put16(h + 0, 0x30);                       /* structure size              */
    h[2] = 2; h[3] = 2;                       /* revisions                   */
    memcpy(h + AMD_ATOM_ROM_MAGIC_PTR, AMD_ATOM_ROM_MAGIC, 4);
    put16(h + AMD_ATOM_ROM_MSG_PTR, VERSION_AT);
    put16(h + AMD_ATOM_ROM_CMD_PTR, 0x0900);
    put16(h + AMD_ATOM_ROM_DATA_PTR, MASTER_AT);

    memcpy(b + VERSION_AT, "113-D7020100-102 / Navi 31 / Kestrel", 37);

    /* The master data table: a header, then one sixteen-bit offset per table.
     * The positions are struct field positions, so they are fixed. */
    u8 *m = b + MASTER_AT;
    u16 entries = 36;
    put16(m + 0, (u16)(4 + entries * 2));
    m[2] = 1; m[3] = 1;
    put16(m + 4 + AMD_ATOM_TABLE_GPIO_I2C_INFO * 2, GPIO_I2C_AT);
    put16(m + 4 + AMD_ATOM_TABLE_OBJECT_HEADER * 2, OBJECT_AT);

    /* GPIO_I2C_Info: which registers and which bits carry each bus. */
    u8 *g = b + GPIO_I2C_AT;
    put16(g + 0, (u16)(4 + MODEL_I2C_LINES * sizeof(amd_i2c_assignment_t)));
    g[2] = 1; g[3] = 2;

    for (int line = 0; line < MODEL_I2C_LINES; line++) {
        amd_i2c_assignment_t a;
        memset(&a, 0, sizeof a);
        u16 base = (u16)(BASE_I2C + line * 8);

        a.clk_mask_reg  = base + 0;
        a.clk_en_reg    = base + 1;
        a.clk_y_reg     = base + 2;
        a.clk_a_reg     = base + 3;
        a.data_mask_reg = base + 4;
        a.data_en_reg   = base + 5;
        a.data_y_reg    = base + 6;
        a.data_a_reg    = base + 7;
        a.i2c_id = (u8)line;

        /* Deliberately not zero, and deliberately different for the two
         * wires: a driver that ignores the shifts still works when they are
         * all zero, which is exactly why they are not. */
        a.clk_mask_shift = a.clk_en_shift = a.clk_y_shift = a.clk_a_shift = 3;
        a.data_mask_shift = a.data_en_shift = a.data_y_shift = a.data_a_shift = 5;

        memcpy(g + 4 + line * sizeof a, &a, sizeof a);
    }

    /* The object table: a header pointing at a connector list. */
    u8 *o = b + OBJECT_AT;
    put16(o + 0, 16);
    o[2] = 1; o[3] = 1;
    put16(o + 4, 0x000F);                     /* usDeviceSupport             */
    put16(o + 6, CONNECTOR_AT);               /* connectors                  */
    put16(o + 8, 0);                          /* routers                     */
    put16(o + 10, 0);                         /* encoders                    */
    put16(o + 12, 0);                         /* protection                  */
    put16(o + 14, 0);                         /* display paths               */

    /* Four connectors, the way a card of this class is actually built. */
    static const struct { u8 id; u8 enumeration; u8 line; } sockets[MODEL_CONNECTORS] = {
        { AMD_CONNECTOR_VGA,         1, 0 },
        { AMD_CONNECTOR_HDMI_TYPE_A, 1, 1 },
        { AMD_CONNECTOR_DISPLAYPORT, 1, 2 },
        { AMD_CONNECTOR_DISPLAYPORT, 2, 3 },
    };

    u8 *ct = b + CONNECTOR_AT;
    ct[0] = MODEL_CONNECTORS;

    for (int i = 0; i < MODEL_CONNECTORS; i++) {
        u16 object_id = (u16)((AMD_OBJECT_TYPE_CONNECTOR << AMD_OBJECT_TYPE_SHIFT) |
                              (sockets[i].enumeration << AMD_ENUM_ID_SHIFT) |
                              sockets[i].id);
        u16 record_at = (u16)(RECORDS_AT + i * 8);

        u8 *obj = ct + 4 + i * 8;
        put16(obj + 0, object_id);
        put16(obj + 2, 0);                    /* source/destination table    */
        put16(obj + 4, record_at);
        put16(obj + 6, 0);

        /* One record saying which bus this connector's monitor answers on,
         * then the end marker. */
        u8 *rec = b + record_at;
        rec[0] = AMD_ATOM_I2C_RECORD_TYPE;
        rec[1] = 4;
        rec[2] = sockets[i].line;
        rec[3] = 0;
        rec[4] = AMD_ATOM_RECORD_END_TYPE;
        rec[5] = 2;
    }
}

/* --------------------------------------------------------- the discovery table */

static void append_ip(u8 *at, u32 *walk, u16 hw_id, u8 instance,
                      u8 major, u8 minor, u8 revision, u32 base) {
    u8 *e = at + *walk;
    put16(e + 0, hw_id);
    e[2] = instance;
    e[3] = 1;                                 /* one base address            */
    e[4] = major; e[5] = minor; e[6] = revision;
    e[7] = 0;
    e[8] = (u8)base; e[9] = (u8)(base >> 8);
    e[10] = (u8)(base >> 16); e[11] = (u8)(base >> 24);
    *walk += 12;
}

static void build_discovery(void) {
    u8 *b = discovery;
    memset(b, 0, DISCOVERY_BYTES);

    u32 ipd_at = 0x24;
    u32 die_at = ipd_at + 0x50;

    /* The block list header. */
    u8 *ipd = b + ipd_at;
    ipd[0] = (u8)AMD_DISCOVERY_SIGNATURE;
    ipd[1] = (u8)(AMD_DISCOVERY_SIGNATURE >> 8);
    ipd[2] = (u8)(AMD_DISCOVERY_SIGNATURE >> 16);
    ipd[3] = (u8)(AMD_DISCOVERY_SIGNATURE >> 24);
    put16(ipd + 4, 1);                        /* version                     */
    put16(ipd + 6, 0x50);                     /* its own size                */
    put16(ipd + 0x0C, 1);                     /* one die                     */
    put16(ipd + 0x0E, 0);                     /* die 0's identifier          */
    put16(ipd + 0x10, (u16)die_at);           /* and where its header is     */

    /* The die, and the blocks on it. */
    u8 *die = b + die_at;
    put16(die + 0, 0);

    u32 walk = die_at + 4;
    int blocks = 0;

    append_ip(b, &walk, AMD_HWID_GC,     0, 11, 0, 0, BASE_GC);     blocks++;
    append_ip(b, &walk, AMD_HWID_DCI,    0,  3, 2, 0, BASE_DCI);    blocks++;
    append_ip(b, &walk, AMD_HWID_SDMA0,  0,  6, 0, 0, BASE_SDMA0);  blocks++;
    append_ip(b, &walk, AMD_HWID_MP0,    0, 13, 0, 0, BASE_MP0);    blocks++;
    append_ip(b, &walk, AMD_HWID_MP1,    0, 13, 0, 0, BASE_MP1);    blocks++;
    append_ip(b, &walk, AMD_HWID_HDP,    0,  6, 0, 0, 0x00000F80);  blocks++;
    append_ip(b, &walk, AMD_HWID_ATHUB,  0,  3, 0, 0, 0x00000C20);  blocks++;
    append_ip(b, &walk, AMD_HWID_UMC,    0,  8, 10, 0, 0x00014000); blocks++;
    append_ip(b, &walk, AMD_HWID_VCN,    0,  4, 0, 0, 0x00001FC0);  blocks++;

    put16(die + 2, (u16)blocks);

    u32 size = walk;

    /* The outer header, last, because it carries the length and the checksum
     * of everything above. */
    b[0] = (u8)AMD_BINARY_SIGNATURE;
    b[1] = (u8)(AMD_BINARY_SIGNATURE >> 8);
    b[2] = (u8)(AMD_BINARY_SIGNATURE >> 16);
    b[3] = (u8)(AMD_BINARY_SIGNATURE >> 24);
    put16(b + 4, 1);                          /* version                     */
    put16(b + 6, 0);
    put16(b + 0x0A, (u16)size);
    put16(b + 0x0C, (u16)ipd_at);             /* table 0: the block list     */
    put16(b + 0x10, (u16)(size - ipd_at));

    u16 sum = 0;
    for (u32 i = 0x24; i < size; i++) sum = (u16)(sum + b[i]);
    put16(b + 8, sum);
}

/* ------------------------------------------------------------- video memory
 *
 * Two regions are backed and everything else reads as zero, which is what a
 * driver that walked off the end of a table would get - so a walk that goes
 * wrong fails a signature check rather than finding something plausible.
 */
static u32 vram_word(u64 at) {
    u8 word[4] = { 0, 0, 0, 0 };

    if (at + 4 <= ATOM_BYTES) {
        memcpy(word, atom_image + at, 4);
    } else {
        u64 discovery_at = MODEL_VRAM - AMD_DISCOVERY_TMR_OFFSET;
        if (at >= discovery_at && at + 4 <= discovery_at + DISCOVERY_BYTES)
            memcpy(word, discovery + (at - discovery_at), 4);
    }

    return (u32)word[0] | ((u32)word[1] << 8) | ((u32)word[2] << 16) |
           ((u32)word[3] << 24);
}

/* ----------------------------------------------------------------- the wires
 *
 * The device on the far end of one of the buses, as it experiences the
 * transfer.  It is driven entirely by edges, because that is all a device on a
 * two-wire bus can see.
 */
static void i2c_update_readback(int line) {
    i2c_bus_t *bus = &model.i2c[line];
    u32 base = BASE_I2C + (u32)line * 8;

    bool scl_high = !bus->driver_scl_low;
    bool sda_high = !(bus->driver_sda_low || bus->device_sda_low);

    u32 clk_y = reg_get((base + 2) * 4) & ~(1u << 3);
    if (scl_high) clk_y |= (1u << 3);
    reg_set((base + 2) * 4, clk_y);

    u32 data_y = reg_get((base + 6) * 4) & ~(1u << 5);
    if (sda_high) data_y |= (1u << 5);
    reg_set((base + 6) * 4, data_y);
}

static void i2c_tx_load(i2c_bus_t *bus) {
    bus->tx = monitor_edid[bus->offset & 0x7F];
    bus->offset++;
    bus->tx_bits = 0;
}

static void i2c_step(int line) {
    i2c_bus_t *bus = &model.i2c[line];

    bool scl = !bus->driver_scl_low;
    bool sda = !(bus->driver_sda_low || bus->device_sda_low);

    /* A start or a stop is a change on the data line while the clock is high;
     * every other data change happens while it is low. */
    if (bus->prev_scl && scl) {
        if (bus->prev_sda && !sda) {
            bus->starts++;
            bus->state = I2C_RX;
            bus->bits = 0;
            bus->shift = 0;
            bus->first_byte = true;
            bus->device_sda_low = false;
        } else if (!bus->prev_sda && sda) {
            bus->stops++;
            bus->state = I2C_IDLE;
            bus->device_sda_low = false;
        }
    }

    bool rising = !bus->prev_scl && scl;
    bool falling = bus->prev_scl && !scl;

    if (rising) {
        switch (bus->state) {
        case I2C_RX:
            bus->shift = (u8)((bus->shift << 1) | (sda ? 1 : 0));
            bus->bits++;
            break;
        case I2C_TX_ACK:
            /* Whether the other side wants another byte. */
            bus->master_acked = !sda;
            break;
        default:
            break;
        }
    }

    if (falling) {
        switch (bus->state) {
        case I2C_RX:
            if (bus->bits < 8) break;

            if (bus->first_byte) {
                /* Only this address is ours, and only on the bus a monitor is
                 * actually plugged into.  A device that answers on a bus it is
                 * not on - or to an address that is not its own - is the reason
                 * a bus with two devices stops working, and a model that does
                 * it reports monitors on connectors with nothing in them. */
                if (line == MONITOR_LINE && (bus->shift & 0xFE) == 0xA0) {
                    bus->reading = (bus->shift & 1) != 0;
                    bus->device_sda_low = true;
                    bus->acks++;
                    bus->state = I2C_ACK;
                } else {
                    bus->state = I2C_IDLE;
                }
            } else {
                bus->device_sda_low = true;
                bus->acks++;
                bus->state = I2C_ACK;
            }
            break;

        case I2C_ACK:
            bus->device_sda_low = false;

            if (bus->first_byte) {
                bus->first_byte = false;
                if (bus->reading) {
                    /* The first bit goes out on this same edge - the edge that
                     * released the acknowledgement.  A device that waits for
                     * the next one is a bit late for the whole transfer. */
                    i2c_tx_load(bus);
                    bus->device_sda_low = !((bus->tx >> 7) & 1);
                    bus->tx_bits = 1;
                    bus->state = I2C_TX;
                } else {
                    bus->expect_offset = true;
                    bus->bits = 0;
                    bus->shift = 0;
                    bus->state = I2C_RX;
                }
            } else {
                if (bus->expect_offset) {
                    bus->offset = bus->shift;
                    bus->expect_offset = false;
                }
                bus->bits = 0;
                bus->shift = 0;
                bus->state = I2C_RX;
            }
            break;

        case I2C_TX:
            if (bus->tx_bits < 8) {
                bus->device_sda_low = !((bus->tx >> (7 - bus->tx_bits)) & 1);
                bus->tx_bits++;
            } else {
                bus->device_sda_low = false;   /* let them answer */
                bus->bytes_out++;
                bus->state = I2C_TX_ACK;
            }
            break;

        case I2C_TX_ACK:
            if (bus->master_acked) {
                i2c_tx_load(bus);
                bus->device_sda_low = !((bus->tx >> 7) & 1);
                bus->tx_bits = 1;
                bus->state = I2C_TX;
            } else {
                bus->device_sda_low = false;
                bus->state = I2C_IDLE;
            }
            break;

        default:
            break;
        }
    }

    bus->prev_scl = scl;
    bus->prev_sda = !(bus->driver_sda_low || bus->device_sda_low);
    i2c_update_readback(line);
}

/* ------------------------------------------------------------- the packets */

static void ring_consume(u32 write_to) {
    if (!model.ring) return;

    u32 mask = model.ring_words - 1;

    while (model.ring_read != (write_to & mask)) {
        u32 header = model.ring[model.ring_read];
        u32 type = header >> 30;

        if (type != 3) {
            kwarn("amd-model", "the ring holds a type %u packet where a type 3 "
                               "was expected", type);
            model.ring_errors++;
            model.ring_read = write_to & mask;
            break;
        }

        u32 opcode = (header >> 8) & 0xFF;
        u32 count = ((header >> 16) & 0x3FFF) + 1;   /* the header's own word
                                                       is not counted        */
        u32 total = count + 1;

        u32 available = (write_to - model.ring_read) & mask;
        if (total > available) {
            kwarn("amd-model", "a packet claims %u words and only %u were "
                               "revealed", total, available);
            model.ring_errors++;
            model.ring_read = write_to & mask;
            break;
        }

        /* The two that do something. */
        if (opcode == AMD_PM4_RELEASE_MEM && total >= 8) {
            u32 at_lo = model.ring[(model.ring_read + 3) & mask];
            u32 at_hi = model.ring[(model.ring_read + 4) & mask];
            u32 lo = model.ring[(model.ring_read + 5) & mask];
            u32 hi = model.ring[(model.ring_read + 6) & mask];
            (void)at_lo; (void)at_hi;

            /* The completion lands where the driver said, which is the only
             * way it ever learns that work finished. */
            if (model.ring_fence)
                *model.ring_fence = ((u64)hi << 32) | lo;
        }

        model.ring_packets++;
        model.ring_total_words += total;
        model.ring_last_opcode = opcode;
        model.ring_read = (model.ring_read + total) & mask;
    }

    if (model.ring_rptr) *model.ring_rptr = model.ring_read;
}

void amd_model_ring_attach(u32 *memory, u32 words, volatile u32 *rptr,
                           volatile u64 *fence, u32 doorbell) {
    model.ring = memory;
    model.ring_words = words;
    model.ring_rptr = rptr;
    model.ring_fence = fence;
    model.ring_doorbell = doorbell;
    model.ring_read = 0;
    model.ring_packets = 0;
    model.ring_total_words = 0;
    model.ring_errors = 0;
}

void amd_model_ring_detach(void) { model.ring = NULL; }

/* ------------------------------------------------------------- the mailboxes */

static void smu_message(u32 message) {
    u32 arg_at = (BASE_MP1 + AMD_SMU_ARG) * 4;
    u32 resp_at = (BASE_MP1 + AMD_SMU_RESP) * 4;

    model.smu_messages++;
    model.smu_last_message = message;

    /* The argument has to have been written since the response was cleared.
     * Otherwise a real processor would read whatever the last message left,
     * and this one says so rather than covering for it. */
    if (!model.smu_arg_fresh) {
        model.smu_out_of_order++;
        reg_set(resp_at, AMD_SMU_RESP_BAD_PREREQ);
        return;
    }
    model.smu_arg_fresh = false;

    u32 argument = reg_get(arg_at);

    switch (message) {
    case AMD_SMU_MSG_TEST:
        reg_set(arg_at, argument + 1);
        reg_set(resp_at, AMD_SMU_RESP_OK);
        break;

    case AMD_SMU_MSG_GET_SMU_VERSION:
        reg_set(arg_at, MODEL_SMU_VERSION);
        reg_set(resp_at, AMD_SMU_RESP_OK);
        break;

    case AMD_SMU_MSG_GET_METRICS_TABLE:
        reg_set(arg_at, (u32)MODEL_TEMP_C |
                        ((u32)(MODEL_GFX_MHZ / 10) << 8) |
                        ((u32)(MODEL_MEM_MHZ / 10) << 20));
        reg_set(resp_at, AMD_SMU_RESP_OK);
        break;

    default:
        /* Not a message it knows.  Answering anything else would let a driver
         * that guessed a number believe it worked. */
        reg_set(resp_at, AMD_SMU_RESP_UNKNOWN);
        break;
    }
}

static void psp_command(u32 command) {
    u32 cmd_at = (BASE_MP0 + AMD_PSP_BL_CMD) * 4;

    if (command == 0) {                       /* the driver clearing it      */
        model.psp_arg_fresh = false;
        return;
    }

    model.psp_loads++;

    if (!model.psp_arg_fresh) {
        /* No address was written, so there is nothing to load from. */
        reg_set(cmd_at, AMD_PSP_BL_READY | 0x0002);
        return;
    }
    model.psp_arg_fresh = false;

    /* And here is the wall.  The security processor checks a signature over
     * the image and nothing outside AMD holds the key.  Reporting success
     * would be reporting a capability that does not exist. */
    model.psp_rejections++;
    reg_set(cmd_at, AMD_PSP_BL_READY | 0x0001);
}

/* ----------------------------------------------------------- the interface */

void amd_model_sync(void) { /* everything here is driven by the writes below */ }

void amd_model_wrote(u32 offset, u32 value) {
    if (!model.present) return;

    /* The index/data pair. */
    if (offset == AMD_MM_INDEX_HI) { model.index_hi = value; return; }

    if (offset == AMD_MM_INDEX) {
        if (value & AMD_MM_INDEX_VRAM) {
            model.index_is_vram = true;
            model.vram_pointer = ((u64)model.index_hi << 31) |
                                 (value & 0x7FFFFFFFu);
        } else {
            model.index_is_vram = false;
            model.register_pointer = value;
        }
        return;
    }

    if (offset == AMD_MM_DATA) {
        if (!model.index_is_vram) reg_set(model.register_pointer, value);
        return;
    }

    /* The two-wire buses.  Only the output-enable registers change what the
     * lines do; the others are read back. */
    for (int line = 0; line < MODEL_I2C_LINES; line++) {
        u32 base = (BASE_I2C + (u32)line * 8) * 4;
        if (offset == base + 1 * 4) {
            model.i2c[line].driver_scl_low = (value >> 3) & 1;
            i2c_step(line);
            return;
        }
        if (offset == base + 5 * 4) {
            model.i2c[line].driver_sda_low = (value >> 5) & 1;
            i2c_step(line);
            return;
        }
    }

    /* The power processor. */
    if (offset == (BASE_MP1 + AMD_SMU_ARG) * 4) { model.smu_arg_fresh = true; return; }
    if (offset == (BASE_MP1 + AMD_SMU_RESP) * 4) { return; }
    if (offset == (BASE_MP1 + AMD_SMU_MSG) * 4) { smu_message(value); return; }

    /* The security processor. */
    if (offset == (BASE_MP0 + AMD_PSP_BL_ARG) * 4) { model.psp_arg_fresh = true; return; }
    if (offset == (BASE_MP0 + AMD_PSP_BL_CMD) * 4) { psp_command(value); return; }
    if (offset == (BASE_MP0 + AMD_PSP_RING_CMD) * 4) {
        /* A ring can only be created once the secure OS is running. */
        u32 at = (BASE_MP0 + AMD_PSP_RING_CMD) * 4;
        reg_set(at, AMD_PSP_RESP_FLAG | (model.sos_running ? 0 : 0x0003));
        return;
    }

    /* The doorbell. */
    if (model.ring && offset == model.ring_doorbell) {
        ring_consume(value);
        return;
    }
}

void amd_model_read(u32 offset) {
    if (!model.present) return;

    if (offset == AMD_MM_DATA) {
        reg_set(AMD_MM_DATA, model.index_is_vram
                                 ? vram_word(model.vram_pointer)
                                 : reg_get(model.register_pointer));
        return;
    }
}

/* ------------------------------------------------------------- bringing up */

volatile u8 *amd_model_attach(size_t *size_out) {
    if (model.present) {
        if (size_out) *size_out = WINDOW_BYTES;
        return window;
    }

    u64 phys;
    window = dma_alloc_pages(WINDOW_BYTES / PAGE_SIZE, &phys);
    if (!window) {
        kwarn("amd-model", "no memory for the register window");
        return NULL;
    }
    memset(window, 0, WINDOW_BYTES);

    memset(&model, 0, sizeof model);
    model.present = true;

    build_edid();
    build_atom();
    build_discovery();

    /* How much memory is fitted, in the register that has held it since
     * Southern Islands. */
    reg_set(AMD_CONFIG_MEMSIZE, MODEL_VRAM_MB);

    /* Both wires of every bus idle high, which with open-drain means nobody is
     * driving them. */
    for (int line = 0; line < MODEL_I2C_LINES; line++) {
        model.i2c[line].prev_scl = true;
        model.i2c[line].prev_sda = true;
        model.i2c[line].state = I2C_IDLE;
        i2c_update_readback(line);
    }

    /* The security processor's bootloader is ready before anything else runs;
     * that is the state a card comes out of reset in. */
    reg_set((BASE_MP0 + AMD_PSP_BL_CMD) * 4, AMD_PSP_BL_READY);

    kinfo("amd-model", "no AMD card present; a model of one is standing in so "
                       "the driver runs");
    if (size_out) *size_out = WINDOW_BYTES;
    return window;
}

bool amd_model_present(void) { return model.present; }

/* The one thing a real machine's firmware does that this has to be told to do:
 * on a machine that POSTed the card, the security processor is already running
 * by the time a driver loads. */
void amd_model_sos_already_running(bool running) {
    model.sos_running = running;
    reg_set((BASE_MP0 + AMD_PSP_SOS_ALIVE) * 4, running ? 1 : 0);
}

/* What the model was built to be, and what it saw. */
u16  amd_model_device(void)      { return MODEL_DEVICE; }
u64  amd_model_vram(void)        { return MODEL_VRAM; }
int  amd_model_temperature(void) { return MODEL_TEMP_C; }
const u8 *amd_model_edid(void)   { return monitor_edid; }
u8   amd_model_monitor_line(void){ return MONITOR_LINE; }
u32  amd_model_smu_messages(void){ return (u32)model.smu_messages; }
u32  amd_model_psp_loads(void)   { return (u32)model.psp_loads; }
u32  amd_model_ring_packets(void){ return (u32)model.ring_packets; }
u32  amd_model_ring_words(void)  { return model.ring_total_words; }
u32  amd_model_last_opcode(void) { return model.ring_last_opcode; }
int  amd_model_ring_errors(void) { return model.ring_errors; }
int  amd_model_smu_out_of_order(void) { return model.smu_out_of_order; }
int  amd_model_psp_rejections(void)   { return model.psp_rejections; }
void amd_model_i2c_report(int line, int *starts, int *acks, int *bytes) {
    if (line < 0 || line >= AMD_MAX_I2C_BUSES) return;
    if (starts) *starts = model.i2c[line].starts;
    if (acks) *acks = model.i2c[line].acks;
    if (bytes) *bytes = model.i2c[line].bytes_out;
}
