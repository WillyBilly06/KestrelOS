/* app_files.c - a file browser.
 *
 * Lists a directory, navigates into subdirectories, and opens text files in
 * the editor.  Deleting asks first, because there is no way back.
 */
#include "desktop.h"

#define MAX_ENTRIES 512
/* Lengths here scale with the interface.
 *
 * Written as plain numbers they are correct on exactly one display and wrong
 * on every other: the text drawn against them grows with the interface scale
 * and the boxes holding it do not, so rows sit on top of one another and
 * columns run into the ones beside them.  This file used gui_scale() nowhere
 * at all before that was noticed on a 1440p screen.
 */
#define ROW_H       (28 * gui_scale())

typedef struct {
    char      name[128];
    uint32_t  type;
    uint64_t  size;
} entry_t;

typedef struct {
    char     path[512];
    entry_t  entries[MAX_ENTRIES];
    int      count;
    int      selected;
    int      scroll;
    int      hover;
    bool     confirm_delete;
    char     status[160];
} files_t;

static int compare_entries(const entry_t *a, const entry_t *b) {
    /* Directories first, then by name, so a deep tree stays navigable. */
    if ((a->type == FT_DIR) != (b->type == FT_DIR)) return a->type == FT_DIR ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void files_load(files_t *f) {
    f->count = 0;
    f->selected = -1;
    f->scroll = 0;
    f->confirm_delete = false;

    DIR *d = opendir(f->path);
    if (!d) {
        snprintf(f->status, sizeof f->status, "%s: %s", f->path, strerror(errno));
        return;
    }

    kdirent_t e;
    while (f->count < MAX_ENTRIES && readdir(d, &e) == 0) {
        entry_t *dst = &f->entries[f->count++];
        strlcpy(dst->name, e.name, sizeof dst->name);
        dst->type = e.type;
        dst->size = e.size;
    }
    closedir(d);

    /* Insertion sort: the lists here are short and it keeps the code obvious. */
    for (int i = 1; i < f->count; i++) {
        entry_t key = f->entries[i];
        int j = i - 1;
        while (j >= 0 && compare_entries(&f->entries[j], &key) > 0) {
            f->entries[j + 1] = f->entries[j];
            j--;
        }
        f->entries[j + 1] = key;
    }

    snprintf(f->status, sizeof f->status, "%d item%s", f->count, f->count == 1 ? "" : "s");
}

static void files_go(files_t *f, const char *name) {
    char next[512];
    if (!strcmp(name, "..")) {
        strlcpy(next, f->path, sizeof next);
        char *slash = strrchr(next, '/');
        if (!slash) return;
        if (slash == next) next[1] = 0;
        else *slash = 0;
    } else if (!strcmp(f->path, "/")) {
        snprintf(next, sizeof next, "/%s", name);
    } else {
        snprintf(next, sizeof next, "%s/%s", f->path, name);
    }
    strlcpy(f->path, next, sizeof f->path);
    files_load(f);
}

/* ------------------------------------------------------------------- paint */

#define TOOLBAR_H (40 * gui_scale())
#define STATUS_H  (26 * gui_scale())

static rect_t list_rect(surface_t *s) {
    const int k = gui_scale();
    return rect_make(0, TOOLBAR_H, s->width - 12 * k, s->height - TOOLBAR_H - STATUS_H);
}

/* Toolbar controls scale with the interface.  Bare 6/28 heights and 84/88
 * widths left the buttons half-size with 2x-tall text overflowing them, and the
 * unscaled right-edge offsets crammed "Open" and "Delete" into each other on a
 * 2x display - the "OpenDelete" smush. */
static rect_t up_button(void) {
    const int k = gui_scale();
    return rect_make(8 * k, 6 * k, 40 * k, 28 * k);
}
static rect_t open_button(surface_t *s) {
    const int k = gui_scale();
    return rect_make(s->width - 190 * k, 6 * k, 84 * k, 28 * k);
}
static rect_t delete_button(surface_t *s) {
    const int k = gui_scale();
    return rect_make(s->width - 98 * k, 6 * k, 88 * k, 28 * k);
}

static void files_paint(files_t *f, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    /* Toolbar. */
    gui_fill(s, rect_make(0, 0, s->width, TOOLBAR_H), colour_shade(g_theme.window, 5));
    gui_hline(s, 0, TOOLBAR_H - 1, s->width, g_theme.window_border);

    rect_t up = up_button();
    gui_button_icon(s, up, ICON_ARROW_UP, NULL, rect_contains(up, mx, my), false,
                    strcmp(f->path, "/") != 0);

    const int k = gui_scale();
    rect_t pathbox = rect_make(56 * k, 6 * k, s->width - 56 * k - 200 * k, 28 * k);
    gui_round_rect_aa(s, pathbox, 3, g_theme.field);
    gui_round_frame_aa(s, pathbox, 3, g_theme.field_border);
    gui_text_clipped(s, FONT_UI, pathbox.x + 8 * k, pathbox.y + 4 * k, pathbox.w - 16 * k,
                     f->path, g_theme.field_text);

    bool has_sel = f->selected >= 0 && f->selected < f->count;
    rect_t ob = open_button(s), db = delete_button(s);
    gui_button(s, ob, "Open", rect_contains(ob, mx, my), false, has_sel);
    gui_button(s, db, f->confirm_delete ? "Confirm" : "Delete",
               rect_contains(db, mx, my), false, has_sel);

    /* The list. */
    rect_t list = list_rect(s);
    rect_t saved = surface_clip(s);
    surface_set_clip(s, list);

    int visible = list.h / ROW_H;
    for (int i = 0; i < visible && f->scroll + i < f->count; i++) {
        int index = f->scroll + i;
        entry_t *e = &f->entries[index];
        rect_t row = rect_make(list.x, list.y + i * ROW_H, list.w, ROW_H);

        if (index == f->selected) gui_fill(s, row, g_theme.selection);
        else if (index == f->hover) gui_fill(s, row, colour_shade(g_theme.window, 6));

        colour_t tc = index == f->selected ? g_theme.selection_text : g_theme.text;
        icon_id icon = e->type == FT_DIR ? ICON_FOLDER
                     : e->type == FT_BLK ? ICON_DISK
                     : e->type == FT_CHR ? ICON_TERMINAL : ICON_FILE;

        gui_icon(s, icon, row.x + 10, row.y + (ROW_H - 16) / 2, 16,
                 index == f->selected ? tc : (e->type == FT_DIR ? g_theme.accent : g_theme.text_dim));
        gui_text_clipped(s, FONT_UI, row.x + 36, row.y + (ROW_H - gui_font_height(FONT_UI)) / 2 + 1,
                         row.w - 160, e->name, tc);

        if (e->type != FT_DIR) {
            char size[24];
            format_size(size, sizeof size, e->size);
            gui_text_right(s, FONT_UI, rect_make(row.x, row.y, row.w - 14, row.h), size,
                           index == f->selected ? tc : g_theme.text_dim);
        }
    }

    if (!f->count)
        gui_text_centred(s, FONT_UI, list, "This folder is empty", g_theme.text_dim);

    surface_set_clip(s, saved);

    if (f->count > visible)
        gui_scrollbar(s, rect_make(s->width - 12 * k, list.y, 12 * k, list.h), f->scroll, visible, f->count);

    /* Status bar. */
    rect_t status = rect_make(0, s->height - STATUS_H, s->width, STATUS_H);
    gui_fill(s, status, colour_shade(g_theme.window, 5));
    gui_hline(s, 0, status.y, s->width, g_theme.window_border);
    gui_text(s, FONT_UI, 10, status.y + 3, f->status,
             f->confirm_delete ? g_theme.warning : g_theme.text_dim);
}

/* ------------------------------------------------------------------ events */

static void files_activate(window_t *w, files_t *f) {
    if (f->selected < 0 || f->selected >= f->count) return;
    entry_t *e = &f->entries[f->selected];

    if (e->type == FT_DIR) { files_go(f, e->name); return; }
    if (e->type == FT_BLK || e->type == FT_CHR) {
        snprintf(f->status, sizeof f->status, "%s is a device, not a file", e->name);
        return;
    }

    char full[600];
    if (!strcmp(f->path, "/")) snprintf(full, sizeof full, "/%s", e->name);
    else snprintf(full, sizeof full, "%s/%s", f->path, e->name);

    /* A program opens by running, not by being read.  A Windows .exe counts:
     * the shell hands one to the Windows loader, so it is the same gesture. */
    size_t len = strlen(e->name);
    bool is_exe = len > 4 && !strcasecmp(e->name + len - 4, ".exe");
    bool in_bin = !strncmp(f->path, "/bin", 4);

    if (is_exe || in_bin) {
        app_terminal_run(w->wm, full);
        snprintf(f->status, sizeof f->status, "running %s", e->name);
        return;
    }

    app_editor_open(w->wm, full);
}

static bool files_proc(window_t *w, const wevent_t *ev) {
    files_t *f = w->data;
    surface_t *s = w->canvas;
    rect_t list = list_rect(s);
    int visible = list.h / ROW_H;

    switch (ev->kind) {
    case WE_PAINT:
        files_paint(f, s, ev->x, ev->y);
        return false;

    case WE_MOUSE_MOVE: {
        int old = f->hover;
        f->hover = -1;
        if (rect_contains(list, ev->x, ev->y)) {
            int index = f->scroll + (ev->y - list.y) / ROW_H;
            if (index >= 0 && index < f->count) f->hover = index;
        }
        return f->hover != old || true;
    }

    case WE_MOUSE_WHEEL:
        f->scroll -= ev->wheel * 3;
        if (f->scroll > f->count - visible) f->scroll = f->count - visible;
        if (f->scroll < 0) f->scroll = 0;
        return true;

    case WE_MOUSE_DOWN: {
        if (rect_contains(up_button(), ev->x, ev->y)) {
            if (strcmp(f->path, "/")) files_go(f, "..");
            return true;
        }
        if (rect_contains(open_button(s), ev->x, ev->y)) { files_activate(w, f); return true; }
        if (rect_contains(delete_button(s), ev->x, ev->y)) {
            if (f->selected < 0) return true;
            entry_t *e = &f->entries[f->selected];
            if (!f->confirm_delete) {
                f->confirm_delete = true;
                snprintf(f->status, sizeof f->status,
                         "Delete %s? Press Delete again to confirm.", e->name);
                return true;
            }
            char full[600];
            if (!strcmp(f->path, "/")) snprintf(full, sizeof full, "/%s", e->name);
            else snprintf(full, sizeof full, "%s/%s", f->path, e->name);

            if (unlink(full) < 0) snprintf(f->status, sizeof f->status, "%s: %s", e->name, strerror(errno));
            else { files_load(f); snprintf(f->status, sizeof f->status, "Deleted."); }
            f->confirm_delete = false;
            return true;
        }
        if (rect_contains(list, ev->x, ev->y)) {
            int index = f->scroll + (ev->y - list.y) / ROW_H;
            if (index >= 0 && index < f->count) {
                if (f->selected == index) files_activate(w, f);
                else { f->selected = index; f->confirm_delete = false; }
            }
            return true;
        }
        return false;
    }

    case WE_KEY_DOWN:
        switch (ev->key) {
        case KK_UP:
            if (f->selected > 0) f->selected--;
            else if (f->selected < 0 && f->count) f->selected = 0;
            if (f->selected < f->scroll) f->scroll = f->selected;
            return true;
        case KK_DOWN:
            if (f->selected < f->count - 1) f->selected++;
            if (f->selected >= f->scroll + visible) f->scroll = f->selected - visible + 1;
            return true;
        case '\n':
            files_activate(w, f);
            return true;
        case '\b':
        case KK_LEFT:
            if (strcmp(f->path, "/")) files_go(f, "..");
            return true;
        case KK_DELETE:
            if (f->selected >= 0) {
                f->confirm_delete = !f->confirm_delete;
                if (f->confirm_delete)
                    snprintf(f->status, sizeof f->status, "Delete %s? Press Delete again to confirm.",
                             f->entries[f->selected].name);
                else snprintf(f->status, sizeof f->status, "Cancelled.");
            }
            return true;
        default:
            return false;
        }

    case WE_CLOSE:
        free(f);
        return false;

    default:
        return false;
    }
}

void app_files_launch(wm_t *wm) {
    files_t *f = calloc(1, sizeof *f);
    if (!f) return;
    strlcpy(f->path, "/", sizeof f->path);
    f->selected = -1;
    f->hover = -1;
    files_load(f);

    window_t *w = desktop_new_window(wm, "Files", ICON_FOLDER, 620, 440, files_proc, f);
    if (!w) { free(f); return; }
    w->min_w = 400;
    w->min_h = 240;
}
