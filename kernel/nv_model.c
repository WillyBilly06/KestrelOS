/* nv_model.c - a model of an NVIDIA graphics card.
 *
 * Nothing this system can be run on has one in it: VMware presents its own
 * display adapter, and so do QEMU and VirtualBox.  So the driver would have
 * nowhere to run and nothing to be checked against.
 *
 * This stands in.  It answers the same register window the driver reads, holds
 * a video BIOS built to the same layout a real one has - option ROM header,
 * BIT tables, connector table - and puts a monitor on one of the connectors
 * that answers the two-wire protocol the driver bit-bangs, one clock edge at a
 * time, with a real identification block.
 *
 * The driver that runs against it is the same driver that would run against a
 * card.  Nothing in it is switched off for the model; the only thing it knows
 * is which side of the bus it is talking to, and that only so the model can
 * catch up before a register is read.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: that the card is identified from PMC_BOOT_0
 * correctly, that the ROM is found and its length read, that the BIT tables
 * are walked and the version string recovered, that the connector table is
 * parsed into the right outputs with the right buses, that the two-wire
 * protocol is driven correctly enough for a device on the far end to answer -
 * start and stop conditions, bit order, acknowledgements - and that a
 * monitor's identification block is read back and understood.
 *
 * What it cannot establish: that these register offsets are the ones NVIDIA's
 * silicon uses.  They come from documentation, and a model written from the
 * same understanding as the driver checks that understanding against itself
 * rather than against the world.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

/* ---------------------------------------------------------------- the card
 *
 * A GP104 - the chip in a GTX 1070 - because it is the newest generation whose
 * display engine still answers to registers, so the whole driver runs rather
 * than stopping at the firmware boundary.
 */
#define MODEL_BOOT0     0x134000A1u        /* Pascal, GP104, revision A1      */
#define MODEL_VRAM      (8ULL << 30)
#define MODEL_TEMP_C    47

/* The register window is sixteen megabytes and a driver touches a few dozen
 * places in it, but those places are spread from zero to the channel block at
 * 0x800000 - which is where a channel's ring is armed, so it has to be inside
 * the window rather than one byte past the end of it.  Backing the whole thing
 * is simpler than any scheme for backing part of it. */
#define WINDOW_BYTES 0x900000

static u8 *window;

/* One bus, as the device on the far end of it experiences the transfer. */
typedef struct {
    bool scl, sda;             /* what the driver is driving              */
    bool last_scl, last_sda;
    /* The monitor's side of the conversation. */
    int  state;                /* 0 idle, 1 taking bytes, 2 giving them   */
    int  bit;                  /* 0 to 7 within a byte                    */
    bool in_ack;               /* the ninth clock, where the ack happens  */
    bool slave_acks;           /* whose acknowledgement this one is       */
    u8   shift;
    bool driving_low;          /* the monitor holding the data line down  */
    int  offset;               /* where in the identification block       */
    int  phase;                /* 0 address byte, 1 offset byte, 2 data   */
} i2c_bus_t;

typedef struct {
    bool present;

    u32  rom_shadow;       /* whether the ROM is switched into the window     */
    u32  pramin_window;

    /* The two-wire lines, per bus, as the far end sees them. */
    i2c_bus_t i2c[16];

    /* What the bus has actually seen, for working out where a transfer went
     * wrong.  A bit-banged bus gives no other clue. */
    int  starts, stops, bytes_in, bytes_out;
    u8   last_address;
    int  edges;
    int  bus_starts[16], bus_acks[16], bus_sent[16];
} model_t;

static model_t model;

static u32 reg_get(u32 off) {
    return (off + 4 <= WINDOW_BYTES && window) ? *(volatile u32 *)(window + off) : 0;
}
static void reg_set(u32 off, u32 v) {
    if (off + 4 <= WINDOW_BYTES && window) *(volatile u32 *)(window + off) = v;
}

/* ------------------------------------------------------------- the monitor
 *
 * A 27-inch 4K panel: the identification block a real one answers with, built
 * here rather than copied, so every field is one this code put there and can
 * be checked against.
 */
#define MONITOR_BUS 2
static u8 monitor_edid[128];

static void build_edid(void) {
    memset(monitor_edid, 0, sizeof monitor_edid);

    /* The fixed header every block starts with. */
    monitor_edid[0] = 0x00;
    for (int i = 1; i <= 6; i++) monitor_edid[i] = 0xFF;
    monitor_edid[7] = 0x00;

    /* The manufacturer, as three five-bit letters: "KES". */
    u16 packed = (u16)(((('K' - 'A' + 1) & 0x1F) << 10) |
                       ((('E' - 'A' + 1) & 0x1F) << 5) |
                       (('S' - 'A' + 1) & 0x1F));
    monitor_edid[8] = (u8)(packed >> 8);
    monitor_edid[9] = (u8)packed;

    monitor_edid[10] = 0x01; monitor_edid[11] = 0x27;      /* product code    */
    monitor_edid[16] = 32;                                 /* week            */
    monitor_edid[17] = 36;                                 /* year, from 1990 */
    monitor_edid[18] = 1; monitor_edid[19] = 4;            /* version 1.4     */

    monitor_edid[20] = 0x80;                               /* digital input   */
    monitor_edid[21] = 60;                                 /* 60 cm wide      */
    monitor_edid[22] = 34;                                 /* 34 cm tall      */
    monitor_edid[23] = 120;                                /* gamma           */
    monitor_edid[24] = 0x0A;

    /* The first detailed descriptor: 3840 by 2160 at 60 Hz.  The numbers are
     * the real ones for that mode, so a driver reading them back gets a
     * refresh rate it can check. */
    u32 pixel_clock = 533250000u;                          /* 533.25 MHz      */
    u16 hactive = 3840, hblank = 160;
    u16 vactive = 2160, vblank = 62;

    u8 *d = monitor_edid + 54;
    u32 clock_units = pixel_clock / 10000u;
    d[0] = (u8)clock_units;
    d[1] = (u8)(clock_units >> 8);
    d[2] = (u8)(hactive & 0xFF);
    d[3] = (u8)(hblank & 0xFF);
    d[4] = (u8)(((hactive >> 8) << 4) | ((hblank >> 8) & 0x0F));
    d[5] = (u8)(vactive & 0xFF);
    d[6] = (u8)(vblank & 0xFF);
    d[7] = (u8)(((vactive >> 8) << 4) | ((vblank >> 8) & 0x0F));
    d[8] = 48; d[9] = 32; d[10] = 0x40;                    /* sync offsets    */
    d[11] = 60; d[12] = 34; d[13] = 0x00;                  /* physical size   */
    d[17] = 0x1E;

    /* The third descriptor holds the model name. */
    u8 *name = monitor_edid + 54 + 2 * 18;
    name[0] = 0; name[1] = 0; name[2] = 0;
    name[3] = 0xFC;                                        /* a name follows  */
    name[4] = 0;
    const char *text = "Kestrel UHD";   /* thirteen bytes is the limit */
    size_t n = strlen(text);
    for (size_t i = 0; i < 13; i++) name[5 + i] = i < n ? (u8)text[i] : (i == n ? 0x0A : 0x20);

    /* The whole block sums to zero, which is what a reader checks. */
    u8 sum = 0;
    for (int i = 0; i < 127; i++) sum = (u8)(sum + monitor_edid[i]);
    monitor_edid[127] = (u8)(256 - sum);
}

/* --------------------------------------------------------------- the ROM
 *
 * Built to the same shape a real one has, so the driver's search for the BIT
 * signature, its walk of the table list and its read of the connector table
 * all do real work.
 */
#define MODEL_ROM_SIZE 0x8000
static u8 rom[MODEL_ROM_SIZE];

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

#define ROM_BIT_AT  0x200
#define ROM_DCB_AT  0x400
#define ROM_VER_AT  0x300

static void build_rom(void) {
    memset(rom, 0, sizeof rom);

    /* The option ROM header every card starts with. */
    rom[0] = 0x55;
    rom[1] = 0xAA;
    rom[2] = MODEL_ROM_SIZE / 512;             /* the length, in half-K units */

    /* The pointer to the connector table, at the one offset that has never
     * moved. */
    put16(&rom[0x36], ROM_DCB_AT);

    /* NVIDIA's own tables.  The header opens with the identifier 0xB8FF -
     * stored low byte first, so 0xFF then 0xB8 - and only then the signature.
     *
     * This model used to put the signature immediately after the 0xFF, with no
     * 0xB8 at all, because the reader it was written alongside looked for it
     * there.  Both were wrong in the same way, so both agreed and neither
     * failed.  A real card's ROM settled it. */
    u8 *bit = &rom[ROM_BIT_AT];
    bit[0] = 0xFF;
    bit[1] = 0xB8;
    put32(&bit[2], NV_BIT_SIGNATURE);
    bit[6] = 0;                                 /* structure version, low     */
    bit[7] = 0;                                 /* structure version, high    */
    bit[8] = 12;                                /* header length              */
    bit[9] = sizeof(nv_bit_entry_t);            /* how long each entry is     */
    bit[10] = 1;                                /* how many                   */
    bit[11] = 0;                                /* checksum                   */

    /* One entry: the version table. */
    nv_bit_entry_t *entry = (nv_bit_entry_t *)&bit[12];
    entry->id = 'i';
    entry->version = 2;
    entry->length = 4;
    entry->offset = ROM_VER_AT;

    /* Version 86.04.52.00, written low byte first the way the driver reads
     * it. */
    rom[ROM_VER_AT + 0] = 0x00;
    rom[ROM_VER_AT + 1] = 0x52;
    rom[ROM_VER_AT + 2] = 0x04;
    rom[ROM_VER_AT + 3] = 0x86;

    /* The connector table: version 4.0, four outputs.
     *
     * The header is twenty-three bytes, not eight: the signature sits at
     * offset six and takes four of them, so a shorter header would have the
     * first entry overlapping it. */
    u8 *dcb = &rom[ROM_DCB_AT];
    dcb[0] = 0x40;                              /* version 4.0                */
    dcb[1] = 0x17;                              /* header length              */
    dcb[2] = 5;                                 /* entries, including the end */
    dcb[3] = 8;                                 /* entry length               */
    put32(&dcb[6], 0x4EDCBDCB);                 /* the signature              */

    /* Each entry: type in the low nibble, I2C bus next, head mask above that,
     * then which physical socket. */
    struct { u8 type, bus, heads, connector; } outputs[4] = {
        { NV_OUTPUT_ANALOG, 0, 0x3, 0 },        /* VGA                        */
        { NV_OUTPUT_TMDS,   1, 0x3, 1 },        /* HDMI                       */
        { NV_OUTPUT_DP,     MONITOR_BUS, 0x3, 2 },  /* DisplayPort, with the
                                                     * monitor on it          */
        { NV_OUTPUT_DP,     3, 0x3, 3 },
    };

    for (int i = 0; i < 4; i++) {
        u32 word = (u32)outputs[i].type
                 | ((u32)outputs[i].bus << 4)
                 | ((u32)outputs[i].heads << 8)
                 | ((u32)outputs[i].connector << 12);
        put32(&dcb[0x17 + i * 8], word);
    }
    /* The end of the list. */
    put32(&dcb[0x17 + 4 * 8], NV_OUTPUT_EOL);
}

/* --------------------------------------------------------- the two wires
 *
 * The far end of an I2C bus, driven one edge at a time.  A monitor is a slave:
 * it watches for a start condition, matches its own address, and then either
 * takes a byte or gives one back, holding the data line down to acknowledge.
 *
 * Modelling this properly rather than answering the whole read at once is the
 * point: it is the only way to check that the driver's start and stop
 * conditions, bit order and acknowledgement handling are right, and those are
 * exactly what goes wrong in a bit-banged bus.
 */
#define EDID_ADDRESS 0x50

/* One edge of one bus, as the device on the far end experiences it.
 *
 * The order matters and is the thing that is easy to get wrong: a byte is
 * complete after the eighth clock, and the ninth is the acknowledgement.  So
 * the byte has to be acted on - and the acknowledgement decided - on the
 * falling edge that ends the eighth bit, before the ninth rising edge arrives
 * for the master to read that acknowledgement on.  Doing it a step later means
 * the master samples an answer the far end has not worked out yet.
 */
static void i2c_step(int bus) {
    if (bus < 0 || bus >= 16) return;
    i2c_bus_t *b = &model.i2c[bus];

    bool scl = b->scl, sda = b->sda;
    bool was_scl = b->last_scl, was_sda = b->last_sda;
    b->last_scl = scl;
    b->last_sda = sda;

    /* Start: the data line falls while the clock is high. */
    model.edges++;
    if (scl && was_scl && was_sda && !sda) {
        model.starts++;
        model.bus_starts[bus & 15]++;
        b->state = 1;
        b->bit = 0;
        b->in_ack = false;
        b->slave_acks = false;
        b->shift = 0;
        b->phase = 0;
        b->driving_low = false;
        return;
    }
    /* Stop: it rises while the clock is high. */
    if (scl && was_scl && !was_sda && sda) {
        model.stops++;
        b->state = 0;
        b->in_ack = false;
        b->driving_low = false;
        return;
    }
    if (!b->state) return;

    /* A rising clock edge is where a bit is read by whichever side is
     * listening. */
    if (scl && !was_scl) {
        if (b->in_ack) {
            /* Which side is acknowledging depends on which side sent the byte,
             * and getting that wrong is the classic way to break a bus: after
             * an address byte it is the far end that answers, and after a byte
             * the far end sent it is the master.  Reading the first as the
             * second makes every read look like the master hanging up
             * immediately. */
            if (!b->slave_acks && sda) {
                /* The master let the line go: it wants no more. */
                b->state = 1;
                b->phase = 0;
            }
        } else if (b->state == 1) {
            b->shift = (u8)((b->shift << 1) | (sda ? 1 : 0));
            b->bit++;
        } else {
            /* Sending: the bit was put on the line at the falling edge and the
             * master has just read it. */
            b->bit++;
        }
        return;
    }

    if (!(!scl && was_scl)) return;              /* only falling edges left */

    if (b->in_ack) {
        /* The acknowledgement is over.  Whether the next byte is a new one
         * depends on whose acknowledgement it was: after the address the far
         * end acknowledged and the byte it already loaded is the one to send,
         * but after a byte it sent the master acknowledged and the next one is
         * due.  Advancing in both cases skips the first byte of every read,
         * which is the sort of fault that produces a block of plausible
         * rubbish rather than an obvious failure. */
        bool was_slave_ack = b->slave_acks;
        b->in_ack = false;
        b->slave_acks = false;
        b->bit = 0;

        if (b->state == 2) {
            if (!was_slave_ack) {
                b->offset = (b->offset + 1) & 127;
                b->shift = monitor_edid[b->offset];
            }
            b->driving_low = !((b->shift >> 7) & 1);
        } else {
            b->driving_low = false;
        }
        return;
    }

    if (b->bit == 8) {
        /* A byte has just finished.  Act on it, and set up whatever this side
         * has to put on the line for the ninth clock. */
        b->in_ack = true;

        if (b->state == 1) {
            if (b->phase == 0) {
                /* The address byte: seven bits of address and one saying which
                 * way the transfer goes. */
                u8 address = (u8)(b->shift >> 1);
                bool read = (b->shift & 1) != 0;
                model.last_address = b->shift;
                model.bytes_in++;

                if (address != EDID_ADDRESS || bus != MONITOR_BUS) {
                    /* Nothing here answers to that, so the line is left high
                     * and the master sees no acknowledgement. */
                    b->state = 0;
                    b->in_ack = false;
                    b->driving_low = false;
                    return;
                }
                b->driving_low = true;                  /* acknowledge */
                b->slave_acks = true;
                model.bus_acks[bus]++;
                if (read) {
                    b->state = 2;
                    b->shift = monitor_edid[b->offset & 127];
                    b->phase = 2;
                } else {
                    b->phase = 1;
                }
            } else {
                /* The offset to start reading from. */
                b->offset = b->shift;
                b->driving_low = true;                  /* acknowledge */
                b->slave_acks = true;
            }
        } else {
            /* This side has finished sending a byte; the master will answer on
             * the ninth clock, so the line is released for it. */
            b->driving_low = false;
            b->slave_acks = false;
            model.bus_sent[bus]++;
        }
        return;
    }

    /* Between bits.  When sending, this is where the next one goes out, most
     * significant first. */
    if (b->state == 2) b->driving_low = !((b->shift >> (7 - b->bit)) & 1);
    else b->driving_low = false;
}

/* ------------------------------------------------------- watching the bus */

static u32 previous_rom_shadow;
static u32 previous_i2c[16];

void nv_model_sync(void) {
    if (!model.present) return;

    /* The ROM being switched into the window, and out again. */
    u32 shadow = reg_get(NV_PBUS_PCI_NV_20);
    if (shadow != previous_rom_shadow) {
        previous_rom_shadow = shadow;
        model.rom_shadow = shadow;

        if (shadow == NV_PBUS_PCI_NV_20_ROM_SHADOW_DISABLED) {
            /* The ROM is readable through the window. */
            if (window)
                memcpy(window + NV_PROM_OFFSET, rom, sizeof rom);
        } else {
            if (window) memset(window + NV_PROM_OFFSET, 0xFF, sizeof rom);
        }
    }

    /* The display channel: whatever the driver has written, the engine reads.
     * A real one takes time over it; here it is immediate, which is the one
     * way the model is kinder than hardware. */
    for (int ch = 0; ch < 4; ch++) {
        u32 put = reg_get(NV_PDISP_CHAN_PUT(ch));
        if (put) reg_set(NV_PDISP_CHAN_GET(ch), put);
    }

    /* Each two-wire bus, stepped for whatever the driver just wrote. */
    for (int bus = 0; bus < 16; bus++) {
        u32 port = NV_PCRTC_I2C_BASE_OLD + (u32)bus * NV_PCRTC_I2C_STRIDE;
        u32 value = reg_get(port);
        if (value == previous_i2c[bus]) continue;
        previous_i2c[bus] = value;

        /* The lines are open-drain, so a set bit pulls low. */
        model.i2c[bus].scl = !(value & NV_I2C_SCL_OUT);
        model.i2c[bus].sda = !(value & NV_I2C_SDA_OUT);
        i2c_step(bus);

        /* What the driver reads back: the clock as it drove it, and the data
         * line low if either side is pulling it down. */
        u32 readback = value;
        if (model.i2c[bus].scl) readback |= NV_I2C_SCL_IN; else readback &= ~NV_I2C_SCL_IN;
        if (model.i2c[bus].sda && !model.i2c[bus].driving_low)
            readback |= NV_I2C_SDA_IN;
        else
            readback &= ~NV_I2C_SDA_IN;

        reg_set(port, readback);
        previous_i2c[bus] = readback;
    }
}

/* ------------------------------------------------------------- bringing up */

volatile u8 *nv_model_attach(size_t *size_out) {
    if (model.present) {
        if (size_out) *size_out = WINDOW_BYTES;
        return window;
    }

    u64 phys;
    window = dma_alloc_pages(WINDOW_BYTES / PAGE_SIZE, &phys);
    if (!window) {
        kwarn("nv-model", "no memory for the register window");
        return NULL;
    }
    memset(window, 0, WINDOW_BYTES);

    memset(&model, 0, sizeof model);
    model.present = true;

    build_edid();
    build_rom();

    /* What the driver reads to find out what this is. */
    reg_set(NV_PMC_BOOT_0, MODEL_BOOT0);

    /* Eight gigabytes, as a mantissa and a power-of-two scale: 8 << (13 + 20)
     * would overflow the field, so it is expressed as 2 << (32 - 20 ... ) -
     * in practice a mantissa of 8 and a scale of 10, which is 8 << 30. */
    reg_set(NV_PFB_LOCAL_MEMORY_RANGE, 8u | (10u << 4));

    /* The temperature, encoded the way the silicon this model stands for
     * actually encodes it: at the Pascal address, in bits 16:8, with a
     * fraction below.
     *
     * This used to sit at the Kepler address shifted left sixteen - the same
     * mistake the driver was making, written a second time.  A model built
     * from the driver's own assumption cannot catch that assumption being
     * wrong; it agrees, every time, and the test passes while the hardware
     * would not.  The half-degree in the fraction bits is here so that a
     * driver which forgets to mask them reads 47 and a bit rather than 47. */
    reg_set(NV_THERM_SENSOR_PASCAL,
            ((u32)MODEL_TEMP_C << NV_THERM_SENSOR_PASCAL_SHIFT) | (1u << 7));
    reg_set(NV_THERM_FAN_PWM_DUTY, 38);

    /* The two-wire lines idle high, which is a zero in the drive register. */
    for (int bus = 0; bus < 16; bus++) {
        u32 port = NV_PCRTC_I2C_BASE_OLD + (u32)bus * NV_PCRTC_I2C_STRIDE;
        reg_set(port, NV_I2C_SCL_IN | NV_I2C_SDA_IN);
        previous_i2c[bus] = NV_I2C_SCL_IN | NV_I2C_SDA_IN;
        model.i2c[bus].scl = model.i2c[bus].sda = true;
        model.i2c[bus].last_scl = model.i2c[bus].last_sda = true;
    }

    previous_rom_shadow = 0xFFFFFFFFu;

    kinfo("nv-model", "no NVIDIA card present; a model of one is standing in so "
                      "the driver runs");
    if (size_out) *size_out = WINDOW_BYTES;
    return window;
}

bool nv_model_present(void) { return model.present; }

/* What the two wires actually carried, for a test that needs to say where a
 * transfer stopped rather than only that it did. */
void nv_model_i2c_report(int *edges, int *starts, int *stops, u8 *address) {
    if (edges) *edges = model.edges;
    if (starts) *starts = model.starts;
    if (stops) *stops = model.stops;
    if (address) *address = model.last_address;
}

void nv_model_i2c_bus(int bus, int *starts, int *acks, int *sent) {
    if (bus < 0 || bus > 15) return;
    if (starts) *starts = model.bus_starts[bus];
    if (acks) *acks = model.bus_acks[bus];
    if (sent) *sent = model.bus_sent[bus];
}

/* What the model was built to be, so a test can compare against it. */
u32         nv_model_boot0(void) { return MODEL_BOOT0; }
u64         nv_model_vram(void) { return MODEL_VRAM; }
int         nv_model_temperature(void) { return MODEL_TEMP_C; }
const u8   *nv_model_edid(void) { return monitor_edid; }
u8          nv_model_monitor_bus(void) { return MONITOR_BUS; }
