/* aatext.c - text with smooth edges, at any size.
 *
 * The console font is one bit per pixel and grid-fitted, which is the right
 * answer for a terminal: every character lands on the same cell and the strokes
 * are crisp.  A page of prose is a different problem.  It wants a heading three
 * times the size of the body text, it wants bold where the author asked for
 * bold, and it wants the letters to look like letters rather than like a
 * staircase.
 *
 * These faces carry a coverage value for every pixel - how much of that pixel
 * the letter covers - so drawing is a blend rather than a decision.  They were
 * rasterised at one size each; other sizes are produced by resampling, and the
 * result is cached, because a page redraws far more often than its text
 * changes and rescaling a glyph on every frame would be visible.
 *
 * Resampling down averages over the area a destination pixel covers, which is
 * what keeps a thirty-two pixel heading face readable at nineteen.  Resampling
 * up interpolates.  Neither is as good as rasterising at the target size from
 * an outline, and both are far better than the alternative available here.
 */
#include "gui.h"
#include "aatext.h"

#include "font_web.h"
#include "font_web_bold.h"
#include "font_web_head.h"
#include "font_web_mono.h"

const aafont_t AA_BODY = {
    (const uint8_t *)font_web, font_web_width,
    FONT_WEB_W, FONT_WEB_H, FONT_WEB_BASELINE, 16, false,
};
const aafont_t AA_BODY_BOLD = {
    (const uint8_t *)font_web_bold, font_web_bold_width,
    FONT_WEB_BOLD_W, FONT_WEB_BOLD_H, FONT_WEB_BOLD_BASELINE, 16, false,
};
const aafont_t AA_HEADING = {
    (const uint8_t *)font_web_head, font_web_head_width,
    FONT_WEB_HEAD_W, FONT_WEB_HEAD_H, FONT_WEB_HEAD_BASELINE, 32, false,
};
const aafont_t AA_CODE = {
    (const uint8_t *)font_web_mono, font_web_mono_width,
    FONT_WEB_MONO_W, FONT_WEB_MONO_H, FONT_WEB_MONO_BASELINE, 15, true,
};

/* ------------------------------------------------------------- the cache
 *
 * One entry per face and size in use.  A page rarely needs more than a
 * handful: body, bold, a heading level or two, and code.
 */
#define CACHE_ENTRIES 12

typedef struct {
    const aafont_t *font;
    int   size;
    int   w, h, baseline;
    uint8_t *glyph[256];           /* built on first use */
    uint8_t  advance[256];
    /* The same advance, in 256ths of a pixel.
     *
     * Rounding each glyph's advance to a whole pixel before adding it to the
     * next is what made spacing uneven: the error does not cancel, it
     * accumulates along the line, so gaps that should be equal differ by a
     * pixel and a glyph whose advance rounded down has the next one set on
     * top of its right-hand side.  Keeping the pen fractional and rounding
     * only when a glyph is placed keeps every gap within half a pixel of
     * where it belongs however long the line is. */
    int      advance_fx[256];
    bool  in_use;
    unsigned last_used;
} cache_t;

static cache_t cache[CACHE_ENTRIES];
static unsigned clock_tick;

static void drop_entry(cache_t *e) {
    for (int i = 0; i < 256; i++) {
        if (e->glyph[i]) { free(e->glyph[i]); e->glyph[i] = NULL; }
    }
    e->in_use = false;
}

static cache_t *entry_for(const aafont_t *font, int size) {
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].in_use && cache[i].font == font && cache[i].size == size) {
            cache[i].last_used = ++clock_tick;
            return &cache[i];
        }
    }

    /* A free slot, or the one nobody has touched for longest. */
    cache_t *chosen = NULL;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].in_use) { chosen = &cache[i]; break; }
        if (!chosen || cache[i].last_used < chosen->last_used) chosen = &cache[i];
    }
    if (chosen->in_use) drop_entry(chosen);

    memset(chosen, 0, sizeof *chosen);
    chosen->font = font;
    chosen->size = size;
    chosen->in_use = true;
    chosen->last_used = ++clock_tick;

    /* The cell scales with the requested size. */
    chosen->w = (font->w * size + font->nominal / 2) / font->nominal;
    chosen->h = (font->h * size + font->nominal / 2) / font->nominal;
    chosen->baseline = (font->baseline * size + font->nominal / 2) / font->nominal;
    if (chosen->w < 1) chosen->w = 1;
    if (chosen->h < 1) chosen->h = 1;

    for (int i = 0; i < 256; i++) {
        int fx = (font->width[i] * size * 256 + font->nominal / 2) / font->nominal;
        if (fx < 256) fx = 256;                 /* never narrower than a pixel */
        chosen->advance_fx[i] = fx;

        int advance = (fx + 128) / 256;
        if (advance < 1) advance = 1;
        if (advance > 255) advance = 255;
        chosen->advance[i] = (uint8_t)advance;
    }
    return chosen;
}

/* Build one glyph at the cache entry's size, by area-averaging the master. */
static uint8_t *build_glyph(cache_t *e, unsigned char ch) {
    const aafont_t *f = e->font;
    uint8_t *out = malloc((size_t)e->w * (size_t)e->h);
    if (!out) return NULL;

    const uint8_t *src = f->pixels + (size_t)ch * (size_t)f->w * (size_t)f->h;

    for (int y = 0; y < e->h; y++) {
        /* The source rows this destination row covers.  Working in the source
         * space rather than stepping a fraction avoids the accumulated error
         * that makes one row in a glyph a pixel too tall. */
        int sy0 = y * f->h / e->h;
        int sy1 = ((y + 1) * f->h + e->h - 1) / e->h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > f->h) sy1 = f->h;

        for (int x = 0; x < e->w; x++) {
            int sx0 = x * f->w / e->w;
            int sx1 = ((x + 1) * f->w + e->w - 1) / e->w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > f->w) sx1 = f->w;

            int total = 0, count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = src + (size_t)sy * (size_t)f->w;
                for (int sx = sx0; sx < sx1; sx++) { total += row[sx]; count++; }
            }
            out[y * e->w + x] = count ? (uint8_t)(total / count) : 0;
        }
    }
    return out;
}

static const uint8_t *glyph_of(cache_t *e, unsigned char ch) {
    if (!e->glyph[ch]) e->glyph[ch] = build_glyph(e, ch);
    return e->glyph[ch];
}

/* ---------------------------------------------------------------- metrics */

int aa_line_height(const aafont_t *font, int size) {
    return (font->h * size + font->nominal / 2) / font->nominal;
}

int aa_baseline(const aafont_t *font, int size) {
    return (font->baseline * size + font->nominal / 2) / font->nominal;
}

int aa_advance(const aafont_t *font, int size, unsigned char ch) {
    int advance = (font->width[ch] * size + font->nominal / 2) / font->nominal;
    return advance < 1 ? 1 : advance;
}

/* Measured exactly as it is drawn.  When these two disagree a caller that
 * centres or right-aligns text places it a pixel or two out, and one that
 * reserves room for it sees the last glyph clipped.
 *
 * The text is bytes into the 256-glyph (CP437-layout) font: ASCII is itself and
 * the upper half is accented letters, box-drawing and symbols.  Callers that
 * take Unicode - the browser - fold it to this layout before it gets here (the
 * HTML parser's fold_to_byte), so nothing above needs to decode UTF-8. */
int aa_text_width_n(const aafont_t *font, int size, const char *text, size_t n) {
    int total = 0;
    cache_t *e = entry_for(font, size);
    int pen = 0;
    for (size_t i = 0; i < n; i++) pen += e->advance_fx[(unsigned char)text[i]];
    total = (pen + 128) >> 8;
    return total;
}

int aa_text_width(const aafont_t *font, int size, const char *text) {
    return aa_text_width_n(font, size, text, strlen(text));
}

/* ---------------------------------------------------------------- drawing */

/* One glyph, blended by coverage.  `top` is the top of the cell, not the
 * baseline: callers that think in baselines subtract aa_baseline first. */
static void draw_glyph(surface_t *s, cache_t *e, unsigned char ch,
                       int x, int top, colour_t colour, int alpha) {
    const uint8_t *glyph = glyph_of(e, ch);
    if (!glyph) return;

    rect_t clip = surface_clip(s);

    int y0 = top, y1 = top + e->h;
    if (y0 < clip.y) y0 = clip.y;
    if (y1 > clip.y + clip.h) y1 = clip.y + clip.h;

    int x0 = x, x1 = x + e->w;
    if (x0 < clip.x) x0 = clip.x;
    if (x1 > clip.x + clip.w) x1 = clip.x + clip.w;
    if (x0 >= x1 || y0 >= y1) return;
    if (s->gpu) {
        if(alpha<=0)return;
        uint8_t *mask=malloc((size_t)e->w*e->h);
        if(!mask){log_write(3,"gui-gpu","no AA glyph staging memory");return;}
        for(int i=0;i<e->w*e->h;i++) {
            int coverage=glyph[i];
            if(alpha<255)coverage=coverage*alpha/255;
            mask[i]=coverage>=254?255:coverage;
        }
        gui_gpu_mask(s,mask,e->w,e->h,e->w,x,top,colour,255);
        free(mask);return;
    }

    int red = (int)RGB_R(colour), green = (int)RGB_G(colour), blue = (int)RGB_B(colour);

    for (int py = y0; py < y1; py++) {
        const uint8_t *row = glyph + (size_t)(py - top) * (size_t)e->w;
        colour_t *dst = s->pixels + (size_t)py * (size_t)s->stride + x0;
        for (int px = x0; px < x1; px++, dst++) {
            int coverage = row[px - x];
            if (!coverage) continue;
            if (alpha < 255) coverage = coverage * alpha / 255;
            if (coverage >= 254) {
                *dst = colour;
                continue;
            }
            colour_t under = *dst;
            int inverse = 255 - coverage;
            int r = ((int)RGB_R(under) * inverse + red * coverage) / 255;
            int g = ((int)RGB_G(under) * inverse + green * coverage) / 255;
            int b = ((int)RGB_B(under) * inverse + blue * coverage) / 255;
            *dst = RGB(r, g, b);
        }
    }
}

int aa_text_n(surface_t *s, const aafont_t *font, int size, int x, int baseline_y,
              const char *text, size_t n, colour_t colour) {
    return aa_text_alpha(s, font, size, x, baseline_y, text, n, colour, 255);
}

int aa_text_alpha(surface_t *s, const aafont_t *font, int size, int x, int baseline_y,
                  const char *text, size_t n, colour_t colour, int alpha) {
    cache_t *e = entry_for(font, size);
    int top = baseline_y - e->baseline;

    /* The pen is fractional; only the position a glyph is placed at is whole. */
    int pen = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch != ' ') draw_glyph(s, e, ch, x + ((pen + 128) >> 8), top, colour, alpha);
        pen += e->advance_fx[ch];
    }
    return x + ((pen + 128) >> 8);
}

int aa_text(surface_t *s, const aafont_t *font, int size, int x, int baseline_y,
            const char *text, colour_t colour) {
    return aa_text_n(s, font, size, x, baseline_y, text, strlen(text), colour);
}

/* A line that has to fit: cut it and mark the cut, rather than letting it run
 * past the edge of whatever it is in. */
int aa_text_clipped(surface_t *s, const aafont_t *font, int size, int x,
                    int baseline_y, int max_width, const char *text,
                    colour_t colour) {
    size_t len = strlen(text);
    if (aa_text_width_n(font, size, text, len) <= max_width)
        return aa_text_n(s, font, size, x, baseline_y, text, len, colour);

    int ellipsis = aa_text_width(font, size, "...");
    int room = max_width - ellipsis;
    cache_t *e = entry_for(font, size);
    size_t fits = 0;
    int pen = 0;
    while (fits < len) {
        int next = pen + e->advance_fx[(unsigned char)text[fits]];
        if (((next + 128) >> 8) > room) break;
        pen = next;
        fits++;
    }
    int at = aa_text_n(s, font, size, x, baseline_y, text, fits, colour);
    return aa_text(s, font, size, at, baseline_y, "...", colour);
}

void aa_forget_cached(void) {
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (cache[i].in_use) drop_entry(&cache[i]);
}
