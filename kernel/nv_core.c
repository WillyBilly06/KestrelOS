/* nv_core.c - an NVIDIA graphics driver.
 *
 * What a driver for one of these cards actually does, before any question of
 * drawing arises, is find out what the card is.  Not from a table of device
 * identifiers - those go out of date the day a new card ships - but by asking
 * the silicon and then reading the card's own description of itself out of its
 * ROM.  That description says how much memory is fitted and of what kind, what
 * connectors exist, which pins drive them, and which two wires each monitor
 * answers on.  Everything else is built on it.
 *
 * All of that works on every NVIDIA card ever made, from the Riva TNT to
 * Blackwell, and none of it needs a byte of firmware from anybody.  It is most
 * of what Linux's own driver for these cards does before it gets to pixels.
 *
 * Above it, the two halves diverge sharply:
 *
 *   - Up to Pascal, the display and drawing engines answer to registers, and
 *     nv_disp.c drives them.
 *   - From Turing on, they answer only to a co-processor running firmware
 *     NVIDIA signs.  nv_gsp.c loads that firmware when the user has supplied
 *     it, and says so plainly when they have not.
 *
 * ---------------------------------------------------------------------------
 * On testing.  Nothing available to run on has an NVIDIA card in it - VMware
 * presents its own display adapter - so this drives a model of one instead,
 * built from the same register map.  What that establishes and what it cannot
 * is set out in nv_model.c.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "klog.h"
#include "nv.h"
#include "nv_compute.h"
#include "nvkms_port.h"

/* --------------------------------------------------------------- the window */

void nv_model_sync(void);
void nv_falcon_model_write(u32 offset, u32 value);
void nv_falcon_model_read(u32 offset);
bool nv_model_present(void);

u32 nv_rd32(nv_card_t *c, u32 offset) {
    if (!c->regs || offset + 4 > c->regs_size) return 0xFFFFFFFFu;
    if (c->modelled) {
        nv_model_sync();
        nv_falcon_model_read(offset);
        nv_fsp_model_read(offset);
    }
    return *(volatile u32 *)(c->regs + offset);
}

/* Whether this driver may write to a real card at all.
 *
 * Reading a graphics card's registers cannot break anything.  Writing to them
 * can, and the thing it breaks first is the screen - which on a machine with
 * no serial cable is the only way anyone would ever find out.  That asymmetry
 * is worth building around, because the state this driver is actually in is
 * that it has been run against models of these cards and never against one.
 *
 * So on a real card it starts read-only.  Everything that can be learned by
 * reading still is - what the chip is, how much memory, what the ROM says, how
 * many connectors, how hot it is - and the two things that need writes are
 * skipped and said out loud rather than attempted:
 *
 *   Switching the ROM into the register window.  The copy in the ROM chip is
 *   reached by taking the window away from what normally lives there and
 *   putting it back afterwards.  If the offset this driver believes that
 *   switch lives at is wrong on this chip, the write lands somewhere else
 *   entirely and the window never comes back.
 *
 *   Driving a monitor's two wires by hand.  Reading a monitor's identification
 *   means writing the data and clock lines one edge at a time, and those lines
 *   belong to a link the firmware has already brought up and is displaying
 *   through.
 *
 * "gpuwrite" on the kernel command line turns writes on.  It is the right
 * thing to pass on the second boot, once the first has shown the card was
 * identified correctly - and it is exactly the wrong thing to have as the
 * default on a machine nobody has booted this on yet.
 */
static bool writes_allowed;
static u32  writes_refused;

void nv_allow_writes(bool yes) { writes_allowed = yes; }
bool nv_writes_allowed(void) { return writes_allowed; }
u32  nv_writes_refused(void) { return writes_refused; }

void nv_wr32(nv_card_t *c, u32 offset, u32 value) {
    if (!c->regs || offset + 4 > c->regs_size) return;

    /* A model has nothing to break, so it is always written to - otherwise the
     * tests would exercise a driver that is not the one that runs. */
    if (!c->modelled && !writes_allowed) { writes_refused++; return; }

    *(volatile u32 *)(c->regs + offset) = value;

    /* A card sees every write as it happens, and some of what this driver
     * writes only means anything as a sequence - the two wires of an I2C bus
     * are driven by writing the same register over and over, and it is the
     * transitions between those writes that carry the message.  A model that
     * only looked when the driver next read would see the last value and none
     * of the edges. */
    if (c->modelled) {
        nv_model_sync();
        nv_falcon_model_write(offset, value);
        nv_gsp_model_wrote(offset, value);
        nv_dp_model_write(offset, value);
        nv_disp_model_write(offset, value);
        nv_fifo_model_write(offset, value);
        nv_fsp_model_write(offset, value);
    }
}

static u8 nv_rd8(nv_card_t *c, u32 offset) {
    if (!c->regs || offset >= c->regs_size) return 0xFF;
    if (c->modelled) nv_model_sync();
    return *(volatile u8 *)(c->regs + offset);
}

/* ------------------------------------------------------------ identification
 *
 * PMC_BOOT_0 is the first register in the window and has held the same meaning
 * since 1998: the architecture in bits 28 to 20, the implementation within it
 * in 19 to 16, and the revision in the low byte.  A card newer than this code
 * still names its own architecture correctly, which no lookup table can do.
 */
typedef struct { u32 family; const char *architecture; const char *codename; } arch_t;

static const arch_t architectures[] = {
    { 0x000, "Fahrenheit",   "NV0x"   },
    { 0x010, "Celsius",      "NV1x"   },
    { 0x020, "Kelvin",       "NV2x"   },
    { 0x030, "Rankine",      "NV3x"   },
    { 0x040, "Curie",        "NV4x"   },
    { 0x060, "Curie",        "NV6x"   },
    { 0x050, "Tesla",        "G8x"    },
    { 0x080, "Tesla",        "G9x"    },
    { 0x090, "Tesla",        "GT2xx"  },
    { 0x0a0, "Tesla",        "GT21x"  },
    { 0x0c0, "Fermi",        "GF10x"  },
    { 0x0d0, "Fermi",        "GF11x"  },
    { 0x0e0, "Kepler",       "GK10x"  },
    { 0x0f0, "Kepler",       "GK11x"  },
    { 0x100, "Kepler",       "GK20x"  },
    { 0x110, "Maxwell",      "GM10x"  },
    { 0x120, "Maxwell",      "GM20x"  },
    { 0x130, "Pascal",       "GP10x"  },
    { 0x140, "Volta",        "GV10x"  },
    { 0x160, "Turing",       "TU10x"  },
    { 0x170, "Ampere",       "GA10x"  },
    { 0x180, "Hopper",       "GH10x"  },
    { 0x190, "Ada Lovelace", "AD10x"  },
    { 0x1a0, "Blackwell",    "GB10x"  },
    { 0x1b0, "Blackwell",    "GB20x"  },
};

bool nv_identify(nv_card_t *c) {
    c->boot0 = nv_rd32(c, NV_PMC_BOOT_0);

    /* All ones means the window is not really mapped; zero means a card that
     * has not been powered up. */
    if (c->boot0 == 0xFFFFFFFFu || c->boot0 == 0) {
        kwarn("nvidia", "the register window reads %08x; the card is not "
                        "responding", c->boot0);
        return false;
    }

    if (c->boot0 & 0x1F000000u) {
        c->chipset = (c->boot0 & 0x1FF00000u) >> 20;
        c->revision = (u8)(c->boot0 & 0xFF);
    } else {
        /* The Riva TNT generation leaves the architecture field clear and puts
         * the identity in the next nibble down. */
        c->chipset = 0x04;
        c->revision = (u8)((c->boot0 >> 16) & 0xFF);
    }

    u32 family = c->chipset & 0x1F0;
    c->architecture = "unknown";
    c->codename = "unknown";
    for (size_t i = 0; i < ARRAY_LEN(architectures); i++) {
        if (architectures[i].family != family) continue;
        c->architecture = architectures[i].architecture;
        c->codename = architectures[i].codename;
        break;
    }

    kinfo("nvidia", "chip %03x, %s (%s), revision %u.%u",
          c->chipset, c->architecture, c->codename,
          c->revision >> 4, c->revision & 0xF);
    return true;
}

/* True for the generations whose display and drawing engines answer to
 * registers rather than to a signed co-processor. */
static bool driveable_by_registers(const nv_card_t *c) {
    return (c->chipset & 0x1F0) < 0x160;
}

/* ------------------------------------------------------------------ the ROM
 *
 * There are three places the card's own description can be read from and no
 * card offers all three, so all three are tried in the order that works most
 * often:
 *
 *   1. The PROM window - the ROM chip itself, mapped into the register
 *      window.  It has to be switched on first, and switched back off, because
 *      leaving it on takes the window away from what normally lives there.
 *   2. PRAMIN - a copy the card's own start-up code left at the top of video
 *      memory.  Present on nearly everything since NV50.
 *   3. The PCI expansion ROM - the firmware's own copy, if the machine's
 *      firmware left it mapped.
 */
static bool rom_looks_valid(const u8 *data, size_t size) {
    if (size < 0x100) return false;
    /* Every option ROM starts with the same two bytes. */
    if (data[0] != 0x55 || data[1] != 0xAA) return false;
    /* And the length in the third byte, in units of half a kilobyte, has to be
     * something a ROM could actually be. */
    if (data[2] == 0 || data[2] > 0x80) return false;
    return true;
}

static bool read_rom_from_prom(nv_card_t *c, u8 *out, size_t cap) {
    /* This one cannot work without a write: the ROM is not in the window until
     * it has been switched in.  Rather than write, fail and let the caller try
     * the next place - and let it say which places it did not look. */
    if (!c->modelled && !nv_writes_allowed()) return false;

    /* Switching the ROM into the window, and remembering to put it back. */
    u32 saved = nv_rd32(c, NV_PBUS_PCI_NV_20);
    nv_wr32(c, NV_PBUS_PCI_NV_20, NV_PBUS_PCI_NV_20_ROM_SHADOW_DISABLED);

    size_t want = cap < NV_PROM_SIZE ? cap : NV_PROM_SIZE;
    for (size_t i = 0; i < want; i++) out[i] = nv_rd8(c, NV_PROM_OFFSET + (u32)i);

    nv_wr32(c, NV_PBUS_PCI_NV_20, saved);
    return rom_looks_valid(out, want);
}

static bool read_rom_from_pramin(nv_card_t *c, u8 *out, size_t cap) {
    /* Same again: the window has to be moved before the copy is under it. */
    if (!c->modelled && !nv_writes_allowed()) return false;

    /* The copy sits at the very top of video memory.  Pointing the instance
     * window at it needs the memory size, which is why this is tried after
     * that has been read. */
    if (!c->vram_bytes) return false;

    u64 at = c->vram_bytes - NV_PRAMIN_SIZE;
    u32 window = (u32)(at >> 16);
    u32 saved = nv_rd32(c, NV_PBUS_BAR0_WINDOW);
    nv_wr32(c, NV_PBUS_BAR0_WINDOW, window);

    size_t want = cap < 0x10000 ? cap : 0x10000;
    for (size_t i = 0; i < want; i++)
        out[i] = nv_rd8(c, NV_PRAMIN_OFFSET + (u32)(NV_PRAMIN_SIZE - 0x10000 + i));

    nv_wr32(c, NV_PBUS_BAR0_WINDOW, saved);
    return rom_looks_valid(out, want);
}

#define NV_VBIOS_MAX 0x10000
static u8 vbios_storage[NV_VBIOS_MAX];

bool nv_read_vbios(nv_card_t *c) {
    c->vbios = NULL;
    c->vbios_size = 0;
    c->vbios_source = NULL;

    if (read_rom_from_prom(c, vbios_storage, sizeof vbios_storage)) {
        c->vbios_source = "the ROM chip";
    } else if (read_rom_from_pramin(c, vbios_storage, sizeof vbios_storage)) {
        c->vbios_source = "the copy in video memory";
    } else {
        kwarn("nvidia", "the card's own description could not be read from "
                        "either the ROM or video memory");
        return false;
    }

    /* The length is in the third byte, counted in half-kilobytes. */
    size_t declared = (size_t)vbios_storage[2] * 512;
    c->vbios = vbios_storage;
    c->vbios_size = declared && declared <= NV_VBIOS_MAX ? declared : NV_VBIOS_MAX;

    kinfo("nvidia", "video BIOS read from %s, %zu bytes",
          c->vbios_source, c->vbios_size);
    return true;
}

/* ---------------------------------------------------------------- the tables
 *
 * NVIDIA's own tables are found by searching for a signature rather than at a
 * fixed offset, because where they sit depends on how much of the ROM the
 * standard header took.  From the signature hangs a list of tables, each
 * marked with a letter saying what it is for.
 */
static inline u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static const u8 *find_bit_table(nv_card_t *c, u8 id, u8 *version_out, u16 *length_out) {
    if (!c->vbios || c->vbios_size < 0x100) return NULL;

    /* The header opens with a two-byte identifier, 0xB8FF, and only then the
     * signature - so the signature starts two bytes in, not one.
     *
     * Reading it one byte in compares against 0xB8 followed by the first three
     * letters, which cannot match, so this used to find no table on any real
     * card at all.  Nothing caught it because the only ROMs it was ever run
     * against were modelled ones, laid out to satisfy this same reading.  A
     * real card's ROM is what showed it up. */
    for (size_t at = 0; at + 12 < c->vbios_size; at++) {
        if (c->vbios[at] != 0xFF) continue;
        if (c->vbios[at + 1] != 0xB8) continue;
        if (le32(&c->vbios[at + 2]) != NV_BIT_SIGNATURE) continue;

        u8 entries = c->vbios[at + 10];
        u8 entry_size = c->vbios[at + 9];
        if (!entry_size || entry_size > 16) continue;

        size_t table = at + c->vbios[at + 8];
        for (u8 i = 0; i < entries; i++) {
            size_t e = table + (size_t)i * entry_size;
            if (e + sizeof(nv_bit_entry_t) > c->vbios_size) break;

            const nv_bit_entry_t *entry = (const void *)&c->vbios[e];
            if (entry->id != id) continue;
            if (entry->offset >= c->vbios_size) return NULL;

            if (version_out) *version_out = entry->version;
            if (length_out) *length_out = entry->length;
            return &c->vbios[entry->offset];
        }
        return NULL;
    }
    return NULL;
}

/* The card's own version string, which is what a person recognises. */
static void read_vbios_version(nv_card_t *c) {
    strlcpy(c->vbios_version, "unknown", sizeof c->vbios_version);
    if (!c->vbios) return;

    u8 version = 0;
    u16 length = 0;
    const u8 *bit_i = find_bit_table(c, 'i', &version, &length);
    if (bit_i && length >= 5) {
        /* Five bytes on anything recent.  The first four are the version and
         * the fifth is the board maker's own revision of it, and NVIDIA's
         * tools print all five - an RTX 5070 Ti reports 98.03.58.00.9d, where
         * the 9d is the board.  Printing only the first four gives a string
         * that looks right, matches nothing anyone can search for, and cannot
         * distinguish two boards built on the same chip with different
         * firmware.  Which is the whole reason to read it. */
        snprintf(c->vbios_version, sizeof c->vbios_version,
                 "%02X.%02X.%02X.%02X.%02X",
                 bit_i[3], bit_i[2], bit_i[1], bit_i[0], bit_i[4]);
        return;
    }
    if (bit_i && length >= 4) {
        /* Older cards stop at four. */
        snprintf(c->vbios_version, sizeof c->vbios_version, "%02X.%02X.%02X.%02X",
                 bit_i[3], bit_i[2], bit_i[1], bit_i[0]);
        return;
    }

    /* Older cards keep a readable string instead.  It is searched for across
     * the whole image rather than only the first kilobyte: on the cards that
     * have one it is not always near the front, and stopping early is a way to
     * report "unknown" about a ROM that says perfectly clearly what it is. */
    for (size_t at = 0; at + 12 < c->vbios_size; at++) {
        if (c->vbios[at] != 'V' || c->vbios[at + 1] != 'e') continue;
        if (memcmp(&c->vbios[at], "Version ", 8)) continue;
        size_t n = 0;
        while (n < sizeof c->vbios_version - 1 &&
               at + 8 + n < c->vbios_size &&
               c->vbios[at + 8 + n] > ' ')
            { c->vbios_version[n] = (char)c->vbios[at + 8 + n]; n++; }
        c->vbios_version[n] = 0;
        return;
    }
}

/* ------------------------------------------------------------ the connectors
 *
 * The display configuration block lists every output the card has: what kind
 * it is, which head can drive it, and which pair of wires the monitor on it
 * answers over.  Without this a driver has to guess, and guessing wrong means
 * driving a signal into a socket that is not there.
 */
const char *nv_output_type_name(nv_output_type t) {
    switch (t) {
    case NV_OUTPUT_ANALOG: return "VGA";
    case NV_OUTPUT_TV:     return "TV";
    case NV_OUTPUT_TMDS:   return "DVI or HDMI";
    case NV_OUTPUT_LVDS:   return "internal panel";
    case NV_OUTPUT_SDI:    return "SDI";
    case NV_OUTPUT_DP:     return "DisplayPort";
    default:               return "unknown";
    }
}

const char *nv_connector_name(nv_connector_kind kind) {
    switch (kind) {
    case NV_CONNECTOR_VGA:   return "VGA";
    case NV_CONNECTOR_DVI:   return "DVI";
    case NV_CONNECTOR_HDMI:  return "HDMI";
    case NV_CONNECTOR_DP:    return "DisplayPort";
    case NV_CONNECTOR_EDP:   return "embedded DisplayPort";
    case NV_CONNECTOR_USB_C: return "USB-C";
    default:                 return "unknown";
    }
}

/* What kind of socket a connector table entry describes.
 *
 * The values are a manufacturer's list rather than a specification's, so this
 * is a table of the ones that appear on cards rather than a formula.  Anything
 * not listed is reported as unknown, which is honest and still counts the
 * socket - a card with a connector nobody here recognised still has that
 * connector.
 */
static nv_connector_kind connector_kind_of(u8 raw) {
    switch (raw) {
    case 0x00: case 0x01:                       return NV_CONNECTOR_VGA;
    case 0x30: case 0x31: case 0x32:            return NV_CONNECTOR_DVI;
    case 0x38: case 0x39:                       return NV_CONNECTOR_DP;
    case 0x40: case 0x41: case 0x42:            return NV_CONNECTOR_EDP;
    case 0x45:                                  return NV_CONNECTOR_EDP;
    case 0x46: case 0x47: case 0x48:
    case 0x64: case 0x65:                       return NV_CONNECTOR_DP;
    case 0x60: case 0x61: case 0x62: case 0x63: return NV_CONNECTOR_HDMI;
    case 0x66: case 0x67: case 0x68:            return NV_CONNECTOR_USB_C;
    default:                                    return NV_CONNECTOR_UNKNOWN;
    }
}

/* The connector table, which the display block points at.  Read after the
 * block itself, because its address comes from the block's header. */
static void parse_connectors(nv_card_t *c, size_t dcb_at, u8 header_size) {
    c->connectors = 0;
    if (header_size < 0x16) return;                 /* too old to carry one */

    size_t p = dcb_at + 0x14;
    if (p + 2 > c->vbios_size) return;

    u16 at = le16(&c->vbios[p]);
    if (!at || (size_t)at + 4 > c->vbios_size) return;

    const u8 *t = &c->vbios[at];
    u8 hlen = t[1], count = t[2], entry_size = t[3];
    if (!entry_size || entry_size > 8 || !count) return;

    for (u8 i = 0; i < count && c->connectors < NV_MAX_OUTPUTS; i++) {
        size_t e = (size_t)at + hlen + (size_t)i * entry_size;
        if (e + 1 > c->vbios_size) break;

        u8 raw = c->vbios[e];
        if (raw == 0xFF) continue;                  /* a slot with no socket */

        c->connector_kind[c->connectors++] = connector_kind_of(raw);
    }
}

static bool parse_dcb(nv_card_t *c) {
    c->outputs = 0;
    if (!c->vbios || c->vbios_size < 0x100) return false;

    /* The block is found through a pointer at a fixed place near the front of
     * the ROM - one of the few things that has never moved. */
    if (0x36 + 2 > c->vbios_size) return false;
    u16 at = le16(&c->vbios[0x36]);
    if (!at || at + 8 > c->vbios_size) return false;

    const u8 *dcb = &c->vbios[at];
    u8 version = dcb[0];

    u8 header_size, entry_size, entries;
    if (version >= 0x20) {
        header_size = dcb[1];
        entries = dcb[2];
        entry_size = dcb[3];
        /* A signature guards against a pointer that landed somewhere else. */
        if (version >= 0x30) {
            /* The signature guards against a pointer that landed somewhere
             * else in the ROM, which is a real possibility: it is a sixteen-bit
             * offset read from a fixed place, and a card whose ROM was read
             * short gives a plausible-looking one. */
            u32 signature = le32(&dcb[6]);
            if (signature != 0x4EDCBDCB) {
                kwarn("nvidia", "the connector table's signature is %08x, not "
                                "the expected 4edcbdcb; not reading it",
                      signature);
                return false;
            }
        }
    } else {
        /* The oldest layout: no header, fixed-size entries. */
        header_size = 0;
        entry_size = 8;
        entries = 16;
    }

    if (!entry_size || entry_size > 16) return false;

    for (u8 i = 0; i < entries && c->outputs < NV_MAX_OUTPUTS; i++) {
        size_t e = (size_t)at + header_size + (size_t)i * entry_size;
        if (e + 4 > c->vbios_size) break;

        u32 word = le32(&c->vbios[e]);
        u8 type = (u8)(word & 0x0F);

        if (type == NV_OUTPUT_EOL) break;          /* the end of the list */
        if (type == NV_OUTPUT_UNUSED) continue;

        nv_output_t *out = &c->output[c->outputs++];
        memset(out, 0, sizeof *out);
        out->type = (nv_output_type)type;
        out->heads = (u8)((word >> 8) & 0x0F);
        out->connector = (u8)((word >> 12) & 0x0F);
        out->i2c_bus = (u8)((word >> 4) & 0x0F);
        out->link = (u8)((word >> 24) & 0x0F);
        out->used = true;
    }

    /* And the sockets those entries come out of, which is a different count
     * from the entries and the one worth showing. */
    parse_connectors(c, at, header_size);

    return c->outputs > 0;
}

bool nv_parse_vbios(nv_card_t *c) {
    c->vbios_valid = false;
    if (!c->vbios) return false;

    read_vbios_version(c);
    bool connectors = parse_dcb(c);

    if (connectors) {
        if (c->connectors) {
            /* Say what is on the bracket, not how many ways the card can
             * drive it: a socket that can carry either DisplayPort or HDMI
             * appears twice in the block and is still one hole. */
            int dp = 0, hdmi = 0, dvi = 0, other = 0;
            for (int i = 0; i < c->connectors; i++) {
                switch (c->connector_kind[i]) {
                case NV_CONNECTOR_DP:  case NV_CONNECTOR_EDP: dp++;   break;
                case NV_CONNECTOR_HDMI:                       hdmi++; break;
                case NV_CONNECTOR_DVI:                        dvi++;  break;
                default:                                      other++; break;
                }
            }
            kinfo("nvidia", "video BIOS %s: %d socket(s) - %d DisplayPort, "
                            "%d HDMI, %d DVI, %d other - across %d output path(s)",
                  c->vbios_version, c->connectors, dp, hdmi, dvi, other,
                  c->outputs);
        } else {
            kinfo("nvidia", "video BIOS %s, %d connector(s)", c->vbios_version, c->outputs);
        }
        for (int i = 0; i < c->outputs; i++)
            kdebug("nvidia", "  connector %d: %s, head mask %x, I2C bus %d",
                   i, nv_output_type_name(c->output[i].type),
                   c->output[i].heads, c->output[i].i2c_bus);
    } else {
        kwarn("nvidia", "video BIOS %s, but its connector table could not be read",
              c->vbios_version);
    }

    c->vbios_valid = true;
    return true;
}

/* ----------------------------------------------------------------- memory */

bool nv_read_vram_size(nv_card_t *c) {
    u32 family = c->chipset & 0x1F0;
    c->vram_bytes = 0;
    c->vram_exact = false;
    c->vram_type = "unknown";

    /* Pascal and later divide the memory among partitions, some of them fused
     * off in the factory.  That is the only reading that gets a modern card
     * right, so it is tried first; the single register below still answers on
     * many of them, and stays as the fallback for the ones where it does. */
    if (family >= 0x130 && nv_read_vram_fbpa(c))
        return true;

    if (family >= 0x0c0) {
        /* Fermi and later: a mantissa and a power-of-two scale, in megabytes.
         * A card that has not been initialised reads zero, so the answer is
         * range-checked rather than trusted. */
        u32 v = nv_rd32(c, NV_PFB_LOCAL_MEMORY_RANGE);
        u32 mantissa = v & 0xF;
        u32 scale = (v >> 4) & 0xF;
        if (mantissa) {
            u64 size = (u64)mantissa << (scale + 20);
            if (size >= (16ULL << 20) && size <= (256ULL << 30)) {
                c->vram_bytes = size;
                c->vram_exact = true;
            }
        }
    } else if (family >= 0x050) {
        /* Tesla: the size in a straightforward register. */
        u32 v = nv_rd32(c, NV_PFB_CSTATUS);
        u64 size = (u64)(v & 0xFFF) << 20;
        if (size >= (16ULL << 20) && size <= (16ULL << 30)) {
            c->vram_bytes = size;
            c->vram_exact = true;
        }
    } else if (family >= 0x010) {
        u32 v = nv_rd32(c, NV_PFB_CSTATUS);
        u64 size = v & 0xFFF00000u;
        if (size >= (4ULL << 20) && size <= (4ULL << 30)) {
            c->vram_bytes = size;
            c->vram_exact = true;
        }
    }

    if (!c->vram_bytes && c->vram_aperture) {
        /* Falling back on the aperture.  Without resizable addressing that is
         * capped at 256 megabytes however much is fitted, so it is reported as
         * a lower bound rather than passed off as the size. */
        c->vram_bytes = c->vram_aperture;
        c->vram_exact = false;
    }

    if (c->vram_bytes) {
        u64 mb = c->vram_bytes >> 20;
        if (mb >= 1024)
            kinfo("nvidia", "%llu GiB of video memory%s", (unsigned long long)(mb >> 10),
                  c->vram_exact ? "" : " (at least; read from the aperture)");
        else
            kinfo("nvidia", "%llu MiB of video memory%s", (unsigned long long)mb,
                  c->vram_exact ? "" : " (at least; read from the aperture)");
    }
    return c->vram_bytes != 0;
}

/* ---------------------------------------------------------------- sensors */

void nv_read_sensors(nv_card_t *c) {
    u32 family = c->chipset & 0x1F0;
    c->temperature_c = -1000;
    c->fan_percent = -1;

    if (family >= 0x130) {
        /* Pascal moved the sensor and changed how it is encoded, and this read
         * had been left at the Kepler address ever since - so every card from
         * the GTX 10 series onwards, this machine's included, was reporting a
         * temperature taken from the wrong register.
         *
         * Degrees are bits 16:8; bits 7:3 below them are a fraction and are
         * dropped rather than rounded, which is what nouveau does and keeps
         * this agreeing with the number a user sees elsewhere. */
        u32 v = nv_rd32(c, NV_THERM_SENSOR_PASCAL);
        int reading = (int)((v & NV_THERM_SENSOR_PASCAL_MASK) >>
                            NV_THERM_SENSOR_PASCAL_SHIFT);
        if (reading > 0 && reading < 130) c->temperature_c = reading;
    } else if (family >= 0x050) {
        /* G84 through Maxwell: the register reads as degrees on its own, with
         * no shift.  Taking bits 23:16 of it, as this used to, produced zero
         * on every one of them - which read as "no sensor" rather than as a
         * fault, and so was never questioned. */
        u32 v = nv_rd32(c, NV_THERM_SENSOR_G84);
        int reading = (int)(v & 0xFF);
        if (reading > 0 && reading < 130) c->temperature_c = reading;
    } else if (family >= 0x040) {
        /* Earlier parts report a raw count that needs the calibration from the
         * ROM to become a temperature.  Without it the value is meaningless,
         * so nothing is reported rather than something wrong. */
        u32 v = nv_rd32(c, NV_THERM_SENSOR_OLD);
        int raw = (int)(v & 0x3FFF);
        if (raw) kdebug("nvidia", "thermal sensor reads %d, uncalibrated", raw);
    }

    u32 duty = nv_rd32(c, NV_THERM_FAN_PWM_DUTY);
    if (duty != 0xFFFFFFFFu) {
        int percent = (int)(duty & 0xFF);
        if (percent <= 100) c->fan_percent = percent;
    }
}

/* -------------------------------------------------------------- the monitors
 *
 * Each connector has a clock and a data line beside it that a monitor answers
 * on.  The driver drives both itself - there is no controller doing it - and
 * the monitor replies with a hundred and twenty-eight bytes describing what it
 * is and what it can display.  That is how a card knows a 4K panel is plugged
 * into the second DisplayPort.
 */
static u32 i2c_port(u8 bus) { return NV_PCRTC_I2C_BASE_OLD + (u32)bus * NV_PCRTC_I2C_STRIDE; }

static void i2c_set(nv_card_t *c, u8 bus, bool scl, bool sda) {
    u32 value = 0;
    /* The lines are open-drain: a zero pulls them low and a one lets them go
     * high, so what is written is the inverse of the level wanted. */
    if (!scl) value |= NV_I2C_SCL_OUT;
    if (!sda) value |= NV_I2C_SDA_OUT;
    nv_wr32(c, i2c_port(bus), value);
    timer_udelay(3);
}

static bool i2c_get_sda(nv_card_t *c, u8 bus) {
    return (nv_rd32(c, i2c_port(bus)) & NV_I2C_SDA_IN) != 0;
}

static void i2c_start(nv_card_t *c, u8 bus) {
    i2c_set(c, bus, true, true);
    i2c_set(c, bus, true, false);
    i2c_set(c, bus, false, false);
}

static void i2c_stop(nv_card_t *c, u8 bus) {
    i2c_set(c, bus, false, false);
    i2c_set(c, bus, true, false);
    i2c_set(c, bus, true, true);
}

/* One byte out, and the acknowledgement the far end pulls low. */
static bool i2c_write_byte(nv_card_t *c, u8 bus, u8 byte) {
    for (int i = 7; i >= 0; i--) {
        bool bit = (byte >> i) & 1;
        i2c_set(c, bus, false, bit);
        i2c_set(c, bus, true, bit);
        i2c_set(c, bus, false, bit);
    }
    /* Release the data line and look for the far end holding it down. */
    i2c_set(c, bus, false, true);
    i2c_set(c, bus, true, true);
    bool acked = !i2c_get_sda(c, bus);
    i2c_set(c, bus, false, true);
    return acked;
}

static u8 i2c_read_byte(nv_card_t *c, u8 bus, bool last) {
    u8 value = 0;
    i2c_set(c, bus, false, true);
    for (int i = 7; i >= 0; i--) {
        i2c_set(c, bus, true, true);
        if (i2c_get_sda(c, bus)) value |= (u8)(1u << i);
        i2c_set(c, bus, false, true);
    }
    /* Acknowledge, unless this was the last byte wanted. */
    i2c_set(c, bus, false, last);
    i2c_set(c, bus, true, last);
    i2c_set(c, bus, false, true);
    return value;
}

#define EDID_ADDRESS 0x50

bool nv_i2c_read_edid(nv_card_t *c, u8 bus, u8 out[128]) {
    if (bus >= 0x0F) return false;

    /* Address the monitor and ask it to start from byte zero. */
    i2c_start(c, bus);
    if (!i2c_write_byte(c, bus, EDID_ADDRESS << 1)) { i2c_stop(c, bus); return false; }
    if (!i2c_write_byte(c, bus, 0x00)) { i2c_stop(c, bus); return false; }
    i2c_stop(c, bus);

    /* Then read it back. */
    i2c_start(c, bus);
    if (!i2c_write_byte(c, bus, (EDID_ADDRESS << 1) | 1)) { i2c_stop(c, bus); return false; }
    for (int i = 0; i < 128; i++) out[i] = i2c_read_byte(c, bus, i == 127);
    i2c_stop(c, bus);

    /* The whole block sums to zero, which is what says a monitor answered
     * rather than the lines floating. */
    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (u8)(sum + out[i]);
    if (sum != 0) return false;

    static const u8 header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    return memcmp(out, header, sizeof header) == 0;
}

/* What a monitor said about itself.  The name and the preferred mode are what
 * a person recognises; the rest of the block is timing detail. */
static void describe_monitor(nv_output_t *out) {
    const u8 *e = out->edid;

    /* The manufacturer is three five-bit letters packed into two bytes. */
    u16 packed = (u16)((e[8] << 8) | e[9]);
    char vendor[4];
    vendor[0] = (char)('A' + ((packed >> 10) & 0x1F) - 1);
    vendor[1] = (char)('A' + ((packed >> 5) & 0x1F) - 1);
    vendor[2] = (char)('A' + (packed & 0x1F) - 1);
    vendor[3] = 0;

    /* The four descriptor blocks; one of them usually holds the model name. */
    strlcpy(out->monitor_name, vendor, sizeof out->monitor_name);
    for (int d = 0; d < 4; d++) {
        const u8 *block = e + 54 + d * 18;
        if (block[0] || block[1] || block[2] || block[4]) continue;
        if (block[3] != 0xFC) continue;                  /* the name block */

        size_t n = 0;
        for (int i = 5; i < 18 && n < sizeof out->monitor_name - 1; i++) {
            if (block[i] == '\n') break;
            out->monitor_name[n++] = (char)block[i];
        }
        out->monitor_name[n] = 0;
        break;
    }

    /* The first detailed descriptor is the panel's own preferred mode. */
    const u8 *d0 = e + 54;
    u32 pixel_clock = (u32)((d0[1] << 8) | d0[0]) * 10000u;
    if (pixel_clock) {
        u16 hactive = (u16)(d0[2] | ((d0[4] & 0xF0) << 4));
        u16 hblank  = (u16)(d0[3] | ((d0[4] & 0x0F) << 8));
        u16 vactive = (u16)(d0[5] | ((d0[7] & 0xF0) << 4));
        u16 vblank  = (u16)(d0[6] | ((d0[7] & 0x0F) << 8));

        out->width = hactive;
        out->height = vactive;

        u32 htotal = (u32)hactive + hblank;
        u32 vtotal = (u32)vactive + vblank;
        if (htotal && vtotal)
            out->refresh_hz = (u16)((pixel_clock + htotal * vtotal / 2) / (htotal * vtotal));
    }
}

int nv_probe_outputs(nv_card_t *c) {
    int found = 0;

    /* Every byte of a monitor's identification arrives because this driver
     * drove two wires up and down itself.  On a card whose display the
     * firmware has already brought up and is showing a picture through, that
     * is not a read. */
    if (!c->modelled && !nv_writes_allowed()) {
        kinfo("nvidia", "not asking the monitors what they are: doing that "
                        "means driving their wires, and the firmware is using "
                        "this display right now.  %d connector(s) were still "
                        "read out of the card's own ROM.  Add \"gpuwrite\" to "
                        "the command line to probe them.", c->outputs);
        return 0;
    }

    for (int i = 0; i < c->outputs; i++) {
        nv_output_t *out = &c->output[i];
        out->monitor_present = false;
        out->edid_valid = false;

        if (out->i2c_bus >= 0x0F) continue;
        if (!nv_i2c_read_edid(c, out->i2c_bus, out->edid)) continue;

        out->edid_valid = true;
        out->monitor_present = true;
        describe_monitor(out);
        found++;

        kinfo("nvidia", "%s: \"%s\", %ux%u at %u Hz",
              nv_output_type_name(out->type), out->monitor_name,
              out->width, out->height, out->refresh_hz);
    }

    if (!found && c->outputs) {
        kinfo("nvidia", "no monitor answered on any of the %d connector(s)",
              c->outputs);
        if (c->modelled) {
            /* A bit-banged bus gives no other clue about where a transfer
             * stopped, so the model keeps a count of what it actually saw. */
            void nv_model_i2c_report(int *edges, int *starts, int *stops, u8 *address);
            int edges = 0, starts = 0, stops = 0;
            u8 address = 0;
            nv_model_i2c_report(&edges, &starts, &stops, &address);
            kdebug("nvidia", "the bus saw %d edge(s), %d start(s), %d stop(s), "
                             "last address byte %02x", edges, starts, stops, address);
        }
    }
    return found;
}

/* ------------------------------------------------------------- bringing up */

static nv_card_t cards[NV_MAX_CARDS];
static int card_count;

nv_card_t *nv_card(int index) {
    return (index >= 0 && index < card_count) ? &cards[index] : NULL;
}
int nv_card_count(void) { return card_count; }

bool nv_display_init(nv_card_t *c);
bool nv_gsp_init(nv_card_t *c);

/* Each step says what it is about to do before doing it.
 *
 * On a machine this driver has never seen there is no serial cable and no
 * second computer watching - if a step stops the machine, the only evidence
 * is whatever is still on the screen.  Announcing the step first rather than
 * reporting it afterwards means the last line standing names the thing that
 * did not come back, which is the difference between a photograph that is
 * useful and one that is not. */
static void step(const char *what) {
    kinfo("nvidia", "about to: %s", what);
}

static void bring_up(nv_card_t *c) {
    step("read the card's name out of its own registers");
    if (!nv_identify(c)) return;
    nv_telemetry_probe_engines(c); /* requires the identified chipset */

    /* From Turing onward there is a second register carrying the same
     * identity, and it is the one NVIDIA's driver reads.  Two views of the
     * same fuses agreeing is worth confirming before anything is built on
     * either; when they disagree, that is said out loud rather than resolved
     * by preferring one silently. */
    step("confirm the name against the card's other identity register");
    nv_read_boot42(c);

    step("find out how much memory the card has");
    nv_read_vram_size(c);

    step("read the card's own description out of its ROM");
    if (nv_read_vbios(c)) {
        nv_parse_vbios(c);
        /* The copy in video memory is only reachable once the size is known,
         * so a card whose ROM window is disabled gets a second chance here. */
        if (!c->vbios_valid && c->vram_bytes && nv_read_vbios(c))
            nv_parse_vbios(c);
    }

    step("look at what is plugged into the card");
    nv_probe_outputs(c);

    step("read the card's temperature and fan");
    nv_read_sensors(c);

    if (c->temperature_c > -1000)
        kinfo("nvidia", "%d degrees%s", c->temperature_c,
              c->fan_percent >= 0 ? "" : "");

    if (driveable_by_registers(c)) {
        /* Up to Pascal the display engine answers to registers, so it can be
         * driven from here. */
        step("bring up a display engine that answers to registers");
        nv_display_init(c);
    } else {
        /* Turing and newer are owned as one stack by NVIDIA's matching host
         * RM + GSP-RM + NVKMS.  Do not start the former hand-written GSP path
         * first: two independent RM owners corrupt the firmware queues and
         * make every subsequent result meaningless. */
        bool nvrm_start_gpu(u8,u8,u8,void *);
        if (!cmdline_has("gpustart")) {
            kinfo("nvidia", "modern NVIDIA takeover skipped outside the named GPU-test boot entry; firmware scanout remains owned by firmware");
            goto modern_done;
        }
        step("start NVIDIA's matching host resource manager");
        if (nvrm_start_gpu(c->pci_bus,c->pci_slot,c->pci_func,(void *)c->regs)) {
            step("load NVIDIA's matching display state machine");
            if (!nvkms_host_attach_full(c))
                kerr("nvidia","host RM started, but NVKMS refused module load");
        } else {
            kerr("nvidia","host RM did not initialize; custom GSP fallback is disabled to preserve single ownership");
        }
        step("modern GPU ownership attempt complete");
modern_done:;
    }

    /* One line saying what was found, at the end where it can be read.
     *
     * On a machine with no serial cable the only record is the screen, and a
     * screen holds a page.  Everything above is worth having when it can be
     * scrolled back to; this is the line worth photographing if only one can
     * be. */
    nv_report_modern(c);

    if (!c->modelled && !nv_writes_allowed() && nv_writes_refused())
        kinfo("nvidia", "%u register write(s) were refused on the way here; "
                        "nothing this driver did could have changed what is on "
                        "the screen", nv_writes_refused());

    kinfo("nvidia", "found: %s %s, chip %03x rev %u.%u, %llu MiB, %d "
                    "connector(s), BIOS %s",
          c->architecture ? c->architecture : "unknown architecture",
          c->codename ? c->codename : "unnamed",
          c->chipset, c->revision >> 4, c->revision & 0xF,
          (unsigned long long)(c->vram_bytes >> 20), c->outputs,
          c->vbios_version[0] ? c->vbios_version : "not read");
}

bool cmdline_has(const char *key);

/* What discovery found, kept for the shutdown verdict.
 *
 * "no NVIDIA card is being driven" was true and useless: it could not say
 * whether the bus had no such card, whether one was there and skipped, or
 * whether its registers would not map.  Those are three different faults and
 * the boot lines that would have separated them are long gone from the log
 * ring by the time anybody collects it. */
static int seen_display, seen_nvidia, no_regs;

void nvidia_discovery_report(void) {
    kinfo("verdict", "GPU: the bus has %d display adapter(s), %d of them "
                     "NVIDIA; %d had no mappable register window; %d card(s) "
                     "are being driven",
          seen_display, seen_nvidia, no_regs, card_count);
    if (seen_nvidia && !card_count)
        kwarn("verdict", "GPU: an NVIDIA card is present and this driver did "
                         "not take it");
    kinfo("verdict", "GPU: writes to the card are %s; %u were refused",
          nv_writes_allowed() ? "allowed" : "REFUSED (needs `gpu start` or the "
                                            "gpuwrite option)",
          nv_writes_refused());
}

void nvidia_driver_init(void) {
    pci_dev_t *pci = NULL;

    /* Read-only unless told otherwise.  See nv_wr32 for why that is the
     * default and not the exception. */
    nv_allow_writes(cmdline_has("gpuwrite"));
    if (nv_writes_allowed())
        kwarn("nvidia", "asked to write to the graphics card's registers.  "
                        "This driver has never been run against one; if the "
                        "screen goes out, boot again without \"gpuwrite\"");
    else
        kinfo("nvidia", "reading the card only, not writing to it - which is "
                        "everything except probing monitors and reading the "
                        "ROM out of the chip itself");

    while ((pci = pci_find(0x03, 0xFF, 0xFF, pci)) != NULL) {
        seen_display++;
        if (pci->vendor != 0x10DE) continue;              /* NVIDIA */
        seen_nvidia++;
        if (card_count >= NV_MAX_CARDS) break;

        nv_card_t *c = &cards[card_count];
        memset(c, 0, sizeof *c);
        c->index = card_count;
        c->pci_bus = pci->bus; c->pci_slot = pci->slot; c->pci_func = pci->func;

        pci_enable_memory(pci);
        pci_enable_bus_master(pci);

        /* The first region is the register window, the second the aperture
         * onto video memory. */
        if (pci->bar[0] && !pci->bar_is_io[0]) {
            size_t len = pci->bar_size[0] ? (size_t)pci->bar_size[0] : 0x1000000;
            if (len > 0x1000000) len = 0x1000000;
            c->regs = vmm_map_mmio(pci->bar[0], len);
            c->regs_size = len;
        }
        if (pci->bar[1] && !pci->bar_is_io[1]) {
            c->vram_base = pci->bar[1];
            c->vram_aperture = (size_t)pci->bar_size[1];
        }

        if (!c->regs) {
            no_regs++;
            kwarn("nvidia", "the card's register window could not be mapped");
            continue;
        }

        /* Which engines this card has, so a monitor can tell an engine that
         * is idle from one that is not fitted. */

        /* And offer its drawing engine to the system, which is what makes the
         * shell ask this card rather than only ever asking a VMware one. */
        nv_accel_init(c);

        card_count++;
        bring_up(c);
    }
}

/* Bring the driver up against a model of a card, so that everything above the
 * register window is exercised on a machine that has no NVIDIA hardware in
 * it. */
volatile u8 *nv_model_attach(size_t *size_out);

bool nvidia_attach_model(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return true;
    if (card_count >= NV_MAX_CARDS) return false;

    size_t size = 0;
    volatile u8 *regs = nv_model_attach(&size);
    if (!regs) return false;

    nv_card_t *c = &cards[card_count];
    memset(c, 0, sizeof *c);
    c->index = card_count;
    card_count++;
    c->regs = regs;
    c->regs_size = size;
    c->modelled = true;
    c->vram_aperture = 256u << 20;

    bring_up(c);
    return true;
}

/* Which card sits at one address on the bus.
 *
 * The system's list of graphics cards is built from the bus and knows nothing
 * about this driver; this is how a caller holding an entry from that list
 * reaches what this driver knows about the same card. */
nv_card_t *nv_card_for_pci(u8 bus, u8 slot, u8 func) {
    for (int i = 0; i < card_count; i++)
        if (!cards[i].modelled &&
            cards[i].pci_bus == bus && cards[i].pci_slot == slot &&
            cards[i].pci_func == func)
            return &cards[i];
    return NULL;
}

/* What to call this driver, and when it was built.
 *
 * A version and a date are what somebody looks for when deciding whether the
 * thing in front of them is current, so they are the build's own - not a
 * number invented to look like a released driver's. */
const char *nv_driver_version(void) { return "KestrelOS nv " NV_DRIVER_VERSION; }
const char *nv_driver_date(void)    { return __DATE__; }

nv_card_t *nv_model_card(void) {
    for (int i = 0; i < card_count; i++)
        if (cards[i].modelled) return &cards[i];
    return NULL;
}

/* ------------------------------------------------------------------- tests
 *
 * Drive the whole driver against a model of a card and check what came back
 * against what the model was built to be.  Returns the number of failures.
 */
u32       nv_model_boot0(void);
u64       nv_model_vram(void);
int       nv_model_temperature(void);
const u8 *nv_model_edid(void);
u8        nv_model_monitor_bus(void);
const u32 *nv_disp_push_buffer(u32 *words_out);

int nvidia_drive_test(void) {
    int failures = 0;

    if (!nvidia_attach_model()) {
        kwarn("nv-test", "the model could not be brought up");
        return 1;
    }

    nv_card_t *c = nv_model_card();
    if (!c) {
        kerr("nv-test", "the model registered no card");
        return 1;
    }

    /* The chip identified itself out of its own first register. */
    if (c->boot0 != nv_model_boot0()) {
        kerr("nv-test", "PMC_BOOT_0 read back as %08x, expected %08x",
             c->boot0, nv_model_boot0());
        failures++;
    } else if (c->chipset != 0x134) {
        kerr("nv-test", "the chip came out as %03x, expected 134", c->chipset);
        failures++;
    } else {
        kinfo("nv-test", "the card named itself: %s (%s), revision %u.%u",
              c->architecture, c->codename, c->revision >> 4, c->revision & 0xF);
    }

    /* Its memory, from the register that holds it as a mantissa and a scale. */
    if (c->vram_bytes != nv_model_vram() || !c->vram_exact) {
        kerr("nv-test", "video memory came out as %llu MiB, expected %llu",
             (unsigned long long)(c->vram_bytes >> 20),
             (unsigned long long)(nv_model_vram() >> 20));
        failures++;
    }

    /* The ROM: found, its length read, its version string recovered. */
    if (!c->vbios_valid) {
        kerr("nv-test", "the card's own description was not read");
        failures++;
    } else if (strcmp(c->vbios_version, "86.04.52.00")) {
        kerr("nv-test", "the video BIOS version came out as \"%s\"",
             c->vbios_version);
        failures++;
    } else {
        kinfo("nv-test", "video BIOS %s read from %s, %zu bytes",
              c->vbios_version, c->vbios_source, c->vbios_size);
    }

    /* The connector table: four outputs, of the right kinds, on the right
     * buses. */
    if (c->outputs != 4) {
        kerr("nv-test", "%d connector(s) found, expected 4", c->outputs);
        failures++;
    } else if (c->output[0].type != NV_OUTPUT_ANALOG ||
               c->output[1].type != NV_OUTPUT_TMDS ||
               c->output[2].type != NV_OUTPUT_DP ||
               c->output[2].i2c_bus != nv_model_monitor_bus()) {
        kerr("nv-test", "the connector table was misread");
        failures++;
    } else {
        kinfo("nv-test", "4 connectors: VGA, HDMI and two DisplayPorts");
    }

    /* The monitor.  This is the one that exercises the most: the driver drove
     * the two wires itself, one clock edge at a time, and something on the far
     * end answered. */
    int with_monitor = 0;
    const nv_output_t *panel = NULL;
    for (int i = 0; i < c->outputs; i++)
        if (c->output[i].monitor_present) { with_monitor++; panel = &c->output[i]; }

    if (with_monitor != 1 || !panel) {
        kerr("nv-test", "%d monitor(s) answered, expected 1", with_monitor);
        failures++;
    } else if (memcmp(panel->edid, nv_model_edid(), 128)) {
        kerr("nv-test", "the monitor's identification came back altered");
        failures++;
    } else if (panel->width != 3840 || panel->height != 2160 ||
               panel->refresh_hz < 59 || panel->refresh_hz > 61) {
        kerr("nv-test", "the monitor's mode came out as %ux%u at %u Hz",
             panel->width, panel->height, panel->refresh_hz);
        failures++;
    } else if (strcmp(panel->monitor_name, "Kestrel UHD")) {
        kerr("nv-test", "the monitor's name came out as \"%s\"", panel->monitor_name);
        failures++;
    } else {
        kinfo("nv-test", "a monitor answered over the two wires the driver drove "
                         "itself: \"%s\", %ux%u at %u Hz",
              panel->monitor_name, panel->width, panel->height, panel->refresh_hz);
    }

    /* The temperature, out of the sensor. */
    if (c->temperature_c != nv_model_temperature()) {
        kerr("nv-test", "the temperature came out as %d, expected %d",
             c->temperature_c, nv_model_temperature());
        failures++;
    } else {
        kinfo("nv-test", "the thermal sensor reads %d degrees", c->temperature_c);
    }

    /* And the display channel the driver built for the mode it found. */
    u32 words = 0;
    const u32 *push = nv_disp_push_buffer(&words);
    if (!words) {
        kerr("nv-test", "the display engine was given nothing to do");
        failures++;
    } else {
        /* The last thing in it has to be the update that applies the rest;
         * without it a card would have taken every method and shown none. */
        bool ends_with_update = false;
        for (u32 i = 0; i + 1 < words; i += 2) {
            u32 method = (push[i] & 0x1FFF) << 2;
            if (i + 2 == words && method == 0x0080) ends_with_update = true;
        }
        if (!ends_with_update) {
            kerr("nv-test", "the mode change was built without the update that "
                            "applies it");
            failures++;
        } else {
            kinfo("nv-test", "a mode change was built into the display channel: "
                             "%u words, ending with the update", words);
        }
    }

    if (!failures)
        kinfo("nv-test", "the NVIDIA driver reads the card, its ROM, its "
                         "connectors, its monitor and its sensors correctly");
    /* And the part every modern card is built on: bringing one of its small
     * processors up.  From Turing onward this is the only way in - the display
     * engine and the graphics engine both sit behind one. */
    failures += nv_falcon_selftest();

    /* Then the layer above it: once that processor is running, the card is
     * driven by leaving messages in memory both sides can see. */
    failures += nv_gsp_queue_selftest();

    /* And the two messages already waiting in that ring when R570 starts:
     * without them GSP-RM cannot create its GPU object even after a successful
     * chain-of-trust boot. */
    failures += nv_gsp_init_rpc_selftest();

    /* And the part between the card and the monitor.  Every output on a card
     * from the last fifteen years is DisplayPort, and a DisplayPort monitor
     * shows nothing at all until the link has been negotiated. */
    failures += nv_dp_selftest();

    /* And the display engine as it has been since Volta - which is the shape
     * every RTX card has, including the 50 series. */
    failures += nv_disp_modern_selftest();

    /* And the layer above the message rings: the object tree and the commands
     * on it, which on Ada and Blackwell is the only thing that reaches the
     * engines at all. */
    failures += nv_rm_selftest();

    /* And the last thing a driver is for: giving the engines work and being
     * told when it is done. */
    failures += nv_fifo_selftest();

    /* The compute dispatch: build a grid launch of the compositing shader and
     * check a model reads the same launch back out of it.  This is the path #2
     * (GPU renders the OS) takes - a compute shader compositing the desktop -
     * short of the channel a real GSP provides. */
    failures += nv_compute_selftest();

    /* And what the card is doing while it does it, which is what a system
     * monitor shows and what tells somebody whether the card is working at
     * all. */
    failures += nv_telemetry_selftest();

    /* The original local tree also runs a self-test against a device ROM.
     * That device-specific fixture is omitted from this public snapshot. */

    /* And the co-processor firmware somebody has actually supplied, read as
     * the driver would read it. */
    int nv_gsp_firmware_selftest(void);
    failures += nv_gsp_firmware_selftest();

    /* And underneath all of it: the tables that turn an address the driver
     * knows into one the card can reach. */
    failures += nv_vmm_selftest();

    /* And what the work says when it is three-dimensional: the state a draw
     * needs, in the order it has to be set. */
    failures += nv_3d_selftest();

    /* And the part of the driver that identifies a card for the device list,
     * which is a different file with a different register window - and the one
     * that faulted the first time it met real silicon. */
    int nvidia_identify_selftest(void);
    failures += nvidia_identify_selftest();

    /* And the newest card there is, which is a different chip from the one
     * everything above ran against: a GB203, the die in an RTX 5070 Ti.  What
     * differs there is identity, memory sizing and class numbers, so that is
     * what gets driven. */
    failures += nv_blackwell_selftest();

    /* And how that card's co-processor is actually started, which is not the
     * way the rest of this driver starts one: the message goes to the card's
     * security processor and it does the starting.  Driven against a model of
     * that processor, because the alternative is finding out on a card that is
     * displaying a picture. */
    failures += nv_fsp_selftest();

    /* And the arguments the FSP route hands the co-processor after it starts:
     * the boot params, WPR meta, radix3 and libos regions.  Their layout is
     * checked against the firmware's, and a staging is assembled from a small
     * synthetic image to prove the pieces link together. */
    failures += nv_gsp_boot_selftest();

    return failures;
}
