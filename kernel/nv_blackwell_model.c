/* nv_blackwell_model.c - a GB203, which is the chip in a GeForce RTX 5070 Ti.
 *
 * nv_model.c stands in for a Pascal card, because Pascal is the newest
 * generation whose display engine still answers to registers - so the whole
 * driver runs against it rather than stopping at the firmware boundary.  That
 * makes it the right model for most of the driver and the wrong one for the
 * part this file exists for: everything Pascal does not have.
 *
 * So this is a second model, deliberately small.  It answers exactly the
 * registers a Blackwell card answers differently from a Pascal one, and
 * nothing else:
 *
 *   PMC_BOOT_0   - the identity, in the layout used since the architecture
 *                  field overflowed its original five bits at Turing.
 *   PMC_BOOT_42  - the same identity again, in the other layout, because the
 *                  point of reading both is that they agree.
 *   the memory partition registers - how many there are, which are fused off,
 *                  and how big each one is.
 *
 * The memory configuration is the interesting part and is chosen to be a trap.
 * A real 5070 Ti has sixteen gigabytes on a two-hundred-and-fifty-six bit bus,
 * which is eight partitions of two gigabytes.  This model reports NINE
 * partitions with the ninth fused off - the shape a die cut down for a cheaper
 * board actually has.  A driver that reads the count and the sizes but skips
 * the fuse mask sums eighteen gigabytes, allocates against memory that is not
 * connected to anything, and fails much later somewhere unrelated.  Getting
 * sixteen out of this model is the check.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: that the driver reads both identity registers and
 * cross-checks them, that it sums memory partition by partition, and that it
 * consults the fuse mask before believing a partition.
 *
 * What it cannot establish: that these are the offsets NVIDIA's silicon uses.
 * A model written from the same understanding as the driver checks that
 * understanding against itself.  It is worth having anyway - most driver bugs
 * are in the understanding being applied, not in the understanding itself -
 * but it is not the same as a card.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "klog.h"
#include "nv.h"

/* ------------------------------------------------------------- the identity
 *
 * Chip 0x1b3: architecture 0x1b, implementation 3.  Revision A0.
 *
 * BOOT_0 carries the chip number at bits 28:20 and the revision in its low
 * byte.  BOOT_42 splits the same chip number into architecture at 28:24 and
 * implementation at 23:20, and carries the revision as separate nibbles.
 */
#define MODEL_CHIPSET   0x1b3u
/* Revision A1.  NVIDIA writes a revision as one byte with the letter in the
 * high nibble as its own hexadecimal digit - A is 0xA - so an A1 part reads
 * back as 0xA1, which is exactly what Windows enumerates this card as
 * (PCI 10de:2c05 REV_A1).  Storing 1 for A would be a different part. */
#define MODEL_REV_MAJOR 0xAu
#define MODEL_REV_MINOR 1u

/* The numbers below are not invented.  They were read off the card in the
 * machine this was written on - an MSI GeForce RTX 5070 Ti, PCI 10de:2c05,
 * subsystem 1462:5314, silicon revision A1 - while Windows was running, which
 * is the one thing that can be checked against real Blackwell hardware without
 * booting this system on it.  What that establishes is worth being exact
 * about: it fixes what the driver should CONCLUDE, not where in the register
 * window the conclusion comes from.  The offsets are still unverified; the
 * answers they have to produce no longer are. */
#define MODEL_PCI_DEVICE    0x2C05u
#define MODEL_PCI_SUBSYSTEM 0x53141462u

#define MODEL_BOOT_0  ((MODEL_CHIPSET << 20) | \
                       (MODEL_REV_MAJOR << 4) | MODEL_REV_MINOR)
#define MODEL_BOOT_42 (((MODEL_CHIPSET >> 4) << 24) | \
                       ((MODEL_CHIPSET & 0xF) << 20) | \
                       (MODEL_REV_MAJOR << 4) | MODEL_REV_MINOR)

/* ----------------------------------------------------------------- the memory
 *
 * Nine partitions, the ninth fused off, two gigabytes on each of the eight
 * that are left.  Two thousand and forty-eight megabytes is what the partition
 * register actually holds - it counts in megabytes, not bytes.
 */
/* Six FBPs of two partitions each - twelve slots - and both kinds of fuse in
 * play, because a model that exercises only one of them cannot tell a driver
 * that consults one from a driver that consults both.
 *
 *   FBP 5 is switched off entirely, taking partitions 10 and 11 with it.
 *   Partitions 3 and 7 are switched off individually inside live FBPs.
 *
 * That leaves eight of twelve carrying two gigabytes each: sixteen, which is
 * what the card in this machine has.  The arrangement is chosen so that a
 * driver getting it wrong lands on a WRONG number rather than by luck on the
 * right one - treating the FBP count as the partition count reads six slots,
 * skips one, and reports ten gigabytes. */
#define MODEL_FBPS            6
#define MODEL_FBPAS_PER_FBP   2
#define MODEL_FBPA_SLOTS      (MODEL_FBPS * MODEL_FBPAS_PER_FBP)
#define MODEL_FBP_FUSED       (1u << 5)
#define MODEL_FUSED_MASK      ((1u << 3) | (1u << 7))
#define MODEL_FBPA_MB    2048u

/* The offsets, repeated here rather than shared with the driver.  A model that
 * imported the driver's constants would agree with it by construction and
 * check nothing about where things are; keeping the two copies separate means
 * a typo in either is caught.  They are the same numbers on purpose, and if
 * one is wrong against real silicon both are wrong together - which is what
 * the note at the top of the file says. */
#define M_PMC_BOOT_0        0x000000
#define M_PMC_BOOT_42       0x000A00
#define M_PTOP_NUM_FBPS     0x022438
#define M_PTOP_NUM_FBPAS    0x022458
#define M_FUSE_OPT_FBP      0x021D38
#define M_FUSE_OPT_FBIO     0x021C14
#define M_FBPA_CSTATUS(i)   (0x90020C + (i) * 0x4000)

/* ------------------------------------------------------------------ the ROM
 *
 * Enough of one to carry the version string, because the version string is the
 * one thing about this card that could be checked against the real one.  The
 * card in this machine reports 98.03.58.00.9d, and the last of those five
 * bytes is the part a driver that stops at four never prints - which is what
 * this exists to catch.
 *
 * The bytes go into the ROM window, in the layout an option ROM has: the two
 * signature bytes, its length, then NVIDIA's own table of tables, found by
 * searching for its signature rather than at any fixed place.
 */
#define M_PROM              0x300000        /* the ROM, in the window        */
#define M_ROM_BIT_AT        0x200
#define M_ROM_VER_AT        0x300
#define M_ROM_BYTES         0x1000

/* 98.03.58.00, low byte first, and then the board's own revision. */
#define M_VER_0  0x00
#define M_VER_1  0x58
#define M_VER_2  0x03
#define M_VER_3  0x98
#define M_VER_4  0x9d

/* The window has to reach the highest offset the driver touches, which is the
 * last partition's size register.  Backing exactly that much and no more means
 * a driver that reads past the end reads past the end here too, rather than
 * finding a convenient zero. */
#define WINDOW_BYTES  (M_FBPA_CSTATUS(MODEL_FBPA_SLOTS - 1) + 0x1000)

static u8 *window;
static void build_rom(void);

static void put32(u32 offset, u32 value) {
    if (offset + 4 > WINDOW_BYTES) return;
    *(volatile u32 *)(window + offset) = value;
}

static void put8(u32 offset, u8 value) {
    if (offset >= WINDOW_BYTES) return;
    window[offset] = value;
}

static void put16le(u32 offset, u16 value) {
    put8(offset, (u8)value);
    put8(offset + 1, (u8)(value >> 8));
}

static void build_rom(void) {
    /* The two bytes every option ROM starts with, and its length in units of
     * half a kilobyte. */
    put8(M_PROM + 0, 0x55);
    put8(M_PROM + 1, 0xAA);
    put8(M_PROM + 2, M_ROM_BYTES / 512);

    /* NVIDIA's tables: a 0xFF byte, then "BIT", then a header saying where the
     * entries begin, how long each is, and how many there are. */
    u32 bit = M_PROM + M_ROM_BIT_AT;
    put8(bit + 0, 0xFF);
    put8(bit + 1, 0xB8);                    /* the identifier, low byte first */
    put8(bit + 2, 'B');
    put8(bit + 3, 'I');
    put8(bit + 4, 'T');
    put8(bit + 5, 0x00);
    put8(bit + 6, 0);                       /* structure version, low        */
    put8(bit + 7, 0);                       /* structure version, high       */
    put8(bit + 8, 12);                      /* header length                 */
    put8(bit + 9, 6);                       /* how long each entry is        */
    put8(bit + 10, 1);                      /* how many                      */
    put8(bit + 11, 0);                      /* checksum                      */

    /* One entry: the version table, five bytes long.  The length is the whole
     * point - a four-byte entry is an older card and prints four fields. */
    u32 e = bit + 12;
    put8(e + 0, 'i');
    put8(e + 1, 2);                         /* table version                 */
    put16le(e + 2, 5);                      /* how many bytes it holds       */
    put16le(e + 4, M_ROM_VER_AT);           /* and where they are            */

    u32 v = M_PROM + M_ROM_VER_AT;
    put8(v + 0, M_VER_0);
    put8(v + 1, M_VER_1);
    put8(v + 2, M_VER_2);
    put8(v + 3, M_VER_3);
    put8(v + 4, M_VER_4);
}

/* What the card in this machine says it is, for the test to compare against. */
/* Upper case, because that is how the version is written everywhere a person
 * would compare it against: the ROM's own printable string, the manufacturer's
 * download page, and every tool that reads one. */
const char *nv_blackwell_model_vbios(void) { return "98.03.58.00.9D"; }
u32 nv_blackwell_model_pci_device(void) { return MODEL_PCI_DEVICE; }
u32 nv_blackwell_model_pci_subsystem(void) { return MODEL_PCI_SUBSYSTEM; }

/* ----------------------------------------------------------------- attaching */

bool nv_blackwell_model_attach(volatile u8 **regs, size_t *size) {
    if (!window) {
        window = kzalloc(WINDOW_BYTES);
        if (!window) return false;
    }
    memset(window, 0, WINDOW_BYTES);

    put32(M_PMC_BOOT_0, MODEL_BOOT_0);
    put32(M_PMC_BOOT_42, MODEL_BOOT_42);

    put32(M_PTOP_NUM_FBPS,  MODEL_FBPS);
    put32(M_PTOP_NUM_FBPAS, MODEL_FBPAS_PER_FBP);
    put32(M_FUSE_OPT_FBP,   MODEL_FBP_FUSED);
    put32(M_FUSE_OPT_FBIO,  MODEL_FUSED_MASK);

    /* Every partition answers with a size, including the ones that are fused
     * off.  That is what real silicon does - the register is in the partition,
     * not in the memory - and it is the whole reason the masks have to be
     * consulted rather than the sizes simply summed. */
    for (int i = 0; i < MODEL_FBPA_SLOTS; i++)
        put32(M_FBPA_CSTATUS(i), MODEL_FBPA_MB);

    build_rom();

    if (regs) *regs = (volatile u8 *)window;
    if (size) *size = WINDOW_BYTES;
    return true;
}

void nv_blackwell_model_detach(void) {
    /* The window is kept.  Freeing and reallocating it on every run would make
     * the test depend on the heap's behaviour, which is not what it is for. */
    if (window) memset(window, 0, WINDOW_BYTES);
}

/* What the model was built to be, for the test to compare against. */
/* Which partitions actually carry memory, worked out the way the driver has
 * to work it out: an FBPA counts only if its own fuse bit is clear AND the FBP
 * it belongs to is switched on. */
static bool model_fbpa_live(int i) {
    int fbp = i / MODEL_FBPAS_PER_FBP;
    if (MODEL_FBP_FUSED & (1u << fbp)) return false;
    if (MODEL_FUSED_MASK & (1u << i)) return false;
    return true;
}

u64 nv_blackwell_model_vram(void) {
    u64 live = 0;
    for (int i = 0; i < MODEL_FBPA_SLOTS; i++)
        if (model_fbpa_live(i)) live += (u64)MODEL_FBPA_MB << 20;
    return live;
}

int nv_blackwell_model_fbpas(int *live_out) {
    int live = 0;
    for (int i = 0; i < MODEL_FBPA_SLOTS; i++)
        if (model_fbpa_live(i)) live++;
    if (live_out) *live_out = live;
    return MODEL_FBPA_SLOTS;
}
