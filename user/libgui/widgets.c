/* widgets.c - the immediate-mode controls the apps are built from.
 *
 * Each function draws a control and, where it makes sense, reports whether the
 * pointer is over it.  Hit testing stays with the caller, which for dialogs of
 * this size is less machinery than a retained widget tree and keeps the layout
 * visible in one place.
 */
#include "window.h"

bool gui_button(surface_t *s, rect_t r, const char *label, bool hover, bool pressed, bool enabled) {
    return gui_button_icon(s, r, ICON_NONE, label, hover, pressed, enabled);
}

bool gui_button_icon(surface_t *s, rect_t r, icon_id icon, const char *label,
                     bool hover, bool pressed, bool enabled) {
    if (rect_empty(r)) return false;
    colour_t face = g_theme.control;
    colour_t edge = g_theme.control_border;
    colour_t text = g_theme.text;

    if (!enabled) {
        face = colour_shade(g_theme.control, -12);
        text = g_theme.text_dim;
        edge = colour_shade(g_theme.control_border, -20);
    } else if (pressed) {
        face = g_theme.control_press;
    } else if (hover) {
        face = g_theme.control_hover;
        edge = g_theme.accent;
    }

    int bk = gui_scale();
    int radius = 6 * bk;
    if (radius > r.h / 2) radius = r.h / 2;
    gui_round_rect_aa(s, r, radius, face);
    if (enabled && !pressed) {
        /* A lighter row along the top and a darker one along the bottom read as
         * a slightly raised, dimensional surface rather than a flat swatch. */
        gui_hline(s, r.x + radius, r.y + 1, r.w - 2 * radius, colour_shade(face, 16));
        gui_hline(s, r.x + radius, r.y + r.h - 2, r.w - 2 * radius, colour_shade(face, -12));
    }
    gui_round_frame_aa(s, r, radius, edge);

    int icon_size = r.h - 12 * bk;
    if (icon_size < 0) icon_size = 0;
    if (icon_size > r.w - 12 * bk) icon_size = r.w - 12 * bk;
    if (icon_size < 0) icon_size = 0;
    rect_t saved = surface_clip(s);
    rect_t inner = rect_make(r.x + 6 * bk, r.y, r.w - 12 * bk, r.h);
    surface_set_clip(s, rect_intersection(saved, inner));
    int text_w = label ? gui_text_width(FONT_UI, label) : 0;
    int total = text_w + (icon ? icon_size + 6 * bk : 0);
    if (total > inner.w) total = inner.w;
    int x = r.x + (r.w - total) / 2;

    if (icon) {
        gui_icon(s, icon, x, r.y + (r.h - icon_size) / 2, icon_size, text);
        x += icon_size + 6 * bk;
    }
    if (label)
        gui_text_clipped(s, FONT_UI, x,
                         r.y + (r.h - gui_font_height(FONT_UI)) / 2 + 1,
                         inner.x + inner.w - x, label, text);
    surface_set_clip(s, saved);

    return hover && enabled;
}

void gui_panel(surface_t *s, rect_t r, const char *title) {
    gui_round_rect_aa(s, r, 5, colour_shade(g_theme.window, 4));
    gui_round_frame_aa(s, r, 5, g_theme.window_border);
    if (!title) return;

    int h = gui_font_height(FONT_UI);
    gui_text(s, FONT_UI, r.x + 12, r.y + 8, title, g_theme.text_dim);
    gui_hline(s, r.x + 10, r.y + 10 + h, r.w - 20, g_theme.window_border);
}

void gui_separator(surface_t *s, int x, int y, int w) {
    gui_hline(s, x, y, w, g_theme.window_border);
}

void gui_checkbox(surface_t *s, rect_t r, const char *label, bool checked, bool hover) {
    int box = 16;
    rect_t b = rect_make(r.x, r.y + (r.h - box) / 2, box, box);

    gui_round_rect_aa(s, b, 3, checked ? g_theme.accent : g_theme.field);
    gui_round_frame_aa(s, b, 3, hover ? g_theme.accent : g_theme.field_border);
    if (checked) gui_icon(s, ICON_CHECK, b.x + 2, b.y + 2, box - 4, g_theme.accent_text);

    if (label)
        gui_text(s, FONT_UI, r.x + box + 10,
                 r.y + (r.h - gui_font_height(FONT_UI)) / 2 + 1, label, g_theme.text);
}

void gui_progress(surface_t *s, rect_t r, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    gui_round_rect_aa(s, r, r.h / 2, g_theme.field);
    gui_round_frame_aa(s, r, r.h / 2, g_theme.field_border);

    int w = ((r.w - 4) * percent) / 100;
    if (w > 0)
        gui_round_rect_aa(s, rect_make(r.x + 2, r.y + 2, w, r.h - 4),
                       (r.h - 4) / 2, g_theme.accent);
}

/* The thumb's size reflects how much of the content is visible, which is the
 * only cue the user gets about how long a list is. */
static rect_t thumb_rect(rect_t r, int offset, int visible, int total) {
    if (total <= visible || total <= 0 || visible <= 0 || r.h <= 2 || r.w <= 4)
        return rect_make(0, 0, 0, 0);

    int track = r.h - 2;
    int size = (int)((int64_t)track * visible / total);
    if (size < 24) size = 24;
    if (size > track) size = track;

    int span = total - visible;
    if (offset < 0) offset = 0;
    if (offset > span) offset = span;
    int pos = span > 0 ? (int)((int64_t)(track - size) * offset / span) : 0;
    return rect_make(r.x + 2, r.y + 1 + pos, r.w - 4, size);
}

void gui_scrollbar(surface_t *s, rect_t r, int offset, int visible, int total) {
    gui_fill(s, r, g_theme.scrollbar);
    rect_t t = thumb_rect(r, offset, visible, total);
    if (rect_empty(t)) return;
    gui_round_rect_aa(s, t, (t.w - 1) / 2, g_theme.scrollbar_thumb);
}

/* Map a click on the track to the offset it should scroll to. */
int gui_scrollbar_hit(rect_t r, int offset, int visible, int total, int y) {
    if (total <= visible || visible <= 0 || r.h <= 2 || r.w <= 4) return 0;
    rect_t t = thumb_rect(r, offset, visible, total);
    int track = r.h - 2;
    int size = t.h;
    int span = total - visible;
    int64_t pos = (int64_t)y - r.y - 1 - size / 2;
    if (track - size <= 0) return 0;
    if (pos <= 0) return 0;
    if (pos >= track - size) return span;
    return (int)(pos * span / (track - size));
}

/* ---------------------------------------------------------------- textfield */

void gui_textfield_init(textfield_t *tf, char *storage, size_t cap) {
    tf->text = storage;
    tf->cap = cap;
    tf->len = strnlen(storage, cap);
    tf->caret = tf->len;
    tf->scroll = 0;
    tf->focused = false;
}

void gui_textfield_draw(surface_t *s, rect_t r, textfield_t *tf) {
    gui_round_rect_aa(s, r, 3, g_theme.field);
    gui_round_frame_aa(s, r, 3, tf->focused ? g_theme.accent : g_theme.field_border);

    rect_t inner = rect_make(r.x + 7, r.y + 1, r.w - 14, r.h - 2);
    rect_t saved = surface_clip(s);
    surface_set_clip(s, rect_intersection(inner, saved));

    /* Keep the caret in view by scrolling the text horizontally. */
    int caret_x = gui_text_width_n(FONT_UI, tf->text, tf->caret);
    if (caret_x - tf->scroll > inner.w - 4) tf->scroll = caret_x - inner.w + 4;
    if (caret_x - tf->scroll < 0) tf->scroll = caret_x;
    if (tf->scroll < 0) tf->scroll = 0;

    int ty = r.y + (r.h - gui_font_height(FONT_UI)) / 2 + 1;
    gui_text(s, FONT_UI, inner.x - tf->scroll, ty, tf->text, g_theme.field_text);

    if (tf->focused) {
        /* Blink at roughly half a second so it is obviously a caret. */
        if ((uptime_ms() / 500) % 2 == 0)
            gui_fill(s, rect_make(inner.x - tf->scroll + caret_x, r.y + 4, 1, r.h - 8),
                     g_theme.field_text);
    }

    surface_set_clip(s, saved);
}

bool gui_textfield_key(textfield_t *tf, uint32_t key, uint32_t mods) {
    (void)mods;
    switch (key) {
    case '\b':
        if (tf->caret == 0) return false;
        memmove(tf->text + tf->caret - 1, tf->text + tf->caret, tf->len - tf->caret + 1);
        tf->caret--;
        tf->len--;
        return true;

    case KK_DELETE:
        if (tf->caret >= tf->len) return false;
        memmove(tf->text + tf->caret, tf->text + tf->caret + 1, tf->len - tf->caret);
        tf->len--;
        return true;

    case KK_LEFT:  if (tf->caret) { tf->caret--; return true; } return false;
    case KK_RIGHT: if (tf->caret < tf->len) { tf->caret++; return true; } return false;
    case KK_HOME:  tf->caret = 0; return true;
    case KK_END:   tf->caret = tf->len; return true;

    default:
        if (key < 32 || key > 126) return false;
        if (tf->len + 1 >= tf->cap) return false;
        memmove(tf->text + tf->caret + 1, tf->text + tf->caret, tf->len - tf->caret + 1);
        tf->text[tf->caret++] = (char)key;
        tf->len++;
        return true;
    }
}
