/* Address-free fixed-function triangle records. Vertex transforms/clipping are
 * upstream; coverage, interpolation, sampling, depth and blending run on GPU.
 * This is not a programmable-shader or complete OpenGL implementation.
 * A batch uses one immutable texture and independent colour/depth allocations.
 */
#ifndef KESTREL_GPU3D_H
#define KESTREL_GPU3D_H

#define KG3D_MAX_COMMANDS 64u
#define KG3D_MAX_WORK 67108864ull
#define KG3D_LINE_WORK 6u
#define KG3D_DEPTH_TEST   1u
#define KG3D_DEPTH_WRITE  2u
#define KG3D_ALPHA_TEST   4u
#define KG3D_BLEND        8u
#define KG3D_TEXTURE      16u
#define KG3D_CLEAR_COLOUR 32u
#define KG3D_CLEAR_DEPTH  64u
#define KG3D_LINE         128u /* v[0:1], legacy inclusive DDA, at most 8192 steps */
#define KG3D_POINT        256u /* v[0], one pixel at truncated window coordinates */
#define KG3D_FLAGS        511u

#define KG3D_NEVER    0u
#define KG3D_LESS     1u
#define KG3D_EQUAL    2u
#define KG3D_LEQUAL   3u
#define KG3D_GREATER  4u
#define KG3D_NOTEQUAL 5u
#define KG3D_GEQUAL   6u
#define KG3D_ALWAYS   7u

#define KG3D_ZERO                0u
#define KG3D_ONE                 1u
#define KG3D_SRC_ALPHA           2u
#define KG3D_ONE_MINUS_SRC_ALPHA 3u
#define KG3D_DST_ALPHA           4u
#define KG3D_ONE_MINUS_DST_ALPHA 5u
#define KG3D_MODULATE 0u
#define KG3D_REPLACE  1u
#define KG3D_DECAL    2u
#define KG3D_REPEAT  0u
#define KG3D_CLAMP   1u
#define KG3D_NEAREST 0u
#define KG3D_LINEAR  1u

typedef struct {
    float x, y, z, inv_w; /* window coordinates and reciprocal clip w */
    float rgba[4], uv[2]; /* already divided by clip w */
} kg3d_vertex_t;

typedef struct {
    kg3d_vertex_t v[3];
    int x, y, width, height; /* viewport intersected with scissor */
    unsigned int flags, depth_func, alpha_func, blend_src, blend_dst;
    unsigned int tex_env, wrap_s, wrap_t, filter;
    float depth_bias, alpha_ref;
    unsigned int reserved[3];
} kg3d_command_t;
/* A clear uses v[0].rgba as un-divided RGBA and v[0].z as depth.
 * Colour storage is XRGB8888 (destination alpha is 1); depth is float32.
 * Texture storage is straight-alpha ARGB8888. Texture v grows upwards.
 * Raster coverage uses a 1/16-pixel grid and the top-left ownership rule.
 * Caller must clip vertices to finite x/y in [-1048576,1048576].
 */
#if defined(__cplusplus)
static_assert(sizeof(kg3d_vertex_t) == 40, "GPU 3D vertex ABI");
static_assert(sizeof(kg3d_command_t) == 192, "GPU 3D command ABI");
#else
_Static_assert(sizeof(kg3d_vertex_t) == 40, "GPU 3D vertex ABI");
_Static_assert(sizeof(kg3d_command_t) == 192, "GPU 3D command ABI");
#endif
#endif
