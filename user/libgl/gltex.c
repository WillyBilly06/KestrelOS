/* gltex.c - textures and vertex arrays.
 *
 * Texels are kept as 32-bit ARGB whatever format they arrived in, so the
 * sampler in the inner loop never has to branch on a format.  The conversion
 * happens once, in glTexImage2D.
 */
#include "glstate.h"
#include "../libc/math.h"

/* ---------------------------------------------------------------- textures */

/* Do not recycle versions when a texture name or allocator address is reused.
 * On theoretical exhaustion, zero disables residency reuse for new images. */
static uint64_t image_version;
static uint64_t next_image_version(void) {
    return image_version==UINT64_MAX ? 0 : ++image_version;
}

void glGenTextures(GLsizei n, GLuint *out) {
    gl_context_init();
    if (n < 0 || !out) { gl_set_error(GL_INVALID_VALUE); return; }

    int made = 0;
    /* Name zero is reserved for "no texture", so allocation starts at one. */
    for (GLuint id = 1; id < GL_MAX_TEXTURES && made < n; id++) {
        if (g_gl.textures[id].used) continue;
        memset(&g_gl.textures[id], 0, sizeof g_gl.textures[id]);
        g_gl.textures[id].used = true;
        g_gl.textures[id].min_filter = GL_LINEAR;
        g_gl.textures[id].mag_filter = GL_LINEAR;
        g_gl.textures[id].wrap_s = GL_REPEAT;
        g_gl.textures[id].wrap_t = GL_REPEAT;
        out[made++] = id;
    }
    for (; made < n; made++) out[made] = 0;
    if (made < n) gl_set_error(GL_OUT_OF_MEMORY);
}

void glDeleteTextures(GLsizei n, const GLuint *ids) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!ids) return;
    for (GLsizei i = 0; i < n; i++) {
        GLuint id = ids[i];
        if (!id || id >= GL_MAX_TEXTURES) continue;
        gl_gpu_texture_release(&g_gl.textures[id]);
        free(g_gl.textures[id].texels);
        memset(&g_gl.textures[id], 0, sizeof g_gl.textures[id]);
        if (g_gl.bound_texture == id) g_gl.bound_texture = 0;
        for (unsigned unit = 0; unit < GL_MAX_TEXTURE_UNITS; unit++)
            if (g_gl.unit_texture[unit] == id) g_gl.unit_texture[unit] = 0;
    }
}

GLboolean glIsTexture(GLuint id) {
    gl_context_init();
    return (id && id < GL_MAX_TEXTURES && g_gl.textures[id].used) ? GL_TRUE : GL_FALSE;
}

void glBindTexture(GLenum target, GLuint id) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }
    if (id >= GL_MAX_TEXTURES) { gl_set_error(GL_INVALID_VALUE); return; }

    /* Binding a name that glGenTextures never handed out creates it, which is
     * what OpenGL specifies and what a good deal of code relies on. */
    if (id && !g_gl.textures[id].used) {
        memset(&g_gl.textures[id], 0, sizeof g_gl.textures[id]);
        g_gl.textures[id].used = true;
        g_gl.textures[id].min_filter = GL_LINEAR;
        g_gl.textures[id].mag_filter = GL_LINEAR;
        g_gl.textures[id].wrap_s = GL_REPEAT;
        g_gl.textures[id].wrap_t = GL_REPEAT;
    }
    g_gl.bound_texture = id;
    /* And into the unit a shader would reach it through.  Unit zero is the
     * same texture the fixed-function path uses, so the two never disagree. */
    if (g_gl.active_unit >= 0 && g_gl.active_unit < GL_MAX_TEXTURE_UNITS)
        g_gl.unit_texture[g_gl.active_unit] = id;
}

void glTexImage2D(GLenum target, GLint level, GLint internal_format,
                  GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    (void)internal_format; (void)border;

    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }
    if (level != 0) return;                       /* no mip levels */
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    GLuint id = g_gl.bound_texture;
    if (!id || id >= GL_MAX_TEXTURES) { gl_set_error(GL_INVALID_OPERATION); return; }

    if (type != GL_UNSIGNED_BYTE) { gl_set_error(GL_INVALID_ENUM); return; }

    int components = (format == GL_RGB) ? 3 :
                     (format == GL_RGBA || format == GL_BGRA) ? 4 :
                     (format == GL_LUMINANCE) ? 1 : 0;
    if (!components) { gl_set_error(GL_INVALID_ENUM); return; }

    /* Rejected/failed uploads must not mutate the existing image or its cache
     * identity. Prepare the replacement before retiring/freeing the old image. */
    colour_t *texels = malloc((size_t)w * h * sizeof(colour_t));
    if (!texels) { gl_set_error(GL_OUT_OF_MEMORY); return; }

    const GLubyte *src = pixels;
    for (int i = 0; i < w * h; i++) {
        if (!src) { texels[i] = 0; continue; }
        const GLubyte *p = src + (size_t)i * components;
        GLubyte r, g, b, a = 255;
        if (components == 1)      { r = g = b = p[0]; }
        else if (format == GL_BGRA) { b = p[0]; g = p[1]; r = p[2]; a = p[3]; }
        else                      { r = p[0]; g = p[1]; b = p[2];
                                    if (components == 4) a = p[3]; }
        texels[i] = ((colour_t)a << 24) | RGB(r, g, b);
    }
    gl_texture_t *t = &g_gl.textures[id];
    gl_gpu_texture_release(t);
    free(t->texels);
    t->texels = texels;
    t->width = w; t->height = h;
    t->content_version = next_image_version();
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }
    GLuint id = g_gl.bound_texture;
    if (!id || id >= GL_MAX_TEXTURES) return;

    gl_texture_t *t = &g_gl.textures[id];
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = (GLenum)param; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = (GLenum)param; break;
    case GL_TEXTURE_WRAP_S:     t->wrap_s = (GLenum)param; break;
    case GL_TEXTURE_WRAP_T:     t->wrap_t = (GLenum)param; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE)
        g_gl.tex_env = (GLenum)param;
}

/* ----------------------------------------------------------------- sampling */

static int wrap(int v, int n, GLenum mode) {
    if (n <= 0) return 0;
    if (mode == GL_REPEAT) {
        v %= n;
        return v < 0 ? v + n : v;
    }
    return v < 0 ? 0 : (v >= n ? n - 1 : v);        /* clamp */
}

static void fetch(const gl_texture_t *t, int x, int y, float *out) {
    x = wrap(x, t->width, t->wrap_s);
    y = wrap(y, t->height, t->wrap_t);
    colour_t c = t->texels[(size_t)y * t->width + x];
    out[0] = RGB_R(c) * (1.0f / 255.0f);
    out[1] = RGB_G(c) * (1.0f / 255.0f);
    out[2] = RGB_B(c) * (1.0f / 255.0f);
    out[3] = ((c >> 24) & 0xFF) * (1.0f / 255.0f);
}

void gl_sample(float s, float t, float *out) {
    gl_sample_texture(g_gl.bound_texture, s, t, out);
}

/* The same, for whichever texture a shader's sampler names.  Splitting it out
 * is what lets a fragment shader read two textures at once. */
void gl_sample_texture(GLuint id, float s, float t, float *out) {
    out[0] = out[1] = out[2] = out[3] = 1.0f;

    if (!id || id >= GL_MAX_TEXTURES) return;
    const gl_texture_t *tex = &g_gl.textures[id];
    if (!tex->used || !tex->texels || tex->width <= 0) return;

    /* Texture space has its origin at the bottom left, a surface's at the top
     * left, so t is flipped on the way in. */
    float u = s * (float)tex->width - 0.5f;
    float v = (1.0f - t) * (float)tex->height - 0.5f;

    if (tex->mag_filter == GL_NEAREST) {
        fetch(tex, (int)floorf(u + 0.5f), (int)floorf(v + 0.5f), out);
        return;
    }

    /* Bilinear: the four texels around the sample point, weighted by how far
     * into the gap between them it falls. */
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - (float)x0, fy = v - (float)y0;

    float c00[4], c10[4], c01[4], c11[4];
    fetch(tex, x0,     y0,     c00);
    fetch(tex, x0 + 1, y0,     c10);
    fetch(tex, x0,     y0 + 1, c01);
    fetch(tex, x0 + 1, y0 + 1, c11);

    for (int i = 0; i < 4; i++) {
        float top = c00[i] + (c10[i] - c00[i]) * fx;
        float bot = c01[i] + (c11[i] - c01[i]) * fx;
        out[i] = top + (bot - top) * fy;
    }
}

/* ------------------------------------------------------------ vertex arrays */

static gl_array_t *array_for(GLenum which) {
    switch (which) {
    case GL_VERTEX_ARRAY:        return &g_gl.array_vertex;
    case GL_NORMAL_ARRAY:        return &g_gl.array_normal;
    case GL_COLOR_ARRAY:         return &g_gl.array_colour;
    case GL_TEXTURE_COORD_ARRAY: return &g_gl.array_texcoord;
    default:                     return NULL;
    }
}

void glEnableClientState(GLenum which) {
    gl_context_init();
    gl_array_t *a = array_for(which);
    if (!a) { gl_set_error(GL_INVALID_ENUM); return; }
    a->enabled = true;
}

void glDisableClientState(GLenum which) {
    gl_context_init();
    gl_array_t *a = array_for(which);
    if (!a) { gl_set_error(GL_INVALID_ENUM); return; }
    a->enabled = false;
}

static int type_size(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT:
    case GL_FLOAT:                         return 4;
    case GL_DOUBLE:                        return 8;
    default:                               return 0;
    }
}

static void set_array(gl_array_t *a, int size, GLenum type, GLsizei stride,
                      const GLvoid *p) {
    int elem = type_size(type);
    if (!elem) { gl_set_error(GL_INVALID_ENUM); return; }
    a->size = size;
    a->type = type;
    a->stride = stride ? stride : size * elem;
    a->pointer = p;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) {
    gl_context_init();
    set_array(&g_gl.array_vertex, size, type, stride, p);
}
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) {
    gl_context_init();
    set_array(&g_gl.array_normal, 3, type, stride, p);
}
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) {
    gl_context_init();
    set_array(&g_gl.array_colour, size, type, stride, p);
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) {
    gl_context_init();
    set_array(&g_gl.array_texcoord, size, type, stride, p);
}

/* Read one element out of an array as floats, converting whatever it holds.
 * Integer colour arrays are scaled to 0..1; positions are not. */
static void read_element(const gl_array_t *a, int index, float *out, int want,
                         bool normalise) {
    for (int i = 0; i < want; i++) out[i] = 0.0f;
    if (!a->pointer) return;

    const unsigned char *base = (const unsigned char *)a->pointer +
                                (size_t)index * a->stride;
    int n = a->size < want ? a->size : want;

    for (int i = 0; i < n; i++) {
        switch (a->type) {
        case GL_FLOAT:  out[i] = ((const float *)base)[i]; break;
        case GL_DOUBLE: out[i] = (float)((const double *)base)[i]; break;
        case GL_BYTE:
            out[i] = ((const signed char *)base)[i];
            if (normalise) out[i] /= 127.0f;
            break;
        case GL_UNSIGNED_BYTE:
            out[i] = base[i];
            if (normalise) out[i] /= 255.0f;
            break;
        case GL_SHORT:
            out[i] = ((const short *)base)[i];
            if (normalise) out[i] /= 32767.0f;
            break;
        case GL_UNSIGNED_SHORT:
            out[i] = ((const unsigned short *)base)[i];
            if (normalise) out[i] /= 65535.0f;
            break;
        case GL_INT:    out[i] = (float)((const int *)base)[i]; break;
        case GL_UNSIGNED_INT: out[i] = (float)((const unsigned *)base)[i]; break;
        default: break;
        }
    }
}

/* Set the current attributes from the arrays and emit one vertex.  Going
 * through glVertex means arrays and immediate mode share the whole transform
 * and lighting path rather than each having their own. */
static void array_vertex(int index) {
    if (g_gl.array_colour.enabled) {
        float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        read_element(&g_gl.array_colour, index, c, 4, true);
        if (g_gl.array_colour.size < 4) c[3] = 1.0f;
        glColor4f(c[0], c[1], c[2], c[3]);
    }
    if (g_gl.array_normal.enabled) {
        float n[3];
        read_element(&g_gl.array_normal, index, n, 3, true);
        glNormal3f(n[0], n[1], n[2]);
    }
    if (g_gl.array_texcoord.enabled) {
        float t[2];
        read_element(&g_gl.array_texcoord, index, t, 2, false);
        glTexCoord2f(t[0], t[1]);
    }

    float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    read_element(&g_gl.array_vertex, index, v, 4, false);
    if (g_gl.array_vertex.size < 4) v[3] = 1.0f;
    glVertex4f(v[0], v[1], v[2], v[3]);
}

/* With a program in use the vertex comes out of the vertex shader rather than
 * the fixed-function transform, and there is no glVertex to go through - the
 * shader wrote clip space itself. */
static void shaded_vertex(int index) {
    if (g_gl.nverts >= GL_MAX_VERTICES) { gl_flush_primitive(); }
    if (g_gl.nverts >= GL_MAX_VERTICES) return;
    gl_shader_vertex(index, &g_gl.verts[g_gl.nverts++]);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (count < 0 || first < 0 || (uint64_t)(unsigned)first+(unsigned)count>0x80000000ull) { gl_set_error(GL_INVALID_VALUE); return; }
    if(gl_shader_draw_gpu(mode,first,count,0,NULL))return;

    if (gl_program_active()) {
        gl_shader_begin_draw();
        glBegin(mode);
        for (GLsizei i = 0; i < count; i++) shaded_vertex(first + i);
        glEnd();
        return;
    }

    if (!g_gl.array_vertex.enabled || !g_gl.array_vertex.pointer) return;

    glBegin(mode);
    for (GLsizei i = 0; i < count; i++) array_vertex(first + i);
    glEnd();
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (count < 0 || !indices) { gl_set_error(GL_INVALID_VALUE); return; }
    if(gl_shader_draw_gpu(mode,0,count,type,indices))return;

    bool shaded = gl_program_active();
    if (shaded) gl_shader_begin_draw();
    else if (!g_gl.array_vertex.enabled || !g_gl.array_vertex.pointer) return;

    glBegin(mode);
    for (GLsizei i = 0; i < count; i++) {
        int index;
        switch (type) {
        case GL_UNSIGNED_BYTE:  index = ((const unsigned char *)indices)[i]; break;
        case GL_UNSIGNED_SHORT: index = ((const unsigned short *)indices)[i]; break;
        case GL_UNSIGNED_INT:   index = (int)((const unsigned *)indices)[i]; break;
        default: glEnd(); gl_set_error(GL_INVALID_ENUM); return;
        }
        if (shaded) shaded_vertex(index);
        else array_vertex(index);
    }
    glEnd();
}

void glGetShaderStats(unsigned *instructions) {
    gl_context_init();
    if (instructions) *instructions = g_gl.stat_shader_instructions;
}

void glBindTextureUnit(GLenum unit, GLuint id) {
    gl_context_init();
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, id);
}
