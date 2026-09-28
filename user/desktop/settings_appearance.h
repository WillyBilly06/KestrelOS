/* Measured, wrapping appearance controls. Paint and input share this model. */
#ifndef KESTREL_SETTINGS_APPEARANCE_H
#define KESTREL_SETTINGS_APPEARANCE_H
typedef struct {
    rect_t bounds;
    int columns, cell_width, cell_height, gap, count;
} settings_grid_t;
typedef struct {
    settings_grid_t groups[4]; /* themes, accents, wallpapers, text sizes */
    int labels[4], animation_y, total_height;
    rect_t viewport, track, animation;
} settings_appearance_t;

static settings_grid_t settings_grid(int width, int y, int wanted_width,
                                     int cell_height, int gap, int count) {
    settings_grid_t g = {0};
    if (width < 1 || wanted_width < 1 || cell_height < 1 || gap < 0 ||
        count < 1 || count > 1024) return g;
    g.cell_width = wanted_width < width ? wanted_width : width;
    g.cell_height = cell_height;
    g.gap = gap; g.count = count;
    g.columns = (int)(((int64_t)width + gap) / ((int64_t)g.cell_width + gap));
    if (g.columns > count) g.columns = count;
    int rows = (count + g.columns - 1) / g.columns;
    g.bounds = rect_make(0, y, width, rows * cell_height + (rows - 1) * gap);
    return g;
}
static rect_t settings_grid_item(settings_grid_t g, int index, int x, int y) {
    if (index < 0 || index >= g.count || g.columns < 1) return rect_make(0,0,0,0);
    return rect_make(x + index % g.columns * (g.cell_width + g.gap),
                     y + g.bounds.y + index / g.columns * (g.cell_height + g.gap),
                     g.cell_width, g.cell_height);
}
static settings_appearance_t settings_appearance_layout(rect_t viewport, int scale,
                                                        int font_height,
                                                        const int widths[4],
                                                        const int counts[4]) {
    settings_appearance_t a = {0};
    a.viewport = viewport;
    if (viewport.w < 1 || viewport.h < 1 || scale < 1 || scale > 8 ||
        font_height < 1 || font_height > 512) return a;
    int track = 12 * scale, gap = 8 * scale;
    if (track > viewport.w / 4) track = viewport.w / 4;
    int content_width = viewport.w - track - gap;
    if (content_width < 1) content_width = 1;
    a.track = rect_make(viewport.x + viewport.w - track, viewport.y, track, viewport.h);
    int y = 2 * font_height + 24 * scale;
    for (int section = 0; section < 4; section++) {
        if (section == 3) {
            a.animation_y = y;
            y += font_height + gap;
            int switch_width = 46 * scale;
            if (switch_width > content_width) switch_width = content_width;
            a.animation = rect_make(0, y, switch_width, 26 * scale);
            y += a.animation.h + 24 * scale;
        }
        a.labels[section] = y;
        y += font_height + 12 * scale;
        a.groups[section] = settings_grid(content_width, y, widths[section],
            section == 1 ? 32 * scale : font_height + 14 * scale, gap, counts[section]);
        y += a.groups[section].bounds.h + 24 * scale;
    }
    a.total_height = y;
    return a;
}
static int settings_scroll_clamp(int64_t requested, int total, int visible) {
    int max = total > visible && visible > 0 ? total - visible : 0;
    return requested < 0 ? 0 : requested > max ? max : (int)requested;
}
typedef struct { rect_t preview, label, marker; } settings_choice_parts_t;
static settings_choice_parts_t settings_choice_parts(rect_t r, int scale) {
    settings_choice_parts_t p = {0};
    if (r.w <= 0 || r.h <= 0 || scale < 1 || scale > 8) return p;
    int pad = 8 * scale;
    if (pad > r.w / 2) pad = r.w / 2;
    int py = 4 * scale;
    if (py > r.h / 2) py = r.h / 2;
    int preview = r.w >= 120 * scale ? 40 * scale : 0;
    int marker = r.w >= 50 * scale ? 16 * scale : 0;
    int gap = preview ? 10 * scale : 0;
    p.preview = rect_make(r.x+pad,r.y+py,preview,r.h-2*py);
    p.marker = rect_make(r.x+r.w-pad-marker,r.y+(r.h-marker)/2,marker,marker);
    if (p.marker.h > r.h) { p.marker.y = r.y; p.marker.h = r.h; }
    int x = r.x+pad+preview+gap;
    int right = p.marker.x-(marker ? 4*scale : 0);
    p.label = rect_make(x,r.y,right>x ? right-x : 0,r.h);
    return p;
}
#endif
