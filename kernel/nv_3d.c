/* nv_3d.c - drawing triangles on the card.
 *
 * nv_fifo.c gets work to the engines.  This is what that work says when the
 * work is three-dimensional: where to draw, what to draw it from, which
 * programs run per vertex and per pixel, and then draw.
 *
 * The shape of it has not changed since Fermi, and the class numbers below run
 * from Fermi to Blackwell.  A driver that knows this method set knows how to
 * draw on every NVIDIA card made in the last fifteen years; what changes
 * between generations is the class number and the shader machine code, not the
 * state model.
 *
 * The state a draw needs, in the order it has to be set:
 *
 *   The render target - where the pixels go.  An address, a size, a format,
 *   and whether it is laid out in rows or in tiles.  Then which of the eight
 *   targets are actually live, because the count is separate from the targets
 *   and a target set but not counted is not drawn to.
 *
 *   The viewport - which part of that target the clip space maps onto.
 *
 *   The programs - one per stage, each a pointer into a region of memory the
 *   engine fetches shader machine code from.  The pointer is an offset from a
 *   base set once, not an address, which is the detail that makes a program
 *   loaded at the wrong offset draw nothing rather than fault.
 *
 *   The vertex streams - where the vertices are and how far apart, and what
 *   each attribute looks like inside one.
 *
 * Then begin, draw, end.  And a report written to memory afterwards, because
 * that is the only way to know it finished.
 *
 * ---------------------------------------------------------------------------
 * The one thing this cannot supply is the shader machine code itself.  The
 * compiler in user/libgl/glsl.c turns GLSL into an instruction set built for
 * this system's own interpreter, which is what draws today.  The engine here
 * wants NVIDIA's own instruction set in the code region instead.
 *
 * CORRECTED 2026-08-31: that instruction set is NOT undocumented, and the old
 * note here ("NVIDIA has never published it ... a compiler back-end for an
 * undocumented target") was wrong.  Blackwell is SM_120 and shares its
 * encoding with earlier SM; NVIDIA's own ptxas/nvdisasm (CUDA 13.3, installed
 * on the build machine) compile and disassemble it, a compute kernel built
 * -arch=sm_120 was run on the real 5070 Ti, and Mesa's NAK compiler emits and
 * validates SM_120 in open source.  See tools/nvshader.py (produces verified
 * sm_120 machine code as an embeddable asset) and the memory notes
 * kestrelos-cuda-toolchain-on-host / kestrelos-gpu-two-walls.
 *
 * So what this establishes is the whole draw pipeline except one box, and that
 * box is a GLSL->SM_120 back-end (port NAK, or drive a graphics compiler) - a
 * large effort, NOT an impossibility.  ptxas already gives SM_120 for COMPUTE;
 * graphics vertex/fragment stages are the part still to wire.  What is not
 * established is a picture on the REAL card, which needs the GSP up first.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"
#include "../tools/shader_3d_triangle.h"  /* graphics-shader supply hook (see file) */

/* Which class draws on which generation.
 *
 * This used to be a ladder of comparisons, and the ladder had a bug that is
 * worth leaving a note about because it is the bug this shape of code invites:
 * it read "0x1A0 or above is Hopper, 0x1B0 or above is Blackwell", which sends
 * every Blackwell that is not a GeForce one to Hopper's class and skips
 * Hopper's own architecture entirely.  Generations are not ordered by number
 * in the way a ladder assumes - 0x180 is Hopper and 0x190 is Ada, and Ada came
 * out first.  So it is a table now, matched exactly, in nv_blackwell.c
 * alongside the other per-generation facts.
 */
u32 nv_3d_class(u32 chipset) {
    const nv_classes_t *cl = nv_classes_for(chipset);
    return cl ? cl->three_d : 0;
}

const char *nv_3d_class_name(u32 class_number) {
    for (u32 family = 0x0c0; family <= 0x1b0; family += 0x10) {
        const nv_classes_t *cl = nv_classes_for(family);
        if (cl && cl->family == family && cl->three_d == class_number)
            return cl->name;
    }
    return "no drawing engine";
}

/* ---------------------------------------------------------- where to draw */

bool nv_3d_set_target(nv_card_t *c, nv_fifo_t *f, int subchannel,
                      u64 surface, u32 width, u32 height, u32 format) {
    (void)c;
    if (!width || !height) return false;
    if (surface & 0xFF) {
        kwarn("nv-3d", "a render target at %llx is not on a 256 byte boundary",
              (unsigned long long)surface);
        return false;
    }

    /* Address, size, format and layout are six consecutive methods, so one
     * packet carries the whole target. */
    nv_push_begin(f, subchannel, NV9097_RT_ADDRESS_HIGH(0), 6);
    nv_push_data(f, (u32)(surface >> 32));
    nv_push_data(f, (u32)surface);
    nv_push_data(f, width);
    nv_push_data(f, height);
    nv_push_data(f, format);
    /* Laid out in rows rather than tiles.  A target the engine thinks is tiled
     * and is not comes out as a picture scrambled into blocks - which looks
     * like a corrupted image rather than a wrong flag. */
    nv_push_data(f, NV9097_RT_TILE_MODE_LINEAR);
    if (!nv_push_end(f)) return false;

    /* And how many targets are live.  Setting a target without counting it
     * draws nothing, with every other register correct. */
    nv_push_begin(f, subchannel, NV9097_RT_CONTROL, 1);
    nv_push_data(f, 1);
    return nv_push_end(f);
}

/* IEEE-754 single bits for (n / 2^shift), n>=0 - integer math only, because the
 * freestanding kernel has no soft-float runtime (no SSE, -mgeneral-regs-only).
 * Viewport scale/offset are exact dyadic rationals (half-integers), so this is
 * exact. */
static u32 f32_scaled(u32 n, u32 shift) {
    if (n == 0) return 0;
    int p = 31; while (!(n & (1u << p))) p--;      /* highest set bit */
    int bexp = p - (int)shift + 127;
    u32 mant = (p >= 23) ? ((n >> (p - 23)) & 0x7FFFFFu)
                         : ((n << (23 - p)) & 0x7FFFFFu);
    return ((u32)bexp << 23) | mant;
}

bool nv_3d_set_viewport(nv_card_t *c, nv_fifo_t *f, int subchannel,
                        u32 x, u32 y, u32 width, u32 height) {
    (void)c;
    if (!width || !height) return false;
    if (width > 0xFFFF || height > 0xFFFF) return false;

    nv_push_begin(f, subchannel, NV9097_VIEWPORT_HORIZ(0), 2);
    nv_push_data(f, x | (width << 16));
    nv_push_data(f, y | (height << 16));
    if (!nv_push_end(f)) return false;

    /* The NDC->screen transform (Volta+).  This is the piece whose absence
     * makes nothing rasterise while every other register looks right: clip
     * space [-1,1] maps to screen via scale/offset, and the transform is off
     * until SET_VIEWPORT_SCALE_OFFSET.ENABLE.  SCALE = half-extent (w/2, h/2, 1),
     * OFFSET = centre (x+w/2, y+h/2, 0).  Floats built with integer math. */
    u32 sx = f32_scaled(width, 1),  sy = f32_scaled(height, 1), sz = 0x3F800000u;
    u32 ox = f32_scaled(2*x + width, 1), oy = f32_scaled(2*y + height, 1), oz = 0;
    nv_push_begin(f, subchannel, NV9097_SET_VIEWPORT_SCALE_X(0), 6);
    nv_push_data(f, sx); nv_push_data(f, sy); nv_push_data(f, sz);
    nv_push_data(f, ox); nv_push_data(f, oy); nv_push_data(f, oz);
    if (!nv_push_end(f)) return false;
    nv_push_begin(f, subchannel, NV9097_SET_VIEWPORT_SCALE_OFFSET, 1);
    nv_push_data(f, 1);   /* ENABLE_TRUE */
    if (!nv_push_end(f)) return false;

    /* The scissor is separate from the viewport and defaults to nothing on
     * some generations, so a driver that sets only the viewport draws into a
     * rectangle of zero size. */
    nv_push_begin(f, subchannel, NV9097_SCREEN_SCISSOR_HORIZ, 2);
    nv_push_data(f, x | (width << 16));
    nv_push_data(f, y | (height << 16));
    return nv_push_end(f);
}

/* ------------------------------------------------------------- the programs
 *
 * A shader is machine code sitting in memory the engine fetches from.  On
 * Volta+ (this class) each stage is given the FULL GPU VA of its shader's
 * program header (SPH) - not an offset into a shared region, which is the
 * pre-Volta model the old code used (SP_SELECT/SP_START_ID @ 0x2004, which is
 * actually SET_PIPELINE_RESERVED_B on this class - writing a program offset
 * there did nothing and no shader was ever pointed at).  Three separate method
 * groups per stage: SET_PIPELINE_SHADER(enable|type), REGISTER_COUNT+BINDING,
 * and PROGRAM_ADDRESS_A/B.  Verified against Mesa clce97.h. */
bool nv_3d_set_program(nv_card_t *c, nv_fifo_t *f, int subchannel,
                       int stage, bool enabled, u64 program_va, u32 reg_count) {
    (void)c;
    if (stage < 0 || stage > 5) return false;

    /* TYPE for the slots we use == the slot index for VERTEX(1)/PIXEL(5). */
    u32 shader = (u32)(stage << 4);
    if (enabled) shader |= NV9097_SET_PIPELINE_SHADER_ENABLE;
    nv_push_begin(f, subchannel, NV9097_SET_PIPELINE_SHADER(stage), 1);
    nv_push_data(f, shader);
    if (!nv_push_end(f)) return false;

    nv_push_begin(f, subchannel, NV9097_SET_PIPELINE_REGISTER_COUNT(stage), 2);
    nv_push_data(f, reg_count & 0x1FF);         /* REGISTER_COUNT_V (8:0)     */
    nv_push_data(f, 0);                          /* BINDING.GROUP = 0          */
    if (!nv_push_end(f)) return false;

    nv_push_begin(f, subchannel, NV9097_SET_PIPELINE_PROGRAM_ADDRESS_A(stage), 2);
    nv_push_data(f, (u32)(program_va >> 32) & 0xFF); /* A_UPPER (7:0)          */
    nv_push_data(f, (u32)program_va);                /* B_LOWER (31:0)         */
    return nv_push_end(f);
}

/* ------------------------------------------------------- where the vertices are */

bool nv_3d_set_vertex_stream(nv_card_t *c, nv_fifo_t *f, int subchannel,
                             int stream, u64 at, u32 stride) {
    (void)c;
    if (stream < 0 || stream > 15) return false;
    if (stride > 0xFFF) {
        kwarn("nv-3d", "a vertex stride of %u does not fit in the field", stride);
        return false;
    }

    /* Enabled, and how far apart the vertices are, in the same word - then the
     * address in the two that follow. */
    nv_push_begin(f, subchannel, NV9097_VERTEX_ARRAY_FETCH(stream), 3);
    nv_push_data(f, NV9097_VERTEX_ARRAY_FETCH_ENABLE | stride);
    nv_push_data(f, (u32)(at >> 32));
    nv_push_data(f, (u32)at);
    return nv_push_end(f);
}

bool nv_3d_set_attribute(nv_card_t *c, nv_fifo_t *f, int subchannel,
                         int attribute, int stream, u32 offset, u32 format) {
    (void)c;
    if (attribute < 0 || attribute > 31) return false;
    if (offset > 0x7FF) return false;

    nv_push_begin(f, subchannel, NV9097_VERTEX_ATTRIB_FORMAT(attribute), 1);
    nv_push_data(f, (u32)stream | (offset << 7) | format);
    return nv_push_end(f);
}

/* ------------------------------------------------------------------- drawing */

bool nv_3d_clear(nv_card_t *c, nv_fifo_t *f, int subchannel,
                 float r, float g, float b, float a) {
    (void)c;
    union { float f; u32 u; } cast;

    nv_push_begin(f, subchannel, NV9097_CLEAR_COLOR(0), 4);
    cast.f = r; nv_push_data(f, cast.u);
    cast.f = g; nv_push_data(f, cast.u);
    cast.f = b; nv_push_data(f, cast.u);
    cast.f = a; nv_push_data(f, cast.u);
    if (!nv_push_end(f)) return false;

    nv_push_begin(f, subchannel, NV9097_CLEAR_BUFFERS, 1);
    nv_push_data(f, NV9097_CLEAR_BUFFERS_R | NV9097_CLEAR_BUFFERS_G |
                    NV9097_CLEAR_BUFFERS_B | NV9097_CLEAR_BUFFERS_A);
    return nv_push_end(f);
}

bool nv_3d_draw(nv_card_t *c, nv_fifo_t *f, int subchannel,
                u32 primitive, u32 first, u32 count) {
    (void)c;
    if (!count) return false;

    nv_push_begin(f, subchannel, NV9097_VERTEX_BEGIN_GL, 1);
    nv_push_data(f, primitive);
    if (!nv_push_end(f)) return false;

    nv_push_begin(f, subchannel, NV9097_VERTEX_BUFFER_FIRST, 2);
    nv_push_data(f, first);
    nv_push_data(f, count);
    if (!nv_push_end(f)) return false;

    nv_push_begin(f, subchannel, NV9097_VERTEX_END_GL, 1);
    nv_push_data(f, 0);
    return nv_push_end(f);
}

/* What the engine writes when the drawing is done.  Not the channel's
 * semaphore: this one is written by the drawing engine after its own pipeline
 * has drained, which is a different moment and the one that matters. */
bool nv_3d_report(nv_card_t *c, nv_fifo_t *f, int subchannel, u64 at,
                  u32 value) {
    (void)c;
    if (at & 0xF) return false;

    nv_push_begin(f, subchannel, NV9097_QUERY_ADDRESS_HIGH, 4);
    nv_push_data(f, (u32)(at >> 32));
    nv_push_data(f, (u32)at);
    nv_push_data(f, value);
    /* Write it when everything before has finished, rather than when this
     * packet is read - the difference between a fence and a lie.  On this class
     * (SET_REPORT_SEMAPHORE_D, 0x1b0c) that is PIPELINE_LOCATION_ALL(0xF<<12) |
     * RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE(bit4) | ONE_WORD(bit28); the
     * old 0x1000 was PIPELINE_LOCATION_DATA_ASSEMBLER - it fires after vertex
     * fetch, before rasterisation/pixel shading, so the "done" flag was a lie. */
    nv_push_data(f, NV9097_REPORT_SEMAPHORE_D_RELEASE_ALL);
    return nv_push_end(f);
}

/* ==========================================================================
 * A drawing engine that is not there.
 *
 * It holds the state the methods set and refuses a draw that is missing any of
 * it - no target, no viewport, no program, no vertices - because those are the
 * four ways a pipeline draws nothing while every individual register looks
 * right.  It does not rasterise: what it checks is that the driver built a
 * complete and correctly ordered pipeline, which is the part that is this
 * code's to get right.
 * ==========================================================================
 */
static struct {
    bool present;

    u64  target;
    u32  target_w, target_h, target_format, target_layout;
    int  targets_live;

    u32  viewport, scissor;
    bool viewport_transform;      /* SET_VIEWPORT_SCALE_OFFSET.ENABLE seen     */
    bool stage_on[6];
    u64  stage_addr[6];           /* full program VA per stage (Volta+)         */

    bool stream_on[16];
    u64  stream_at[16];
    u32  stream_stride[16];
    u32  attribute[32];
    int  attributes;

    u32  clear_colour[4];
    int  clears;

    int  draws;
    u32  last_primitive, last_first, last_count;

    u64  report_at;
    u32  report_value;
    int  reports;

    int  refusals;
    char complaint[160];
} three;

static void three_complain(const char *what) {
    three.refusals++;
    if (three.complaint[0]) return;
    size_t n = 0;
    while (what[n] && n < sizeof three.complaint - 1) {
        three.complaint[n] = what[n];
        n++;
    }
    three.complaint[n] = 0;
}

/* Everything a draw needs.  Checked here rather than assumed, because the
 * whole value of a model is refusing what the hardware refuses. */
static bool ready_to_draw(void) {
    if (!three.target || !three.target_w || !three.target_h) {
        three_complain("a draw with no render target");
        return false;
    }
    if (!three.targets_live) {
        three_complain("a draw with a target set but none counted live");
        return false;
    }
    if (!three.viewport || !three.scissor) {
        three_complain("a draw with no viewport or no scissor");
        return false;
    }
    if (!three.viewport_transform) {
        three_complain("a draw with no viewport scale/offset transform enabled");
        return false;
    }
    if ((!three.stage_on[0] || !three.stage_addr[0]) &&
        (!three.stage_on[1] || !three.stage_addr[1])) {
        three_complain("a draw with no vertex program (enabled + address)");
        return false;
    }
    if (!three.stage_on[5] || !three.stage_addr[5]) {
        three_complain("a draw with no fragment program (enabled + address)");
        return false;
    }

    bool any = false;
    for (int i = 0; i < 16; i++) if (three.stream_on[i]) any = true;
    if (!any) {
        three_complain("a draw with no vertices");
        return false;
    }
    if (!three.attributes) {
        three_complain("a draw with vertices whose layout was never described");
        return false;
    }
    return true;
}

void nv_3d_model_method(int subchannel, u32 method, u32 value) {
    (void)subchannel;
    if (!three.present) return;

    for (int i = 0; i < 8; i++) {
        if (method == (u32)NV9097_RT_ADDRESS_HIGH(i)) {
            three.target = (three.target & 0xFFFFFFFFull) | ((u64)value << 32);
            return;
        }
        if (method == (u32)NV9097_RT_ADDRESS_HIGH(i) + 4) {
            three.target = (three.target & ~0xFFFFFFFFull) | value;
            return;
        }
        if (method == (u32)NV9097_RT_HORIZ(i)) { three.target_w = value; return; }
        if (method == (u32)NV9097_RT_VERT(i))  { three.target_h = value; return; }
        if (method == (u32)NV9097_RT_FORMAT(i)) { three.target_format = value; return; }
        if (method == (u32)NV9097_RT_TILE_MODE(i)) { three.target_layout = value; return; }
    }

    if (method == NV9097_RT_CONTROL) {
        three.targets_live = (int)(value & 0xF);
        return;
    }
    if (method == (u32)NV9097_VIEWPORT_HORIZ(0)) { three.viewport = value; return; }
    if (method == (u32)NV9097_VIEWPORT_HORIZ(0) + 4) { return; }
    if (method == NV9097_SET_VIEWPORT_SCALE_OFFSET) {
        three.viewport_transform = (value & 1) != 0; return;
    }
    if (method >= (u32)NV9097_SET_VIEWPORT_SCALE_X(0) &&
        method <= (u32)NV9097_SET_VIEWPORT_OFFSET_X(0) + 8) { return; }  /* scale/offset words */
    if (method == NV9097_SCREEN_SCISSOR_HORIZ) { three.scissor = value; return; }
    if (method == NV9097_SCREEN_SCISSOR_VERT) { return; }

    for (int s = 0; s < 6; s++) {
        if (method == (u32)NV9097_SET_PIPELINE_SHADER(s)) {
            three.stage_on[s] = (value & NV9097_SET_PIPELINE_SHADER_ENABLE) != 0;
            /* TYPE (7:4) must equal the slot for the stages we use. */
            if (((value >> 4) & 0xF) != (u32)s)
                three_complain("a program slot was told it was a different stage");
            return;
        }
        if (method == (u32)NV9097_SET_PIPELINE_PROGRAM_ADDRESS_A(s)) {
            three.stage_addr[s] = (three.stage_addr[s] & 0xFFFFFFFFull) |
                                  ((u64)(value & 0xFF) << 32);
            return;
        }
        if (method == (u32)NV9097_SET_PIPELINE_PROGRAM_ADDRESS_B(s)) {
            three.stage_addr[s] = (three.stage_addr[s] & ~0xFFFFFFFFull) | value;
            return;
        }
        if (method == (u32)NV9097_SET_PIPELINE_REGISTER_COUNT(s)) return;
        if (method == (u32)NV9097_SET_PIPELINE_BINDING(s)) return;
    }

    for (int i = 0; i < 16; i++) {
        if (method == (u32)NV9097_VERTEX_ARRAY_FETCH(i)) {
            three.stream_on[i] = (value & NV9097_VERTEX_ARRAY_FETCH_ENABLE) != 0;
            three.stream_stride[i] = value & 0xFFF;
            return;
        }
        if (method == (u32)NV9097_VERTEX_ARRAY_START_HIGH(i)) {
            three.stream_at[i] = (three.stream_at[i] & 0xFFFFFFFFull) |
                                 ((u64)value << 32);
            return;
        }
        if (method == (u32)NV9097_VERTEX_ARRAY_START_HIGH(i) + 4) {
            three.stream_at[i] = (three.stream_at[i] & ~0xFFFFFFFFull) | value;
            return;
        }
    }

    for (int i = 0; i < 32; i++) {
        if (method == (u32)NV9097_VERTEX_ATTRIB_FORMAT(i)) {
            if (!three.attribute[i]) three.attributes++;
            three.attribute[i] = value;
            return;
        }
    }

    for (int i = 0; i < 4; i++) {
        if (method == (u32)NV9097_CLEAR_COLOR(i)) {
            three.clear_colour[i] = value;
            return;
        }
    }
    if (method == NV9097_CLEAR_BUFFERS) {
        if (!three.target) three_complain("a clear with no render target");
        else three.clears++;
        return;
    }

    if (method == NV9097_VERTEX_BEGIN_GL) { three.last_primitive = value; return; }
    if (method == NV9097_VERTEX_BUFFER_FIRST) { three.last_first = value; return; }
    if (method == NV9097_VERTEX_BUFFER_FIRST + 4) {
        three.last_count = value;
        return;
    }
    if (method == NV9097_VERTEX_END_GL) {
        if (ready_to_draw()) three.draws++;
        return;
    }

    if (method == NV9097_QUERY_ADDRESS_HIGH) {
        three.report_at = (three.report_at & 0xFFFFFFFFull) | ((u64)value << 32);
        return;
    }
    if (method == NV9097_QUERY_ADDRESS_HIGH + 4) {
        three.report_at = (three.report_at & ~0xFFFFFFFFull) | value;
        return;
    }
    if (method == NV9097_QUERY_SEQUENCE) { three.report_value = value; return; }
    if (method == NV9097_QUERY_GET) {
        three.reports++;
        return;
    }
}

void nv_3d_model_attach(void) {
    memset(&three, 0, sizeof three);
    three.present = true;
}

void nv_3d_model_detach(void) { three.present = false; }

int  nv_3d_model_draws(void)      { return three.draws; }
int  nv_3d_model_clears(void)     { return three.clears; }
int  nv_3d_model_reports(void)    { return three.reports; }
int  nv_3d_model_refusals(void)   { return three.refusals; }
u32  nv_3d_model_primitive(void)  { return three.last_primitive; }
u32  nv_3d_model_count(void)      { return three.last_count; }
u64  nv_3d_model_target(void)     { return three.target; }
bool nv_3d_model_viewport_transform(void) { return three.viewport_transform; }
u64  nv_3d_model_stage_addr(int s) {
    return (s >= 0 && s < 6) ? three.stage_addr[s] : 0;
}
const char *nv_3d_model_complaint(void) { return three.complaint; }

/* ------------------------------------------------------------------- test */

#define TD_RING   16
#define TD_PUSH   1024
#define TD_SURF_W 64
#define TD_SURF_H 64

int nv_3d_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) return 0;

    /* The class each generation draws with, from what the chip said it was. */
    struct { u32 chipset; u32 expect; } generations[] = {
        { 0x0C0, NV_CLASS_3D_FERMI },
        { 0x0E0, NV_CLASS_3D_KEPLER },
        { 0x110, NV_CLASS_3D_MAXWELL },
        { 0x120, NV_CLASS_3D_MAXWELL_B },   /* the same family, two classes */
        { 0x134, NV_CLASS_3D_PASCAL },
        { 0x140, NV_CLASS_3D_VOLTA },
        { 0x162, NV_CLASS_3D_TURING },
        { 0x172, NV_CLASS_3D_AMPERE },
        { 0x192, NV_CLASS_3D_ADA },
        /* Hopper sits at a LOWER number than Ada and shipped after it, which
         * is why picking a generation by "this number or above" gets it
         * wrong.  Both Blackwells are here for the same reason: the die that
         * went to datacentres is not the die in an RTX 50 card. */
        { 0x180, NV_CLASS_3D_HOPPER },
        { 0x1A0, NV_CLASS_3D_BLACKWELL },
        { 0x1B3, NV_CLASS_3D_BLACKWELL_GEFORCE },  /* GB203 - an RTX 5070 Ti */
    };
    for (size_t i = 0; i < sizeof generations / sizeof generations[0]; i++) {
        u32 got = nv_3d_class(generations[i].chipset);
        if (got != generations[i].expect) {
            kerr("nv-3d", "chipset %03x draws with class %04x, expected %04x",
                 generations[i].chipset, got, generations[i].expect);
            failures++;
        }
    }

    u64 phys = 0;
    u8 *pages = dma_alloc_pages(16, &phys);
    if (!pages) {
        kerr("nv-3d", "no memory for a drawing channel");
        return failures + 1;
    }
    memset(pages, 0, PAGE_SIZE * 16);

    u64 *ring = (u64 *)pages;
    u32 *push = (u32 *)(pages + PAGE_SIZE);
    volatile u32 *semaphore = (volatile u32 *)(pages + PAGE_SIZE * 3);
    u64 surface_gpu = phys + PAGE_SIZE * 4;      /* where the picture goes   */
    u64 vertices_gpu = phys + PAGE_SIZE * 12;    /* and where it comes from  */
    u64 code_gpu = phys + PAGE_SIZE * 13;        /* and the programs         */
    u64 report_gpu = phys + PAGE_SIZE * 15;

    u32 class_3d = nv_3d_class(0x1B3);           /* Blackwell */
    nv_fifo_model_set_3d_class(class_3d);
    nv_fifo_model_attach(phys + PAGE_SIZE * 3, phys, pages, PAGE_SIZE * 16);
    nv_3d_model_attach();

    static nv_fifo_t f;
    if (!nv_fifo_init(c, &f, 0, ring, phys, TD_RING, push, phys + PAGE_SIZE,
                      TD_PUSH, semaphore, phys + PAGE_SIZE * 3)) {
        kerr("nv-3d", "the channel would not come up");
        nv_3d_model_detach();
        nv_fifo_model_detach();
        return failures + 1;
    }

    const int subchannel = 2;
    if (!nv_fifo_bind(c, &f, subchannel, class_3d)) {
        kerr("nv-3d", "the drawing class would not bind");
        failures++;
    }

    /* A draw with nothing set up has to be refused by the engine rather than
     * silently producing nothing - which is the failure every one of the four
     * missing pieces below actually causes on hardware. */
    {
        int before = nv_3d_model_refusals();
        nv_3d_draw(c, &f, subchannel, NV9097_PRIMITIVE_TRIANGLES, 0, 3);
        nv_fifo_submit(c, &f);
        if (nv_3d_model_refusals() == before) {
            kerr("nv-3d", "a draw with no state at all was accepted");
            failures++;
        }
    }

    /* A fresh engine for the real pipeline, so the complaint the deliberate
     * failure above provoked is not still standing when the good one is
     * checked. */
    nv_3d_model_detach();
    nv_3d_model_attach();

    /* Now the whole pipeline, in the order it has to be built. */
    f.push_at = 0;
    f.submitted_to = 0;

    bool built = true;
    built &= nv_3d_set_target(c, &f, subchannel, surface_gpu,
                              TD_SURF_W, TD_SURF_H, 0xCF /* A8R8G8B8 */);
    built &= nv_3d_set_viewport(c, &f, subchannel, 0, 0, TD_SURF_W, TD_SURF_H);

    /* Two programs, each at the FULL GPU VA of its shader header (Volta+).  In a
     * real draw these are the VAs the captured VS/FS SPH+microcode are loaded
     * at; here two distinct addresses in the code page exercise the encoding. */
    u64 vs_va = code_gpu, fs_va = code_gpu + 0x400;
    built &= nv_3d_set_program(c, &f, subchannel, NV_STAGE_VERTEX_B, true, vs_va, 8);
    built &= nv_3d_set_program(c, &f, subchannel, NV_STAGE_FRAGMENT, true, fs_va, 8);

    built &= nv_3d_set_vertex_stream(c, &f, subchannel, 0, vertices_gpu,
                                     7 * sizeof(float));
    built &= nv_3d_set_attribute(c, &f, subchannel, 0, 0, 0,
                                 NV9097_ATTRIB_32_32_32_32_FLOAT);
    built &= nv_3d_set_attribute(c, &f, subchannel, 1, 0, 16,
                                 NV9097_ATTRIB_32_32_FLOAT);

    if (!built) {
        kerr("nv-3d", "the pipeline state would not build");
        failures++;
    }

    built = nv_3d_clear(c, &f, subchannel, 0.1f, 0.2f, 0.3f, 1.0f);
    built &= nv_3d_draw(c, &f, subchannel, NV9097_PRIMITIVE_TRIANGLES, 0, 36);
    built &= nv_3d_report(c, &f, subchannel, report_gpu, 0x5EED);

    if (!built || !nv_fifo_submit(c, &f)) {
        kerr("nv-3d", "the drawing was not submitted");
        failures++;
    }

    if (nv_3d_model_complaint()[0]) {
        kerr("nv-3d", "the engine complained: %s", nv_3d_model_complaint());
        failures++;
    }
    if (nv_3d_model_draws() != 1) {
        kerr("nv-3d", "%d draw(s) reached the engine, expected 1",
             nv_3d_model_draws());
        failures++;
    }
    if (nv_3d_model_clears() != 1) {
        kerr("nv-3d", "%d clear(s) reached it, expected 1",
             nv_3d_model_clears());
        failures++;
    }
    if (nv_3d_model_count() != 36 ||
        nv_3d_model_primitive() != NV9097_PRIMITIVE_TRIANGLES) {
        kerr("nv-3d", "it drew %u of primitive %u, expected 36 triangles",
             nv_3d_model_count(), nv_3d_model_primitive());
        failures++;
    }
    if (nv_3d_model_target() != surface_gpu) {
        kerr("nv-3d", "the render target came out at %llx, expected %llx",
             (unsigned long long)nv_3d_model_target(),
             (unsigned long long)surface_gpu);
        failures++;
    }
    if (!nv_3d_model_viewport_transform()) {
        kerr("nv-3d", "the NDC->screen viewport transform was never enabled");
        failures++;
    }
    if (nv_3d_model_stage_addr(NV_STAGE_VERTEX_B) != code_gpu ||
        nv_3d_model_stage_addr(NV_STAGE_FRAGMENT) != code_gpu + 0x400) {
        kerr("nv-3d", "the program VAs came out wrong: vs %llx fs %llx",
             (unsigned long long)nv_3d_model_stage_addr(NV_STAGE_VERTEX_B),
             (unsigned long long)nv_3d_model_stage_addr(NV_STAGE_FRAGMENT));
        failures++;
    }
    if (nv_3d_model_reports() != 1) {
        kerr("nv-3d", "the engine was not asked to report when it finished");
        failures++;
    }

    /* Each of the ways a pipeline draws nothing while every individual
     * register looks correct.  These are the ones worth catching, because the
     * screen is black either way and only one of them is visible in a
     * register dump. */
    struct { const char *what; int stage; int stream; bool live; } holes[] = {
        { "no target counted live", -1, -1, false },
        { "no fragment program",    NV_STAGE_FRAGMENT, -1, true },
        { "no vertices",            -1, 0, true },
    };

    for (size_t i = 0; i < sizeof holes / sizeof holes[0]; i++) {
        nv_3d_model_detach();
        nv_3d_model_attach();
        f.push_at = 0;
        f.submitted_to = 0;

        nv_3d_set_target(c, &f, subchannel, surface_gpu, TD_SURF_W, TD_SURF_H,
                         0xCF);
        if (!holes[i].live) {
            /* Set, but not counted. */
            nv_push_begin(&f, subchannel, NV9097_RT_CONTROL, 1);
            nv_push_data(&f, 0);
            nv_push_end(&f);
        }
        nv_3d_set_viewport(c, &f, subchannel, 0, 0, TD_SURF_W, TD_SURF_H);
        nv_3d_set_program(c, &f, subchannel, NV_STAGE_VERTEX_B, true, code_gpu, 8);
        if (holes[i].stage != NV_STAGE_FRAGMENT)
            nv_3d_set_program(c, &f, subchannel, NV_STAGE_FRAGMENT, true,
                              code_gpu + 0x400, 8);
        if (holes[i].stream != 0) {
            nv_3d_set_vertex_stream(c, &f, subchannel, 0, vertices_gpu,
                                    7 * sizeof(float));
            nv_3d_set_attribute(c, &f, subchannel, 0, 0, 0,
                                NV9097_ATTRIB_32_32_32_32_FLOAT);
        }

        int before = nv_3d_model_refusals();
        nv_3d_draw(c, &f, subchannel, NV9097_PRIMITIVE_TRIANGLES, 0, 3);
        nv_fifo_submit(c, &f);

        if (nv_3d_model_refusals() == before || nv_3d_model_draws() != 0) {
            kerr("nv-3d", "a draw with %s was accepted", holes[i].what);
            failures++;
        }
    }

    /* And the things a state-setting call must refuse outright. */
    if (nv_3d_set_target(c, &f, subchannel, surface_gpu + 8, 64, 64, 0xCF)) {
        kerr("nv-3d", "a misaligned render target was accepted");
        failures++;
    }
    if (nv_3d_set_vertex_stream(c, &f, subchannel, 0, vertices_gpu, 0x2000)) {
        kerr("nv-3d", "a vertex stride too large for the field was accepted");
        failures++;
    }
    if (nv_3d_set_viewport(c, &f, subchannel, 0, 0, 0, 64)) {
        kerr("nv-3d", "a viewport of zero width was accepted");
        failures++;
    }

    nv_3d_model_detach();
    nv_fifo_model_detach();

    /* Two lines rather than one: a log line is 168 characters, and a message
     * that runs past it loses its end - which is exactly where the part worth
     * reading tends to be. */
    if (!failures) {
        kinfo("nv-3d", "the drawing pipeline is built and accepted: class %s "
                       "(%04x), a target, a viewport, two programs and 36 "
                       "triangles", nv_3d_class_name(class_3d), class_3d);
        kinfo("nv-3d", "and every incomplete pipeline refused: no live target, "
                       "no fragment program, no vertices");
    }
    return failures;
}

/* Real-hardware 3D triangle.  The method stream (above) is complete and verified
 * against clce97.h, but a draw needs the vertex + fragment shader MACHINE CODE,
 * which cannot be produced on this host (ptxas emits compute, not graphics; only
 * Mesa/NVK's NAK emits Blackwell VTG/PS microcode - see tools/shader_3d_triangle.h
 * for the capture recipe).  So until real shaders are supplied this SKIPS rather
 * than dispatch garbage: a wrong SPH/program faults the GR engine.  When
 * NV3D_SHADERS_PRESENT becomes 1, the full path (map render-target / vertices /
 * shader SPH+code / report buffer into the channel vaspace via nv_vmm_map exactly
 * as the copy path does, build the pipeline with the real program VAs + reg
 * counts, submit, and read back non-zero pixels) is wired in here.  Returns 0
 * (not a failure) when skipped - the triangle is method-complete, shader-blocked. */
int nv_3d_selftest_hw(void) {
#if NV3D_SHADERS_PRESENT
    /* TODO(shader-supply): with real SPH+microcode present, build the draw on the
     * proven nv_chan GR channel (golden context is already brought up there) and
     * verify rendered pixels.  Intentionally not written against fabricated bytes. */
    kwarn("nv-3d", "3D shaders present but the real-HW draw path is not wired yet");
    return 0;
#else
    kinfo("nv-3d", "3D method stream is complete and verified vs clce97.h "
                   "(viewport transform, SET_PIPELINE_SHADER/PROGRAM_ADDRESS, "
                   "report fence)");
    kinfo("nv-3d", "triangle SKIPPED: needs captured sm_120 vertex+fragment "
                   "shaders (Mesa/NVK build) - see tools/shader_3d_triangle.h; "
                   "method-complete, shader-blocked");
    return 0;
#endif
}
