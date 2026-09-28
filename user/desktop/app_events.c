/* app_events.c - the graphical view of the system event log.
 *
 * The whole point of the log is that someone can find out what went wrong, so
 * this opens on warnings and errors and makes the counts visible at a glance
 * rather than burying them behind a filter.
 */
#include "desktop.h"

#define MAX_SHOWN 2000
#define ROW_H     (24 * gui_scale())

typedef struct {
    klog_record_t records[MAX_SHOWN];
    int      count;
    int      scroll;
    int      selected;
    int      min_level;
    uint32_t counts[5];
    uint64_t next_seq;
    bool     follow;
    char     status[200];
} events_t;

static const char *level_names[5] = { "Debug", "Info", "Warning", "Error", "Critical" };

static colour_t level_colour(int level) {
    switch (level) {
    case 0: return g_theme.text_dim;
    case 1: return g_theme.text;
    case 2: return g_theme.warning;
    case 3: return g_theme.error;
    case 4: return g_theme.error;
    default: return g_theme.text;
    }
}

static void events_reload(events_t *e) {
    e->count = 0;
    log_counts(e->counts);

    uint64_t seq = 1;
    klog_record_t batch[32];
    for (;;) {
        int n = log_read(seq, batch, 32);
        if (n <= 0) break;
        for (int i = 0; i < n && e->count < MAX_SHOWN; i++) {
            if ((int)batch[i].level < e->min_level) continue;
            e->records[e->count++] = batch[i];
        }
        seq = batch[n - 1].seq + 1;
    }
    e->next_seq = seq;

    uint32_t bad = e->counts[2] + e->counts[3] + e->counts[4];
    if (bad == 0)
        snprintf(e->status, sizeof e->status, "No warnings or errors have been recorded.");
    else
        snprintf(e->status, sizeof e->status, "%u warning%s, %u error%s, %u critical",
                 e->counts[2], e->counts[2] == 1 ? "" : "s",
                 e->counts[3], e->counts[3] == 1 ? "" : "s", e->counts[4]);
}

/* Pull in anything logged since the last look. */
static bool events_poll(events_t *e) {
    klog_record_t batch[32];
    bool added = false;

    for (;;) {
        int n = log_read(e->next_seq, batch, 32);
        if (n <= 0) break;
        for (int i = 0; i < n && e->count < MAX_SHOWN; i++) {
            if ((int)batch[i].level < e->min_level) continue;
            e->records[e->count++] = batch[i];
            added = true;
        }
        e->next_seq = batch[n - 1].seq + 1;
    }
    if (added) log_counts(e->counts);
    return added;
}

/* ------------------------------------------------------------------- paint */

#define TOOLBAR_H (44 * gui_scale())
#define STATUS_H  (26 * gui_scale())
#define DETAIL_H  (76 * gui_scale())

static rect_t list_rect(surface_t *s) {
    return rect_make(0, TOOLBAR_H, s->width - 12, s->height - TOOLBAR_H - STATUS_H - DETAIL_H);
}

/* The toolbar buttons scale with the interface; bare 92/88/28 left them half
 * size and mispositioned on a 2x display beside the already-scaled ROW_H list. */
static rect_t filter_button(int i) {
    const int k = gui_scale();
    return rect_make(10 * k + i * 92 * k, 8 * k, 88 * k, 28 * k);
}
static rect_t save_button(surface_t *s) {
    const int k = gui_scale();
    return rect_make(s->width - 106 * k, 8 * k, 96 * k, 28 * k);
}

static void events_paint(events_t *e, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    gui_fill(s, rect_make(0, 0, s->width, TOOLBAR_H), colour_shade(g_theme.window, 5));
    gui_hline(s, 0, TOOLBAR_H - 1, s->width, g_theme.window_border);

    static const char *filters[3] = { "All", "Warnings", "Errors" };
    static const int filter_level[3] = { 0, 2, 3 };
    for (int i = 0; i < 3; i++) {
        rect_t r = filter_button(i);
        bool active = (e->min_level == filter_level[i]);
        if (active) {
            gui_round_rect_aa(s, r, 4, g_theme.accent);
            gui_text_centred(s, FONT_UI, r, filters[i], g_theme.accent_text);
        } else {
            gui_button(s, r, filters[i], rect_contains(r, mx, my), false, true);
        }
    }

    rect_t save = save_button(s);
    gui_button(s, save, "Save", rect_contains(save, mx, my), false, true);

    /* Counts, so the state of the machine is visible without reading rows. */
    int x = 300 * gui_scale();
    for (int level = 2; level <= 4; level++) {
        if (!e->counts[level]) continue;
        char text[40];
        snprintf(text, sizeof text, "%u %s", e->counts[level], level_names[level]);
        gui_icon(s, ICON_WARNING, x, 12 * gui_scale(), 14 * gui_scale(),
                 level_colour(level));
        gui_text(s, FONT_UI, x + 20 * gui_scale(), 10 * gui_scale(), text,
                 level_colour(level));
        x += gui_text_width(FONT_UI, text) + 40 * gui_scale();
    }

    /* The list. */
    rect_t list = list_rect(s);
    rect_t saved = surface_clip(s);
    surface_set_clip(s, list);

    int visible = list.h / ROW_H;
    for (int i = 0; i < visible && e->scroll + i < e->count; i++) {
        int index = e->scroll + i;
        klog_record_t *r = &e->records[index];
        rect_t row = rect_make(list.x, list.y + i * ROW_H, list.w, ROW_H);

        if (index == e->selected) gui_fill(s, row, g_theme.selection);
        else if (i % 2) gui_fill(s, row, colour_shade(g_theme.window, 3));

        colour_t tc = index == e->selected ? g_theme.selection_text : level_colour((int)r->level);
        colour_t dim = index == e->selected ? g_theme.selection_text : g_theme.text_dim;

        char stamp[24];
        snprintf(stamp, sizeof stamp, "%llu.%03llu",
                 (unsigned long long)(r->time_ms / 1000), (unsigned long long)(r->time_ms % 1000));

        /* Four columns, at four fixed distances from the left of the row.
         *
         * They were plain numbers - 10, 88, 158, 240 - while the text in them
         * grows with the interface.  At twice the size the subsystem, which
         * starts at 158, is drawn straight across the level, which starts at
         * 88 and is by then more than seventy pixels wide.  The system's own
         * overlap detector reported exactly that, in this window, and nothing
         * was watching it:
         *
         *     text overlaps text: "pci" at 158,33 lands on "WARN" at 88,33
         */
        const int k = gui_scale();
        int ty = row.y + (ROW_H - gui_font_height(FONT_UI)) / 2 + 1 * k;
        gui_text(s, FONT_UI, row.x + 10 * k, ty, stamp, dim);
        gui_text(s, FONT_UI, row.x + 88 * k, ty, log_level_name((int)r->level), tc);
        gui_text(s, FONT_UI, row.x + 158 * k, ty, r->subsys, dim);
        gui_text_clipped(s, FONT_UI, row.x + 240 * k, ty, row.w - 250 * k, r->msg,
                         index == e->selected ? g_theme.selection_text : g_theme.text);
    }

    if (!e->count) {
        gui_icon(s, ICON_CHECK, list.x + list.w / 2 - 16 * gui_scale(),
                 list.y + list.h / 2 - 30 * gui_scale(), 32 * gui_scale(),
                 g_theme.success);
        gui_text_centred(s, FONT_UI, list,
                         e->min_level >= 2 ? "Nothing has gone wrong." : "The log is empty.",
                         g_theme.text_dim);
    }
    surface_set_clip(s, saved);

    if (e->count > visible)
        gui_scrollbar(s, rect_make(s->width - 12, list.y, 12, list.h), e->scroll, visible, e->count);

    /* Detail panel for the selected entry. */
    rect_t detail = rect_make(0, s->height - STATUS_H - DETAIL_H, s->width, DETAIL_H);
    gui_fill(s, detail, colour_shade(g_theme.window, -6));
    gui_hline(s, 0, detail.y, s->width, g_theme.window_border);

    if (e->selected >= 0 && e->selected < e->count) {
        klog_record_t *r = &e->records[e->selected];
        char head[160];
        snprintf(head, sizeof head, "%s  -  %s  -  %llu.%03llu s after start-up",
                 log_level_name((int)r->level), r->subsys,
                 (unsigned long long)(r->time_ms / 1000), (unsigned long long)(r->time_ms % 1000));
        gui_text(s, FONT_UI, 12, detail.y + 8, head, level_colour((int)r->level));

        /* Wrap the message across the remaining lines. */
        const char *p = r->msg;
        int y = detail.y + 8 + gui_font_height(FONT_UI) + 4;
        while (*p && y + gui_font_height(FONT_UI) < detail.y + detail.h) {
            size_t fit = strlen(p);
            while (fit && gui_text_width_n(FONT_UI, p, fit) > s->width - 24) fit--;
            if (!fit) break;
            /* Break on a space where possible. */
            if (fit < strlen(p)) {
                size_t back = fit;
                while (back > 1 && p[back] != ' ') back--;
                if (back > 1) fit = back;
            }
            gui_text_n(s, FONT_UI, 12, y, p, fit, g_theme.text);
            y += gui_font_height(FONT_UI);
            p += fit;
            while (*p == ' ') p++;
        }
    } else {
        gui_text_centred(s, FONT_UI, detail, "Select an entry to see it in full", g_theme.text_dim);
    }

    /* Status bar. */
    rect_t status = rect_make(0, s->height - STATUS_H, s->width, STATUS_H);
    gui_fill(s, status, colour_shade(g_theme.window, 5));
    gui_hline(s, 0, status.y, s->width, g_theme.window_border);
    gui_text(s, FONT_UI, 10, status.y + 3, e->status, g_theme.text_dim);

    char right[40];
    snprintf(right, sizeof right, "%d shown", e->count);
    gui_text_right(s, FONT_UI, rect_make(0, status.y, s->width - 12, status.h), right, g_theme.text_dim);
}

/* ------------------------------------------------------------------ events */

static void events_save(events_t *e) {
    if (!file_exists("/data")) {
        /* Not a reason to refuse any more: the boot volume is tried below,
         * and on a stick that is the only one there is. */
    }

    /* Wherever this machine can write.
     *
     * The data volume first, because an installed system has one and it is the
     * right place.  Then the volume this was booted from, which is the only
     * place a stick has - and a stick is exactly the case where somebody wants
     * these entries off the machine, because it is the case where they cannot
     * simply come back and read them later.
     *
     * Saving only to /data meant Export did nothing on the machines that
     * needed it most. */
    static const char *targets[] = {
        "/data/logs/events-export.log",
        "/boot/KESTREL/events-export.log",
        "/boot/events-export.log",
    };

    int fd = -1;
    const char *saved_to = NULL;
    for (int t = 0; t < 3; t++) {
        fd = open(targets[t], O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) { saved_to = targets[t]; break; }
    }
    if (fd < 0) {
        snprintf(e->status, sizeof e->status,
                 "Nowhere to save to: no volume this machine can write was found.");
        return;
    }
    for (int i = 0; i < e->count; i++) {
        char line[280];
        int n = snprintf(line, sizeof line, "[%llu.%03llu] %-5s %-9s %s\n",
                         (unsigned long long)(e->records[i].time_ms / 1000),
                         (unsigned long long)(e->records[i].time_ms % 1000),
                         log_level_name((int)e->records[i].level),
                         e->records[i].subsys, e->records[i].msg);
        if (n > 0) write(fd, line, (size_t)n);
    }
    close(fd);
    sync();
    snprintf(e->status, sizeof e->status, "Saved %d entries to %s",
             e->count, saved_to);
}

static bool events_proc(window_t *w, const wevent_t *ev) {
    events_t *e = w->data;
    surface_t *s = w->canvas;
    rect_t list = list_rect(s);
    int visible = list.h / ROW_H;

    switch (ev->kind) {
    case WE_PAINT:
        events_paint(e, s, ev->x, ev->y);
        return false;

    case WE_MOUSE_MOVE:
        return true;

    case WE_MOUSE_WHEEL:
        e->scroll -= ev->wheel * 3;
        if (e->scroll > e->count - visible) e->scroll = e->count - visible;
        if (e->scroll < 0) e->scroll = 0;
        return true;

    case WE_MOUSE_DOWN: {
        static const int filter_level[3] = { 0, 2, 3 };
        for (int i = 0; i < 3; i++) {
            if (!rect_contains(filter_button(i), ev->x, ev->y)) continue;
            e->min_level = filter_level[i];
            e->selected = -1;
            events_reload(e);
            return true;
        }
        if (rect_contains(save_button(s), ev->x, ev->y)) { events_save(e); return true; }
        if (rect_contains(list, ev->x, ev->y)) {
            int index = e->scroll + (ev->y - list.y) / ROW_H;
            e->selected = (index >= 0 && index < e->count) ? index : -1;
            return true;
        }
        return false;
    }

    case WE_KEY_DOWN:
        if (ev->key == KK_UP && e->selected > 0) {
            e->selected--;
            if (e->selected < e->scroll) e->scroll = e->selected;
            return true;
        }
        if (ev->key == KK_DOWN && e->selected < e->count - 1) {
            e->selected++;
            if (e->selected >= e->scroll + visible) e->scroll = e->selected - visible + 1;
            return true;
        }
        return false;

    case WE_TICK: {
        bool added = events_poll(e);
        if (added && e->follow) {
            e->scroll = e->count - visible;
            if (e->scroll < 0) e->scroll = 0;
        }
        return added;
    }

    case WE_CLOSE:
        free(e);
        return false;

    default:
        return false;
    }
}

void app_events_launch(wm_t *wm) {
    events_t *e = calloc(1, sizeof *e);
    if (!e) return;
    e->min_level = 2;          /* warnings and worse, which is what matters */
    e->selected = -1;
    e->follow = true;
    events_reload(e);

    window_t *w = desktop_new_window(wm, "Event Viewer", ICON_LOG, 760, 500, events_proc, e);
    if (!w) { free(e); return; }
    w->min_w = 520;
    w->min_h = 320;
}
