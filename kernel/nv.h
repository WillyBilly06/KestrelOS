/* nv.h - what an NVIDIA GPU looks like from the outside.
 *
 * The register map has been broadly stable since the Riva TNT: the card
 * presents a sixteen-megabyte window of registers divided into named blocks at
 * fixed offsets, and the blocks that matter for bringing a card up - the master
 * control, the framebuffer controller, the ROM window, the instance memory
 * window, the I2C lines, the thermal sensors - have moved very little in
 * twenty-five years.  That stability is why a driver written from the older
 * documentation still finds its way around a card released this year.
 *
 * What has changed, and changed completely, is what happens above that: from
 * Turing onward the display and graphics engines are driven by a co-processor
 * running signed firmware rather than by writes to these registers.  So this
 * header covers what every card can do, and the parts that stop at Turing say
 * so where they are used.
 */
#ifndef KESTREL_NV_H
#define KESTREL_NV_H

#include "kernel.h"
#include "../include/kestrel/shader_setup.h"

/* Caller owns one render transaction and an immutable kernel submission.
 * Success attests fenced setup/compaction/raster semantic completion; no CPU
 * vertex or varying readback. Failure never replays a partially retired draw. */
bool nv_surface_shader_geometry(u64 owner, u64 destination, u64 depth, u64 texture,
                                 const kshs_submission_t *submission, u32 *emitted,
                                 u64 *executed);

/* ------------------------------------------------------------ master control */

#define NV_PMC_BOOT_0            0x000000  /* what this chip is                */
#define NV_PMC_INTR_0            0x000100
#define NV_PMC_INTR_EN_0         0x000140
#define NV_PMC_ENABLE            0x000200  /* which engines are switched on    */
#define NV_PMC_BOOT_1            0x000004  /* endianness                       */

#define NV_PMC_ENABLE_PGRAPH     (1u << 12)
#define NV_PMC_ENABLE_PFIFO      (1u << 8)
#define NV_PMC_ENABLE_PDISP      (1u << 30)

/* ------------------------------------------------------------------ the ROM
 *
 * The video BIOS is the card's own description of itself: which memory is
 * fitted, how to bring the clocks up, what connectors exist and which pins
 * drive them.  It can be reached three ways and not every way works on every
 * card, which is why all three are tried.
 */
#define NV_PROM_OFFSET           0x300000  /* the ROM, mapped into the window  */
#define NV_PROM_SIZE             0x010000
#define NV_PBUS_PCI_NV_20        0x001850  /* enables the ROM window           */
#define NV_PBUS_PCI_NV_20_ROM_SHADOW_DISABLED 0x00000000
#define NV_PBUS_PCI_NV_20_ROM_SHADOW_ENABLED  0x00000001

/* Instance memory: a movable window onto the card's own memory, which is how
 * the driver reads and writes anything in VRAM without a full aperture. */
#define NV_PRAMIN_OFFSET         0x700000
#define NV_PRAMIN_SIZE           0x100000
#define NV_PBUS_BAR0_WINDOW      0x001700  /* which part of VRAM PRAMIN shows  */

/* --------------------------------------------------------- the framebuffer */

#define NV_PFB_BOOT_0            0x100000  /* the oldest cards' memory config  */
#define NV_PFB_CFG0              0x100200
#define NV_PFB_CSTATUS           0x10020C  /* Riva through GeForce 7           */
#define NV_PFB_LOCAL_MEMORY_RANGE 0x100CE0 /* Fermi and later                  */
#define NV_PFB_NISO_FLUSH        0x100C80
#define NV_PFB_FBPA_CSTATUS      0x10F20C  /* per-partition, Fermi and later   */

/* --------------------------------------------------------------- engines
 *
 * The drawing engine's status register has been in the same place since
 * Fermi, and its low bit says whether the engine has work in it.  It is the
 * one engine on the card whose activity can be seen without the firmware that
 * owns the others.
 */
#define NV_PGRAPH_STATUS         0x400700
#define NV_PGRAPH_STATUS_BUSY    (1u << 0)

#define NV_MAX_CARDS 2

/* This driver's own version.  Bumped when what it can drive changes. */
#define NV_DRIVER_VERSION "0.4"

enum {
    NV_ENGINE_3D = 0,
    NV_ENGINE_COPY,
    NV_ENGINE_ENCODE,
    NV_ENGINE_DECODE,
    NV_ENGINE_COUNT
};

/* Two things a percentage cannot say.  They are kept distinct because they
 * mean opposite things and both would otherwise be shown as zero: an engine
 * the card does not have, and one it has whose activity this driver cannot
 * see. */
#define NV_ENGINE_ABSENT      (-1)
#define NV_ENGINE_UNMEASURED  (-2)

typedef struct {
    int  engine_percent[NV_ENGINE_COUNT];
    bool sampled;             /* false until a window has been collected     */
    u32  samples;
    int  temperature_c;       /* -1000 when it could not be read             */
    int  fan_percent;
    u64  vram_bytes;
    u64  vram_used;           /* what this driver handed out; a floor        */
} nv_telemetry_t;

/* ------------------------------------------------------------------ thermal
 *
 * Where the temperature is read from moved once, at Kepler.  Both places are
 * a signed value in degrees; the older one needs a calibration from the VBIOS
 * and the newer one does not.
 */
#define NV_THERM_SENSOR_OLD      0x0015B4  /* NV40 through Fermi               */

/* The internal sensor, which moved at Pascal and was being read at the old
 * address on every card since.
 *
 * Checked against nouveau, which reads these registers directly rather than
 * asking firmware - so it is the source that actually says where they are.
 * NVIDIA's own published headers are no help here: the open kernel modules
 * delegate thermal reads to GSP firmware, and the only thermal register
 * published for Blackwell is an I2C scratch word at 0x00ad00bc.
 *
 *   G84 through Maxwell   0x020400, the value read as it stands
 *                         (nvkm/subdev/therm/g84.c: g84_temp_get)
 *   Pascal and later      0x020460, degrees in bits 16:8
 *                         (nvkm/subdev/therm/gp100.c: gp100_temp_get)
 *
 * Reading 0x020400 on a Pascal or later card does not fail - it returns
 * whatever that address now holds, and the old code then took bits 23:16 of
 * it. That is the worst shape a bug can have: a plausible small number, from
 * the wrong register, with nothing to say so. */
#define NV_THERM_SENSOR_G84      0x020400  /* G84 through Maxwell              */
#define NV_THERM_SENSOR_PASCAL   0x020460  /* Pascal and later                 */
#define NV_THERM_SENSOR_PASCAL_MASK  0x0001FFF8u   /* 16:3, the low bits are a
                                                    * fraction of a degree    */
#define NV_THERM_SENSOR_PASCAL_SHIFT 8
#define NV_THERM_FAN_TACH        0x00E114
#define NV_THERM_FAN_PWM_DUTY    0x00E11C

/* ------------------------------------------------------------------- I2C
 *
 * Every display connector has two wires alongside it that a monitor answers
 * on, and reading a monitor's identification over them is how a card knows
 * what is plugged in.  The lines are bit-banged: the driver drives clock and
 * data itself rather than a controller doing it.
 */
#define NV_PCRTC_I2C_BASE_OLD    0x00E138  /* NV17 and later, per bus          */
#define NV_PCRTC_I2C_STRIDE      0x0018

#define NV_I2C_SDA_OUT           (1u << 4)
#define NV_I2C_SCL_OUT           (1u << 5)
#define NV_I2C_SDA_IN            (1u << 27)
#define NV_I2C_SCL_IN            (1u << 28)

/* ------------------------------------------------------------------ display
 *
 * NV50 and later drive the display through a command channel rather than
 * through registers directly: the driver writes a stream of method-and-value
 * pairs into a buffer in memory and rings a doorbell.  This is the block those
 * doorbells live in.
 */
#define NV_PDISP_BASE            0x610000
#define NV_PDISP_CHAN_PUT(c)     (0x640000 + (c) * 0x1000)
#define NV_PDISP_CHAN_GET(c)     (0x640004 + (c) * 0x1000)

/* -------------------------------------------------------------- DisplayPort
 *
 * On a card built in the last fifteen years the two wires beside a connector
 * are not an I2C bus any more.  DisplayPort replaced them with AUX: a
 * differential pair carrying addressed transactions, over which a driver reads
 * the monitor's capabilities, reads its identification block through an
 * emulated I2C bus, and then trains the link - negotiating the voltage swing
 * and pre-emphasis of every lane until the monitor reports it can read them.
 *
 * None of that is optional.  A DisplayPort monitor shows nothing at all until
 * the link is trained, so a driver that can bit-bang I2C and no more can light
 * up a monitor from 2005 and not one from 2015.
 *
 * The register block below is the one Maxwell 2 introduced and every card
 * since - Pascal, Turing, Ampere, Ada, Blackwell - has kept.
 */
#define NV_AUX_DATA_WRITE(ch, i)  (0x00D930 + (ch) * 0x50 + (i))
#define NV_AUX_DATA_READ(ch, i)   (0x00D940 + (ch) * 0x50 + (i))
#define NV_AUX_ADDR(ch)           (0x00D950 + (ch) * 0x50)
#define NV_AUX_CTRL(ch)           (0x00D954 + (ch) * 0x50)
#define NV_AUX_STAT(ch)           (0x00D958 + (ch) * 0x50)

/* CTRL: the transaction is started by setting bit 16 and is finished when the
 * card clears it.  The pad has to be requested first and released after, and
 * a driver that forgets either one works until something else wants the pad. */
#define NV_AUX_CTRL_TRANSACT      0x00010000u
#define NV_AUX_CTRL_RESET         0x80000000u
#define NV_AUX_CTRL_REQUEST       0x00100000u
#define NV_AUX_CTRL_GRANTED       0x01000000u
#define NV_AUX_CTRL_REQ_MASK      0x00700000u
#define NV_AUX_CTRL_REP_MASK      0x07000000u
#define NV_AUX_CTRL_BUSY_MASK     0x07010000u
#define NV_AUX_CTRL_TYPE_SHIFT    12
#define NV_AUX_CTRL_FIELDS        0x0001F1FFu

/* STAT: whether anything is plugged in, what the far end replied, and how many
 * bytes it actually transferred - which is not always how many were asked for. */
#define NV_AUX_STAT_SINK          0x10000000u
#define NV_AUX_STAT_REPLY_MASK    0x000F0000u
#define NV_AUX_STAT_REPLY_SHIFT   16
#define NV_AUX_STAT_TIMEOUT       0x00000100u
#define NV_AUX_STAT_ERROR         0x00000E00u
#define NV_AUX_STAT_COUNT_MASK    0x0000001Fu

/* A request's command nibble: bit 3 says native rather than I2C, bit 0 says
 * read rather than write, bit 2 says the I2C transaction continues. */
#define NV_AUX_I2C_WRITE          0x0
#define NV_AUX_I2C_READ           0x1
#define NV_AUX_I2C_WRITE_MOT      0x4
#define NV_AUX_I2C_READ_MOT       0x5
#define NV_AUX_NATIVE_WRITE       0x8
#define NV_AUX_NATIVE_READ        0x9

/* And the reply nibble.  A defer is not a failure: it means "ask again", and a
 * driver that treats it as one fails on monitors that are merely slow. */
#define NV_AUX_ACK                0x0
#define NV_AUX_NACK               0x1
#define NV_AUX_DEFER              0x2
#define NV_AUX_I2C_NACK           0x4
#define NV_AUX_I2C_DEFER          0x8

/* The monitor's own register space, addressed over AUX.  These addresses are
 * the DisplayPort specification's and are the same on every monitor. */
#define DPCD_REV                       0x00000
#define DPCD_MAX_LINK_RATE             0x00001
#define DPCD_MAX_LANE_COUNT            0x00002
#define DPCD_MAX_DOWNSPREAD            0x00003
#define DPCD_TRAINING_AUX_RD_INTERVAL  0x0000E
#define DPCD_LINK_BW_SET               0x00100
#define DPCD_LANE_COUNT_SET            0x00101
#define DPCD_TRAINING_PATTERN_SET      0x00102
#define DPCD_TRAINING_LANE0_SET        0x00103
#define DPCD_SINK_COUNT                0x00200
#define DPCD_LANE0_1_STATUS            0x00202
#define DPCD_LANE2_3_STATUS            0x00203
#define DPCD_LANE_ALIGN_STATUS         0x00204
#define DPCD_ADJUST_REQUEST_LANE0_1    0x00206
#define DPCD_ADJUST_REQUEST_LANE2_3    0x00207
#define DPCD_SET_POWER                 0x00600

#define DPCD_LANE_CR_DONE              0x01
#define DPCD_LANE_EQ_DONE              0x02
#define DPCD_LANE_SYMBOL_LOCKED        0x04
#define DPCD_INTERLANE_ALIGN_DONE      0x01
#define DPCD_ENHANCED_FRAME_CAP        0x80
#define DPCD_TPS3_SUPPORTED            0x40
#define DPCD_MAX_SWING_REACHED         0x04
#define DPCD_MAX_PRE_EMPHASIS_REACHED  0x20
#define DPCD_SCRAMBLING_DISABLE        0x20

/* Link rates, as the byte that carries them.  Each is the serial bit rate of
 * one lane; the usable pixel bandwidth is eight tenths of it, because the data
 * is 8b/10b coded. */
#define DP_RATE_1_62               0x06
#define DP_RATE_2_70               0x0A
#define DP_RATE_5_40               0x14
#define DP_RATE_8_10               0x1E

/* The card's side of the link, in the SOR - the block that turns pixels into
 * whatever the connector carries. */
#define NV_PDISP_SOR_CLK_CNTL      0x612300   /* + sor * 0x800              */
#define NV_PDISP_SOR_DP_PADCTL     0x61C110   /* + sor * 0x800              */
#define NV_PDISP_SOR_DP_LINKCTL    0x61C10C   /* + sor * 0x800 + link*0x80  */
#define NV_PDISP_SOR_DP_DRIVE      0x61C118   /* one byte per lane          */
#define NV_PDISP_SOR_DP_PREEMPH    0x61C120
#define NV_PDISP_SOR_DP_POSTCURSOR 0x61C13C
#define NV_PDISP_SOR_STRIDE        0x800
#define NV_PDISP_SOR_LINK_STRIDE   0x80

/* The link itself, and everything done over it, is declared after the card -
 * see below. */


/* --------------------------------------------------------------- the VBIOS */

/* The image starts with the same header a PCI option ROM does, then NVIDIA's
 * own tables hang off a signature further in. */
#define NV_ROM_SIGNATURE         0xAA55
#define NV_BIT_SIGNATURE         0x00544942   /* "BIT\0", little-endian        */

/* One entry of the BIT structure: a letter saying what the table is for, and
 * where to find it. */
typedef struct {
    u8  id;
    u8  version;
    u16 length;
    u16 offset;
} __attribute__((packed)) nv_bit_entry_t;

/* The display configuration block: one entry per connector, saying what kind
 * of output it is, which head can drive it and which I2C bus it answers on.
 * This is how a driver knows a card has three DisplayPorts and an HDMI rather
 * than having to guess. */
typedef enum {
    NV_OUTPUT_ANALOG   = 0,     /* VGA                                       */
    NV_OUTPUT_TV       = 1,
    NV_OUTPUT_TMDS     = 2,     /* DVI, and HDMI which is TMDS underneath    */
    NV_OUTPUT_LVDS     = 3,     /* a laptop panel                            */
    NV_OUTPUT_RESERVED = 4,
    NV_OUTPUT_SDI      = 5,
    NV_OUTPUT_DP       = 6,     /* DisplayPort                               */
    NV_OUTPUT_EOL      = 14,
    NV_OUTPUT_UNUSED   = 15,
} nv_output_type;

#define NV_MAX_OUTPUTS 16

typedef struct {
    nv_output_type type;
    u8   heads;             /* which display heads can drive it              */
    u8   connector;         /* which physical socket it belongs to           */
    u8   i2c_bus;           /* where its monitor answers, 0x0f for none      */
    u8   link;
    bool used;
    /* Filled in if a monitor answered. */
    bool     monitor_present;
    char     monitor_name[16];
    u16      width, height;
    u16      refresh_hz;
    u8       edid[128];
    bool     edid_valid;
} nv_output_t;

/* The sockets on the bracket.
 *
 * A display block entry says how a socket is driven, not what it is: three of
 * the four sockets on an RTX 5070 Ti appear twice, once as DisplayPort and
 * once as TMDS, because those sockets can be driven either way.  Counting
 * entries therefore reports seven outputs on a card with four holes in it.
 *
 * What the sockets actually are is in a second table the block points at, and
 * it is the only place the difference between DVI and HDMI is written down -
 * both are TMDS as far as the first table is concerned.
 */
typedef enum {
    NV_CONNECTOR_UNKNOWN = 0,
    NV_CONNECTOR_VGA,
    NV_CONNECTOR_DVI,
    NV_CONNECTOR_HDMI,
    NV_CONNECTOR_DP,
    NV_CONNECTOR_EDP,
    NV_CONNECTOR_USB_C,
} nv_connector_kind;

const char *nv_connector_name(nv_connector_kind kind);

/* ---------------------------------------------------------------- the card */

typedef struct {
    volatile u8 *regs;          /* BAR0: the register window                 */
    size_t       regs_size;
    u64          vram_base;     /* BAR1: the framebuffer aperture            */
    size_t       vram_aperture;

    u32          boot0;
    u32          chipset;       /* the architecture and implementation       */
    u8           revision;
    const char  *architecture;
    const char  *codename;

    /* The second identity register, which is the one NVIDIA's own driver
     * reads from Turing onward.  It carries the same chip number and a finer
     * revision; whether the two agree is worth knowing on its own. */
    u32          boot42;
    bool         boot42_agrees;
    u8           revision_major;
    u8           revision_minor;
    u8           revision_extended;

    u64          vram_bytes;
    bool         vram_exact;
    const char  *vram_type;

    /* How the memory is divided.  From Pascal on it is in partitions, some
     * fused off in the factory, and they need not all be the same size. */
    int          fbpa_total;
    int          fbpa_live;
    bool         fbpa_uneven;

    /* What the VBIOS said. */
    u8          *vbios;
    size_t       vbios_size;
    char         vbios_version[32];
    const char  *vbios_source;
    bool         vbios_valid;

    nv_output_t  output[NV_MAX_OUTPUTS];
    int          outputs;

    /* The sockets those outputs share, which is what somebody looking at the
     * back of the card counts. */
    nv_connector_kind connector_kind[NV_MAX_OUTPUTS];
    int          connectors;

    int          temperature_c;   /* -1000 when it could not be read         */
    int          fan_percent;

    int          index;           /* which card this is, for per-card state  */
    u8           pci_bus, pci_slot, pci_func;   /* where it sits on the bus  */
    u32          engines_present; /* a bit per NV_ENGINE_*                   */
    u64          vram_allocated;  /* what this driver has handed out         */

    bool         modelled;
} nv_card_t;

/* Reading the card's activity.  Declared here rather than beside the engine
 * constants above because every one of these takes a card. */
const char *nv_engine_name(int engine);
void nv_telemetry_probe_engines(nv_card_t *c);
void nv_telemetry_sample(nv_card_t *c);
void nv_telemetry_bind_host(nv_card_t *c, u32 client, u32 device, u32 subdevice, u32 engines);
void nv_telemetry_read(nv_card_t *c, nv_telemetry_t *out);
int  nv_telemetry_selftest(void);

/* Read a ROM's tables into a card: what outputs it has, what sockets they
 * come out of, and what version the ROM is.  Exposed so that the parsing can
 * be run against a real card's ROM rather than only against a model. */
bool nv_vbios_parse(nv_card_t *c);
int  nv_vbios_real_selftest(void);

/* The card at one place on the bus, so a caller holding a device's address can
 * reach the driver's own state for it. */
nv_card_t  *nv_card_for_pci(u8 bus, u8 slot, u8 func);
/* Start the card's own co-processor.  Deliberate only - see the note in
 * nv_gsp.c: it takes the card away from whatever is driving the screen. */
/* The four words in front of every message to the card's co-processor.  The
 * order is the firmware's, not a choice. */
typedef struct {
    u32 transport;
    u32 message;
    u32 checksum;
    u32 sequence;
} nv_gsp_element_t;

/* The header at the start of a ring, written by whichever side owns it.  The
 * field order is the firmware's. */
typedef struct {
    u32 version;
    u32 size;          /* bytes, page aligned                               */
    u32 entry_size;    /* a power of two, at least sixteen                  */
    u32 entry_count;
    u32 write;         /* how far the owner has written                     */
    u32 flags;
    u32 rx_header_offset;
    u32 entry_offset;
} nv_gsp_ring_header_t;

u32  nv_gsp_ring_free(u32 write, u32 read, u32 count);
u32  nv_gsp_ring_available(u32 write, u32 read, u32 count);
u32  nv_gsp_ring_advance(u32 pointer, u32 by, u32 count);
bool nv_gsp_ring_check(const nv_gsp_ring_header_t *h, const char *which);
int  nv_gsp_ring_selftest(void);

u32  nv_gsp_transport_header(bool first, bool last, u8 source, u8 destination,
                             u8 sequence);
u32  nv_gsp_message_header(u8 kind);
u32  nv_gsp_checksum(const void *data, u32 length);
void nv_gsp_frame(nv_gsp_element_t *out, const void *payload, u32 length,
                  u32 sequence);
int  nv_gsp_msg_selftest(void);

/* --------------------------------------------------------- the RPC header
 *
 * Eight words at the front of every request to the card's firmware, and of
 * every reply.  The signature is what the firmware checks to decide it is
 * looking at a request at all; a header without it is dropped in silence.
 */
typedef struct {
    u32 header_version;
    u32 signature;
    u32 length;               /* the whole request, this header included */
    u32 function;
    u32 result;
    u32 result_private;
    u32 sequence;
    u32 spare;
} nv_gsp_rpc_header_t;

u32  nv_gsp_rpc_version(void);
void nv_gsp_rpc_header(nv_gsp_rpc_header_t *out, u32 function, u32 length,
                       u32 sequence);
bool nv_gsp_rpc_reply_ok(const nv_gsp_rpc_header_t *h, u32 expect_function,
                         u32 expect_sequence);
int  nv_gsp_rpc_selftest(void);

bool nv_gsp_start(nv_card_t *c);

/* Assemble everything the GSP co-processor's firmware reads on the FSP route -
 * the boot params, WPR meta, radix3 over the GSP-RM image, libos regions and
 * the shared message rings - and hand back the one pointer the FSP is given.
 * See nv_gsp_boot.c. */
bool nv_gsp_boot_stage(nv_card_t *c, u64 *boot_params_phys, u64 *rsvd_out);
u8  *nv_gsp_boot_cmdq(void);
u8  *nv_gsp_boot_msgq(void);
int  nv_gsp_boot_selftest(void);

const char *nv_driver_version(void);
const char *nv_driver_date(void);

/* Offering this card's drawing engine to the system.  See nv_accel.c. */
void nv_accel_init(nv_card_t *c);

/* ------------------------------------------------- what an engine answers to
 *
 * A class number is the contract between the driver and an engine: sent once
 * when a subchannel is bound, and from then on every method number in that
 * subchannel means whatever that class says it means.  They change every
 * generation and cannot be derived, so they are a table - in nv_blackwell.c,
 * because that is the file whose whole subject is per-generation facts.
 *
 * A zero display class is not a gap.  Some dies - Hopper, and the Blackwell
 * that went to datacentres - have no connectors on them at all.
 */
typedef struct {
    u32         family;         /* the architecture, chipset & 0x1F0         */
    u32         three_d;
    u32         compute;
    u32         copy;
    u32         gpfifo;
    u32         disp_core;
    u32         disp_window;
    const char *name;
} nv_classes_t;

void               nv_allow_writes(bool yes);
bool               nv_writes_allowed(void);
u32                nv_writes_refused(void);

const char        *nv_chip_die(u32 chipset);
const char        *nv_chip_boards(u32 chipset);
bool               nv_read_boot42(nv_card_t *c);
bool               nv_read_vram_fbpa(nv_card_t *c);
const nv_classes_t *nv_classes_for(u32 chipset);
const char        *nv_gsp_core_name(u32 chipset);
const char        *nv_gsp_directory(u32 chipset);
const char        *nv_gsp_release(u32 chipset);
void               nv_report_modern(nv_card_t *c);
int                nv_blackwell_selftest(void);

/* ------------------------------------------------------- a DisplayPort link */

typedef struct {
    int  channel;            /* which AUX pad this connector uses          */
    int  sor;                /* which serialiser drives it                 */
    int  link;               /* which of the serialiser's links            */

    /* What the monitor said it can do. */
    u8   rev;
    u8   max_rate;
    u8   max_lanes;
    bool enhanced_framing;
    bool tps3;
    int  aux_rd_interval_us;

    /* What was actually agreed. */
    u8   rate;
    u8   lanes;
    u8   swing[4], preemphasis[4];
    bool trained;

    /* How it went, for a report that says something rather than yes or no. */
    int  attempts;
    int  cr_loops, eq_loops;
    int  fallbacks;
} nv_dp_link_t;

int  nv_aux_transfer(nv_card_t *c, int channel, u8 type, u32 address,
                     u8 *data, u8 *size);
bool nv_dpcd_read(nv_card_t *c, nv_dp_link_t *l, u32 address, u8 *out, int len);
bool nv_dpcd_write(nv_card_t *c, nv_dp_link_t *l, u32 address, const u8 *in,
                   int len);
bool nv_dp_read_caps(nv_card_t *c, nv_dp_link_t *l);
bool nv_dp_read_edid(nv_card_t *c, nv_dp_link_t *l, u8 out[128]);
bool nv_dp_train(nv_card_t *c, nv_dp_link_t *l, u32 pixel_khz, int bits_per_pixel);
u32  nv_dp_rate_khz(u8 rate);

/* A monitor on the far end of the pair, for exercising all of the above where
 * there is no card and no monitor.  See nv_dp_model.c. */
void nv_dp_model_attach(int channel);
void nv_dp_model_detach(void);
void nv_dp_model_write(u32 offset, u32 value);
void nv_dp_model_read(u32 offset);
int  nv_dp_model_transactions(void);
int  nv_dp_model_defers(void);
bool nv_dp_model_trained(void);
u8   nv_dp_model_trained_rate(void);
u8   nv_dp_model_trained_lanes(void);
int  nv_dp_model_pattern_changes(void);
bool nv_dp_model_saw_unrequested(void);
const u8 *nv_dp_model_edid(void);

int  nv_dp_selftest(void);

/* ------------------------------------------------- the display, Volta onward
 *
 * Volta split the display engine into pieces that had been one thing since
 * NV50.  A head owns a raster - the timing a monitor is driven with - and a
 * window owns a surface, and the two are joined by telling a window which head
 * it belongs to.  Everything a mode change touches is spread across both, and
 * both have to be updated together or the screen shows half of each.
 *
 * The class number says which generation's method set the engine speaks, and
 * the numbers themselves have been stable within each generation.  These are
 * nouveau's, which are NVIDIA's published class numbers.
 */
#define NV_DISP_CORE_GV100   0xC37D      /* Volta                            */
#define NV_DISP_CORE_TU102   0xC57D      /* Turing                           */
#define NV_DISP_CORE_GA102   0xC67D      /* Ampere                           */
#define NV_DISP_CORE_AD102   0xC77D      /* Ada Lovelace                     */
#define NV_DISP_CORE_GB202   0xCA7D      /* Blackwell, the RTX 50 series     */

#define NV_DISP_WINDOW_GV100 0xC37E
#define NV_DISP_WINDOW_TU102 0xC57E
#define NV_DISP_WINDOW_GA102 0xC67E
#define NV_DISP_WINDOW_GB202 0xCA7E

/* The core channel's methods.  Head blocks are 0x400 apart, window-ownership
 * blocks 0x80, serialiser blocks 0x20. */
#define NVC37D_UPDATE                       0x0200
#define NVC37D_SET_CONTEXT_DMA_NOTIFIER     0x0208
#define NVC37D_SOR_SET_CONTROL(s)           (0x0300 + (s) * 0x20)
#define NVC37D_WINDOW_SET_CONTROL(w)        (0x1000 + (w) * 0x80)
#define NVC37D_HEAD_SET_PROCAMP(h)          (0x2000 + (h) * 0x400)
#define NVC37D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(h) (0x2004 + (h) * 0x400)
#define NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(h)   (0x200C + (h) * 0x400)
#define NVC37D_HEAD_SET_HEAD_USAGE_BOUNDS(h)       (0x2030 + (h) * 0x400)
#define NVC37D_HEAD_SET_VIEWPORT_SIZE_IN(h) (0x204C + (h) * 0x400)
#define NVC37D_HEAD_SET_VIEWPORT_SIZE_OUT(h)(0x2058 + (h) * 0x400)
#define NVC37D_HEAD_SET_RASTER_SIZE(h)      (0x2064 + (h) * 0x400)
#define NVC37D_HEAD_SET_RASTER_SYNC_END(h)  (0x2068 + (h) * 0x400)
#define NVC37D_HEAD_SET_RASTER_BLANK_END(h) (0x206C + (h) * 0x400)
#define NVC37D_HEAD_SET_RASTER_BLANK_START(h) (0x2070 + (h) * 0x400)

#define NVC37D_WINDOW_OWNER_NONE            0x0F
#define NVC37D_SOR_PROTOCOL_SINGLE_TMDS_A   0x01
#define NVC37D_SOR_PROTOCOL_DUAL_TMDS       0x05
#define NVC37D_SOR_PROTOCOL_DP_A            0x08
#define NVC37D_SOR_PROTOCOL_DP_B            0x09
#define NVC37D_PIXEL_DEPTH_BPP_24_444       0x04

/* And the window channel's. */
#define NVC37E_UPDATE                       0x0200
#define NVC37E_SET_SIZE                     0x0224
#define NVC37E_SET_STORAGE                  0x0228
#define NVC37E_SET_PARAMS                   0x022C
#define NVC37E_SET_PLANAR_STORAGE(p)        (0x0230 + (p) * 4)
#define NVC37E_SET_CONTEXT_DMA_ISO(p)       (0x0240 + (p) * 4)
#define NVC37E_SET_OFFSET(p)                (0x0260 + (p) * 4)
#define NVC37E_SET_POINT_IN(p)              (0x0290 + (p) * 4)
#define NVC37E_SET_SIZE_IN                  0x0298
#define NVC37E_SET_SIZE_OUT                 0x02A4
#define NVC37E_SET_COMPOSITION_CONTROL      0x02EC
#define NVC37E_SET_PRESENT_CONTROL          0x0308

#define NVC37E_FORMAT_A8R8G8B8              0xCF
#define NVC37E_STORAGE_PITCH                0x10   /* bit 4: not blocklinear */

/* Where a channel is armed, from Volta onward. */
#define NV_PDISP_CHAN_PUSH_HI(chid)  (0x610B20 + (chid) * 0x10)
#define NV_PDISP_CHAN_PUSH_LO(chid)  (0x610B24 + (chid) * 0x10)
#define NV_PDISP_CHAN_PUSH_CFG(chid) (0x610B28 + (chid) * 0x10)
#define NV_PDISP_CHAN_PUSH_LEN(chid) (0x610B2C + (chid) * 0x10)
#define NV_PDISP_CHAN_CTRL(chid)     (0x6104E0 + (chid) * 0x04)
#define NV_PDISP_CHAN_CTRL_ENABLE    0x00000010u
#define NV_PDISP_CHAN_CTRL_RUN       0x00000013u

/* The channel's own pair of pointers, in the window the card exposes for it. */
#define NV_PDISP_CHAN_USER(chid)     (0x690000 + ((chid) - 1) * 0x1000)
#define NV_PDISP_CHAN_USER_PUT(chid) (NV_PDISP_CHAN_USER(chid) + 0x00)
#define NV_PDISP_CHAN_USER_GET(chid) (NV_PDISP_CHAN_USER(chid) + 0x04)

/* Which channel is which.  The core is first and the windows follow it. */
#define NV_DISP_CHID_CORE            1
#define NV_DISP_CHID_WINDOW(w)       (2 + (w))

/* One mode, as a monitor describes it: the visible size, and where the sync
 * pulse sits inside a raster that is larger.  Named for the raster rather than
 * the mode because nv_disp.c already has a mode in the shape the older engine
 * wanted, and the two are not the same thing. */
typedef struct {
    u32  clock_khz;
    u32  hdisplay, hsync_start, hsync_end, htotal;
    u32  vdisplay, vsync_start, vsync_end, vtotal;
    bool hsync_negative, vsync_negative;
} nv_raster_t;

typedef struct {
    u32 *buffer;
    u64  phys;
    u32  words;
    u32  put;
    int  chid;
    bool ready;
} nv_dchan_t;

u32  nv_disp_core_class(u32 chipset);
u32  nv_disp_window_class(u32 chipset);
const char *nv_disp_class_name(u32 class_number);

bool nv_dchan_init(nv_card_t *c, nv_dchan_t *ch, int chid, u32 *memory,
                   u64 phys, u32 words);
void nv_dchan_method(nv_dchan_t *ch, u32 method, u32 value);
bool nv_dchan_kick(nv_card_t *c, nv_dchan_t *ch);

bool nv_disp_modern_modeset(nv_card_t *c, nv_dchan_t *core, nv_dchan_t *window,
                            int head, int window_index, int sor,
                            const nv_raster_t *mode, u64 surface, u32 pitch,
                            bool displayport);

/* The engine on the other side of the channel, for exercising the above.  See
 * the model at the end of nv_dispc37d.c. */
void nv_disp_model_attach(u32 core_class, u32 window_class);
void nv_disp_model_detach(void);
void nv_disp_model_write(u32 offset, u32 value);
int  nv_disp_model_methods(void);
bool nv_disp_model_updated(void);
u32  nv_disp_model_head_raster_width(void);
u32  nv_disp_model_head_raster_height(void);
u32  nv_disp_model_pixel_hz(void);
int  nv_disp_model_window_owner(int window);
u64  nv_disp_model_surface(void);
u32  nv_disp_model_window_width(void);
const char *nv_disp_model_complaint(void);

void nv_display_modern_init(nv_card_t *c);
int  nv_disp_modern_selftest(void);


/* ---------------------------------------------------------------- a Falcon
 *
 * One of the small processors inside the card.  From Turing onward the
 * important one is the GSP, which runs the whole of what used to be the
 * driver; before that they handled one job each.  All of them boot the same
 * way - see nv_falcon.c. */
typedef struct {
    const char *name;
    u32  base;               /* where its register block sits */
    u32  imem_size;          /* filled in by the reset, from the card itself */
    u32  dmem_size;
    bool ready;
    bool running;
} nv_falcon_t;

bool nv_falcon_reset(nv_card_t *c, nv_falcon_t *f);
bool nv_falcon_load_imem(nv_card_t *c, nv_falcon_t *f, const u8 *code,
                         size_t len, u32 at, bool secure);
bool nv_falcon_load_dmem(nv_card_t *c, nv_falcon_t *f, const u8 *data,
                         size_t len, u32 at);
bool nv_falcon_read_dmem(nv_card_t *c, nv_falcon_t *f, u8 *out, size_t len,
                         u32 at);
bool nv_falcon_start(nv_card_t *c, nv_falcon_t *f, u32 boot_vector);
bool nv_falcon_wait_halt(nv_card_t *c, nv_falcon_t *f, int timeout_ms);
bool nv_falcon_halted(nv_card_t *c, nv_falcon_t *f);
u32  nv_falcon_mailbox(nv_card_t *c, nv_falcon_t *f, int which);
void nv_falcon_set_mailbox(nv_card_t *c, nv_falcon_t *f, int which, u32 value);

/* Where the ones this driver cares about live. */
#define NV_FALCON_GSP   0x110000

/* ------------------------------------------------- the security processor
 *
 * On Hopper and Blackwell the host does not start the GSP itself.  It asks the
 * security processor - NVIDIA's FSP - to do it, by posting a message into that
 * processor's queue.  The boot registers further down are the older route and
 * are locked against the host on these parts.
 *
 * Established 2026-08-30 by reading nouveau, which drives this generation:
 * nvkm/subdev/gsp/gb202.c binds a GB202 to gh100_gsp_init, and that calls
 * nvkm_fsp_boot_gsp_fmc() rather than writing any boot register.  It also
 * names the firmware release as 570.144, which is the one this system already
 * looks for.
 *
 * Addresses from nouveau's copy of NVIDIA's Hopper header,
 * include/nvhw/ref/gh100/dev_fsp_pri.h.  Blackwell's own published header
 * (swref/published/blackwell/gb202/dev_fsp_pri.h) confirms the block sits in
 * the same place - its scratch registers are at 0x008F03xx - but does not
 * publish the queue itself.
 */
#define NV_PFSP_BASE              0x008F0000
#define NV_PFSP_LIMIT             0x008F3FFF
#define NV_PFSP_QUEUE_HEAD(i)     (0x008F2C00u + (i) * 8u)
#define NV_PFSP_QUEUE_TAIL(i)     (0x008F2C04u + (i) * 8u)
#define NV_PFSP_MSGQ_HEAD(i)      (0x008F2C80u + (i) * 8u)
#define NV_PFSP_MSGQ_TAIL(i)      (0x008F2C84u + (i) * 8u)

/* The security processor's own scratch registers.  Its boot and command
 * handling leave a status/error code here - NVIDIA's kfspDumpDebugState reads
 * exactly these four when the co-processor will not start, and they are the
 * only account of WHY the processor refused a message.  0x008F0320 + i*4,
 * i in 0..3, verified against gb202/gh100 dev_fsp_pri.h. */
#define NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(i)  (0x008F0320u + (i) * 4u)

/* The frame-buffer runtime-security (FRTS) scratch region in system memory.
 * Before the chain-of-trust message, NVIDIA's driver tells the security
 * processor where it reserved a region for FRTS in three PBUS SW-scratch
 * registers (kfspFrtsSysmemLocationProgram): the low and high halves of the
 * physical address and a config word (size in 4 KiB units, media type SYSMEM).
 * On a bare-metal card this is programmed BEFORE the COT; leaving these at
 * their reset value - a zero, i.e. INVALID, size - is a divergence from what
 * the FSP is set up to expect on this SKU. */
#define NV_PBUS_SW_SCRATCH(i)                     (0x00001400u + (i) * 4u)
#define NV_PBUS_SW_FRTS_INSECURE_ADDR_LO32        NV_PBUS_SW_SCRATCH(0x3Du)
#define NV_PBUS_SW_FRTS_INSECURE_ADDR_HI32        NV_PBUS_SW_SCRATCH(0x3Eu)
#define NV_PBUS_SW_FRTS_INSECURE_CONFIG           NV_PBUS_SW_SCRATCH(0x3Fu)
#define NV_PBUS_SW_FRTS_INSECURE_CONFIG_SIZE_4K_SHIFT      12u
#define NV_PBUS_SW_FRTS_INSECURE_CONFIG_MEDIA_TYPE_SYSMEM  (1u << 16)

/* The FRTS vidmem offset the FSP wants is measured FROM THE END OF THE FRAME
 * BUFFER and is a small fixed end-of-FB reserve, not the co-processor's reserved
 * region.  NVIDIA hardcodes it per architecture in memmgrGetFBEndReserveSizeEstimate_*:
 * GB20x (Blackwell) uses memmgrGetFBEndReserveSizeEstimate_GB100 == 0x220000
 * (Maxwell..Hopper base is 0x200000).  See kfspPrepareBootCommands_GH100. */
#define NV_FRTS_VIDMEM_END_RESERVE_GB20X          0x220000u

/* The message is MCTP-framed and carries an NVIDIA payload inside it: two
 * header words, then the "chain of trust" record naming where the firmware,
 * its hash, its public key and its signature are.  The processor verifies all
 * three before it starts anything, which is the whole reason this route
 * exists and the host route does not work here. */
#define NVDM_TYPE_COT             0x14

/* Which of the four processors on the card this system is starting. */
#define NV_FSP_QUEUE_BOOT         0

/* The message is not written to memory the card fetches; it is pushed word by
 * word into the security processor's own scratchpad through a two-register
 * window, and only then is the queue pointer moved.  Setting bit 24 in the
 * control register starts an auto-incrementing write at the given offset, so
 * the data register is written repeatedly and the address follows along; bit
 * 25 does the same for reading.
 *
 * Offsets are relative to the processor's block, so absolute for the FSP.
 * From nouveau nvkm/falcon/gp102.c (gp102_flcn_emem_pio_wr), which is the
 * routine gh100's FSP send path reaches through nvkm_falcon_pio_wr. */
/* EMEMC/EMEMD are at BASE+0x2AC0 / +0x2AC4, NOT +0xAC0 - dev_fsp_pri.h (gh100)
 * puts NV_PFSP_EMEMC at 0x008F2AC0 (the same 0x2xxx block as the queues at
 * 0x2C00), and the 0xAC0 form was missing the 0x2000, so the chain-of-trust
 * boot message was written 0x2000 low of the FSP's embedded-memory channel and
 * would never have reached the security processor on real silicon.  Verified by
 * tools/verify_nv_fsp.py.  WRITE/READ are the AINCW/AINCR auto-increment bits
 * (24/25). */
#define NV_PFSP_EMEMC(port)       (NV_PFSP_BASE + 0x2AC0u + (port) * 8u)
#define NV_PFSP_EMEMD(port)       (NV_PFSP_BASE + 0x2AC4u + (port) * 8u)
#define NV_PFSP_EMEMC_WRITE       (1u << 24)   /* AINCW */
#define NV_PFSP_EMEMC_READ        (1u << 25)   /* AINCR */

/* ------------------------------------------- starting the RISC-V co-processor
 *
 * From Ampere onward the processor that owns the card is a RISC-V core, and
 * from Hopper onward it is started from a firmware management controller image
 * rather than from a pair of signed booters.  The registers that do that are
 * below, as offsets inside the co-processor's block at NV_FALCON_GSP.
 *
 * These are not inferred.  NVIDIA publishes them, under the MIT licence, in
 * their open kernel modules - dev_riscv_pri.h for gb202 and gh100 - and the
 * values here were read from there rather than worked out from behaviour.
 * That matters: this driver has already once asked a card for firmware files
 * whose names were a plausible pattern rather than the published ones, and no
 * amount of care in the code around them made that work.
 */
#define NV_PRISCV_CPUCTL                    0x388
#define NV_PRISCV_CPUCTL_STARTCPU           (1u << 0)
#define NV_PRISCV_CPUCTL_HALTED             (1u << 4)
#define NV_PRISCV_CPUCTL_STOPPED            (1u << 5)
#define NV_PRISCV_CPUCTL_ACTIVE             (1u << 7)

/* Where the core is to fetch its first image from, and how. */
#define NV_PRISCV_BCR_DMACFG                0x66C
#define NV_PRISCV_BCR_DMACFG_TARGET_LOCAL_FB        0u
#define NV_PRISCV_BCR_DMACFG_TARGET_COHERENT_SYS    1u
#define NV_PRISCV_BCR_DMACFG_TARGET_NONCOHERENT_SYS 2u
#define NV_PRISCV_BCR_DMACFG_LOCK           (1u << 31)

/* The signature parameters, and the two halves of the management controller:
 * its code and its data.  Each address is a 32-bit low half and a 12-bit high
 * half, which is why the high registers are so narrow. */
#define NV_PRISCV_BCR_PKCPARAM_LO           0x670
#define NV_PRISCV_BCR_PKCPARAM_HI           0x674
#define NV_PRISCV_BCR_FMCCODE_LO            0x678
#define NV_PRISCV_BCR_FMCCODE_HI            0x67C
#define NV_PRISCV_BCR_FMCDATA_LO            0x680
#define NV_PRISCV_BCR_FMCDATA_HI            0x684

/* The addresses above are not stored as addresses.  Each is shifted right by
 * eight before it is written, which is why a thirty-two bit low half and a
 * twelve bit high half between them reach far more than forty-four bits of
 * memory - and why writing a physical address into them directly points the
 * co-processor at a location 256 times too low. */
#define NV_PRISCV_BCR_ADDR_SHIFT            8

/* The two mailboxes, which is where the arguments for the image being started
 * are left.  Ordinary Falcon registers, at the same offsets they have had
 * since Fermi. */
#define NV_PFALCON_FALCON_HWCFG2         0xF4
/* RISCV_BR_PRIV_LOCKDOWN is bit 13, NOT 11 - dev_falcon_v4.h puts
 * RISCV_PL3_DISABLE at 11:11 and RISCV_BR_PRIV_LOCKDOWN at 13:13, verified by
 * tools/verify_nv_gspboot.py.  The GSP boot polls this to clear (the boot ROM
 * releasing lockdown after it accepts the FMC); on bit 11 it watched the wrong
 * bit and would have misjudged the release on real Blackwell silicon. */
#define NV_PFALCON_HWCFG2_LOCKDOWN       (1u << 13)  /* RISCV_BR_PRIV_LOCKDOWN */
#define NV_PFALCON_MAILBOX0                 0x40
#define NV_PFALCON_MAILBOX1                 0x44
#define NV_FALCON_SEC2  0x840000

/* A Falcon that is not there, for exercising the code above where no card is.
 * See nv_falcon_model.c. */
void nv_falcon_model_attach(volatile u8 *window, u32 base);
void nv_falcon_model_detach(void);
/* The model is told about each access exactly, rather than left to notice that
 * a register changed: loading code is a run of writes to one port, and a block
 * of padding is a run of identical zeros that no change-detector can see. */
void nv_falcon_model_write(u32 offset, u32 value);
void nv_falcon_model_read(u32 offset);
int  nv_falcon_model_resets(void);
u32  nv_falcon_model_imem_written(void);
u32  nv_falcon_model_dmem_written(void);
int  nv_falcon_model_tags(void);
bool nv_falcon_model_started(void);
u32  nv_falcon_model_result(void);
u32  nv_falcon_model_bootvec(void);
bool nv_falcon_model_wrote_while_scrubbing(void);
const u8 *nv_falcon_model_imem(void);
const u8 *nv_falcon_model_dmem(void);

int  nv_falcon_selftest(void);

/* ------------------------------------------------- talking to the processor
 *
 * Once a Falcon is running, the card is driven by leaving messages in memory
 * both sides can see rather than by writing registers.  See nv_gsp_queue.c. */
/* Each ring lives in its own 256 KiB region: a header page, then sixty-three
 * page-sized elements.  The read pointers are swapped - each side's progress
 * through the OTHER's ring is parked one header's width into its own region -
 * which is why a queue remembers where its peer's region is. */
#define NV_GSP_QUEUE_REGION_BYTES 0x40000u

typedef struct {
    u8  *memory;             /* this ring's region                          */
    u8  *peer;               /* the other ring's region - see above         */
    u32  count;              /* elements in the ring                        */
    u32  sequence;           /* the last number handed out                  */
    bool ready;
} nv_gsp_queue_t;

void nv_gsp_cmdq_init(nv_gsp_queue_t *q, u8 *own, u8 *peer);
void nv_gsp_msgq_adopt(nv_gsp_queue_t *q, u8 *own, u8 *peer);

bool nv_gsp_rpc_send(nv_card_t *c, nv_gsp_queue_t *q, u32 function,
                     const void *payload, u32 payload_len, u32 *sequence_out);
/* Queue an early-boot RPC with the protocol's NOSEQ value and without ringing
 * a processor that has not been started yet.  GSP-RM consumes these entries
 * while it creates its object tree. */
bool nv_gsp_rpc_enqueue_preboot(nv_gsp_queue_t *q, u32 function,
                                const void *payload, u32 payload_len);
bool nv_gsp_rpc_receive(nv_card_t *c, nv_gsp_queue_t *q, u32 *function,
                        u32 *sequence, void *payload, u32 payload_cap,
                        u32 *payload_len, int timeout_ms);
/* Result words from the most recently received RPC header.  GSP operations
 * such as ALLOC_MEMORY return their status only here (there is deliberately
 * no status member in their body), so discarding these made a transport reply
 * indistinguishable from a successful RM operation. */
extern u32 nv_gsp_last_rpc_result;
extern u32 nv_gsp_last_rpc_result_private;
bool nv_gsp_rpc_call(nv_card_t *c, nv_gsp_queue_t *command,
                     nv_gsp_queue_t *status, u32 function,
                     const void *payload, u32 payload_len,
                     void *reply, u32 reply_cap, u32 *reply_len,
                     int timeout_ms);
bool nv_gsp_rpc_poll(nv_card_t *c, nv_gsp_queue_t *status, u32 function,
                     int timeout_ms);
void nv_gsp_ring_state(const char *tag, nv_gsp_queue_t *command,
                       nv_gsp_queue_t *status);

/* A processor that is not there, answering the way the protocol says.  By
 * default it echoes; a layer that understands the messages can supply its own
 * answers instead. */
typedef bool (*nv_gsp_model_handler_t)(u32 function, const u8 *in, u32 in_len,
                                       u8 *out, u32 out_cap, u32 *out_len);
void nv_gsp_model_set_handler(nv_gsp_model_handler_t handler);
void nv_gsp_model_attach(u8 *cmdq_region, u8 *msgq_region);
int  nv_gsp_model_bad_checksums(void);
void nv_gsp_model_detach(void);
void nv_gsp_model_wrote(u32 offset, u32 value);
int  nv_gsp_model_handled(void);
int  nv_gsp_model_events(void);

int  nv_gsp_queue_selftest(void);

/* R570 queues these two configuration messages before GSP bootstrap. */
bool nv_gsp_queue_async_init_rpcs(nv_card_t *c, nv_gsp_queue_t *command);
int  nv_gsp_init_rpc_selftest(void);

/* ------------------------------------------------- NVIDIA's resource manager
 *
 * The layer above the message rings.  On Ada and Blackwell it is the only way
 * to reach the engines at all: everything is an object in a tree, and the
 * whole interface is allocate, control, free.  See nv_gsp_rm.c.
 */
/* Big enough for the largest params struct we send: NV_CHANNEL_ALLOC_PARAMS is
 * 360 bytes (nv_chan.c static-asserts it).  The old 256 rejected the GPFIFO
 * channel allocation in nv_rm_alloc BEFORE it reached the wire ("a 360 byte
 * allocation is larger than a message") - a staging-buffer cap, not a hardware
 * limit: a queue element is a full page, so 360 (and this 1024) fit in one
 * element with room to spare (48-byte element header + RPC header + params). */
/* The Blackwell GLOBAL_SM_ORDER reply is 23,072 bytes.  The transport spans up
 * to 16 pages / 64 KiB, so a 32 KiB staging area fits both that topology reply
 * and the older ~3.2 KiB FIFO device-info table in one control call. */
#define NV_RM_MAX_PARAMS     32768
#define NV_RM_MAX_CONNECTORS 8

typedef struct {
    nv_gsp_queue_t command;
    nv_gsp_queue_t status;
    u32  client;
    /* True when allocate/control/free are dispatched through NVIDIA's linked
     * 595.99.02 host RM API instead of a second, directly-owned GSP queue.
     * Modern Kestrel boots must use this backend: host RM already owns GSP-RM
     * for NVKMS and two independent queue owners would corrupt the firmware. */
    bool host_api;
    bool display_query_attempted;
    u32 display_query_status;
    /* CPU mapping of the official HOPPER_USERMODE_A region.  Host RM creates
     * this object for the render client and returns the supported BAR0-backed
     * doorbell mapping; +0x90 is NVC361_NOTIFY_CHANNEL_PENDING. */
    volatile u8 *usermode;
    bool ready;
    bool up;
    int  allocations, controls, frees;
} nv_rm_t;

/* Allocate only NV04_DISPLAY_COMMON for read-only host queries. Boot caller
 * serializes this once per render tree; object lives until that tree is freed. */
u32 nv_rm_host_display_query_object(nv_card_t *c, nv_rm_t *rm, u32 *object);

typedef struct {
    u32 display_mask;
    u32 heads;
    u32 head_mask;
    u32 dp_max_rate, dp_max_lanes;
    struct {
        u32 display_id;
        u32 type;
    } connector[NV_RM_MAX_CONNECTORS];
    int connectors;
} nv_rm_display_t;

bool nv_rm_alloc(nv_card_t *c, nv_rm_t *rm, u32 parent, u32 object,
                 u32 class_number, void *params, u32 params_size);
extern u32 nv_last_alloc_status;   /* RM status of the last nv_rm_alloc reply */
/* Diagnostic for the last nv_rm_control call: nv_last_control_status is the RM
 * status when the reply came back (NV_OK=0 on success), or one of the sentinels
 * below when nv_rm_control returned false WITHOUT an RM status (so a silent
 * failure can be told apart from a real refusal in the durable diag).
 * nv_last_control_replylen is the reply byte count seen. */
#define NV_CTRL_FAIL_NOT_READY    0xE0000001u  /* rm not ready              */
#define NV_CTRL_FAIL_PARAMS_BIG   0xE0000002u  /* params_size > max         */
#define NV_CTRL_FAIL_NO_ANSWER    0xE0000003u  /* RPC timed out / no reply  */
#define NV_CTRL_FAIL_SHORT_REPLY  0xE0000004u  /* reply < header size       */
extern u32 nv_last_control_status;
extern u32 nv_last_control_replylen;
bool nv_rm_control(nv_card_t *c, nv_rm_t *rm, u32 object, u32 cmd,
                   const void *params, u32 params_size,
                   void *out, u32 out_cap, u32 *out_len);
/* Like nv_rm_control(RM_SUBDEVICE, ...) but falls back to GSP-RM's internal
 * privileged subdevice (from RPC 65) if the query is refused - for the
 * NV2080_CTRL_CMD_INTERNAL_* controls (GR ctxbufs, falcon info, display). */
bool nv_rm_control_internal(nv_card_t *c, nv_rm_t *rm, u32 cmd,
                            const void *params, u32 params_size,
                            void *out, u32 out_cap, u32 *out_len);
/* Like the above but tries the GSP internal privileged subdevice FIRST (what the
 * reference always does for ROUTE_TO_PHYSICAL / STATIC_KGR static queries). */
bool nv_rm_control_prefer_internal(nv_card_t *c, nv_rm_t *rm, u32 cmd,
                                   const void *params, u32 params_size,
                                   void *out, u32 out_cap, u32 *out_len);
/* Fetch GspStaticConfigInfo (RPC 65); fills the internal handles used above. */
bool nv_rm_get_static_info(nv_card_t *c, nv_rm_t *rm);
bool nv_rm_free(nv_card_t *c, nv_rm_t *rm, u32 parent, u32 object);
/* Send one of NVIDIA's versioned RM RPC payloads over the already-proven live
 * GSP queues.  Used by the NVKMS compatibility bridge for operations whose
 * payload is not GSP_RM_ALLOC/CONTROL/FREE. */
bool nv_rm_rpc_raw(nv_card_t *c, nv_rm_t *rm, u32 function,
                   const void *request, u32 request_size,
                   void *reply, u32 reply_cap, u32 *reply_size);
bool nv_rm_bring_up(nv_card_t *c, nv_rm_t *rm);
/* Create an independent render/codec client under the already-running
 * NVIDIA host RM.  It deliberately does not boot or reset GSP. */
bool nv_rm_host_bring_up(nv_card_t *c, nv_rm_t *rm);
/* Build a SECOND, independent RM client+device+subdevice for the display object
 * tree, mirroring nouveau's dedicated disp->rm.client/device (r535 disp.c:1536).
 * The disp objcom (0x0073) and disp-engine root (0xca70) must live under this,
 * not the shared device. Defined in nv_gsp_rm.c. */
bool nv_rm_disp_client_ctor(nv_card_t *c, nv_rm_t *rm,
                            u32 *out_client, u32 *out_device, u32 *out_subdevice);
bool nv_rm_shut_down(nv_card_t *c, nv_rm_t *rm);
bool nv_rm_query_display(nv_card_t *c, nv_rm_t *rm, nv_rm_display_t *out);

int  nv_rm_model_objects(void);
int  nv_rm_model_refusals(void);
bool nv_rm_model_saw_out_of_order(void);
int  nv_rm_selftest(void);

/* ------------------------------------------------- a real GPFIFO channel
 * Opens a channel + copy engine on the card over the live GSP-RM object tree
 * (nv_chan.c).  The gate for all engine work; returns 0 on success. */
int  nv_chan_open(nv_card_t *c, nv_rm_t *rm);
int  nv_chan_open_engines(nv_card_t *c, nv_rm_t *rm);  /* 3D/compute/NVDEC/NVENC */
bool nv_chan_is_open(void);
int  nv_chan_selftest(void);   /* runs a real copy on the card; 0 = worked */
int  nv_compute_selftest_hw(void); /* dispatches a real sm_120 shader; 0 = ran */
/* Nonblocking process-context transaction guard shared by native scanout and
 * off-screen submissions. Never hold a spinlock/disable interrupts while the
 * GPU runs. False means busy: retain the frame and retry, not CPU fallback. */
bool nv_render_try_begin(void);
void nv_render_end(void);
int  nv_3d_raster_selftest_hw(void); /* sm_120 triangle raster + byte proof */
int  nv_nvdec_selftest_hw(void);   /* decodes a real H.264 I-frame; 0 = decoded */
int  nv_nvdec_application_selftest_hw(void); /* serialized GPU-test boot: nonuniform independent reference */
int  nv_nvenc_selftest_hw(void);   /* bounded IDR smoke test; not round-trip validation */
bool nv_nvenc_test_bitstream(const u8 **data, u32 *bytes, u32 *slice_start, u32 *slice_end);
int  nv_nvenc_roundtrip_hw(void); /* serialized boot test: actual NVENC IDR -> NVDEC, full NV12 proof */
void nv_gsp_hw_verdicts(int *compute, int *runtime, int *nvdec,
                        int *nvenc, int *d3d);
/* Runtime render/display bridge.  The display driver binds its live VRAM
 * scanout into the already-proven copy and compute VASes after modeset.  These
 * calls are then the production path used by FB_PRESENT/SYS_GPU, rather than
 * boot-only self-tests. */
bool nv_chan_bind_scanout(u64 fb, u64 bytes, u32 pitch, u32 width, u32 height);
bool nv_chan_display_ready(void);
nv_card_t *nv_chan_display_card(void);
bool nv_chan_raster_ready(nv_card_t *card);
bool nv_chan_present_image(const u32 *pixels, u32 width, u32 height,
                           u32 source_stride, s32 x, s32 y);
bool nv_chan_fill_scanout(s32 x, s32 y, s32 w, s32 h, u32 colour);
bool nv_chan_copy_scanout(s32 from_x, s32 from_y, s32 to_x, s32 to_y,
                          s32 w, s32 h);
int  nv_chan_draw_triangles(const float *vertices, u32 triangles);
bool nv_chan_render_keepalive(void); /* fenced CE+SM heartbeat during display dwell */
int  nv_chan_runtime_selftest_hw(void); /* CE present/fill/copy + SM-to-scanout */
int  nv_chan_visible_2d_hw(void); /* full-screen CAB5 scene on bound scanout */
int  nv_chan_visible_3d_hw(void); /* full-screen SM raster scene on scanout */

/* VRAM allocation helper (nv_chan.c): allocate `size` bytes of VRAM through RM
 * and return its physical FB offset in *fb_offset.  Exported for the display
 * bring-up, which needs VRAM for the display instance block (RAMIN). */
bool nv_vram_alloc(nv_card_t *c, nv_rm_t *rm, u32 handle, u64 size, u64 *fb_offset);
/* Like nv_vram_alloc but with the driver's TYPE_PRIMARY scanout params (no
 * NO_SCANOUT) - use for a display surface that will actually be scanned out. */
bool nv_vram_alloc_scanout(nv_card_t *c, nv_rm_t *rm, u32 handle, u64 size, u64 *fb_offset);

/* DisplayPort re-light (nv_dispca7d.c): bring up the disp object tree + core and
 * window channels, train one DP output, and scan out the boot framebuffer so an
 * accelerated desktop is visible after GSP boot.  Gated behind the "displaytest"
 * cmdline flag by the caller (UPDATE on an untrained link can black the panel). */
void nv_disp_lightup(nv_card_t *c, nv_rm_t *rm);

/* Pre-allocate the display scanout after GR golden-image capture and before
 * ordinary 3D/compute/NVDEC/NVENC channel buffers.  A late 22MB contiguous
 * alloc failed with NV_ERR_NO_MEMORY, while putting it before golden capture
 * starved the golden CE97 constructor.  Engine bring-up owns this ordering. */
void nv_disp_prealloc_scanout(nv_card_t *c, nv_rm_t *rm);

/* Durable re-light diagnostics.  The klog ring (512 entries) evicts the whole
 * nv-disp trace before the stick flush, so nv_disp_lightup records each RM
 * step's result HERE (survives the ring) and gpu.c prints it in the summary -
 * one boot then says exactly which query returned what and where it stopped.
 * See [[kestrelos-derive-verdicts-dont-recover-them]]. */
typedef struct {
    bool ran;                 /* nv_disp_lightup was entered              */
    const char *stopped_at;   /* last stage reached (a static string)     */
    bool edid_ok;             /* EDID parsed to a usable native mode       */
    u32  mode_w, mode_h, mode_pclk_khz;
    bool disp_root_ok;        /* 0xca70 disp root allocated                */
    u32  disp_root_status;    /* RM status if the 0xca70 alloc was refused */
    bool inst_mem_ok;         /* WRITE_INST_MEM (RAMIN registered) accepted */
    bool static_info_ok; u32 windows_present, num_heads;
    u32  supported_mask;      /* re-light's OWN GET_SUPPORTED (post-init)  */
    u32  connected_mask;      /* GET_CONNECT_STATE: which slots have a sink */
    u32  sink_count;          /* connected sinks enumerated at runtime       */
    u32  edid_mask;           /* sinks with a valid per-output GET_EDID_V2    */
    u32  lit_mask;            /* outputs with a committed scanout path        */
    u32  rgbw_mask;           /* outputs participating in the RGBW sequence   */
    u32  display_id;          /* chosen display id                         */
    bool or_info_ok; u32 or_index, or_type, or_protocol;
    bool caps_ok; u32 max_link_rate;
    bool assign_sor_ok; int sor_slot;
    int  dp_attempts;         /* DP_CTRL attempts made                     */
    u32  dp_last_err, dp_last_retry_ms;
    u32  dp_ctrl_status;      /* nv_last_control_status after DP_CTRL (real reason) */
    bool dpcd_valid;          /* sink DPCD AUX read succeeded                        */
    u32  dpcd_aux_status;     /* nv_last_control_status of the AUXCH_CTRL read       */
    u32  dpcd_reply_type;     /* AUX replyType (0=ACK 1=NACK 2=DEFER 3=TIMEOUT)      */
    u32  dpcd_got_bytes;      /* bytes the AUX read returned                         */
    u32  dpcd_rate;           /* DPCD max_link_rate_code read from the sink          */
    u32  dpcd_lanes;          /* DPCD max_lane_count read from the sink              */
    u32  dp_sent_cmd;         /* the cmd word actually sent to DP_CTRL               */
    u32  dp_sent_data;        /* the data word actually sent to DP_CTRL              */
    bool restore_sor_ok;      /* post-release DFP_ASSIGN_SOR restored the RM route     */
    bool rgb_test_shown;      /* solid-colour scanout was committed for the RGB test */
    u32  rgb_alloc_status;    /* nv_last_alloc_status if the scanout surface alloc failed */
    u32  chan_pb_status;      /* CHANNEL_PUSHBUFFER control status at the failing dchan */
    u32  chan_alloc_status;   /* channel-object alloc status at the failing dchan     */
    u8   chan_fail_stage;     /* 0=none 1=pushbuffer 2=object-alloc                    */
    bool trained;             /* DP link trained clean (UPDATE permitted)  */
    bool core_chan_ok, window_chan_ok;
    bool committed;           /* full 3-phase modeset kicked               */
    u32  core_put;            /* core channel PUT after the final kick      */
    u32  core_get;            /* core channel GET (advances to PUT if the channel consumed it) */
    u32  window_put;          /* window0 channel PUT after interlocked attach */
    u32  window_get;          /* window0 GET; must reach PUT with core partner */
    bool wm_override;         /* DP_CONFIG_STREAM sent with bEnableOverride=1 + computed watermark */
    u32  wm_value;            /* the computed SST waterMark                 */
    bool olut_ok;             /* identity OLUT surface allocated + filled   */
    bool update_notified;     /* the core UPDATE completion notifier fired (modeset really latched) */
    u32  notifier_val;        /* raw first dword the display engine wrote to the notifier */
    u32  core_exc_stat, core_exc_data, core_exc_code; /* NVDisplay FE_EXCEPT chid 0 */
    u32  win_exc_stat, win_exc_data, win_exc_code;    /* NVDisplay FE_EXCEPT chid 1 */
    u32  exc_other;           /* 0x611854 core/other exception summary */
    u32  exc_window;          /* 0x61184c window exception summary     */
    u32  ctrl_detail;         /* 0x611848 control error/detail, raw   */
    u32  awaken_win;          /* 0x611858 window awaken status, raw   */
    u32  awaken_other;        /* 0x61185c core awaken status, raw     */
    u32  sem_win;             /* 0x611868 window semaphore status     */
    u32  ctrl_intr;           /* 0x611c30 display-control interrupt   */
    u32  rg_loadv_before, rg_loadv_after;             /* 0x616320 head0 */
    u32  postcomp_loadv_before, postcomp_loadv_after; /* 0x61a11c head0 */
    u32  crashlock_before, crashlock_after;           /* 0x616484 head0 */
    u32  sor_dp_linkctl;      /* NV_PDISP_SOR_DP_LINKCTL(or,link0) after UPDATE - is the DP link enabled on the pad */
    u32  sor_dp_padctl;       /* NV_PDISP_SOR_DP_PADCTL(or) after UPDATE - which lanes the SOR pad is driving */
    /* ARM-vs-ASSY promotion probe (gv100.c:266-285 dual-copy; ARM=ASSY+0x8000).
     * THE decisive "did the UPDATE promote" signal: ASSY holds what we pushed,
     * ARM holds what is actually latched/live.  ASSY set but ARM zero/stale =
     * UPDATE accepted into assembly but never promoted (the exact failure). */
    u32  head_raster_assy;    /* 0x682064 + h*0x800  VTOTAL[31:16]|HTOTAL[15:0] pushed  */
    u32  head_raster_arm;     /* 0x68a064 + h*0x800  same, actually latched              */
    u32  head_pclk_arm;       /* 0x68a00c + h*0x400  latched pixel clock Hz (0 = none)   */
    u32  rg_dpca_1, rg_dpca_2;/* 0x616330 + h*0x800  LINE[15:0]|FRM[31:16] sampled twice */
    u32  sf_dp_ctl;           /* 0x616550 + h*0x800  bit27=active-sym enable, [5:0]=wm   */
    u32  sor_owner_assy;      /* 0x680300 + sor*0x20 [7:0]=head mask [11:8]=proto pushed */
    u32  sor_owner_arm;       /* 0x688300 + sor*0x20 same, actually latched              */
    u32  sor_pwr;             /* 0x61c004 + sor*0x800 [0]=PU normal [31]=pending         */
    u32  sor_seq_ctl;         /* 0x61c030 + sor*0x800 [28]=sequencer busy                */
    u32  core_chan_state;     /* 0x610630 [20:16]==0xb = core channel quiescent/idle-good */
    u32  sv_pending;          /* 0x611c30 [2:0] = supervisor interrupt pending           */
    u32  sor_owner_assy_p1;   /* 0x680300 SOR state right after Phase-1 UPDATE (owner+proto) */
    u32  sor_owner_arm_p1;    /* 0x688300 same, ARM copy - did Phase 1 attach the SOR?      */
    u32  sor_attach_value;    /* exact CA7D SOR_SET_CONTROL value requested (expected 0x801) */
    u32  xbar_output;         /* connector output index = ctz(display_id)             */
    u32  xbar_expected;       /* expected low 5 route bits: (SOR+1) | linkB<<4        */
    u32  xbar_a_before, xbar_b_before; /* route registers before DFP_ASSIGN_SOR       */
    u32  xbar_a_assigned, xbar_b_assigned; /* immediately after RM assignment          */
    u32  xbar_a_after, xbar_b_after;   /* after final CA7D attach                     */
    bool rm_active_ok;        /* SYSTEM_GET_ACTIVE control accepted                  */
    u32  rm_active_display;   /* RM's active display for head0 after DISPLAY_CHANGE  */
    bool display_from_active; /* displayId chosen from GET_ACTIVE boot head (not lowest-bit) */
    u32  active_head;         /* the hw head whose active boot display we selected   */
    /* DP detach-before-attach (Phase B/C/D): release a possibly-stuck RM-owned SOR
     * (post-SBR) in a SEPARATE, EARLIER core UPDATE before the fresh attach, the
     * NVKMS normal DP path (nvkms-modeset.c:2455-2498; nouveau r535_dp_release
     * disp.c:1124-1137). */
    bool detach_kicked;                 /* Phase B SOR-detach UPDATE was kicked+consumed */
    bool sor_released;                  /* SOR ARM owner cleared after the detach UPDATE  */
    u32  sor_owner_arm_before_detach;   /* 0x688300 SOR owner ARM before Phase B          */
    u32  sor_owner_arm_after_detach;    /* 0x688300 SOR owner ARM after Phase B           */
    u32  detach_dp_release_err;         /* dp.err from the Phase C DP_CTRL lane=0 release */
} nv_disp_relight_diag_t;
void nv_disp_relight_get_diag(nv_disp_relight_diag_t *out);

/* DisplayPort sink capabilities read from the monitor's DPCD over the AUX
 * channel (NV0073_CTRL_CMD_DP_AUXCH_CTRL).  nouveau needs these to choose the
 * link config (lane count + rate) before asking GSP-RM to train the link.
 * Implemented in nv_dp_aux.c. */
typedef struct {
    u8   raw[16];             /* DPCD bytes 0x00000..0x0000F                */
    u32  rev;                 /* DPCD 0x0 (e.g. 0x14 = DP 1.4)              */
    u32  max_link_rate_code;  /* DPCD 0x1: 0x06=1.62 0x0a=2.7 0x14=5.4 0x1e=8.1 */
    u32  max_lanes;           /* DPCD 0x2 bits[4:0]: 1/2/4                   */
    bool enhanced_framing;    /* DPCD 0x2 bit7 (ENHANCED_FRAME_CAP)          */
    bool tps4_supported;      /* DPCD 0x3 bit? (TPS4_SUPPORTED)              */
    bool valid;               /* the AUX read succeeded                      */
    u32  reply_type;          /* NV0073_CTRL_DP_AUXCH_REPLYTYPE (0=ACK 1=NACK 2=DEFER 3=TIMEOUT) */
    u32  got_bytes;           /* bytes the AUX read returned                 */
} nv_dpcd_t;
/* Read the first 16 DPCD bytes for display_id through the disp objcom, parse the
 * link caps into *out. Returns out->valid. objcom is the NV04_DISPLAY_COMMON
 * handle; call with rm->client already set to the disp client. */
bool nv_dp_read_dpcd(nv_card_t *c, nv_rm_t *rm, u32 objcom, u32 display_id,
                     nv_dpcd_t *out);
/* Read an arbitrary DPCD range, split into the RM AUX ABI's 16-byte maximum.
 * Needed for DSC capability bytes (0x60..0x6f) and FEC (0x90). */
bool nv_dp_aux_read(nv_card_t *c, nv_rm_t *rm, u32 objcom, u32 display_id,
                    u32 addr, u8 *data, u32 len);
/* Native-AUX write of one DPCD byte (e.g. DP_SET_POWER 0x600 = D0). */
bool nv_dp_write_dpcd(nv_card_t *c, nv_rm_t *rm, u32 objcom, u32 display_id,
                      u32 addr, u8 val);

/* ------------------------------------------------------- submitting work
 *
 * How anything is drawn faster than the processor can draw it.  A ring of
 * pointers to push buffers, a push buffer of method-and-value pairs addressed
 * to subchannels, and a semaphore that says when it is done.  The same shape
 * since Fermi and still the shape on Blackwell.  See nv_fifo.c.
 */
#define NV_PUSH_OP_INC   1     /* values go to consecutive methods           */
#define NV_PUSH_OP_NINC  3     /* all to the same method                     */
#define NV_PUSH_OP_IMMD  4     /* the value travels inside the header        */
#define NV_PUSH_OP_1INC  5     /* the first two, then the same one           */

#define NV_FIFO_SET_OBJECT          0x0000
#define NV_FIFO_SEMAPHOREA          0x0010
#define NV_FIFO_SEMAPHOREB          0x0014
#define NV_FIFO_SEMAPHOREC          0x0018
#define NV_FIFO_SEMAPHORED          0x001C
#define NV_FIFO_SEMAPHORED_ACQUIRE  0x00000001
#define NV_FIFO_SEMAPHORED_RELEASE  0x00000002
#define NV_FIFO_SEMAPHORED_AWAKEN   0x00100000

/* Where a channel's ring is told about.  The instance block that describes the
 * rest of the channel is built by whatever set the channel up. */
#define NV_PFIFO_GPFIFO_BASE_LO(ch)  (0x800000 + (ch) * 0x20 + 0x00)
#define NV_PFIFO_GPFIFO_BASE_HI(ch)  (0x800000 + (ch) * 0x20 + 0x04)
#define NV_PFIFO_GPFIFO_SIZE(ch)     (0x800000 + (ch) * 0x20 + 0x08)
#define NV_PFIFO_CHANNEL_GP_PUT(ch)  (0x800000 + (ch) * 0x20 + 0x0C)
/* How far the card has actually read.  Without it a driver has no way to know
 * an entry is free again, and the ring fills up once and stays full. */
#define NV_PFIFO_CHANNEL_GP_GET(ch)  (0x800000 + (ch) * 0x20 + 0x10)

/* The two-dimensional class, unchanged since Fermi. */
#define NV_CLASS_TWOD                        0x902D
#define NV902D_SET_DST_FORMAT                0x0200
#define NV902D_SET_DST_MEMORY_LAYOUT         0x0204
#define NV902D_SET_DST_PITCH                 0x0214
#define NV902D_SET_DST_WIDTH                 0x0218
#define NV902D_SET_DST_HEIGHT                0x021C
#define NV902D_SET_DST_OFFSET_UPPER          0x0220
#define NV902D_SET_DST_OFFSET_LOWER          0x0224
#define NV902D_SET_CLIP_ENABLE               0x0290
#define NV902D_SET_OPERATION                 0x02AC
#define NV902D_RENDER_SOLID_PRIM_MODE        0x0580
#define NV902D_SET_RENDER_SOLID_PRIM_COLOR   0x0588
#define NV902D_RENDER_SOLID_PRIM_POINT_SET_X(i) (0x0600 + (i) * 8)
#define NV902D_RENDER_SOLID_PRIM_POINT_Y(i)     (0x0604 + (i) * 8)

/* The source surface, and the blit that reads from it.  These MIRROR the
 * destination methods above at a base 0x30 higher (DST is 0x0200, SRC 0x0230),
 * same fields in the same order - NOT at 0x0100, which is where they were first
 * transcribed and is wrong: cl902d.h puts SRC right after DST.  Verified by
 * tools/verify_nv_2d.py against NVIDIA's cl902d.h; a wrong SRC offset makes the
 * copy engine read the source from nowhere and corrupt every blit. */
#define NV902D_SET_SRC_FORMAT                0x0230
#define NV902D_SET_SRC_MEMORY_LAYOUT         0x0234
#define NV902D_SET_SRC_PITCH                 0x0244
#define NV902D_SET_SRC_WIDTH                 0x0248
#define NV902D_SET_SRC_HEIGHT                0x024C
#define NV902D_SET_SRC_OFFSET_UPPER          0x0250
#define NV902D_SET_SRC_OFFSET_LOWER          0x0254

/* Whether the engine must assume the two rectangles overlap.  A window being
 * dragged across the screen copies a surface onto itself, and without this the
 * engine may read a row it has already overwritten. */
#define NV902D_SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP   0x0888
#define NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE    0x088C

#define NV902D_SET_PIXELS_FROM_MEMORY_DST_X0         0x08B0
#define NV902D_SET_PIXELS_FROM_MEMORY_DST_Y0         0x08B4
#define NV902D_SET_PIXELS_FROM_MEMORY_DST_WIDTH      0x08B8
#define NV902D_SET_PIXELS_FROM_MEMORY_DST_HEIGHT     0x08BC
#define NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC     0x08C0
#define NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_INT      0x08C4
#define NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_FRAC     0x08C8
#define NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_INT      0x08CC
#define NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC    0x08D0
#define NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_INT     0x08D4
#define NV902D_SET_PIXELS_FROM_MEMORY_SRC_Y0_FRAC    0x08D8
/* Writing this one is what starts the blit, so it goes last. */
#define NV902D_PIXELS_FROM_MEMORY_SRC_Y0_INT         0x08DC

#define NV902D_FORMAT_A8R8G8B8   0xCF
#define NV902D_LAYOUT_PITCH      0x01
#define NV902D_OPERATION_SRCCOPY 0x03
#define NV902D_PRIM_MODE_RECTS   0x04

typedef struct {
    u64 *entries;
    u64  entries_gpu;
    u32  count;
    u32  put, got;

    u32 *push;
    u64  push_gpu;
    u32  push_words;
    u32  push_at;
    u32  submitted_to;

    volatile u32 *semaphore;
    u64  semaphore_gpu;

    u32  bound[8];
    u32  expected, emitted;
    int  bad_packets;
    int  submissions;
    bool overflowed;
    int  chid;
    bool ready;
} nv_fifo_t;

bool nv_fifo_init(nv_card_t *c, nv_fifo_t *f, int chid,
                  u64 *entries, u64 entries_gpu, u32 entry_count,
                  u32 *push, u64 push_gpu, u32 push_words,
                  volatile u32 *semaphore, u64 semaphore_gpu);
void nv_push_begin(nv_fifo_t *f, int subchannel, u32 method, u32 count);
void nv_push_begin_same(nv_fifo_t *f, int subchannel, u32 method, u32 count);
void nv_push_data(nv_fifo_t *f, u32 value);
void nv_push_immediate(nv_fifo_t *f, int subchannel, u32 method, u32 data);
bool nv_push_end(nv_fifo_t *f);
bool nv_fifo_submit(nv_card_t *c, nv_fifo_t *f);
bool nv_fifo_bind(nv_card_t *c, nv_fifo_t *f, int subchannel, u32 class_number);

/* Handed a channel, this is where acceleration begins - declared here because
 * it is the first thing above that needs a channel's type. */
bool nv_accel_use_channel(nv_card_t *c, const nv_fifo_t *f);
bool nv_fifo_fence(nv_card_t *c, nv_fifo_t *f, u32 value);
bool nv_fifo_wait(nv_fifo_t *f, u32 value, int timeout_ms);
bool nv_2d_fill(nv_card_t *c, nv_fifo_t *f, int subchannel, u64 surface,
                u32 pitch, u32 width, u32 height,
                u32 x, u32 y, u32 w, u32 h, u32 colour);
bool nv_2d_copy(nv_card_t *c, nv_fifo_t *f, int subchannel,
                u64 dst, u32 dst_pitch, u32 dst_width, u32 dst_height,
                u64 src, u32 src_pitch, u32 src_width, u32 src_height,
                u32 dx, u32 dy, u32 sx, u32 sy, u32 w, u32 h);

void nv_fifo_model_attach(u64 semaphore_gpu, u64 gpu_base, u8 *host_base,
                          u32 span);
void nv_fifo_model_set_3d_class(u32 class_number);
void nv_fifo_model_detach(void);
void nv_fifo_model_write(u32 offset, u32 value);
int  nv_fifo_model_packets(void);
int  nv_fifo_model_methods(void);
int  nv_fifo_model_draws(void);
const char *nv_fifo_model_complaint(void);
int  nv_fifo_selftest(void);

/* ------------------------------------------------- the card's own page tables
 *
 * Every address a card is given is a graphics address, and the card walks a
 * tree of tables the driver built to turn it into a real one.  Five levels on
 * Pascal and everything since.  See nv_vmm.c.
 */
typedef struct {
    u8  *pool;          /* memory the tables themselves live in            */
    u64  pool_gpu;
    u32  pages;
    u32  used;
    u64  root_gpu;      /* what the card is told about                     */
    u32  mapped;
    bool tables_in_vram;
    bool ready;
} nv_vmm_t;

/* Description of one page-directory level supplied to
 * NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES.  The control needs every
 * page-table allocation from the root down through the level whose entries
 * cover the reserved 512-MiB server-RM window. */
typedef struct {
    u64 phys_address;
    u64 size;
    u32 aperture;
    u8  page_shift;
    u8  _pad[3];
} nv_vmm_level_t;
_Static_assert(sizeof(nv_vmm_level_t) == 24,
               "server-PDE level descriptor is 24 bytes");

bool nv_vmm_init(nv_vmm_t *vmm, u8 *pool, u64 pool_gpu, u32 pages,
                 bool tables_in_vram);
/* `priv` maps the pages with a PRIVILEGE PCF (for GR/engine context-switch
 * buffers); false gives the REGULAR PCF used by rings/pushbuffers/semaphores. */
bool nv_vmm_map(nv_vmm_t *vmm, u64 va, u64 pa, u64 bytes, bool vram,
                bool read_only, bool priv);
/* Same mapping primitive for RM-owned allocations whose PTE KIND is reported
 * by NV2080_CTRL_*_GET_CTX_BUFFER_INFO. */
bool nv_vmm_map_kind(nv_vmm_t *vmm, u64 va, u64 pa, u64 bytes, bool vram,
                     bool read_only, bool priv, u32 kind);
bool nv_vmm_unmap(nv_vmm_t *vmm, u64 va, u64 bytes);
bool nv_vmm_reserve_512m_levels(nv_vmm_t *vmm, u64 va,
                                nv_vmm_level_t levels[4]);
int  nv_vmm_selftest(void);

/* --------------------------------------------------------- drawing triangles
 *
 * The three-dimensional class.  The method set has not changed since Fermi and
 * these class numbers run to Blackwell, so a driver that speaks it draws on
 * every NVIDIA card of the last fifteen years.  See nv_3d.c.
 */
#define NV_CLASS_3D_FERMI      0x9097
#define NV_CLASS_3D_KEPLER     0xA097
#define NV_CLASS_3D_MAXWELL    0xB097   /* GM10x                            */
#define NV_CLASS_3D_MAXWELL_B  0xB197   /* GM20x - a second class, same family */
#define NV_CLASS_3D_PASCAL     0xC097
#define NV_CLASS_3D_VOLTA      0xC397
#define NV_CLASS_3D_TURING     0xC597
#define NV_CLASS_3D_AMPERE     0xC697
#define NV_CLASS_3D_ADA        0xC997
#define NV_CLASS_3D_HOPPER     0xCB97
/* Blackwell is two chips, not one binned two ways: the die that went to
 * datacentres and the die that went into GeForce are different silicon and
 * answer to different class numbers. */
#define NV_CLASS_3D_BLACKWELL  0xCD97   /* GB10x - B100, B200               */
#define NV_CLASS_3D_BLACKWELL_GEFORCE 0xCE97  /* GB20x - the RTX 50 series  */

#define NV9097_MEM_BARRIER              0x021C
#define NV9097_RT_ADDRESS_HIGH(i)       (0x0800 + (i) * 0x40)
#define NV9097_RT_HORIZ(i)              (0x0808 + (i) * 0x40)
#define NV9097_RT_VERT(i)               (0x080C + (i) * 0x40)
#define NV9097_RT_FORMAT(i)             (0x0810 + (i) * 0x40)
#define NV9097_RT_TILE_MODE(i)          (0x0814 + (i) * 0x40)
#define NV9097_RT_TILE_MODE_LINEAR      0x00001000
#define NV9097_VIEWPORT_HORIZ(i)        (0x0C00 + (i) * 0x10)
#define NV9097_VERTEX_BUFFER_FIRST      0x0D74
#define NV9097_CLEAR_COLOR(i)           (0x0D80 + (i) * 4)
#define NV9097_SCREEN_SCISSOR_HORIZ     0x0FF4
#define NV9097_SCREEN_SCISSOR_VERT      0x0FF8
#define NV9097_VERTEX_ATTRIB_FORMAT(i)  (0x1160 + (i) * 4)
#define NV9097_RT_CONTROL               0x121C
#define NV9097_CODE_ADDRESS_HIGH        0x1608
#define NV9097_VERTEX_END_GL            0x1614
#define NV9097_VERTEX_BEGIN_GL          0x1618
#define NV9097_CLEAR_BUFFERS            0x19D0
#define NV9097_QUERY_ADDRESS_HIGH       0x1B00
#define NV9097_QUERY_SEQUENCE           0x1B08
#define NV9097_QUERY_GET                0x1B0C
#define NV9097_VERTEX_ARRAY_FETCH(i)    (0x1C00 + (i) * 0x10)
#define NV9097_VERTEX_ARRAY_START_HIGH(i) (0x1C04 + (i) * 0x10)
#define NV9097_SP_SELECT(i)             (0x2000 + (i) * 0x40)
#define NV9097_SP_START_ID(i)           (0x2004 + (i) * 0x40)

/* Volta+ (NVC397 .. NVCE97/Blackwell) additions - verified against Mesa
 * clce97.h.  These REPLACE the pre-Volta SP_SELECT/SP_START_ID/CODE_ADDRESS
 * shader-load and the missing viewport transform:
 *   - the NDC->screen transform (SCALE and OFFSET, enabled by SCALE_OFFSET),
 *     without which nothing rasterises even with everything else correct;
 *   - the shader load: SET_PIPELINE_SHADER(j) [enable|type], REGISTER_COUNT,
 *     BINDING, and PROGRAM_ADDRESS_A/B(j) = the full 40-bit GPU VA of the
 *     shader's program header (SPH).  0x2004/0x2008 are RESERVED - never write.
 * SET_PIPELINE_SHADER(j) reuses the SP_SELECT offset (0x2000 + j*0x40). */
#define NV9097_SET_VIEWPORT_SCALE_X(i)  (0x0A00 + (i) * 0x20)   /* +0/4/8 = X/Y/Z */
#define NV9097_SET_VIEWPORT_OFFSET_X(i) (0x0A0C + (i) * 0x20)   /* +0/4/8 = X/Y/Z */
#define NV9097_SET_VIEWPORT_SCALE_OFFSET 0x192C                 /* bit0 ENABLE    */
#define NV9097_SET_PIPELINE_SHADER(j)         (0x2000 + (j) * 0x40)
#define NV9097_SET_PIPELINE_REGISTER_COUNT(j) (0x200C + (j) * 0x40)
#define NV9097_SET_PIPELINE_BINDING(j)        (0x2010 + (j) * 0x40)
#define NV9097_SET_PIPELINE_PROGRAM_ADDRESS_A(j) (0x2014 + (j) * 0x40)  /* upper 8 */
#define NV9097_SET_PIPELINE_PROGRAM_ADDRESS_B(j) (0x2018 + (j) * 0x40)  /* lower 32 */
#define NV9097_SET_PIPELINE_SHADER_ENABLE     0x00000001
#define NV9097_SET_PIPELINE_SHADER_TYPE_VERTEX 0x1     /* type field bits 7:4 */
#define NV9097_SET_PIPELINE_SHADER_TYPE_PIXEL  0x5
/* SET_REPORT_SEMAPHORE_D (0x1b0c) trigger: OPERATION_RELEASE(0) |
 * RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE(bit4) |
 * PIPELINE_LOCATION_ALL(0xF<<12) | STRUCTURE_SIZE_ONE_WORD(bit28). */
#define NV9097_REPORT_SEMAPHORE_D_RELEASE_ALL  0x1000F010u

#define NV9097_SP_SELECT_ENABLE         0x00000001
#define NV9097_VERTEX_ARRAY_FETCH_ENABLE 0x00001000
#define NV9097_CLEAR_BUFFERS_R          0x00000004
#define NV9097_CLEAR_BUFFERS_G          0x00000008
#define NV9097_CLEAR_BUFFERS_B          0x00000010
#define NV9097_CLEAR_BUFFERS_A          0x00000020
#define NV9097_PRIMITIVE_POINTS         0
#define NV9097_PRIMITIVE_LINES          1
#define NV9097_PRIMITIVE_TRIANGLES      4

/* Which program slot is which stage. */
#define NV_STAGE_VERTEX_A   0
#define NV_STAGE_VERTEX_B   1
#define NV_STAGE_GEOMETRY   4
#define NV_STAGE_FRAGMENT   5

/* How an attribute is laid out inside a vertex, as the field encodes it. */
#define NV9097_ATTRIB_32_32_32_32_FLOAT  (0x00200000 | (0x7 << 27))
#define NV9097_ATTRIB_32_32_FLOAT        (0x00800000 | (0x7 << 27))

u32  nv_3d_class(u32 chipset);
const char *nv_3d_class_name(u32 class_number);

bool nv_3d_set_target(nv_card_t *c, nv_fifo_t *f, int subchannel,
                      u64 surface, u32 width, u32 height, u32 format);
bool nv_3d_set_viewport(nv_card_t *c, nv_fifo_t *f, int subchannel,
                        u32 x, u32 y, u32 width, u32 height);
/* Volta+ shader load: PROGRAM_ADDRESS is a full 40-bit GPU VA of the shader's
 * program header (SPH), not an offset into a shared code region - so there is
 * no separate code-region call any more. */
bool nv_3d_set_program(nv_card_t *c, nv_fifo_t *f, int subchannel,
                       int stage, bool enabled, u64 program_va, u32 reg_count);
bool nv_3d_set_vertex_stream(nv_card_t *c, nv_fifo_t *f, int subchannel,
                             int stream, u64 at, u32 stride);
bool nv_3d_set_attribute(nv_card_t *c, nv_fifo_t *f, int subchannel,
                         int attribute, int stream, u32 offset, u32 format);
bool nv_3d_clear(nv_card_t *c, nv_fifo_t *f, int subchannel,
                 float r, float g, float b, float a);
bool nv_3d_draw(nv_card_t *c, nv_fifo_t *f, int subchannel,
                u32 primitive, u32 first, u32 count);
bool nv_3d_report(nv_card_t *c, nv_fifo_t *f, int subchannel, u64 at, u32 value);

void nv_3d_model_attach(void);
void nv_3d_model_detach(void);
void nv_3d_model_method(int subchannel, u32 method, u32 value);
int  nv_3d_model_draws(void);
int  nv_3d_model_clears(void);
int  nv_3d_model_reports(void);
int  nv_3d_model_refusals(void);
u32  nv_3d_model_primitive(void);
u32  nv_3d_model_count(void);
u64  nv_3d_model_target(void);
bool nv_3d_model_viewport_transform(void);
u64  nv_3d_model_stage_addr(int stage);
const char *nv_3d_model_complaint(void);
int  nv_3d_selftest(void);
int  nv_3d_selftest_hw(void);

/* Reading and writing the register window.  Every access to a card goes
 * through these two, so a model can stand in for one. */
u32  nv_rd32(nv_card_t *c, u32 offset);
void nv_wr32(nv_card_t *c, u32 offset, u32 value);

/* The security processor model, which watches the register writes that make up
 * a chain-of-trust message. */
void nv_fsp_model_write(u32 offset, u32 value);
void nv_fsp_model_read(u32 offset);
int  nv_fsp_selftest(void);

/* A durable account of the most recent FSP handoff.  The ordinary kernel log
 * may disappear with the display when a reset is attempted, so gpu.c also
 * writes these fields to the FAT boot volume. */
typedef struct {
    bool attempted;
    bool flr_requested;
    bool flr_supported;
    bool flr_performed;
    bool flr_returned;
    bool framebuffer_released;
    bool secure_boot_ready;
    bool emem_verified;
    bool command_consumed;
    bool response_seen;
    bool response_valid;
    u32  secure_boot_value;
    u32  cmd_head_before, cmd_tail_before;
    u32  msg_head_before, msg_tail_before;
    u32  cmd_head_after, cmd_tail_after;
    u32  msg_head_after, msg_tail_after;
    u32  response_task, response_command, response_error;
    u32  scratch[4];
} nv_fsp_diag_t;

void nv_fsp_get_diag(nv_fsp_diag_t *out);
const char *nv_fsp_last_stage(void);
bool nv_fsp_secure_boot_ready(nv_card_t *c, int ms);
bool nv_fsp_boot_gsp(nv_card_t *c, u64 fmc_phys, u64 boot_args_phys,
                     u64 rsvd_size,
                     const void *hash, size_t hash_len,
                     const void *key, size_t key_len,
                     const void *sig, size_t sig_len);

/* Reset the card through a secondary-bus reset on its upstream bridge, which is
 * the only reset that re-runs the on-chip boot sequence and so clears WPR2 (a
 * bare function-level reset does not).  The screen blanks: the framebuffer is
 * the card's own memory.  Returns true when the card answers configuration
 * reads again afterwards. */
bool nv_gpu_reset_wpr2(nv_card_t *c);

/* Bringing a card up, in the order it has to happen. */
bool nv_identify(nv_card_t *c);
bool nv_read_vbios(nv_card_t *c);
bool nv_parse_vbios(nv_card_t *c);
bool nv_read_vram_size(nv_card_t *c);
void nv_read_sensors(nv_card_t *c);
int  nv_probe_outputs(nv_card_t *c);

/* The I2C lines behind a connector, and the monitor identification a display
 * answers with. */
bool nv_i2c_read_edid(nv_card_t *c, u8 bus, u8 out[128]);

const char *nv_output_type_name(nv_output_type t);

/* The cards the driver found. */
nv_card_t *nv_card(int index);
int        nv_card_count(void);
nv_card_t *nv_model_card(void);

#endif
