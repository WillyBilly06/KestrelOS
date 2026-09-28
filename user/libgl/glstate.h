/* glstate.h - the pipeline's own view of itself.
 *
 * Shared between the API layer, which turns calls into state changes and
 * vertices, and the rasteriser, which turns triangles into pixels.  Nothing
 * outside libgl includes this.
 */
#ifndef KESTREL_GLSTATE_H
#define KESTREL_GLSTATE_H

#include "GL.h"
#include "shader.h"
#include "../../include/kestrel/shader_raster.h"
#include "../libgui/gui.h"

/* ------------------------------------------------------------------- maths */

typedef struct { float x, y, z, w; } vec4_t;
typedef struct { float m[16]; } mat4_t;      /* column major, as OpenGL wants */

mat4_t mat4_identity(void);
mat4_t mat4_multiply(const mat4_t *a, const mat4_t *b);
vec4_t mat4_transform(const mat4_t *m, vec4_t v);
mat4_t mat4_translate(float x, float y, float z);
mat4_t mat4_scale(float x, float y, float z);
mat4_t mat4_rotate(float radians, float x, float y, float z);
mat4_t mat4_ortho(float l, float r, float b, float t, float n, float f);
mat4_t mat4_frustum(float l, float r, float b, float t, float n, float f);
/* The inverse transpose of the upper 3x3, which is what a normal transforms by
 * when the model matrix is not a pure rotation. */
mat4_t mat4_normal_matrix(const mat4_t *modelview);

static inline vec4_t vec4_make(float x, float y, float z, float w) {
    vec4_t v = { x, y, z, w };
    return v;
}

/* ---------------------------------------------------------------- textures */

#define GL_MAX_TEXTURES 64

typedef struct {
    bool      used;
    int       width, height;
    colour_t *texels;          /* 0xAARRGGBB, premultiplied by nothing */
    GLenum    min_filter, mag_filter;
    GLenum    wrap_s, wrap_t;
    surface_t *gpu_image;
    uint64_t gpu_used;
    uint64_t content_version; /* unique successful image upload; zero is uncacheable */
} gl_texture_t;

/* ---------------------------------------------------------------- lighting */

#define GL_MAX_LIGHTS 4

typedef struct {
    bool   enabled;
    float  ambient[4], diffuse[4], specular[4];
    float  position[4];        /* already in eye space when set */
    float  attenuation[3];     /* constant, linear, quadratic */
} gl_light_t;

typedef struct {
    float ambient[4], diffuse[4], specular[4], emission[4];
    float shininess;
} gl_material_t;

/* ----------------------------------------------------------------- vertices */

/* One vertex after the whole transform stage: clip position, the colour it
 * carries, its texture coordinates and the eye-space normal lighting used. */
typedef struct {
    vec4_t clip;
    bool fixed_pending; /* captured object-space fixed vertex, not raster-ready */
    float  r, g, b, a;
    float  s, t;
    /* What a vertex shader wrote for the fragment shader to read.  Unused by
     * the fixed-function path, which is why it costs nothing there. */
    float  varying[SR_VARYING_N][4];
} gl_vertex_t;

/* ------------------------------------------------------------------- arrays */

typedef struct {
    bool         enabled;
    const void  *pointer;
    int          size;         /* components per element */
    GLenum       type;
    int          stride;       /* bytes, 0 meaning tightly packed */
    bool         normalized;   /* generic integer attribute conversion */
} gl_array_t;

/* ------------------------------------------------------ shader objects */

#define GL_MAX_SHADER_OBJECTS  8
#define GL_MAX_PROGRAM_OBJECTS 4
#define GL_MAX_TEXTURE_UNITS   4

typedef struct {
    bool        used;
    bool        delete_pending;
    unsigned    attachments;
    sh_stage_t  stage;
    sh_shader_t shader;
    char        source[8192];
    int         source_len;
    bool        compiled;
} gl_shader_object_t;

typedef struct {
    bool         used;
    bool         delete_pending;
    GLuint       vertex, fragment;
    sh_program_t program;
    sh_shader_t *executable; /* private vertex + fragment copies, owned by program */
    float        uniform_value[SH_MAX_SYMBOLS * 2][16];
    bool         uniform_set[SH_MAX_SYMBOLS * 2];
    bool         linked;
} gl_program_object_t;

/* ------------------------------------------------------------------- state */

#define GL_MATRIX_DEPTH 32
#define GL_MAX_VERTICES 4096   /* one glBegin/glEnd batch */

typedef struct {
    /* Where the pixels go. */
    surface_t *colour;
    float     *depth;
    bool       gpu_target;
    int        depth_w, depth_h;

    /* Viewport and scissor, in surface pixels. */
    int   vp_x, vp_y, vp_w, vp_h;
    float depth_near, depth_far;
    /* OpenGL's clip space runs z from -1 to 1; Direct3D's and Vulkan's run it
     * from 0 to 1.  Mapping the wrong one halves the usable depth precision and
     * puts everything in front of the near plane, so the convention travels
     * with whichever API set it. */
    bool  clip_depth_zero_to_one;
    /* Vulkan's clip space also has y pointing down, where OpenGL's and
     * Direct3D's point up.  It is a separate question from the depth range:
     * Direct3D shares OpenGL's y and not its z. */
    bool  clip_y_down;
    bool  scissor_on;
    int   sc_x, sc_y, sc_w, sc_h;

    /* Matrices. */
    GLenum mode;
    mat4_t modelview[GL_MATRIX_DEPTH];
    mat4_t projection[GL_MATRIX_DEPTH];
    mat4_t texture[GL_MATRIX_DEPTH];
    int    mv_top, proj_top, tex_top;

    /* Fragment pipeline. */
    bool   depth_test, depth_write;
    GLenum depth_func;
    bool   blend;
    GLenum blend_src, blend_dst;
    bool   alpha_test;
    GLenum alpha_func;
    float  alpha_ref;
    bool   cull;
    GLenum cull_face, front_face;
    GLenum shade_model;
    float  poly_offset_units;

    /* Clear values. */
    float clear_r, clear_g, clear_b, clear_a;
    float clear_depth;

    /* Texturing. */
    bool          texture_2d;
    GLuint        bound_texture;
    /* Texture units, for shaders that sample more than one thing.  Unit zero
     * is the same texture the fixed-function path uses, so the two share. */
    int           active_unit;
    GLuint        unit_texture[GL_MAX_TEXTURE_UNITS];
    GLenum        tex_env;
    gl_texture_t  textures[GL_MAX_TEXTURES];

    /* Lighting. */
    bool          lighting;
    bool          normalize;
    bool          colour_material;
    gl_light_t    lights[GL_MAX_LIGHTS];
    gl_material_t material;
    float         light_model_ambient[4];

    /* Immediate mode. */
    GLenum      primitive;
    bool        in_begin;
    bool        gpu_immediate;
    bool        gpu_fixed_failed;
    bool        loop_segmented;
    gl_vertex_t loop_origin;
    gl_vertex_t verts[GL_MAX_VERTICES];
    int         nverts;

    /* The current vertex attributes, as glColor and friends set them. */
    float cur_colour[4];
    float cur_normal[3];
    float cur_texcoord[2];

    /* Vertex arrays. */
    gl_array_t array_vertex, array_normal, array_colour, array_texcoord;

    /* --------------------------------------------------- the programmable path
     *
     * When a program is in use the fixed-function transform, lighting and
     * texture-environment stages are all bypassed: the two shaders decide
     * everything between a vertex arriving and a pixel being written.
     */
    gl_shader_object_t  shader_object[GL_MAX_SHADER_OBJECTS];
    gl_program_object_t program_object[GL_MAX_PROGRAM_OBJECTS];
    GLuint              bound_program;

    gl_array_t attrib_array[SR_ATTRIB_N];
    float      attrib_constant[SR_ATTRIB_N][4];

    unsigned stat_shader_instructions;

    /* Diagnostics. */
    unsigned stat_triangles, stat_fragments;
    GLenum   error;
} gl_context_t;

extern gl_context_t g_gl;

/* Set up once, lazily, on the first call that needs it. */
void gl_context_init(void);

/* Turn one object-space vertex into a clip-space one, running the transform
 * and lighting stages. */
void gl_process_vertex(float x, float y, float z, float w, gl_vertex_t *out);
void gl_fixed_capture(float x,float y,float z,float w,gl_vertex_t *out);
bool gl_fixed_transform_gpu(gl_vertex_t *vertices,unsigned count);
/* Raw captured triangle lists: transform, lighting, clipping and raster on GPU.
 * No CPU vertex readback; caller must not replay a failed draw. */
bool gl_fixed_triangles_gpu(const gl_vertex_t *vertices,unsigned count);
void gl_fixed_gpu_release(void);

/* The rasteriser.  Takes clip-space vertices, does the clipping, the divide,
 * the viewport transform and the fill. */
void gl_raster_triangle(const gl_vertex_t *a, const gl_vertex_t *b, const gl_vertex_t *c);
void gl_raster_line(const gl_vertex_t *a, const gl_vertex_t *b);
void gl_raster_point(const gl_vertex_t *a, float size);

/* Sample the bound texture, with the current filter and wrap modes. */
void gl_sample(float s, float t, float *out_rgba);
/* The same, for a named texture: what a shader's sampler reaches. */
void gl_sample_texture(GLuint id, float s, float t, float *out_rgba);

/* --------------------------------------------------------- the shader path */

/* Whether a linked program is in use, in which case the rasteriser runs the
 * fragment shader instead of the texture environment. */
bool gl_program_active(void);
const sh_program_t *gl_current_program(void);

/* Uniforms are uploaded once per draw rather than once per pixel. */
void gl_shader_begin_draw(void);

/* Run the vertex shader over one element of the attribute arrays. */
void gl_shader_vertex(int index, gl_vertex_t *out);
bool gl_shader_snapshot(ksh_dispatch_t *vertex, kshr_job_t *fragment, float seed[SR_REGISTERS][4]);
bool gl_shader_vertex_input(int index, float *registers, unsigned lanes, unsigned lane);
bool gl_shader_immediate_input(float x, float y, float z, float w, float *registers, unsigned lanes, unsigned lane);
void gl_shader_immediate_begin_gpu(GLenum mode);
void gl_shader_immediate_vertex_gpu(float x, float y, float z, float w);
void gl_shader_immediate_end_gpu(void);
bool gl_shader_draw_gpu(GLenum mode, GLint first, GLsizei count, GLenum type, const void *indices);
bool gl_shader_gpu_active(void);
bool gl_shader_gpu_enqueue(const kshr_command_t *command);
bool gl_shader_gpu_flush(void);
void gl_shader_gpu_release(void);
rect_t gl_gpu_viewport(void);
void gl_gpu_fragment_state(kg3d_command_t *command,unsigned kind);

/* Run the fragment shader for one pixel.  The varyings arrive already
 * interpolated and perspective-corrected. */
bool gl_shader_fragment(const float varying[SR_VARYING_N][4],
                        float x, float y, float z, float w, float *rgba);

/* Flush whatever glBegin accumulated. */
void gl_flush_primitive(void);

void gl_set_error(GLenum e);
bool gl_gpu_flush(void);
bool gl_gpu_software(const char *reason);
bool gl_gpu_clear(GLbitfield mask);
bool gl_gpu_triangle(const kg3d_vertex_t *a, const kg3d_vertex_t *b, const kg3d_vertex_t *c);
bool gl_gpu_line(const kg3d_vertex_t *a, const kg3d_vertex_t *b);
bool gl_gpu_point(const kg3d_vertex_t *a);
bool gl_gpu_shader_primitive(const kg3d_vertex_t *a,const kg3d_vertex_t *b,const kg3d_vertex_t *c,
                             const float *av,const float *bv,const float *cv,unsigned kind);
void gl_gpu_texture_release(gl_texture_t *texture);

/* Set by the Direct3D and Vulkan layers; OpenGL leaves it false. */
void gl_set_clip_depth_zero_to_one(bool yes);
void gl_set_clip_y_down(bool yes);

#endif
