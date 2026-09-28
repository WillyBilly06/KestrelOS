/* gl.c - the OpenGL state machine and the transform stage.
 *
 * The API side of the pipeline: calls become state, vertices are transformed
 * and lit, and complete primitives are handed to the rasteriser.  Nothing here
 * touches a pixel.
 */
#include "glstate.h"
#include "../libc/math.h"

gl_context_t g_gl;
static bool  initialised;

static void reset_arrays(gl_array_t *a) {
    a->enabled = false;
    a->pointer = NULL;
    a->size = 0;
    a->type = GL_FLOAT;
    a->stride = 0;
}

void gl_set_clip_depth_zero_to_one(bool yes) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    g_gl.clip_depth_zero_to_one = yes;
}

void gl_set_clip_y_down(bool yes) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    g_gl.clip_y_down = yes;
}

void gl_set_error(GLenum e) {
    /* OpenGL keeps the first error until it is read, not the last. */
    if (g_gl.error == GL_NO_ERROR) g_gl.error = e;
}

void gl_context_init(void) {
    if (initialised) return;
    initialised = true;

    memset(&g_gl, 0, sizeof g_gl);

    g_gl.mode = GL_MODELVIEW;
    g_gl.modelview[0] = mat4_identity();
    g_gl.projection[0] = mat4_identity();
    g_gl.texture[0] = mat4_identity();

    g_gl.depth_near = 0.0f;
    g_gl.depth_far = 1.0f;
    g_gl.depth_test = false;
    g_gl.depth_write = true;
    g_gl.depth_func = GL_LESS;
    g_gl.clear_depth = 1.0f;

    g_gl.blend_src = GL_ONE;
    g_gl.blend_dst = GL_ZERO;
    g_gl.alpha_func = GL_ALWAYS;

    g_gl.cull_face = GL_BACK;
    g_gl.front_face = GL_CCW;
    g_gl.shade_model = GL_SMOOTH;
    g_gl.tex_env = GL_MODULATE;

    g_gl.cur_colour[0] = g_gl.cur_colour[1] = 1.0f;
    g_gl.cur_colour[2] = g_gl.cur_colour[3] = 1.0f;
    g_gl.cur_normal[2] = 1.0f;

    /* The defaults OpenGL specifies for light zero and for materials. */
    for (int i = 0; i < GL_MAX_LIGHTS; i++) {
        gl_light_t *l = &g_gl.lights[i];
        l->diffuse[3] = l->specular[3] = l->ambient[3] = 1.0f;
        if (i == 0) {
            l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = 1.0f;
            l->specular[0] = l->specular[1] = l->specular[2] = 1.0f;
        }
        l->position[2] = 1.0f;
        l->attenuation[0] = 1.0f;
    }
    g_gl.material.ambient[0] = g_gl.material.ambient[1] = g_gl.material.ambient[2] = 0.2f;
    g_gl.material.ambient[3] = 1.0f;
    g_gl.material.diffuse[0] = g_gl.material.diffuse[1] = g_gl.material.diffuse[2] = 0.8f;
    g_gl.material.diffuse[3] = 1.0f;
    g_gl.material.specular[3] = 1.0f;
    g_gl.material.emission[3] = 1.0f;
    g_gl.material.shininess = 0.0f;
    g_gl.light_model_ambient[0] = g_gl.light_model_ambient[1] = 0.2f;
    g_gl.light_model_ambient[2] = 0.2f;
    g_gl.light_model_ambient[3] = 1.0f;

    reset_arrays(&g_gl.array_vertex);
    reset_arrays(&g_gl.array_normal);
    reset_arrays(&g_gl.array_colour);
    reset_arrays(&g_gl.array_texcoord);
    for(unsigned i=0;i<SR_ATTRIB_N;i++)g_gl.attrib_constant[i][3]=1;
}

/* ---------------------------------------------------------------- the target */

void glSetTarget(void *colour_surface) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    surface_t *s = colour_surface;
    if(!gl_gpu_flush())return;
    g_gl.colour = s;
    g_gl.gpu_target = s && s->gpu;
    if (!s) {gl_shader_gpu_release();gl_fixed_gpu_release();return;}

    /* Grow the depth buffer to match.  It is kept separate from the colour
     * surface because a window's surface belongs to the toolkit and may be
     * reallocated on a resize. */
    if (s->gpu) {
        free(g_gl.depth);g_gl.depth=NULL;g_gl.depth_w=g_gl.depth_h=0;
    } else if (g_gl.depth_w != s->width || g_gl.depth_h != s->height) {
        free(g_gl.depth);
        g_gl.depth = malloc((size_t)s->width * s->height * sizeof(float));
        g_gl.depth_w = g_gl.depth ? s->width : 0;
        g_gl.depth_h = g_gl.depth ? s->height : 0;
        if (!g_gl.depth) gl_set_error(GL_OUT_OF_MEMORY);
    }

    if (!g_gl.vp_w || !g_gl.vp_h) {
        g_gl.vp_x = 0; g_gl.vp_y = 0;
        g_gl.vp_w = s->width; g_gl.vp_h = s->height;
    }
}

void glGetTargetSize(int *width, int *height) {
    if (width)  *width  = g_gl.colour ? g_gl.colour->width : 0;
    if (height) *height = g_gl.colour ? g_gl.colour->height : 0;
}

/* --------------------------------------------------------------- the basics */

void glClear(GLbitfield mask) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    surface_t *s = g_gl.colour;
    if(gl_gpu_clear(mask))return;

    if ((mask & GL_COLOR_BUFFER_BIT) && s) {
        int r = (int)(g_gl.clear_r * 255.0f + 0.5f);
        int g = (int)(g_gl.clear_g * 255.0f + 0.5f);
        int b = (int)(g_gl.clear_b * 255.0f + 0.5f);
        colour_t c = RGB(r < 0 ? 0 : r > 255 ? 255 : r,
                         g < 0 ? 0 : g > 255 ? 255 : g,
                         b < 0 ? 0 : b > 255 ? 255 : b);

        int x0 = g_gl.vp_x, y0 = g_gl.vp_y;
        int x1 = g_gl.vp_x + g_gl.vp_w, y1 = g_gl.vp_y + g_gl.vp_h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > s->width)  x1 = s->width;
        if (y1 > s->height) y1 = s->height;
        for (int y = y0; y < y1; y++) {
            colour_t *row = &s->pixels[(size_t)y * s->stride];
            for (int x = x0; x < x1; x++) row[x] = c;
        }
    }

    if ((mask & GL_DEPTH_BUFFER_BIT) && g_gl.depth) {
        size_t n = (size_t)g_gl.depth_w * g_gl.depth_h;
        for (size_t i = 0; i < n; i++) g_gl.depth[i] = g_gl.clear_depth;
    }
}

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    gl_context_init();
    g_gl.clear_r = r; g_gl.clear_g = g; g_gl.clear_b = b; g_gl.clear_a = a;
}

void glClearDepth(GLdouble depth) { gl_context_init(); g_gl.clear_depth = (float)depth; }

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (w < 0 || h < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    g_gl.vp_x = x; g_gl.vp_y = y; g_gl.vp_w = w; g_gl.vp_h = h;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (w < 0 || h < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    g_gl.sc_x = x; g_gl.sc_y = y; g_gl.sc_w = w; g_gl.sc_h = h;
}

void glDepthRange(GLdouble n, GLdouble f) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    g_gl.depth_near = (float)n;
    g_gl.depth_far = (float)f;
}

static bool *capability(GLenum cap) {
    switch (cap) {
    case GL_DEPTH_TEST:     return &g_gl.depth_test;
    case GL_CULL_FACE:      return &g_gl.cull;
    case GL_BLEND:          return &g_gl.blend;
    case GL_TEXTURE_2D:     return &g_gl.texture_2d;
    case GL_LIGHTING:       return &g_gl.lighting;
    case GL_NORMALIZE:      return &g_gl.normalize;
    case GL_SCISSOR_TEST:   return &g_gl.scissor_on;
    case GL_ALPHA_TEST:     return &g_gl.alpha_test;
    case GL_COLOR_MATERIAL: return &g_gl.colour_material;
    default: break;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + GL_MAX_LIGHTS)
        return &g_gl.lights[cap - GL_LIGHT0].enabled;
    return NULL;
}

void glEnable(GLenum cap) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    bool *p = capability(cap);
    /* Fog and dither are accepted and ignored: a program that enables them
     * should not fail, and neither changes what this pipeline produces. */
    if (!p) { if (cap != GL_FOG && cap != GL_DITHER) gl_set_error(GL_INVALID_ENUM); return; }
    *p = true;
}

void glDisable(GLenum cap) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    bool *p = capability(cap);
    if (!p) { if (cap != GL_FOG && cap != GL_DITHER) gl_set_error(GL_INVALID_ENUM); return; }
    *p = false;
}

GLboolean glIsEnabled(GLenum cap) {
    gl_context_init();
    bool *p = capability(cap);
    return (p && *p) ? GL_TRUE : GL_FALSE;
}

/* ------------------------------------------------------------------ matrices */

static mat4_t *stack_top(void) {
    switch (g_gl.mode) {
    case GL_PROJECTION: return &g_gl.projection[g_gl.proj_top];
    case GL_TEXTURE:    return &g_gl.texture[g_gl.tex_top];
    default:            return &g_gl.modelview[g_gl.mv_top];
    }
}

static int *stack_depth(void) {
    switch (g_gl.mode) {
    case GL_PROJECTION: return &g_gl.proj_top;
    case GL_TEXTURE:    return &g_gl.tex_top;
    default:            return &g_gl.mv_top;
    }
}

static mat4_t *stack_base(void) {
    switch (g_gl.mode) {
    case GL_PROJECTION: return g_gl.projection;
    case GL_TEXTURE:    return g_gl.texture;
    default:            return g_gl.modelview;
    }
}

void glMatrixMode(GLenum mode) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    g_gl.clip_depth_zero_to_one = false;
    g_gl.clip_y_down = false;
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    g_gl.mode = mode;
}

void glLoadIdentity(void) { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} *stack_top() = mat4_identity(); }

void glLoadMatrixf(const GLfloat *m) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (!m) return;
    memcpy(stack_top()->m, m, sizeof(float) * 16);
}

void glMultMatrixf(const GLfloat *m) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (!m) return;
    mat4_t rhs;
    memcpy(rhs.m, m, sizeof rhs.m);
    mat4_t *top = stack_top();
    *top = mat4_multiply(top, &rhs);
}

void glPushMatrix(void) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    int *depth = stack_depth();
    if (*depth + 1 >= GL_MATRIX_DEPTH) { gl_set_error(GL_STACK_OVERFLOW); return; }
    mat4_t *base = stack_base();
    base[*depth + 1] = base[*depth];
    (*depth)++;
}

void glPopMatrix(void) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    int *depth = stack_depth();
    if (*depth == 0) { gl_set_error(GL_STACK_UNDERFLOW); return; }
    (*depth)--;
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    mat4_t t = mat4_translate(x, y, z);
    mat4_t *top = stack_top();
    *top = mat4_multiply(top, &t);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    mat4_t t = mat4_scale(x, y, z);
    mat4_t *top = stack_top();
    *top = mat4_multiply(top, &t);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    mat4_t t = mat4_rotate(radiansf(angle), x, y, z);
    mat4_t *top = stack_top();
    *top = mat4_multiply(top, &t);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    mat4_t o = mat4_ortho((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
    mat4_t *top = stack_top();
    *top = mat4_multiply(top, &o);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    mat4_t p = mat4_frustum((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
    mat4_t *top = stack_top();
    *top = mat4_multiply(top, &p);
}

void glGetFloatv(GLenum pname, GLfloat *out) {
    gl_context_init();
    if (!out) return;
    switch (pname) {
    case GL_MODELVIEW:  memcpy(out, g_gl.modelview[g_gl.mv_top].m, sizeof(float) * 16); break;
    case GL_PROJECTION: memcpy(out, g_gl.projection[g_gl.proj_top].m, sizeof(float) * 16); break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

/* ------------------------------------------------------------ fragment state */

void glDepthFunc(GLenum f)   { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} g_gl.depth_func = f; }
void glDepthMask(GLboolean f){ gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} g_gl.depth_write = f != GL_FALSE; }
void glCullFace(GLenum m)    { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} g_gl.cull_face = m; }
void glFrontFace(GLenum m)   { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} g_gl.front_face = m; }
void glShadeModel(GLenum m)  { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} g_gl.shade_model = m; }
void glLineWidth(GLfloat w)  { (void)w; gl_context_init(); }
void glPointSize(GLfloat sz) { (void)sz; gl_context_init(); }

void glBlendFunc(GLenum src, GLenum dst) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    g_gl.blend_src = src;
    g_gl.blend_dst = dst;
}

void glAlphaFunc(GLenum func, GLclampf ref) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    g_gl.alpha_func = func;
    g_gl.alpha_ref = ref;
}

void glPolygonOffset(GLfloat factor, GLfloat units) {
    (void)factor;
    gl_context_init();
    /* Only the constant term is implemented; the slope term needs the depth
     * gradient, which this rasteriser does not carry out of the fill. */
    g_gl.poly_offset_units = units * 1e-6f;
}

/* -------------------------------------------------------------- the vertex */

/* Object space to clip space, plus lighting, which OpenGL evaluates in eye
 * space - after the modelview matrix and before the projection. */
void gl_process_vertex(float x, float y, float z, float w, gl_vertex_t *out) {
    out->fixed_pending=false;
    const mat4_t *mv = &g_gl.modelview[g_gl.mv_top];
    const mat4_t *proj = &g_gl.projection[g_gl.proj_top];

    vec4_t object = vec4_make(x, y, z, w);
    vec4_t eye = mat4_transform(mv, object);
    out->clip = mat4_transform(proj, eye);

    out->s = g_gl.cur_texcoord[0];
    out->t = g_gl.cur_texcoord[1];

    if (!g_gl.lighting) {
        out->r = g_gl.cur_colour[0];
        out->g = g_gl.cur_colour[1];
        out->b = g_gl.cur_colour[2];
        out->a = g_gl.cur_colour[3];
        return;
    }

    /* The normal goes to eye space by the inverse transpose, not the modelview
     * itself, so a non-uniform scale does not tilt the lighting. */
    mat4_t nm = mat4_normal_matrix(mv);
    vec4_t n4 = mat4_transform(&nm, vec4_make(g_gl.cur_normal[0],
                                              g_gl.cur_normal[1],
                                              g_gl.cur_normal[2], 0.0f));
    float nx = n4.x, ny = n4.y, nz = n4.z;
    float nl = sqrtf(nx*nx + ny*ny + nz*nz);
    if (nl > 1e-8f) { nx /= nl; ny /= nl; nz /= nl; }

    const gl_material_t *mat = &g_gl.material;
    /* glColorMaterial in its usual mode: the current colour stands in for the
     * ambient and diffuse reflectance, which is how most programs use it. */
    const float *diffuse = g_gl.colour_material ? g_gl.cur_colour : mat->diffuse;
    const float *ambient = g_gl.colour_material ? g_gl.cur_colour : mat->ambient;

    float acc[3];
    for (int i = 0; i < 3; i++)
        acc[i] = mat->emission[i] + ambient[i] * g_gl.light_model_ambient[i];

    for (int li = 0; li < GL_MAX_LIGHTS; li++) {
        const gl_light_t *l = &g_gl.lights[li];
        if (!l->enabled) continue;

        /* A w of zero means a directional light: the position is a direction
         * and every vertex sees it from the same angle. */
        float lx, ly, lz, attenuation = 1.0f;
        if (l->position[3] == 0.0f) {
            lx = l->position[0]; ly = l->position[1]; lz = l->position[2];
        } else {
            lx = l->position[0] - eye.x;
            ly = l->position[1] - eye.y;
            lz = l->position[2] - eye.z;
            float d = sqrtf(lx*lx + ly*ly + lz*lz);
            if (d > 1e-8f) {
                float denom = l->attenuation[0] + l->attenuation[1] * d +
                              l->attenuation[2] * d * d;
                if (denom > 1e-8f) attenuation = 1.0f / denom;
            }
        }
        float ll = sqrtf(lx*lx + ly*ly + lz*lz);
        if (ll > 1e-8f) { lx /= ll; ly /= ll; lz /= ll; }

        float ndotl = nx*lx + ny*ly + nz*lz;
        if (ndotl < 0.0f) ndotl = 0.0f;

        for (int i = 0; i < 3; i++)
            acc[i] += attenuation * (ambient[i] * l->ambient[i] +
                                     diffuse[i] * l->diffuse[i] * ndotl);

        if (mat->shininess > 0.0f && ndotl > 0.0f) {
            /* The halfway vector with an eye at infinity, which is what
             * OpenGL's default local-viewer-off setting means. */
            float hx = lx, hy = ly, hz = lz + 1.0f;
            float hl = sqrtf(hx*hx + hy*hy + hz*hz);
            if (hl > 1e-8f) { hx /= hl; hy /= hl; hz /= hl; }
            float ndoth = nx*hx + ny*hy + nz*hz;
            if (ndoth > 0.0f) {
                float spec = powf(ndoth, mat->shininess);
                for (int i = 0; i < 3; i++)
                    acc[i] += attenuation * mat->specular[i] * l->specular[i] * spec;
            }
        }
    }

    out->r = acc[0] > 1.0f ? 1.0f : acc[0];
    out->g = acc[1] > 1.0f ? 1.0f : acc[1];
    out->b = acc[2] > 1.0f ? 1.0f : acc[2];
    out->a = diffuse[3];
}

/* ------------------------------------------------------------ immediate mode */

void glBegin(GLenum mode) {
    gl_context_init();
    if (g_gl.in_begin) { gl_set_error(GL_INVALID_OPERATION); return; }
    if(mode>GL_POLYGON){gl_set_error(GL_INVALID_ENUM);return;}
    g_gl.in_begin = true;
    g_gl.primitive = mode;
    g_gl.nverts = 0;
    g_gl.gpu_fixed_failed=false;
    g_gl.loop_segmented=false;
    g_gl.gpu_immediate=g_gl.colour && g_gl.colour->gpu && gl_program_active();
    if(g_gl.gpu_immediate)gl_shader_immediate_begin_gpu(mode);
}

void glEnd(void) {
    gl_context_init();
    if (!g_gl.in_begin) { gl_set_error(GL_INVALID_OPERATION); return; }
    if(g_gl.gpu_immediate)gl_shader_immediate_end_gpu();
    else gl_flush_primitive();
    g_gl.in_begin = false;
    g_gl.gpu_immediate = false;
    g_gl.nverts = 0;
}

/* Under flat shading, every vertex of a face takes the colour of its last one,
 * which is the convention OpenGL specifies. */
static void flatten(gl_vertex_t *a, gl_vertex_t *b, gl_vertex_t *c) {
    if (g_gl.shade_model != GL_FLAT) return;
    a->r = b->r = c->r;
    a->g = b->g = c->g;
    a->b = b->b = c->b;
    a->a = b->a = c->a;
}

static void tri(gl_vertex_t a, gl_vertex_t b, gl_vertex_t c) {
    flatten(&a, &b, &c);
    gl_raster_triangle(&a, &b, &c);
}

void gl_flush_primitive(void) {
    gl_vertex_t *v = g_gl.verts;
    int n = g_gl.nverts;
    if(g_gl.gpu_immediate)return; /* streaming shader path owns this primitive */
    if(g_gl.colour && g_gl.colour->gpu && !gl_program_active()) {
        if(g_gl.primitive==GL_TRIANGLES) {
            /* Capacity flushes already preserve the one trailing raw vertex.
             * At End, incomplete triangles have no output. The resident path
             * owns all transforms/setup/raster and failure invalidation; never
             * feed its results into the CPU clipper or replay after failure. */
            unsigned count=(unsigned)n-(unsigned)n%3u;
            if(count)(void)gl_fixed_triangles_gpu(v,count);
            return;
        }
        if(!gl_fixed_transform_gpu(v,(unsigned)n))return;
    }

    switch (g_gl.primitive) {
    case GL_POINTS:
        for (int i = 0; i < n; i++) gl_raster_point(&v[i], 1.0f);
        break;

    case GL_LINES:
        for (int i = 0; i + 1 < n; i += 2) gl_raster_line(&v[i], &v[i + 1]);
        break;

    case GL_LINE_STRIP:
        for (int i = 0; i + 1 < n; i++) gl_raster_line(&v[i], &v[i + 1]);
        break;

    case GL_LINE_LOOP:
        for (int i = 0; i + 1 < n; i++) gl_raster_line(&v[i], &v[i + 1]);
        if(g_gl.loop_segmented && n)gl_raster_line(&v[n-1],&g_gl.loop_origin);
        else if(n>=2)gl_raster_line(&v[n-1],&v[0]);
        break;

    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) tri(v[i], v[i + 1], v[i + 2]);
        break;

    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            /* Every other triangle is wound the other way, so the winding is
             * swapped back to keep face culling consistent across the strip. */
            if (i & 1) tri(v[i + 1], v[i], v[i + 2]);
            else       tri(v[i], v[i + 1], v[i + 2]);
        }
        break;

    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (int i = 1; i + 1 < n; i++) tri(v[0], v[i], v[i + 1]);
        break;

    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            tri(v[i], v[i + 1], v[i + 2]);
            tri(v[i], v[i + 2], v[i + 3]);
        }
        break;

    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            tri(v[i], v[i + 1], v[i + 3]);
            tri(v[i], v[i + 3], v[i + 2]);
        }
        break;

    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
    gl_gpu_flush();
}

static void push_vertex(float x, float y, float z, float w) {
    if (!g_gl.in_begin) { gl_set_error(GL_INVALID_OPERATION); return; }
    if(g_gl.gpu_immediate){gl_shader_immediate_vertex_gpu(x,y,z,w);return;}
    if(g_gl.gpu_fixed_failed)return;
    if (g_gl.nverts >= GL_MAX_VERTICES) {
        /* Rather than dropping the rest of a long primitive, draw what is
         * there and carry on.  For strips and fans the join needs the last
         * vertices kept, so those are moved down instead of discarded. */
        GLenum mode=g_gl.primitive;
        gl_vertex_t carry;
        if(mode==GL_TRIANGLES){carry=g_gl.verts[g_gl.nverts-1];g_gl.nverts--;}
        /* A capacity flush is not the end of a loop. Close it once, at End,
         * against the first retired vertex rather than the last chunk's first. */
        if(mode==GL_LINE_LOOP)g_gl.primitive=GL_LINE_STRIP;
        gl_flush_primitive();
        g_gl.primitive=mode;
        if(g_gl.gpu_fixed_failed)return;
        if(mode==GL_LINE_LOOP && !g_gl.loop_segmented){
            g_gl.loop_origin=g_gl.verts[0];g_gl.loop_segmented=true;
        }
        switch (g_gl.primitive) {
        case GL_TRIANGLES:
            g_gl.verts[0]=carry;g_gl.nverts=1;
            break;
        case GL_TRIANGLE_STRIP:
        case GL_QUAD_STRIP:
            g_gl.verts[0] = g_gl.verts[g_gl.nverts - 2];
            g_gl.verts[1] = g_gl.verts[g_gl.nverts - 1];
            g_gl.nverts = 2;
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            g_gl.verts[1] = g_gl.verts[g_gl.nverts - 1];
            g_gl.nverts = 2;
            break;
        case GL_LINE_STRIP:
        case GL_LINE_LOOP:
            g_gl.verts[0] = g_gl.verts[g_gl.nverts - 1];
            g_gl.nverts = 1;
            break;
        default:
            g_gl.nverts = 0;
            break;
        }
    }
    if(g_gl.colour && g_gl.colour->gpu && !gl_program_active())
        gl_fixed_capture(x,y,z,w,&g_gl.verts[g_gl.nverts++]);
    else gl_process_vertex(x, y, z, w, &g_gl.verts[g_gl.nverts++]);
}

void glVertex2f(GLfloat x, GLfloat y)                       { push_vertex(x, y, 0.0f, 1.0f); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z)            { push_vertex(x, y, z, 1.0f); }
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { push_vertex(x, y, z, w); }
void glVertex3fv(const GLfloat *v)  { if (v) push_vertex(v[0], v[1], v[2], 1.0f); }
void glVertex2i(GLint x, GLint y)   { push_vertex((float)x, (float)y, 0.0f, 1.0f); }
void glVertex3i(GLint x, GLint y, GLint z) { push_vertex((float)x, (float)y, (float)z, 1.0f); }

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    gl_context_init();
    g_gl.cur_colour[0] = r; g_gl.cur_colour[1] = g;
    g_gl.cur_colour[2] = b; g_gl.cur_colour[3] = a;
}
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1.0f); }
void glColor3fv(const GLfloat *v) { if (v) glColor4f(v[0], v[1], v[2], 1.0f); }
void glColor4fv(const GLfloat *v) { if (v) glColor4f(v[0], v[1], v[2], v[3]); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) {
    glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
    glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void glNormal3f(GLfloat x, GLfloat y, GLfloat z) {
    gl_context_init();
    if (g_gl.normalize && !(g_gl.colour && g_gl.colour->gpu)) {
        float l = sqrtf(x*x + y*y + z*z);
        if (l > 1e-8f) { x /= l; y /= l; z /= l; }
    }
    g_gl.cur_normal[0] = x; g_gl.cur_normal[1] = y; g_gl.cur_normal[2] = z;
}
void glNormal3fv(const GLfloat *v) { if (v) glNormal3f(v[0], v[1], v[2]); }

void glTexCoord2f(GLfloat s, GLfloat t) {
    gl_context_init();
    g_gl.cur_texcoord[0] = s;
    g_gl.cur_texcoord[1] = t;
}
void glTexCoord2fv(const GLfloat *v) { if (v) glTexCoord2f(v[0], v[1]); }

/* -------------------------------------------------------------- lighting */

static float *light_field(gl_light_t *l, GLenum pname) {
    switch (pname) {
    case GL_AMBIENT:  return l->ambient;
    case GL_DIFFUSE:  return l->diffuse;
    case GL_SPECULAR: return l->specular;
    case GL_POSITION: return l->position;
    default:          return NULL;
    }
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + GL_MAX_LIGHTS || !params) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_light_t *l = &g_gl.lights[light - GL_LIGHT0];
    float *field = light_field(l, pname);
    if (!field) { gl_set_error(GL_INVALID_ENUM); return; }

    if (pname == GL_POSITION) {
        /* A light's position is transformed by the modelview matrix in force
         * when it is set, and stays put in eye space from then on. */
        const mat4_t *mv = &g_gl.modelview[g_gl.mv_top];
        vec4_t p = mat4_transform(mv, vec4_make(params[0], params[1],
                                                params[2], params[3]));
        field[0] = p.x; field[1] = p.y; field[2] = p.z; field[3] = p.w;
    } else {
        for (int i = 0; i < 4; i++) field[i] = params[i];
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + GL_MAX_LIGHTS) return;
    gl_light_t *l = &g_gl.lights[light - GL_LIGHT0];
    /* The three attenuation coefficients, in the order OpenGL numbers them. */
    if (pname == 0x1207) l->attenuation[1] = param;        /* linear    */
    else if (pname == 0x1208) l->attenuation[2] = param;   /* quadratic */
    else if (pname == 0x1209) l->attenuation[0] = param;   /* constant  */
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    gl_context_init();
    (void)face;                     /* one material, applied to both sides */
    if (!params) return;
    gl_material_t *m = &g_gl.material;
    switch (pname) {
    case GL_AMBIENT:  memcpy(m->ambient, params, sizeof(float) * 4); break;
    case GL_DIFFUSE:  memcpy(m->diffuse, params, sizeof(float) * 4); break;
    case GL_SPECULAR: memcpy(m->specular, params, sizeof(float) * 4); break;
    case GL_EMISSION: memcpy(m->emission, params, sizeof(float) * 4); break;
    case GL_SHININESS: m->shininess = params[0]; break;
    case GL_AMBIENT_AND_DIFFUSE:
        memcpy(m->ambient, params, sizeof(float) * 4);
        memcpy(m->diffuse, params, sizeof(float) * 4);
        break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    float v[4] = { param, param, param, param };
    glMaterialfv(face, pname, v);
}

void glLightModelfv(GLenum pname, const GLfloat *params) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (pname == GL_LIGHT_MODEL_AMBIENT && params)
        memcpy(g_gl.light_model_ambient, params, sizeof(float) * 4);
}

/* --------------------------------------------------------------- odds, ends */

void glFinish(void) { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} gl_gpu_flush();gui_gpu_flush(); }
void glFlush(void)  { gl_context_init(); if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;} gl_gpu_flush();gui_gpu_flush(); }

GLenum glGetError(void) {
    gl_context_init();
    GLenum e = g_gl.error;
    g_gl.error = GL_NO_ERROR;
    return e;
}

const GLubyte *glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"KestrelOS";
    case GL_RENDERER: return (const GLubyte *)(g_gl.gpu_target ?
        "Kestrel native GPU raster and GLSL bytecode" : "Kestrel software rasteriser");
    case GL_VERSION:  return (const GLubyte *)"1.2 (Kestrel subset)";
    case GL_EXTENSIONS: return (const GLubyte *)"";
    default: gl_set_error(GL_INVALID_ENUM); return (const GLubyte *)"";
    }
}

void glGetStats(unsigned *triangles, unsigned *fragments) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    gl_gpu_flush(); /* stats do not race queued application primitives */
    if (triangles) *triangles = g_gl.stat_triangles;
    if (fragments) *fragments = g_gl.stat_fragments;
    g_gl.stat_triangles = 0;
    g_gl.stat_fragments = 0;
}

/* --------------------------------------------------------------- GLU-ish */

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble n, GLdouble f) {
    float half = (float)(fovy * 0.5 * (M_PI / 180.0));
    float c = cosf(half), s = sinf(half);
    if (c < 1e-8f) return;

    /* The near plane's half-height is near * tan(fovy/2).  Using the cotangent
     * instead scales the whole scene by cot squared, which looks like a working
     * renderer with an oddly distant camera rather than like a bug. */
    float top = (float)n * (s / c);
    float right = top * (float)aspect;
    glFrustum(-right, right, -top, top, n, f);
}

void gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t) {
    glOrtho(l, r, b, t, -1.0, 1.0);
}

void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez,
               GLdouble cx, GLdouble cy, GLdouble cz,
               GLdouble ux, GLdouble uy, GLdouble uz) {
    float fx = (float)(cx - ex), fy = (float)(cy - ey), fz = (float)(cz - ez);
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl < 1e-8f) return;
    fx /= fl; fy /= fl; fz /= fl;

    float upx = (float)ux, upy = (float)uy, upz = (float)uz;
    float ul = sqrtf(upx*upx + upy*upy + upz*upz);
    if (ul > 1e-8f) { upx /= ul; upy /= ul; upz /= ul; }

    /* side = forward x up, then a true up = side x forward, so the basis is
     * orthogonal even when the caller's up vector was not perpendicular. */
    float sx = fy*upz - fz*upy;
    float sy = fz*upx - fx*upz;
    float sz = fx*upy - fy*upx;
    float sl = sqrtf(sx*sx + sy*sy + sz*sz);
    if (sl < 1e-8f) return;
    sx /= sl; sy /= sl; sz /= sl;

    float tx = sy*fz - sz*fy;
    float ty = sz*fx - sx*fz;
    float tz = sx*fy - sy*fx;

    GLfloat m[16] = {
         sx,  tx, -fx, 0.0f,
         sy,  ty, -fy, 0.0f,
         sz,  tz, -fz, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    glMultMatrixf(m);
    glTranslatef((float)-ex, (float)-ey, (float)-ez);
}
