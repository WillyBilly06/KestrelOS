/* app_settings.c - Settings.
 *
 * A settings window is a sidebar of pages and, on each page, a column of rows:
 * a label on the left saying what the thing is, a line under it saying what it
 * means, and the control that changes it on the right.  That shape is not an
 * accident of fashion - it is what lets someone find a setting they have never
 * looked for before, because everything is in the same place on every page.
 *
 * Everything here changes something real.  The Wi-Fi switch stops and starts
 * the radio; picking a network runs the whole join, including the four-way
 * handshake; the display page writes the choice where the loader will read it
 * on the next start.  Nothing on any page is decoration.
 */
#include "desktop.h"
#include "refresh_modes.h"
#include "display_settings.h"
#include "settings_navigation.h"
#include "settings_appearance.h"

/* ------------------------------------------------------------------ pages */

typedef enum {
    PAGE_NETWORK,
    PAGE_DISPLAY,
    PAGE_SOUND,
    PAGE_APPEARANCE,
    PAGE_STORAGE,
    PAGE_DEVICES,
    PAGE_ABOUT,
    PAGE_COUNT,
} page_t;

static const struct { const char *name; icon_id icon; } pages[PAGE_COUNT] = {
    { "Network",    ICON_WIFI     },
    { "Display",    ICON_DISPLAY  },
    { "Sound",      ICON_SOUND    },
    { "Appearance", ICON_PALETTE  },
    { "Storage",    ICON_DISK     },
    { "Devices",    ICON_CHIP     },
    { "About",      ICON_KESTREL  },
};

/* Sized from what it holds, not declared.
 *
 * The sidebar was 190 pixels at every scale, chosen once for the small font.
 * At scale two the entries doubled and the column did not: "Network" showed as
 * "Netwo...", "Appearance" as "Appea...", and the window's own name was drawn
 * clipped.  A column whose job is to show seven words must be as wide as the
 * widest of them - measured, every time, in whatever font and scale are
 * current. */
static int sidebar_width(void) {
    int w = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int need = gui_sidebar_item_width(pages[i].icon, pages[i].name);
        if (need > w) w = need;
    }
    int least = gui_sidebar_item_width(ICON_SETTINGS, "Settings");
    if (least > w) w = least;
    return w + 8 * gui_scale();
}

#define SIDEBAR_W   (sidebar_width())
#define SIDEBAR_ROW ((gui_font_height(FONT_UI) * 5) / 3)
#define PAD         (24 * gui_scale())

/* The firmware alone can offer twenty-five, and a driven adapter adds most of
 * the useful range on top of that.  Cutting the list short does not lose the
 * least important entries - it loses whichever happened to be added last. */
#define MAX_MODES    64
#define MAX_NETWORKS 24
#define MAX_VOLUMES  8

typedef struct {
    page_t page;
    int    hover_page;
    int    pointer_x, pointer_y;
    char   status[200];
    uint64_t status_at;

    /* Network */
    kwifiinfo_t wifi;
    bool        have_wifi;
    bool        wifi_on;
    bool        scanning;
    uint64_t    scan_started;
    kwifinet_t  networks[MAX_NETWORKS];
    int         network_count;
    int         selected_network;
    int         hover_network;
    bool        asking_password;
    char        password[64];
    textfield_t password_field;
    knetinfo_t  wired;
    bool        have_wired;

    /* Display */
    kvideomode_t modes[MAX_MODES];
    int          mode_count;
    int          current_mode;
    int          chosen_mode;
    int          hover_mode;
    int          mode_scroll;
    kdisplayinfo_t display;
    display_settings_t native_display;
    rect_t       display_mode_rects[10];
    int          display_mode_count;

    /* The refresh rates worth offering for this panel (millihertz), from
     * refresh_options() over what the display reported. */
    uint32_t     refresh_rates[12];
    int          refresh_rate_count;

    /* Sound */
    kaudioinfo_t audio;
    bool         have_audio;

    /* What the output converter offers, read once when the page opens.  The
     * chips below are drawn from this, so a codec that offers three rates
     * shows three chips and one that offers eight shows eight. */
    uint32_t     audio_rates[12];
    int          audio_rate_count;
    uint8_t      audio_depths[5];
    int          audio_depth_count;

    /* The output and input endpoints the codec offers, and which is active
     * (from /dev/audio ioctl 10), so the user can pick where sound goes and
     * where it is recorded from. */
    struct { uint8_t node, device, is_output, current; } audio_eps[24];
    int          audio_ep_count;
    bool         audio_muted;

    /* Hit-rects kept from the last Sound paint.  The page outgrew fixed
     * offsets once it gained device pickers, so each control records where it
     * was actually drawn and the click handler reads that - paint and hit test
     * cannot drift, and nothing lands on top of anything else. */
    rect_t       rc_mute;
    rect_t       rc_out[24]; int rc_out_n;
    rect_t       rc_in[24];  int rc_in_n;
    int          sound_scroll, sound_total;
    bool         sound_dragging;
    rect_t       rc_rate[12];  int rc_rate_n;
    rect_t       rc_depth[5];  int rc_depth_n;

    /* Appearance */
    bool dark;
    int  theme;      /* which of the built-in looks   */
    int  accent;     /* which accent it is wearing    */
    int  wallpaper;
    bool animations;
    int appearance_scroll;
    bool appearance_dragging;

    /* Storage */
    kblockinfo_t volumes[MAX_VOLUMES];
    int          volume_count;

    /* Devices - a summary; the full list is its own window. */
    int  pci_count, usb_count;
} settings_t;

/* ------------------------------------------------------------------ layout */

static settings_navigation_t settings_nav(surface_t *s) {
    return settings_navigation(s->width, s->height, gui_scale(), gui_font_height(FONT_UI),
                               SIDEBAR_W, PAGE_COUNT);
}
static rect_t sidebar_rect(surface_t *s) { return rect_make(0, 0, settings_nav(s).sidebar, s->height); }
static rect_t sidebar_item_rect(surface_t *s, int i) {
    /* Below the window's name, which is itself set in the scaled font now. */
    int top = 20 * gui_scale() + gui_font_height(FONT_UI) + 16 * gui_scale();
    return rect_make(0, top + i * SIDEBAR_ROW, settings_nav(s).sidebar, SIDEBAR_ROW - 4 * gui_scale());
}
static rect_t body_rect(surface_t *s) {
    return settings_nav(s).body;
}

static void say(settings_t *st, const char *text) {
    strlcpy(st->status, text, sizeof st->status);
    st->status_at = uptime_ms();
}

/* --------------------------------------------------------------- gathering */

static void load_wifi(settings_t *st) {
    st->have_wifi = enum_wifi(0, &st->wifi) == 0;
    if (st->have_wifi) st->wifi_on = st->wifi.enabled != 0;

    st->network_count = 0;
    for (uint32_t i = 0; i < MAX_NETWORKS; i++) {
        kwifinet_t n;
        if (enum_scan(i, &n) < 0) break;
        st->networks[st->network_count++] = n;
    }

    st->have_wired = false;
    for (uint32_t i = 0; i < 8; i++) {
        knetinfo_t n;
        if (enum_net(i, &n) < 0) break;
        /* The wireless interface appears here too; the wired one is the other. */
        if (st->have_wifi && !strcmp(n.name, st->wifi.name)) continue;
        st->wired = n;
        st->have_wired = true;
        break;
    }
}

static void add_driver_modes(settings_t *st);

static void load_modes(settings_t *st) {
    ds_load(&st->native_display);
    st->mode_count = 0;
    st->current_mode = -1;
    st->chosen_mode = -1;
    memset(&st->display, 0, sizeof st->display);
    enum_display(&st->display);

    for (uint32_t i = 0; i < MAX_MODES; i++) {
        kvideomode_t m;
        if (enum_videomode(i, &m) < 0) break;
        st->modes[st->mode_count++] = m;
    }

    /* Then everything the adapter can reach that the firmware never mentioned,
     * which on a driven adapter is most of the useful range. */
    add_driver_modes(st);

    /* Largest first: what someone is looking for is nearly always near the
     * top of that order, and it puts the panel's own mode near the front. */
    for (int i = 1; i < st->mode_count; i++) {
        kvideomode_t key = st->modes[i];
        int j = i - 1;
        while (j >= 0 && (int)st->modes[j].width * st->modes[j].height <
                         (int)key.width * key.height) {
            st->modes[j + 1] = st->modes[j];
            j--;
        }
        st->modes[j + 1] = key;
    }
    for (int i = 0; i < st->mode_count; i++)
        if (st->modes[i].current) st->current_mode = i;

    /* The refresh rates this panel accepts, turned from its reported ceiling
     * into the handful of standard rates at or below it (plus the current one).
     * Empty on a virtual display that reports no EDID, which is why the chooser
     * only draws when there is more than one to choose between. */
    st->refresh_rate_count = refresh_options(st->display.max_refresh_mhz,
                                             st->display.refresh_mhz,
                                             st->refresh_rates,
                                             (int)(sizeof st->refresh_rates /
                                                   sizeof st->refresh_rates[0]));
}

static void load_storage(settings_t *st) {
    st->volume_count = 0;
    for (uint32_t i = 0; i < MAX_VOLUMES; i++) {
        kblockinfo_t b;
        if (enum_block(i, &b) < 0) break;
        st->volumes[st->volume_count++] = b;
    }
}

static void load_devices(settings_t *st) {
    st->pci_count = 0;
    st->usb_count = 0;
    for (uint32_t i = 0; i < 128; i++) {
        kpciinfo_t p;
        if (enum_pci(i, &p) < 0) break;
        st->pci_count++;
    }
    for (uint32_t i = 0; i < 32; i++) {
        kusbinfo_t u;
        if (enum_usb(i, &u) < 0) break;
        st->usb_count++;
    }
}

/* ---------------------------------------------------------------- network */

static rect_t wifi_switch_rect(surface_t *s) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    return gui_switch_rect(b.x + b.w - 46 * k, b.y + 74 * k);
}
static rect_t network_row_rect(surface_t *s, int i) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    /* Offset and stride both scale: a bare 150/42/40 drew the Wi-Fi list on top
     * of itself on a 2x display, the same smushing the mode list had. */
    return rect_make(b.x, b.y + 150 * k + i * 42 * k, b.w, 40 * k);
}
static rect_t scan_button_rect(surface_t *s) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    return rect_make(b.x + b.w - 110 * k, b.y + 118 * k, 110 * k, 28 * k);
}
static rect_t password_field_rect(surface_t *s) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    return rect_make(b.x + 20 * k, b.y + b.h - 92 * k, b.w - 160 * k, 32 * k);
}
static rect_t password_join_rect(surface_t *s) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    return rect_make(b.x + b.w - 130 * k, b.y + b.h - 92 * k, 130 * k, 32 * k);
}

static const char *security_word(uint8_t s) {
    switch (s) {
    case 0: return "open";
    case 1: return "WEP";
    case 2: return "WPA";
    case 3: return "WPA2";
    case 4: return "WPA3";
    default: return "secured";
    }
}

/* Signal strength as bars, which is what people read rather than a number. */
static void draw_signal(surface_t *s, int x, int y, int dbm, colour_t on, colour_t off) {
    const int k = gui_scale();
    int bars = dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -78 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = (4 + i * 3) * k;
        gui_round_rect_aa(s, rect_make(x + i * 5 * k, y + 13 * k - h, 3 * k, h), 1 * k,
                          i < bars ? on : off);
    }
}

static void paint_network(settings_t *st, surface_t *s, int mx, int my) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    int y = gui_page_header(s, b.x, b.y, b.w, "Network",
                            "Wireless and wired connections.");

    if (!st->have_wifi) {
        gui_text(s, FONT_UI, b.x, y, "No wireless interface was found.", g_theme.text_dim);
        y += gui_font_height(FONT_UI) * 2;
    } else {
        /* The switch. */
        char detail[128];
        if (!st->wifi_on) {
            strlcpy(detail, "The radio is off.", sizeof detail);
        } else if (st->wifi.state == 5) {
            snprintf(detail, sizeof detail, "Connected to \"%s\"  %s  %d dBm",
                     st->wifi.ssid, security_word(st->wifi.security),
                     st->wifi.signal_dbm);
        } else if (!st->wifi.radio_up) {
            snprintf(detail, sizeof detail, "On, but the radio did not start%s",
                     st->wifi.firmware_needed && !st->wifi.firmware_present
                     ? " - its firmware is missing" : "");
        } else {
            strlcpy(detail, "On, not connected to anything.", sizeof detail);
        }

        rect_t label = rect_make(b.x, b.y + 62 * k, b.w - 60 * k, 40 * k);
        gui_text(s, FONT_UI, label.x, label.y + 2 * k, "Wi-Fi", g_theme.text_bright);
        gui_text_clipped(s, FONT_UI, label.x, label.y + 2 * k + gui_font_height(FONT_UI) + 3 * k,
                         label.w, detail, g_theme.text_dim);

        rect_t sw = wifi_switch_rect(s);
        gui_switch(s, sw, st->wifi_on, rect_contains(sw, mx, my), true);

        gui_text_clipped(s, FONT_UI, b.x, b.y + 124 * k, b.w - 130 * k,
                         st->wifi.model, g_theme.text_dim);

        if (st->wifi_on) {
            rect_t scan = scan_button_rect(s);
            gui_button(s, scan, st->scanning ? "Scanning" : "Scan",
                       rect_contains(scan, mx, my), false, !st->scanning && st->wifi.radio_up);
            if (st->scanning)
                gui_spinner(s, b.x + b.w - 130 * k, b.y + 132 * k, 8 * k, g_theme.accent);
        }

        y = b.y + 150 * k;

        if (!st->wifi_on) {
            gui_text(s, FONT_UI, b.x, y + 10 * k,
                     "Switch Wi-Fi on to see the networks around you.",
                     g_theme.text_dim);
        } else if (!st->wifi.radio_up) {
            gui_text_clipped(s, FONT_UI, b.x, y + 10 * k, b.w,
                             "Radio startup incomplete. Scanning is unavailable.", g_theme.text_dim);
        } else if (!st->network_count) {
            gui_text(s, FONT_UI, b.x, y + 10 * k,
                     st->scanning ? "Looking for networks..."
                                  : "No networks found yet.  Press Scan.",
                     g_theme.text_dim);
        } else {
            for (int i = 0; i < st->network_count && i < 6; i++) {
                rect_t r = network_row_rect(s, i);
                if (r.y + r.h > b.y + b.h - 110 * k) break;

                bool selected = (i == st->selected_network);
                bool joined = st->wifi.state == 5 &&
                              !strcmp(st->networks[i].ssid, st->wifi.ssid);

                if (selected) gui_round_rect_aa(s, r, 6 * k, g_theme.selection);
                else if (i == st->hover_network)
                    gui_round_rect_aa(s, r, 6 * k, colour_shade(g_theme.window, 8));

                colour_t tc = selected ? g_theme.selection_text : g_theme.text;
                draw_signal(s, r.x + 12 * k, r.y + 12 * k, st->networks[i].signal_dbm,
                            selected ? tc : g_theme.accent, g_theme.field);

                gui_text_clipped(s, FONT_UI, r.x + 42 * k, r.y + 4 * k, r.w - 200 * k,
                                 st->networks[i].ssid[0] ? st->networks[i].ssid
                                                         : "(hidden)", tc);
                char sub[64];
                snprintf(sub, sizeof sub, "channel %u  %s",
                         st->networks[i].channel,
                         security_word(st->networks[i].security));
                gui_text_clipped(s, FONT_UI, r.x + 42 * k,
                                 r.y + 4 * k + gui_font_height(FONT_UI) + 1 * k, r.w - 200 * k,
                                 sub, selected ? tc : g_theme.text_dim);

                if (joined)
                    gui_badge(s, r.x + r.w - 110 * k, r.y + 10 * k, "Connected", g_theme.success);
            }
        }

        /* The password prompt appears under the list once a network is
         * chosen, rather than as a separate window: it is part of the same
         * action. */
        if (st->asking_password && st->selected_network >= 0) {
            rect_t f = password_field_rect(s);
            char prompt[96];
            snprintf(prompt, sizeof prompt, "Password for \"%s\"",
                     st->networks[st->selected_network].ssid);
            gui_text(s, FONT_UI, f.x, f.y - gui_font_height(FONT_UI) - 5 * k, prompt,
                     g_theme.text);
            gui_textfield_draw(s, f, &st->password_field);

            rect_t join = password_join_rect(s);
            gui_button(s, join, "Join", rect_contains(join, mx, my), false, true);
        }
    }

    /* The wired interface, underneath. */
    if (st->have_wired) {
        int wy = b.y + b.h - 42 * k;
        gui_hline(s, b.x, wy - 12 * k, b.w, g_theme.window_border);
        char line[160];
        char ip[20];
        format_ipv4(st->wired.ip, ip, sizeof ip);
        snprintf(line, sizeof line, "%s   %s   %s%s", st->wired.name,
                 st->wired.ip ? ip : "no address",
                 st->wired.link_up ? "link up" : "no link",
                 st->wired.link_up && st->wired.link_speed_mbps ? "" : "");
        gui_text(s, FONT_UI, b.x, wy, line, g_theme.text_dim);
    }
}

/* ---------------------------------------------------------------- display */

/* Where the list of sizes begins.  Not a fixed distance down the page: what is
 * above it - the heading, its explanation, the size in use, the monitor's name
 * - is however many lines it turns out to be, and a fixed offset draws the
 * first few rows straight through them. */
static int mode_list_top;

static rect_t mode_row_rect(surface_t *s, int i) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    int top = mode_list_top > 0 ? mode_list_top : b.y + 130 * k;
    /* Tall enough for the two lines some rows carry (the size, and "in use now"
     * or "the panel's own" under it).  This MUST scale with the UI: at 1x two
     * lines fit in thirty-eight pixels, but on a 2x display the font is twice
     * as tall and a fixed thirty-eight draws each row's second line over the
     * next row - which is the "smushed text" this page used to show. */
    int rh = 2 * gui_font_height(FONT_UI) + 4 * k;
    return rect_make(b.x, top + i * (rh + 2 * k), b.w - 20 * k, rh);
}
static rect_t apply_mode_rect(surface_t *s) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    int bw = 150 * k, bh = gui_font_height(FONT_UI) + 12 * k;
    return rect_make(b.x + b.w - bw, b.y + b.h - bh - 8 * k, bw, bh);
}

/* A refresh rate as text: a whole number where it divides evenly (240 Hz), two
 * decimals otherwise (59.94 Hz), which is exactly how panels report the 60-that-
 * is-really-59.94 the video standards define. */
static void format_hz(uint32_t mhz, char *buf, size_t cap) {
    if (mhz % 1000 == 0)
        snprintf(buf, cap, "%u Hz", mhz / 1000);
    else
        snprintf(buf, cap, "%u.%02u Hz", mhz / 1000, (mhz % 1000) / 10);
}

/* Where the refresh-rate chips sit, set while painting so a click can find the
 * same chips without re-deriving the layout above them. */
static int refresh_row_top;

/* The chip for refresh rate i, laid left to right with each chip only as wide
 * as its own label - the same shape as the sound page's format chips. */
static rect_t refresh_chip_rect(settings_t *st, surface_t *s, int i) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    int x = b.x;
    for (int j = 0; j < i && j < st->refresh_rate_count; j++) {
        char t[24];
        format_hz(st->refresh_rates[j], t, sizeof t);
        x += gui_text_width(FONT_UI, t) + 20 * k + 8 * k;   /* padding + gap */
    }
    char t[24];
    format_hz(st->refresh_rates[i], t, sizeof t);
    return rect_make(x, refresh_row_top, gui_text_width(FONT_UI, t) + 20 * k,
                     gui_font_height(FONT_UI) + 12 * k);
}

/* Two group policies plus one generated single-screen policy per connected
 * output.  This scales to hot-plugged screens without hard-coding "other" as
 * screen 2. */
static int display_choice_count(const settings_t *st) {
    int n = 2 + (int)st->display.display_count;
    return n > 10 ? 10 : n;
}
static void display_choice(int choice, char *label, size_t lcap,
                           char *key, size_t kcap, int *mode) {
    if (choice == 0) {
        strlcpy(label, "Extend", lcap); strlcpy(key, "extend", kcap); *mode = 1;
    } else if (choice == 1) {
        strlcpy(label, "Duplicate", lcap); strlcpy(key, "mirror", kcap); *mode = 2;
    } else {
        int screen = choice - 1;                 /* user-facing, one based */
        snprintf(label, lcap, "Only screen %d", screen);
        snprintf(key, kcap, "only:%d", screen);
        *mode = 16 + screen - 1;                 /* DL_ONLY_OUTPUT(index) */
    }
}

/* The sizes a driven adapter can reach, beyond whatever the firmware offered.
 *
 * The firmware's list is a handful of modes it thought were reasonable before
 * this system was running. The adapter's own limit is set by how much video
 * memory it has and how wide it can scan out - and on anything modern that is
 * far past the list. A system that only ever shows the firmware's list cannot
 * be asked for the resolution the screen actually is. */
static void add_driver_modes(settings_t *st) {
    if (!st->display.driver_present) return;
    if (!st->display.driver_max_width || !st->display.driver_max_height) return;

    /* Largest first, so that if the list does fill up it is the small sizes
     * that are left out rather than the ones somebody went looking for. */
    static const struct { uint16_t w, h; } sizes[] = {
        { 7680, 4320 }, { 6016, 3384 }, { 5120, 2880 }, { 5120, 1440 },
        { 4096, 2160 }, { 3840, 2160 }, { 3840, 1600 }, { 3440, 1440 },
        { 2560, 1600 }, { 2560, 1440 }, { 2560, 1080 }, { 2048, 1152 },
        { 1920, 1200 }, { 1920, 1080 }, { 1680, 1050 }, { 1600,  900 },
        { 1440,  900 }, { 1366,  768 }, { 1280,  720 },
    };

    uint64_t vram = st->display.driver_vram_bytes;

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        if (st->mode_count >= (int)(sizeof st->modes / sizeof st->modes[0])) break;
        if (sizes[i].w > st->display.driver_max_width) continue;
        if (sizes[i].h > st->display.driver_max_height) continue;

        /* It has to fit in video memory, twice over: what is being scanned out
         * and what is being drawn into. */
        uint64_t needed = (uint64_t)sizes[i].w * sizes[i].h * 4ull;
        if (vram && needed > vram) continue;

        bool already = false;
        for (int j = 0; j < st->mode_count; j++)
            if (st->modes[j].width == sizes[i].w &&
                st->modes[j].height == sizes[i].h) { already = true; break; }
        if (already) continue;

        st->modes[st->mode_count].width = sizes[i].w;
        st->modes[st->mode_count].height = sizes[i].h;
        st->mode_count++;
    }

    /* Largest first, which is the order somebody looking for the biggest one
     * expects to find it in. */
    for (int i = 1; i < st->mode_count; i++) {
        kvideomode_t key = st->modes[i];
        int j = i - 1;
        while (j >= 0 &&
               (uint32_t)st->modes[j].width * st->modes[j].height <
               (uint32_t)key.width * key.height) {
            st->modes[j + 1] = st->modes[j];
            j--;
        }
        st->modes[j + 1] = key;
    }
}

static void paint_display(settings_t *st, surface_t *s, int mx, int my) {
    if (st->native_display.output_count) {
        ds_paint(&st->native_display, s, body_rect(s), mx, my);
        return;
    }
    rect_t b = body_rect(s);
    int y = gui_page_header(s, b.x, b.y, b.w, "Display",
                            "The resolution. Where the display adapter is "
                            "driven this changes immediately; otherwise the "
                            "mode belongs to the firmware and the change waits "
                            "for the next start.");

    char line[160];
    if (st->current_mode >= 0) {
        snprintf(line, sizeof line, "Now: %u x %u",
                 st->modes[st->current_mode].width,
                 st->modes[st->current_mode].height);
        if (st->display.refresh_mhz) {
            char hz[24];
            format_hz(st->display.refresh_mhz, hz, sizeof hz);
            snprintf(line + strlen(line), sizeof line - strlen(line),
                     " at %s", hz);
        }
        gui_text(s, FONT_UI, b.x, y, line, g_theme.text);
        y += gui_font_height(FONT_UI) + 2;
    }
    if (st->display.present && st->display.model[0]) {
        snprintf(line, sizeof line, "Monitor: %.4s %.16s",
                 st->display.manufacturer, st->display.model);
        gui_text(s, FONT_UI, b.x, y, line, g_theme.text_dim);
        y += gui_font_height(FONT_UI) + 2;
    }

    /* Refresh rate (#5), as chips like the sound page's formats: the rates this
     * panel accepts, the one in use marked.  Only shown when there is a real
     * choice - a virtual display that reports one rate (or none) shows none, so
     * the chooser never offers a decision that is not there to make. */
    const int k = gui_scale();
    if (st->refresh_rate_count > 1) {
        gui_text(s, FONT_UI, b.x, y, "Refresh rate", g_theme.text);
        y += gui_font_height(FONT_UI) + 6 * k;
        refresh_row_top = y;
        for (int i = 0; i < st->refresh_rate_count; i++) {
            rect_t r = refresh_chip_rect(st, s, i);
            char t[24];
            format_hz(st->refresh_rates[i], t, sizeof t);
            bool current = (st->refresh_rates[i] == st->display.refresh_mhz);
            gui_button(s, r, t, rect_contains(r, mx, my), current, true);
        }
        y += gui_font_height(FONT_UI) + 12 * k + 12 * k;
    }

    /* Multiple displays (#7): the three arrangements, as buttons.  Only shown
     * when a second display is actually present (the VM's single virtual one
     * reports one), so it never offers an arrangement there is nothing to
     * arrange.  Applied at the next start - the loader sets it up before the
     * desktop, which is the clean point to rearrange the scan-outs. */
    if (st->display.display_count > 1) {
        gui_text(s, FONT_UI, b.x, y, "Multiple displays", g_theme.text);
        y += gui_font_height(FONT_UI) + 6 * k;
        int x = b.x, row_h = gui_font_height(FONT_UI) + 12 * k;
        st->display_mode_count = display_choice_count(st);
        for (int i = 0; i < st->display_mode_count; i++) {
            char label[32], key[16]; int mode;
            display_choice(i, label, sizeof label, key, sizeof key, &mode);
            int w = gui_text_width(FONT_UI, label) + 20 * k;
            if (x != b.x && x + w > b.x + b.w) { x = b.x; y += row_h + 8 * k; }
            rect_t r = rect_make(x, y, w, row_h);
            st->display_mode_rects[i] = r;
            gui_button(s, r, label, rect_contains(r, mx, my), false, true);
            x += w + 8 * k;
        }
        y += row_h + 12 * k;
    } else {
        st->display_mode_count = 0;
    }

    /* Now that everything above is drawn, the list knows where to start. */
    mode_list_top = y + 12;

    for (int i = 0; i < st->mode_count; i++) {
        rect_t r = mode_row_rect(s, i - st->mode_scroll);
        if (r.y < mode_list_top) continue;
        if (r.y + r.h > b.y + b.h - 56) break;

        bool chosen = (i == st->chosen_mode);
        bool current = (i == st->current_mode);
        snprintf(line, sizeof line, "%u x %u", st->modes[i].width, st->modes[i].height);

        char detail[48];
        detail[0] = 0;
        if (current) strlcpy(detail, "in use now", sizeof detail);
        else if (st->display.present &&
                 st->display.native_width == st->modes[i].width &&
                 st->display.native_height == st->modes[i].height)
            strlcpy(detail, "the panel's own", sizeof detail);

        gui_list_row(s, r, line, detail[0] ? detail : NULL, ICON_NONE,
                     chosen, i == st->hover_mode);
    }

    if (st->chosen_mode >= 0 && st->chosen_mode != st->current_mode) {
        rect_t apply = apply_mode_rect(s);
        gui_button(s, apply, "Use this size", rect_contains(apply, mx, my),
                   false, true);
    }
}

/* ------------------------------------------------------------------ sound */

/* Ask the driver what the converter offers.  Slot layout is described where
 * the device answers it, in hda.c. */
static void load_audio_formats(settings_t *st) {
    st->audio_rate_count = 0;
    st->audio_depth_count = 0;

    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) return;

    uint32_t slots[20];
    memset(slots, 0, sizeof slots);
    if (ioctl(fd, 8, slots) == 0) {
        int nr = slots[0] <= 12 ? (int)slots[0] : 0;
        for (int i = 0; i < nr; i++) st->audio_rates[i] = slots[1 + i];
        st->audio_rate_count = nr;

        int nd = slots[13] <= 5 ? (int)slots[13] : 0;
        for (int i = 0; i < nd; i++) st->audio_depths[i] = (uint8_t)slots[14 + i];
        st->audio_depth_count = nd;
    }
    close(fd);
}

/* The output and input pins the codec offers, and which is active, so the user
 * can pick where sound goes and what it records from.  Slot layout is in hda.c
 * beside the ioctl that answers it. */
static void load_audio_endpoints(settings_t *st) {
    st->audio_ep_count = 0;
    st->audio_muted = false;

    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) return;

    uint32_t buf[128];
    memset(buf, 0, sizeof buf);
    if (ioctl(fd, 10, buf) == 0) {
        if (buf[0] > 24) { close(fd); return; }
        int n = (int)buf[0];
        for (int i = 0; i < n; i++) {
            uint32_t *e = &buf[1 + i * 4];
            st->audio_eps[i].node      = (uint8_t)e[0];
            st->audio_eps[i].device    = (uint8_t)e[1];
            st->audio_eps[i].is_output = (uint8_t)e[2];
            st->audio_eps[i].current   = (uint8_t)e[3];
        }
        st->audio_ep_count = n;
        st->audio_muted = buf[1 + n * 4] != 0;
    }
    close(fd);
}

/* The HDA "default device" code a pin was wired as, named for a human. */
static const char *audio_device_label(uint8_t dev) {
    switch (dev) {
    case 0x0: return "Line Out";
    case 0x1: return "Speakers";
    case 0x2: return "Headphones";
    case 0x3: return "CD";
    case 0x4: return "S/PDIF Out";
    case 0x5: return "Digital Out";
    case 0x8: return "Line In";
    case 0x9: return "Aux";
    case 0xA: return "Microphone";
    case 0xB: return "Telephony";
    case 0xC: return "S/PDIF In";
    case 0xD: return "Digital In";
    default:  return "Audio";
    }
}

/* Hand the choice to the driver.  It refuses anything the converter did not
 * offer, so the worst a stray click can do is nothing. */
static void apply_audio_format(settings_t *st, uint32_t rate, uint8_t bits) {
    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) { say(st, "The audio device would not open."); return; }

    uint32_t args[2] = { rate, bits };
    if (ioctl(fd, 9, args) == 0) {
        st->have_audio = enum_audio(&st->audio) == 0 && st->audio.present;
        char line[80];
        snprintf(line, sizeof line, "Output is now %u Hz at %u bits.", rate, bits);
        say(st, line);
    } else {
        say(st, "The device refused that format.");
    }
    close(fd);
}

/* Move playback or recording to a pin the user chose (ioctl 11/12), or mute the
 * output (13).  Each re-reads the endpoint list so the active mark follows. */
static void select_audio_endpoint(settings_t *st, uint8_t node, bool is_output) {
    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) { say(st, "The audio device would not open."); return; }
    uint32_t a = node;
    if (ioctl(fd, is_output ? 11 : 12, &a) == 0) {
        load_audio_endpoints(st);
        st->have_audio = enum_audio(&st->audio) == 0 && st->audio.present;
        if (is_output) load_audio_formats(st);
        bool selected = false;
        for (int i = 0; i < st->audio_ep_count; i++)
            if (st->audio_eps[i].node == node &&
                (bool)st->audio_eps[i].is_output == is_output && st->audio_eps[i].current)
                selected = true;
        say(st, selected ? (is_output ? "Output selected." : "Input selected.")
                         : "The driver did not confirm that selection.");
    } else {
        say(st, "That device could not be selected.");
    }
    close(fd);
}

static void toggle_audio_mute(settings_t *st) {
    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) return;
    uint32_t a = st->audio_muted ? 0 : 1;
    if (ioctl(fd, 13, &a) == 0) {
        load_audio_endpoints(st);
        say(st, st->audio_muted ? "Output muted." : "Output unmuted.");
    } else {
        say(st, "The output did not accept the mute change.");
    }
    close(fd);
}

/* Draw one pill chip at (*x, *y), wrapping to the next line when the row is
 * full, and advance *x past it.  Every chip row on this page uses it, so they
 * all size to their own label and none can ever overlap another. */
static rect_t sound_chip(surface_t *s, rect_t b, int *x, int *y,
                         const char *label, int mx, int my, bool active) {
    const int k = gui_scale();
    int w = gui_text_width(FONT_UI, label) + 24 * k;
    if (w > b.w) w = b.w;
    int h = gui_font_height(FONT_UI) + 12 * k;
    if (*x > b.x && *x + w > b.x + b.w) { *x = b.x; *y += h + 8 * k; }
    rect_t r = rect_make(*x, *y, w, h);
    gui_button(s, r, label, rect_contains(r, mx, my), active, true);
    *x += w + 8 * k;
    return r;
}

/* One row of chips for a set of endpoints of a given direction, under a section
 * label.  Records each chip's rect for the click handler.  Advances *y past the
 * row.  Returns how many chips it drew. */
static int sound_endpoint_row(settings_t *st, surface_t *s, rect_t b, int *y,
                              const char *label, bool outputs,
                              rect_t *rects, int mx, int my) {
    const int k = gui_scale();
    int chip_h = gui_font_height(FONT_UI) + 12 * k;
    int n = 0;
    for (int i = 0; i < st->audio_ep_count; i++)
        if ((bool)st->audio_eps[i].is_output == outputs) n++;
    gui_section(s, b.x, *y, b.w, label);
    *y += gui_font_height(FONT_UI) + 8 * k;
    if (!n) {
        gui_text_clipped(s, FONT_UI, b.x, *y, b.w,
                         outputs ? "No output route available." : "No recording route available.",
                         g_theme.text_dim);
        *y += gui_font_height(FONT_UI) + 16 * k;
        return 0;
    }

    int row_y = *y, drawn = 0;
    for (int i = 0; i < st->audio_ep_count && drawn < 24; i++) {
        if ((bool)st->audio_eps[i].is_output != outputs) continue;
        const char *nm = audio_device_label(st->audio_eps[i].device);
        char title[80];
        snprintf(title, sizeof title, "%s - port %u%s", nm, st->audio_eps[i].node,
                 st->audio_eps[i].current ? " (selected)" : "");
        rect_t r = rect_make(b.x, row_y, b.w, chip_h);
        rects[drawn++] = r;
        gui_button(s, r, title, rect_contains(r, mx, my), st->audio_eps[i].current, true);
        row_y += chip_h + 8 * k;
    }
    *y = row_y + 8 * k;
    return drawn;
}

static void paint_sound_content(settings_t *st, surface_t *s, int mx, int my) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    b.w -= b.w > 20 * k ? 20 * k : 0;
    b.y -= st->sound_scroll;
    int fh = gui_font_height(FONT_UI);
    int y = gui_page_header(s, b.x, b.y, b.w, "Sound",
                            "HD Audio output, input and format.");

    st->rc_out_n = st->rc_in_n = st->rc_rate_n = st->rc_depth_n = 0;
    st->rc_mute = rect_make(0,0,0,0);

    if (!st->have_audio) {
        gui_text_clipped(s, FONT_UI, b.x, y, b.w, "No HD Audio device was found.", g_theme.text_dim);
        st->sound_total = y + fh - b.y;
        return;
    }

    int mute_w = gui_text_width(FONT_UI, "Unmute output") + 28 * k;
    if (mute_w > b.w) mute_w = b.w;
    st->rc_mute = rect_make(b.x, y, mute_w, fh + 12 * k);
    gui_button(s, st->rc_mute, st->audio_muted ? "Unmute output" : "Mute output",
               rect_contains(st->rc_mute, mx, my), st->audio_muted, st->audio.output_ready);
    y += fh + 24 * k;

    /* Output and input device pickers - the pins the codec offers.  Clicking
     * one routes to it immediately (select_audio_endpoint). */
    st->rc_out_n = sound_endpoint_row(st, s, b, &y, "Output", true,
                                      st->rc_out, mx, my);
    st->rc_in_n  = sound_endpoint_row(st, s, b, &y, "Input", false,
                                      st->rc_in, mx, my);

    /* The formats the converter itself offers, as chips.  Clicking one takes
     * effect immediately; the driver refuses anything the hardware did not
     * list, so there is nothing here to get wrong. */
    st->rc_rate_n = 0;
    if (st->audio_rate_count) {
        gui_section(s, b.x, y, b.w - 40 * k, "Sample rate");
        y += fh + 8 * k;
        int x = b.x, row_y = y;
        for (int i = 0; i < st->audio_rate_count && i < 12; i++) {
            char t[16];
            snprintf(t, sizeof t, "%u", st->audio_rates[i]);
            st->rc_rate[st->rc_rate_n++] =
                sound_chip(s, b, &x, &row_y, t, mx, my,
                           st->audio_rates[i] == st->audio.sample_rate);
        }
        y = row_y + (fh + 12 * k) + 16 * k;
    }

    st->rc_depth_n = 0;
    if (st->audio_depth_count) {
        gui_section(s, b.x, y, b.w - 40 * k, "Bit depth");
        y += fh + 8 * k;
        int x = b.x, row_y = y;
        for (int i = 0; i < st->audio_depth_count && i < 5; i++) {
            char t[16];
            snprintf(t, sizeof t, "%u-bit", st->audio_depths[i]);
            st->rc_depth[st->rc_depth_n++] =
                sound_chip(s, b, &x, &row_y, t, mx, my,
                           st->audio_depths[i] == st->audio.bits);
        }
        y = row_y + (fh + 12 * k) + 16 * k;
    }

    /* What is actually playing, dim, at the foot of the page. */
    gui_hline(s, b.x, y, b.w - 40 * k, g_theme.window_border);
    y += 12 * k;
    char info[96];
    snprintf(info, sizeof info, "%s%s%s",
             st->audio.controller[0] ? st->audio.controller : "audio device",
             st->audio.codec[0] ? "  -  " : "",
             st->audio.codec[0] ? st->audio.codec : "");
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, info, g_theme.text_dim);
    if (st->audio.sample_rate) {
        char rate[80];
        snprintf(rate, sizeof rate, "Output format: %u Hz, %u channel(s), %u-bit%s",
                 st->audio.sample_rate, st->audio.channels, st->audio.bits,
                 st->audio_muted ? "  (muted)" : "");
        gui_text_clipped(s, FONT_UI, b.x, y + fh + 3 * k, b.w, rate, g_theme.text_dim);
    }
    y += 2 * fh + 16 * k;
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, "USB/Bluetooth routing is not available here yet.", g_theme.text_dim);
    y += fh + 4 * k;
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, "System volume adjustment is not implemented yet.", g_theme.text_dim);
    st->sound_total = y + fh + 8 * k - b.y;
}

static rect_t sound_track(surface_t *s) {
    rect_t b = body_rect(s);
    int width = 10 * gui_scale();
    if (width > b.w) width = b.w;
    return rect_make(b.x + b.w - width,b.y,width,b.h);
}

static void paint_sound(settings_t *st, surface_t *s, int mx, int my) {
    paint_sound_content(st,s,mx,my);
    rect_t b = body_rect(s);
    int clamped = settings_scroll_clamp(st->sound_scroll,st->sound_total,b.h);
    if (clamped != st->sound_scroll) {
        st->sound_scroll = clamped;
        gui_fill(s,b,g_theme.window);
        paint_sound_content(st,s,mx,my);
    }
    if (st->sound_total > b.h)
        gui_scrollbar(s,sound_track(s),st->sound_scroll,b.h,st->sound_total);
}

/* ------------------------------------------------------------- appearance */

static const char *appearance_sizes[3] = { "100%", "200%", "300%" };

static settings_appearance_t appearance_layout(surface_t *s) {
    int k = gui_scale();
    int widths[4] = {1, 32*k, 1, 1};
    int counts[4] = {theme_count(), accent_count(), desktop_wallpaper_count(), 3};
    for (int i = 0; i < counts[0]; i++) {
        const theme_spec_t *t = theme_at(i);
        int width = gui_text_width(FONT_UI,t ? t->name : "") + 96*k;
        if (width > widths[0]) widths[0] = width;
    }
    for (int i = 0; i < counts[2]; i++) {
        int width = gui_text_width(FONT_UI,desktop_wallpaper_name(i)) + 96*k;
        if (width > widths[2]) widths[2] = width;
    }
    for (int i = 0; i < 3; i++) {
        int width = gui_text_width(FONT_UI,appearance_sizes[i]) + 28*k;
        if (width > widths[3]) widths[3] = width;
    }
    return settings_appearance_layout(body_rect(s),k,gui_font_height(FONT_UI),widths,counts);
}
static rect_t appearance_item(settings_t *st, surface_t *s, int group, int index) {
    settings_appearance_t a = appearance_layout(s);
    int offset = settings_scroll_clamp(st->appearance_scroll,a.total_height,a.viewport.h);
    return settings_grid_item(a.groups[group],index,a.viewport.x,a.viewport.y-offset);
}
static rect_t theme_button_rect(settings_t *st, surface_t *s, int i) {
    return appearance_item(st,s,0,i);
}
static rect_t accent_swatch_rect(settings_t *st, surface_t *s, int i) {
    return appearance_item(st,s,1,i);
}
static rect_t wallpaper_button_rect(settings_t *st, surface_t *s, int i) {
    return appearance_item(st,s,2,i);
}
static rect_t scale_button_rect(settings_t *st, surface_t *s, int i) {
    return appearance_item(st,s,3,i);
}
static rect_t animation_switch_rect(settings_t *st, surface_t *s) {
    settings_appearance_t a = appearance_layout(s);
    a.animation.x += a.viewport.x;
    a.animation.y += a.viewport.y-settings_scroll_clamp(st->appearance_scroll,a.total_height,a.viewport.h);
    return a.animation;
}
static void appearance_preview_button(settings_t *st, surface_t *s, rect_t r,
                                      int group, int index, const char *name,
                                      bool hover, bool selected) {
    int k = gui_scale();
    gui_button(s,r,"",hover,selected,true);
    settings_choice_parts_t p = settings_choice_parts(r,k);
    rect_t saved = surface_clip(s);
    surface_set_clip(s,rect_intersection(saved,r));
    if (!rect_empty(p.preview)) {
        colour_t top, bottom;
        desktop_wallpaper_colours(group == 0 ? st->wallpaper : index,&top,&bottom);
        const theme_spec_t *t = group == 0 ? theme_at(index) : NULL;
        if (t && st->wallpaper == 0) { top = t->desktop_top; bottom = t->desktop_bottom; }
        gui_gradient_v(s,p.preview,top,bottom);
        if (t) {
            rect_t mini = rect_make(p.preview.x+8*k,p.preview.y+5*k,
                                    p.preview.w-12*k,p.preview.h-8*k);
            if (!rect_empty(mini)) {
                gui_fill(s,mini,t->window);
                gui_fill(s,rect_make(mini.x,mini.y,mini.w,3*k),accent_at(st->accent));
            }
        }
        gui_frame(s,p.preview,g_theme.control_border);
    }
    gui_text_clipped(s,FONT_UI,p.label.x,p.label.y+(p.label.h-gui_font_height(FONT_UI))/2,
                     p.label.w,name,g_theme.text);
    if (selected) {
        gui_round_frame_aa(s,r,6*k,g_theme.accent);
        gui_icon(s,ICON_CHECK,p.marker.x,p.marker.y,p.marker.w,g_theme.text_bright);
    }
    surface_set_clip(s,saved);
}

static void paint_appearance(settings_t *st, surface_t *s, int mx, int my) {
    settings_appearance_t a = appearance_layout(s);
    st->appearance_scroll = settings_scroll_clamp(st->appearance_scroll,a.total_height,a.viewport.h);
    int k = gui_scale(), y = a.viewport.y-st->appearance_scroll;
    int width = a.groups[0].bounds.w;
    gui_text_clipped(s,FONT_UI,a.viewport.x,y,width,"Appearance",g_theme.text_bright);
    gui_text_clipped(s,FONT_UI,a.viewport.x,y+gui_font_height(FONT_UI)+4*k,
                     width,"Make the desktop yours.",g_theme.text_dim);
    static const char *labels[4] = {"Theme","Accent","Wallpaper","Interface size"};
    for (int group = 0; group < 4; group++) {
        gui_text_clipped(s,FONT_UI,a.viewport.x,y+a.labels[group],width,labels[group],g_theme.text_bright);
        for (int i = 0; i < a.groups[group].count; i++) {
            rect_t r = settings_grid_item(a.groups[group],i,a.viewport.x,y);
            if (!rect_intersects(r,a.viewport)) continue;
            bool hover = rect_contains(r,mx,my) && rect_contains(a.viewport,mx,my);
            if (group == 1) {
                gui_round_rect_aa(s,r,8*k,accent_at(i));
                if (i == st->accent || hover)
                    gui_round_frame_aa(s,r,8*k,i == st->accent ? g_theme.text_bright : g_theme.text_dim);
                if (i == st->accent) {
                    colour_t c = accent_at(i);
                    colour_t ink = RGB_R(c)*299+RGB_G(c)*587+RGB_B(c)*114 > 150000 ? RGB(0,0,0) : RGB(255,255,255);
                    gui_icon(s,ICON_CHECK,r.x+8*k,r.y+8*k,16*k,ink);
                }
            } else {
                const theme_spec_t *t = group == 0 ? theme_at(i) : NULL;
                const char *name = group == 0 ? (t ? t->name : "") :
                                   group == 2 ? desktop_wallpaper_name(i) : appearance_sizes[i];
                bool selected = group == 0 ? i == st->theme : group == 2 ? i == st->wallpaper : i+1 == k;
                if (group == 0 || group == 2)
                    appearance_preview_button(st,s,r,group,i,name,hover,selected);
                else gui_button(s,r,name,hover,selected,true);
            }
        }
    }
    gui_text_clipped(s,FONT_UI,a.viewport.x,y+a.animation_y,width,"Window animations",g_theme.text);
    rect_t sw = animation_switch_rect(st,s);
    gui_switch(s,sw,st->animations,rect_contains(sw,mx,my) && rect_contains(a.viewport,mx,my),true);
    if (a.total_height > a.viewport.h)
        gui_scrollbar(s,a.track,st->appearance_scroll,a.viewport.h,a.total_height);
}

/* ---------------------------------------------------------------- storage */

static void paint_storage(settings_t *st, surface_t *s, int mx, int my) {
    (void)mx; (void)my;
    rect_t b = body_rect(s);
    int y = gui_page_header(s, b.x, b.y, b.w, "Storage",
                            "The disks and volumes this machine can see.");

    if (!st->volume_count) {
        gui_text(s, FONT_UI, b.x, y, "No storage was found.", g_theme.text_dim);
        return;
    }

    const int k = gui_scale();
    for (int i = 0; i < st->volume_count; i++) {
        int row_height = 2 * gui_font_height(FONT_UI) + 8 * k;
        rect_t r = rect_make(b.x, y + i * row_height, b.w, row_height - 4 * k);
        if (r.y + r.h > b.y + b.h) break;

        gui_icon(s, ICON_DISK, r.x + 4 * gui_scale(), r.y + 10 * gui_scale(),
                 20 * gui_scale(), g_theme.accent);

        char size[24];
        format_size(size, sizeof size, st->volumes[i].size);

        gui_text_clipped(s, FONT_UI, r.x + 34 * k, r.y + 2 * k,
                         r.w - 40 * k, st->volumes[i].name, g_theme.text);
        char sub[160];
        snprintf(sub, sizeof sub, "%s   %s%s%s", size,
                 st->volumes[i].model[0] ? st->volumes[i].model
                                         : (st->volumes[i].is_partition ? "partition"
                                                                        : "disk"),
                 st->volumes[i].label[0] ? "   " : "",
                 st->volumes[i].label[0] ? st->volumes[i].label : "");
        gui_text_clipped(s, FONT_UI, r.x + 34 * k, r.y + gui_font_height(FONT_UI) + 4 * k,
                         r.w - 40 * k, sub, g_theme.text_dim);
    }
}

/* ---------------------------------------------------------------- devices */

static rect_t open_devices_rect(surface_t *s) {
    rect_t b = body_rect(s);
    const int k = gui_scale();
    /* Sized around its own label and icon - it was 190 wide at every scale,
     * and the user photographed "Open Device Manager" running off both ends
     * of it. */
    int w = gui_text_width(FONT_UI, "Open Device Manager") + 52 * k;
    return rect_make(b.x, b.y + 130 * k, w, gui_font_height(FONT_UI) + 14 * k);
}

static void paint_devices(settings_t *st, surface_t *s, int mx, int my) {
    rect_t b = body_rect(s);
    int y = gui_page_header(s, b.x, b.y, b.w, "Devices",
                            "Everything on the buses, and which driver claimed it.");

    char line[128];
    snprintf(line, sizeof line, "%d device(s) on PCI, %d on USB",
             st->pci_count, st->usb_count);
    gui_text(s, FONT_UI, b.x, y, line, g_theme.text);

    rect_t open = open_devices_rect(s);
    gui_button_icon(s, open, ICON_USB, "Open Device Manager",
                    rect_contains(open, mx, my), false, true);
}

/* ------------------------------------------------------------------ about */

static void paint_about(settings_t *st, surface_t *s, int mx, int my) {
    (void)st; (void)mx; (void)my;
    rect_t b = body_rect(s);
    int k = gui_scale(), line_height = gui_font_height(FONT_UI);
    int y = gui_page_header(s, b.x, b.y, b.w, "About", NULL) + 12 * k;
    gui_icon(s, ICON_KESTREL, b.x, y, 48 * k, g_theme.accent);
    gui_text_clipped(s, FONT_UI, b.x + 62 * k, y, b.w - 62 * k,
                     "KestrelOS", g_theme.text_bright);

    ksysinfo_t si;
    memset(&si, 0, sizeof si);
    sysinfo(&si);

    gui_text_clipped(s, FONT_UI, b.x + 62 * k, y + line_height + 4 * k,
                     b.w - 62 * k, si.cpu[0] ? si.cpu : "x86-64", g_theme.text_dim);
    int identity_height = 2 * line_height + 4 * k;
    if (identity_height < 48 * k) identity_height = 48 * k;
    y += identity_height + 24 * k;
    uint64_t mem[6];
    if (meminfo(mem) == 0) {
        char total[24], used[24], line[128];
        format_size(total, sizeof total, mem[0]);
        format_size(used, sizeof used, mem[0] - mem[1]);
        snprintf(line, sizeof line, "Memory: %s of %s in use", used, total);
        gui_text_clipped(s, FONT_UI, b.x, y, b.w, line, g_theme.text);
        y += line_height + 4 * k;
    }

    char line[160];
    snprintf(line, sizeof line, "Graphics engine: %s",
             s->gpu ? "GPU-backed toolkit" : blit_engine_name());
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, line, g_theme.text);
    y += line_height + 4 * k;

    uint64_t up = uptime_ms() / 1000;
    snprintf(line, sizeof line, "Running for %llu:%02llu:%02llu",
             (unsigned long long)(up / 3600), (unsigned long long)((up / 60) % 60),
             (unsigned long long)(up % 60));
    gui_text_clipped(s, FONT_UI, b.x, y, b.w, line, g_theme.text_dim);
}

/* ------------------------------------------------------------------ paint */

static void settings_paint(settings_t *st, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    settings_navigation_t nav = settings_nav(s);
    if (nav.sidebar) {
    rect_t side = sidebar_rect(s);
    gui_sidebar(s, side);
    gui_text(s, FONT_UI, 20 * gui_scale(), 20 * gui_scale(), "Settings",
             g_theme.text_bright);

    for (int i = 0; i < PAGE_COUNT; i++) {
        rect_t r = sidebar_item_rect(s, i);
        gui_sidebar_item(s, r, pages[i].icon, pages[i].name,
                         i == (int)st->page, i == st->hover_page);
    }
    } else {
        gui_sidebar(s, rect_make(0, 0, s->width, nav.top));
        rect_t arrows[2] = {nav.previous, nav.next};
        for (int i = 0; i < 2; i++) {
            rect_t r = arrows[i];
            rect_t saved = surface_clip(s);
            surface_set_clip(s,rect_intersection(saved,r));
            if (rect_contains(r,mx,my))
                gui_round_rect_aa(s,r,4*gui_scale(),g_theme.control_hover);
            int size = 16*gui_scale();
            gui_icon(s,i ? ICON_ARROW_RIGHT : ICON_ARROW_LEFT,
                     r.x+(r.w-size)/2,r.y+(r.h-size)/2,size,g_theme.text);
            surface_set_clip(s,saved);
        }
        char caption[80];
        snprintf(caption,sizeof caption,"%s  (%d/%d)",pages[st->page].name,(int)st->page+1,PAGE_COUNT);
        gui_text_clipped(s,FONT_UI,nav.caption.x+8*gui_scale(),
                         nav.caption.y+(nav.caption.h-gui_font_height(FONT_UI))/2,
                         nav.caption.w-16*gui_scale(),caption,g_theme.text_bright);
    }

    rect_t saved_clip = surface_clip(s);
    surface_set_clip(s,rect_intersection(saved_clip,nav.body));
    switch (st->page) {
    case PAGE_NETWORK:    paint_network(st, s, mx, my); break;
    case PAGE_DISPLAY:    paint_display(st, s, mx, my); break;
    case PAGE_SOUND:      paint_sound(st, s, mx, my); break;
    case PAGE_APPEARANCE: paint_appearance(st, s, mx, my); break;
    case PAGE_STORAGE:    paint_storage(st, s, mx, my); break;
    case PAGE_DEVICES:    paint_devices(st, s, mx, my); break;
    default:              paint_about(st, s, mx, my); break;
    }
    surface_set_clip(s,saved_clip);

    /* Whatever just happened, said once and then faded. */
    if (st->status[0] && uptime_ms() - st->status_at < 6000) {
        const int k = gui_scale();
        rect_t bar = nav.status;
        gui_round_rect_aa(s, bar, 6, colour_mix(g_theme.window, g_theme.accent, 40));
        surface_set_clip(s,rect_intersection(saved_clip,bar));
        gui_text_clipped(s, FONT_UI, bar.x + 12 * k, bar.y + (bar.h-gui_font_height(FONT_UI))/2, bar.w - 24 * k,
                         st->status, g_theme.text);
        surface_set_clip(s,saved_clip);
    }
}

/* ------------------------------------------------------------------ events */

static void do_scan(settings_t *st) {
    if (!st->have_wifi || !st->wifi_on) return;
    if (!st->wifi.radio_up) {
        say(st, "Radio startup incomplete. Scanning is unavailable.");
        return;
    }
    st->scanning = true;
    st->scan_started = uptime_ms();
    say(st, "Looking for networks...");
}

static void finish_scan(settings_t *st) {
    int result = wifi_scan(st->wifi.name, 2500);
    load_wifi(st);
    st->scanning = false;
    if (result < 0 || !st->wifi.radio_up) {
        say(st, "Scan failed: the radio is not ready or did not answer.");
    } else if (st->network_count) {
        char line[64];
        snprintf(line, sizeof line, "%d network(s) found.", st->network_count);
        say(st, line);
    } else {
        say(st, "No networks found.");
    }
}

static void join_selected(settings_t *st) {
    if (st->selected_network < 0 || st->selected_network >= st->network_count) return;
    const kwifinet_t *n = &st->networks[st->selected_network];

    char line[128];
    snprintf(line, sizeof line, "Joining \"%s\"...", n->ssid);
    say(st, line);

    int r = wifi_connect(st->wifi.name, n->ssid,
                         n->security ? st->password : "", 9000);
    load_wifi(st);

    if (r == 0 && st->wifi.state == 5) {
        snprintf(line, sizeof line, "Connected to \"%s\".", n->ssid);
        say(st, line);
        st->asking_password = false;
        st->password[0] = 0;
        gui_textfield_init(&st->password_field, st->password, sizeof st->password);
    } else {
        snprintf(line, sizeof line, "Could not join \"%s\".  Check the password.",
                 n->ssid);
        say(st, line);
    }
}

static void settings_select_page(settings_t *st, int page) {
    if (page < 0 || page >= PAGE_COUNT || page == (int)st->page) return;
    st->page = (page_t)page;
    /* A network-scan result must not follow the user onto Display/Sound. */
    st->status[0] = '\0';
    st->status_at = 0;
    st->hover_page = st->hover_network = st->hover_mode = -1;
    st->appearance_dragging = false;
    st->sound_dragging = false;
    if (st->page != PAGE_NETWORK) {
        st->asking_password = false;
        memset(st->password, 0, sizeof st->password);
        gui_textfield_init(&st->password_field, st->password, sizeof st->password);
    }
    switch (st->page) {
    case PAGE_NETWORK: load_wifi(st); break;
    case PAGE_DISPLAY: load_modes(st); break;
    case PAGE_STORAGE: load_storage(st); break;
    case PAGE_DEVICES: load_devices(st); break;
    case PAGE_SOUND:
        st->have_audio = enum_audio(&st->audio) == 0 && st->audio.present;
        load_audio_formats(st);
        load_audio_endpoints(st);
        break;
    default: break;
    }
}

static bool settings_proc(window_t *w, const wevent_t *ev) {
    settings_t *st = w->data;
    surface_t *s = w->canvas;
    settings_navigation_t nav = settings_nav(s);
    if (ev->kind == WE_MOUSE_MOVE || ev->kind == WE_MOUSE_DOWN || ev->kind == WE_MOUSE_UP) {
        st->pointer_x = ev->x; st->pointer_y = ev->y;
    }
    if (ev->kind == WE_KEY_DOWN && (ev->mods & KMOD_CTRL) &&
        (ev->key == KK_PAGEUP || ev->key == KK_PAGEDOWN)) {
        int direction = ev->key == KK_PAGEUP ? -1 : 1;
        settings_select_page(st, ((int)st->page + PAGE_COUNT + direction) % PAGE_COUNT);
        return true;
    }
    if (!nav.sidebar && ev->kind == WE_MOUSE_DOWN && ev->button == MB_LEFT) {
        int direction = rect_contains(nav.previous, ev->x, ev->y) ? -1 :
                        rect_contains(nav.next, ev->x, ev->y) ? 1 : 0;
        if (direction) {
            settings_select_page(st, ((int)st->page + PAGE_COUNT + direction) % PAGE_COUNT);
            return true;
        }
    }

    if (st->page == PAGE_SOUND) {
        rect_t b = body_rect(s), track = sound_track(s);
        int before = st->sound_scroll;
        int64_t requested = before;
        if (ev->kind == WE_MOUSE_UP) st->sound_dragging = false;
        if (ev->kind == WE_MOUSE_WHEEL && rect_contains(b,ev->x,ev->y))
            requested -= (int64_t)ev->wheel * 36 * gui_scale();
        if (ev->kind == WE_KEY_DOWN) {
            if (ev->key == KK_PAGEUP) requested -= b.h;
            if (ev->key == KK_PAGEDOWN) requested += b.h;
            if (ev->key == KK_HOME) requested = 0;
            if (ev->key == KK_END) requested = st->sound_total;
        }
        bool grab = ev->kind == WE_MOUSE_DOWN && ev->button == MB_LEFT &&
                    rect_contains(track,ev->x,ev->y) && st->sound_total > b.h;
        if (grab || (ev->kind == WE_MOUSE_MOVE && st->sound_dragging)) {
            st->sound_dragging = true;
            requested = gui_scrollbar_hit(track,before,b.h,st->sound_total,ev->y);
        }
        st->sound_scroll = settings_scroll_clamp(requested,st->sound_total,b.h);
        if (grab || st->sound_dragging || st->sound_scroll != before) return true;
    }

    if (st->page == PAGE_APPEARANCE) {
        settings_appearance_t a = appearance_layout(s);
        int before = st->appearance_scroll;
        int64_t requested = before;
        if (ev->kind == WE_MOUSE_UP) st->appearance_dragging = false;
        if (ev->kind == WE_MOUSE_WHEEL && rect_contains(a.viewport,ev->x,ev->y))
            requested -= (int64_t)ev->wheel * 36 * gui_scale();
        if (ev->kind == WE_KEY_DOWN) {
            if (ev->key == KK_PAGEUP) requested -= a.viewport.h;
            if (ev->key == KK_PAGEDOWN) requested += a.viewport.h;
            if (ev->key == KK_HOME) requested = 0;
            if (ev->key == KK_END) requested = a.total_height;
        }
        bool grab = ev->kind == WE_MOUSE_DOWN && ev->button == MB_LEFT &&
                    rect_contains(a.track,ev->x,ev->y) && a.total_height > a.viewport.h;
        if (grab || (ev->kind == WE_MOUSE_MOVE && st->appearance_dragging)) {
            st->appearance_dragging = true;
            requested = gui_scrollbar_hit(a.track,before,a.viewport.h,a.total_height,ev->y);
        }
        st->appearance_scroll = settings_scroll_clamp(requested,a.total_height,a.viewport.h);
        if (grab || st->appearance_dragging || st->appearance_scroll != before) return true;
    }

    if (st->page == PAGE_DISPLAY && st->native_display.output_count &&
        (ev->kind == WE_MOUSE_MOVE || ev->kind == WE_MOUSE_DOWN ||
         ev->kind == WE_MOUSE_UP || ev->kind == WE_MOUSE_WHEEL)) {
        bool changed = ds_event(&st->native_display, ev);
        if (st->native_display.requested_group >= 0) {
            int choice = st->native_display.requested_group;
            st->native_display.requested_group = -1;
            char key[24];
            if (choice == 0) strlcpy(key,"extend",sizeof key);
            else if (choice == 1) strlcpy(key,"mirror",sizeof key);
            else snprintf(key,sizeof key,"only:%d",ds_boot_slot(&st->native_display,
                                                               st->native_display.selected_output));
            say(st,desktop_save_displaymode(key) ? "Startup arrangement saved. Current displays are unchanged."
                                                : "Could not save the startup arrangement.");
        }
        /* Native inspector must never fall through to firmware mode writes. */
        if (!nav.sidebar || ev->x >= nav.sidebar || ev->kind == WE_MOUSE_UP) return changed;
    }

    switch (ev->kind) {
    case WE_PAINT:
        settings_paint(st, s, st->pointer_x, st->pointer_y);
        return false;

    case WE_TICK:
        if (st->scanning && uptime_ms() - st->scan_started > 400) finish_scan(st);
        /* Live pages keep themselves current. */
        if (st->page == PAGE_NETWORK) load_wifi(st);
        if (st->page == PAGE_DISPLAY) ds_tick(&st->native_display);
        return true;

    case WE_MOUSE_MOVE: {
        int old_page = st->hover_page, old_net = st->hover_network,
            old_mode = st->hover_mode;

        st->hover_page = -1;
        for (int i = 0; i < PAGE_COUNT; i++)
            if (rect_contains(sidebar_item_rect(s, i), ev->x, ev->y)) st->hover_page = i;

        st->hover_network = -1;
        if (st->page == PAGE_NETWORK)
            for (int i = 0; i < st->network_count && i < 6; i++)
                if (rect_contains(network_row_rect(s, i), ev->x, ev->y))
                    st->hover_network = i;

        st->hover_mode = -1;
        if (st->page == PAGE_DISPLAY)
            for (int i = 0; i < st->mode_count; i++)
                {
                    rect_t r = mode_row_rect(s, i - st->mode_scroll);
                    rect_t body = body_rect(s);
                    if (r.y < mode_list_top) continue;
                    if (r.y + r.h > body.y + body.h - 56) break;
                    if (rect_contains(r, ev->x, ev->y)) st->hover_mode = i;
                }

        return old_page != st->hover_page || old_net != st->hover_network ||
               old_mode != st->hover_mode || true;
    }

    case WE_MOUSE_UP:
        return true;

    case WE_MOUSE_DOWN:
        if (ev->button != MB_LEFT) return false;
        /* The sidebar. */
        for (int i = 0; i < PAGE_COUNT; i++) {
            if (!rect_contains(sidebar_item_rect(s, i), ev->x, ev->y)) continue;
            settings_select_page(st, i);
            return true;
        }

        /* Clipped content is not clickable through navigation or page padding. */
        if (!rect_contains(nav.body,ev->x,ev->y)) return false;

        if (st->page == PAGE_SOUND && st->have_audio) {
            /* All hit-tested against the rects the last paint recorded, so a
             * click lands on whatever was actually drawn there. */
            if (st->audio.output_ready && rect_contains(st->rc_mute, ev->x, ev->y)) {
                toggle_audio_mute(st);
                return true;
            }
            for (int i = 0; i < st->rc_out_n; i++)
                if (rect_contains(st->rc_out[i], ev->x, ev->y)) {
                    /* Map the drawn output chip back to its endpoint. */
                    int seen = 0;
                    for (int j = 0; j < st->audio_ep_count; j++) {
                        if (!st->audio_eps[j].is_output) continue;
                        if (seen++ == i) {
                            select_audio_endpoint(st, st->audio_eps[j].node, true);
                            break;
                        }
                    }
                    return true;
                }
            for (int i = 0; i < st->rc_in_n; i++)
                if (rect_contains(st->rc_in[i], ev->x, ev->y)) {
                    int seen = 0;
                    for (int j = 0; j < st->audio_ep_count; j++) {
                        if (st->audio_eps[j].is_output) continue;
                        if (seen++ == i) {
                            select_audio_endpoint(st, st->audio_eps[j].node, false);
                            break;
                        }
                    }
                    return true;
                }
            for (int i = 0; i < st->rc_rate_n; i++)
                if (rect_contains(st->rc_rate[i], ev->x, ev->y)) {
                    apply_audio_format(st, st->audio_rates[i], st->audio.bits);
                    return true;
                }
            for (int i = 0; i < st->rc_depth_n; i++)
                if (rect_contains(st->rc_depth[i], ev->x, ev->y)) {
                    apply_audio_format(st, st->audio.sample_rate,
                                       st->audio_depths[i]);
                    return true;
                }
        }

        if (st->page == PAGE_NETWORK && st->have_wifi) {
            if (rect_contains(wifi_switch_rect(s), ev->x, ev->y)) {
                st->wifi_on = !st->wifi_on;
                int r = wifi_enable(st->wifi.name, st->wifi_on);
                load_wifi(st);
                if (r < 0) say(st, "The radio did not answer.");
                else if (st->wifi_on && !st->wifi.radio_up)
                    say(st, st->wifi.firmware_needed && !st->wifi.firmware_present
                            ? "Switched on, but its firmware is missing."
                            : "Switched on, but the radio did not start.");
                else say(st, st->wifi_on ? "Wi-Fi is on." : "Wi-Fi is off.");
                return true;
            }
            if (st->wifi_on && rect_contains(scan_button_rect(s), ev->x, ev->y)) {
                do_scan(st);
                return true;
            }
            if (st->asking_password) {
                if (rect_contains(password_field_rect(s), ev->x, ev->y)) {
                    st->password_field.focused = true;
                    return true;
                }
                if (rect_contains(password_join_rect(s), ev->x, ev->y)) {
                    join_selected(st);
                    return true;
                }
            }
            for (int i = 0; i < st->network_count && i < 6; i++) {
                if (!rect_contains(network_row_rect(s, i), ev->x, ev->y)) continue;
                st->selected_network = i;
                if (st->networks[i].security) {
                    st->asking_password = true;
                    st->password[0] = 0;
                    gui_textfield_init(&st->password_field, st->password,
                                       sizeof st->password);
                    st->password_field.focused = true;
                } else {
                    st->asking_password = false;
                    join_selected(st);
                }
                return true;
            }
        }

        if (st->page == PAGE_DISPLAY) {
            rect_t body = body_rect(s);

            for (int i = 0; i < st->mode_count; i++) {
                rect_t r = mode_row_rect(s, i - st->mode_scroll);

                /* Only the rows that are actually on the page.  Without this
                 * the list carries on past the bottom of its area as far as
                 * however many modes there are - invisible, but still catching
                 * clicks, including the ones meant for the button underneath
                 * it.  Picking a size then appeared to do nothing, because the
                 * press that should have applied it silently selected a row
                 * nobody could see. */
                if (r.y < mode_list_top) continue;
                if (r.y + r.h > body.y + body.h - 56) break;

                if (!rect_contains(r, ev->x, ev->y)) continue;
                st->chosen_mode = i;
                return true;
            }
            if (st->chosen_mode >= 0 &&
                rect_contains(apply_mode_rect(s), ev->x, ev->y)) {
                unsigned want_w = st->modes[st->chosen_mode].width;
                unsigned want_h = st->modes[st->chosen_mode].height;

                /* Try it now.  Where the display adapter is being driven this
                 * simply works, and waiting for a restart to see a resolution
                 * change is a thing no system should ask of anybody. */
                if (wm_set_mode(w->wm, (int)want_w, (int)want_h)) {
                    desktop_save_mode(want_w, want_h, 0);
                    /* Re-read the modes so "Now:" and the "in use now" marker
                     * reflect what the adapter is actually driving - without
                     * this the page kept naming the old size after the screen
                     * had already changed under it. */
                    load_modes(st);
                    say(st, "Changed.");
                } else if (desktop_save_mode(want_w, want_h, 0)) {
                    say(st, "This display cannot be changed while running, so "
                            "the new size is saved for the next start.");
                } else {
                    say(st, "The boot configuration could not be written.");
                }
                return true;
            }

            /* A refresh-rate chip.  Unlike a resolution, a panel's rate cannot
             * be changed on the running firmware framebuffer (or the VM's fixed
             * virtual display) - only a display driver reprograms the timing -
             * so the choice is recorded for the next start rather than applied
             * live, and the message says exactly that instead of implying a
             * change that did not happen. */
            if (st->refresh_rate_count > 1 && st->current_mode >= 0) {
                for (int i = 0; i < st->refresh_rate_count; i++) {
                    rect_t r = refresh_chip_rect(st, s, i);
                    if (!rect_contains(r, ev->x, ev->y)) continue;
                    unsigned hz = (st->refresh_rates[i] + 500) / 1000;
                    unsigned cw = st->modes[st->current_mode].width;
                    unsigned ch = st->modes[st->current_mode].height;
                    if (desktop_save_mode(cw, ch, hz)) {
                        char m[128];
                        snprintf(m, sizeof m, "%u Hz recorded. No display driver "
                                 "reprograms panel timing yet, so the rate is "
                                 "not applied until one does.", hz);
                        say(st, m);
                    } else {
                        say(st, "The boot configuration could not be written.");
                    }
                    return true;
                }
            }

            /* A multiple-displays arrangement (#7).  Recorded for the next
             * start, where the loader sets it up before the desktop - the clean
             * point to rearrange the scan-outs. */
            if (st->display.display_count > 1) {
                for (int i = 0; i < st->display_mode_count; i++) {
                    rect_t r = st->display_mode_rects[i];
                    if (!rect_contains(r, ev->x, ev->y)) continue;
                    /* Apply it now where the framebuffer allows (mirror /
                     * only-the-other switch live); extend needs a wide
                     * framebuffer set up at boot, so it is saved for the next
                     * start.  Either way the choice is remembered in BOOT.CFG. */
                    char label[32], key[16]; int mode;
                    display_choice(i, label, sizeof label, key, sizeof key, &mode);
                    int applied = fb_set_display_mode(mode);
                    desktop_save_displaymode(key);
                    char m[112];
                    if (applied == 0)
                        snprintf(m, sizeof m, "%s: applied now, and saved.",
                                 label);
                    else
                        snprintf(m, sizeof m, "%s: saved; it takes effect at the "
                                 "next start.", label);
                    say(st, m);
                    return true;
                }
            }
        }

        if (st->page == PAGE_APPEARANCE) {
            for (int i = 0; i < theme_count(); i++) {
                if (!rect_contains(theme_button_rect(st, s, i), ev->x, ev->y)) continue;
                const theme_spec_t *t = theme_at(i);
                st->theme = i;
                st->dark = t ? t->dark : true;
                theme_apply(st->theme, accent_at(st->accent));
                desktop_prefs_save(st->theme, st->accent, st->wallpaper);
                wm_damage_all(w->wm);
                say(st, t ? t->name : "");
                return true;
            }
            for (int i = 0; i < accent_count(); i++) {
                if (!rect_contains(accent_swatch_rect(st, s, i), ev->x, ev->y)) continue;
                st->accent = i;
                theme_apply(st->theme, accent_at(st->accent));
                desktop_prefs_save(st->theme, st->accent, st->wallpaper);
                wm_damage_all(w->wm);
                say(st, accent_name(i));
                return true;
            }
            for (int i = 0; i < 3; i++) {
                if (!rect_contains(scale_button_rect(st, s, i), ev->x, ev->y)) continue;

                /* Everything is measured against this, so everything has to be
                 * drawn again - including this window, whose own buttons have
                 * just moved. */
                gui_set_scale(i + 1);
                desktop_prefs_save(st->theme, st->accent, st->wallpaper);
                wm_damage_all(w->wm);
                say(st, i == 0 ? "Interface size: 100%" : i == 1 ? "Interface size: 200%" : "Interface size: 300%");
                return true;
            }

            for (int i = 0; i < desktop_wallpaper_count(); i++) {
                if (!rect_contains(wallpaper_button_rect(st, s, i), ev->x, ev->y)) continue;
                st->wallpaper = i;
                desktop_set_wallpaper(i);
                desktop_prefs_save(st->theme, st->accent, st->wallpaper);
                wm_damage_all(w->wm);
                return true;
            }
            if (rect_contains(animation_switch_rect(st, s), ev->x, ev->y)) {
                st->animations = !st->animations;
                w->wm->animations_on = st->animations;
                say(st, st->animations ? "Animations on." : "Animations off.");
                return true;
            }
        }

        if (st->page == PAGE_DEVICES &&
            rect_contains(open_devices_rect(s), ev->x, ev->y)) {
            app_devices_launch(w->wm);
            return true;
        }
        return true;

    case WE_KEY_DOWN:
        if (st->asking_password && st->password_field.focused) {
            if (ev->key == '\n' || ev->key == '\r') { join_selected(st); return true; }
            if (ev->key == 27) { st->asking_password = false; return true; }
            gui_textfield_key(&st->password_field, ev->key, ev->mods);
            return true;
        }
        return false;

    case WE_CLOSE:
        free(st);
        return false;

    default:
        return false;
    }
}

void app_settings_launch(wm_t *wm) {
    settings_t *st = calloc(1, sizeof *st);
    if (!st) return;

    st->page = PAGE_NETWORK;
    st->hover_page = -1;
    st->hover_network = -1;
    st->hover_mode = -1;
    st->pointer_x = st->pointer_y = -1;
    st->selected_network = -1;
    st->dark = true;
    st->theme = 0;
    st->accent = 0;
    st->animations = wm->animations_on;
    desktop_wallpaper_index(&st->wallpaper);
    gui_textfield_init(&st->password_field, st->password, sizeof st->password);

    load_wifi(st);
    load_modes(st);
    load_storage(st);
    load_devices(st);
    st->have_audio = enum_audio(&st->audio) == 0 && st->audio.present;
    load_audio_formats(st);
    load_audio_endpoints(st);

    window_t *w = desktop_new_window(wm, "Settings", ICON_SETTINGS, 780, 560,
                                     settings_proc, st);
    if (!w) { free(st); return; }
    w->min_w = 640;
    w->min_h = 440;
}
