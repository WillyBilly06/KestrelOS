/* draw.c - surfaces, primitives, text and icons.
 *
 * Every drawing call clips against the surface's current clip rectangle, so a
 * widget can be handed a sub-rectangle and draw freely without checking bounds
 * itself.  Colours are stored as 0x00RRGGBB and converted to the framebuffer's
 * channel order only when a surface is presented.
 */
#include "gui.h"
#include "../../include/kestrel/gpu2d.h"
#include "font_ui.h"
#include "font_uiaa.h"
#include "font_ui2x.h"
#include "font_ui3x.h"
#include "font_mono.h"

/* Compact by default. Explicit accessibility scaling uses the pre-rasterized
 * 1x/2x/3x font faces and scales matching control metrics. */
static int ui_scale = 1;
static bool ui_scale_explicit;

/* Scoped content override used by window painting and hit testing. Zero uses
 * the desktop preference; window dimensions do not determine pixel density. */
static int scale_override = 0;

static int eff_scale(void) { return scale_override ? scale_override : ui_scale; }

int gui_scale(void) { return eff_scale(); }

/* The display's own scale, ignoring any window override - for the chrome and
 * the desktop, which are the same size whatever window is in front. */
int gui_screen_scale(void) { return ui_scale; }

int gui_scale_push(int scale) {
    int prev = scale_override;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    scale_override = scale;
    return prev;
}
void gui_scale_pop(int prev) { scale_override = prev; }

void gui_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    ui_scale = scale;
    ui_scale_explicit = true;
}

void gui_set_default_scale(int scale) {
    if (ui_scale_explicit) return;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    ui_scale = scale;
}

/* Default when no explicit scale preference was loaded. */
int gui_scale_for_width(int width) {
    /* Pixel width is not physical density and may span several monitors.
     * Use compact 100% until the user chooses an accessibility scale. */
    (void)width;
    return 1;
}

theme_t g_theme;

/* ------------------------------------------------------------------ colour */

colour_t colour_mix(colour_t a, colour_t b, int alpha) {
    if (alpha <= 0) return a;
    if (alpha >= 255) return b;
    int inv = 255 - alpha;
    int r = (int)((RGB_R(a) * inv + RGB_R(b) * alpha) / 255);
    int g = (int)((RGB_G(a) * inv + RGB_G(b) * alpha) / 255);
    int bl = (int)((RGB_B(a) * inv + RGB_B(b) * alpha) / 255);
    return RGB(r, g, bl);
}

colour_t colour_shade(colour_t c, int percent) {
    int r = (int)RGB_R(c), g = (int)RGB_G(c), b = (int)RGB_B(c);
    if (percent >= 0) {
        r += (255 - r) * percent / 100;
        g += (255 - g) * percent / 100;
        b += (255 - b) * percent / 100;
    } else {
        r += r * percent / 100;
        g += g * percent / 100;
        b += b * percent / 100;
    }
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* ------------------------------------------------------------------ themes
 *
 * A palette used to be thirty-odd colours written out by hand, once per theme.
 * That is why there were two themes and no way to change the accent: every new
 * look meant another thirty constants, and moving the accent meant finding
 * every colour derived from it by eye and moving those too.
 *
 * So a theme is now described by the few decisions that actually differ - the
 * surface colours, an accent, and whether it is a dark look or a light one -
 * and the rest is derived from those.  Controls sit a step away from the
 * window, text is placed for contrast against what is behind it, and the
 * selection follows the accent.  Choosing a different accent recolours
 * everything that should follow it and nothing that should not, which is what
 * makes it worth offering as a choice rather than as a rebuild.
 */

/* Lift a surface away from the background: lighter on a dark theme, darker on
 * a light one.  Every control that has to read as raised uses this, so which
 * direction "raised" means is decided in one place. */
static colour_t lift(colour_t c, bool dark, int amount) {
    return colour_shade(c, dark ? amount : -amount);
}

void theme_build(const theme_spec_t *spec) {
    bool dark = spec->dark;

    g_theme.desktop_top    = spec->desktop_top;
    g_theme.desktop_bottom = spec->desktop_bottom;

    g_theme.window        = spec->window;
    g_theme.window_border = lift(spec->window, dark, 34);
    g_theme.window_shadow = colour_shade(spec->window, dark ? -60 : -55);

    /* The active title bar leans towards the accent, so which window has focus
     * is clear at a glance without another colour having to be chosen. */
    g_theme.title_active   = colour_mix(spec->window, spec->accent, dark ? 46 : 30);
    g_theme.title_inactive = lift(spec->window, dark, 6);
    g_theme.title_text     = dark ? RGB(0xF0, 0xF3, 0xF8) : RGB(0x1A, 0x1E, 0x26);
    g_theme.title_text_inactive = dark ? RGB(0x8A, 0x92, 0xA0) : RGB(0x76, 0x7E, 0x8A);

    g_theme.text        = dark ? RGB(0xD8, 0xDD, 0xE6) : RGB(0x1E, 0x22, 0x2A);
    g_theme.text_dim    = dark ? RGB(0x8A, 0x92, 0xA0) : RGB(0x6A, 0x72, 0x7E);
    g_theme.text_bright = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);

    g_theme.accent      = spec->accent;
    g_theme.accent_dark = colour_shade(spec->accent, -32);

    /* Dark text on a pale accent, light text on a deep one, so that whatever
     * accent somebody picks the text on top of it stays readable. */
    {
        int luma = (int)((RGB_R(spec->accent) * 30 + RGB_G(spec->accent) * 59 +
                          RGB_B(spec->accent) * 11) / 100);
        g_theme.accent_text = luma > 150 ? RGB(0x10, 0x14, 0x1A)
                                         : RGB(0xFF, 0xFF, 0xFF);
    }

    g_theme.control        = lift(spec->window, dark, 18);
    g_theme.control_hover  = lift(spec->window, dark, 32);
    g_theme.control_press  = lift(spec->window, dark, -10);
    g_theme.control_border = lift(spec->window, dark, 46);

    g_theme.field        = dark ? colour_shade(spec->window, -30) : RGB(0xFF, 0xFF, 0xFF);
    g_theme.field_border = lift(spec->window, dark, 40);
    g_theme.field_text   = dark ? RGB(0xE4, 0xE8, 0xF0) : RGB(0x10, 0x14, 0x1A);

    g_theme.taskbar      = colour_shade(spec->window, dark ? -24 : 10);
    g_theme.taskbar_text = dark ? RGB(0xD0, 0xD6, 0xE0) : RGB(0x20, 0x26, 0x30);
    g_theme.taskbar_hover = lift(g_theme.taskbar, dark, 26);

    g_theme.selection      = g_theme.accent_dark;
    g_theme.selection_text = RGB(0xFF, 0xFF, 0xFF);

    /* These three mean the same thing in every theme and must not drift with
     * the accent: a warning that took on the accent's hue would stop reading
     * as a warning at all. */
    g_theme.warning = dark ? RGB(0xE8, 0xB3, 0x39) : RGB(0xB8, 0x7A, 0x00);
    g_theme.error   = dark ? RGB(0xE0, 0x5A, 0x5A) : RGB(0xC0, 0x30, 0x30);
    g_theme.success = dark ? RGB(0x5A, 0xC8, 0x7A) : RGB(0x1E, 0x8E, 0x40);

    g_theme.scrollbar       = colour_shade(spec->window, dark ? -18 : -8);
    g_theme.scrollbar_thumb = lift(spec->window, dark, 46);
}

/* The looks that come with the system.  Any of them takes any accent, so this
 * is a list of surfaces and a starting accent rather than finished palettes. */
static const theme_spec_t built_in[] = {
    { "Midnight",   RGB(0x22, 0x26, 0x2E), RGB(0x10, 0x18, 0x28), RGB(0x1E, 0x2C, 0x44), RGB(0x4A, 0x9E, 0xE0), true  },
    { "Graphite",   RGB(0x26, 0x26, 0x28), RGB(0x16, 0x16, 0x18), RGB(0x2A, 0x2A, 0x2E), RGB(0x7A, 0x8A, 0x9A), true  },
    { "Deep space", RGB(0x1C, 0x1B, 0x26), RGB(0x0E, 0x0C, 0x18), RGB(0x24, 0x1E, 0x38), RGB(0xA0, 0x7C, 0xE8), true  },
    { "Forest",     RGB(0x1E, 0x26, 0x22), RGB(0x0E, 0x18, 0x14), RGB(0x1C, 0x30, 0x28), RGB(0x4A, 0xC0, 0x84), true  },
    { "Daylight",   RGB(0xF2, 0xF4, 0xF7), RGB(0xD6, 0xE2, 0xF0), RGB(0xB4, 0xC8, 0xDE), RGB(0x1F, 0x6F, 0xC0), false },
    { "Paper",      RGB(0xF6, 0xF4, 0xEE), RGB(0xE8, 0xE2, 0xD4), RGB(0xD2, 0xC8, 0xB4), RGB(0xB0, 0x6A, 0x28), false },
};

int theme_count(void) { return (int)(sizeof built_in / sizeof built_in[0]); }

const theme_spec_t *theme_at(int index) {
    if (index < 0 || index >= theme_count()) return NULL;
    return &built_in[index];
}

/* The accents offered as a row of swatches.  Any colour works; these are the
 * ones worth having one click away. */
static const struct {
    const char *name;
    colour_t    colour;
} accents[] = {
    { "Blue",   RGB(0x4A, 0x9E, 0xE0) },
    { "Teal",   RGB(0x2E, 0xB8, 0xB0) },
    { "Green",  RGB(0x4A, 0xC0, 0x84) },
    { "Amber",  RGB(0xE0, 0xA0, 0x38) },
    { "Red",    RGB(0xE0, 0x6A, 0x5A) },
    { "Purple", RGB(0xA0, 0x7C, 0xE8) },
    { "Pink",   RGB(0xE0, 0x70, 0xB0) },
    { "Slate",  RGB(0x8A, 0x96, 0xA6) },
};

int accent_count(void) { return (int)(sizeof accents / sizeof accents[0]); }

colour_t accent_at(int index) {
    if (index < 0 || index >= accent_count()) return accents[0].colour;
    return accents[index].colour;
}

const char *accent_name(int index) {
    if (index < 0 || index >= accent_count()) return "";
    return accents[index].name;
}

/* Apply one of the built-in looks, optionally with an accent of one's own. */
void theme_apply(int index, colour_t accent) {
    const theme_spec_t *spec = theme_at(index);
    if (!spec) spec = &built_in[0];

    theme_spec_t chosen = *spec;
    if (accent) chosen.accent = accent;
    theme_build(&chosen);
}

/* The two names the rest of the system already used, kept so that nothing has
 * to know about the list above just to ask for a dark or a light look. */
void theme_set_dark(void)  { theme_apply(0, 0); }
void theme_set_light(void) { theme_apply(4, 0); }

/* ---------------------------------------------------------------- geometry */

bool rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }

bool rect_contains(rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

bool rect_intersects(rect_t a, rect_t b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

rect_t rect_intersection(rect_t a, rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return rect_make(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* ---------------------------------------------------------------- surfaces */

/* The clip rectangle is kept beside the surface rather than inside it, so the
 * struct stays a plain description of the pixels. */
#define MAX_SURFACES 64
static struct { const surface_t *s; rect_t clip; } clips[MAX_SURFACES];

static rect_t *clip_slot(const surface_t *s, bool create) {
    for (int i = 0; i < MAX_SURFACES; i++)
        if (clips[i].s == s) return &clips[i].clip;
    if (!create) return NULL;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (clips[i].s) continue;
        clips[i].s = s;
        clips[i].clip = rect_make(0, 0, s->width, s->height);
        return &clips[i].clip;
    }
    return NULL;
}

rect_t surface_clip(const surface_t *s) {
    rect_t *c = clip_slot(s, false);
    return c ? *c : rect_make(0, 0, s->width, s->height);
}

void surface_set_clip(surface_t *s, rect_t clip) {
    rect_t *c = clip_slot(s, true);
    if (c) *c = rect_intersection(clip, rect_make(0, 0, s->width, s->height));
}

void surface_reset_clip(surface_t *s) {
    rect_t *c = clip_slot(s, true);
    if (c) *c = rect_make(0, 0, s->width, s->height);
}

surface_t *surface_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    surface_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->pixels = calloc((size_t)w * h, sizeof(colour_t));
    if (!s->pixels) { free(s); return NULL; }
    s->width = w;
    s->height = h;
    s->stride = w;
    s->owns_pixels = true;
    surface_reset_clip(s);
    return s;
}

void surface_destroy(surface_t *s) {
    if (!s) return;
    gui_gpu_detach(s);
    rect_t *c = clip_slot(s, false);
    if (c) {
        for (int i = 0; i < MAX_SURFACES; i++) if (clips[i].s == s) clips[i].s = NULL;
    }
    if (s->owns_pixels) free(s->pixels);
    free(s);
}

/* ---------------------------------------------------------------- painting */

/* Defined further down, beside the text bookkeeping it belongs to. */
static void text_runs_covered(const surface_t *s, rect_t r);

void gui_pixel(surface_t *s, int x, int y, colour_t c) {
    rect_t clip = surface_clip(s);
    if (!rect_contains(clip, x, y)) return;
    if (gui_gpu_paint(s,rect_make(x,y,1,1),KG2D_SOLID,c,0,255)) return;
    s->pixels[(size_t)y * s->stride + x] = c;
}

void gui_fill(surface_t *s, rect_t r, colour_t c) {
    rect_t a = rect_intersection(r, surface_clip(s));
    if (rect_empty(a)) return;

    text_runs_covered(s, a);
    if (gui_gpu_paint(s,a,KG2D_SOLID,c,0,255)) return;
    for (int y = a.y; y < a.y + a.h; y++)
        blit_fill_row(s->pixels + (size_t)y * s->stride + a.x, a.w, c);
}

void gui_clear(surface_t *s, colour_t c) {
    gui_fill(s, rect_make(0, 0, s->width, s->height), c);
}

void gui_hline(surface_t *s, int x, int y, int w, colour_t c) {
    gui_fill(s, rect_make(x, y, w, 1), c);
}

void gui_vline(surface_t *s, int x, int y, int h, colour_t c) {
    gui_fill(s, rect_make(x, y, 1, h), c);
}

void gui_frame(surface_t *s, rect_t r, colour_t c) {
    if (rect_empty(r)) return;
    gui_hline(s, r.x, r.y, r.w, c);
    gui_hline(s, r.x, r.y + r.h - 1, r.w, c);
    gui_vline(s, r.x, r.y, r.h, c);
    gui_vline(s, r.x + r.w - 1, r.y, r.h, c);
}

void gui_frame_thick(surface_t *s, rect_t r, int thickness, colour_t c) {
    for (int i = 0; i < thickness; i++)
        gui_frame(s, rect_make(r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i), c);
}

/* A line with soft edges, in fixed point.
 *
 * Bresenham puts down one hard pixel per step, which is why every diagonal in
 * every icon in this system looks like a staircase.  This puts down two and
 * splits the ink between them by how far the true line falls between the
 * pixel centres - Xiaolin Wu's method - so an edge that lands between pixels
 * looks like it lands between them.
 *
 * There is no floating point anywhere in this system and there is none here.
 * The gradient is kept as a 16.16 fixed-point number, which for a line no
 * longer than a screen is exact to well under a pixel, and the fractional part
 * doubles as the coverage: the low sixteen bits scaled to 0..255 IS how much
 * of the lower pixel the line covers.
 *
 * Blending, rather than writing.  A line drawn over what is already there has
 * to read it, which a hard pixel never had to do - so this cannot use
 * gui_pixel and does the clip test itself.
 */
#define AA_ONE  (1 << 16)

static void blend_at(surface_t *s, int x, int y, colour_t c, int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    rect_t clip = surface_clip(s);
    if (!rect_contains(clip, x, y)) return;
    if (gui_gpu_paint(s,rect_make(x,y,1,1),KG2D_SOLID,c,0,alpha)) return;
    colour_t *p = &s->pixels[(size_t)y * s->stride + x];
    *p = colour_mix(*p, c, alpha);
}

void gui_blend_pixel(surface_t *s,int x,int y,colour_t c,int alpha) {
    blend_at(s,x,y,c,alpha);
}

void gui_line_aa(surface_t *s, int x0, int y0, int x1, int y1, colour_t c) {
    if(gui_gpu_line_aa(s,x0,y0,x1,y1,c))return;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    /* Straight lines have no fraction to split, and drawing them this way
     * would only make them fainter than the hard version beside them. */
    if (!adx || !ady) { gui_line(s, x0, y0, x1, y1, c); return; }

    bool steep = ady > adx;
    if (steep) {
        int t;
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
        t = dx; dx = dy; dy = t;
    }
    if (x0 > x1) {
        int t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dx = -dx; dy = -dy;
    }

    /* How far the line moves down for each step across, as 16.16. */
    int gradient = (int)(((long long)dy << 16) / (dx ? dx : 1));

    int y = (y0 << 16);
    for (int x = x0; x <= x1; x++) {
        int whole = y >> 16;
        int frac = y & (AA_ONE - 1);
        int lower = (frac * 255) >> 16;      /* how much of the pixel below   */
        int upper = 255 - lower;             /* and what is left for the one
                                              * the line is mostly in         */

        if (steep) {
            blend_at(s, whole, x, c, upper);
            blend_at(s, whole + 1, x, c, lower);
        } else {
            blend_at(s, x, whole, c, upper);
            blend_at(s, x, whole + 1, c, lower);
        }
        y += gradient;
    }
}

void gui_line(surface_t *s, int x0, int y0, int x1, int y1, colour_t c) {
    if(gui_gpu_line(s,x0,y0,x1,y1,c))return;
    /* Bresenham: no floating point anywhere in this system. */
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;
    for (;;) {
        gui_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void gui_gradient_v(surface_t *s, rect_t r, colour_t top, colour_t bottom) {
    rect_t a = rect_intersection(r, surface_clip(s));
    if (rect_empty(a) || r.h <= 0) return;
    if (gui_gpu_paint(s,r,KG2D_GRADIENT_V,top,bottom,255)) return;
    for (int y = a.y; y < a.y + a.h; y++) {
        int t = ((y - r.y) * 255) / (r.h > 1 ? r.h - 1 : 1);
        colour_t c = colour_mix(top, bottom, t);
        blit_fill_row(s->pixels + (size_t)y * s->stride + a.x, a.w, c);
    }
}

void gui_gradient_h(surface_t *s, rect_t r, colour_t left, colour_t right) {
    rect_t a = rect_intersection(r, surface_clip(s));
    if (rect_empty(a) || r.w <= 0) return;
    if (gui_gpu_paint(s,r,KG2D_GRADIENT_H,left,right,255)) return;
    for (int y = a.y; y < a.y + a.h; y++) {
        colour_t *row = s->pixels + (size_t)y * s->stride;
        for (int x = a.x; x < a.x + a.w; x++) {
            int t = ((x - r.x) * 255) / (r.w > 1 ? r.w - 1 : 1);
            row[x] = colour_mix(left, right, t);
        }
    }
}

void gui_blend_rect(surface_t *s, rect_t r, colour_t c, int alpha) {
    rect_t a = rect_intersection(r, surface_clip(s));
    if (rect_empty(a)) return;
    if (gui_gpu_paint(s,a,KG2D_SOLID,c,0,alpha)) return;
    for (int y = a.y; y < a.y + a.h; y++)
        blit_blend_row(s->pixels + (size_t)y * s->stride + a.x, a.w, c, alpha);
}

/* Corner radius is applied by skipping the pixels outside a quarter circle;
 * with no anti-aliasing a simple distance test is enough. */
static bool corner_inside(int dx, int dy, int radius) {
    return dx * dx + dy * dy <= radius * radius;
}

void gui_round_rect(surface_t *s, rect_t r, int radius, colour_t c) {
    if (gui_gpu_rounded(s,r,radius,4u,c,0)) return;
    if (radius <= 0) { gui_fill(s, r, c); return; }
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius * 2 > r.h) radius = r.h / 2;

    gui_fill(s, rect_make(r.x + radius, r.y, r.w - 2 * radius, r.h), c);
    gui_fill(s, rect_make(r.x, r.y + radius, radius, r.h - 2 * radius), c);
    gui_fill(s, rect_make(r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius), c);

    for (int y = 0; y < radius; y++) {
        for (int x = 0; x < radius; x++) {
            if (!corner_inside(radius - x - 1, radius - y - 1, radius)) continue;
            gui_pixel(s, r.x + x, r.y + y, c);
            gui_pixel(s, r.x + r.w - 1 - x, r.y + y, c);
            gui_pixel(s, r.x + x, r.y + r.h - 1 - y, c);
            gui_pixel(s, r.x + r.w - 1 - x, r.y + r.h - 1 - y, c);
        }
    }
}

void gui_round_frame(surface_t *s, rect_t r, int radius, colour_t c) {
    if (gui_gpu_rounded(s,r,radius,5u,c,0)) return;
    if (radius <= 0) { gui_frame(s, r, c); return; }
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius * 2 > r.h) radius = r.h / 2;

    gui_hline(s, r.x + radius, r.y, r.w - 2 * radius, c);
    gui_hline(s, r.x + radius, r.y + r.h - 1, r.w - 2 * radius, c);
    gui_vline(s, r.x, r.y + radius, r.h - 2 * radius, c);
    gui_vline(s, r.x + r.w - 1, r.y + radius, r.h - 2 * radius, c);

    /* Draw the arc by keeping only the outermost pixel of each row. */
    for (int y = 0; y < radius; y++) {
        int best = -1;
        for (int x = 0; x < radius; x++) {
            if (corner_inside(radius - x - 1, radius - y - 1, radius)) { best = x; break; }
        }
        if (best < 0) continue;
        gui_pixel(s, r.x + best, r.y + y, c);
        gui_pixel(s, r.x + r.w - 1 - best, r.y + y, c);
        gui_pixel(s, r.x + best, r.y + r.h - 1 - y, c);
        gui_pixel(s, r.x + r.w - 1 - best, r.y + r.h - 1 - y, c);
    }
}

void gui_shadow(surface_t *s, rect_t r, int depth) {
    /* A soft edge made of progressively fainter frames; cheap and it reads as
     * depth without needing a blur. */
    for (int i = depth; i >= 1; i--) {
        int alpha = 40 - (i * 30) / (depth + 1);
        if (alpha <= 0) continue;
        rect_t f = rect_make(r.x - i, r.y - i + 2, r.w + 2 * i, r.h + 2 * i);
        if (s->gpu) {
            /* Submit geometry, not one CPU-generated command per edge pixel.
             * Keep both complete vertical edges: the old painter blends each
             * corner twice, with integer rounding at each blend. */
            gui_blend_rect(s, rect_make(f.x, f.y, f.w, 1), g_theme.window_shadow, alpha);
            gui_blend_rect(s, rect_make(f.x, f.y + f.h - 1, f.w, 1), g_theme.window_shadow, alpha);
            gui_blend_rect(s, rect_make(f.x, f.y, 1, f.h), g_theme.window_shadow, alpha);
            gui_blend_rect(s, rect_make(f.x + f.w - 1, f.y, 1, f.h), g_theme.window_shadow, alpha);
            continue;
        }
        rect_t clip = surface_clip(s);
        for (int x = f.x; x < f.x + f.w; x++) {
            if (rect_contains(clip, x, f.y))
                blend_at(s,x,f.y,g_theme.window_shadow,alpha);
            int by = f.y + f.h - 1;
            if (rect_contains(clip, x, by))
                blend_at(s,x,by,g_theme.window_shadow,alpha);
        }
        for (int y = f.y; y < f.y + f.h; y++) {
            if (rect_contains(clip, f.x, y))
                blend_at(s,f.x,y,g_theme.window_shadow,alpha);
            int rx = f.x + f.w - 1;
            if (rect_contains(clip, rx, y))
                blend_at(s,rx,y,g_theme.window_shadow,alpha);
        }
    }
}

void gui_blit_rect(surface_t *dst, const surface_t *src, rect_t src_rect, int x, int y) {
    rect_t sr = rect_intersection(src_rect, rect_make(0, 0, src->width, src->height));
    rect_t dr = rect_intersection(rect_make(x, y, sr.w, sr.h), surface_clip(dst));
    if (rect_empty(dr)) return;

    int skip_x = dr.x - x, skip_y = dr.y - y;
    if (gui_gpu_blit(dst,src,rect_make(sr.x+skip_x,sr.y+skip_y,dr.w,dr.h),dr,255)) return;
    for (int row = 0; row < dr.h; row++) {
        const colour_t *sp = src->pixels + (size_t)(sr.y + skip_y + row) * src->stride + sr.x + skip_x;
        colour_t *d = dst->pixels + (size_t)(dr.y + row) * dst->stride + dr.x;
        blit_copy_row(d, sp, dr.w);
    }
}

void gui_blit(surface_t *dst, const surface_t *src, int x, int y) {
    gui_blit_rect(dst, src, rect_make(0, 0, src->width, src->height), x, y);
}

/* -------------------------------------------------------------------- text */

/* Two kinds of glyph, and the difference is what the interface looks like.
 *
 * The monospaced face is one bit per pixel, which is right for a terminal: at
 * that size a grid-fitted bitmap is sharper than anything anti-aliasing would
 * produce, and every character lands on the same grid.
 *
 * The interface face is not a terminal.  It was one bit per pixel too, and the
 * result was hard-edged letters - every curve a staircase, every diagonal a
 * row of steps.  It reads as a screenshot from a much older machine, and no
 * amount of layout work makes up for it.  So it carries a coverage value per
 * pixel instead: how much of that pixel the letter covers, blended into
 * whatever is behind it.
 *
 * The metrics come from the same generator as the shapes, so measuring and
 * drawing agree - which is the property that keeps text from overlapping.
 */
typedef struct {
    const uint8_t *glyphs;         /* packed bits, or one byte per pixel */
    const uint8_t *widths;
    int cell_w, cell_h, stride, bytes;
    bool coverage;                 /* a byte per pixel rather than a bit */

    /* How much to multiply THIS face by, which is not the interface scale.
     *
     * A face drawn at its own size needs no multiplying; one drawn at twice
     * its size needs two.  Keeping the number on the face rather than reading
     * the interface scale everywhere is what lets a larger face be substituted
     * without every measurement following it upwards. */
    int draw_scale;
} font_desc_t;

static font_desc_t font_of(font_id id) {
    font_desc_t d;
    int es = eff_scale();
    if (id == FONT_MONO) {
        d.glyphs = (const uint8_t *)font_mono;
        d.widths = font_mono_width;
        d.cell_w = FONT_MONO_W;
        d.cell_h = FONT_MONO_H;
        d.stride = FONT_MONO_STRIDE;
        d.bytes  = FONT_MONO_H * FONT_MONO_STRIDE;
        d.coverage = false;
    } else if (es >= 3) {
        /* A face cut at three times the base, so a window that steps up to the
         * largest size gets genuinely larger letters rather than the 2x cut
         * reused - the flaw the old `ui_scale / 2` had at three times. */
        d.glyphs = (const uint8_t *)font_ui3x;
        d.widths = font_ui3x_width;
        d.cell_w = FONT_UI3X_W;
        d.cell_h = FONT_UI3X_H;
        d.stride = FONT_UI3X_W;
        d.bytes  = FONT_UI3X_H * FONT_UI3X_W;
        d.coverage = true;
        d.draw_scale = 1;
        return d;
    } else if (es >= 2) {
        /* A face cut at twice the size, rather than the small one with every
         * pixel drawn as a square of four.
         *
         * Doubling is what this did, and it is wrong in two ways that compound.
         * The obvious one is that the edges become stairs and the coverage
         * values - the whole point of an anti-aliased face - are spread over
         * four pixels that should each have had their own.
         *
         * The one that is harder to see and matters more is SPACING.  Every
         * advance width was `width * 2`, so every letter landed on an even
         * column and the gaps between words could only ever be even numbers.
         * A face measured at its real size has odd widths too, and the
         * proportions between letters come out as the face was drawn rather
         * than rounded to the nearest two pixels.  That is what makes doubled
         * text look subtly badly spaced without any single letter being
         * wrong. */
        d.glyphs = (const uint8_t *)font_ui2x;
        d.widths = font_ui2x_width;
        d.cell_w = FONT_UI2X_W;
        d.cell_h = FONT_UI2X_H;
        d.stride = FONT_UI2X_W;
        d.bytes  = FONT_UI2X_H * FONT_UI2X_W;
        d.coverage = true;
        d.draw_scale = 1;    /* the 2x cut, drawn at its own size (es == 2) */
        return d;
    } else {
        d.glyphs = (const uint8_t *)font_uiaa;
        d.widths = font_uiaa_width;
        d.cell_w = FONT_UIAA_W;
        d.cell_h = FONT_UIAA_H;
        d.stride = FONT_UIAA_W;        /* one byte per pixel, so no packing */
        d.bytes  = FONT_UIAA_H * FONT_UIAA_W;
        d.coverage = true;
    }

    /* Everything that reaches here (the AA face, or the mono console face) is
     * drawn at its own size multiplied by the effective scale, which is what a
     * face with no larger cut available has to do. */
    d.draw_scale = es;
    return d;
}

int gui_font_height(font_id font) {
    font_desc_t f = font_of(font);
    return f.cell_h * f.draw_scale;
}

int gui_font_advance(font_id font, unsigned char ch) {
    font_desc_t f = font_of(font);
    return f.widths[ch] * f.draw_scale;
}

int gui_text_width_n(font_id font, const char *text, size_t n) {
    font_desc_t f = font_of(font);
    int w = 0;
    for (size_t i = 0; i < n && text[i]; i++) w += f.widths[(unsigned char)text[i]];
    return w * f.draw_scale;
}

int gui_text_width(font_id font, const char *text) {
    return text ? gui_text_width_n(font, text, strlen(text)) : 0;
}

static void draw_glyph(surface_t *s, const font_desc_t *f, int x, int y,
                       unsigned char ch, colour_t c) {
    const uint8_t *g = f->glyphs + (size_t)ch * f->bytes;
    rect_t clip = surface_clip(s);

    const int k = f->draw_scale;

    if (s->gpu) {
        gui_gpu_glyph(s,g,f->cell_w,f->cell_h,f->stride,f->coverage,k,x,y,c);
        return;
    }

    for (int row = 0; row < f->cell_h; row++) {
        for (int sy = 0; sy < k; sy++) {
            int py = y + row * k + sy;
            if (py < clip.y || py >= clip.y + clip.h) continue;

            for (int col = 0; col < f->cell_w; col++) {
                if (f->coverage) {
                    /* How much of this pixel the letter covers.  Fully covered
                     * is written straight in; anything between is mixed with
                     * what is already there, which is the whole point. */
                    int a = g[row * f->stride + col];
                    if (!a) continue;

                    for (int sx = 0; sx < k; sx++) {
                        int px = x + col * k + sx;
                        if (px < clip.x || px >= clip.x + clip.w) continue;
                         blend_at(s,px,py,c,a>=254?255:a);
                    }
                } else {
                    const uint8_t byte = g[row * f->stride + (col >> 3)];
                    if (!(byte & (0x80 >> (col & 7)))) continue;

                    for (int sx = 0; sx < k; sx++) {
                        int px = x + col * k + sx;
                        if (px < clip.x || px >= clip.x + clip.w) continue;
                         gui_pixel(s,px,py,c);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------- words on top of words
 *
 * Three separate places in this system once drew text over other text, and
 * each was found by looking at a screenshot and noticing.  That does not
 * scale: a fourth would be found the same way or not at all, and "not at all"
 * is what had been happening.
 *
 * Every piece of text in this system is drawn through the function below, so
 * this is the one place that can see all of it.  Each run's box is remembered,
 * and a run that lands on a box already taken says so.
 *
 * What is deliberately NOT reported:
 *
 *   The same string twice.  Drawing a word, then drawing it again a pixel
 *   across in another colour, is how a shadow or a highlight is made.  It
 *   overlaps on purpose and always will.
 *
 *   Runs into different surfaces.  Every window paints into its own canvas,
 *   and two windows' text overlapping there means nothing - what decides
 *   whether they overlap on screen is where the windows are.  The list is
 *   dropped whenever the surface being drawn into changes, which also keeps it
 *   to one window's worth at a time.
 *
 * It reports once per pair and stops after a handful, because the second
 * hundred lines of this say nothing the first ten did not.
 */
/* 48 was not enough to see a window.
 *
 * A Settings page at 2560x1440 draws well over a hundred separate runs, so the
 * tracker filled up a third of the way down and everything below it was
 * unchecked - which is exactly where a layout that ran out of width would go
 * wrong.  The array is a few kilobytes; the blind spot was the whole point of
 * the exercise. */
#define TEXT_RUNS_TRACKED  256
#define OVERLAPS_REPORTED  40

typedef struct {
    int x, y, w, h;
    char text[24];
} text_run_t;

static text_run_t runs[TEXT_RUNS_TRACKED];
static int runs_used;
static const surface_t *runs_surface;
static int overlaps_said;

/* Do these two runs really sit on top of one another?
 *
 * Touching is not overlapping.  A font cell is taller than the ink in it -
 * there is room above for accents and below for descenders - so two lines of a
 * wrapped label, set closer together than the cell height because that is what
 * looks right, have boxes that meet and glyphs that never come near each other.
 * The first run of this check reported four such pairs and no real ones: both
 * halves of "Event Viewer" and of "Task Manager" under the desktop icons.
 *
 * So the test is not whether the boxes meet but whether they meet BY ENOUGH:
 * more than a third of the shorter one's height, and some real width.  Words
 * genuinely drawn over each other overlap almost entirely; two lines of a label
 * overlap by the few pixels of padding the cell carries. */
static bool boxes_meet(const text_run_t *a, int x, int y, int w, int h) {
    int left = a->x > x ? a->x : x;
    int right = (a->x + a->w) < (x + w) ? (a->x + a->w) : (x + w);
    if (right - left <= 1) return false;

    int top = a->y > y ? a->y : y;
    int bottom = (a->y + a->h) < (y + h) ? (a->y + a->h) : (y + h);
    int shared = bottom - top;
    if (shared <= 0) return false;

    /* Most of BOTH, not a little of one.
     *
     * Every threshold below this reported things that were not wrong: two
     * lines of a wrapped label, a menu correctly drawn over the desktop, a
     * window's title over an icon beneath it.  Each was a different kind of
     * near-miss and each needed its own answer, and what is left after all of
     * them is that a genuine overlay - the thing that was actually reported
     * three times in this system - is words sitting almost exactly on top of
     * other words.  Anything less is two things next to each other.
     *
     * Set deliberately so this is SILENT on a correct interface.  A check that
     * cries wolf on every menu gets ignored, and then it is worth nothing when
     * it is finally right. */
    int overlap_w = right - left;
    int narrower = a->w < w ? a->w : w;
    int shorter = a->h < h ? a->h : h;

    return shared * 10 > shorter * 7 && overlap_w * 10 > narrower * 7;
}

/* Two runs on the same line with almost no gap between them.
 *
 * This is a different fault from the one above and needs its own test.
 * `boxes_meet` asks whether two pieces of text are drawn ON one another, which
 * is what a column at a fixed offset does when the text beside it has outgrown
 * its place.  But long before that happens the interface is already wrong: a
 * label whose value begins one pixel after it ends reads as a single run-on
 * word.  That is what "the words are smushed together" describes, and nothing
 * was looking for it.
 *
 * Deliberately narrow: the two must sit on the SAME LINE - most of their
 * heights shared - and be separated by less than a quarter of the space a
 * character occupies.  Text that merely sits close is left alone; text with no
 * gap at all is not.  Unlike the overlap test this cannot fire on a menu drawn
 * over the desktop, because those overlap completely rather than nearly touch.
 */
static bool boxes_crowd(const text_run_t *a, int x, int y, int w, int h,
                        int cell_h, size_t alen, size_t blen) {
    /* A terminal is not crowded text; it is a grid.
     *
     * The first run of this check reported forty faults and every one was the
     * Terminal drawing "K", "e", "s", "t", "r", "e", "l" one glyph at a time,
     * sixteen pixels apart, exactly as a monospace cell grid must.  Single
     * characters set side by side are the intended layout there, and nothing
     * about them is smushed.
     *
     * What this is for is two pieces of TEXT - a label and its value, a column
     * and the next - with no gap between them.  So both sides have to be words
     * rather than letters. */
    if (alen < 3 || blen < 3) return false;

    /* An ellipsis is supposed to touch what it truncates.
     *
     * gui_text_clipped draws the text that fits and then "..." immediately
     * after it, and those are two runs with no gap by design - that is what
     * makes it read as one shortened phrase rather than a phrase and a stray
     * mark.  Reporting it drowned every real fault: a sweep of ten
     * applications returned hundreds of these and the two genuine faults were
     * somewhere in the middle. */
    if (!strcmp(a->text, "...")) return false;
    int top = a->y > y ? a->y : y;
    int bottom = (a->y + a->h) < (y + h) ? (a->y + a->h) : (y + h);
    int shared = bottom - top;
    int shorter = a->h < h ? a->h : h;
    if (shared * 10 <= shorter * 7) return false;      /* not the same line */

    /* The gap, whichever way round they are. */
    int gap;
    if (a->x >= x + w)      gap = a->x - (x + w);
    else if (x >= a->x + a->w) gap = x - (a->x + a->w);
    else return false;                                 /* they overlap; not this */

    int least = cell_h / 4;
    if (least < 2) least = 2;
    return gap < least;
}

static void note_text_run(const surface_t *s, int x, int y, int w, int h,
                          const char *text, size_t n) {
    if (w <= 0 || h <= 0) return;
    /* The renderer clips glyphs. Track only their visible box, otherwise a
     * scrolled/clipped page falsely overlaps a separately clipped footer. */
    rect_t visible = rect_intersection(surface_clip(s), rect_make(x,y,w,h));
    if (visible.w <= 0 || visible.h <= 0) return;
    x=visible.x; y=visible.y; w=visible.w; h=visible.h;

    /* A new surface is a new window, and it gets its own budget.
     *
     * The cap used to be for the life of the process, so whichever window drew
     * first spent it and every window after that was silently unchecked.  A
     * sweep across ten applications reported forty faults in the first and
     * zero in the other nine, which read as nine clean applications and was
     * nothing of the sort. */
    if (s != runs_surface) {
        runs_surface = s;
        runs_used = 0;
        overlaps_said = 0;
    }

    char label[24];
    size_t take = n < sizeof label - 1 ? n : sizeof label - 1;
    memcpy(label, text, take);
    label[take] = '\0';

    for (int i = 0; i < runs_used; i++) {
        if (strcmp(runs[i].text, label) == 0) continue;   /* a shadow, not a bug */

        const char *how = NULL;
        if (boxes_meet(&runs[i], x, y, w, h))
            how = "overlaps";
        else if (strcmp(label, "...") &&
                 boxes_crowd(&runs[i], x, y, w, h, h,
                             strlen(runs[i].text), take))
            how = "crowds";
        else
            continue;

        if (overlaps_said < OVERLAPS_REPORTED) {
            overlaps_said++;
            char line[160];
            snprintf(line, sizeof line,
                     "text %s text: \"%s\" at %d,%d lands on \"%s\" at "
                     "%d,%d", how, label, x, y, runs[i].text,
                     runs[i].x, runs[i].y);
            log_write(2, "gui", line);
        }
        break;
    }

    if (runs_used < TEXT_RUNS_TRACKED) {
        runs[runs_used].x = x; runs[runs_used].y = y;
        runs[runs_used].w = w; runs[runs_used].h = h;
        memcpy(runs[runs_used].text, label, take + 1);
        runs_used++;
    }
}

/* Something opaque has been painted; anything under it is no longer on the
 * surface and must not count as being overlapped.
 *
 * Without this the check reports every menu in the system.  A menu paints its
 * own panel and then its items, and the desktop's icon labels were painted
 * into the same surface a moment earlier - so the boxes coincide exactly, and
 * the only thing separating "a menu, correctly drawn over the desktop" from
 * "two labels on top of each other" is the fill in between, which the text
 * path cannot see on its own.  So the fill tells it. */
static void text_runs_covered(const surface_t *s, rect_t r) {
    /* Only what is on the same surface.
     *
     * Rectangles here are in each surface's own coordinates, so a window
     * clearing its canvas at (0,0) and the desktop drawing at (0,0) describe
     * the same numbers and completely different places.  Comparing them
     * without knowing which surface each belongs to buries runs that are still
     * on screen, and leaves buried ones that are not - which is how a check
     * for words on top of words ends up reporting a clock that redraws itself
     * correctly every second. */
    if (s != runs_surface) return;

    int kept = 0;
    for (int i = 0; i < runs_used; i++) {
        bool buried = runs[i].x >= r.x && runs[i].y >= r.y &&
                      runs[i].x + runs[i].w <= r.x + r.w &&
                      runs[i].y + runs[i].h <= r.y + r.h;
        if (!buried) runs[kept++] = runs[i];
    }
    runs_used = kept;
}

/* A new frame: nothing drawn in the last one is on the surface any more.
 *
 * Resetting only when the SURFACE changed was not enough, and the way it
 * failed was instructive: the screen surface never changes, so runs piled up
 * frame after frame and the check started reporting one frame's menu against
 * the next frame's desktop icons - complete with the order reversed, which is
 * what gave it away.  The compositor paints the background before any overlay,
 * so an icon can never land on a menu within one frame. */
void gui_text_frame_begin(void) {
    runs_used = 0;
    runs_surface = NULL;
}

/* How many were found, for a check to ask about. */
int gui_text_overlaps(void) { return overlaps_said; }

void gui_text_n(surface_t *s, font_id font, int x, int y, const char *text, size_t n, colour_t c) {
    if (!text) return;
    font_desc_t f = font_of(font);

    int began = x;
    for (size_t i = 0; i < n && text[i]; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch != ' ') draw_glyph(s, &f, x, y, ch, c);
        x += f.widths[ch] * f.draw_scale;
    }

    note_text_run(s, began, y, x - began, f.cell_h * f.draw_scale, text, n);
}

void gui_text(surface_t *s, font_id font, int x, int y, const char *text, colour_t c) {
    if (text) gui_text_n(s, font, x, y, text, strlen(text), c);
}

void gui_text_centred(surface_t *s, font_id font, rect_t r, const char *text, colour_t c) {
    if (!text || r.w <= 0 || r.h <= 0) return;
    int w = gui_text_width(font, text);
    int h = gui_font_height(font);
    rect_t saved = surface_clip(s);
    surface_set_clip(s,rect_intersection(saved,r));
    int offset = w < r.w ? (r.w - w) / 2 : 0;
    gui_text_clipped(s, font, r.x + offset, r.y + (r.h - h) / 2, r.w-offset, text, c);
    surface_set_clip(s,saved);
}

void gui_text_right(surface_t *s, font_id font, rect_t r, const char *text, colour_t c) {
    if (!text || r.w <= 0 || r.h <= 0) return;
    int w = gui_text_width(font, text);
    int h = gui_font_height(font);
    rect_t saved = surface_clip(s);
    surface_set_clip(s,rect_intersection(saved,r));
    int offset = w < r.w ? r.w - w : 0;
    gui_text_clipped(s, font, r.x + offset, r.y + (r.h - h) / 2, r.w-offset, text, c);
    surface_set_clip(s,saved);
}

void gui_text_clipped(surface_t *s, font_id font, int x, int y, int max_w, const char *text, colour_t c) {
    if (!text || max_w <= 0) return;
    rect_t saved = surface_clip(s);
    surface_set_clip(s,rect_intersection(saved,rect_make(x,y,max_w,gui_font_height(font))));
    if (gui_text_width(font, text) <= max_w) {
        gui_text(s, font, x, y, text, c);
        surface_set_clip(s,saved);
        return;
    }
    font_desc_t f = font_of(font);
    int dot = f.widths[(unsigned char)'.'] * f.draw_scale;
    if (dot <= 0) { surface_set_clip(s,saved); return; }
    int dots = max_w / dot;
    if (dots > 3) dots = 3;
    int ellipsis = dot * dots;
    int used = 0;
    size_t n = 0;
    while (dots == 3 && text[n]) {
        int adv = f.widths[(unsigned char)text[n]] * f.draw_scale;
        if (adv > max_w - used - ellipsis) break;
        used += adv;
        n++;
    }
    gui_text_n(s, font, x, y, text, n, c);
    gui_text_n(s, font, x + used, y, "...", (size_t)dots, c);
    surface_set_clip(s,saved);
}

/* ---------------------------------------------------------- image app-icons
 *
 * The desktop decodes the app-icon PNGs (see genicons.py / user/desktop) and
 * registers each as an ARGB bitmap here.  gui_app_icon() draws them - real
 * images, scaled with bilinear filtering and per-pixel alpha so the rounded
 * corners sit cleanly on any wallpaper - and falls back to the drawn glyph
 * when no image was registered.  Kept separate from gui_icon() so the small
 * in-app glyphs (a monitor in the Display page, a chip in a device list) stay
 * as line-art while the desktop/taskbar/title-bar show the polished tiles. */
#define APP_ICON_SLOTS 48
typedef struct { colour_t *argb; int w, h; surface_t *gpu_image; } app_icon_bitmap_t;
static app_icon_bitmap_t app_icons[APP_ICON_SLOTS];

/* Tintable single-channel masks (the OS logo): drawn in whatever colour the
 * caller of gui_icon passes, so the brand mark follows the accent on the
 * taskbar and start menu.  Registered by the desktop from logo_mask.png. */
typedef struct { uint8_t *a; int w, h; surface_t *gpu_image; } icon_mask_t;
static icon_mask_t icon_masks[APP_ICON_SLOTS];

void gui_register_icon_mask(icon_id id, const uint8_t *alpha, int w, int h) {
    if ((unsigned)id >= APP_ICON_SLOTS || !alpha || w <= 0 || h <= 0) return;
    uint8_t *copy = malloc((size_t)w * h);
    if (!copy) return;
    memcpy(copy, alpha, (size_t)w * h);
    surface_destroy(icon_masks[id].gpu_image);icon_masks[id].gpu_image=NULL;
    if (icon_masks[id].a) free(icon_masks[id].a);
    icon_masks[id].a = copy; icon_masks[id].w = w; icon_masks[id].h = h;
}

/* Draw a registered mask scaled into (x,y,size) tinted with colour c. */
static void draw_icon_mask(surface_t *s, icon_mask_t *m, int x, int y,
                           int size, colour_t c) {
    if(gui_gpu_icon(s,&m->gpu_image,m->a,m->w,m->h,true,x,y,size,true,c))return;
    int iw = m->w, ih = m->h;
    for (int dy = 0; dy < size; dy++) {
        int py = y + dy; if (py < 0 || py >= s->height) continue;
        int syq = (dy * (ih - 1) * 256) / (size > 1 ? size - 1 : 1);
        int sy = syq >> 8, fy = syq & 0xFF; if (sy >= ih - 1) { sy = ih - 1; fy = 0; }
        for (int dx = 0; dx < size; dx++) {
            int px = x + dx; if (px < 0 || px >= s->width) continue;
            int sxq = (dx * (iw - 1) * 256) / (size > 1 ? size - 1 : 1);
            int sx = sxq >> 8, fx = sxq & 0xFF; if (sx >= iw - 1) { sx = iw - 1; fx = 0; }
            const uint8_t *r0 = &m->a[(size_t)sy * iw + sx];
            const uint8_t *r1 = &m->a[(size_t)(sy + (fy ? 1 : 0)) * iw + sx];
            int a00 = r0[0], a10 = r0[fx ? 1 : 0], a01 = r1[0], a11 = r1[fx ? 1 : 0];
            int top = a00 + ((a10 - a00) * fx >> 8);
            int bot = a01 + ((a11 - a01) * fx >> 8);
            int a = top + ((bot - top) * fy >> 8);
            if (a <= 0) continue;
            blend_at(s,px,py,c,a);
        }
    }
}

void gui_register_app_icon(icon_id id, const colour_t *argb, int w, int h) {
    if ((unsigned)id >= APP_ICON_SLOTS || !argb || w <= 0 || h <= 0) return;
    colour_t *copy = malloc((size_t)w * h * sizeof(colour_t));
    if (!copy) return;
    memcpy(copy, argb, (size_t)w * h * sizeof(colour_t));
    surface_destroy(app_icons[id].gpu_image);app_icons[id].gpu_image=NULL;
    if (app_icons[id].argb) free(app_icons[id].argb);
    app_icons[id].argb = copy; app_icons[id].w = w; app_icons[id].h = h;
}

bool gui_has_app_icon(icon_id id) {
    return (unsigned)id < APP_ICON_SLOTS && app_icons[id].argb != NULL;
}

/* Interpolate two ARGB pixels; t is 0..256 towards b. */
static colour_t lerp_argb(colour_t a, colour_t b, int t) {
    int ta = 256 - t;
    int A = (((a>>24)&0xFF)*ta + ((b>>24)&0xFF)*t) >> 8;
    int R = (((a>>16)&0xFF)*ta + ((b>>16)&0xFF)*t) >> 8;
    int G = (((a>>8) &0xFF)*ta + ((b>>8) &0xFF)*t) >> 8;
    int B = (((a)    &0xFF)*ta + ((b)    &0xFF)*t) >> 8;
    return ((colour_t)A<<24)|((colour_t)R<<16)|((colour_t)G<<8)|(colour_t)B;
}

/* The shared bilinear blit behind both public entry points.  When `tinted` is
 * set the image supplies only coverage (its alpha) and every pixel is painted
 * in `tint`: that is how a glyph stored as an image still follows the theme and
 * lights up white on a coloured hover, instead of being frozen to the colour it
 * was drawn at.  Untinted, it paints the image's own colours (the app tiles). */
static void app_icon_blit(surface_t *s, icon_id icon, int x, int y, int size,
                          bool tinted, colour_t tint) {
    app_icon_bitmap_t *ic = &app_icons[icon];
    if(gui_gpu_icon(s,&ic->gpu_image,ic->argb,ic->w,ic->h,false,x,y,size,tinted,tint))return;
    int iw = ic->w, ih = ic->h;
    for (int dy = 0; dy < size; dy++) {
        int py = y + dy;
        if (py < 0 || py >= s->height) continue;
        int syq = (dy * (ih - 1) * 256) / (size > 1 ? size - 1 : 1);
        int sy = syq >> 8, fy = syq & 0xFF;
        if (sy >= ih - 1) { sy = ih - 1; fy = 0; }
        for (int dx = 0; dx < size; dx++) {
            int px = x + dx;
            if (px < 0 || px >= s->width) continue;
            int sxq = (dx * (iw - 1) * 256) / (size > 1 ? size - 1 : 1);
            int sx = sxq >> 8, fx = sxq & 0xFF;
            if (sx >= iw - 1) { sx = iw - 1; fx = 0; }
            const colour_t *r0 = &ic->argb[(size_t)sy * iw + sx];
            const colour_t *r1 = &ic->argb[(size_t)(sy + (fy ? 1 : 0)) * iw + sx];
            colour_t top = lerp_argb(r0[0], r0[fx ? 1 : 0], fx);
            colour_t bot = lerp_argb(r1[0], r1[fx ? 1 : 0], fx);
            colour_t sc  = lerp_argb(top, bot, fy);
            int a = (sc >> 24) & 0xFF;
            if (a <= 0) continue;
            colour_t rgb = tinted ? (tint & 0x00FFFFFF) : (sc & 0x00FFFFFF);
            blend_at(s,px,py,rgb,a);
        }
    }
}

void gui_app_icon(surface_t *s, icon_id icon, int x, int y, int size) {
    if (!gui_has_app_icon(icon) || size <= 0) {
        gui_icon(s, icon, x, y, size, RGB(0xE6, 0xEC, 0xF6));
        return;
    }
    app_icon_blit(s, icon, x, y, size, false, 0);
}

/* Draw a registered glyph image in a chosen colour.  Falls back to the drawn
 * gui_icon glyph when no image is registered, so callers get an image where one
 * exists and never a blank where one does not. */
void gui_app_icon_tinted(surface_t *s, icon_id icon, int x, int y, int size,
                         colour_t tint) {
    if (!gui_has_app_icon(icon) || size <= 0) {
        gui_icon(s, icon, x, y, size, tint);
        return;
    }
    app_icon_blit(s, icon, x, y, size, true, tint);
}

/* ------------------------------------------------------------------- icons */

/* Icons are drawn from primitives rather than stored as bitmaps: they scale to
 * whatever size the caller asks for and follow the theme colour. */
void gui_icon(surface_t *s, icon_id icon, int x, int y, int size, colour_t c) {
    /* A registered tint mask (the OS logo) wins over the drawn glyph. */
    if ((unsigned)icon < APP_ICON_SLOTS && icon_masks[icon].a && size > 0) {
        draw_icon_mask(s, &icon_masks[icon], x, y, size, c);
        return;
    }
    int q = size / 4;
    if (q < 1) q = 1;
    rect_t box = rect_make(x, y, size, size);

    switch (icon) {
    case ICON_TERMINAL: {
        gui_round_frame(s, box, 2, c);
        /* a prompt caret and a line */
        gui_line_aa(s, x + q, y + q + 1, x + q + q / 2 + 1, y + size / 2, c);
        gui_line_aa(s, x + q + q / 2 + 1, y + size / 2, x + q, y + size - q - 1, c);
        gui_hline(s, x + size / 2 + 1, y + size - q - 1, size / 2 - q - 1, c);
        break;
    }
    case ICON_FOLDER: {
        gui_fill(s, rect_make(x, y + q, size, size - q - 1), c);
        gui_fill(s, rect_make(x, y + q / 2, size / 2, q), c);
        gui_fill(s, rect_make(x + 1, y + q + 2, size - 2, size - q - 4), colour_shade(c, -35));
        break;
    }
    case ICON_FILE: {
        int fold = size / 3;
        gui_fill(s, rect_make(x + q / 2, y, size - q, size), c);
        gui_fill(s, rect_make(x + size - q - fold, y, fold, fold), colour_shade(c, -40));
        for (int i = 0; i < 3; i++)
            gui_hline(s, x + q, y + size / 2 + i * (size / 8), size - 2 * q, colour_shade(c, -50));
        break;
    }
    case ICON_DISK: {
        gui_round_rect(s, box, 3, c);
        gui_fill(s, rect_make(x + q, y + q / 2, size - 2 * q, size / 3), colour_shade(c, -45));
        gui_fill(s, rect_make(x + q + 1, y + size - size / 3, size - 2 * q - 2, size / 4), colour_shade(c, -25));
        break;
    }
    case ICON_LOG: {
        gui_frame(s, box, c);
        for (int i = 1; i <= 3; i++) {
            gui_hline(s, x + q / 2, y + i * size / 4, size - q, c);
        }
        break;
    }
    case ICON_DISPLAY: {
        /* A monitor: a screen on a stand. */
        int h = size - 2 * q - 2;
        gui_round_frame(s, rect_make(x + q, y + q, size - 2 * q, h), 2, c);
        gui_fill(s, rect_make(x + size / 2 - 1, y + q + h, 2, 4), c);
        gui_fill(s, rect_make(x + q + 2, y + size - q - 1, size - 2 * q - 4, 2), c);
        break;
    }
    case ICON_WIFI: {
        /* Three arcs and a dot, which is what a signal looks like everywhere. */
        int cx = x + size / 2, cy = y + size - q;
        for (int ring = 1; ring <= 3; ring++) {
            int radius = ring * size / 5;
            /* The top half of a circle, drawn from the distance test so the
             * arc stays even at every size. */
            for (int dx = -radius; dx <= radius; dx++) {
                int best = -1;
                for (int dy = 0; dy <= radius; dy++) {
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= radius * radius && d2 >= (radius - 1) * (radius - 1))
                        best = dy;
                }
                if (best >= 0) gui_pixel(s, cx + dx, cy - best, c);
            }
        }
        gui_round_rect_aa(s, rect_make(cx - 1, cy - 1, 3, 3), 1, c);
        break;
    }
    case ICON_SOUND: {
        /* A speaker and two waves. */
        int cy = y + size / 2;
        gui_fill(s, rect_make(x + q / 2, cy - q / 2, q, q), c);
        for (int i = 0; i < q + 1; i++)
            gui_vline(s, x + q / 2 + q + i, cy - i - 1, (i + 1) * 2 + 2, c);
        for (int wave = 1; wave <= 2; wave++) {
            int radius = wave * size / 5;
            int wx = x + size / 2 + q / 2;
            for (int dy = -radius; dy <= radius; dy++) {
                int dx2 = radius * radius - dy * dy;
                if (dx2 < 0) continue;
                int dx = 0;
                while ((dx + 1) * (dx + 1) <= dx2) dx++;
                if (dx > radius / 2) gui_pixel(s, wx + dx, cy + dy, c);
            }
        }
        break;
    }
    case ICON_PALETTE: {
        /* A rounded blob with three spots on it. */
        gui_round_rect_aa(s, rect_make(x + 1, y + 1, size - 2, size - 2), size / 3, c);
        colour_t hole = colour_shade(c, -60);
        gui_round_rect_aa(s, rect_make(x + q, y + q, q, q), q / 2, hole);
        gui_round_rect_aa(s, rect_make(x + size - 2 * q, y + q, q, q), q / 2, hole);
        gui_round_rect_aa(s, rect_make(x + size / 2 - q / 2, y + size - 2 * q, q, q),
                          q / 2, hole);
        break;
    }
    case ICON_CHIP: {
        /* A square with legs, which is what a processor looks like. */
        rect_t body = rect_make(x + q, y + q, size - 2 * q, size - 2 * q);
        gui_round_frame_aa(s, body, 2, c);
        gui_round_rect_aa(s, rect_make(body.x + 3, body.y + 3, body.w - 6, body.h - 6),
                          1, colour_shade(c, -50));
        for (int i = 0; i < 3; i++) {
            int at = q + 2 + i * ((size - 2 * q - 4) / 2);
            gui_hline(s, x, y + at, q, c);
            gui_hline(s, x + size - q, y + at, q, c);
            gui_vline(s, x + at, y, q, c);
            gui_vline(s, x + at, y + size - q, q, c);
        }
        break;
    }
    case ICON_NETWORK: {
        /* Two boxes joined by a line: what a wired connection is. */
        gui_round_frame_aa(s, rect_make(x, y + q / 2, size / 2 - 1, size / 2 - 1), 2, c);
        gui_round_frame_aa(s, rect_make(x + size / 2 + 1, y + size / 2,
                                        size / 2 - 1, size / 2 - 1), 2, c);
        gui_line_aa(s, x + size / 2 - 3, y + size / 2 - 2,
                 x + size / 2 + 3, y + size / 2 + 2, c);
        break;
    }
    case ICON_SEARCH: {
        /* A ring and a handle. */
        int cx = x + size / 2 - 1, cy = y + size / 2 - 1;
        int radius = size / 3;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dy = -radius; dy <= radius; dy++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= radius * radius && d2 >= (radius - 1) * (radius - 1))
                    gui_pixel(s, cx + dx, cy + dy, c);
            }
        gui_line_aa(s, cx + radius - 1, cy + radius - 1, x + size - 1, y + size - 1, c);
        break;
    }
    case ICON_BROWSER: {
        /* A globe: a circle with a meridian and two parallels. */
        int cx = x + size / 2, cy = y + size / 2;
        int radius = size / 2 - 1;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dy = -radius; dy <= radius; dy++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= radius * radius && d2 >= (radius - 1) * (radius - 1))
                    gui_pixel(s, cx + dx, cy + dy, c);
            }
        gui_vline(s, cx, cy - radius, radius * 2, c);
        gui_hline(s, cx - radius, cy, radius * 2, c);
        /* The meridian, narrowed towards the poles. */
        for (int dy = -radius; dy <= radius; dy++) {
            int w = (radius - (dy < 0 ? -dy : dy)) / 2;
            if (w > 0) { gui_pixel(s, cx - w, cy + dy, c); gui_pixel(s, cx + w, cy + dy, c); }
        }
        break;
    }
    case ICON_STORE: {
        /* An awning over a doorway. */
        gui_fill(s, rect_make(x, y + q, size, q / 2 + 1), c);
        gui_round_frame_aa(s, rect_make(x + 1, y + q + q / 2, size - 2, size - q - q / 2 - 1),
                           2, c);
        gui_fill(s, rect_make(x + size / 2 - q / 2, y + size - q - 1, q, q + 1), c);
        break;
    }
    case ICON_LOCK: {
        /* A padlock: the shackle above, the body below. */
        int bw = size - 2 * (q / 2) - 2;
        rect_t body = rect_make(x + (size - bw) / 2, y + size / 2 - 1, bw, size / 2);
        gui_round_rect_aa(s, body, 2, c);
        int radius = bw / 2 - 1;
        int cx = x + size / 2, cy = y + size / 2 - 1;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dy = -radius; dy <= 0; dy++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= radius * radius && d2 >= (radius - 2) * (radius - 2))
                    gui_pixel(s, cx + dx, cy + dy, c);
            }
        break;
    }
    case ICON_RELOAD: {
        /* An arc with an arrowhead, which reads as "again". */
        int cx = x + size / 2, cy = y + size / 2;
        int radius = size / 2 - 2;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dy = -radius; dy <= radius; dy++) {
                if (dx > 0 && dy < 0) continue;         /* the gap */
                int d2 = dx * dx + dy * dy;
                if (d2 <= radius * radius && d2 >= (radius - 1) * (radius - 1))
                    gui_pixel(s, cx + dx, cy + dy, c);
            }
        for (int i = 0; i < q; i++) {
            gui_hline(s, cx, cy - radius - 1 + i, q - i, c);
        }
        break;
    }
    case ICON_HOME: {
        /* A roof over a box. */
        for (int i = 0; i < size / 2; i++)
            gui_hline(s, x + size / 2 - i, y + i, i * 2 + 1, c);
        gui_round_frame_aa(s, rect_make(x + q / 2, y + size / 2, size - q, size / 2 - 1),
                           1, c);
        break;
    }
    case ICON_USB: {
        /* The trident, near enough at this size. */
        int cx = x + size / 2;
        gui_line_aa(s, cx, y + q, cx, y + size - q, c);
        gui_fill(s, rect_make(cx - 3, y + q, 6, 6), c);
        gui_line_aa(s, cx, y + size / 2, cx - size / 4, y + size / 3, c);
        gui_line_aa(s, cx, y + size / 2 + 3, cx + size / 4, y + size / 3 + 3, c);
        gui_fill(s, rect_make(cx - size / 4 - 2, y + size / 3 - 2, 4, 4), c);
        gui_fill(s, rect_make(cx + size / 4 - 2, y + size / 3 + 1, 4, 4), c);
        break;
    }
    case ICON_INFO: {
        gui_round_frame(s, box, size / 2, c);
        gui_fill(s, rect_make(x + size / 2 - 1, y + q, 2, 2), c);
        gui_fill(s, rect_make(x + size / 2 - 1, y + q + 4, 2, size - 2 * q - 2), c);
        break;
    }
    case ICON_SETTINGS: {
        gui_round_frame(s, rect_make(x + q, y + q, size - 2 * q, size - 2 * q), (size - 2 * q) / 2, c);
        for (int i = 0; i < 4; i++) {
            int cx = x + size / 2, cy = y + size / 2;
            int dx = (i == 0) ? 0 : (i == 1) ? 0 : (i == 2) ? -size / 2 : size / 2;
            int dy = (i == 0) ? -size / 2 : (i == 1) ? size / 2 : 0;
            gui_line_aa(s, cx, cy, cx + dx, cy + dy, c);
        }
        break;
    }
    case ICON_EDITOR: {
        gui_fill(s, rect_make(x + q / 2, y, size - q, size), colour_shade(c, -20));
        for (int i = 1; i <= 3; i++)
            gui_hline(s, x + q, y + i * size / 5, size - 2 * q - (i == 3 ? q : 0), c);
        break;
    }
    case ICON_INSTALL: {
        gui_line_aa(s, x + size / 2, y, x + size / 2, y + size - q, c);
        gui_line_aa(s, x + q, y + size / 2, x + size / 2, y + size - q, c);
        gui_line_aa(s, x + size - q, y + size / 2, x + size / 2, y + size - q, c);
        gui_hline(s, x, y + size - 1, size, c);
        break;
    }
    case ICON_POWER: {
        gui_round_frame(s, rect_make(x + 1, y + q, size - 2, size - q - 1), (size - 2) / 2, c);
        gui_fill(s, rect_make(x + size / 2 - 1, y, 2, size / 2), c);
        break;
    }
    case ICON_CLOSE: {
        gui_line_aa(s, x + q, y + q, x + size - q - 1, y + size - q - 1, c);
        gui_line_aa(s, x + size - q - 1, y + q, x + q, y + size - q - 1, c);
        break;
    }
    case ICON_MINIMISE:
        gui_fill(s, rect_make(x + q, y + size - q - 2, size - 2 * q, 2), c);
        break;
    case ICON_MAXIMISE:
        gui_frame(s, rect_make(x + q, y + q, size - 2 * q, size - 2 * q), c);
        gui_hline(s, x + q, y + q + 1, size - 2 * q, c);
        break;
    case ICON_RESTORE:
        gui_frame(s, rect_make(x + q, y + q + 2, size - 2 * q - 2, size - 2 * q - 2), c);
        gui_hline(s, x + q + 2, y + q, size - 2 * q - 2, c);
        gui_vline(s, x + size - q - 1, y + q, size - 2 * q - 2, c);
        break;
    case ICON_ARROW_UP:
        for (int i = 0; i < size / 2; i++)
            gui_hline(s, x + size / 2 - i, y + size / 4 + i, i * 2 + 1, c);
        break;
    case ICON_ARROW_DOWN:
        for (int i = 0; i < size / 2; i++)
            gui_hline(s, x + size / 2 - i, y + size - size / 4 - i, i * 2 + 1, c);
        break;
    case ICON_ARROW_LEFT:
        for (int i = 0; i < size / 2; i++)
            gui_vline(s, x + size / 4 + i, y + size / 2 - i, i * 2 + 1, c);
        break;
    case ICON_ARROW_RIGHT:
        for (int i = 0; i < size / 2; i++)
            gui_vline(s, x + size - size / 4 - i, y + size / 2 - i, i * 2 + 1, c);
        break;
    case ICON_CHECK:
        gui_line_aa(s, x + q, y + size / 2, x + size / 2 - 1, y + size - q - 1, c);
        gui_line_aa(s, x + size / 2 - 1, y + size - q - 1, x + size - q, y + q, c);
        gui_line_aa(s, x + q, y + size / 2 + 1, x + size / 2 - 1, y + size - q, c);
        gui_line_aa(s, x + size / 2 - 1, y + size - q, x + size - q, y + q + 1, c);
        break;
    case ICON_WARNING: {
        for (int i = 0; i < size; i++)
            gui_hline(s, x + size / 2 - i / 2, y + i, i + 1, i < size - 2 ? c : c);
        gui_fill(s, rect_make(x + size / 2, y + size / 3, 1, size / 3), colour_shade(c, -70));
        gui_fill(s, rect_make(x + size / 2, y + size - size / 5, 1, 2), colour_shade(c, -70));
        break;
    }
    case ICON_KESTREL: {
        /* A stylised bird: two swept wings meeting at a point. */
        for (int i = 0; i < size / 2; i++) {
            gui_line_aa(s, x + size / 2, y + size - q, x + i, y + q + i / 2, c);
        }
        break;
    }
    default:
        gui_frame(s, box, c);
        break;
    }
}

/* ------------------------------------------------------------------ cursor */

/* The classic arrow, as a small bitmap: 0 transparent, 1 outline, 2 fill. */
static const uint8_t cursor_bits[19][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,1,2,2,1,0,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

/* ------------------------------------------------------------- cursor shapes
 *
 * More than one pointer now: the arrow, the four resize shapes the window
 * edges want, a text I-beam and a hand.  The window manager calls
 * gui_set_cursor() from its hover test (a horizontal edge -> the left-right
 * arrows, a corner -> a diagonal, and so on) and the same shape is used whether
 * the display draws the pointer itself or this code draws it into the frame.
 *
 * The non-arrow shapes are rasterised from a filled silhouette with the outline
 * derived automatically - any transparent cell touching a filled one becomes
 * the dark border - so they stay crisp without a hand-drawn mask per shape. */
#define CUR_MAX 28

static cursor_kind g_cursor = CURSOR_ARROW;
static bool        g_cursor_dirty = false;

void gui_set_cursor(cursor_kind k) {
    if ((unsigned)k > CURSOR_HAND) k = CURSOR_ARROW;
    if (k != g_cursor) { g_cursor = k; g_cursor_dirty = true; }
}
cursor_kind gui_cursor_get(void)   { return g_cursor; }
bool gui_cursor_take_dirty(void)   { bool d = g_cursor_dirty; g_cursor_dirty = false; return d; }

/* Draw a horizontal double arrow into the 0/2 grid, centred on row cy. */
static void raster_we(uint8_t *g, int w, int h) {
    int cy = h / 2, head = 6;
    for (int x = head; x < w - head; x++)             /* the shaft            */
        for (int y = cy - 1; y <= cy + 1; y++) g[y * w + x] = 2;
    for (int x = 0; x <= head; x++) {                 /* two arrowheads       */
        int e = head - x;
        for (int y = cy - e; y <= cy + e; y++) {
            g[y * w + x] = 2;
            g[y * w + (w - 1 - x)] = 2;
        }
    }
}
static void raster_ns(uint8_t *g, int w, int h) {     /* the same, rotated    */
    int cx = w / 2, head = 6;
    for (int y = head; y < h - head; y++)
        for (int x = cx - 1; x <= cx + 1; x++) g[y * w + x] = 2;
    for (int y = 0; y <= head; y++) {
        int e = head - y;
        for (int x = cx - e; x <= cx + e; x++) {
            g[y * w + x] = 2;
            g[(h - 1 - y) * w + x] = 2;
        }
    }
}
static void raster_diag(uint8_t *g, int w, int h, bool nwse) {
    /* A thick diagonal shaft corner to corner, with an arrowhead at each end. */
    for (int i = 0; i < w; i++) {
        int x = i, y = nwse ? i : (h - 1 - i);
        for (int t = -1; t <= 1; t++) {
            int yy = y + t;
            if (x >= 0 && x < w && yy >= 0 && yy < h) g[yy * w + x] = 2;
            int xx = x + t;
            if (xx >= 0 && xx < w && y >= 0 && y < h) g[y * w + xx] = 2;
        }
    }
    int hd = 7;
    for (int a = 0; a < hd; a++) for (int b = 0; b < hd - a; b++) {
        int c1x = a, c1y = nwse ? b : (h - 1 - b);            /* near corner  */
        int c2x = w - 1 - a, c2y = nwse ? (h - 1 - b) : b;    /* far corner   */
        if (c1x < w && c1y >= 0 && c1y < h) g[c1y * w + c1x] = 2;
        if (c2x >= 0 && c2y >= 0 && c2y < h) g[c2y * w + c2x] = 2;
    }
}
static void raster_text(uint8_t *g, int w, int h) {   /* an I-beam            */
    int cx = w / 2;
    for (int y = 1; y < h - 1; y++) g[y * w + cx] = 2;
    for (int x = cx - 2; x <= cx + 2; x++) { g[1 * w + x] = 2; g[(h - 2) * w + x] = 2; }
}
static void raster_hand(uint8_t *g, int w, int h) {   /* a simple pointing hand */
    int cx = w / 2;
    for (int y = 2; y < 9; y++) { g[y * w + cx] = 2; g[y * w + cx + 1] = 2; }   /* finger */
    for (int y = 8; y < h - 2; y++)                                             /* palm   */
        for (int x = cx - 3; x <= cx + 3; x++) g[y * w + x] = 2;
}

/* Fill `g` for a shape and report its box + hotspot.  Returns 0 for the arrow
 * (handled from its own bitmap by the callers). */
static int cursor_grid(cursor_kind k, uint8_t *g, int *w, int *h, int *hx, int *hy) {
    int W = 0, H = 0, HX = 0, HY = 0;
    switch (k) {
    case CURSOR_SIZE_WE:   W = 23; H = 15; HX = 11; HY = 7; break;
    case CURSOR_SIZE_NS:   W = 15; H = 23; HX = 7;  HY = 11; break;
    case CURSOR_SIZE_NWSE:
    case CURSOR_SIZE_NESW: W = 19; H = 19; HX = 9;  HY = 9; break;
    case CURSOR_TEXT:      W = 7;  H = 19; HX = 3;  HY = 9; break;
    case CURSOR_HAND:      W = 14; H = 19; HX = 7;  HY = 2; break;
    default: return 0;                                    /* arrow */
    }
    for (int i = 0; i < W * H; i++) g[i] = 0;
    switch (k) {
    case CURSOR_SIZE_WE:   raster_we(g, W, H); break;
    case CURSOR_SIZE_NS:   raster_ns(g, W, H); break;
    case CURSOR_SIZE_NWSE: raster_diag(g, W, H, true); break;
    case CURSOR_SIZE_NESW: raster_diag(g, W, H, false); break;
    case CURSOR_TEXT:      raster_text(g, W, H); break;
    case CURSOR_HAND:      raster_hand(g, W, H); break;
    default: break;
    }
    /* Derive the outline: a transparent cell next to a filled one. */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        if (g[y * W + x]) continue;
        int touch = 0;
        for (int dy = -1; dy <= 1 && !touch; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < W && ny >= 0 && ny < H && g[ny * W + nx] == 2) { touch = 1; break; }
        }
        if (touch) g[y * W + x] = 1;
    }
    *w = W; *h = H; *hx = HX; *hy = HY;
    return 1;
}

/* The current pointer as a picture the display adapter can be given.  Costs
 * nothing to move once handed over; the window manager re-hands it only when
 * the SHAPE changes (gui_cursor_take_dirty). */
int gui_cursor_image(uint32_t *out, int *width, int *height, int *hot_x, int *hot_y) {
    static uint8_t g[CUR_MAX * CUR_MAX];
    int w, h, hx, hy;
    if (cursor_grid(g_cursor, g, &w, &h, &hx, &hy)) {
        for (int i = 0; i < w * h; i++)
            out[i] = g[i] == 1 ? 0xFF101010u : g[i] == 2 ? 0xFFFFFFFFu : 0u;
        if (width) *width = w;
        if (height) *height = h;
        if (hot_x) *hot_x = hx;
        if (hot_y) *hot_y = hy;
        return w;
    }
    for (int row = 0; row < 19; row++)                 /* the arrow bitmap    */
        for (int col = 0; col < 12; col++) {
            uint8_t v = cursor_bits[row][col];
            out[row * 12 + col] = v == 1 ? 0xFF101010u : v ? 0xFFFFFFFFu : 0u;
        }
    if (width) *width = 12;
    if (height) *height = 19;
    if (hot_x) *hot_x = 0;
    if (hot_y) *hot_y = 0;
    return 12;
}

void gui_cursor(surface_t *s, int x, int y) {
    static uint8_t g[CUR_MAX * CUR_MAX];
    int w, h, hx, hy;
    if (cursor_grid(g_cursor, g, &w, &h, &hx, &hy)) {
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++) {
                uint8_t v = g[row * w + col];
                if (!v) continue;
                gui_pixel(s, x - hx + col, y - hy + row,
                          v == 1 ? RGB(0x10, 0x10, 0x10) : RGB(0xFF, 0xFF, 0xFF));
            }
        return;
    }
    for (int row = 0; row < 19; row++)
        for (int col = 0; col < 12; col++) {
            uint8_t v = cursor_bits[row][col];
            if (!v) continue;
            gui_pixel(s, x + col, y + row, v == 1 ? RGB(0x10, 0x10, 0x10) : RGB(0xFF, 0xFF, 0xFF));
        }
}

/* Break on spaces to fit `r`, and return the y just past the last line drawn.
 * Long words are left to overhang rather than being split, which reads better
 * than a hyphen the caller did not ask for. */
int gui_text_wrapped(surface_t *s, font_id font, rect_t r, const char *text, colour_t c) {
    if (!text || r.w <= 0 || r.h <= 0) return r.y;
    int line_h = gui_font_height(font) + 3;
    int y = r.y;
    const char *p = text;
    rect_t saved = surface_clip(s);
    surface_set_clip(s,rect_intersection(saved,r));
    while (*p && (int64_t)y + line_h <= (int64_t)r.y + r.h) {
        size_t n = 0, last_space = 0;
        int used = 0;
        while (p[n] && p[n] != '\n') {
            int advance = gui_font_advance(font,(unsigned char)p[n]);
            if (advance > r.w - used) break;
            used += advance;
            if (p[n] == ' ') last_space = n + 1;
            n++;
        }
        /* Prefer a word boundary, but split an overlong word to guarantee
         * progress. Even a glyph wider than the box is clipped to the box. */
        if (p[n] && p[n] != '\n' && last_space) n = last_space;
        if (!n && *p != '\n') n = 1;
        size_t draw = n;
        while (draw && p[draw-1] == ' ') draw--;
        gui_text_n(s,font,r.x,y,p,draw,c);
        y += line_h;
        p += n;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    surface_set_clip(s,saved);
    return y;
}

/* ------------------------------------------------------------ compositing */

void gui_blit_alpha(surface_t *dst, const surface_t *src, int x, int y, int alpha) {
    if (alpha >= 255) { gui_blit(dst, src, x, y); return; }
    if (alpha <= 0) return;

    rect_t dr = rect_intersection(rect_make(x, y, src->width, src->height),
                                  surface_clip(dst));
    if (rect_empty(dr)) return;

    int skip_x = dr.x - x, skip_y = dr.y - y;
    if (gui_gpu_blit(dst,src,rect_make(skip_x,skip_y,dr.w,dr.h),dr,alpha)) return;
    for (int row = 0; row < dr.h; row++) {
        const colour_t *sp = src->pixels + (size_t)(skip_y + row) * src->stride + skip_x;
        colour_t *dp = dst->pixels + (size_t)(dr.y + row) * dst->stride + dr.x;
        blit_blend_copy_row(dp, sp, dr.w, alpha);
    }
}

/* Nearest-neighbour, because the scale factors an animation uses are close to
 * one and the frames go by in a fifth of a second: a smoother filter would
 * cost more than it could possibly show. */
void gui_blit_scaled(surface_t *dst, const surface_t *src, rect_t dst_rect, int alpha) {
    if (dst_rect.w <= 0 || dst_rect.h <= 0 || alpha <= 0) return;
    if (src->width <= 0 || src->height <= 0) return;

    rect_t dr = rect_intersection(dst_rect, surface_clip(dst));
    if (rect_empty(dr)) return;
    if (gui_gpu_blit(dst,src,rect_make(0,0,src->width,src->height),dst_rect,alpha)) return;

    for (int y = dr.y; y < dr.y + dr.h; y++) {
        int sy = (y - dst_rect.y) * src->height / dst_rect.h;
        if (sy < 0) sy = 0;
        if (sy >= src->height) sy = src->height - 1;

        const colour_t *sp = src->pixels + (size_t)sy * src->stride;
        colour_t *dp = dst->pixels + (size_t)y * dst->stride;

        for (int x = dr.x; x < dr.x + dr.w; x++) {
            int sx = (x - dst_rect.x) * src->width / dst_rect.w;
            if (sx < 0) sx = 0;
            if (sx >= src->width) sx = src->width - 1;
            dp[x] = alpha >= 255 ? sp[sx] : colour_mix(dp[x], sp[sx], alpha);
        }
    }
}

/* ---------------------------------------------------------------- shadows */

/* The falloff of a shadow at a given distance from the edge.  Squared rather
 * than linear, which is what makes it read as soft rather than as a ramp. */
static int shadow_falloff(int distance, int spread, int strength) {
    if (distance >= spread) return 0;
    int remaining = spread - distance;
    return (strength * remaining * remaining) / (spread * spread);
}

void gui_soft_shadow(surface_t *s, rect_t r, int spread, int strength, int radius) {
    if (spread <= 0) return;
    if (gui_gpu_shadow(s,r,spread,strength,radius,g_theme.window_shadow)) return;
    rect_t clip = surface_clip(s);
    rect_t area = rect_intersection(
        rect_make(r.x - spread, r.y - spread + 2, r.w + 2 * spread, r.h + 2 * spread + 2),
        clip);
    if (rect_empty(area)) return;

    /* The shadow sits a little below the window, the way a light from above
     * would put it. */
    rect_t body = rect_make(r.x, r.y + 3, r.w, r.h);

    /* Only the ring outside the window is drawn.  Walking the whole rectangle
     * and skipping the middle costs a branch for every pixel the window is
     * about to cover anyway - for a window most of the screen wide that is
     * several hundred thousand pixels of arithmetic per frame, thrown away.
     * Splitting the ring into four bands never touches the interior at all.
     *
     * The bands: everything above the window, everything below it, and the two
     * strips beside it, which between them cover the ring exactly once. */
    typedef struct { int x0, y0, x1, y1; } band_t;
    band_t band[4];
    int bands = 0;

    int left = area.x, right = area.x + area.w;
    int top = area.y, bottom = area.y + area.h;

    int body_top = body.y > top ? body.y : top;
    int body_bottom = body.y + body.h < bottom ? body.y + body.h : bottom;
    int body_left = body.x > left ? body.x : left;
    int body_right = body.x + body.w < right ? body.x + body.w : right;

    if (body_top > top) {
        band_t b = { left, top, right, body_top };
        band[bands++] = b;
    }
    if (bottom > body_bottom) {
        band_t b = { left, body_bottom, right, bottom };
        band[bands++] = b;
    }
    if (body_left > left) {
        band_t b = { left, body_top, body_left, body_bottom };
        band[bands++] = b;
    }
    if (right > body_right) {
        band_t b = { body_right, body_top, right, body_bottom };
        band[bands++] = b;
    }

    for (int i = 0; i < bands; i++) {
        for (int y = band[i].y0; y < band[i].y1; y++) {

            /* The vertical distance is the same for every pixel in the row. */
            int dy = 0;
            if (y < body.y) dy = body.y - y;
            else if (y >= body.y + body.h) dy = y - (body.y + body.h) + 1;

            for (int x = band[i].x0; x < band[i].x1; x++) {
                int dx = 0;
                if (x < body.x) dx = body.x - x;
                else if (x >= body.x + body.w) dx = x - (body.x + body.w) + 1;

                /* Distance outside the body, measured per axis and combined; at
                 * a corner both contribute, which rounds the shadow. */
                int distance = dx > dy ? dx + dy / 2 : dy + dx / 2;
                /* Near a rounded corner the window does not reach the edge, so
                 * the shadow starts a little further in. */
                if (dx && dy && radius > 0) distance += radius / 3;

                int alpha = shadow_falloff(distance, spread, strength);
                if (alpha > 0) blend_at(s,x,y,g_theme.window_shadow,alpha);
            }
        }
    }
}

/* ------------------------------------------------------- rounded, smoothed */

/* How much of the pixel at (dx, dy) from the corner's centre is inside the
 * circle, as 0 to 255.  Sampling the distance rather than testing it is what
 * removes the staircase. */
static int corner_coverage(int dx, int dy, int radius) {
    /* Work in sixteenths so the comparison stays in integers. */
    int d2 = dx * dx + dy * dy;
    if (d2 <= (radius - 1) * (radius - 1)) return 255;
    if (d2 >= (radius + 1) * (radius + 1)) return 0;

    /* Between the two, fade across the two-pixel band. */
    int inner = (radius - 1) * (radius - 1);
    int outer = (radius + 1) * (radius + 1);
    return 255 - (d2 - inner) * 255 / (outer - inner);
}

void gui_round_rect_aa(surface_t *s, rect_t r, int radius, colour_t c) {
    if(gui_gpu_rounded(s,r,radius,0u,c,c))return;
    if (radius <= 0) { gui_fill(s, r, c); return; }
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius * 2 > r.h) radius = r.h / 2;

    gui_fill(s, rect_make(r.x + radius, r.y, r.w - 2 * radius, r.h), c);
    gui_fill(s, rect_make(r.x, r.y + radius, radius, r.h - 2 * radius), c);
    gui_fill(s, rect_make(r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius), c);

    rect_t clip = surface_clip(s);
    for (int y = 0; y < radius; y++) {
        for (int x = 0; x < radius; x++) {
            int coverage = corner_coverage(radius - x, radius - y, radius);
            if (!coverage) continue;

            int px[4] = { r.x + x, r.x + r.w - 1 - x, r.x + x, r.x + r.w - 1 - x };
            int py[4] = { r.y + y, r.y + y, r.y + r.h - 1 - y, r.y + r.h - 1 - y };
            for (int i = 0; i < 4; i++) {
                if (!rect_contains(clip, px[i], py[i])) continue;
                blend_at(s,px[i],py[i],c,coverage);
            }
        }
    }
}

void gui_round_frame_aa(surface_t *s, rect_t r, int radius, colour_t c) {
    if(gui_gpu_rounded(s,r,radius,1u,c,c))return;
    if (radius <= 0) { gui_frame(s, r, c); return; }
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius * 2 > r.h) radius = r.h / 2;

    gui_hline(s, r.x + radius, r.y, r.w - 2 * radius, c);
    gui_hline(s, r.x + radius, r.y + r.h - 1, r.w - 2 * radius, c);
    gui_vline(s, r.x, r.y + radius, r.h - 2 * radius, c);
    gui_vline(s, r.x + r.w - 1, r.y + radius, r.h - 2 * radius, c);

    rect_t clip = surface_clip(s);
    for (int y = 0; y < radius; y++) {
        for (int x = 0; x < radius; x++) {
            /* The outline is where coverage changes: full inside, none
             * outside, and the band between is the line itself. */
            int coverage = corner_coverage(radius - x, radius - y, radius);
            int inner = corner_coverage(radius - x, radius - y, radius - 1);
            int edge = coverage - inner;
            if (edge <= 4) continue;

            int px[4] = { r.x + x, r.x + r.w - 1 - x, r.x + x, r.x + r.w - 1 - x };
            int py[4] = { r.y + y, r.y + y, r.y + r.h - 1 - y, r.y + r.h - 1 - y };
            for (int i = 0; i < 4; i++) {
                if (!rect_contains(clip, px[i], py[i])) continue;
                blend_at(s,px[i],py[i],c,edge);
            }
        }
    }
}

void gui_round_top(surface_t *s, rect_t r, int radius, colour_t c) {
    if(gui_gpu_rounded(s,r,radius,2u,c,c))return;
    if (radius <= 0) { gui_fill(s, r, c); return; }
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius > r.h) radius = r.h;

    gui_fill(s, rect_make(r.x + radius, r.y, r.w - 2 * radius, radius), c);
    gui_fill(s, rect_make(r.x, r.y + radius, r.w, r.h - radius), c);

    rect_t clip = surface_clip(s);
    for (int y = 0; y < radius; y++) {
        for (int x = 0; x < radius; x++) {
            int coverage = corner_coverage(radius - x, radius - y, radius);
            if (!coverage) continue;
            int px[2] = { r.x + x, r.x + r.w - 1 - x };
            for (int i = 0; i < 2; i++) {
                if (!rect_contains(clip, px[i], r.y + y)) continue;
                blend_at(s,px[i],r.y+y,c,coverage);
            }
        }
    }
}

void gui_round_gradient_top(surface_t *s, rect_t r, int radius, colour_t top, colour_t bottom) {
    if(gui_gpu_rounded(s,r,radius,3u,top,bottom))return;
    /* The gradient is drawn first and the corners cut out of it afterwards, so
     * the rounding follows the colour rather than flattening it. */
    if (r.h <= 0) return;
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius > r.h) radius = r.h;

    rect_t clip = surface_clip(s);
    for (int y = 0; y < r.h; y++) {
        colour_t line = colour_mix(top, bottom, r.h > 1 ? y * 255 / (r.h - 1) : 0);
        int inset = 0;
        if (y < radius) {
            /* How far in the edge is at this row, from the same circle. */
            while (inset < radius &&
                   !corner_coverage(radius - inset, radius - y, radius)) inset++;
        }
        int sy = r.y + y;
        if (!rect_contains(clip, r.x, sy) && !rect_contains(clip, r.x + r.w - 1, sy)) {
            if (sy < clip.y || sy >= clip.y + clip.h) continue;
        }
        gui_hline(s, r.x + inset, sy, r.w - 2 * inset, line);

        if (y < radius && inset > 0) {
            /* Feather the two pixels at the ends of the row. */
            for (int x = inset - 1; x >= 0 && x >= inset - 2; x--) {
                int coverage = corner_coverage(radius - x, radius - y, radius);
                if (!coverage) continue;
                int px[2] = { r.x + x, r.x + r.w - 1 - x };
                for (int i = 0; i < 2; i++) {
                    if (!rect_contains(clip, px[i], sy)) continue;
                    blend_at(s,px[i],sy,line,coverage);
                }
            }
        }
    }
}

/* -------------------------------------------------------------- easing */

int anim_lerp(int from, int to, int t) {
    if (t <= 0) return from;
    if (t >= ANIM_ONE) return to;
    return from + (to - from) * t / ANIM_ONE;
}

rect_t anim_lerp_rect(rect_t from, rect_t to, int t) {
    return rect_make(anim_lerp(from.x, to.x, t), anim_lerp(from.y, to.y, t),
                     anim_lerp(from.w, to.w, t), anim_lerp(from.h, to.h, t));
}

/* Cubic ease-out: fast at first and settling gently, which is what makes a
 * window look like it arrived rather than teleported. */
int anim_ease_out(int t) {
    if (t <= 0) return 0;
    if (t >= ANIM_ONE) return ANIM_ONE;
    int inverse = ANIM_ONE - t;
    long long cube = (long long)inverse * inverse * inverse;
    return ANIM_ONE - (int)(cube / ((long long)ANIM_ONE * ANIM_ONE));
}

int anim_ease_in(int t) {
    if (t <= 0) return 0;
    if (t >= ANIM_ONE) return ANIM_ONE;
    long long cube = (long long)t * t * t;
    return (int)(cube / ((long long)ANIM_ONE * ANIM_ONE));
}

int anim_ease_in_out(int t) {
    if (t < ANIM_ONE / 2) return anim_ease_in(t * 2) / 2;
    return ANIM_ONE / 2 + anim_ease_out((t - ANIM_ONE / 2) * 2) / 2;
}
