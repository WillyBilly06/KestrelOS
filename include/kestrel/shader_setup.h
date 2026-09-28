/* GPU-resident triangle-list setup. Config has no pointers/addresses. Native
 * launch arguments supply separately owned, non-aliasing buffers and sizes.
 * Used by the native resident triangle-list submission. Compilation does not
 * establish successful execution on the physical GPU. */
#ifndef KESTREL_SHADER_SETUP_H
#define KESTREL_SHADER_SETUP_H
#include "shader_raster.h"

#define KSHS_ABI 1u
#define KSHS_MAX_TRIANGLES (KSH_MAX_LANES/3u)
#define KSHS_CLIP_VERTICES 16u
#define KSHS_OUTPUT_TRIANGLES 8u
#define KSHS_CULL_FRONT 0u
#define KSHS_CULL_BACK 1u
#define KSHS_CULL_BOTH 2u

typedef struct {
    unsigned version,lanes,first_vertex,triangle_count;
    unsigned varying_count,framebuffer_width,framebuffer_height,reserved0;
    int viewport_x,viewport_y,viewport_w,viewport_h;
    int clip_x,clip_y,clip_w,clip_h; /* preintersected framebuffer/scissor/viewport */
    unsigned clip_zero_to_one,clip_y_down,cull_enable,cull_face;
    /* Bit r selects the original third/provoking vertex for varying r on all
     * three input vertices BEFORE clipping. Only active varying bits are legal;
     * zero preserves smooth interpolation. Reuses the former reserved1 word. */
    unsigned front_ccw,flat_varying_mask,reserved2,reserved3;
    float depth_near,depth_far;
    unsigned reserved4[2];
    kg3d_command_t state; /* triangle state; v[] and rectangle are ignored */
} kshs_config_t;

typedef struct { float clip[4],varying[SR_VARYING_N][4]; } kshs_vertex_t;
typedef struct { int x,y,width,height; } kshs_rect_t;
typedef struct {
    unsigned result,count; /* KSH_COMPLETE + count 0 is a fully clipped primitive */
    kshs_rect_t bounds[KSHS_OUTPUT_TRIANGLES];
} kshs_primitive_t;

/* Separate fenced compaction stage. Prefix and total are derived from the
 * complete immutable setup-result array; user-provided offsets are not used.
 * Each primitive owns its status record and a disjoint stable command span. */
typedef struct { unsigned result,count,first,total; } kshs_compact_status_t;

/* One kernel transaction owns setup, compaction and all raster windows. The
 * fragment commands[] are ignored, never accepted as GPU-generated data.
 * All five handles name distinct owner-scoped allocations, also distinct from
 * destination/depth/texture handles supplied in kg2d_request_t. */
typedef struct {
    unsigned long long registers,vertex_status,workspace,raster_scratch,raster_status;
    kshs_config_t setup;
    kshr_job_t fragment;
} kshs_submission_t;

/* Whole-operation admission, in addition to each raster window's work bound.
 * Exceeding either bound rejects the draw, never presents a truncated result. */
#define KSHS_MAX_RASTER_WORK (16ull*KSH_MAX_WORK)
#define KSHS_MAX_RASTER_DISPATCHES 4096u

#define KSHS_REGISTER_BYTES(lanes) ((unsigned long long)(lanes)*SR_REGISTERS*16u)
#define KSHS_VERTEX_STATUS_BYTES(lanes) ((unsigned long long)(lanes)*sizeof(ksh_status_t))
#define KSHS_COMMAND_BYTES(triangles) ((unsigned long long)(triangles)*KSHS_OUTPUT_TRIANGLES*sizeof(kshr_command_t))
#define KSHS_RESULT_BYTES(triangles) ((unsigned long long)(triangles)*sizeof(kshs_primitive_t))
#define KSHS_SCRATCH_BYTES(triangles) ((unsigned long long)(triangles)*2u*KSHS_CLIP_VERTICES*sizeof(kshs_vertex_t))
#define KSHS_COMPACT_STATUS_BYTES(triangles) ((unsigned long long)(triangles)*sizeof(kshs_compact_status_t))
#define KSHS_COMPACT_RECT_BYTES(commands) ((unsigned long long)(commands)*sizeof(kshs_rect_t))

/* A single owner-scoped workspace can contain these disjoint regions. The
 * config is read-only to both stages; neither shader receives the whole
 * allocation as writable scratch. Offsets are computed from bounded counts,
 * never supplied by an application as GPU pointers. Framebuffer, vertex
 * registers/status and raster scratch must be different allocations. */
typedef struct {
    unsigned long long config,clip,commands,primitives,packed,rects,compact;
    unsigned long long bytes;
} kshs_storage_layout_t;

static inline int kshs_storage_layout(unsigned triangles,kshs_storage_layout_t *out) {
    if (!out || !triangles || triangles>KSHS_MAX_TRIANGLES) return 0;
    unsigned long long n=0;
    out->config=n;n=(n+sizeof(kshs_config_t)+255u)&~255ull;
    out->clip=n;n=(n+KSHS_SCRATCH_BYTES(triangles)+255u)&~255ull;
    out->commands=n;n=(n+KSHS_COMMAND_BYTES(triangles)+255u)&~255ull;
    out->primitives=n;n=(n+KSHS_RESULT_BYTES(triangles)+255u)&~255ull;
    out->packed=n;n=(n+KSHS_COMMAND_BYTES(triangles)+255u)&~255ull;
    out->rects=n;n=(n+KSHS_COMPACT_RECT_BYTES(triangles*KSHS_OUTPUT_TRIANGLES)+255u)&~255ull;
    out->compact=n;n+=KSHS_COMPACT_STATUS_BYTES(triangles);
    out->bytes=(n+65535u)&~65535ull;
    return 1;
}

/* Outputs for primitive p are commands[p*8..p*8+count), in stable clipped-fan
 * order. On COMPLETE, inactive commands are unspecified and inactive bounds
 * are zero. On failure, ignore all bounds and command contents. Consume
 * count/bounds only after a fence and COMPLETE for EVERY primitive and input
 * vertex lane. Failure count is always zero, even after partial output writes.
 * Scratch is explicitly addressed global memory, never CUDA local/shared.
 * Kernel must initialize result records to PENDING before launch, validate all
 * config fields/capacities/ownership/aliases, and retain allocations until fence.
 */
#if defined(__cplusplus)
static_assert(sizeof(kshs_config_t)==304,"setup config ABI");
static_assert(sizeof(kshs_vertex_t)==144,"setup clip vertex ABI");
static_assert(sizeof(kshs_primitive_t)==136,"setup result ABI");
static_assert(sizeof(kshs_compact_status_t)==16,"setup compaction status ABI");
static_assert(sizeof(kshs_submission_t)==56984,"resident geometry submission ABI");
#else
_Static_assert(sizeof(kshs_config_t)==304,"setup config ABI");
_Static_assert(sizeof(kshs_vertex_t)==144,"setup clip vertex ABI");
_Static_assert(sizeof(kshs_primitive_t)==136,"setup result ABI");
_Static_assert(sizeof(kshs_compact_status_t)==16,"setup compaction status ABI");
_Static_assert(sizeof(kshs_submission_t)==56984,"resident geometry submission ABI");
#endif
#endif
