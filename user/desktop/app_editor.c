/* app_editor.c - a plain text editor.
 *
 * Enough to read a configuration file and change it: typing, the arrow keys,
 * backspace, Enter, and save.  It refuses to open something that does not look
 * like text rather than filling the window with rubbish.
 */
#include "desktop.h"

#define MAX_LINES 2000
#define MAX_LINE  512

typedef struct {
    char   path[512];
    char  *line[MAX_LINES];
    int    count;
    int    cursor_line, cursor_col;
    int    scroll;
    bool   dirty;
    bool   readonly;
    char   status[200];
} editor_t;

/* Scaled with the interface, like the rest of the desktop.  These were bare
 * pixels (40 tall toolbar, 26 tall status, 84-wide buttons at x 10 and 102)
 * while the text inside them doubled with the scale, so on a 2x display the two
 * buttons' labels overran their boxes and merged ("SaveReload"), the filename
 * started at a fixed x=200 on top of them, and the status bar was too short for
 * its own line and clipped at the bottom of the window. */
#define TOOLBAR_H (40 * gui_scale())
#define STATUS_H  (26 * gui_scale())

static int line_h(void) { return gui_font_height(FONT_MONO) + 2 * gui_scale(); }

static rect_t text_rect(surface_t *s) {
    return rect_make(0, TOOLBAR_H, s->width - 12 * gui_scale(),
                     s->height - TOOLBAR_H - STATUS_H);
}
static rect_t save_button(void) {
    const int k = gui_scale();
    int w = gui_text_width(FONT_UI, "Reload") + 28 * k;   /* the wider label sizes both */
    return rect_make(10 * k, 6 * k, w, TOOLBAR_H - 12 * k);
}
static rect_t new_button(void) {
    const int k = gui_scale();
    rect_t sv = save_button();
    return rect_make(sv.x + sv.w + 8 * k, sv.y, sv.w, sv.h);
}

/* ------------------------------------------------------------------ buffer */

static void editor_free_lines(editor_t *e) {
    for (int i = 0; i < e->count; i++) free(e->line[i]);
    e->count = 0;
}

static bool editor_add_line(editor_t *e, const char *text, size_t len) {
    if (e->count >= MAX_LINES) return false;
    if (len > MAX_LINE - 1) len = MAX_LINE - 1;

    char *copy = malloc(MAX_LINE);
    if (!copy) return false;
    memcpy(copy, text, len);
    copy[len] = 0;
    e->line[e->count++] = copy;
    return true;
}

static void editor_load(editor_t *e, const char *path) {
    editor_free_lines(e);
    strlcpy(e->path, path, sizeof e->path);
    e->cursor_line = e->cursor_col = e->scroll = 0;
    e->dirty = false;
    e->readonly = false;

    kstat_t st;
    if (stat(path, &st) < 0) {
        editor_add_line(e, "", 0);
        snprintf(e->status, sizeof e->status, "New file");
        return;
    }
    if (st.type == FT_DIR) {
        editor_add_line(e, "", 0);
        snprintf(e->status, sizeof e->status, "%s is a directory", path);
        e->readonly = true;
        return;
    }
    if (st.size > 512 * 1024) {
        editor_add_line(e, "", 0);
        snprintf(e->status, sizeof e->status, "That file is too large to edit here (%llu KiB)",
                 (unsigned long long)(st.size / 1024));
        e->readonly = true;
        return;
    }

    char *buf = malloc((size_t)st.size + 1);
    if (!buf) { editor_add_line(e, "", 0); return; }

    ssize_t n = read_file(path, buf, (size_t)st.size);
    if (n < 0) {
        free(buf);
        editor_add_line(e, "", 0);
        snprintf(e->status, sizeof e->status, "%s: %s", path, strerror(errno));
        return;
    }
    buf[n] = 0;

    /* A run of bytes outside the printable range means this is not text. */
    int binary = 0;
    for (ssize_t i = 0; i < n && i < 4096; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0 || (c < 9) || (c > 13 && c < 32)) binary++;
    }
    if (binary > 8) {
        free(buf);
        editor_add_line(e, "", 0);
        e->readonly = true;
        snprintf(e->status, sizeof e->status, "%s does not look like text", path);
        return;
    }

    const char *p = buf;
    const char *end = buf + n;
    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        while (len && p[len - 1] == '\r') len--;
        if (!editor_add_line(e, p, len)) break;
        if (!nl) break;
        p = nl + 1;
    }
    if (!e->count) editor_add_line(e, "", 0);

    free(buf);
    snprintf(e->status, sizeof e->status, "%d line%s", e->count, e->count == 1 ? "" : "s");
}

static void editor_save(editor_t *e) {
    if (e->readonly) {
        snprintf(e->status, sizeof e->status, "This file is open read-only.");
        return;
    }

    size_t total = 0;
    for (int i = 0; i < e->count; i++) total += strlen(e->line[i]) + 1;

    char *buf = malloc(total + 1);
    if (!buf) { snprintf(e->status, sizeof e->status, "Out of memory."); return; }

    size_t n = 0;
    for (int i = 0; i < e->count; i++) {
        size_t len = strlen(e->line[i]);
        memcpy(buf + n, e->line[i], len);
        n += len;
        buf[n++] = '\n';
    }

    int r = write_file(e->path, buf, n);
    free(buf);

    if (r < 0) snprintf(e->status, sizeof e->status, "Could not save: %s", strerror(errno));
    else {
        sync();
        e->dirty = false;
        snprintf(e->status, sizeof e->status, "Saved %d line%s to %s",
                 e->count, e->count == 1 ? "" : "s", e->path);
    }
}

/* ------------------------------------------------------------------- paint */

static void editor_paint(editor_t *e, surface_t *s, int mx, int my) {
    gui_clear(s, g_theme.window);

    gui_fill(s, rect_make(0, 0, s->width, TOOLBAR_H), colour_shade(g_theme.window, 5));
    gui_hline(s, 0, TOOLBAR_H - 1, s->width, g_theme.window_border);

    rect_t save = save_button(), fresh = new_button();
    gui_button(s, save, "Save", rect_contains(save, mx, my), false, !e->readonly);
    gui_button(s, fresh, "Reload", rect_contains(fresh, mx, my), false, true);

    const int k = gui_scale();
    char head[560];
    snprintf(head, sizeof head, "%s%s", e->path, e->dirty ? " *" : "");
    int hx = fresh.x + fresh.w + 16 * k;                /* clear of the buttons */
    gui_text_clipped(s, FONT_UI, hx, (TOOLBAR_H - gui_font_height(FONT_UI)) / 2,
                     s->width - hx - 12 * k, head,
                     e->dirty ? g_theme.warning : g_theme.text);

    rect_t text = text_rect(s);
    rect_t saved = surface_clip(s);
    surface_set_clip(s, text);

    int lh = line_h();
    int cw = gui_font_advance(FONT_MONO, 'M');
    int gutter = 52 * k;
    int visible = text.h / lh;

    gui_fill(s, rect_make(text.x, text.y, gutter - 8, text.h), colour_shade(g_theme.window, -5));

    for (int i = 0; i < visible && e->scroll + i < e->count; i++) {
        int index = e->scroll + i;
        int y = text.y + i * lh + 2;

        char num[12];
        snprintf(num, sizeof num, "%d", index + 1);
        gui_text_right(s, FONT_MONO, rect_make(text.x, y, gutter - 16, lh), num,
                       index == e->cursor_line ? g_theme.accent : g_theme.text_dim);

        if (index == e->cursor_line)
            gui_fill(s, rect_make(text.x + gutter - 8, text.y + i * lh, text.w - gutter + 8, lh),
                     colour_shade(g_theme.window, 4));

        gui_text(s, FONT_MONO, text.x + gutter, y, e->line[index], g_theme.text);
    }

    /* Caret. */
    int cy = e->cursor_line - e->scroll;
    if (cy >= 0 && cy < visible && (uptime_ms() / 500) % 2 == 0) {
        int cx = text.x + gutter + e->cursor_col * cw;
        gui_fill(s, rect_make(cx, text.y + cy * lh + 1, 1, lh - 2), g_theme.accent);
    }

    surface_set_clip(s, saved);

    if (e->count > visible)
        gui_scrollbar(s, rect_make(s->width - 12 * k, text.y, 12 * k, text.h),
                      e->scroll, visible, e->count);

    rect_t status = rect_make(0, s->height - STATUS_H, s->width, STATUS_H);
    gui_fill(s, status, colour_shade(g_theme.window, 5));
    gui_hline(s, 0, status.y, s->width, g_theme.window_border);
    int sy = status.y + (STATUS_H - gui_font_height(FONT_UI)) / 2;
    gui_text(s, FONT_UI, 10 * k, sy, e->status, g_theme.text_dim);

    char pos[40];
    snprintf(pos, sizeof pos, "line %d, column %d", e->cursor_line + 1, e->cursor_col + 1);
    gui_text_right(s, FONT_UI, rect_make(0, status.y, s->width - 12 * k, status.h),
                   pos, g_theme.text_dim);
}

/* ------------------------------------------------------------------ typing */

static void clamp_cursor(editor_t *e, surface_t *s) {
    if (e->cursor_line < 0) e->cursor_line = 0;
    if (e->cursor_line >= e->count) e->cursor_line = e->count - 1;

    int len = (int)strlen(e->line[e->cursor_line]);
    if (e->cursor_col > len) e->cursor_col = len;
    if (e->cursor_col < 0) e->cursor_col = 0;

    int visible = text_rect(s).h / line_h();
    if (e->cursor_line < e->scroll) e->scroll = e->cursor_line;
    if (e->cursor_line >= e->scroll + visible) e->scroll = e->cursor_line - visible + 1;
    if (e->scroll < 0) e->scroll = 0;
}

static bool editor_key(editor_t *e, surface_t *s, uint32_t key) {
    char *line = e->line[e->cursor_line];
    int len = (int)strlen(line);

    switch (key) {
    case KK_LEFT:
        if (e->cursor_col > 0) e->cursor_col--;
        else if (e->cursor_line > 0) { e->cursor_line--; e->cursor_col = (int)strlen(e->line[e->cursor_line]); }
        break;
    case KK_RIGHT:
        if (e->cursor_col < len) e->cursor_col++;
        else if (e->cursor_line < e->count - 1) { e->cursor_line++; e->cursor_col = 0; }
        break;
    case KK_UP:       e->cursor_line--; break;
    case KK_DOWN:     e->cursor_line++; break;
    case KK_HOME:     e->cursor_col = 0; break;
    case KK_END:      e->cursor_col = len; break;
    case KK_PAGEUP:   e->cursor_line -= text_rect(s).h / line_h(); break;
    case KK_PAGEDOWN: e->cursor_line += text_rect(s).h / line_h(); break;

    case '\b':
        if (e->readonly) break;
        if (e->cursor_col > 0) {
            memmove(line + e->cursor_col - 1, line + e->cursor_col, (size_t)(len - e->cursor_col + 1));
            e->cursor_col--;
            e->dirty = true;
        } else if (e->cursor_line > 0) {
            /* Join with the line above. */
            char *prev = e->line[e->cursor_line - 1];
            int plen = (int)strlen(prev);
            if (plen + len < MAX_LINE - 1) {
                memcpy(prev + plen, line, (size_t)len + 1);
                free(line);
                memmove(&e->line[e->cursor_line], &e->line[e->cursor_line + 1],
                        sizeof(char *) * (size_t)(e->count - e->cursor_line - 1));
                e->count--;
                e->cursor_line--;
                e->cursor_col = plen;
                e->dirty = true;
            }
        }
        break;

    case KK_DELETE:
        if (e->readonly) break;
        if (e->cursor_col < len) {
            memmove(line + e->cursor_col, line + e->cursor_col + 1, (size_t)(len - e->cursor_col));
            e->dirty = true;
        }
        break;

    case '\n': {
        if (e->readonly) break;
        if (e->count >= MAX_LINES) break;
        char *tail = malloc(MAX_LINE);
        if (!tail) break;
        strlcpy(tail, line + e->cursor_col, MAX_LINE);
        line[e->cursor_col] = 0;

        memmove(&e->line[e->cursor_line + 2], &e->line[e->cursor_line + 1],
                sizeof(char *) * (size_t)(e->count - e->cursor_line - 1));
        e->line[e->cursor_line + 1] = tail;
        e->count++;
        e->cursor_line++;
        e->cursor_col = 0;
        e->dirty = true;
        break;
    }

    default:
        if (e->readonly) break;
        if (key < 32 || key > 126) {
            if (key != '\t') return false;
            key = ' ';
        }
        if (len + 1 >= MAX_LINE - 1) break;
        memmove(line + e->cursor_col + 1, line + e->cursor_col, (size_t)(len - e->cursor_col + 1));
        line[e->cursor_col++] = (char)key;
        e->dirty = true;
        break;
    }

    clamp_cursor(e, s);
    return true;
}

/* ------------------------------------------------------------------ window */

static bool editor_proc(window_t *w, const wevent_t *ev) {
    editor_t *e = w->data;
    surface_t *s = w->canvas;

    switch (ev->kind) {
    case WE_PAINT:
        editor_paint(e, s, ev->x, ev->y);
        return false;

    case WE_MOUSE_MOVE:
        return true;

    case WE_MOUSE_WHEEL: {
        int visible = text_rect(s).h / line_h();
        e->scroll -= ev->wheel * 3;
        if (e->scroll > e->count - visible) e->scroll = e->count - visible;
        if (e->scroll < 0) e->scroll = 0;
        return true;
    }

    case WE_MOUSE_DOWN: {
        if (rect_contains(save_button(), ev->x, ev->y)) { editor_save(e); return true; }
        if (rect_contains(new_button(), ev->x, ev->y)) {
            char path[512];
            strlcpy(path, e->path, sizeof path);
            editor_load(e, path);
            return true;
        }
        rect_t text = text_rect(s);
        if (rect_contains(text, ev->x, ev->y)) {
            int cw = gui_font_advance(FONT_MONO, 'M');
            e->cursor_line = e->scroll + (ev->y - text.y) / line_h();
            e->cursor_col = (ev->x - text.x - 52) / cw;
            clamp_cursor(e, s);
            return true;
        }
        return false;
    }

    case WE_KEY_DOWN:
        if ((ev->mods & KMOD_CTRL) && (ev->key == 's' || ev->key == 19)) { editor_save(e); return true; }
        return editor_key(e, s, ev->key);

    case WE_TICK:
        return true;      /* the caret blinks */

    case WE_CLOSE:
        editor_free_lines(e);
        free(e);
        return false;

    default:
        return false;
    }
}

static void editor_open_window(wm_t *wm, const char *path) {
    editor_t *e = calloc(1, sizeof *e);
    if (!e) return;
    editor_load(e, path);

    const char *leaf = strrchr(path, '/');
    char title[96];
    snprintf(title, sizeof title, "%s - Text Editor", leaf && leaf[1] ? leaf + 1 : path);

    window_t *w = desktop_new_window(wm, title, ICON_EDITOR, 660, 460, editor_proc, e);
    if (!w) { editor_free_lines(e); free(e); return; }
    w->min_w = 400;
    w->min_h = 260;
}

void app_editor_open(wm_t *wm, const char *path) { editor_open_window(wm, path); }

void app_editor_launch(wm_t *wm) {
    /* Something is always more useful than an empty buffer, and the message of
     * the day is a file every installation has. */
    editor_open_window(wm, file_exists("/etc/motd") ? "/etc/motd" : "/untitled.txt");
}
