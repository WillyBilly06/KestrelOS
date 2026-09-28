/* app_devices.c - Device Manager.
 *
 * Everything the machine found, grouped by what it is rather than by which bus
 * it happens to hang off, with the driver that claimed each one and what state
 * it is in.  The grouping matters: somebody looking for why their sound does
 * not work wants to look under Sound, not to know that the codec is on PCI
 * function 3 of device 31.
 *
 * The detail pane is where the honest reporting lives.  A device with no
 * driver says so; one whose driver is waiting for a firmware file says which
 * file and where to get it; one that is working says what it is doing.  A
 * device manager that showed everything as fine would be worse than none.
 */
#include "desktop.h"

/* ------------------------------------------------------------- the groups */

typedef enum {
    GROUP_DISPLAY,
    GROUP_NETWORK,
    GROUP_WIRELESS,
    GROUP_STORAGE,
    GROUP_SOUND,
    GROUP_USB,
    GROUP_OTHER,
    GROUP_COUNT,
} group_t;

static const struct { const char *name; icon_id icon; } groups[GROUP_COUNT] = {
    { "Display adapters",   ICON_DISPLAY },
    { "Network adapters",   ICON_NETWORK },
    { "Wireless",           ICON_WIFI    },
    { "Storage",            ICON_DISK    },
    { "Sound",              ICON_SOUND   },
    { "USB",                ICON_USB     },
    { "Other devices",      ICON_CHIP    },
};

typedef enum { HEALTH_WORKING, HEALTH_WAITING, HEALTH_NONE } health_t;

#define MAX_DEVICES 96
#define NAME_MAX 72
#define LINE_MAX 96
#define DETAIL_LINES 10

typedef struct {
    group_t  group;
    icon_id  icon;
    char     name[NAME_MAX];
    char     driver[32];
    health_t health;
    char     detail[DETAIL_LINES][LINE_MAX];
    int      detail_count;
} device_t;

typedef struct {
    device_t devices[MAX_DEVICES];
    int      count;
    bool     open[GROUP_COUNT];
    int      selected;
    int      hover;
    int      scroll;
    uint64_t refreshed;
} devices_t;

/* One row of the tree: either a group heading or a device under it. */
typedef struct { bool is_group; int group; int device; } row_t;

/* Everything scales, and the tree is wider than it was even at scale one -
 * "Realtek RTL8922AE Wi-Fi 7" should not have to become "Realtek RTL89..."
 * for the column to hold it.  On the machine this exists to describe, the
 * device names are the content. */
#define TREE_W  (360 * gui_scale())
#define ROW_H   ((gui_font_height(FONT_UI) * 3) / 2)
#define PAD     (16 * gui_scale())

static int build_rows(devices_t *d, row_t *rows, int cap) {
    int n = 0;
    for (int g = 0; g < GROUP_COUNT && n < cap; g++) {
        int members = 0;
        for (int i = 0; i < d->count; i++)
            if ((int)d->devices[i].group == g) members++;
        if (!members) continue;

        rows[n].is_group = true;
        rows[n].group = g;
        rows[n].device = -1;
        n++;

        if (!d->open[g]) continue;
        for (int i = 0; i < d->count && n < cap; i++) {
            if ((int)d->devices[i].group != g) continue;
            rows[n].is_group = false;
            rows[n].group = g;
            rows[n].device = i;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------- collecting */

static device_t *add(devices_t *d, group_t group, icon_id icon, const char *name) {
    if (d->count >= MAX_DEVICES) return NULL;
    device_t *dev = &d->devices[d->count++];
    memset(dev, 0, sizeof *dev);
    dev->group = group;
    dev->icon = icon;
    strlcpy(dev->name, name, sizeof dev->name);
    dev->health = HEALTH_NONE;
    return dev;
}

static void detail(device_t *dev, const char *fmt, ...) {
    if (!dev || dev->detail_count >= DETAIL_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dev->detail[dev->detail_count], LINE_MAX, fmt, ap);
    va_end(ap);
    dev->detail_count++;
}

/* Which group a PCI class belongs in.  The class codes are the ones the bus
 * itself reports, so this works for a device nothing here has a driver for. */
static group_t group_for_class(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x01: return GROUP_STORAGE;
    case 0x02: return sub == 0x80 ? GROUP_WIRELESS : GROUP_NETWORK;
    case 0x03: return GROUP_DISPLAY;
    case 0x04: return GROUP_SOUND;
    case 0x0C: return sub == 0x03 ? GROUP_USB : GROUP_OTHER;
    default:   return GROUP_OTHER;
    }
}

static void collect_graphics(devices_t *d) {
    kgpuinfo_t g;
    for (uint32_t i = 0; enum_gpu(i, &g) == 0; i++) {
        device_t *dev = add(d, GROUP_DISPLAY, ICON_DISPLAY, g.name);
        if (!dev) return;

        strlcpy(dev->driver, g.arch[0] ? g.arch : "generic", sizeof dev->driver);
        detail(dev, "PCI %04x:%04x at %02x:%02x.%u", g.pci_vendor, g.pci_device,
               g.bus, g.slot, g.func);
        if (g.vram_bytes) {
            char size[24];
            format_size(size, sizeof size, g.vram_bytes);
            detail(dev, "Video memory: %s%s", size, g.vram_exact ? "" : " (at least)");
        }
        if (g.boot_display) detail(dev, "This is the display the machine started on.");
        if (g.note[0]) detail(dev, "%s", g.note);

        if (g.firmware_needed && !g.firmware_present) {
            dev->health = HEALTH_WAITING;
            detail(dev, "Needs %s", g.firmware_name);
            detail(dev, "Get it from linux-firmware, then `firmware import`.");
        } else {
            dev->health = HEALTH_WORKING;
        }
    }

    /* What the graphics driver itself read, which is more than the bus says. */
    kgpudetail_t detail_info;
    if (gpu_detail(&detail_info) == 0) {
        device_t *dev = add(d, GROUP_DISPLAY, ICON_DISPLAY, "Graphics driver");
        if (!dev) return;
        strlcpy(dev->driver, "nvidia", sizeof dev->driver);
        dev->health = HEALTH_WORKING;
        detail(dev, "Chip %03x, %s (%s)", detail_info.chipset,
               detail_info.architecture, detail_info.codename);
        if (detail_info.vbios_version[0])
            detail(dev, "Video BIOS %s, from %s", detail_info.vbios_version,
                   detail_info.vbios_source);
        if (detail_info.temperature_c > -1000)
            detail(dev, "%d degrees", detail_info.temperature_c);
        for (int i = 0; i < detail_info.outputs && i < 4; i++)
            if (detail_info.connector[i][0]) detail(dev, "%s", detail_info.connector[i]);
        if (detail_info.modelled)
            detail(dev, "A model is standing in; there is no card in this machine.");
    }
}

static void collect_network(devices_t *d) {
    knetinfo_t n;
    for (uint32_t i = 0; enum_net(i, &n) == 0; i++) {
        /* A wireless interface is listed under Wireless with its own detail. */
        bool wireless = false;
        kwifiinfo_t w;
        for (uint32_t k = 0; enum_wifi(k, &w) == 0; k++)
            if (!strcmp(w.name, n.name)) wireless = true;
        if (wireless) continue;

        device_t *dev = add(d, GROUP_NETWORK, ICON_NETWORK,
                            n.model[0] ? n.model : n.name);
        if (!dev) return;
        strlcpy(dev->driver, n.name, sizeof dev->driver);
        dev->health = n.link_up ? HEALTH_WORKING : HEALTH_NONE;

        char mac[24];
        format_mac(n.mac, mac, sizeof mac);
        detail(dev, "Interface %s, %s", n.name, mac);
        detail(dev, "Link: %s%s", n.link_up ? "up" : "down",
               n.link_up && n.link_speed_mbps ? "" : "");
        if (n.link_up && n.link_speed_mbps)
            detail(dev, "Speed: %u Mbit/s", n.link_speed_mbps);

        if (n.ip) {
            char ip[20], gw[20];
            format_ipv4(n.ip, ip, sizeof ip);
            format_ipv4(n.gateway, gw, sizeof gw);
            detail(dev, "Address %s, gateway %s", ip, gw);
        } else {
            detail(dev, "No address.  Run `net dhcp` to ask for one.");
        }
        detail(dev, "%llu packet(s) in, %llu out",
               (unsigned long long)n.rx_packets, (unsigned long long)n.tx_packets);
        if (n.rx_errors || n.tx_errors)
            detail(dev, "%llu error(s) in, %llu out",
                   (unsigned long long)n.rx_errors, (unsigned long long)n.tx_errors);
    }
}

static void collect_wireless(devices_t *d) {
    kwifiinfo_t w;
    for (uint32_t i = 0; enum_wifi(i, &w) == 0; i++) {
        device_t *dev = add(d, GROUP_WIRELESS, ICON_WIFI,
                            w.model[0] ? w.model : w.name);
        if (!dev) return;
        strlcpy(dev->driver, w.vendor, sizeof dev->driver);

        char mac[24];
        format_mac(w.mac, mac, sizeof mac);
        detail(dev, "Interface %s, %s", w.name, mac);

        if (!w.enabled) {
            dev->health = HEALTH_NONE;
            detail(dev, "Switched off.  Turn it on in Settings.");
        } else if (w.firmware_needed && !w.firmware_present) {
            dev->health = HEALTH_WAITING;
            detail(dev, "Needs %s", w.firmware_name);
            detail(dev, "It is in the linux-firmware package.  Copy it onto a");
            detail(dev, "volume this machine can read, then `firmware import`.");
        } else if (w.unsupported_generation) {
            /* Said plainly, because the alternative reading - a radio that
             * failed - sends somebody looking for a fault in a card that is
             * working perfectly well. */
            dev->health = HEALTH_NONE;
            detail(dev, "This system has no driver for this generation of card.");
            detail(dev, "It is left alone rather than driven: the sequence for");
            detail(dev, "the older parts writes to registers that mean other");
            detail(dev, "things here, so running it would do harm and no good.");
            if (w.firmware_present)
                detail(dev, "Its firmware is present - what is missing is the driver.");
        } else if (!w.radio_up) {
            dev->health = HEALTH_WAITING;
            detail(dev, "The radio did not start.");
        } else if (w.state == 5) {
            dev->health = HEALTH_WORKING;
            detail(dev, "Connected to \"%s\" on channel %u", w.ssid, w.channel);
            detail(dev, "Signal %d dBm", w.signal_dbm);
        } else {
            dev->health = HEALTH_WORKING;
            detail(dev, "On, not connected.");
        }
    }
}

static void collect_storage(devices_t *d) {
    kblockinfo_t b;
    for (uint32_t i = 0; enum_block(i, &b) == 0; i++) {
        if (b.is_partition) continue;      /* listed under its disk instead */

        device_t *dev = add(d, GROUP_STORAGE, ICON_DISK,
                            b.model[0] ? b.model : b.name);
        if (!dev) return;
        strlcpy(dev->driver, b.name, sizeof dev->driver);
        dev->health = HEALTH_WORKING;

        char size[24];
        format_size(size, sizeof size, b.size);
        detail(dev, "%s, %u byte sectors", size, b.sector_size);
        if (b.readonly) detail(dev, "Read-only.");

        /* And the volumes on it. */
        for (uint32_t k = 0; ; k++) {
            kblockinfo_t part;
            if (enum_block(k, &part) < 0) break;
            if (!part.is_partition) continue;
            if (strncmp(part.name, b.name, strlen(b.name))) continue;

            char psize[24];
            format_size(psize, sizeof psize, part.size);
            detail(dev, "%s  %s%s%s", part.name, psize,
                   part.label[0] ? "  " : "", part.label[0] ? part.label : "");
        }
    }
}

/* A USB audio device, described by what it said it could do rather than by
 * registers this system set.  These were invisible: the driver read a
 * headset's settings, chose the best and opened the endpoint, and nothing
 * could ask what it found. */
static void collect_usb_sound(devices_t *d) {
    for (int i = 1; i < 8; i++) {
        kaudioinfo_t a;
        if (enum_audio_at(i, &a) < 0 || !a.present) break;

        device_t *dev = add(d, GROUP_SOUND, ICON_SOUND,
                            a.codec[0] ? a.codec : "USB audio device");
        if (!dev) return;
        strlcpy(dev->driver, "usbaudio", sizeof dev->driver);
        dev->health = (a.output_ready || a.input_ready) ? HEALTH_WORKING
                                                        : HEALTH_NONE;

        if (a.output_ready)
            detail(dev, "Playback: %u channel(s) at %u bits", a.channels, a.bits);
        if (a.input_ready)
            detail(dev, "Recording: %u channel(s) at %u bits",
                   a.input_channels, a.input_bits);
        if (a.note[0]) detail(dev, "%s", a.note);
    }
}

static void collect_sound(devices_t *d) {
    collect_usb_sound(d);

    kaudioinfo_t a;
    if (enum_audio(&a) < 0 || !a.present) return;

    device_t *dev = add(d, GROUP_SOUND, ICON_SOUND,
                        a.controller[0] ? a.controller : "Audio controller");
    if (!dev) return;
    strlcpy(dev->driver, "hda", sizeof dev->driver);
    dev->health = a.output_ready ? HEALTH_WORKING : HEALTH_NONE;

    if (a.codec[0]) detail(dev, "Codec: %s", a.codec);
    if (a.sample_rate)
        detail(dev, "%u Hz, %u channel(s), %u bit", a.sample_rate, a.channels, a.bits);
    if (a.dac_node || a.pin_node)
        detail(dev, "Converter node %u, output pin %u", a.dac_node, a.pin_node);
    if (!a.output_ready) detail(dev, "No output path was found on this codec.");
    if (a.note[0]) detail(dev, "%s", a.note);
}

static void collect_usb(devices_t *d) {
    kusbinfo_t u;
    for (uint32_t i = 0; enum_usb(i, &u) == 0; i++) {
        device_t *dev = add(d, GROUP_USB, ICON_USB, u.name[0] ? u.name : "USB device");
        if (!dev) return;
        strlcpy(dev->driver, u.driver[0] ? u.driver : "none", sizeof dev->driver);
        dev->health = u.driver[0] ? HEALTH_WORKING : HEALTH_NONE;

        detail(dev, "%04x:%04x on port %u", u.vendor, u.product, u.port);
        static const char *speeds[] = { "unknown", "low", "full", "high", "super" };
        detail(dev, "Speed: %s", u.speed < 5 ? speeds[u.speed] : "unknown");
        if (!u.driver[0]) detail(dev, "No driver claimed it.");
    }
}

/* Everything else on the bus that nothing above accounted for.  This is the
 * part a device manager exists for: showing what is there that nothing is
 * driving. */
static void collect_remaining_pci(devices_t *d) {
    kpciinfo_t p;
    for (uint32_t i = 0; enum_pci(i, &p) == 0; i++) {
        /* Whether something drives this device is now a fact the kernel
         * records, so it is simply read.
         *
         * It used to be guessed at, and the guess was wrong in the direction
         * that mattered.  Whole categories were skipped on the assumption that
         * a driver above had already listed everything in them - so an
         * undriven IDE controller (storage) and an undriven USB 1.1 controller
         * (USB) were both invisible here, on a machine that has one of each.
         * The one part of the bus a device manager exists to show was the part
         * it hid.  Network controllers were counted against the number of
         * interfaces that came up, which failed the same way for the same
         * reason and needed a paragraph to explain.
         *
         * A device is skipped now if, and only if, a driver said it took it. */
        if (p.driver[0]) continue;

        /* Bridges are not missing drivers.  The firmware configured them and
         * this system only walks through them; listing thirty of them as
         * unsupported hardware would bury the two findings that are real. */
        if (p.class_code == 0x06) continue;
        if (p.class_code == 0x08 && p.subclass == 0x80) continue;

        group_t group = group_for_class(p.class_code, p.subclass);

        device_t *dev = add(d, group, ICON_CHIP,
                            p.description[0] ? p.description : "Unknown device");
        if (!dev) return;
        strlcpy(dev->driver, "none", sizeof dev->driver);
        dev->health = HEALTH_NONE;
        detail(dev, "PCI %04x:%04x at %02x:%02x.%u", p.vendor, p.device,
               p.bus, p.slot, p.func);
        detail(dev, "Class %02x.%02x interface %02x", p.class_code, p.subclass,
               p.prog_if);
        detail(dev, "Nothing here drives this.");
    }
}

static void collect(devices_t *d) {
    d->count = 0;
    collect_graphics(d);
    collect_network(d);
    collect_wireless(d);
    collect_storage(d);
    collect_sound(d);
    collect_usb(d);
    collect_remaining_pci(d);
    d->refreshed = uptime_ms();
}

/* ------------------------------------------------------------------ paint */

/* Below the header, which is as tall as the text and button in it. */
static int header_h(void) {
    return gui_font_height(FONT_UI) + 24 * gui_scale();
}
static rect_t tree_rect(surface_t *s) {
    int top = header_h();
    return rect_make(0, top, TREE_W, s->height - top);
}
static rect_t detail_rect(surface_t *s) {
    int top = header_h() + 12 * gui_scale();
    return rect_make(TREE_W + PAD, top, s->width - TREE_W - PAD * 2,
                     s->height - top - PAD);
}
static rect_t row_rect(int visible) {
    const int k = gui_scale();
    return rect_make(6 * k, header_h() + 6 * k + visible * ROW_H,
                     TREE_W - 12 * k, ROW_H - 2 * k);
}
static rect_t refresh_rect(surface_t *s) {
    const int k = gui_scale();
    int w = gui_text_width(FONT_UI, "Refresh") + 24 * k;
    int h = gui_font_height(FONT_UI) + 10 * k;
    return rect_make(s->width - w - 14 * k, (header_h() - h) / 2, w, h);
}

static colour_t health_colour(health_t h) {
    switch (h) {
    case HEALTH_WORKING: return g_theme.success;
    case HEALTH_WAITING: return g_theme.warning;
    default:             return g_theme.text_dim;
    }
}
static const char *health_word(health_t h) {
    switch (h) {
    case HEALTH_WORKING: return "Working";
    case HEALTH_WAITING: return "Needs firmware";
    default:             return "No driver";
    }
}

static void devices_paint(devices_t *d, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    gui_text(s, FONT_UI, PAD,
             (header_h() - gui_font_height(FONT_UI)) / 2,
             "Device Manager", g_theme.text_bright);
    rect_t refresh = refresh_rect(s);
    gui_button(s, refresh, "Refresh", rect_contains(refresh, mx, my), false, true);
    gui_hline(s, 0, header_h() - 2, s->width, g_theme.window_border);

    rect_t tree = tree_rect(s);
    gui_fill(s, tree, colour_shade(g_theme.window, -5));
    gui_vline(s, tree.x + tree.w - 1, tree.y, tree.h, g_theme.window_border);

    row_t rows[MAX_DEVICES + GROUP_COUNT];
    int row_count = build_rows(d, rows, (int)(sizeof rows / sizeof rows[0]));

    int visible = (tree.h - 12 * gui_scale()) / ROW_H;
    for (int i = 0; i < row_count; i++) {
        int at = i - d->scroll;
        if (at < 0 || at >= visible) continue;
        rect_t r = row_rect(at);

        if (rows[i].is_group) {
            int members = 0;
            for (int k = 0; k < d->count; k++)
                if ((int)d->devices[k].group == rows[i].group) members++;

            if (at == d->hover)
                gui_round_rect_aa(s, r, 5, colour_shade(g_theme.window, 4));

            /* A triangle that turns over when the group is open. */
            const int k2 = gui_scale();
            int cx = r.x + 12 * k2, cy = r.y + r.h / 2;
            colour_t arrow = g_theme.text_dim;
            for (int k = 0; k < 4; k++) {
                if (d->open[rows[i].group])
                    gui_hline(s, cx - 3 + k / 2, cy - 2 + k, 7 - k, arrow);
                else
                    gui_vline(s, cx - 2 + k, cy - 3 + k / 2, 7 - k, arrow);
            }

            gui_icon(s, groups[rows[i].group].icon, r.x + 26 * k2,
                     r.y + (r.h - 15 * k2) / 2, 15 * k2, g_theme.accent);
            char label[80];
            snprintf(label, sizeof label, "%s  (%d)", groups[rows[i].group].name, members);
            gui_text(s, FONT_UI, r.x + 48 * k2,
                     r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                     label, g_theme.text);
        } else {
            device_t *dev = &d->devices[rows[i].device];
            bool selected = (rows[i].device == d->selected);

            if (selected) gui_round_rect_aa(s, r, 5, g_theme.selection);
            else if (at == d->hover)
                gui_round_rect_aa(s, r, 5, colour_shade(g_theme.window, 4));

            colour_t tc = selected ? g_theme.selection_text : g_theme.text;

            /* A dot in the health colour, which is what makes a problem
             * visible without reading anything. */
            const int k2 = gui_scale();
            gui_round_rect_aa(s, rect_make(r.x + 30 * k2, r.y + r.h / 2 - 3 * k2,
                                           6 * k2, 6 * k2), 3 * k2,
                              health_colour(dev->health));
            gui_text_clipped(s, FONT_UI, r.x + 44 * k2,
                             r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                             r.w - 52 * k2, dev->name, tc);
        }
    }

    /* The detail pane. */
    rect_t body = detail_rect(s);
    if (d->selected < 0 || d->selected >= d->count) {
        /* Wrapped to the pane, not drawn as one long line off the right edge -
         * at 2x the sentence was wider than the pane and ran past the window. */
        rect_t hint = rect_make(body.x, body.y + 4 * gui_scale(), body.w, body.h);
        gui_text_wrapped(s, FONT_UI, hint,
                 "Pick a device to see what it is and what state it is in.",
                 g_theme.text_dim);
        return;
    }

    device_t *dev = &d->devices[d->selected];
    gui_icon(s, dev->icon, body.x, body.y, 28 * gui_scale(), g_theme.accent);
    gui_text_clipped(s, FONT_UI, body.x + 40 * gui_scale(), body.y + 2 * gui_scale(),
                     body.w - 48 * gui_scale(), dev->name, g_theme.text_bright);

    int y = body.y + 40 * gui_scale();
    gui_badge(s, body.x, y, health_word(dev->health), health_colour(dev->health));
    if (dev->driver[0]) {
        char line[64];
        snprintf(line, sizeof line, "driver: %s", dev->driver);
        gui_text(s, FONT_UI,
                 body.x + gui_badge_width(health_word(dev->health)) + 14 * gui_scale(),
                 y + 3 * gui_scale(), line, g_theme.text_dim);
    }

    y += 38 * gui_scale();
    gui_hline(s, body.x, y - 10 * gui_scale(), body.w, g_theme.window_border);

    for (int i = 0; i < dev->detail_count; i++) {
        gui_text_clipped(s, FONT_UI, body.x, y, body.w, dev->detail[i], g_theme.text);
        y += gui_font_height(FONT_UI) + 5 * gui_scale();
    }
}

/* ------------------------------------------------------------------ events */

static bool devices_proc(window_t *w, const wevent_t *ev) {
    devices_t *d = w->data;
    surface_t *s = w->canvas;

    row_t rows[MAX_DEVICES + GROUP_COUNT];
    int row_count = build_rows(d, rows, (int)(sizeof rows / sizeof rows[0]));
    int visible = (tree_rect(s).h - 12 * gui_scale()) / ROW_H;

    switch (ev->kind) {
    case WE_PAINT:
        devices_paint(d, s, ev->x, ev->y);
        return false;

    case WE_TICK:
        /* Live: a device that appears or a link that comes up shows up without
         * anybody pressing anything. */
        if (uptime_ms() - d->refreshed > 2000) { collect(d); return true; }
        return false;

    case WE_MOUSE_MOVE: {
        int old = d->hover;
        d->hover = -1;
        for (int at = 0; at < visible && at + d->scroll < row_count; at++)
            if (rect_contains(row_rect(at), ev->x, ev->y)) d->hover = at;
        return old != d->hover || true;
    }

    case WE_MOUSE_WHEEL:
        d->scroll -= ev->wheel * 3;
        if (d->scroll > row_count - visible) d->scroll = row_count - visible;
        if (d->scroll < 0) d->scroll = 0;
        return true;

    case WE_MOUSE_DOWN:
        if (rect_contains(refresh_rect(s), ev->x, ev->y)) { collect(d); return true; }

        for (int at = 0; at < visible && at + d->scroll < row_count; at++) {
            if (!rect_contains(row_rect(at), ev->x, ev->y)) continue;
            row_t *row = &rows[at + d->scroll];
            if (row->is_group) d->open[row->group] = !d->open[row->group];
            else d->selected = row->device;
            return true;
        }
        return false;

    case WE_KEY_DOWN:
        if (ev->key == KK_UP && d->selected > 0) { d->selected--; return true; }
        if (ev->key == KK_DOWN && d->selected < d->count - 1) { d->selected++; return true; }
        return false;

    case WE_CLOSE:
        free(d);
        return false;

    default:
        return false;
    }
}

void app_devices_launch(wm_t *wm) {
    devices_t *d = calloc(1, sizeof *d);
    if (!d) return;

    d->selected = -1;
    d->hover = -1;
    for (int i = 0; i < GROUP_COUNT; i++) d->open[i] = true;
    collect(d);

    window_t *w = desktop_new_window(wm, "Device Manager", ICON_USB, 900, 560,
                                     devices_proc, d);
    if (!w) { free(d); return; }
    w->min_w = 700;
    w->min_h = 400;
}
