/* raster.c - clip-space triangles to pixels.
 *
 * The stage that matters.  A triangle arrives in clip space, is clipped against
 * the near plane, divided through by w, mapped into the viewport, and filled by
 * walking its bounding box and testing each pixel against the three edges.
 *
 * Two details are what separate a rasteriser that looks right from one that
 * does not:
 *
 * Perspective correction.  Interpolating a texture coordinate linearly across
 * the screen is wrong as soon as the triangle is not parallel to the viewer -
 * a floor's texture visibly slides and warps.  What varies linearly in screen
 * space is the attribute divided by w, so each one is interpolated as u/w
 * alongside 1/w, and divided back out per pixel.
 *
 * The fill rule.  Two triangles sharing an edge must paint every pixel on it
 * exactly once: paint it twice and blending doubles up along the seam, paint it
 * never and a crack of background shows through.  A top-left rule decides
 * ownership consistently from the edge's direction alone, so neighbouring
 * triangles always agree without either knowing about the other.
 */
#include "glstate.h"
#include "../libc/math.h"

/* Attributes carried through the pipeline, in the order the interpolator
 * walks them.  Keeping them in one array means adding an attribute is a
 * change to this constant rather than to every loop. */
enum { A_R, A_G, A_B, A_A, A_S, A_T, A_COUNT };

typedef struct {
    float x, y, z;             /* window coordinates, z in [0,1]          */
    float inv_w;               /* 1/w, interpolated linearly in screen space */
    float attr[A_COUNT];       /* already divided by w                    */
    /* What a vertex shader wrote, divided by w like everything else so the
     * interpolation below is perspective-correct for these too - which is the
     * whole reason a shaded floor does not warp. */
    float varying[SR_VARYING_N][4];
} frag_vertex_t;

/* ------------------------------------------------------------------ helpers */

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float minf3(float a, float b, float c) {
    float m = a < b ? a : b;
    return m < c ? m : c;
}
static inline float maxf3(float a, float b, float c) {
    float m = a > b ? a : b;
    return m > c ? m : c;
}

static bool depth_passes(GLenum func, float incoming, float stored) {
    switch (func) {
    case GL_NEVER:    return false;
    case GL_LESS:     return incoming <  stored;
    case GL_EQUAL:    return incoming == stored;
    case GL_LEQUAL:   return incoming <= stored;
    case GL_GREATER:  return incoming >  stored;
    case GL_NOTEQUAL: return incoming != stored;
    case GL_GEQUAL:   return incoming >= stored;
    default:          return true;
    }
}

static bool alpha_passes(GLenum func, float a, float ref) {
    switch (func) {
    case GL_NEVER:    return false;
    case GL_LESS:     return a <  ref;
    case GL_EQUAL:    return a == ref;
    case GL_LEQUAL:   return a <= ref;
    case GL_GREATER:  return a >  ref;
    case GL_NOTEQUAL: return a != ref;
    case GL_GEQUAL:   return a >= ref;
    default:          return true;
    }
}

static float blend_factor(GLenum f, const float *src, const float *dst) {
    switch (f) {
    case GL_ZERO:                return 0.0f;
    case GL_ONE:                 return 1.0f;
    case GL_SRC_ALPHA:           return src[3];
    case GL_ONE_MINUS_SRC_ALPHA: return 1.0f - src[3];
    case GL_DST_ALPHA:           return dst[3];
    case GL_ONE_MINUS_DST_ALPHA: return 1.0f - dst[3];
    default:                     return 1.0f;
    }
}

/* --------------------------------------------------------------- the target */

static inline bool inside_scissor(int x, int y) {
    if (!g_gl.scissor_on) return true;
    return x >= g_gl.sc_x && x < g_gl.sc_x + g_gl.sc_w &&
           y >= g_gl.sc_y && y < g_gl.sc_y + g_gl.sc_h;
}

/* One finished fragment: test it, blend it, store it. */
static void emit(int x, int y, float z, const float *rgba) {
    surface_t *s = g_gl.colour;
    if (!s || x < 0 || y < 0 || x >= s->width || y >= s->height) return;
    if (!inside_scissor(x, y)) return;

    float src[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };

    if (g_gl.alpha_test && !alpha_passes(g_gl.alpha_func, src[3], g_gl.alpha_ref))
        return;

    float *depth = NULL;
    if (g_gl.depth && x < g_gl.depth_w && y < g_gl.depth_h)
        depth = &g_gl.depth[(size_t)y * g_gl.depth_w + x];

    if (g_gl.depth_test && depth && !depth_passes(g_gl.depth_func, z, *depth))
        return;

    if (g_gl.blend) {
        colour_t existing = s->pixels[(size_t)y * s->stride + x];
        float dst[4] = {
            RGB_R(existing) * (1.0f / 255.0f),
            RGB_G(existing) * (1.0f / 255.0f),
            RGB_B(existing) * (1.0f / 255.0f),
            1.0f,
        };
        float sf = blend_factor(g_gl.blend_src, src, dst);
        float df = blend_factor(g_gl.blend_dst, src, dst);
        for (int i = 0; i < 3; i++) src[i] = clampf(src[i] * sf + dst[i] * df, 0.0f, 1.0f);
    }

    int r = (int)(clampf(src[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    int gg = (int)(clampf(src[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    int b = (int)(clampf(src[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    s->pixels[(size_t)y * s->stride + x] = RGB(r, gg, b);

    if (depth && g_gl.depth_write) *depth = z;
    g_gl.stat_fragments++;
}

/* ------------------------------------------------------- clip and transform */

/* Divide through by w and map into the viewport.  The attributes are divided
 * by w here so the interpolator can walk them linearly. */
static void to_window(const gl_vertex_t *v, frag_vertex_t *out) {
    float w = v->clip.w;
    if (fabsf(w) < 1e-7f) w = (w < 0.0f) ? -1e-7f : 1e-7f;
    float inv_w = 1.0f / w;

    float ndc_x = v->clip.x * inv_w;
    float ndc_y = v->clip.y * inv_w;
    float ndc_z = v->clip.z * inv_w;

    out->x = g_gl.vp_x + (ndc_x * 0.5f + 0.5f) * (float)g_gl.vp_w;
    /* OpenGL's and Direct3D's y grows upwards where a surface's grows down, so
     * it is flipped; Vulkan's already points down and is taken as it comes. */
    float ndc_y_down = g_gl.clip_y_down ? ndc_y : -ndc_y;
    out->y = g_gl.vp_y + (ndc_y_down * 0.5f + 0.5f) * (float)g_gl.vp_h;
    float depth01 = g_gl.clip_depth_zero_to_one ? ndc_z : (ndc_z * 0.5f + 0.5f);
    out->z = g_gl.depth_near + depth01 * (g_gl.depth_far - g_gl.depth_near);
    out->inv_w = inv_w;

    out->attr[A_R] = v->r * inv_w;
    out->attr[A_G] = v->g * inv_w;
    out->attr[A_B] = v->b * inv_w;
    out->attr[A_A] = v->a * inv_w;
    out->attr[A_S] = v->s * inv_w;
    out->attr[A_T] = v->t * inv_w;

    if (gl_program_active()) {
        for (int i = 0; i < SR_VARYING_N; i++)
            for (int c = 0; c < 4; c++)
                out->varying[i][c] = v->varying[i][c] * inv_w;
    }
}

/* Interpolate a whole vertex, in clip space, for the clipper. */
static gl_vertex_t lerp_vertex(const gl_vertex_t *a, const gl_vertex_t *b, float t) {
    gl_vertex_t r;
    r.clip.x = a->clip.x + (b->clip.x - a->clip.x) * t;
    r.clip.y = a->clip.y + (b->clip.y - a->clip.y) * t;
    r.clip.z = a->clip.z + (b->clip.z - a->clip.z) * t;
    r.clip.w = a->clip.w + (b->clip.w - a->clip.w) * t;
    r.r = a->r + (b->r - a->r) * t;
    r.g = a->g + (b->g - a->g) * t;
    r.b = a->b + (b->b - a->b) * t;
    r.a = a->a + (b->a - a->a) * t;
    r.s = a->s + (b->s - a->s) * t;
    r.t = a->t + (b->t - a->t) * t;
    /* A triangle clipped against the near plane produces new vertices, and
     * whatever the vertex shader wrote has to be carried onto them or the
     * shading changes wherever the geometry crosses the eye. */
    for (int i = 0; i < SR_VARYING_N; i++)
        for (int c = 0; c < 4; c++)
            r.varying[i][c] = a->varying[i][c] +
                              (b->varying[i][c] - a->varying[i][c]) * t;
    return r;
}

/* Clip against w > epsilon.  This is the one plane that must be clipped rather
 * than left to the scissor: a vertex behind the eye has a negative w, and
 * dividing by it flips the triangle inside out and smears it across the screen.
 * The other five planes are handled for free by the bounding box and scissor. */
static int clip_near(const gl_vertex_t *in, int n, gl_vertex_t *out) {
    const float epsilon = 1e-5f;
    int count = 0;

    for (int i = 0; i < n; i++) {
        const gl_vertex_t *cur = &in[i];
        const gl_vertex_t *next = &in[(i + 1) % n];
        bool cur_in = cur->clip.w > epsilon;
        bool next_in = next->clip.w > epsilon;

        if (cur_in) out[count++] = *cur;
        if (cur_in != next_in) {
            float t = (epsilon - cur->clip.w) / (next->clip.w - cur->clip.w);
            out[count++] = lerp_vertex(cur, next, t);
        }
        if (count >= 8) break;
    }
    return count;
}

/* GPU records must remain bounded even when a triangle crosses the eye.
 * Clipping only w would create enormous window coordinates at w=epsilon.
 * A convex triangle gains at most one vertex per plane: 16 slots cover all
 * seven planes, for either GL [-w,w] or D3D/Vulkan [0,w] depth convention. */
static float gpu_plane(const gl_vertex_t *v,unsigned plane) {
    switch(plane) {
    case 0:return v->clip.w-1e-5f;
    case 1:return v->clip.w+v->clip.x;
    case 2:return v->clip.w-v->clip.x;
    case 3:return v->clip.w+v->clip.y;
    case 4:return v->clip.w-v->clip.y;
    case 5:return g_gl.clip_depth_zero_to_one?v->clip.z:v->clip.w+v->clip.z;
    default:return v->clip.w-v->clip.z;
    }
}
static int clip_gpu(const gl_vertex_t *in,gl_vertex_t *out) {
    gl_vertex_t a[16],b[16];
    for(int i=0;i<3;i++) {
        if(!__builtin_isfinite(in[i].clip.x) || !__builtin_isfinite(in[i].clip.y) ||
           !__builtin_isfinite(in[i].clip.z) || !__builtin_isfinite(in[i].clip.w))return 0;
        a[i]=in[i];
    }
    int count=3;
    for(unsigned plane=0;plane<7 && count;plane++) {
        int next_count=0;
        for(int i=0;i<count;i++) {
            const gl_vertex_t *cur=&a[i],*next=&a[(i+1)%count];
            float dc=gpu_plane(cur,plane),dn=gpu_plane(next,plane);
            if(!__builtin_isfinite(dc) || !__builtin_isfinite(dn))return 0;
            bool ci=dc>=0,ni=dn>=0;
            if(ci){if(next_count>=16)return 0;b[next_count++]=*cur;}
            if(ci!=ni) {
                if(next_count>=16)return 0;
                b[next_count++]=lerp_vertex(cur,next,dc/(dc-dn));
            }
        }
        count=next_count;memcpy(a,b,(size_t)count*sizeof a[0]);
    }
    memcpy(out,a,(size_t)count*sizeof a[0]);return count;
}

static bool clip_gpu_line(gl_vertex_t *a,gl_vertex_t *b) {
    for(unsigned plane=0;plane<7;plane++) {
        float da=gpu_plane(a,plane),db=gpu_plane(b,plane);
        if(!__builtin_isfinite(da) || !__builtin_isfinite(db))return false;
        if(da<0 && db<0)return false;
        if((da<0)!=(db<0)) {
            gl_vertex_t v=lerp_vertex(a,b,da/(da-db));
            if(da<0)*a=v;else *b=v;
        }
    }
    return true;
}

static kg3d_vertex_t gpu_vertex(const frag_vertex_t *v) {
    kg3d_vertex_t out={v->x,v->y,v->z,v->inv_w,
        {v->attr[0],v->attr[1],v->attr[2],v->attr[3]},{v->attr[4],v->attr[5]}};
    return out;
}

/* ---------------------------------------------------------------- the fill */

/* Twice the signed area of the triangle abc.  Positive means counter-clockwise
 * in window coordinates, and the sign is also what tells an edge function
 * which side of a line a point is on. */
static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Whether a pixel exactly on this edge belongs to this triangle.  Deciding from
 * the edge's direction alone means two triangles sharing an edge always reach
 * opposite answers, so the pixel is painted exactly once. */
static inline bool top_left(float ax, float ay, float bx, float by) {
    if (ay == by) return bx < ax;      /* a horizontal top edge */
    return by < ay;                     /* a left edge          */
}

static void fill(const frag_vertex_t *v0, const frag_vertex_t *v1,
                 const frag_vertex_t *v2) {
    surface_t *s = g_gl.colour;
    if (!s) return;
    if(s->gpu) {
        kg3d_vertex_t a={v0->x,v0->y,v0->z,v0->inv_w,{v0->attr[0],v0->attr[1],v0->attr[2],v0->attr[3]},{v0->attr[4],v0->attr[5]}};
        kg3d_vertex_t b={v1->x,v1->y,v1->z,v1->inv_w,{v1->attr[0],v1->attr[1],v1->attr[2],v1->attr[3]},{v1->attr[4],v1->attr[5]}};
        kg3d_vertex_t c={v2->x,v2->y,v2->z,v2->inv_w,{v2->attr[0],v2->attr[1],v2->attr[2],v2->attr[3]},{v2->attr[4],v2->attr[5]}};
        if(gl_shader_gpu_active())gl_gpu_shader_primitive(&a,&b,&c,&v0->varying[0][0],&v1->varying[0][0],&v2->varying[0][0],0);
        else gl_gpu_triangle(&a,&b,&c);
        return;
    }

    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (area == 0.0f) return;

    /* Work in one winding so the edge tests all point the same way. */
    const frag_vertex_t *a = v0, *b = v1, *c = v2;
    if (area < 0.0f) { const frag_vertex_t *t = b; b = c; c = t; area = -area; }

    int min_x = (int)floorf(minf3(a->x, b->x, c->x));
    int max_x = (int)ceilf (maxf3(a->x, b->x, c->x));
    int min_y = (int)floorf(minf3(a->y, b->y, c->y));
    int max_y = (int)ceilf (maxf3(a->y, b->y, c->y));

    int lo_x = g_gl.vp_x, hi_x = g_gl.vp_x + g_gl.vp_w;
    int lo_y = g_gl.vp_y, hi_y = g_gl.vp_y + g_gl.vp_h;
    if (g_gl.scissor_on) {
        if (g_gl.sc_x > lo_x) lo_x = g_gl.sc_x;
        if (g_gl.sc_y > lo_y) lo_y = g_gl.sc_y;
        if (g_gl.sc_x + g_gl.sc_w < hi_x) hi_x = g_gl.sc_x + g_gl.sc_w;
        if (g_gl.sc_y + g_gl.sc_h < hi_y) hi_y = g_gl.sc_y + g_gl.sc_h;
    }
    if (lo_x < 0) lo_x = 0;
    if (lo_y < 0) lo_y = 0;
    if (hi_x > s->width)  hi_x = s->width;
    if (hi_y > s->height) hi_y = s->height;

    if (min_x < lo_x) min_x = lo_x;
    if (min_y < lo_y) min_y = lo_y;
    if (max_x > hi_x) max_x = hi_x;
    if (max_y > hi_y) max_y = hi_y;
    if (min_x >= max_x || min_y >= max_y) return;

    float inv_area = 1.0f / area;

    /* Bias each edge by the fill rule, so a pixel exactly on it is included
     * only for the triangle that owns it. */
    float bias0 = top_left(b->x, b->y, c->x, c->y) ? 0.0f : -1e-5f;
    float bias1 = top_left(c->x, c->y, a->x, a->y) ? 0.0f : -1e-5f;
    float bias2 = top_left(a->x, a->y, b->x, b->y) ? 0.0f : -1e-5f;

    bool textured = g_gl.texture_2d &&
                    g_gl.bound_texture < GL_MAX_TEXTURES &&
                    g_gl.textures[g_gl.bound_texture].used;

    /* With a program in use the fixed-function colour and texture stages are
     * bypassed entirely: the fragment shader decides. */
    const sh_program_t *program = gl_current_program();
    int nvaryings = program ? program->nvaryings : 0;

    /* The edge functions are affine in x and y, so they only have to be
     * evaluated once per row and stepped along it. */
    float dw0_dx = -(c->y - b->y);
    float dw1_dx = -(a->y - c->y);
    float dw2_dx = -(b->y - a->y);

    for (int y = min_y; y < max_y; y++) {
        float py = (float)y + 0.5f;
        float px = (float)min_x + 0.5f;

        float w0 = edge(b->x, b->y, c->x, c->y, px, py);
        float w1 = edge(c->x, c->y, a->x, a->y, px, py);
        float w2 = edge(a->x, a->y, b->x, b->y, px, py);

        /* Rather than testing every pixel across the bounding box against
         * three edges, work out where along this row all three are satisfied
         * and visit only that.  Each edge function is linear in x, so the
         * range it allows is a half-line whose end can be solved for directly;
         * the span is the intersection of three of them.
         *
         * For a long thin triangle - which is most of what a mesh is made of -
         * the bounding box is mostly outside the triangle, so this is the
         * difference between touching every pixel of the box and touching only
         * the ones that are actually covered. */
        int span_lo = min_x, span_hi = max_x;
        {
            const float ws[3] = { w0 + bias0, w1 + bias1, w2 + bias2 };
            const float ds[3] = { dw0_dx, dw1_dx, dw2_dx };
            for (int e = 0; e < 3; e++) {
                if (ds[e] > 1e-9f) {
                    /* Satisfied from some x onward. */
                    if (ws[e] < 0.0f) {
                        /* ceil, not truncation+1: an exactly integral crossing
                         * on an owned top/left edge includes that pixel. Clamp
                         * before converting so a distant crossing cannot wrap. */
                        float crossing = ceilf(-ws[e] / ds[e]);
                        int from = crossing >= (float)(max_x - min_x) ? max_x :
                                   min_x + (int)crossing;
                        if (from > span_lo) span_lo = from;
                    }
                } else if (ds[e] < -1e-9f) {
                    /* Satisfied up to some x. */
                    if (ws[e] < 0.0f) { span_hi = span_lo; break; }
                    float crossing = floorf(ws[e] / -ds[e]);
                    int upto = crossing >= (float)(max_x - min_x) ? max_x :
                               min_x + (int)crossing + 1;
                    if (upto < span_hi) span_hi = upto;
                } else if (ws[e] < 0.0f) {
                    /* Constant along the row and never satisfied. */
                    span_hi = span_lo;
                    break;
                }
            }
            if (span_lo < min_x) span_lo = min_x;
            if (span_hi > max_x) span_hi = max_x;
        }
        if (span_lo >= span_hi) continue;

        /* Step the edge values up to where the span starts. */
        {
            float advance = (float)(span_lo - min_x);
            w0 += dw0_dx * advance;
            w1 += dw1_dx * advance;
            w2 += dw2_dx * advance;
        }

        for (int x = span_lo; x < span_hi; x++,
             w0 += dw0_dx, w1 += dw1_dx, w2 += dw2_dx) {

            /* The span arithmetic is derived from the same edge functions, but
             * rounding at its ends can be a pixel out either way, so the test
             * stays. */
            if (w0 + bias0 < 0.0f || w1 + bias1 < 0.0f || w2 + bias2 < 0.0f)
                continue;

            float l0 = w0 * inv_area, l1 = w1 * inv_area, l2 = w2 * inv_area;

            float z = a->z * l0 + b->z * l1 + c->z * l2 + g_gl.poly_offset_units;

            /* Everything else was divided by w on the way in, so it is linear
             * here; dividing by the interpolated 1/w undoes it per pixel and
             * gives the perspective-correct value. */
            float inv_w = a->inv_w * l0 + b->inv_w * l1 + c->inv_w * l2;
            if (inv_w == 0.0f) continue;
            float w = 1.0f / inv_w;

            float rgba[4];

            if (program) {
                float varying[SR_VARYING_N][4];
                for (int i = 0; i < nvaryings; i++)
                    for (int k = 0; k < 4; k++)
                        varying[i][k] = (a->varying[i][k] * l0 +
                                         b->varying[i][k] * l1 +
                                         c->varying[i][k] * l2) * w;
                for (int i = nvaryings; i < SR_VARYING_N; i++)
                    varying[i][0] = varying[i][1] =
                    varying[i][2] = varying[i][3] = 0.0f;

                /* A fragment the shader threw away is not a fragment: no
                 * colour, and no depth either, which is what makes discard
                 * usable for cut-out shapes. */
                if (!gl_shader_fragment(varying, (float)x + 0.5f,
                                        (float)y + 0.5f, z, w, rgba))
                    continue;

                emit(x, y, z, rgba);
                continue;
            }

            rgba[0] = (a->attr[A_R]*l0 + b->attr[A_R]*l1 + c->attr[A_R]*l2) * w;
            rgba[1] = (a->attr[A_G]*l0 + b->attr[A_G]*l1 + c->attr[A_G]*l2) * w;
            rgba[2] = (a->attr[A_B]*l0 + b->attr[A_B]*l1 + c->attr[A_B]*l2) * w;
            rgba[3] = (a->attr[A_A]*l0 + b->attr[A_A]*l1 + c->attr[A_A]*l2) * w;

            if (textured) {
                float s_coord = (a->attr[A_S]*l0 + b->attr[A_S]*l1 + c->attr[A_S]*l2) * w;
                float t_coord = (a->attr[A_T]*l0 + b->attr[A_T]*l1 + c->attr[A_T]*l2) * w;

                float texel[4];
                gl_sample(s_coord, t_coord, texel);

                switch (g_gl.tex_env) {
                case GL_REPLACE:
                    rgba[0] = texel[0]; rgba[1] = texel[1];
                    rgba[2] = texel[2]; rgba[3] = texel[3];
                    break;
                case GL_DECAL:
                    for (int i = 0; i < 3; i++)
                        rgba[i] = rgba[i] * (1.0f - texel[3]) + texel[i] * texel[3];
                    break;
                default:                            /* modulate */
                    for (int i = 0; i < 4; i++) rgba[i] *= texel[i];
                    break;
                }
            }

            emit(x, y, z, rgba);
        }
    }
}

/* ---------------------------------------------------------------- triangles */

void gl_raster_triangle(const gl_vertex_t *a, const gl_vertex_t *b,
                        const gl_vertex_t *c) {
    if (!g_gl.colour) return;

    gl_vertex_t in[3] = { *a, *b, *c };
    gl_vertex_t clipped[16];
    int n = g_gl.colour->gpu ? clip_gpu(in,clipped) : clip_near(in, 3, clipped);
    if (n < 3) return;

    /* Face culling happens in window space, after the divide, because that is
     * where the winding is actually decided. */
    for (int i = 1; i + 1 < n; i++) {
        frag_vertex_t f0, f1, f2;
        to_window(&clipped[0], &f0);
        to_window(&clipped[i], &f1);
        to_window(&clipped[i + 1], &f2);

        if (g_gl.cull) {
            /* Screen y grows downward, which flips the sign of the area
             * relative to what OpenGL calls counter-clockwise. */
            float area = edge(f0.x, f0.y, f1.x, f1.y, f2.x, f2.y);
            bool ccw = (g_gl.front_face == GL_CCW) ? (area < 0.0f) : (area > 0.0f);
            bool is_front = ccw;
            if (g_gl.cull_face == GL_FRONT_AND_BACK) continue;
            if (g_gl.cull_face == GL_BACK && !is_front) continue;
            if (g_gl.cull_face == GL_FRONT && is_front) continue;
        }

        fill(&f0, &f1, &f2);
        g_gl.stat_triangles++;
    }
}

/* -------------------------------------------------------------- lines, dots */

void gl_raster_line(const gl_vertex_t *a, const gl_vertex_t *b) {
    if (!g_gl.colour) return;

    gl_vertex_t in[2] = { *a, *b };
    if(g_gl.colour->gpu) {
        if(!clip_gpu_line(&in[0],&in[1]))return;
        frag_vertex_t p,q;to_window(&in[0],&p);to_window(&in[1],&q);
        kg3d_vertex_t gp=gpu_vertex(&p),gq=gpu_vertex(&q);
        if(gl_shader_gpu_active())gl_gpu_shader_primitive(&gp,&gq,&gq,&p.varying[0][0],&q.varying[0][0],&q.varying[0][0],KG3D_LINE);
        else gl_gpu_line(&gp,&gq);
        return;
    }
    if (in[0].clip.w <= 1e-5f && in[1].clip.w <= 1e-5f) return;
    /* Clip the segment against the near plane the same way. */
    if (in[0].clip.w <= 1e-5f || in[1].clip.w <= 1e-5f) {
        int near_idx = in[0].clip.w <= 1e-5f ? 0 : 1;
        int far_idx = 1 - near_idx;
        float t = (1e-5f - in[near_idx].clip.w) /
                  (in[far_idx].clip.w - in[near_idx].clip.w);
        in[near_idx] = lerp_vertex(&in[near_idx], &in[far_idx], t);
    }

    frag_vertex_t p, q;
    to_window(&in[0], &p);
    to_window(&in[1], &q);

    float dx = q.x - p.x, dy = q.y - p.y;
    int steps = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy));
    if (steps <= 0) steps = 1;
    if (steps > 8192) return;

    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float inv_w = p.inv_w + (q.inv_w - p.inv_w) * t;
        if (inv_w == 0.0f) continue;
        float w = 1.0f / inv_w;

        float rgba[4];
        for (int k = 0; k < 4; k++)
            rgba[k] = (p.attr[k] + (q.attr[k] - p.attr[k]) * t) * w;

        emit((int)(p.x + dx * t), (int)(p.y + dy * t),
             p.z + (q.z - p.z) * t, rgba);
    }
}

void gl_raster_point(const gl_vertex_t *a, float size) {
    if (!g_gl.colour || a->clip.w <= 1e-5f) return;
    if(g_gl.colour->gpu) {
        for(unsigned plane=0;plane<7;plane++)
            if(!(gpu_plane(a,plane)>=0) || !__builtin_isfinite(gpu_plane(a,plane)))return;
        frag_vertex_t p;to_window(a,&p);kg3d_vertex_t v=gpu_vertex(&p);
        if(gl_shader_gpu_active())gl_gpu_shader_primitive(&v,&v,&v,&p.varying[0][0],&p.varying[0][0],&p.varying[0][0],KG3D_POINT);
        else gl_gpu_point(&v);
        return;
    }

    frag_vertex_t p;
    to_window(a, &p);

    float rgba[4] = { a->r, a->g, a->b, a->a };
    int half = (int)(size * 0.5f);
    if (half < 0) half = 0;

    for (int dy = -half; dy <= half; dy++)
        for (int dx = -half; dx <= half; dx++)
            emit((int)p.x + dx, (int)p.y + dy, p.z, rgba);
}
