/* controls.c - the controls a settings window is made of.
 *
 * These are the pieces the older widgets.c did not have: a switch that reads
 * as on or off at a glance, a slider, a dropdown, a list with a selection, a
 * sidebar, and the search field a launcher needs.  Everything is
 * immediate-mode - drawn and hit-tested in the same pass - which for windows
 * of this size stays far simpler than a retained tree and keeps the whole
 * layout of a page visible in one function.
 *
 * Two things run through all of them.  Hover and press are animated rather
 * than switched, because a control that changes instantly reads as a redraw
 * and one that eases reads as a response; and every one of them is drawn with
 * the anti-aliased rounded primitives, because at this size a stepped corner is
 * the difference between looking finished and looking hand-drawn.
 */
#include "window.h"

/* ------------------------------------------------------------- animation
 *
 * A control's hover and press states are not booleans on the screen even when
 * they are booleans in the program: each one eases towards where it should be.
 * Rather than making every caller keep that state, it is kept here, keyed on
 * the address of the control's own rectangle - which is stable for as long as
 * the control exists and unique between controls. */
#define GLOW_SLOTS 96
#define GLOW_RATE  55            /* per tick, out of 255 */

typedef struct { const void *key; int level; uint64_t seen; } glow_t;
static glow_t glows[GLOW_SLOTS];

static int glow_for(const void *key, bool on) {
    glow_t *slot = NULL, *oldest = &glows[0];

    for (int i = 0; i < GLOW_SLOTS; i++) {
        if (glows[i].key == key) { slot = &glows[i]; break; }
        if (glows[i].seen < oldest->seen) oldest = &glows[i];
    }
    if (!slot) {
        slot = oldest;
        slot->key = key;
        slot->level = on ? 255 : 0;
    }
    slot->seen = uptime_ms();

    int target = on ? 255 : 0;
    if (slot->level < target) slot->level = (slot->level + GLOW_RATE > target)
                                          ? target : slot->level + GLOW_RATE;
    else if (slot->level > target) slot->level = (slot->level - GLOW_RATE < target)
                                               ? target : slot->level - GLOW_RATE;
    return slot->level;
}

/* True while any control is still easing, so a window knows to keep
 * repainting until everything has settled. */
bool gui_controls_settling(void) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < GLOW_SLOTS; i++) {
        if (!glows[i].key) continue;
        if (now - glows[i].seen > 500) continue;       /* not on screen now */
        if (glows[i].level != 0 && glows[i].level != 255) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ switch
 *
 * The control for turning Wi-Fi on.  A switch rather than a checkbox because
 * it is a state rather than a choice, and because the travel of the knob is
 * what makes the change feel like it took effect. */
/* Scaled with everything else.  A switch left at a fixed size on a screen
 * where the text around it has doubled is a control that has become harder to
 * hit than the words describing it - which is the wrong way round, and the
 * usual sign of a layout that was only half converted. */
#define SWITCH_W (46 * gui_scale())
#define SWITCH_H (24 * gui_scale())

rect_t gui_switch_rect(int x, int y) { return rect_make(x, y, SWITCH_W, SWITCH_H); }

bool gui_switch(surface_t *s, rect_t r, bool on, bool hover, bool enabled) {
    int t = glow_for((const void *)(uintptr_t)(r.x * 8191 + r.y), on);

    colour_t off_face = colour_shade(g_theme.field, 6);
    colour_t on_face = enabled ? g_theme.accent : colour_shade(g_theme.accent, -40);
    colour_t face = colour_mix(off_face, on_face, t);

    if (!enabled) face = colour_mix(face, g_theme.window, 120);

    int radius = r.h / 2;
    gui_round_rect_aa(s, r, radius, face);
    gui_round_frame_aa(s, r, radius,
                       hover && enabled ? g_theme.accent : g_theme.field_border);

    /* The knob travels the width of the track as the state changes. */
    int travel = r.w - r.h;
    int knob_x = r.x + 2 + travel * t / 255;
    rect_t knob = rect_make(knob_x, r.y + 2, r.h - 4, r.h - 4);

    colour_t knob_colour = enabled ? RGB(0xFF, 0xFF, 0xFF) : g_theme.text_dim;
    gui_round_rect_aa(s, knob, knob.h / 2, knob_colour);

    return hover && enabled;
}

/* ------------------------------------------------------------------ slider */

#define SLIDER_H (20 * gui_scale())

bool gui_slider(surface_t *s, rect_t r, int value, int min, int max,
                bool hover, bool dragging) {
    if (max <= min) max = min + 1;
    if (value < min) value = min;
    if (value > max) value = max;

    int track_y = r.y + r.h / 2 - 2;
    rect_t track = rect_make(r.x, track_y, r.w, 4);
    gui_round_rect_aa(s, track, 2, g_theme.field);

    int filled = (value - min) * r.w / (max - min);
    if (filled > 0)
        gui_round_rect_aa(s, rect_make(r.x, track_y, filled, 4), 2, g_theme.accent);

    int grow = glow_for((const void *)(uintptr_t)(r.x * 7919 + r.y + 1),
                        hover || dragging);
    int size = 12 + 4 * grow / 255;
    rect_t knob = rect_make(r.x + filled - size / 2, r.y + r.h / 2 - size / 2, size, size);

    /* A soft ring under the knob while it is being used, which is what tells
     * the eye which control has the pointer. */
    if (grow > 8)
        gui_round_rect_aa(s, rect_make(knob.x - 4, knob.y - 4, knob.w + 8, knob.h + 8),
                          (knob.w + 8) / 2, colour_mix(g_theme.window, g_theme.accent, grow / 4));

    gui_round_rect_aa(s, knob, size / 2, dragging ? g_theme.accent : RGB(0xFF, 0xFF, 0xFF));
    gui_round_frame_aa(s, knob, size / 2, g_theme.accent);
    return hover;
}

/* Where a click in a slider's rectangle puts the value. */
int gui_slider_value_at(rect_t r, int x, int min, int max) {
    if (r.w <= 0) return min;
    int offset = x - r.x;
    if (offset < 0) offset = 0;
    if (offset > r.w) offset = r.w;
    return min + offset * (max - min) / r.w;
}

/* ---------------------------------------------------------------- dropdown */

void gui_dropdown(surface_t *s, rect_t r, const char *text, bool hover, bool open) {
    int t = glow_for((const void *)(uintptr_t)(r.x * 6367 + r.y + 2), hover || open);
    colour_t face = colour_mix(g_theme.field, g_theme.control_hover, t);

    gui_round_rect_aa(s, r, 5, face);
    gui_round_frame_aa(s, r, 5, (hover || open) ? g_theme.accent : g_theme.field_border);

    gui_text_clipped(s, FONT_UI, r.x + 10,
                     r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                     r.w - 34, text ? text : "", g_theme.text);

    /* The arrow turns over when the list is open. */
    int cx = r.x + r.w - 16, cy = r.y + r.h / 2;
    colour_t arrow = g_theme.text_dim;
    for (int i = 0; i < 4; i++) {
        int spread = 4 - i;
        int y = open ? cy + 2 - i : cy - 2 + i;
        gui_hline(s, cx - spread, y, spread * 2 + 1, arrow);
    }
}

/* One row of an open dropdown, or of a list. */
bool gui_list_row(surface_t *s, rect_t r, const char *text, const char *detail,
                  icon_id icon, bool selected, bool hover) {
    int t = glow_for((const void *)(uintptr_t)(r.x * 5381 + r.y * 31 + 3), hover);

    if (selected) {
        gui_round_rect_aa(s, r, 5, g_theme.selection);
    } else if (t > 4) {
        gui_round_rect_aa(s, r, 5, colour_mix(g_theme.window, g_theme.control_hover, t));
    }

    colour_t text_colour = selected ? g_theme.selection_text : g_theme.text;
    int x = r.x + 10;

    if (icon) {
        int size = r.h - 14;
        if (size > 20) size = 20;
        gui_icon(s, icon, x, r.y + (r.h - size) / 2, size,
                 selected ? text_colour : g_theme.accent);
        x += size + 10;
    }

    int line = gui_font_height(FONT_UI);
    if (detail && *detail) {
        gui_text_clipped(s, FONT_UI, x, r.y + r.h / 2 - line, r.w - (x - r.x) - 12,
                         text, text_colour);
        gui_text_clipped(s, FONT_UI, x, r.y + r.h / 2 + 1, r.w - (x - r.x) - 12,
                         detail, selected ? colour_mix(text_colour, g_theme.selection, 90)
                                          : g_theme.text_dim);
    } else {
        gui_text_clipped(s, FONT_UI, x, r.y + (r.h - line) / 2, r.w - (x - r.x) - 12,
                         text, text_colour);
    }
    return hover;
}

/* ----------------------------------------------------------------- sidebar
 *
 * The column of pages down the left of a settings window.  The selected one is
 * marked with a bar rather than a filled row, which stays legible when the
 * window is not the one with focus. */
void gui_sidebar(surface_t *s, rect_t r) {
    gui_fill(s, r, colour_shade(g_theme.window, -6));
    gui_vline(s, r.x + r.w - 1, r.y, r.h, g_theme.window_border);
}

bool gui_sidebar_item(surface_t *s, rect_t r, icon_id icon, const char *label,
                      bool selected, bool hover) {
    int t = glow_for((const void *)(uintptr_t)(r.x * 4093 + r.y * 17 + 4), hover);

    if (selected) {
        gui_round_rect_aa(s, rect_make(r.x + 6, r.y, r.w - 10, r.h), 6,
                          colour_mix(g_theme.window, g_theme.accent, 45));
    } else if (t > 4) {
        gui_round_rect_aa(s, rect_make(r.x + 6, r.y, r.w - 10, r.h), 6,
                          colour_mix(colour_shade(g_theme.window, -6),
                                     g_theme.control_hover, t));
    }

    if (selected) {
        /* The marker down the left edge. */
        rect_t bar = rect_make(r.x + 1, r.y + 6, 3, r.h - 12);
        gui_round_rect_aa(s, bar, 1, g_theme.accent);
    }

    /* The inset, the icon and the gap all scale together, and the same
     * numbers are used by gui_sidebar_item_width below - which is what lets a
     * sidebar be exactly as wide as its widest entry instead of a guess that
     * held at one scale and truncated "Network" to "Netwo..." at another. */
    colour_t colour = selected ? g_theme.text_bright : g_theme.text;
    const int k = gui_scale();
    int x = r.x + 16 * k;
    if (icon) {
        int is = 17 * k;
        gui_icon(s, icon, x, r.y + (r.h - is) / 2, is,
                 selected ? g_theme.accent : g_theme.text_dim);
        x += 26 * k;
    }
    gui_text_clipped(s, FONT_UI, x, r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                     r.w - (x - r.x) - 12 * k, label, colour);
    return hover;
}

/* How wide a sidebar row must be for this label to show in full. */
int gui_sidebar_item_width(icon_id icon, const char *label) {
    const int k = gui_scale();
    int w = 16 * k + 12 * k + gui_text_width(FONT_UI, label);
    if (icon) w += 26 * k;
    return w;
}

/* ------------------------------------------------------------------ header */

/* The title at the top of a settings page, and the description under it. */
/* A heading over a group of controls.
 *
 * There is no bold face here - the interface has one proportional font and one
 * fixed one - so a heading cannot be made heavier and has to be made
 * different in some other way.  Dimmer than the text it introduces, with a
 * hairline running out to the edge of the group, reads as a heading in the
 * same way a rule under a chapter title does: quieter than the content, and
 * clearly not part of it.
 *
 * Drawing it in the same colour and size as body text, which is what happened
 * before, leaves a window that is a list of words with no shape to it. */
int gui_section(surface_t *s, int x, int y, int width, const char *title) {
    int h = gui_font_height(FONT_UI);
    gui_text(s, FONT_UI, x, y, title, g_theme.text_dim);

    int label = gui_text_width(FONT_UI, title);
    int rule_x = x + label + 10;
    int rule_w = width - label - 10;
    if (rule_w > 8)
        gui_hline(s, rule_x, y + h / 2 + 1, rule_w,
                  colour_mix(g_theme.window, g_theme.text_dim, 60));

    return y + h + 8;
}

int gui_page_header(surface_t *s, int x, int y, int width, const char *title,
                    const char *description) {
    gui_text(s, FONT_UI, x, y, title, g_theme.text_bright);
    y += gui_font_height(FONT_UI) + 4;
    if (description && *description) {
        rect_t box = rect_make(x, y, width, 200);
        y = gui_text_wrapped(s, FONT_UI, box, description, g_theme.text_dim);
        y += 4;
    }
    gui_hline(s, x, y, width, g_theme.window_border);
    return y + 14;
}

/* One labelled row of a settings page: the label on the left, the control on
 * the right.  Returns where the control belongs. */
rect_t gui_setting_row(surface_t *s, int x, int y, int width, const char *label,
                       const char *detail, int control_w, int control_h) {
    int line = gui_font_height(FONT_UI);
    int height = detail && *detail ? line * 2 + 12 : line + 14;

    gui_text(s, FONT_UI, x, y + (height - (detail && *detail ? line * 2 + 2 : line)) / 2,
             label, g_theme.text);
    if (detail && *detail)
        gui_text_clipped(s, FONT_UI, x, y + (height - line * 2 - 2) / 2 + line + 2,
                         width - control_w - 30, detail, g_theme.text_dim);

    return rect_make(x + width - control_w, y + (height - control_h) / 2,
                     control_w, control_h);
}

int gui_setting_row_height(const char *detail) {
    int line = gui_font_height(FONT_UI);
    return detail && *detail ? line * 2 + 12 : line + 14;
}

/* ------------------------------------------------------------------ badge */

/* A small coloured label - "Connected", "Missing", "Working" - which is how a
 * status reads at a glance in a list of many things. */
void gui_badge(surface_t *s, int x, int y, const char *text, colour_t colour) {
    int w = gui_text_width(FONT_UI, text) + 14;
    int h = gui_font_height(FONT_UI) + 6;
    rect_t r = rect_make(x, y, w, h);
    gui_round_rect_aa(s, r, h / 2, colour_mix(g_theme.window, colour, 60));
    gui_round_frame_aa(s, r, h / 2, colour_mix(g_theme.window, colour, 140));
    gui_text(s, FONT_UI, x + 7, y + 3, text, colour);
}

int gui_badge_width(const char *text) { return gui_text_width(FONT_UI, text) + 14; }

/* ------------------------------------------------------------- search field */

void gui_search_field(surface_t *s, rect_t r, const char *text, bool focused,
                      const char *placeholder) {
    gui_round_rect_aa(s, r, r.h / 2, g_theme.field);
    gui_round_frame_aa(s, r, r.h / 2, focused ? g_theme.accent : g_theme.field_border);

    /* A magnifier, drawn rather than iconified: a circle and a handle. */
    int cx = r.x + 16, cy = r.y + r.h / 2;
    colour_t glyph = focused ? g_theme.accent : g_theme.text_dim;
    for (int a = 0; a < 360; a += 12) {
        /* A small circle, from a coarse table of directions. */
        static const int sn[30] = { 0, 1, 2, 3, 3, 4, 4, 5, 5, 5, 5, 5, 4, 4, 3,
                                    3, 2, 1, 0, -1, -2, -3, -3, -4, -4, -5, -5,
                                    -5, -5, -5 };
        int i = a / 12;
        int dx = sn[(i + 7) % 30], dy = sn[i % 30];
        gui_pixel(s, cx + dx, cy + dy, glyph);
    }
    gui_line(s, cx + 4, cy + 4, cx + 7, cy + 7, glyph);

    bool empty = !text || !*text;
    gui_text_clipped(s, FONT_UI, r.x + 30, r.y + (r.h - gui_font_height(FONT_UI)) / 2,
                     r.w - 40, empty ? (placeholder ? placeholder : "") : text,
                     empty ? g_theme.text_dim : g_theme.text);

    if (focused && !empty && (uptime_ms() / 500) % 2 == 0) {
        int caret = r.x + 30 + gui_text_width(FONT_UI, text) + 1;
        if (caret < r.x + r.w - 8) gui_vline(s, caret, r.y + 6, r.h - 12, g_theme.text);
    }
}

/* -------------------------------------------------------------- progress */

/* A determinate bar.  The indeterminate case - where the length is not known -
 * is a band that travels, which is the only honest way to show progress that
 * cannot be measured. */
void gui_progress_bar(surface_t *s, rect_t r, int percent, bool indeterminate) {
    gui_round_rect_aa(s, r, r.h / 2, g_theme.field);

    if (indeterminate) {
        int span = r.w / 3;
        int travel = (int)((uptime_ms() / 6) % (uint64_t)(r.w + span));
        int x = r.x + travel - span;
        int left = x < r.x ? r.x : x;
        int right = x + span > r.x + r.w ? r.x + r.w : x + span;
        if (right > left)
            gui_round_rect_aa(s, rect_make(left, r.y, right - left, r.h),
                              r.h / 2, g_theme.accent);
    } else {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        int w = r.w * percent / 100;
        if (w > 0)
            gui_round_rect_aa(s, rect_make(r.x, r.y, w, r.h), r.h / 2, g_theme.accent);
    }
    gui_round_frame_aa(s, r, r.h / 2, g_theme.field_border);
}

/* ------------------------------------------------------------------ spinner
 *
 * For work whose length is unknown and which has no bar to fill: eight dots
 * around a circle, each fading in turn. */
void gui_spinner(surface_t *s, int cx, int cy, int radius, colour_t colour) {
    static const int dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const int dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };

    int phase = (int)((uptime_ms() / 90) % 8);
    for (int i = 0; i < 8; i++) {
        int fade = 255 - ((i - phase + 8) % 8) * 30;
        if (fade < 40) fade = 40;
        int x = cx + dx[i] * radius / 10;
        int y = cy + dy[i] * radius / 10;
        gui_round_rect_aa(s, rect_make(x - 2, y - 2, 4, 4), 2,
                          colour_mix(g_theme.window, colour, fade));
    }
}
