/* display_layout.c - the geometry of several monitors sharing one framebuffer.
 * See display_layout.h.  No hardware, no allocation: pure arithmetic, so it is
 * testable on a single-headed machine. */
#include "../include/kestrel/display_layout.h"

int display_layout_arrange(const dl_placement_t *outputs, int n, int primary,
                           dl_mode_t mode, int max_width, int max_height,
                           dl_layout_t *out) {
    if (!outputs || !out || n < 1 || n > DISPLAY_MAX_OUTPUTS ||
        primary < 0 || primary >= n || max_width < 1 || max_height < 1)
        return 0;
    int only = -1;
    if (mode == DL_ONLY_PRIMARY) only = primary;
    else if (mode == DL_ONLY_SECONDARY) {
        if (n < 2) return 0;
        only = primary == 0 ? 1 : 0;
    } else if ((int)mode >= DL_ONLY_OUTPUT_BASE &&
               (int)mode < DL_ONLY_OUTPUT_BASE + DISPLAY_MAX_OUTPUTS) {
        only = DL_ONLY_OUTPUT_INDEX(mode);
        if (only >= n) return 0;
    } else if (mode != DL_EXTEND && mode != DL_MIRROR) return 0;

    dl_layout_t next = {0};
    next.n = n;
    long long min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    int enabled = 0;
    for (int i = 0; i < n; i++) {
        const dl_placement_t *p = &outputs[i];
        if (p->width < 1 || p->height < 1 || p->width > 32768 || p->height > 32768 ||
            (p->active != 0 && p->active != 1) ||
            (p->rotation != 0 && p->rotation != 90 && p->rotation != 180 && p->rotation != 270))
            return 0;
        int active = only >= 0 ? i == only : p->active;
        if (!active) continue;
        dl_scanout_t *s = &next.out[i];
        s->active = 1; s->rotation = p->rotation;
        s->out_w = p->width; s->out_h = p->height;
        int swap = p->rotation == 90 || p->rotation == 270;
        s->src_w = swap ? p->height : p->width;
        s->src_h = swap ? p->width : p->height;
        long long x = mode == DL_EXTEND ? p->x : 0;
        long long y = mode == DL_EXTEND ? p->y : 0;
        if (!enabled || x < min_x) min_x = x;
        if (!enabled || y < min_y) min_y = y;
        if (!enabled || x+s->src_w > max_x) max_x = x+s->src_w;
        if (!enabled || y+s->src_h > max_y) max_y = y+s->src_h;
        enabled++;
    }
    if (!enabled || (only < 0 && !next.out[primary].active)) return 0;
    if (mode == DL_MIRROR) {
        max_x = next.out[primary].src_w; max_y = next.out[primary].src_h;
        for (int i = 0; i < n; i++) if (next.out[i].active) {
            next.out[i].src_w = (int)max_x; next.out[i].src_h = (int)max_y;
        }
    }
    if (max_x-min_x > max_width || max_y-min_y > max_height) return 0;
    next.fb_width = (int)(max_x-min_x); next.fb_height = (int)(max_y-min_y);
    next.origin_x = (int)min_x; next.origin_y = (int)min_y;
    if (mode == DL_EXTEND) {
        for (int i = 0; i < n; i++) if (next.out[i].active) {
            dl_scanout_t *s = &next.out[i];
            s->src_x = (int)((long long)outputs[i].x-min_x);
            s->src_y = (int)((long long)outputs[i].y-min_y);
            for (int j = 0; j < i; j++) if (next.out[j].active) {
                const dl_scanout_t *a = &next.out[j];
                if ((long long)s->src_x < (long long)a->src_x+a->src_w &&
                    (long long)a->src_x < (long long)s->src_x+s->src_w &&
                    (long long)s->src_y < (long long)a->src_y+a->src_h &&
                    (long long)a->src_y < (long long)s->src_y+s->src_h) return 0;
            }
        }
    }
    *out = next;
    return 1;
}

int display_layout_damage(const dl_scanout_t *s, int x, int y, int w, int h,
                          int *out_x, int *out_y, int *out_w, int *out_h) {
    if (!s || !out_x || !out_y || !out_w || !out_h || w < 0 || h < 0)
        return -1;
    if (!s->active || !w || !h) return 0;
    if (s->src_x < 0 || s->src_y < 0 || s->src_w <= 0 || s->src_h <= 0 ||
        s->out_w <= 0 || s->out_h <= 0 ||
        (s->rotation != 0 && s->rotation != 90 && s->rotation != 180 && s->rotation != 270)) return -1;
    long long x0 = x, y0 = y, x1 = (long long)x + w, y1 = (long long)y + h;
    long long right = (long long)s->src_x + s->src_w;
    long long bottom = (long long)s->src_y + s->src_h;
    if (x0 < s->src_x) x0 = s->src_x;
    if (y0 < s->src_y) y0 = s->src_y;
    if (x1 > right) x1 = right;
    if (y1 > bottom) y1 = bottom;
    if (x1 <= x0 || y1 <= y0) return 0;
    /* floor(output * source_extent / output_extent) must lie in [lo, hi).
     * Thus BOTH output endpoints use ceil, not floor at the left edge. */
    int swap = s->rotation == 90 || s->rotation == 270;
    int uw = swap ? s->out_h : s->out_w, uh = swap ? s->out_w : s->out_h;
    long long ox0 = ((x0-s->src_x)*uw + s->src_w-1) / s->src_w;
    long long oy0 = ((y0-s->src_y)*uh + s->src_h-1) / s->src_h;
    long long ox1 = ((x1-s->src_x)*uw + s->src_w-1) / s->src_w;
    long long oy1 = ((y1-s->src_y)*uh + s->src_h-1) / s->src_h;
    if (ox1 <= ox0 || oy1 <= oy0) return 0;
    long long u0=ox0, u1=ox1, v0=oy0, v1=oy1;
    if (s->rotation == 90) {
        ox0=s->out_w-v1; ox1=s->out_w-v0; oy0=u0; oy1=u1;
    } else if (s->rotation == 180) {
        ox0=s->out_w-u1; ox1=s->out_w-u0;
        oy0=s->out_h-v1; oy1=s->out_h-v0;
    } else if (s->rotation == 270) {
        ox0=v0; ox1=v1; oy0=s->out_h-u1; oy1=s->out_h-u0;
    }
    *out_x = (int)ox0; *out_y = (int)oy0;
    *out_w = (int)(ox1-ox0); *out_h = (int)(oy1-oy0);
    return 1;
}

const char *display_layout_mode_name(dl_mode_t mode) {
    if ((int)mode >= (int)DL_ONLY_OUTPUT_BASE &&
        DL_ONLY_OUTPUT_INDEX(mode) < DISPLAY_MAX_OUTPUTS)
        return "only one selected display";
    switch (mode) {
        case DL_ONLY_PRIMARY:   return "only the main display";
        case DL_EXTEND:         return "extend across displays";
        case DL_MIRROR:         return "mirror the main display";
        case DL_ONLY_SECONDARY: return "only the other display";
        default:                return "unknown";
    }
}

static void one_output(dl_layout_t *out, const dl_output_size_t *o) {
    out->fb_width = o->width;
    out->fb_height = o->height;
    out->out[0].active = 1;
    out->out[0].src_x = 0;
    out->out[0].src_y = 0;
    out->out[0].src_w = o->width;
    out->out[0].src_h = o->height;
    out->out[0].out_w = o->width;
    out->out[0].out_h = o->height;
    for (int i = 1; i < out->n; i++) {
        out->out[i].active = 0;
        out->out[i].src_x = out->out[i].src_y = 0;
        out->out[i].src_w = out->out[i].src_h = 0;
        out->out[i].out_w = 0;
        out->out[i].out_h = 0;
    }
}

void display_layout_compute(const dl_output_size_t *outputs, int n,
                            dl_mode_t mode, dl_layout_t *out) {
    if (n < 1) n = 1;
    if (n > DISPLAY_MAX_OUTPUTS) n = DISPLAY_MAX_OUTPUTS;
    out->n = n;
    out->origin_x = out->origin_y = 0;
    for (int i = 0; i < DISPLAY_MAX_OUTPUTS; i++) out->out[i].rotation = 0;

    int only = -1;
    if (mode == DL_ONLY_PRIMARY) only = 0;
    else if (mode == DL_ONLY_SECONDARY) only = 1;
    else if ((int)mode >= (int)DL_ONLY_OUTPUT_BASE)
        only = DL_ONLY_OUTPUT_INDEX(mode);

    /* Without a second output, "only the other" and "extend"/"mirror" have
     * nothing to act on: fall back to the one screen there is. */
    if (n < 2)
        mode = DL_ONLY_PRIMARY;

    if (only >= 0) {
        if (only >= n) only = 0;
        out->fb_width = outputs[only].width;
        out->fb_height = outputs[only].height;
        for (int i = 0; i < n; i++) {
            dl_scanout_t *s = &out->out[i];
            if (i == only) {
                s->active = 1;
                s->src_x = s->src_y = 0;
                s->src_w = s->out_w = outputs[i].width;
                s->src_h = s->out_h = outputs[i].height;
            } else {
                s->active = 0;
                s->src_x = s->src_y = s->src_w = s->src_h = 0;
                s->out_w = s->out_h = 0;
            }
        }
        return;
    }

    switch (mode) {

    case DL_MIRROR: {
        /* Every output shows the SAME picture.  The framebuffer is the
         * primary's size; a differently-sized panel displays that same region
         * at its own native size (the driver scales it), so src is always the
         * whole primary and out_w/out_h is the panel's own size. */
        out->fb_width = outputs[0].width;
        out->fb_height = outputs[0].height;
        for (int i = 0; i < n; i++) {
            dl_scanout_t *s = &out->out[i];
            s->active = 1;
            s->src_x = 0; s->src_y = 0;
            s->src_w = outputs[0].width; s->src_h = outputs[0].height;
            s->out_w = outputs[i].width; s->out_h = outputs[i].height;
        }
        return;
    }

    case DL_EXTEND: {
        /* The desktop spans the outputs left to right.  The framebuffer is as
         * wide as the sum and as tall as the tallest; each output scans out
         * its own tile, top-aligned. */
        int total_w = 0, max_h = 0;
        for (int i = 0; i < n; i++) {
            total_w += outputs[i].width;
            if (outputs[i].height > max_h) max_h = outputs[i].height;
        }
        out->fb_width = total_w;
        out->fb_height = max_h;
        int x = 0;
        for (int i = 0; i < n; i++) {
            dl_scanout_t *s = &out->out[i];
            s->active = 1;
            s->src_x = x; s->src_y = 0;
            s->src_w = outputs[i].width; s->src_h = outputs[i].height;
            s->out_w = outputs[i].width; s->out_h = outputs[i].height;
            x += outputs[i].width;
        }
        return;
    }

    default:
        one_output(out, &outputs[0]);
        return;
    }
}
