/* usbhid.c - USB keyboards, mice, tablets and touch devices.
 *
 * A HID device describes its own report format in a report descriptor: a byte
 * code that says which usages appear in a report, how many bits each takes and
 * whether the value is absolute or relative.  Parsing it is more work than
 * reading the fixed boot-protocol layout, but boot protocol only covers plain
 * keyboards and two-button mice.  Anything else - a tablet, a touchscreen, a
 * mouse with a high-resolution wheel, a keyboard with more than six keys down -
 * either reports nothing useful in boot mode or refuses it outright.  Parsing
 * the descriptor is what makes the driver work with the device the user
 * actually owns rather than the device the specification imagines.
 *
 * Boot protocol is still the fallback: if a descriptor will not parse, a device
 * that claims the boot subclass gets the fixed layout instead.
 */
#include "kernel.h"
#include "mm.h"
#include "input.h"
#include "time.h"
#include "klog.h"
#include "usb.h"

/* Injected into the console line discipline and the /dev/input queue. */
void input_inject_key(u16 code, u8 mods, bool pressed);
void mouse_inject_relative(int dx, int dy, int wheel, u8 buttons);
void mouse_inject_relative_from(int source, int dx, int dy, int wheel, u8 buttons);
void mouse_set_present(void);
void mouse_inject_absolute(int x, int y, int max_x, int max_y, int wheel, u8 buttons);

/* Which merge slot this interface's buttons live in - see mouse.c for why
 * every pointer must have its own.  Slots 0 to 2 belong to PS/2, the legacy
 * single-device entry and the absolute path, so HID interfaces start at 3. */
static int hid_mouse_source(const void *h);

/* How many HID interfaces can be attached at once.
 *
 * This was four, on the reasoning that a machine has a keyboard and a mouse.
 * That is not how the devices are built.  One gaming mouse - a Razer Viper -
 * presents three: a pointer, a keyboard (for the buttons that send keystrokes)
 * and a combined one.  A single mouse therefore filled three quarters of the
 * table on its own, and the keyboard that followed took the last place, after
 * which every further device was refused with a warning nobody would see.
 *
 * A desk with a mouse, a keyboard, a headset, a microphone and a monitor hub
 * is ordinary, and each of those can present several.  Thirty-two costs a few
 * kilobytes and stops the limit being something anyone has to think about. */
#define MAX_HID         32
#define HID_MAX_FIELDS  24
#define HID_MAX_KEYS    16

/* Autorepeat, matched to what a PS/2 keyboard's own hardware does. */
#define REPEAT_DELAY_MS  500
#define REPEAT_RATE_MS    33

/* Usage pages worth recognising. */
#define PAGE_GENERIC   0x01
#define PAGE_KEYBOARD  0x07
#define PAGE_BUTTON    0x09
#define PAGE_CONSUMER  0x0C
#define PAGE_DIGITIZER 0x0D

/* Generic Desktop usages. */
#define USAGE_X       0x30
#define USAGE_Y       0x31
#define USAGE_WHEEL   0x38

/* Digitizer usages. */
#define USAGE_TIP_SWITCH 0x42

/* What one Input item contributes to a report. */
typedef struct {
    u8   report_id;
    u16  bit_offset;
    u8   bit_size;
    u8   count;          /* how many values of that size, laid out in a row */
    u8   page;
    /* A variable item names its controls either as an explicit list or as a
     * range.  Both forms occur, and a mouse's X, Y and Wheel are the common
     * case where the list is not consecutive, so both have to be kept. */
    u16  usages[8];
    u8   nusages;
    u16  usage_min, usage_max;
    bool has_range;
    bool is_array;
    bool is_relative;
    s32  logical_min, logical_max;
} hid_field_t;

typedef struct {
    bool  used;
    usb_device_t *dev;
    u8    interface;

    bool  boot_mode;
    u8    boot_protocol; /* advertised boot subclass + keyboard/mouse protocol */
    bool  is_keyboard;
    bool  is_pointer;
    bool  uses_report_ids;

    hid_field_t fields[HID_MAX_FIELDS];
    int   nfields;

    /* Keyboard state, kept as a set of usages rather than a fixed six so a
     * keyboard that reports more of them does not lose keys. */
    u8    last_keys[HID_MAX_KEYS];
    int   last_key_count;
    u8    last_mods;

    /* Pointer state. */
    u8    buttons;
    bool  absolute;

    u16   repeat_key;
    u8    repeat_mods;
    u64   repeat_at;

    /* Diagnostic counters only: silence is normal with SET_IDLE(0). */
    u64   attached_at;
    u32   reports_seen;
} hid_t;

static hid_t slots[MAX_HID];

int hid_mouse_source(const void *h) {
    const hid_t *p = h;
    if (p >= slots && p < slots + MAX_HID) return 3 + (int)(p - slots);
    return 3 + MAX_HID;    /* a detached interface (Bluetooth) - one shared slot */
}

static bool  caps_on;

/* ------------------------------------------------------------ translation */

/* HID Keyboard/Keypad usages, unshifted and shifted.  Only what an ordinary
 * keyboard produces is listed; anything else falls through to zero. */
static const u16 usage_plain[0x68] = {
    /* 00 */ 0, 0, 0, 0,
    /* 04 */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
    /* 11 */ 'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 1e */ '1','2','3','4','5','6','7','8','9','0',
    /* 28 */ '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '\\',
    /* 33 */ ';', '\'', '`', ',', '.', '/',
    /* 39 */ 0,                                        /* caps lock */
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,                              /* print, scroll, pause */
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN,
    /* 4f */ KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0,                                        /* num lock */
    /* 54 */ '/', '*', '-', '+', '\n',
    /* 59 */ '1','2','3','4','5','6','7','8','9','0','.',
    /* 64 */ '\\', 0, 0, '=',
};

static const u16 usage_shift[0x68] = {
    /* 00 */ 0, 0, 0, 0,
    /* 04 */ 'A','B','C','D','E','F','G','H','I','J','K','L','M',
    /* 11 */ 'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    /* 1e */ '!','@','#','$','%','^','&','*','(',')',
    /* 28 */ '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', '|',
    /* 33 */ ':', '"', '~', '<', '>', '?',
    /* 39 */ 0,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN,
    /* 4f */ KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0,
    /* 54 */ '/', '*', '-', '+', '\n',
    /* 59 */ '1','2','3','4','5','6','7','8','9','0','.',
    /* 64 */ '|', 0, 0, '+',
};

#define USAGE_CAPSLOCK 0x39

static u8 translate_mods(u8 hid_mods) {
    u8 m = 0;
    if (hid_mods & 0x22) m |= MOD_SHIFT;      /* either shift   */
    if (hid_mods & 0x11) m |= MOD_CTRL;       /* either control */
    if (hid_mods & 0x44) m |= MOD_ALT;        /* either alt     */
    if (caps_on) m |= MOD_CAPS;
    return m;
}

static u16 translate(u8 usage, u8 mods) {
    if (usage >= ARRAY_LEN(usage_plain)) return KEY_NONE;

    bool shift = (mods & MOD_SHIFT) != 0;
    u16 base = usage_plain[usage];
    /* Caps lock turns letters over, not the digit row. */
    if ((mods & MOD_CAPS) && base >= 'a' && base <= 'z') shift = !shift;

    u16 c = shift ? usage_shift[usage] : usage_plain[usage];
    if (!c) return KEY_NONE;

    if ((mods & MOD_CTRL) && c < 0x100) {
        if (c >= 'a' && c <= 'z') return (u16)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (u16)(c - 'A' + 1);
        if (c == '[')  return 27;
        if (c == '\\') return 28;
        if (c == ']')  return 29;
    }
    return c;
}

/* -------------------------------------------------- report descriptor parse */

/* Global and local item state, as the parser walks the descriptor. */
typedef struct {
    u16 page;
    s32 logical_min, logical_max;
    u32 report_size, report_count;
    u8  report_id;
} hid_global_t;

#define LOCAL_USAGES 16

typedef struct {
    u16 usage[LOCAL_USAGES];
    int usage_count;
    u32 usage_min, usage_max;
    bool have_range;
} hid_local_t;

/* Where the next bit of each report goes.  A device with report IDs keeps a
 * separate bit position per ID; without them there is only one report. */
typedef struct {
    u8  id;
    u32 bits;
} report_pos_t;

#define MAX_REPORT_IDS 8

static u32 *position_for(report_pos_t *pos, int *npos, u8 id) {
    for (int i = 0; i < *npos; i++) if (pos[i].id == id) return &pos[i].bits;
    if (*npos >= MAX_REPORT_IDS) return NULL;
    pos[*npos].id = id;
    /* A report that carries an ID spends its first byte on it. */
    pos[*npos].bits = id ? 8 : 0;
    return &pos[(*npos)++].bits;
}

static s32 item_signed(const u8 *p, int size) {
    switch (size) {
    case 1: return (s8)p[0];
    case 2: return (s16)(p[0] | (p[1] << 8));
    case 4: return (s32)((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
    default: return 0;
    }
}

static u32 item_unsigned(const u8 *p, int size) {
    switch (size) {
    case 1: return p[0];
    case 2: return (u32)(p[0] | (p[1] << 8));
    case 4: return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    default: return 0;
    }
}

/* Record one Input item as a field, if it carries anything this driver reads. */
static void add_field(hid_t *h, const hid_global_t *g, const hid_local_t *l,
                      u32 bit_offset, u8 flags) {
    if (h->nfields >= HID_MAX_FIELDS) return;

    bool variable = (flags & 0x02) != 0;
    bool constant = (flags & 0x01) != 0;
    if (constant) return;                       /* padding, nothing to read */

    u8 page = (u8)g->page;
    if (page != PAGE_GENERIC && page != PAGE_KEYBOARD &&
        page != PAGE_BUTTON  && page != PAGE_DIGITIZER)
        return;

    hid_field_t *f = &h->fields[h->nfields];
    memset(f, 0, sizeof *f);
    f->report_id = g->report_id;
    f->bit_offset = (u16)bit_offset;
    f->bit_size = (u8)(g->report_size > 32 ? 32 : g->report_size);
    f->count = (u8)(g->report_count > 255 ? 255 : g->report_count);
    f->page = page;
    f->is_array = !variable;
    f->is_relative = (flags & 0x04) != 0;
    f->logical_min = g->logical_min;
    f->logical_max = g->logical_max;

    f->has_range = l->have_range;
    f->usage_min = (u16)l->usage_min;
    f->usage_max = (u16)l->usage_max;
    f->nusages = (u8)(l->usage_count > 8 ? 8 : l->usage_count);
    for (int i = 0; i < f->nusages; i++) f->usages[i] = l->usage[i];

    h->nfields++;
}

/* Walk the byte code.  Only Input items matter; Output and Feature items are
 * skipped but still advance nothing, since they live in their own reports. */
/* Not static.
 *
 * These two functions are the whole of what "HID" means once the bytes have
 * arrived, and neither touches anything about USB - checked rather than
 * assumed: no reference to the device or the interface appears in either.
 * What carried the bytes is the transport's business and stops mattering here.
 *
 * So Bluetooth uses these rather than a second copy.  A second copy of a
 * report-descriptor parser is a second set of the same bugs, discovered
 * separately, and this one has been driven against real descriptors with and
 * without report identifiers.
 *
 * Deliberately NOT moved into a file of its own.  Moving it would mean editing
 * the path that carries this machine's only working keyboard for no gain
 * beyond tidiness. */
/* A HID device that arrived by some other road.
 *
 * Everything below the transport is the same whatever carried the bytes, so a
 * Bluetooth keyboard gets one of these and fills in nothing about USB.  It is
 * taken from the same table the USB devices use, because the limit on how many
 * input devices this system tracks is a property of the system and not of any
 * one bus. */
hid_t *hid_alloc_detached(void) {
    for (int i = 0; i < MAX_HID; i++) {
        if (slots[i].used) continue;
        memset(&slots[i], 0, sizeof slots[i]);
        slots[i].used = true;
        slots[i].dev = NULL;
        return &slots[i];
    }
    return NULL;
}

void hid_free_detached(hid_t *h) {
    if (h) memset(h, 0, sizeof *h);
}

bool hid_is_keyboard(const hid_t *h) { return h && h->is_keyboard; }
bool hid_is_pointer(const hid_t *h)  { return h && h->is_pointer; }

static u16 field_usage(const hid_field_t *f, int n);

/* What a description says the device is.
 *
 * Part of parsing, not of attaching, because it is decided entirely by the
 * description - which is the same whether it arrived over USB or over a radio.
 */
static void hid_classify(hid_t *h) {
    for (int i = 0; i < h->nfields; i++) {
        hid_field_t *f = &h->fields[i];
        if (f->page == PAGE_KEYBOARD) h->is_keyboard = true;
        if (f->page == PAGE_BUTTON || f->page == PAGE_DIGITIZER)
            h->is_pointer = true;
        if (f->page == PAGE_GENERIC && !f->is_array)
            for (int n = 0; n < f->count; n++) {
                u16 u = field_usage(f, n);
                if (u == USAGE_X || u == USAGE_Y) h->is_pointer = true;
            }
    }
}

bool hid_parse_descriptor(hid_t *h, const u8 *d, int len) {
    hid_global_t g;
    hid_local_t  l;
    report_pos_t pos[MAX_REPORT_IDS];
    int npos = 0;

    memset(&g, 0, sizeof g);
    memset(&l, 0, sizeof l);
    g.logical_max = 1;

    h->nfields = 0;
    h->uses_report_ids = false;

    /* A separate cursor per report id for the output/feature streams, so an
     * Output item never shifts where the Input fields land. */
    report_pos_t opos[MAX_REPORT_IDS];
    int onpos = 0;

    int off = 0;
    while (off < len) {
        u8 prefix = d[off++];
        if (prefix == 0xFE) {                    /* long item: skip it */
            if (off + 1 >= len) break;
            int dsize = d[off];
            off += 2 + dsize;
            continue;
        }

        int size = prefix & 0x03;
        if (size == 3) size = 4;
        int type = (prefix >> 2) & 0x03;
        int tag  = (prefix >> 4) & 0x0F;
        if (off + size > len) break;
        const u8 *data = d + off;
        off += size;

        if (type == 1) {                                     /* global */
            switch (tag) {
            case 0: g.page = (u16)item_unsigned(data, size); break;
            case 1: g.logical_min = item_signed(data, size); break;
            case 2:
                /* Logical maximum is signed, but a device that says 0..255
                 * encodes 255 in one byte, which reads as -1.  When the
                 * minimum is not negative, take it as unsigned. */
                g.logical_max = (g.logical_min >= 0) ? (s32)item_unsigned(data, size)
                                                     : item_signed(data, size);
                break;
            case 7: g.report_size = item_unsigned(data, size); break;
            case 8: g.report_id = (u8)item_unsigned(data, size);
                    if (g.report_id) h->uses_report_ids = true;
                    break;
            case 9: g.report_count = item_unsigned(data, size); break;
            default: break;                        /* physical, unit, push/pop */
            }
        } else if (type == 2) {                              /* local */
            switch (tag) {
            case 0:
                if (l.usage_count < LOCAL_USAGES)
                    l.usage[l.usage_count++] = (u16)item_unsigned(data, size);
                break;
            case 1: l.usage_min = item_unsigned(data, size); l.have_range = true; break;
            case 2: l.usage_max = item_unsigned(data, size); l.have_range = true; break;
            default: break;
            }
        } else if (type == 0) {                              /* main */
            u32 bits = g.report_size * g.report_count;

            if (tag == 8) {                                  /* Input */
                u32 *p = position_for(pos, &npos, g.report_id);
                if (p) {
                    add_field(h, &g, &l, *p, size ? data[0] : 0);
                    *p += bits;
                }
            } else if (tag == 9 || tag == 10) {              /* Output, Feature */
                u32 *p = position_for(opos, &onpos, g.report_id);
                if (p) *p += bits;
            }
            /* Collection and End Collection carry no bits. */
            memset(&l, 0, sizeof l);                 /* locals reset per item */
        }
    }

    if (h->nfields <= 0) return false;

    hid_classify(h);
    return true;
}

/* Which control the n'th value of a variable field belongs to.  An explicit
 * list wins; where the list runs out the last entry covers the rest, which is
 * what the specification says a short list means. */
static u16 field_usage(const hid_field_t *f, int n) {
    if (f->nusages) return f->usages[n < f->nusages ? n : f->nusages - 1];
    if (f->has_range) {
        u32 u = (u32)f->usage_min + (u32)n;
        return (u16)(u > f->usage_max ? f->usage_max : u);
    }
    return 0;
}

/* ------------------------------------------------------------- extraction */

/* HID packs values least-significant-bit first, running across byte
 * boundaries without regard for them. */
static u32 get_bits(const u8 *data, int len, u32 bit_off, u8 bit_size) {
    u32 v = 0;
    for (u8 i = 0; i < bit_size; i++) {
        u32 b = bit_off + i;
        if ((int)(b >> 3) >= len) break;
        if (data[b >> 3] & (1u << (b & 7))) v |= (1u << i);
    }
    return v;
}

static s32 get_signed(const u8 *data, int len, u32 bit_off, u8 bit_size) {
    u32 v = get_bits(data, len, bit_off, bit_size);
    if (bit_size < 32 && (v & (1u << (bit_size - 1))))
        v |= ~((1u << bit_size) - 1);
    return (s32)v;
}

/* --------------------------------------------------------------- keyboard */

static bool in_set(const u8 *set, int n, u8 v) {
    for (int i = 0; i < n; i++) if (set[i] == v) return true;
    return false;
}

static void emit_keys(hid_t *h, const u8 *keys, int count, u8 mods) {
    /* Releases first, so a chord that swaps one key for another reads in the
     * order the user's fingers moved. */
    for (int i = 0; i < h->last_key_count; i++) {
        u8 usage = h->last_keys[i];
        if (!usage || in_set(keys, count, usage)) continue;
        u16 key = translate(usage, mods);
        if (key != KEY_NONE) input_inject_key(key, mods, false);
        if (h->repeat_key && translate(usage, h->repeat_mods) == h->repeat_key)
            h->repeat_key = 0;
    }

    for (int i = 0; i < count; i++) {
        u8 usage = keys[i];
        if (!usage || in_set(h->last_keys, h->last_key_count, usage)) continue;
        u16 key = translate(usage, mods);
        if (key == KEY_NONE) continue;
        input_inject_key(key, mods, true);
        /* The most recent key wins the repeat, as a keyboard's own hardware
         * does. */
        h->repeat_key = key;
        h->repeat_mods = mods;
        h->repeat_at = g_uptime_ms + REPEAT_DELAY_MS;
    }

    if (count > HID_MAX_KEYS) count = HID_MAX_KEYS;
    memcpy(h->last_keys, keys, (size_t)count);
    h->last_key_count = count;
    h->last_mods = mods;
}

/* ------------------------------------------------------- descriptor-driven */

void hid_handle_report(hid_t *h, const u8 *data, int len) {
    u8 id = 0;
    if (h->uses_report_ids) {
        if (len < 1) return;
        id = data[0];
    }

    u8   keys[HID_MAX_KEYS];
    int  nkeys = 0;
    u8   hid_mods = 0;
    bool have_keys = false;

    bool have_pointer = false, relative = true;
    s32  x = 0, y = 0, max_x = 0, max_y = 0;
    int  wheel = 0;
    u8   buttons = 0;
    bool have_xy = false;

    for (int i = 0; i < h->nfields; i++) {
        hid_field_t *f = &h->fields[i];
        if (f->report_id != id) continue;

        if (f->page == PAGE_KEYBOARD) {
            have_keys = true;
            if (f->is_array) {
                for (int n = 0; n < f->count && nkeys < HID_MAX_KEYS; n++) {
                    u32 v = get_bits(data, len, f->bit_offset + (u32)n * f->bit_size,
                                     f->bit_size);
                    if (v == 0 || v == 1) continue;      /* none, or rollover */
                    keys[nkeys++] = (u8)v;
                }
            } else {
                /* The modifier keys are a run of single bits, usages E0..E7. */
                for (int n = 0; n < f->count; n++) {
                    u16 usage = field_usage(f, n);
                    if (usage < 0xE0 || usage > 0xE7) continue;
                    if (get_bits(data, len, f->bit_offset + (u32)n * f->bit_size,
                                 f->bit_size))
                        hid_mods |= (u8)(1u << (usage - 0xE0));
                }
            }
        } else if (f->page == PAGE_BUTTON) {
            have_pointer = true;
            for (int n = 0; n < f->count && n < 8; n++) {
                u16 usage = field_usage(f, n);
                if (usage < 1 || usage > 8) continue;
                if (get_bits(data, len, f->bit_offset + (u32)n * f->bit_size,
                             f->bit_size))
                    buttons |= (u8)(1u << (usage - 1));
            }
        } else if (f->page == PAGE_DIGITIZER) {
            /* A pen or finger touching down reads as the left button. */
            if (!f->is_array) {
                for (int n = 0; n < f->count; n++) {
                    if (field_usage(f, n) != USAGE_TIP_SWITCH) continue;
                    have_pointer = true;
                    if (get_bits(data, len, f->bit_offset + (u32)n * f->bit_size,
                                 f->bit_size))
                        buttons |= 0x01;
                }
            }
        } else if (f->page == PAGE_GENERIC && !f->is_array) {
            for (int n = 0; n < f->count; n++) {
                u16 usage = field_usage(f, n);
                u32 bit = f->bit_offset + (u32)n * f->bit_size;
                s32 v = f->is_relative ? get_signed(data, len, bit, f->bit_size)
                                       : (s32)get_bits(data, len, bit, f->bit_size);

                if (usage == USAGE_X) {
                    have_pointer = have_xy = true;
                    relative = f->is_relative;
                    x = v; max_x = f->logical_max;
                } else if (usage == USAGE_Y) {
                    have_pointer = have_xy = true;
                    relative = f->is_relative;
                    y = v; max_y = f->logical_max;
                } else if (usage == USAGE_WHEEL) {
                    have_pointer = true;
                    if (!f->is_relative) v = get_signed(data, len, bit, f->bit_size);
                    wheel = -v;              /* away from the user is up */
                }
            }
        }
    }

    if (have_keys) {
        /* A rollover error means more keys are down than the device can
         * report; its usage bytes say nothing, so leave the state alone. */
        u8 mods = translate_mods(hid_mods);
        if (in_set(keys, nkeys, USAGE_CAPSLOCK) &&
            !in_set(h->last_keys, h->last_key_count, USAGE_CAPSLOCK)) {
            caps_on = !caps_on;
            mods = translate_mods(hid_mods);
        }
        emit_keys(h, keys, nkeys, mods);
    }

    if (have_pointer) {
        h->buttons = buttons;
        if (have_xy && !relative && max_x > 0 && max_y > 0)
            mouse_inject_absolute(x, y, max_x, max_y, wheel, buttons);
        else
            mouse_inject_relative_from(hid_mouse_source(h),
                                       have_xy ? x : 0, have_xy ? y : 0,
                                       wheel, buttons);
    }
}

/* ------------------------------------------------------------ boot protocol */

static void boot_report(hid_t *h, const u8 *data, int len) {
    if (h->is_keyboard) {
        if (len < 3) return;
        u8 keys[HID_MAX_KEYS];
        int n = 0;
        for (int i = 2; i < len && i < 8 && n < HID_MAX_KEYS; i++)
            if (data[i] > 1) keys[n++] = data[i];
        if (len > 2 && data[2] == 0x01) return;         /* rollover */

        u8 mods = translate_mods(data[0]);
        if (in_set(keys, n, USAGE_CAPSLOCK) &&
            !in_set(h->last_keys, h->last_key_count, USAGE_CAPSLOCK)) {
            caps_on = !caps_on;
            mods = translate_mods(data[0]);
        }
        emit_keys(h, keys, n, mods);
    } else {
        if (len < 3) return;
        u8 buttons = (u8)(data[0] & 0x07);
        h->buttons = buttons;
        mouse_inject_relative_from(hid_mouse_source(h), (s8)data[1], (s8)data[2],
                                   len >= 4 ? -(int)(s8)data[3] : 0, buttons);
    }
}

/* ------------------------------------------------------------------- probe */

void *usbhid_probe(usb_device_t *dev, const usb_interface_t *ifc) {
    if (ifc->dev_class != USB_CLASS_HID) return NULL;

    hid_t *h = NULL;
    for (int i = 0; i < MAX_HID; i++) if (!slots[i].used) { h = &slots[i]; break; }
    if (!h) {
        kwarn("usbhid", "no room for another HID device");
        return NULL;
    }

    memset(h, 0, sizeof *h);
    h->dev = dev;
    h->interface = ifc->number;
    if (ifc->subclass == HID_SUBCLASS_BOOT &&
        (ifc->protocol == HID_PROTO_KEYBOARD || ifc->protocol == HID_PROTO_MOUSE))
        h->boot_protocol = ifc->protocol;

    /* Read the report descriptor first: what it says beats what the interface
     * descriptor claims, because a device that reports a protocol of zero can
     * still be a perfectly ordinary mouse. */
    bool parsed = false;
    if (ifc->report_desc_len && ifc->report_desc_len <= 1024) {
        u8 *rd = kmalloc(ifc->report_desc_len);
        if (rd) {
            int got = usb_control(dev,
                                  USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                                  USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8,
                                  ifc->number, rd, ifc->report_desc_len);
            if (got > 0) parsed = hid_parse_descriptor(h, rd, got);
            kfree(rd);
        }
    }

    /* What it is was decided by the parser, from the description.  It used to
     * be decided here, after the parse and inside the USB attach - so a device
     * that arrived any other way was parsed correctly and then classified as
     * nothing.  A Bluetooth keyboard came through reading "a device over
     * Bluetooth", with every field of a keyboard in it. */

    if (!parsed || (!h->is_keyboard && !h->is_pointer)) {
        /* Nothing recognisable in the descriptor.  Fall back to boot protocol
         * if the interface says it supports one. */
        h->is_keyboard = false;
        h->is_pointer = false;
        if (h->boot_protocol == HID_PROTO_KEYBOARD)   { h->is_keyboard = true; h->boot_mode = true; }
        else if (h->boot_protocol == HID_PROTO_MOUSE) { h->is_pointer = true;  h->boot_mode = true; }
        else return NULL;
        h->nfields = 0;
        h->uses_report_ids = false;
    }

    /* Establish the protocol before queuing interrupt reports. Firmware may
     * have used boot protocol; a parsed descriptor must not be paired with that
     * different wire format. SET_PROTOCOL is defined only for boot-subclass
     * interfaces, not every vendor/composite interface containing HID usages. */
    if (h->boot_protocol &&
        usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    HID_REQ_SET_PROTOCOL, h->boot_mode ? 0 : 1,
                    ifc->number, NULL, 0) < 0) {
        kwarn("usbhid", "%s: could not establish %s protocol on interface %u",
              usb_device_name(dev), h->boot_mode ? "boot" : "report", ifc->number);
        return NULL;
    }

    /* Report only when something changes, rather than on every poll. */
    usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                HID_REQ_SET_IDLE, 0, ifc->number, NULL, 0);

    h->used = true;
    h->attached_at = g_uptime_ms;
    h->reports_seen = 0;
    usb_count_hid(h->is_keyboard ? 1 : 0, h->is_pointer ? 1 : 0);

    /* Say a pointer exists now, not when it first moves.
     *
     * The graphical shell asks once, while it is starting, whether the machine
     * has a pointer - and then draws a cursor or does not for the rest of the
     * session.  A mouse that only becomes "present" on its first movement is
     * therefore never present at the moment the question is asked, and the
     * answer sticks.  A device that has been enumerated and claimed is a
     * pointer whether or not anybody has touched it yet. */
    if (h->is_pointer) mouse_set_present();

    kinfo("usbhid", "%s: %s%s%s (%s)", usb_device_name(dev),
          h->is_keyboard ? "keyboard" : "",
          (h->is_keyboard && h->is_pointer) ? " and " : "",
          h->is_pointer ? "pointer" : "",
          h->boot_mode ? "boot protocol" : "report descriptor");
    return h;
}

void usbhid_detach(void *ctx) {
    hid_t *h = ctx;
    if (!h || !h->used) return;
    usb_count_hid(h->is_keyboard ? -1 : 0, h->is_pointer ? -1 : 0);
    /* Do not leave a key stuck down if the keyboard is pulled out mid-press. */
    if (h->repeat_key) input_inject_key(h->repeat_key, h->repeat_mods, false);
    memset(h, 0, sizeof *h);
}

/* How many reports have arrived, and what the last one said.
 *
 * These exist because of a deadlock in diagnosing this system on a machine
 * with no working input: nobody can type a command to ask what is happening,
 * and the one thing that could show it - the screen - is covered by a desktop
 * that cannot be driven.  A count that moves is the difference between "the
 * device never sends anything" and "it sends and something downstream drops
 * it", and those need completely different fixes.
 */
u32 g_hid_reports;
u32 g_hid_rejected;    /* arrived, but had no driver to go to */
u8  g_hid_last[8];
u8  g_hid_last_len;

void usbhid_report(void *ctx, const u8 *data, int len) {
    hid_t *h = ctx;
    if (h && h->used) h->reports_seen++;

    /* Counted before anything can reject it.
     *
     * This sat below the check, which meant a report that arrived and was
     * turned away here - because the driver handle was wrong, or the device
     * had been torn down - was indistinguishable from a report that never
     * arrived.  Those are opposite faults living in different layers, and the
     * whole purpose of this counter is to tell them apart. */
    g_hid_reports++;
    g_hid_last_len = (u8)(len > 8 ? 8 : len);
    for (int i = 0; i < g_hid_last_len && i < 8; i++) g_hid_last[i] = data[i];

    if (!h || !h->used || len <= 0) { g_hid_rejected++; return; }
    if (h->nfields) hid_handle_report(h, data, len);
    else            boot_report(h, data, len);
}

void usbhid_tick(void) {
    u64 now = g_uptime_ms;
    for (int i = 0; i < MAX_HID; i++) {
        hid_t *h = &slots[i];
        if (!h->used) continue;

        /* SET_IDLE(0) explicitly requests reports only on change. Silence
         * throughout a long GPU test is normal. Never change the wire protocol
         * (or issue cross-controller control transfers) from this timer. */

        if (!h->repeat_key) continue;
        if (now < h->repeat_at) continue;
        input_inject_key(h->repeat_key, h->repeat_mods, true);
        h->repeat_at = now + REPEAT_RATE_MS;
    }
}

/* ------------------------------------------------------------------- tests
 *
 * A report descriptor is the device telling the driver where in each report
 * its buttons and axes are, and this parser is what turns that into a cursor
 * moving.  It had no tests, which meant the only way to find out whether it
 * understood a particular mouse was to plug that mouse in - and every device
 * describes itself differently, so "it worked on the one we had" says very
 * little.
 *
 * A report descriptor is only bytes, so this needs no hardware at all.  The
 * two below are the shapes that actually turn up:
 *
 *   A plain mouse, with no report IDs.  The whole report is one item and the
 *   first byte is data rather than an identifier.
 *
 *   A composite device with report IDs, which is what a gaming mouse is: the
 *   pointer, the keyboard it pretends to be for its side buttons, and a vendor
 *   report, all on one interface, each prefixed with its own number.  Getting
 *   the identifier byte wrong here shifts every field by eight bits and turns
 *   a small movement into a large one in a random direction.
 */

/* A mouse: three buttons, then eight-bit relative X and Y. */
static const u8 test_desc_mouse[] = {
    0x05, 0x01,        /* usage page: generic desktop      */
    0x09, 0x02,        /* usage: mouse                     */
    0xA1, 0x01,        /* collection: application          */
    0x09, 0x01,        /*   usage: pointer                 */
    0xA1, 0x00,        /*   collection: physical           */
    0x05, 0x09,        /*     usage page: button           */
    0x19, 0x01,        /*     usage minimum: 1             */
    0x29, 0x03,        /*     usage maximum: 3             */
    0x15, 0x00,        /*     logical minimum: 0           */
    0x25, 0x01,        /*     logical maximum: 1           */
    0x95, 0x03,        /*     report count: 3              */
    0x75, 0x01,        /*     report size: 1               */
    0x81, 0x02,        /*     input: data, variable        */
    0x95, 0x01,        /*     report count: 1              */
    0x75, 0x05,        /*     report size: 5  (padding)    */
    0x81, 0x03,        /*     input: constant              */
    0x05, 0x01,        /*     usage page: generic desktop  */
    0x09, 0x30,        /*     usage: X                     */
    0x09, 0x31,        /*     usage: Y                     */
    0x15, 0x81,        /*     logical minimum: -127        */
    0x25, 0x7F,        /*     logical maximum: 127         */
    0x75, 0x08,        /*     report size: 8               */
    0x95, 0x02,        /*     report count: 2              */
    0x81, 0x06,        /*     input: data, variable, rel   */
    0xC0,              /*   end collection                 */
    0xC0               /* end collection                   */
};

/* The same mouse, but every report prefixed with an identifier - which is how
 * a device that presents more than one thing on one interface has to do it. */
static const u8 test_desc_composite[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x01,        /*   report ID: 1                   */
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0
};

int usbhid_selftest(void) {
    int failures = 0;
    hid_t h;

    /* 1. A plain mouse.  The parser has to come out of this knowing it is a
     *    pointer, and knowing that the movement is relative - an absolute
     *    reading of the same bits would put the cursor at a fixed spot near
     *    the top-left corner and leave it there. */
    memset(&h, 0, sizeof h);
    if (!hid_parse_descriptor(&h, test_desc_mouse, sizeof test_desc_mouse)) {
        kerr("usbhid", "a plain mouse descriptor was not understood");
        failures++;
    } else if (h.uses_report_ids) {
        kerr("usbhid", "a descriptor with no report IDs was read as having them");
        failures++;
    } else {
        /* Find the X axis and check it was read as relative and eight bits. */
        bool found = false;
        for (int i = 0; i < h.nfields; i++) {
            hid_field_t *f = &h.fields[i];
            for (int u = 0; u < f->nusages; u++)
                if (f->page == 0x01 && f->usages[u] == 0x30) {
                    found = true;
                    if (!f->is_relative) {
                        kerr("usbhid", "the X axis was read as absolute");
                        failures++;
                    }
                    if (f->bit_size != 8) {
                        kerr("usbhid", "the X axis came out %u bits, expected 8",
                             f->bit_size);
                        failures++;
                    }
                    if (f->bit_offset != 8) {
                        kerr("usbhid", "the X axis is at bit %u, expected 8 "
                                       "(three buttons and five bits of padding)",
                             f->bit_offset);
                        failures++;
                    }
                }
        }
        if (!found) {
            kerr("usbhid", "the descriptor named no X axis - nothing would move "
                           "the cursor");
            failures++;
        } else {
            kinfo("usbhid", "a mouse descriptor parses: pointer, relative X at "
                            "bit 8, three buttons before it");
        }
    }

    /* 2. The composite form.  The one thing that must differ is that the
     *    first byte of every report is now an identifier rather than data, so
     *    everything sits eight bits further along.  A parser that misses this
     *    still works perfectly on the device it was written against and is
     *    wrong on every device that uses IDs - which is most of them. */
    memset(&h, 0, sizeof h);
    if (!hid_parse_descriptor(&h, test_desc_composite,
                                 sizeof test_desc_composite)) {
        kerr("usbhid", "a composite descriptor was not understood");
        failures++;
    } else if (!h.uses_report_ids) {
        kerr("usbhid", "a descriptor with report IDs was read as having none");
        failures++;
    } else {
        bool ok = false;
        for (int i = 0; i < h.nfields; i++) {
            hid_field_t *f = &h.fields[i];
            for (int u = 0; u < f->nusages; u++)
                if (f->page == 0x01 && f->usages[u] == 0x30) {
                    ok = true;
                    if (f->report_id != 1) {
                        kerr("usbhid", "the X axis belongs to report %u, "
                                       "expected 1", f->report_id);
                        failures++;
                    }
                    /* Sixteen, not eight.  Offsets are measured from the
                     * start of the whole report, and a report that has an
                     * identifier begins with it - so everything is eight bits
                     * further along than the same device without one.  This is
                     * the convention the extraction uses, and the test exists
                     * to hold the two together: a parser that counted from
                     * after the identifier would work on every device with no
                     * IDs and be wrong on every device with them. */
                    if (f->bit_offset != 16) {
                        kerr("usbhid", "with a report ID the X axis is at bit "
                                       "%u, expected 16 - eight for the "
                                       "identifier and eight for the buttons",
                             f->bit_offset);
                        failures++;
                    }
                }
        }
        if (!ok) {
            kerr("usbhid", "the composite descriptor named no X axis");
            failures++;
        } else {
            kinfo("usbhid", "a composite descriptor parses too, with its fields "
                            "offset past the report identifier");
        }
    }

    if (!failures)
        kinfo("usbhid", "report descriptors are understood, with and without "
                        "report identifiers");
    return failures;
}
