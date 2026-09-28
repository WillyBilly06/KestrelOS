/* amd_atom.c - reading the card's own description of itself.
 *
 * AMD's video BIOS is not a table of numbers the way NVIDIA's is.  It is a
 * program: a bytecode interpreter's instruction stream, with the command
 * tables meant to be executed and the data tables meant to be read.  The whole
 * thing is called ATOM, and it has been the same shape since 2006.
 *
 * A driver that wanted to change a display mode the way the card's own
 * firmware does would have to implement that interpreter.  A driver that wants
 * to know what the card is, what memory it has and what connectors are on the
 * bracket only has to read the data tables - which is what this does, because
 * that is what has to be right before any pixel can be correct.
 *
 * The chain is: find the image, check it is an ATOM image and not whatever else
 * was in memory, follow the pointer at 0x48 to the ROM header, follow the
 * pointer at 0x20 in that to the master data table, index into it, and there
 * are the tables.  Each step is a pointer read out of an image the driver did
 * not write, so each step is bounds-checked - a video BIOS is the first
 * untrusted input a graphics driver ever handles.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "amd.h"

#define VBIOS_MAX (64 * 1024)

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

/* ------------------------------------------------------------ finding it
 *
 * Two places it can be.  The card's own firmware leaves a copy at the bottom
 * of video memory during power-on, which is the one that works even when the
 * expansion ROM has been switched off by the machine's own firmware - and on a
 * machine that booted through UEFI, it usually has been.
 */
static bool looks_like_atom(const u8 *image, size_t size) {
    if (size < 0x60) return false;
    if (rd16(image) != AMD_ATOM_BIOS_MAGIC) return false;

    u16 header_at = rd16(image + AMD_ATOM_ROM_TABLE_PTR);
    if (header_at + 0x30 > size) return false;

    return memcmp(image + header_at + AMD_ATOM_ROM_MAGIC_PTR,
                  AMD_ATOM_ROM_MAGIC, 4) == 0;
}

bool amd_read_vbios(amd_card_t *c) {
    static u8 image[VBIOS_MAX];

    c->vbios = NULL;
    c->vbios_size = 0;
    c->vbios_valid = false;
    c->vbios_source = NULL;

    /* The copy in video memory.  Read through the index/data pair rather than
     * an aperture, because at this point in bringing a card up there may not be
     * an aperture yet. */
    if (c->vram_bytes) {
        amd_read_vram(c, 0, image, VBIOS_MAX);
        if (looks_like_atom(image, VBIOS_MAX)) {
            c->vbios = image;
            c->vbios_size = VBIOS_MAX;
            c->vbios_valid = true;
            c->vbios_source = "the copy in video memory";
            return true;
        }
    }

    kwarn("amd", "no ATOM image found; the card cannot describe itself");
    return false;
}

/* ------------------------------------------------------------ the tables */

bool amd_parse_vbios(amd_card_t *c) {
    if (!c->vbios_valid) return false;

    const u8 *image = c->vbios;
    size_t size = c->vbios_size;

    u16 header_at = rd16(image + AMD_ATOM_ROM_TABLE_PTR);
    if (header_at + 0x30 > size) return false;

    const u8 *header = image + header_at;

    /* Where the two lists of tables are.  A driver only needs the data one,
     * but the command one is recorded because its absence is how a truncated
     * image announces itself. */
    c->atom_cmd_table = rd16(header + AMD_ATOM_ROM_CMD_PTR);
    c->atom_data_table = rd16(header + AMD_ATOM_ROM_DATA_PTR);

    if (c->atom_data_table == 0 || c->atom_data_table + 4 > size) {
        kwarn("amd", "the ATOM image has no data tables");
        return false;
    }

    /* The version string, which is a plain string sitting where the header's
     * message pointer says. */
    u16 message_at = rd16(header + AMD_ATOM_ROM_MSG_PTR);
    c->vbios_version[0] = 0;
    if (message_at && message_at < size) {
        size_t out = 0;
        for (size_t i = message_at; i < size && out + 1 < sizeof c->vbios_version; i++) {
            u8 ch = image[i];
            if (ch == 0 || ch == 0x0D || ch == 0x0A) break;
            if (ch < 0x20 || ch > 0x7E) continue;
            c->vbios_version[out++] = (char)ch;
        }
        c->vbios_version[out] = 0;
    }

    if (!c->vbios_version[0])
        memcpy(c->vbios_version, "(no version string)", 20);

    return true;
}

/* One entry of the master data table.  The table is a common header followed
 * by an array of sixteen-bit offsets, so the index is a position in that
 * array - which is a position in a C struct, and so fixed forever. */
bool amd_atom_data_table(amd_card_t *c, int index, u16 *offset, u16 *size_out) {
    if (!c->vbios_valid || !c->atom_data_table) return false;
    if (index < 0 || index > 63) return false;

    const u8 *image = c->vbios;
    size_t size = c->vbios_size;

    /* The master table's own header says how long it is, which bounds how many
     * entries it has - a driver that indexes past that reads whatever follows
     * and calls it a table offset. */
    if ((size_t)c->atom_data_table + 4 > size) return false;
    u16 master_size = rd16(image + c->atom_data_table);
    u32 entry_at = (u32)c->atom_data_table + 4 + (u32)index * 2;

    if (entry_at + 2 > (u32)c->atom_data_table + master_size) return false;
    if (entry_at + 2 > size) return false;

    u16 at = rd16(image + entry_at);
    if (at == 0) return false;                  /* this card has no such table */
    if ((size_t)at + 4 > size) return false;

    u16 table_size = rd16(image + at);
    if (table_size < 4 || (size_t)at + table_size > size) {
        kwarn("amd", "data table %d claims %u bytes at %04x, past the end of a "
                     "%u byte image", index, table_size, at, (unsigned)size);
        return false;
    }

    if (offset) *offset = at;
    if (size_out) *size_out = table_size;
    return true;
}

/* --------------------------------------------------------- what is plugged in
 *
 * The object table describes the card the way a person would: there is a
 * connector of this kind, it is the second of its kind, and the two wires
 * beside it are driven by this set of pins.  Walking it is how a driver knows
 * a card has three DisplayPorts and an HDMI rather than assuming.
 */
static const char *connector_name(u8 id) {
    switch (id) {
    case AMD_CONNECTOR_VGA:               return "VGA";
    case AMD_CONNECTOR_SINGLE_LINK_DVI_I: return "DVI-I";
    case AMD_CONNECTOR_DUAL_LINK_DVI_I:   return "DVI-I (dual link)";
    case AMD_CONNECTOR_SINGLE_LINK_DVI_D: return "DVI-D";
    case AMD_CONNECTOR_DUAL_LINK_DVI_D:   return "DVI-D (dual link)";
    case AMD_CONNECTOR_HDMI_TYPE_A:       return "HDMI";
    case AMD_CONNECTOR_HDMI_TYPE_B:       return "HDMI type B";
    case AMD_CONNECTOR_LVDS:              return "LVDS panel";
    case AMD_CONNECTOR_DISPLAYPORT:       return "DisplayPort";
    case AMD_CONNECTOR_eDP:               return "eDP panel";
    case AMD_CONNECTOR_USBC:              return "USB-C";
    default:                              return "unknown connector";
    }
}

/* GPIO_I2C_Info: one 27-byte entry per bus, saying which registers and which
 * bits within them carry each of the two wires. */
static int parse_i2c_table(amd_card_t *c) {
    u16 at = 0, size = 0;
    c->i2c_buses = 0;

    if (!amd_atom_data_table(c, AMD_ATOM_TABLE_GPIO_I2C_INFO, &at, &size))
        return 0;

    const u8 *table = c->vbios + at;
    u32 entries = (size - 4) / (u32)sizeof(amd_i2c_assignment_t);
    if (entries > AMD_MAX_I2C_BUSES) entries = AMD_MAX_I2C_BUSES;

    for (u32 i = 0; i < entries; i++) {
        memcpy(&c->i2c[i], table + 4 + i * sizeof(amd_i2c_assignment_t),
               sizeof(amd_i2c_assignment_t));
        c->i2c_buses++;
    }
    return c->i2c_buses;
}

int amd_parse_connectors(amd_card_t *c) {
    u16 at = 0, size = 0;

    c->connectors = 0;
    parse_i2c_table(c);

    if (!amd_atom_data_table(c, AMD_ATOM_TABLE_OBJECT_HEADER, &at, &size)) {
        kwarn("amd", "the ATOM image has no object table, so what is on the "
                     "bracket cannot be known");
        return 0;
    }

    const u8 *image = c->vbios;
    const u8 *header = image + at;

    /* ATOM_OBJECT_HEADER: a four-byte common header, then usDeviceSupport,
     * then an offset per object table. */
    u16 connector_table = rd16(header + 6);
    if (!connector_table || (size_t)connector_table + 4 > c->vbios_size) {
        kwarn("amd", "the object table has no connector list");
        return 0;
    }

    const u8 *table = image + connector_table;
    u8 count = table[0];

    for (u8 i = 0; i < count && c->connectors < AMD_MAX_CONNECTORS; i++) {
        u32 object_at = (u32)connector_table + 4 + (u32)i * 8;
        if (object_at + 8 > c->vbios_size) break;

        const u8 *object = image + object_at;
        u16 object_id = rd16(object + 0);
        u16 record_at = rd16(object + 4);

        u16 type = (object_id & AMD_OBJECT_TYPE_MASK) >> AMD_OBJECT_TYPE_SHIFT;
        if (type != AMD_OBJECT_TYPE_CONNECTOR) continue;

        amd_connector_t *conn = &c->connector[c->connectors];
        memset(conn, 0, sizeof *conn);
        conn->object_id = object_id;
        conn->connector_id = (u8)(object_id & AMD_OBJECT_ID_MASK);
        conn->enum_id = (u8)((object_id & AMD_ENUM_ID_MASK) >> AMD_ENUM_ID_SHIFT);
        conn->name = connector_name(conn->connector_id);
        conn->has_i2c = false;

        /* The records hanging off it.  Each is a type and a length, and the
         * list ends with a type of 0xFF - so the walk is bounded by the image
         * and by a record claiming a length of zero, which would otherwise
         * spin here forever. */
        if (record_at && (size_t)record_at + 2 <= c->vbios_size) {
            u32 walk = record_at;
            for (int guard = 0; guard < 64; guard++) {
                if (walk + 2 > c->vbios_size) break;
                u8 record_type = image[walk];
                u8 record_size = image[walk + 1];

                if (record_type == AMD_ATOM_RECORD_END_TYPE) break;
                if (record_size < 2) break;
                if (walk + record_size > c->vbios_size) break;

                if (record_type == AMD_ATOM_I2C_RECORD_TYPE && record_size >= 4) {
                    /* sucI2cId is a byte whose low bits are the line number. */
                    conn->has_i2c = true;
                    conn->i2c_line = (u8)(image[walk + 2] & 0x0F);
                }

                walk += record_size;
            }
        }

        c->connectors++;
    }

    return c->connectors;
}

/* ------------------------------------------------------------- the two wires
 *
 * AMD does not put an I2C controller in the path.  The driver drives the pins
 * itself: the lines are open-drain, so a device pulls one low by enabling its
 * output driver and releases it by disabling it.  Nobody ever drives a line
 * high - the pull-up resistor does that - and a driver that tries produces a
 * bus where two devices fight and neither is heard.
 *
 * Which registers and which bits carry the lines is not knowable in advance;
 * it comes from GPIO_I2C_Info above, which is why this takes a line number and
 * looks it up.
 */
static void line_set(amd_card_t *c, u16 en_reg, u8 en_shift, bool low) {
    u32 offset = (u32)en_reg * 4;
    u32 value = amd_rd32(c, offset);
    if (low) value |= (1u << en_shift);       /* drive it down */
    else     value &= ~(1u << en_shift);      /* let it float up */
    amd_wr32(c, offset, value);
}

static bool line_get(amd_card_t *c, u16 y_reg, u8 y_shift) {
    u32 value = amd_rd32(c, (u32)y_reg * 4);
    return (value >> y_shift) & 1;
}

typedef struct {
    amd_card_t *card;
    const amd_i2c_assignment_t *bus;
} wire_t;

static void scl(wire_t *w, bool high) {
    line_set(w->card, w->bus->clk_en_reg, w->bus->clk_en_shift, !high);
    timer_udelay(3);
}

static void sda(wire_t *w, bool high) {
    line_set(w->card, w->bus->data_en_reg, w->bus->data_en_shift, !high);
    timer_udelay(3);
}

static bool sda_read(wire_t *w) {
    return line_get(w->card, w->bus->data_y_reg, w->bus->data_y_shift);
}

static void i2c_start(wire_t *w) {
    sda(w, true); scl(w, true);
    sda(w, false);              /* data falls while the clock is high */
    scl(w, false);
}

static void i2c_stop(wire_t *w) {
    sda(w, false); scl(w, true);
    sda(w, true);               /* data rises while the clock is high */
}

static bool i2c_write_byte(wire_t *w, u8 byte) {
    for (int bit = 7; bit >= 0; bit--) {
        sda(w, (byte >> bit) & 1);
        scl(w, true);
        scl(w, false);
    }
    /* The ninth clock is the other side's answer: it pulls the line down to
     * say it heard. */
    sda(w, true);
    scl(w, true);
    bool acked = !sda_read(w);
    scl(w, false);
    return acked;
}

static u8 i2c_read_byte(wire_t *w, bool ack) {
    u8 byte = 0;
    sda(w, true);
    for (int bit = 7; bit >= 0; bit--) {
        scl(w, true);
        if (sda_read(w)) byte |= (u8)(1 << bit);
        scl(w, false);
    }
    sda(w, !ack);
    scl(w, true);
    scl(w, false);
    sda(w, true);
    return byte;
}

bool amd_i2c_read_edid(amd_card_t *c, u8 line, u8 out[128]) {
    if (line >= c->i2c_buses) return false;

    wire_t w = { c, &c->i2c[line] };

    /* The masks say which bits of these registers belong to the bus at all.
     * Setting them is what connects the pins to the pads; without it the lines
     * are driven into nothing. */
    u32 mask_offset = (u32)w.bus->clk_mask_reg * 4;
    amd_wr32(c, mask_offset, amd_rd32(c, mask_offset) |
                             (1u << w.bus->clk_mask_shift));
    mask_offset = (u32)w.bus->data_mask_reg * 4;
    amd_wr32(c, mask_offset, amd_rd32(c, mask_offset) |
                             (1u << w.bus->data_mask_shift));

    /* Both lines released, so anything already on the bus can finish. */
    sda(&w, true); scl(&w, true);

    i2c_start(&w);
    if (!i2c_write_byte(&w, 0xA0)) { i2c_stop(&w); return false; }
    if (!i2c_write_byte(&w, 0x00)) { i2c_stop(&w); return false; }

    /* A repeated start rather than a stop, so the address pointer set above
     * survives into the read. */
    i2c_start(&w);
    if (!i2c_write_byte(&w, 0xA1)) { i2c_stop(&w); return false; }

    for (int i = 0; i < 128; i++)
        out[i] = i2c_read_byte(&w, i < 127);

    i2c_stop(&w);

    static const u8 edid_header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (memcmp(out, edid_header, 8)) return false;

    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (u8)(sum + out[i]);
    return sum == 0;
}

/* What the block says, once it is in hand. */
static void decode_edid(amd_connector_t *conn) {
    const u8 *e = conn->edid;

    for (int d = 0; d < 4; d++) {
        const u8 *desc = e + 54 + d * 18;

        /* A descriptor beginning with two zero bytes is a text block rather
         * than a timing; 0xFC in the third says the text is the name. */
        if (desc[0] == 0 && desc[1] == 0) {
            if (desc[3] == 0xFC) {
                size_t out = 0;
                for (int i = 5; i < 18 && out + 1 < sizeof conn->monitor_name; i++) {
                    if (desc[i] == 0x0A) break;
                    conn->monitor_name[out++] = (char)desc[i];
                }
                while (out && conn->monitor_name[out - 1] == ' ') out--;
                conn->monitor_name[out] = 0;
            }
            continue;
        }

        /* The first real timing block is the preferred mode. */
        if (!conn->width) {
            u32 pixel_khz = (u32)(desc[0] | (desc[1] << 8)) * 10;
            u32 h_active = desc[2] | ((u32)(desc[4] & 0xF0) << 4);
            u32 h_blank = desc[3] | ((u32)(desc[4] & 0x0F) << 8);
            u32 v_active = desc[5] | ((u32)(desc[7] & 0xF0) << 4);
            u32 v_blank = desc[6] | ((u32)(desc[7] & 0x0F) << 8);

            u32 h_total = h_active + h_blank;
            u32 v_total = v_active + v_blank;

            conn->width = (u16)h_active;
            conn->height = (u16)v_active;
            if (h_total && v_total)
                conn->refresh_hz = (u16)((pixel_khz * 1000 + h_total * v_total / 2) /
                                         (h_total * v_total));
        }
    }
}

int amd_probe_monitors(amd_card_t *c) {
    int found = 0;

    for (int i = 0; i < c->connectors; i++) {
        amd_connector_t *conn = &c->connector[i];
        if (!conn->has_i2c) continue;

        if (!amd_i2c_read_edid(c, conn->i2c_line, conn->edid)) continue;

        conn->edid_valid = true;
        conn->monitor_present = true;
        decode_edid(conn);
        found++;
    }

    return found;
}
