/* nv_blackwell.c - the RTX 50 series, and the four things that make it one.
 *
 * The rest of this driver is written to be generation-agnostic on purpose: the
 * card is asked what it is and the answer drives everything, so a chip that did
 * not exist when the code was written still lands in the right place.  That
 * carries a long way.  It does not carry all the way, and this file is the
 * places it stops.
 *
 * A GB203 - the chip in a GeForce RTX 5070 Ti and a 5080 - differs from an
 * AD102 in four ways that a driver has to know about by name:
 *
 *   1. Which chip it is.  PMC_BOOT_0 has held the identity since 1998, but
 *      from Turing onward NVIDIA's own driver reads PMC_BOOT_42 instead, and
 *      that register carries a finer revision than BOOT_0's low byte does.
 *      Reading both and comparing is worth doing: they are two views of the
 *      same fuses, and a card where they disagree is a card where something
 *      ahead of us - a hypervisor, a broken BAR decode - is lying.
 *
 *   2. How much memory is fitted.  The single register that answered that
 *      question from Fermi to Pascal answers for a memory controller that no
 *      longer exists in that shape.  A modern card has its memory in
 *      partitions, some of which are fused off in the factory to make a
 *      cheaper part out of a bigger die, and the size is the sum of the ones
 *      that are left.  This is also the only way to see the configurations
 *      where the partitions are not all the same size, which are real and
 *      which are the reason a card can have memory that is slower above a
 *      certain address.
 *
 *   3. Which class numbers its engines answer to.  A class is the contract
 *      between the driver and an engine: what method 0x0204 means depends
 *      entirely on it.  Every generation gets new numbers, and they are not
 *      guessable from the previous ones - so they are a table, and the table
 *      is here rather than spread over four files.
 *
 *   4. Which firmware it needs.  From Turing on the card is driven by a
 *      processor running an image NVIDIA signs, and the image is
 *      per-architecture.  Blackwell is not in the release the rest of this
 *      driver was written against, and telling a 5070 Ti owner to go and find
 *      a file that does not exist is worse than telling them nothing.
 *
 * ---------------------------------------------------------------------------
 * What this establishes and what it cannot.  Run against the model in
 * nv_blackwell_model.c, everything below is exercised end to end: a GB203 is
 * identified from both of its identity registers, sixteen gigabytes is summed
 * out of eight memory partitions with a ninth fused off, the class numbers
 * come out per generation, and the firmware the chip needs is named.
 *
 * It cannot establish that these are the offsets NVIDIA's silicon uses, since
 * the model was written from the same understanding as the driver.  Where a
 * number is one I would want checked first on real hardware, it says so at the
 * place it is used rather than in a note at the top that nobody reads.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "klog.h"
#include "nv.h"

/* ============================================================ 1. which chip
 *
 * The chip number is the architecture in the high five bits and the
 * implementation within it in the low four: 0x1b3 is architecture 0x1b -
 * Blackwell as it shipped in GeForce - implementation 3, which is GB203.
 *
 * What that implementation number does NOT tell you is the board.  One die
 * ships as several cards, cut down differently; a GB203 is a 5080 with
 * everything enabled and a 5070 Ti with some of it fused off, and nothing in
 * the register window distinguishes them.  So the table below names the die
 * and lists the boards it ships as, which is honest, rather than naming one
 * board, which would be a coin flip.
 *
 * This is the one table in the driver that goes out of date, and it is
 * deliberately not load-bearing: everything else works from the architecture
 * alone, and a chip missing from here is described by its number and its
 * generation and nothing is lost but the name.
 */
typedef struct { u32 chip; const char *die; const char *boards; } chip_t;

static const chip_t chips[] = {
    /* Turing. */
    { 0x162, "TU102", "RTX 2080 Ti, Titan RTX"              },
    { 0x164, "TU104", "RTX 2080, 2080 Super"                },
    { 0x166, "TU106", "RTX 2060, 2070"                      },
    { 0x167, "TU117", "GTX 1650"                            },
    { 0x168, "TU116", "GTX 1660, 1660 Ti"                   },

    /* Ampere. */
    { 0x170, "GA100", "A100"                                },
    { 0x172, "GA102", "RTX 3080, 3080 Ti, 3090, 3090 Ti"    },
    { 0x173, "GA103", "RTX 3080 Ti laptop"                  },
    { 0x174, "GA104", "RTX 3060 Ti, 3070, 3070 Ti"          },
    { 0x176, "GA106", "RTX 3050, 3060"                      },
    { 0x177, "GA107", "RTX 3050 laptop"                     },

    /* Hopper - datacentre only, no display engine at all. */
    { 0x180, "GH100", "H100, H200"                          },

    /* Ada Lovelace. */
    { 0x192, "AD102", "RTX 4090"                            },
    { 0x193, "AD103", "RTX 4070 Ti Super, 4080, 4080 Super" },
    { 0x194, "AD104", "RTX 4070, 4070 Super, 4070 Ti"       },
    { 0x196, "AD106", "RTX 4060 Ti"                         },
    { 0x197, "AD107", "RTX 4060"                            },

    /* Blackwell as it shipped for compute. */
    { 0x1a0, "GB100", "B100, B200"                          },
    { 0x1a2, "GB102", "B200 second die"                     },

    /* Blackwell as it shipped in GeForce - the RTX 50 series. */
    { 0x1b2, "GB202", "RTX 5090, RTX PRO 6000"              },
    { 0x1b3, "GB203", "RTX 5080, RTX 5070 Ti"               },
    { 0x1b5, "GB205", "RTX 5070"                            },
    { 0x1b6, "GB206", "RTX 5060, RTX 5060 Ti"               },
    { 0x1b7, "GB207", "RTX 5050"                            },
};

const char *nv_chip_die(u32 chipset) {
    for (size_t i = 0; i < ARRAY_LEN(chips); i++)
        if (chips[i].chip == chipset) return chips[i].die;
    return NULL;
}

const char *nv_chip_boards(u32 chipset) {
    for (size_t i = 0; i < ARRAY_LEN(chips); i++)
        if (chips[i].chip == chipset) return chips[i].boards;
    return NULL;
}

/* ---------------------------------------------- the second identity register
 *
 * PMC_BOOT_42 is what NVIDIA's own driver reads from Turing onward.  It is the
 * same fuses BOOT_0 reports, laid out differently and with more of them:
 *
 *   28:24  architecture      0x1b for the Blackwell that shipped in GeForce
 *   23:20  implementation    3 for GB203
 *   11:8   extended revision
 *    7:4   major revision    the letter, A or B
 *    3:0   minor revision    the digit
 *
 * So the chip number is the top two fields put together, which is the same
 * nine bits BOOT_0 carries at 28:20, and the two should agree.  When they do,
 * the finer revision from here is used, because "A1" from BOOT_0's low byte
 * and "A1 extended 0" from here are the same answer at different resolution.
 *
 * When they do not agree, both are printed and BOOT_0 is kept.  Preferring one
 * silently would hide the only case this check exists for.
 */
#define NV_PMC_BOOT_42  0x000A00

/* The letter in a revision.  It is stored as its own hexadecimal digit - A is
 * 0xA, B is 0xB - so an A1 part reads back as the byte 0xA1 and 'A' plus the
 * nibble would give 'K'.  Anything outside the letters is printed as a digit
 * rather than as whatever character the arithmetic lands on. */
static char revision_letter(u8 major) {
    return (major >= 0xA && major <= 0xF) ? (char)('A' + major - 0xA)
                                          : (char)('0' + (major & 0xF));
}

bool nv_read_boot42(nv_card_t *c) {
    /* Only meaningful from Turing; on older parts the offset is not this
     * register and reading it proves nothing. */
    if ((c->chipset & 0x1F0) < 0x160) return false;

    u32 b42 = nv_rd32(c, NV_PMC_BOOT_42);
    if (b42 == 0xFFFFFFFFu || b42 == 0) return false;

    c->boot42 = b42;

    u32 arch = (b42 >> 24) & 0x1F;
    u32 impl = (b42 >> 20) & 0x0F;
    u32 chip = (arch << 4) | impl;

    c->revision_major    = (u8)((b42 >> 4) & 0xF);
    c->revision_minor    = (u8)(b42 & 0xF);
    c->revision_extended = (u8)((b42 >> 8) & 0xF);

    if (chip != c->chipset) {
        kwarn("nvidia", "the two identity registers disagree: BOOT_0 says chip "
                        "%03x and BOOT_42 says %03x; keeping BOOT_0",
              c->chipset, chip);
        return false;
    }

    c->boot42_agrees = true;
    return true;
}

/* ======================================================= 2. how much memory
 *
 * A card's memory is not one block.  It is divided among partitions - NVIDIA
 * calls each one an FBPA - and each partition owns a slice of the memory bus
 * and the chips on it.  A GB203 has eight; a GB202 has sixteen, which is what
 * a five-hundred-and-twelve-bit bus means.
 *
 * Two things follow that a single size register cannot express:
 *
 *   Partitions are fused off.  The same die sold as a cheaper card has some of
 *   them permanently disabled, and a disabled one still answers its size
 *   register with the size of memory that is not connected to it.  So the fuse
 *   mask has to be read and consulted, and a driver that skips it reports a
 *   card as having more memory than it has and then writes off the end of it.
 *
 *   Partitions are not all the same size.  When a card has a memory size that
 *   is not the bus width times a power of two - the GTX 970's three and a half
 *   usable gigabytes, and several cards since - it is because the partitions
 *   are uneven.  Summing them gets the total right; noticing they are uneven
 *   is what tells you the top of memory is slower than the bottom, which is
 *   something an allocator wants to know.
 *
 * The three registers.  These are the ones I would check first against real
 * silicon, because they are the part of this file least likely to have
 * survived unchanged from the generation they were documented for:
 */
/* Checked against nouveau, which reads these directly rather than asking
 * firmware (nvkm/subdev/fb: ramgf100.c, ramgm107.c, ramgm200.c, ramgp100.c).
 *
 * The correction that mattered: 0x022438 is the number of FBPs, and an FBP
 * contains SEVERAL FBPAs - 0x022458 says how many.  The total number of
 * partitions is the product, and this used to treat the first register as the
 * partition count and stop there.  On a card with two FBPAs to an FBP that
 * reads half of them and reports half the memory, which on the card in this
 * machine is eight gigabytes instead of sixteen.
 *
 * There are two fuse masks for the same reason, and both have to be consulted:
 * a whole FBP can be switched off at 0x021D38, and an individual FBPA within a
 * live FBP at 0x021C14 (nouveau calls the latter `fbpao`). */
#define NV_PTOP_SCAL_NUM_FBPS      0x022438  /* how many FBPs exist            */
#define NV_PTOP_SCAL_NUM_FBPAS     0x022458  /* how many FBPAs are in each FBP */
#define NV_FUSE_STATUS_OPT_FBP     0x021D38  /* a set bit: this FBP is off     */
#define NV_FUSE_STATUS_OPT_FBIO    0x021C14  /* a set bit: this FBPA is off    */
#define NV_PFB_FBPA_CSTATUS_BASE   0x90020C  /* partition 0's size, in MiB     */
#define NV_PFB_FBPA_CSTATUS_STRIDE 0x004000

#define NV_MAX_FBPAS 32

bool nv_read_vram_fbpa(nv_card_t *c) {
    /* Pascal brought this arrangement in; everything since has kept it. */
    if ((c->chipset & 0x1F0) < 0x130) return false;

    u32 fbps       = nv_rd32(c, NV_PTOP_SCAL_NUM_FBPS);
    u32 per_fbp    = nv_rd32(c, NV_PTOP_SCAL_NUM_FBPAS);
    u32 fbp_fused  = nv_rd32(c, NV_FUSE_STATUS_OPT_FBP);
    u32 fbpa_fused = nv_rd32(c, NV_FUSE_STATUS_OPT_FBIO);

    /* A window that is not really mapped reads all ones, and all ones is also
     * a plausible-looking count.  Anything outside what a card could have is
     * treated as no answer rather than as an answer. */
    if (!fbps || fbps > NV_MAX_FBPAS) return false;
    if (!per_fbp || per_fbp > 8) return false;
    if (fbps * per_fbp > NV_MAX_FBPAS) return false;
    if (fbpa_fused == 0xFFFFFFFFu || fbp_fused == 0xFFFFFFFFu) return false;

    u32 count = fbps * per_fbp;                   /* partitions, all told     */

    u64 total = 0;
    u64 smallest = ~0ULL;
    u64 largest = 0;
    int live = 0;

    for (u32 fbp = 0; fbp < fbps; fbp++) {
        if (fbp_fused & (1u << fbp)) continue;    /* the whole FBP is off     */

        for (u32 k = 0; k < per_fbp; k++) {
            u32 i = fbp * per_fbp + k;
            if (fbpa_fused & (1u << i)) continue; /* this partition is off    */

            u32 mb = nv_rd32(c, NV_PFB_FBPA_CSTATUS_BASE +
                                i * NV_PFB_FBPA_CSTATUS_STRIDE);
            if (!mb || mb > (64u << 10)) continue;   /* nothing sensible there */

            u64 bytes = (u64)mb << 20;
            total += bytes;
            if (bytes < smallest) smallest = bytes;
            if (bytes > largest) largest = bytes;
            live++;
        }
    }

    if (!live || total < (16ULL << 20) || total > (256ULL << 30)) return false;

    c->vram_bytes  = total;
    c->vram_exact  = true;
    c->fbpa_total  = (int)count;
    c->fbpa_live   = live;
    c->fbpa_uneven = (smallest != largest);

    kinfo("nvidia", "%llu MiB across %d of %u memory partition%s%s",
          (unsigned long long)(total >> 20), live, count,
          count == 1 ? "" : "s",
          c->fbpa_uneven ? " - and they are not all the same size, so the top "
                           "of memory is not as fast as the bottom" : "");

    if (live < (int)count)
        kinfo("nvidia", "%u partition(s) fused off in the factory; this die "
                        "also ships with them enabled, as a larger card",
              count - (u32)live);

    return true;
}

/* ===================================================== 3. which class numbers
 *
 * A class number is what an engine is told it is being talked to as.  It is
 * sent once, when a subchannel is bound, and from then on every method number
 * in that subchannel means whatever that class says it means.  Send the wrong
 * one and the card does not misbehave: it refuses the object outright, which
 * is the good failure - it is loud, immediate, and names the number it did not
 * recognise.
 *
 * The numbers themselves are NVIDIA's, published in the class headers that
 * ship with their open kernel modules.  There is no way to derive them; a
 * driver either knows them or does not.  Blackwell has two sets, because
 * Blackwell is two chips: the one that went to datacentres and the one that
 * went into GeForce, and they are different silicon with different class
 * numbers, not two bins of the same part.
 */
static const nv_classes_t class_table[] = {
    /* family  3D      compute copy    gpfifo  core    window  name          */
    /* The two display columns are the MODERN display engine - the one Volta
     * introduced, where a head owns the timing and a window owns the surface.
     * Everything older is a different engine with a different shape, driven by
     * nv_disp.c, and it has class numbers of its own.  Putting those numbers
     * here would make this column mean two different things depending on the
     * row, and a caller asking "which modern display class" would get an
     * answer for an engine that is not the one it is about to drive.  So they
     * are zero, and zero means "not this kind of engine" throughout. */
    { 0x0c0, 0x9097, 0x90C0, 0x90B5, 0x906F, 0,      0,      "Fermi"        },
    { 0x0d0, 0x9097, 0x90C0, 0x90B5, 0x906F, 0,      0,      "Fermi"        },
    { 0x0e0, 0xA097, 0xA0C0, 0xA0B5, 0xA06F, 0,      0,      "Kepler"       },
    { 0x0f0, 0xA197, 0xA1C0, 0xA0B5, 0xA06F, 0,      0,      "Kepler"       },
    { 0x100, 0xA297, 0xA1C0, 0xA0B5, 0xA06F, 0,      0,      "Kepler"       },
    { 0x110, 0xB097, 0xB0C0, 0xB0B5, 0xB06F, 0,      0,      "Maxwell"      },
    { 0x120, 0xB197, 0xB1C0, 0xB0B5, 0xB06F, 0,      0,      "Maxwell"      },
    { 0x130, 0xC097, 0xC0C0, 0xC0B5, 0xC06F, 0,      0,      "Pascal"       },
    { 0x140, 0xC397, 0xC3C0, 0xC3B5, 0xC36F, 0xC37D, 0xC37E, "Volta"        },
    { 0x160, 0xC597, 0xC5C0, 0xC5B5, 0xC46F, 0xC57D, 0xC57E, "Turing"       },
    { 0x170, 0xC697, 0xC6C0, 0xC6B5, 0xC56F, 0xC67D, 0xC67E, "Ampere"       },
    /* Hopper has no display engine.  It is not that this driver cannot drive
     * it; there is nothing there to drive - the die has no connectors. */
    { 0x180, 0xCB97, 0xCBC0, 0xC8B5, 0xC86F, 0,      0,      "Hopper"       },
    { 0x190, 0xC997, 0xC9C0, 0xC7B5, 0xC56F, 0xC77D, 0xC77E, "Ada Lovelace" },
    { 0x1a0, 0xCD97, 0xCDC0, 0xC9B5, 0xC96F, 0,      0,
                                            "Blackwell (datacentre)"        },
    { 0x1b0, 0xCE97, 0xCEC0, 0xCAB5, 0xCA6F, 0xCA7D, 0xCA7E,
                                            "Blackwell (GeForce)"           },
};

const nv_classes_t *nv_classes_for(u32 chipset) {
    /* These entries describe the nine-bit chip IDs supported by this table.
     * Do not truncate a wider identity into an unrelated known generation. */
    if (chipset > 0x1FF) return NULL;
    u32 family = chipset & 0x1F0;

    for (size_t i = 0; i < ARRAY_LEN(class_table); i++)
        if (class_table[i].family == family) return &class_table[i];

    /* Unknown generations need their own verified command classes.  Being
     * newer does not establish compatibility with the last table entry. */
    return NULL;
}

/* ================================================= 4. which firmware it needs
 *
 * From Turing on, the engines are behind a processor that will not run
 * anything NVIDIA has not signed, and the signed image is per-architecture.
 * Which release it comes from matters as much as which directory: a release
 * predating the chip does not contain an image for it, and no amount of
 * looking in the right directory finds one.
 *
 * Blackwell is the case in point.  The 535 release the Turing-through-Ada
 * files come from shipped in 2023 and knows nothing about GB20x; the first
 * that does is the 570 series.  A driver that names 535 for a 5070 Ti sends
 * its owner looking for a file that was never made.
 */
typedef struct {
    u32         family;
    const char *directory;      /* named after the first chip of the family  */
    const char *release;        /* the earliest release that knows the chip  */
} gsp_arch_t;

static const gsp_arch_t gsp_archs[] = {
    { 0x160, "tu102", "535.113.01" },
    { 0x170, "ga102", "535.113.01" },
    { 0x180, "gh100", "535.113.01" },
    { 0x190, "ad102", "535.113.01" },
    { 0x1a0, "gb100", "570.144"    },
    { 0x1b0, "gb202", "570.144"    },   /* GB202/GB203/GB205.  Use the MATCHED
                                         * 570.144 fmc+bootloader+RM: it is the
                                         * one stack whose RM actually runs and
                                         * consumes the command ring on the GB203
                                         * 5070 Ti.  The 595.99.02 RM was tried
                                         * (it has explicit GB20x signatures) but
                                         * with the 570-era fmc/bootloader - the
                                         * only loaders in the .run - it is an ABI
                                         * mismatch and halts SILENTLY before
                                         * reading the ring (worse than 570).  The
                                         * "570.144 NOCAT loop" seen earlier was
                                         * the empty-ring boot: normal NOCAT
                                         * telemetry plus a missing INIT_DONE
                                         * because the init RPCs were not
                                         * pre-queued - now fixed in nv_gsp.c. */
};

const char *nv_gsp_directory(u32 chipset) {
    u32 family = chipset & 0x1F0;
    for (size_t i = 0; i < ARRAY_LEN(gsp_archs); i++)
        if (gsp_archs[i].family == family) return gsp_archs[i].directory;
    return NULL;
}

const char *nv_gsp_release(u32 chipset) {
    u32 family = chipset & 0x1F0;
    for (size_t i = 0; i < ARRAY_LEN(gsp_archs); i++)
        if (gsp_archs[i].family == family) return gsp_archs[i].release;
    /* Newer than anything here.  The newest release named is the only honest
     * guess, and it is a lower bound rather than an answer. */
    return gsp_archs[ARRAY_LEN(gsp_archs) - 1].release;
}

/* -------------------------------------------------------------- the summary
 *
 * Called at the end of bring-up, and written to be worth reading on a machine
 * with no serial cable and no second computer watching: what the chip is, what
 * boards it ships as, how much memory across how many partitions, and what it
 * would need before an engine could be started.
 */
void nv_report_modern(nv_card_t *c) {
    if ((c->chipset & 0x1F0) < 0x160) return;

    const char *die = nv_chip_die(c->chipset);
    const char *boards = nv_chip_boards(c->chipset);

    if (die && boards)
        kinfo("nvidia", "this is a %s - the chip in a %s", die, boards);
    else
        kinfo("nvidia", "chip %03x has no board name in this driver's table",
              c->chipset);

    if (c->boot42_agrees)
        kinfo("nvidia", "both identity registers agree, revision %c%u%s",
              revision_letter(c->revision_major), c->revision_minor,
              c->revision_extended ? " (extended)" : "");

    const nv_classes_t *cl = nv_classes_for(c->chipset);
    if (cl) {
        kinfo("nvidia", "%s engines: draws with class %04x, computes with "
                        "%04x, copies with %04x, on channels of class %04x",
              cl->name, cl->three_d, cl->compute, cl->copy, cl->gpfifo);
        if (cl->disp_core)
            kinfo("nvidia", "and displays through class %04x, windows %04x",
                  cl->disp_core, cl->disp_window);
        else
            kinfo("nvidia", "and has no display engine; this die has no "
                            "connectors on it");
    } else {
        kwarn("nvidia", "chip %03x has no known command-class mapping; "
                        "not substituting another generation", c->chipset);
    }
}

/* -------------------------------------------------------------------- tests
 *
 * Driven against the model in nv_blackwell_model.c, which presents a GB203 -
 * a 5070 Ti - with sixteen gigabytes across eight of nine partitions.
 */
bool nv_blackwell_model_attach(volatile u8 **regs, size_t *size);
void nv_blackwell_model_detach(void);
u64  nv_blackwell_model_vram(void);
int  nv_blackwell_model_fbpas(int *live);
const char *nv_blackwell_model_vbios(void);

int nv_blackwell_selftest(void) {
    int failures = 0;

    nv_card_t card;
    memset(&card, 0, sizeof card);

    /* The card below is deliberately NOT marked as a model: it stands in for
     * silicon, so the read-only guard applies to it exactly as it would to a
     * real one. */
    nv_allow_writes(false);

    volatile u8 *regs = NULL;
    size_t size = 0;
    if (!nv_blackwell_model_attach(&regs, &size) || !regs) {
        kerr("nv-blackwell", "the model would not attach");
        return 1;
    }
    card.regs = regs;
    card.regs_size = size;

    /* 1. Identification, from the register that has meant the same thing since
     *    1998 - and this is the case it was extended for. */
    if (!nv_identify(&card)) {
        kerr("nv-blackwell", "the card would not identify itself");
        failures++;
    } else if (card.chipset != 0x1b3) {
        kerr("nv-blackwell", "the chip came out as %03x, expected 1b3 (GB203)",
             card.chipset);
        failures++;
    } else if (strcmp(card.architecture, "Blackwell")) {
        kerr("nv-blackwell", "the architecture came out as \"%s\"",
             card.architecture);
        failures++;
    } else {
        kinfo("nv-blackwell", "identified as chip %03x, %s %s",
              card.chipset, card.architecture, card.codename);
    }

    /* 2. And the same identity out of the other register, which is the one
     *    NVIDIA's driver reads.  Two views of the same fuses agreeing is the
     *    whole value of reading both. */
    if (!nv_read_boot42(&card) || !card.boot42_agrees) {
        kerr("nv-blackwell", "the second identity register did not confirm the "
                             "first");
        failures++;
    } else if (card.revision_major != 0xA || card.revision_minor != 1) {
        /* A1, which is the revision the card in this machine reports - Windows
         * enumerates it as PCI 10de:2c05 REV_A1.  The revision is not
         * decoration: it selects errata, and a driver that reads it out of the
         * wrong nibble gets a number that looks plausible for every part ever
         * made. */
        kerr("nv-blackwell", "the revision came out as %x.%x, expected a.1 (A1)",
             card.revision_major, card.revision_minor);
        failures++;
    } else {
        kinfo("nv-blackwell", "both identity registers agree, revision %c%u - "
                              "which is what the card in this machine is",
              revision_letter(card.revision_major), card.revision_minor);
    }

    /* 3. The name of the die and the boards it ships as. */
    const char *die = nv_chip_die(card.chipset);
    const char *boards = nv_chip_boards(card.chipset);
    if (!die || strcmp(die, "GB203") ||
        !boards || strcmp(boards, "RTX 5080, RTX 5070 Ti")) {
        kerr("nv-blackwell", "the die was not recognised as a GB203");
        failures++;
    } else {
        kinfo("nv-blackwell", "%s, which is the chip in a %s", die, boards);
    }

    /* 4. Memory, summed out of the partitions rather than read from a single
     *    register that no longer exists.  This is the check that catches a
     *    driver ignoring the fuse mask: the model has nine partitions with one
     *    fused off, so ignoring the mask gives eighteen gigabytes and the
     *    right answer is sixteen. */
    if (!nv_read_vram_fbpa(&card)) {
        kerr("nv-blackwell", "the memory partitions could not be read");
        failures++;
    } else if (card.vram_bytes != nv_blackwell_model_vram()) {
        kerr("nv-blackwell", "memory came out as %llu MiB, expected %llu MiB",
             (unsigned long long)(card.vram_bytes >> 20),
             (unsigned long long)(nv_blackwell_model_vram() >> 20));
        failures++;
    } else {
        int live = 0;
        int total = nv_blackwell_model_fbpas(&live);
        if (card.fbpa_total != total || card.fbpa_live != live) {
            kerr("nv-blackwell", "%d of %d partitions counted, expected %d of %d",
                 card.fbpa_live, card.fbpa_total, live, total);
            failures++;
        } else {
            kinfo("nv-blackwell", "%llu GiB summed from %d live partitions of "
                                  "%d, with the fused one skipped",
                  (unsigned long long)(card.vram_bytes >> 30),
                  card.fbpa_live, card.fbpa_total);
        }
    }

    /* 5. The class numbers, per generation.  The check that matters here is
     *    that the two Blackwells are not the same: GB100 went to datacentres
     *    and GB20x went into GeForce, and they are different silicon. */
    const nv_classes_t *geforce = nv_classes_for(0x1b3);
    const nv_classes_t *compute = nv_classes_for(0x1a0);
    const nv_classes_t *ada     = nv_classes_for(0x192);

    if (!geforce || !compute || !ada) {
        kerr("nv-blackwell", "a generation has no class numbers at all");
        failures++;
    } else if (geforce->three_d == compute->three_d) {
        kerr("nv-blackwell", "both Blackwells were given the same drawing class");
        failures++;
    } else if (geforce->three_d != 0xCE97 || geforce->disp_core != 0xCA7D) {
        kerr("nv-blackwell", "the GeForce Blackwell classes came out as %04x "
                             "and %04x", geforce->three_d, geforce->disp_core);
        failures++;
    } else if (compute->disp_core != 0) {
        kerr("nv-blackwell", "a die with no connectors was given a display class");
        failures++;
    } else if (ada->three_d != 0xC997) {
        kerr("nv-blackwell", "Ada's drawing class came out as %04x", ada->three_d);
        failures++;
    } else {
        kinfo("nv-blackwell", "class numbers are per generation: %s draws with "
                              "%04x and displays with %04x, %s draws with %04x "
                              "and has no display engine",
              geforce->name, geforce->three_d, geforce->disp_core,
              compute->name, compute->three_d);
    }

    /* 6. And what it would need before an engine could be started.  The point
     *    of the check is the release, not the directory: naming a release that
     *    predates the chip sends someone looking for a file nobody made. */
    const char *dir = nv_gsp_directory(0x1b3);
    const char *rel = nv_gsp_release(0x1b3);
    const char *ada_rel = nv_gsp_release(0x192);
    if (!dir || strcmp(dir, "gb202")) {
        kerr("nv-blackwell", "the firmware directory came out as \"%s\"",
             dir ? dir : "(none)");
        failures++;
    } else if (!rel || !strcmp(rel, ada_rel)) {
        kerr("nv-blackwell", "Blackwell was given the same firmware release as "
                             "Ada, which does not contain an image for it");
        failures++;
    } else {
        kinfo("nv-blackwell", "would need nvidia/%s/gsp/gsp-%s.bin - a later "
                              "release than Ada's %s, because 535 shipped "
                              "before this chip existed", dir, rel, ada_rel);
    }

    /* 7. And the card's own description of itself, out of its ROM.
     *
     * This is the one check here that is anchored to a real card rather than
     * to an understanding of one.  The version string the model carries -
     * 98.03.58.00.9d - was read off the RTX 5070 Ti in this machine while
     * Windows was running, which is the only Blackwell fact available without
     * booting on it.  The last of those five bytes is the board maker's own
     * revision, and a driver that stops at four prints something that looks
     * right, matches nothing anyone can search for, and cannot tell two boards
     * built on the same chip apart. */
    /* Reading the ROM out of the chip means switching it into the register
     * window, which is a write - and writes are exactly what the driver
     * refuses by default on real silicon.  So this is the one part of the test
     * that asks for them, the way somebody would on a second boot once the
     * first had shown the card identified correctly.  Doing it here rather
     * than leaving it on for the whole test keeps the check below honest: it
     * has to observe writes being refused. */
    nv_allow_writes(true);
    bool rom_read = nv_read_vbios(&card);
    bool rom_parsed = rom_read && nv_parse_vbios(&card);
    nv_allow_writes(false);

    if (!rom_read) {
        kerr("nv-blackwell", "the card's ROM could not be read");
        failures++;
    } else if (!rom_parsed && !card.vbios_version[0]) {
        kerr("nv-blackwell", "the ROM was read but nothing was understood in it");
        failures++;
    } else if (strcmp(card.vbios_version, nv_blackwell_model_vbios())) {
        kerr("nv-blackwell", "the firmware version came out as \"%s\", and the "
                             "card in this machine reports \"%s\"",
             card.vbios_version, nv_blackwell_model_vbios());
        failures++;
    } else {
        kinfo("nv-blackwell", "firmware version %s, all five fields - which is "
                              "what the card in this machine reports",
              card.vbios_version);
    }

    /* 8. And the guard that stands between this driver and somebody's screen.
     *
     * On a real card the driver is read-only until told otherwise, and the
     * whole value of that depends on there being no way round it.  So it is
     * checked the only way worth checking: by trying.  The card here is not
     * marked as a model, so it is treated exactly as silicon would be - a
     * write is aimed at a scratch offset, and the test is that the offset
     * still holds what it held.
     *
     * A choke point that is merely believed in is not a guarantee.  This is
     * the difference between "the code takes the safe path" and "there is no
     * unsafe path to take". */
    {
        const u32 scratch = 0x001700;          /* the instance-window register */
        u32 before = nv_rd32(&card, scratch);
        u32 refused_before = nv_writes_refused();

        nv_allow_writes(false);
        nv_wr32(&card, scratch, ~before);
        u32 after = nv_rd32(&card, scratch);

        if (after != before) {
            kerr("nv-blackwell", "a write reached a card the driver was told "
                                 "not to write to: %08x became %08x",
                 before, after);
            failures++;
        } else if (nv_writes_refused() == refused_before) {
            kerr("nv-blackwell", "the write was dropped but not counted, so "
                                 "nothing would ever report it");
            failures++;
        } else {
            /* And that it is a guard rather than a wall: with writes allowed
             * the same write goes through, or the driver could never do
             * anything on a card at all. */
            nv_allow_writes(true);
            nv_wr32(&card, scratch, ~before);
            u32 allowed = nv_rd32(&card, scratch);
            nv_allow_writes(false);
            nv_wr32(&card, scratch, before);
            (void)nv_rd32(&card, scratch);

            if (allowed != (u32)~before) {
                kerr("nv-blackwell", "with writes allowed the write still did "
                                     "not land: %08x", allowed);
                failures++;
            } else {
                kinfo("nv-blackwell", "reads are free and writes are refused "
                                      "until asked for: %u refused so far, and "
                                      "the same write lands once allowed",
                      nv_writes_refused());
            }
        }
        /* Put the real card back to where it was found, whichever way the
         * checks went, and leave the driver read-only. */
        nv_allow_writes(true);
        nv_wr32(&card, scratch, before);
        nv_allow_writes(false);
    }

    /* 9. And which processor actually runs the firmware.  "The GSP" is a job,
     *    not a design: Turing put a Falcon there and everything since Ampere
     *    put a RISC-V core there instead.  The Falcon registers still answer
     *    on the newer cards, so a driver that assumes Falcon runs its whole
     *    boot sequence without a single step failing and starts nothing. */
    if (strcmp(nv_gsp_core_name(0x162), "a Falcon")) {
        kerr("nv-blackwell", "Turing's co-processor came out as \"%s\"",
             nv_gsp_core_name(0x162));
        failures++;
    } else if (strcmp(nv_gsp_core_name(0x1b3), "a RISC-V core")) {
        kerr("nv-blackwell", "Blackwell's co-processor came out as \"%s\"",
             nv_gsp_core_name(0x1b3));
        failures++;
    } else if (strcmp(nv_gsp_core_name(0x134), "nothing")) {
        kerr("nv-blackwell", "Pascal was given a co-processor it does not have");
        failures++;
    } else {
        kinfo("nv-blackwell", "the firmware runs on %s here and on %s on "
                              "Turing, so the Falcon boot sequence is not the "
                              "one this card needs",
              nv_gsp_core_name(0x1b3), nv_gsp_core_name(0x162));
    }

    /* And the summary, run for real, so the path a photograph would show is a
     * path that has been executed. */
    nv_report_modern(&card);

    nv_blackwell_model_detach();

    if (!failures)
        kinfo("nv-blackwell", "an RTX 5070 Ti is identified, its memory summed "
                              "partition by partition, its engine classes "
                              "chosen and its firmware named");
    return failures;
}
