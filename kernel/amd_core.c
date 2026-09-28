/* amd_core.c - finding out what an AMD card is, and where its parts are.
 *
 * There are two eras of AMD graphics hardware and a driver has to handle both.
 *
 * Up to Vega, a card is what its PCI identifier says it is, and every block of
 * registers is at an offset a driver can be born knowing.  A table of device
 * identifiers works, until the day a card ships that is not in it.
 *
 * From Navi onward that stops being true, and AMD's answer is better than a
 * bigger table: the card carries a description of itself in the last sixty-four
 * kilobytes of its own memory.  Which blocks it has.  Which version each one
 * is.  Where each one sits.  A driver reads it and then knows the layout of a
 * chip nobody had built when the driver was written.
 *
 * That is what makes this driver's answer to "will it work on the next card"
 * something other than a guess.  It reads the table.  The identifier table
 * below is a courtesy - it turns a number into a name a person recognises -
 * and nothing depends on a card being in it.
 *
 * ---------------------------------------------------------------------------
 * What this establishes and what it does not.  Every register number and every
 * table layout here came from AMD's own published headers, cited at the point
 * of use in amd.h.  Nothing available to run on has an AMD card in it, so the
 * code is exercised against a model built to the same map (amd_model.c).  That
 * proves the parsing, the ordering and the error handling.  It cannot prove
 * that the numbers match silicon, because model and driver were written from
 * the same understanding.  That distinction is repeated wherever a result from
 * this driver is reported.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "gpu.h"
#include "amd.h"

/* -------------------------------------------------------------- the window */

u32 amd_rd32(amd_card_t *c, u32 offset) {
    if (!c->regs) return 0xFFFFFFFFu;
    if (offset + 4 > c->regs_size) return amd_rd32_indirect(c, offset);
    if (c->modelled) { amd_model_sync(); amd_model_read(offset); }
    return *(volatile u32 *)(c->regs + offset);
}

/* The same read-only default the NVIDIA side has, and for the same reason:
 * this driver has been run against a model of these cards and never against
 * one, reading cannot break anything, writing can break the screen, and on a
 * machine with no serial cable a broken screen is the end of the evidence.
 *
 * On AMD there is one extra thing writes would reach that is worth naming.
 * Everything past the end of the register window goes through a shared pair of
 * registers - an address and then a value - and a write that lands in the
 * middle of somebody else's pair does not just fail, it sends the value to
 * whatever address that other user had just set up.  The firmware is still
 * running its own display code on this card.
 *
 * "gpuwrite" on the kernel command line turns writes on, for both vendors.
 */
static bool amd_writes_allowed;
static u32  amd_writes_refused;

void amd_allow_writes(bool yes) { amd_writes_allowed = yes; }
bool amd_writes_ok(void) { return amd_writes_allowed; }
u32  amd_writes_dropped(void) { return amd_writes_refused; }

void amd_wr32(amd_card_t *c, u32 offset, u32 value) {
    if (!c->regs) return;

    /* Checked before the split, so the indirect path cannot be a way round
     * it - that path is two writes, and it is the more dangerous of the two. */
    if (!c->modelled && !amd_writes_allowed) { amd_writes_refused++; return; }

    if (offset + 4 > c->regs_size) { amd_wr32_indirect(c, offset, value); return; }
    *(volatile u32 *)(c->regs + offset) = value;
    if (c->modelled) { amd_model_sync(); amd_model_wrote(offset, value); }
}

/* Past the end of the bar.  Two transactions instead of one, and the pair is
 * shared - so on a real system this needs a lock and a driver that forgets it
 * gets a register read that belongs to somebody else. */
u32 amd_rd32_indirect(amd_card_t *c, u32 offset) {
    if (!c->regs || c->regs_size < 8) return 0xFFFFFFFFu;
    amd_wr32(c, AMD_MM_INDEX, offset);
    return amd_rd32(c, AMD_MM_DATA);
}

void amd_wr32_indirect(amd_card_t *c, u32 offset, u32 value) {
    if (!c->regs || c->regs_size < 8) return;
    amd_wr32(c, AMD_MM_INDEX, offset);
    amd_wr32(c, AMD_MM_DATA, value);
}

/* The card's own memory, before there is any aperture onto it.  This is the
 * only way to read the discovery table, because the table is what says how
 * much memory there is to map. */
void amd_read_vram(amd_card_t *c, u64 at, void *into, size_t len) {
    u8 *out = (u8 *)into;

    for (size_t done = 0; done < len; done += 4) {
        u64 address = at + done;

        /* The high half goes in its own register; without it only the first
         * four gigabytes are reachable, which on a modern card is most of the
         * memory but not the part the table is in. */
        amd_wr32(c, AMD_MM_INDEX_HI, (u32)(address >> 31));
        amd_wr32(c, AMD_MM_INDEX, ((u32)address & 0x7FFFFFFFu) | AMD_MM_INDEX_VRAM);
        u32 word = amd_rd32(c, AMD_MM_DATA);

        for (size_t i = 0; i < 4 && done + i < len; i++)
            out[done + i] = (u8)(word >> (8 * i));
    }

    amd_wr32(c, AMD_MM_INDEX_HI, 0);
}

/* --------------------------------------------------------------- what it is
 *
 * A courtesy table, not a dependency.  Ranges rather than individual
 * identifiers, because AMD assigns a block per chip and the individual numbers
 * within it are board variants of the same silicon - so a card that shipped
 * after this was written still lands in the right family.
 */
typedef struct {
    u16 first, last;
    const char *codename;
    const char *architecture;
    const char *marketing;
} amd_family_t;

static const amd_family_t families[] = {
    /* GCN, the generation that started it.  */
    { 0x6780, 0x679F, "Tahiti",       "GCN 1",  "Radeon HD 7900"      },
    { 0x6800, 0x683F, "Pitcairn",     "GCN 1",  "Radeon HD 7800"      },
    { 0x6600, 0x666F, "Oland",        "GCN 1",  "Radeon R7 200"       },
    { 0x6640, 0x665F, "Bonaire",      "GCN 2",  "Radeon R7 260"       },
    { 0x67A0, 0x67BF, "Hawaii",       "GCN 2",  "Radeon R9 290"       },
    { 0x6900, 0x694F, "Tonga",        "GCN 3",  "Radeon R9 380"       },
    { 0x7300, 0x730F, "Fiji",         "GCN 3",  "Radeon R9 Fury"      },
    { 0x67C0, 0x67DF, "Polaris 10",   "GCN 4",  "Radeon RX 470/480/570/580" },
    { 0x67E0, 0x67FF, "Polaris 11",   "GCN 4",  "Radeon RX 460/560"   },
    { 0x6980, 0x699F, "Polaris 12",   "GCN 4",  "Radeon RX 540/550"   },
    { 0x6860, 0x687F, "Vega 10",      "GCN 5",  "Radeon RX Vega"      },
    { 0x66A0, 0x66BF, "Vega 20",      "GCN 5",  "Radeon VII"          },
    { 0x15DD, 0x15DD, "Raven",        "GCN 5",  "Ryzen integrated"    },
    { 0x1636, 0x1636, "Renoir",       "GCN 5",  "Ryzen 4000 integrated" },
    { 0x1638, 0x1638, "Cezanne",      "GCN 5",  "Ryzen 5000 integrated" },

    /* RDNA, where the discovery table arrives.  */
    { 0x7310, 0x733F, "Navi 10",      "RDNA 1", "Radeon RX 5700"      },
    { 0x7340, 0x735F, "Navi 14",      "RDNA 1", "Radeon RX 5500"      },
    { 0x7360, 0x737F, "Navi 12",      "RDNA 1", "Radeon Pro V520"     },
    { 0x73A0, 0x73BF, "Navi 21",      "RDNA 2", "Radeon RX 6800/6900" },
    { 0x73C0, 0x73DF, "Navi 22",      "RDNA 2", "Radeon RX 6700"      },
    { 0x73E0, 0x73FF, "Navi 23",      "RDNA 2", "Radeon RX 6600"      },
    { 0x7420, 0x743F, "Navi 24",      "RDNA 2", "Radeon RX 6400/6500" },
    { 0x1681, 0x1681, "Rembrandt",    "RDNA 2", "Ryzen 6000 integrated" },
    { 0x163F, 0x163F, "Van Gogh",     "RDNA 2", "Steam Deck"          },
    { 0x744C, 0x744C, "Navi 31",      "RDNA 3", "Radeon RX 7900"      },
    { 0x7448, 0x7448, "Navi 31",      "RDNA 3", "Radeon Pro W7900"    },
    { 0x745E, 0x745E, "Navi 31",      "RDNA 3", "Radeon RX 7900 GRE"  },
    { 0x7470, 0x747F, "Navi 32",      "RDNA 3", "Radeon RX 7700/7800" },
    { 0x7480, 0x749F, "Navi 33",      "RDNA 3", "Radeon RX 7600"      },
    { 0x15BF, 0x15BF, "Phoenix",      "RDNA 3", "Ryzen 7040 integrated" },
    { 0x150E, 0x150E, "Strix Point",  "RDNA 3", "Ryzen AI 300 integrated" },
    { 0x7550, 0x756F, "Navi 48",      "RDNA 4", "Radeon RX 9070"      },
    { 0x7590, 0x759F, "Navi 44",      "RDNA 4", "Radeon RX 9060"      },
};

bool amd_identify(amd_card_t *c, u16 device, u8 revision) {
    c->pci_device = device;
    c->pci_revision = revision;
    c->codename = "unknown";
    c->architecture = "unknown";
    c->marketing = NULL;

    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        if (device >= families[i].first && device <= families[i].last) {
            c->codename = families[i].codename;
            c->architecture = families[i].architecture;
            c->marketing = families[i].marketing;
            return true;
        }
    }

    /* Not in the table, which is the case this driver is built to survive: the
     * discovery table below says what the card actually contains, and that is
     * what everything else uses. */
    kinfo("amd", "device %04x is not in the identifier table; what it contains "
                 "will come from the card itself", device);
    return false;
}

/* ------------------------------------------------------- how much memory
 *
 * A register that holds it in megabytes, and has since Southern Islands.  It
 * is read before anything else because the discovery table sits relative to
 * the top of memory, so its address is not knowable until this is.
 */
bool amd_read_vram_size(amd_card_t *c) {
    u32 megabytes = amd_rd32(c, AMD_CONFIG_MEMSIZE);

    /* A card that is powered but not initialised reads back all-ones, and a
     * card that is not there reads back all-ones too.  Neither is a size. */
    if (megabytes == 0 || megabytes == 0xFFFFFFFFu || megabytes > (1u << 20)) {
        c->vram_bytes = 0;
        c->vram_exact = false;
        return false;
    }

    c->vram_bytes = (u64)megabytes << 20;
    c->vram_exact = true;
    return true;
}

/* ------------------------------------------------------- the discovery table
 *
 * The layout, from AMD's discovery.h:
 *
 *   binary_header      signature, version, checksum, size, and a list of six
 *                      tables with their offsets
 *   ip_discovery_header  its own signature, then a count of dies
 *   die_header         a die identifier and how many blocks are on it
 *   ip                 per block: what it is, which instance, what version,
 *                      and one base address per aperture
 *
 * Everything is little-endian and packed, and the offsets inside are relative
 * to the start of the binary rather than to VRAM - which is the detail that
 * turns a working parser into one that reads garbage.
 */
static u16 checksum16(const u8 *data, u32 len) {
    u16 sum = 0;
    for (u32 i = 0; i < len; i++) sum += data[i];
    return sum;
}

#define DISCOVERY_BYTES (16 * 1024)

bool amd_read_discovery(amd_card_t *c) {
    c->ips = 0;
    c->discovered = false;

    if (!c->vram_bytes) {
        kwarn("amd", "the discovery table sits relative to the top of memory "
                     "and the memory size is not known yet");
        return false;
    }

    static u8 binary[DISCOVERY_BYTES];
    u64 at = c->vram_bytes - AMD_DISCOVERY_TMR_OFFSET;
    amd_read_vram(c, at, binary, sizeof binary);

    u32 signature = (u32)binary[0] | ((u32)binary[1] << 8) |
                    ((u32)binary[2] << 16) | ((u32)binary[3] << 24);
    if (signature != AMD_BINARY_SIGNATURE) {
        kinfo("amd", "no discovery table at the top of memory (read %08x); "
                     "this is a card old enough to have a fixed layout",
              signature);
        return false;
    }

    u16 binary_size = (u16)(binary[0x0A] | (binary[0x0B] << 8));
    u16 stored = (u16)(binary[0x08] | (binary[0x09] << 8));
    if (binary_size < 0x24 || binary_size > DISCOVERY_BYTES) {
        kwarn("amd", "the discovery table claims to be %u bytes", binary_size);
        return false;
    }

    /* The checksum covers everything after the header.  Believing a table that
     * fails it means parsing whatever was in memory. */
    u16 computed = checksum16(binary + 0x24, binary_size - 0x24);
    if (computed != stored) {
        kwarn("amd", "the discovery table checksums to %04x, not %04x - it is "
                     "not being read correctly", computed, stored);
        return false;
    }

    /* The first of the six tables is the one that lists the blocks. */
    u16 table_offset = (u16)(binary[0x0C] | (binary[0x0D] << 8));
    if (table_offset + 0x30 > binary_size) {
        kwarn("amd", "the block list is outside the table");
        return false;
    }

    const u8 *ipd = binary + table_offset;
    u32 ipd_signature = (u32)ipd[0] | ((u32)ipd[1] << 8) |
                        ((u32)ipd[2] << 16) | ((u32)ipd[3] << 24);
    if (ipd_signature != AMD_DISCOVERY_SIGNATURE) {
        kwarn("amd", "the block list carries signature %08x", ipd_signature);
        return false;
    }

    u16 dies = (u16)(ipd[0x0C] | (ipd[0x0D] << 8));
    if (dies == 0 || dies > 16) {
        kwarn("amd", "the table says %u dies", dies);
        return false;
    }

    /* die_info[] is a list of offsets, one per die, each relative to the start
     * of the binary. */
    for (u16 die = 0; die < dies; die++) {
        /* Each die_info is an identifier then an offset, so the offset is
         * two bytes in.  Reading the identifier instead lands the walk in the
         * middle of the header and everything after it is nonsense. */
        u16 die_at = (u16)(ipd[0x10 + die * 4] | (ipd[0x11 + die * 4] << 8));
        if (die_at + 4 > binary_size) continue;

        const u8 *dh = binary + die_at;
        u16 blocks = (u16)(dh[2] | (dh[3] << 8));
        u32 walk = die_at + 4;

        for (u16 b = 0; b < blocks; b++) {
            if (walk + 8 > binary_size) break;
            const u8 *e = binary + walk;

            u16 hw_id = (u16)(e[0] | (e[1] << 8));
            u8  instance = e[2];
            u8  bases = e[3];
            u8  major = e[4], minor = e[5], revision = e[6];

            /* The entry's length depends on how many apertures it has, so it
             * has to be stepped over rather than indexed. */
            u32 entry_len = 8 + (u32)bases * 4;
            if (walk + entry_len > binary_size) break;

            if (c->ips < AMD_MAX_IP) {
                amd_ip_t *ip = &c->ip[c->ips++];
                ip->hw_id = hw_id;
                ip->instance = instance;
                ip->major = major;
                ip->minor = minor;
                ip->revision = revision;
                ip->bases = bases > 4 ? 4 : bases;
                for (int i = 0; i < ip->bases; i++)
                    ip->base[i] = (u32)e[8 + i * 4] |
                                  ((u32)e[9 + i * 4] << 8) |
                                  ((u32)e[10 + i * 4] << 16) |
                                  ((u32)e[11 + i * 4] << 24);
            }

            walk += entry_len;
        }
    }

    if (!c->ips) {
        kwarn("amd", "the discovery table parsed but listed no blocks");
        return false;
    }

    /* The ones the rest of the driver needs to know where to find. */
    const amd_ip_t *gc = amd_find_ip(c, AMD_HWID_GC, 0);
    const amd_ip_t *mp0 = amd_find_ip(c, AMD_HWID_MP0, 0);
    const amd_ip_t *mp1 = amd_find_ip(c, AMD_HWID_MP1, 0);
    const amd_ip_t *dc = amd_find_ip(c, AMD_HWID_DCI, 0);
    const amd_ip_t *sdma = amd_find_ip(c, AMD_HWID_SDMA0, 0);

    c->gc_base = gc ? gc->base[0] : 0;
    c->mp0_base = mp0 ? mp0->base[0] : 0;
    c->mp1_base = mp1 ? mp1->base[0] : 0;
    c->dc_base = dc ? dc->base[0] : 0;
    c->sdma_base = sdma ? sdma->base[0] : 0;

    if (gc) { c->gfx_major = gc->major; c->gfx_minor = gc->minor; }

    c->discovered = true;
    return true;
}

const amd_ip_t *amd_find_ip(amd_card_t *c, u16 hw_id, u8 instance) {
    for (int i = 0; i < c->ips; i++)
        if (c->ip[i].hw_id == hw_id && c->ip[i].instance == instance)
            return &c->ip[i];
    return NULL;
}

/* ----------------------------------------------------- driving a real card
 *
 * Everything above is the driver.  This is what the rest of the system asks
 * for: given a card that is actually on the bus, drive it far enough to say
 * what it is and what is reachable, and be exact about the difference.
 *
 * AMD does not put its registers where NVIDIA does.  On an NVIDIA card BAR 0
 * is a sixteen-megabyte register window; on an AMD card BAR 0 is the memory
 * aperture and the registers are in a small BAR further along - 256 or 512
 * kilobytes, and on most cards it is BAR 5.  Picking the first memory BAR, the
 * way a driver written for one vendor would, lands in the middle of video
 * memory and reads back whatever was there.
 */
void amd_identify_gpu(gpu_info_t *g, pci_dev_t *d) {
    static amd_card_t card;

    memset(&card, 0, sizeof card);
    card.temperature_c = -1000;

    amd_identify(&card, d->device, d->revision);

    if (card.marketing)
        snprintf(g->name, sizeof g->name, "AMD %s", card.marketing);
    else
        snprintf(g->name, sizeof g->name, "AMD graphics [1002:%04x]", d->device);
    snprintf(g->arch, sizeof g->arch, "%s (%s)", card.architecture,
             card.codename);
    g->revision = d->revision;

    /* Find the register window by its size rather than its position. */
    u64 registers = 0;
    size_t register_size = 0;
    for (int i = 0; i < 6; i++) {
        if (!d->bar[i] || d->bar_is_io[i]) continue;
        u64 size = d->bar_size[i];
        if (size >= 0x20000 && size <= 0x100000) {
            registers = d->bar[i];
            register_size = (size_t)size;
            break;
        }
    }

    if (!registers) {
        strlcpy(g->note, "no register window found; display via the firmware "
                         "framebuffer only", sizeof g->note);
        return;
    }

    card.regs = vmm_map_mmio(registers, register_size);
    card.regs_size = register_size;
    if (!card.regs) {
        strlcpy(g->note, "the register window could not be mapped",
                sizeof g->note);
        return;
    }

    /* How much memory is fitted, from the register that has held it since
     * Southern Islands - a real number rather than the size of an aperture
     * that is usually a fraction of it. */
    if (amd_read_vram_size(&card)) {
        g->vram_bytes = card.vram_bytes;
        g->vram_exact = true;
    }

    /* And the card's own description of its blocks, which is what makes this
     * work on a chip this driver has never seen. */
    if (amd_read_discovery(&card)) {
        char version[24] = "";
        if (card.gfx_major)
            snprintf(version, sizeof version, ", graphics %d.%d",
                     card.gfx_major, card.gfx_minor);
        snprintf(g->note, sizeof g->note,
                 "read its own block table: %d blocks%s; the command processor "
                 "needs microcode signed by AMD", card.ips, version);
    } else {
        strlcpy(g->note, "identified from its registers; the command processor "
                         "needs microcode signed by AMD", sizeof g->note);
    }

    /* Which is the honest bound on all of it: nothing outside AMD is signed by
     * AMD, so the engines stay where firmware left them. */
    g->firmware_needed = true;
    g->firmware_present = false;
    strlcpy(g->firmware_name, "AMD-signed PSP and SMU microcode",
            sizeof g->firmware_name);
}

/* ------------------------------------------------------- bringing one up
 *
 * Everything above this point is a piece: reading a name, a size, a table, a
 * connector, a temperature.  This is the order they go in, and it is the only
 * place in the AMD driver that runs on a real card at boot.
 *
 * Each step says what it is about to do before it does it.  On a machine with
 * no serial cable a driver that stops takes the screen with it, and the last
 * thing printed is then the only evidence of where it stopped.  Announcing
 * beforehand turns that into a name; announcing afterwards would leave a
 * silence that could be any of seven things.
 */
static void amd_step(const char *what) { kinfo("amd", "about to: %s", what); }

void amd_bring_up(amd_card_t *c) {
    amd_step("read the card's name from its device number");
    amd_identify(c, c->pci_device, c->pci_revision);

    amd_step("find out how much memory the card has");
    amd_read_vram_size(c);

    /* Every AMD card since Navi carries a table at the top of its memory
     * saying which blocks it is built from and where each one's registers
     * are.  Without it the register offsets have to be guessed from the
     * device number, which is how a driver ends up writing to the wrong
     * block on a card it has not been told about. */
    amd_step("read the table of what blocks the card is built from");
    amd_read_discovery(c);

    amd_step("read the card's own description out of its ROM");
    if (amd_read_vbios(c)) amd_parse_vbios(c);

    amd_step("look at what is plugged into the card");
    amd_parse_connectors(c);

    amd_step("ask each connector whether a monitor is on the end of it");
    amd_probe_monitors(c);

    amd_step("wake the power processor and read the temperature");
    if (amd_smu_bring_up(c)) amd_read_sensors(c);

    amd_step("nothing further; the engines need microcode signed by AMD");

    /* The line worth photographing if only one can be. */
    /* The revision is written in hex everywhere AMD writes it - a Navi 31 is
     * a C8, not a 200 - so printing it in decimal makes it unsearchable. */
    kinfo("amd", "found: %s %s, device %04x rev %02x, %llu MiB, %d "
                 "connector(s), "
                 "%d block(s), BIOS %s",
          c->architecture ? c->architecture : "unknown architecture",
          c->codename ? c->codename : "unnamed",
          c->pci_device, c->pci_revision,
          (unsigned long long)(c->vram_bytes >> 20), c->connectors, c->ips,
          c->vbios_version[0] ? c->vbios_version : "not read");
}

#define AMD_MAX_CARDS 4
static amd_card_t amd_cards[AMD_MAX_CARDS];
static int amd_card_count;

void amd_driver_init(void) {
    pci_dev_t *pci = NULL;

    amd_allow_writes(cmdline_has("gpuwrite"));

    while ((pci = pci_find(0x03, 0xFF, 0xFF, pci)) != NULL) {
        if (pci->vendor != 0x1002) continue;              /* AMD/ATI */
        if (amd_card_count >= AMD_MAX_CARDS) break;

        amd_card_t *c = &amd_cards[amd_card_count];
        memset(c, 0, sizeof *c);

        pci_enable_memory(pci);
        pci_enable_bus_master(pci);

        c->pci_device   = pci->device;
        c->pci_revision = pci->revision;

        /* AMD puts these the other way round from NVIDIA: the first region is
         * the window onto video memory, and the registers are the sixth on
         * anything since Southern Islands and the third before that.  Taking
         * the first region as registers - which is the arrangement on every
         * other card here - would read video memory and call it a register
         * map, and every number after that would be furniture. */
        if (pci->bar[5] && !pci->bar_is_io[5]) {
            size_t len = pci->bar_size[5] ? (size_t)pci->bar_size[5] : 0x80000;
            c->regs = vmm_map_mmio(pci->bar[5], len);
            c->regs_size = len;
        } else if (pci->bar[2] && !pci->bar_is_io[2]) {
            size_t len = pci->bar_size[2] ? (size_t)pci->bar_size[2] : 0x80000;
            c->regs = vmm_map_mmio(pci->bar[2], len);
            c->regs_size = len;
        }
        if (pci->bar[0] && !pci->bar_is_io[0]) {
            c->vram_base     = pci->bar[0];
            c->vram_aperture = (size_t)pci->bar_size[0];
        }

        if (!c->regs) {
            kwarn("amd", "the card's register window could not be mapped");
            continue;
        }

        amd_card_count++;
        amd_bring_up(c);
    }
}
