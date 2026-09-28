/* d3d.c - Direct3D on the software rasteriser.
 *
 * Direct3D 9's fixed-function device and this pipeline want the same things, so
 * most of this file is a translation table rather than an emulation: a render
 * state becomes a rasteriser state, a transform becomes a matrix, and
 * DrawPrimitiveUP walks a flexible vertex format straight into the vertex
 * stage.
 *
 * Two differences are real and are handled rather than ignored.  Direct3D's
 * matrices are row-major and multiply row-vectors on the left, where OpenGL's
 * are column-major and multiply column-vectors on the right - so a matrix
 * crossing this boundary is transposed.  And its clip space runs z from zero to
 * one, not minus one to one, so the depth range is set to match.
 */
#include "d3d.h"
#include "glstate.h"
#include "../libc/math.h"

/* ------------------------------------------------------------- Direct3D 9 */

struct IDirect3DTexture9 {
    GLuint name;
};

typedef struct {
    IDirect3DDevice9 base;

    surface_t *target;
    DWORD      fvf;
    mat4_t     world, view, projection;
    bool       in_scene;
    bool       lighting;
    D3DSHADEMODE shade;
    IDirect3DTexture9 *texture;
} d3d9_device_t;

typedef struct {
    IDirect3D9 base;
} d3d9_t;

/* Direct3D's matrices are the transpose of OpenGL's, and are applied in the
 * opposite order, so both the storage and the multiplication swap. */
static mat4_t from_d3d(const D3DMATRIX *m) {
    mat4_t r;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            r.m[col * 4 + row] = m->m[row][col];
    return r;
}

static GLenum d3d_compare(DWORD f) {
    switch (f) {
    case D3DCMP_NEVER:        return GL_NEVER;
    case D3DCMP_EQUAL:        return GL_EQUAL;
    case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
    case D3DCMP_GREATER:      return GL_GREATER;
    case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    case D3DCMP_ALWAYS:       return GL_ALWAYS;
    default:                  return GL_LESS;
    }
}

static GLenum d3d_blend(DWORD f) {
    switch (f) {
    case D3DBLEND_ZERO:        return GL_ZERO;
    case D3DBLEND_SRCALPHA:    return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    default:                   return GL_ONE;
    }
}

static GLenum d3d_topology(D3DPRIMITIVETYPE t) {
    switch (t) {
    case D3DPT_POINTLIST:     return GL_POINTS;
    case D3DPT_LINELIST:      return GL_LINES;
    case D3DPT_LINESTRIP:     return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   return GL_TRIANGLE_FAN;
    default:                  return GL_TRIANGLES;
    }
}

static UINT vertex_count_for(D3DPRIMITIVETYPE t, UINT primitives) {
    switch (t) {
    case D3DPT_POINTLIST:     return primitives;
    case D3DPT_LINELIST:      return primitives * 2;
    case D3DPT_LINESTRIP:     return primitives + 1;
    case D3DPT_TRIANGLELIST:  return primitives * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return primitives + 2;
    default:                  return 0;
    }
}

/* Walk one vertex of a flexible vertex format.  The fields appear in the fixed
 * order Direct3D defines, so the offsets follow from the format alone. */
static void emit_fvf_vertex(d3d9_device_t *dev, const unsigned char *v) {
    const unsigned char *p = v;

    float x = 0, y = 0, z = 0, w = 1;
    bool transformed = (dev->fvf & D3DFVF_XYZRHW) != 0;

    if (transformed) {
        const float *f = (const float *)p;
        x = f[0]; y = f[1]; z = f[2];
        /* RHW is the reciprocal of w, already divided through by the caller. */
        w = (f[3] != 0.0f) ? 1.0f / f[3] : 1.0f;
        p += 4 * sizeof(float);
    } else if (dev->fvf & D3DFVF_XYZ) {
        const float *f = (const float *)p;
        x = f[0]; y = f[1]; z = f[2];
        p += 3 * sizeof(float);
    }

    if (dev->fvf & D3DFVF_NORMAL) {
        const float *f = (const float *)p;
        glNormal3f(f[0], f[1], f[2]);
        p += 3 * sizeof(float);
    }

    if (dev->fvf & D3DFVF_DIFFUSE) {
        D3DCOLOR c = *(const D3DCOLOR *)p;
        glColor4f(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                  (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
        p += sizeof(D3DCOLOR);
    }

    if (dev->fvf & D3DFVF_TEX1) {
        const float *f = (const float *)p;
        glTexCoord2f(f[0], f[1]);
        p += 2 * sizeof(float);
    }

    glVertex4f(x, y, z, w);
}

/* Direct3D keeps world, view and projection separately; OpenGL folds the first
 * two into one modelview matrix, so they are combined here on the way in. */
static void apply_matrices(d3d9_device_t *dev) {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(dev->projection.m);

    mat4_t modelview = mat4_multiply(&dev->view, &dev->world);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(modelview.m);

    /* glMatrixMode clears the convention, so it is set back afterwards. */
    gl_set_clip_depth_zero_to_one(true);
}

static HRESULT d9_clear(IDirect3DDevice9 *self, DWORD count, const void *rects,
                        DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
    (void)count; (void)rects; (void)stencil;
    d3d9_device_t *dev = (d3d9_device_t *)self;
    if (!dev->target) return E_FAIL;

    glSetTarget(dev->target);
    GLbitfield mask = 0;
    if (flags & D3DCLEAR_TARGET) {
        glClearColor(((colour >> 16) & 0xFF) / 255.0f,
                     ((colour >> 8) & 0xFF) / 255.0f,
                     (colour & 0xFF) / 255.0f,
                     ((colour >> 24) & 0xFF) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER) {
        glClearDepth(z);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(mask);
    return S_OK;
}

static HRESULT d9_begin_scene(IDirect3DDevice9 *self) {
    d3d9_device_t *dev = (d3d9_device_t *)self;
    if (!dev->target) return E_FAIL;
    if (dev->in_scene) return E_FAIL;
    dev->in_scene = true;

    glSetTarget(dev->target);
    /* Direct3D's clip space runs z from zero to one, not minus one to one, and
     * its y points up as OpenGL's does. */
    gl_set_clip_depth_zero_to_one(true);
    gl_set_clip_y_down(false);
    glDepthRange(0.0, 1.0);
    apply_matrices(dev);
    return S_OK;
}

static HRESULT d9_end_scene(IDirect3DDevice9 *self) {
    d3d9_device_t *dev = (d3d9_device_t *)self;
    if (!dev->in_scene) return E_FAIL;
    dev->in_scene = false;
    return S_OK;
}

static HRESULT d9_present(IDirect3DDevice9 *self, const void *src, const void *dst,
                          void *window, const void *dirty) {
    (void)self; (void)src; (void)dst; (void)window; (void)dirty;
    /* Rendering already went into the caller's surface; putting that surface on
     * screen is the window system's job, not this one's. */
    return S_OK;
}

static HRESULT d9_set_transform(IDirect3DDevice9 *self, D3DTRANSFORMSTATETYPE which,
                                const D3DMATRIX *matrix) {
    d3d9_device_t *dev = (d3d9_device_t *)self;
    if (!matrix) return E_INVALIDARG;

    mat4_t m = from_d3d(matrix);
    switch (which) {
    case D3DTS_WORLD:      dev->world = m; break;
    case D3DTS_VIEW:       dev->view = m; break;
    case D3DTS_PROJECTION: dev->projection = m; break;
    default: return E_INVALIDARG;
    }
    apply_matrices(dev);
    return S_OK;
}

static HRESULT d9_set_render_state(IDirect3DDevice9 *self, D3DRENDERSTATETYPE state,
                                   DWORD value) {
    d3d9_device_t *dev = (d3d9_device_t *)self;

    switch (state) {
    case D3DRS_ZENABLE:
        if (value) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        break;
    case D3DRS_ZWRITEENABLE:
        glDepthMask(value ? GL_TRUE : GL_FALSE);
        break;
    case D3DRS_ZFUNC:
        glDepthFunc(d3d_compare(value));
        break;
    case D3DRS_CULLMODE:
        if (value == D3DCULL_NONE) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            /* Direct3D names the winding it culls; OpenGL names the winding it
             * keeps, so the two are opposites. */
            glFrontFace(value == D3DCULL_CCW ? GL_CW : GL_CCW);
        }
        break;
    case D3DRS_ALPHABLENDENABLE:
        if (value) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        break;
    case D3DRS_SRCBLEND:
    case D3DRS_DESTBLEND: {
        static DWORD src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
        if (state == D3DRS_SRCBLEND) src = value; else dst = value;
        glBlendFunc(d3d_blend(src), d3d_blend(dst));
        break;
    }
    case D3DRS_ALPHATESTENABLE:
        if (value) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
        break;
    case D3DRS_ALPHAFUNC:
        glAlphaFunc(d3d_compare(value), 0.5f);
        break;
    case D3DRS_ALPHAREF:
        glAlphaFunc(GL_GEQUAL, (float)(value & 0xFF) / 255.0f);
        break;
    case D3DRS_SHADEMODE:
        dev->shade = (D3DSHADEMODE)value;
        glShadeModel(value == D3DSHADE_FLAT ? GL_FLAT : GL_SMOOTH);
        break;
    case D3DRS_LIGHTING:
        dev->lighting = value != 0;
        if (value) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
        break;
    case D3DRS_AMBIENT: {
        float a[4] = { ((value >> 16) & 0xFF) / 255.0f,
                       ((value >> 8) & 0xFF) / 255.0f,
                       (value & 0xFF) / 255.0f, 1.0f };
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, a);
        break;
    }
    default:
        /* Unknown states are accepted rather than failing the call: a program
         * setting one this pipeline has no equivalent for should still run. */
        break;
    }
    return S_OK;
}

static HRESULT d9_set_viewport(IDirect3DDevice9 *self, const D3DVIEWPORT9 *vp) {
    (void)self;
    if (!vp) return E_INVALIDARG;
    glViewport(vp->x, vp->y, (GLsizei)vp->Width, (GLsizei)vp->Height);
    glDepthRange(vp->MinZ, vp->MaxZ);
    return S_OK;
}

static HRESULT d9_set_fvf(IDirect3DDevice9 *self, DWORD fvf) {
    ((d3d9_device_t *)self)->fvf = fvf;
    return S_OK;
}

static HRESULT d9_set_texture(IDirect3DDevice9 *self, DWORD stage,
                              IDirect3DTexture9 *texture) {
    if (stage != 0) return S_OK;                  /* one texture stage */
    d3d9_device_t *dev = (d3d9_device_t *)self;
    dev->texture = texture;

    if (texture) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture->name);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    return S_OK;
}

static HRESULT d9_draw_up(IDirect3DDevice9 *self, D3DPRIMITIVETYPE type,
                          UINT primitive_count, const void *vertices, UINT stride) {
    d3d9_device_t *dev = (d3d9_device_t *)self;
    if (!dev->in_scene || !vertices || !stride) return E_FAIL;

    UINT n = vertex_count_for(type, primitive_count);
    if (!n) return E_INVALIDARG;

    glBegin(d3d_topology(type));
    for (UINT i = 0; i < n; i++)
        emit_fvf_vertex(dev, (const unsigned char *)vertices + (size_t)i * stride);
    glEnd();
    return S_OK;
}

static HRESULT d9_draw_indexed_up(IDirect3DDevice9 *self, D3DPRIMITIVETYPE type,
                                  UINT min_index, UINT vertex_count,
                                  UINT primitive_count, const void *indices,
                                  UINT index_size, const void *vertices,
                                  UINT stride) {
    (void)min_index; (void)vertex_count;
    d3d9_device_t *dev = (d3d9_device_t *)self;
    if (!dev->in_scene || !vertices || !indices || !stride) return E_FAIL;

    UINT n = vertex_count_for(type, primitive_count);
    if (!n) return E_INVALIDARG;

    glBegin(d3d_topology(type));
    for (UINT i = 0; i < n; i++) {
        UINT index = (index_size == 2) ? ((const uint16_t *)indices)[i]
                                       : ((const uint32_t *)indices)[i];
        emit_fvf_vertex(dev, (const unsigned char *)vertices + (size_t)index * stride);
    }
    glEnd();
    return S_OK;
}

static HRESULT d9_device_release(IDirect3DDevice9 *self) {
    free(self);
    return S_OK;
}

static const IDirect3DDevice9Vtbl d9_device_vtbl = {
    d9_device_release,
    d9_clear, d9_begin_scene, d9_end_scene, d9_present,
    d9_set_transform, d9_set_render_state, d9_set_viewport, d9_set_fvf,
    d9_set_texture, d9_draw_up, d9_draw_indexed_up,
};

static HRESULT d9_create_device(IDirect3D9 *self, UINT adapter, DWORD type,
                                void *focus_window, DWORD behaviour,
                                void *present_params, IDirect3DDevice9 **out) {
    (void)self; (void)adapter; (void)type; (void)focus_window;
    (void)behaviour; (void)present_params;
    if (!out) return E_INVALIDARG;

    d3d9_device_t *dev = calloc(1, sizeof *dev);
    if (!dev) return E_OUTOFMEMORY;

    dev->base.lpVtbl = &d9_device_vtbl;
    dev->world = mat4_identity();
    dev->view = mat4_identity();
    dev->projection = mat4_identity();
    dev->fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    dev->shade = D3DSHADE_GOURAUD;

    *out = &dev->base;
    return S_OK;
}

static UINT d9_adapter_count(IDirect3D9 *self) { (void)self; return 1; }
static HRESULT d9_release(IDirect3D9 *self) { (void)self; return S_OK; }

static const IDirect3D9Vtbl d9_vtbl = { d9_release, d9_create_device, d9_adapter_count };
static d3d9_t g_d3d9 = { { &d9_vtbl } };

IDirect3D9 *Direct3DCreate9(UINT sdk_version) {
    (void)sdk_version;
    return &g_d3d9.base;
}

HRESULT D3D9SetTargetKESTREL(IDirect3DDevice9 *device, void *colour_surface) {
    if (!device || !colour_surface) return E_INVALIDARG;
    d3d9_device_t *dev = (d3d9_device_t *)device;
    dev->target = colour_surface;
    glSetTarget(dev->target);
    return S_OK;
}

IDirect3DTexture9 *D3D9CreateTextureKESTREL(IDirect3DDevice9 *device,
                                            UINT width, UINT height,
                                            const void *rgba) {
    (void)device;
    if (!width || !height) return NULL;

    IDirect3DTexture9 *t = calloc(1, sizeof *t);
    if (!t) return NULL;

    glGenTextures(1, &t->name);
    glBindTexture(GL_TEXTURE_2D, t->name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}

void D3D9ReleaseTextureKESTREL(IDirect3DTexture9 *texture) {
    if (!texture) return;
    glDeleteTextures(1, &texture->name);
    free(texture);
}

/* -------------------------------------------------------------- Direct3D 11 */

struct ID3D11Buffer {
    void  *data;
    UINT   size;
};

struct ID3D11RenderTargetView {
    surface_t *surface;
};

#define D3D11_MAX_ELEMENTS 8

struct ID3D11InputLayout {
    struct {
        DXGI_FORMAT format;
        UINT        offset;
        int         semantic;      /* 0 position, 1 colour, 2 texcoord, -1 other */
        int         components;
    } element[D3D11_MAX_ELEMENTS];
    UINT count;
};

typedef struct {
    ID3D11DeviceContext base;

    ID3D11RenderTargetView *rtv;
    ID3D11InputLayout      *layout;
    ID3D11Buffer           *vertex_buffer;
    UINT                    vertex_stride, vertex_offset;
    ID3D11Buffer           *index_buffer;
    DXGI_FORMAT             index_format;
    UINT                    index_offset;
    D3D11_PRIMITIVE_TOPOLOGY topology;
} d3d11_context_t;

typedef struct { ID3D11Device base; } d3d11_device_t;

static int format_components_d3d(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32_FLOAT:       return 2;
    case DXGI_FORMAT_R32G32B32_FLOAT:    return 3;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 4;
    default:                             return 0;
    }
}

/* Which part of a vertex an element is, taken from its semantic name - which
 * is the only statement of meaning available without a shader to compile. */
static int semantic_of(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "POSITION") || !strcmp(name, "SV_POSITION")) return 0;
    if (!strcmp(name, "COLOR") || !strcmp(name, "COLOUR")) return 1;
    if (!strcmp(name, "TEXCOORD")) return 2;
    return -1;
}

static void d11_clear_rtv(ID3D11DeviceContext *self, ID3D11RenderTargetView *rtv,
                          const FLOAT rgba[4]) {
    (void)self;
    if (!rtv || !rtv->surface || !rgba) return;
    glSetTarget(rtv->surface);
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void d11_om_set_targets(ID3D11DeviceContext *self, UINT count,
                               ID3D11RenderTargetView *const *rtvs, void *depth) {
    (void)depth;
    d3d11_context_t *c = (d3d11_context_t *)self;
    if (!count || !rtvs || !rtvs[0]) { c->rtv = NULL; return; }
    c->rtv = rtvs[0];
    if (c->rtv->surface) {
        glSetTarget(c->rtv->surface);

        /* Direct3D 11 has no fixed-function projection: the vertex shader - or
         * here, SetTransform - supplies the whole thing.  The projection matrix
         * is reset so nothing another API left behind is applied on top. */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        gl_set_clip_depth_zero_to_one(true);      /* as with Direct3D 9 */
        gl_set_clip_y_down(false);
        glDepthRange(0.0, 1.0);

        /* Direct3D 11 carries its rasteriser and depth state in state objects.
         * Without those, what a program gets is the documented default: depth
         * testing on with a less-than comparison, back faces culled, and
         * clockwise winding treated as front-facing - which is the same
         * convention Direct3D 9 means by D3DCULL_CW. */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_LIGHTING);
        glDisable(GL_BLEND);
        glShadeModel(GL_SMOOTH);
    }
}

static void d11_rs_set_viewports(ID3D11DeviceContext *self, UINT count,
                                 const D3D11_VIEWPORT *viewports) {
    (void)self;
    if (!count || !viewports) return;
    glViewport((GLint)viewports[0].TopLeftX, (GLint)viewports[0].TopLeftY,
               (GLsizei)viewports[0].Width, (GLsizei)viewports[0].Height);
    glDepthRange(viewports[0].MinDepth, viewports[0].MaxDepth);
}

static void d11_ia_set_layout(ID3D11DeviceContext *self, ID3D11InputLayout *layout) {
    ((d3d11_context_t *)self)->layout = layout;
}

static void d11_ia_set_vertex_buffers(ID3D11DeviceContext *self, UINT slot, UINT count,
                                      ID3D11Buffer *const *buffers,
                                      const UINT *strides, const UINT *offsets) {
    (void)slot;
    d3d11_context_t *c = (d3d11_context_t *)self;
    if (!count || !buffers) { c->vertex_buffer = NULL; return; }
    c->vertex_buffer = buffers[0];
    c->vertex_stride = strides ? strides[0] : 0;
    c->vertex_offset = offsets ? offsets[0] : 0;
}

static void d11_ia_set_index_buffer(ID3D11DeviceContext *self, ID3D11Buffer *buffer,
                                    DXGI_FORMAT format, UINT offset) {
    d3d11_context_t *c = (d3d11_context_t *)self;
    c->index_buffer = buffer;
    c->index_format = format;
    c->index_offset = offset;
}

static void d11_ia_set_topology(ID3D11DeviceContext *self,
                                D3D11_PRIMITIVE_TOPOLOGY topology) {
    ((d3d11_context_t *)self)->topology = topology;
}

static GLenum d11_topology(D3D11_PRIMITIVE_TOPOLOGY t) {
    switch (t) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return GL_POINTS;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return GL_LINES;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return GL_LINE_STRIP;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    default:                                     return GL_TRIANGLES;
    }
}

static void d11_emit_vertex(d3d11_context_t *c, UINT index) {
    if (!c->layout || !c->vertex_buffer || !c->vertex_stride) return;

    const unsigned char *base = (const unsigned char *)c->vertex_buffer->data +
                                c->vertex_offset + (size_t)index * c->vertex_stride;

    float pos[4] = { 0, 0, 0, 1 };
    float col[4] = { 1, 1, 1, 1 };
    float uv[2]  = { 0, 0 };

    for (UINT e = 0; e < c->layout->count; e++) {
        const float *src = (const float *)(base + c->layout->element[e].offset);
        int n = c->layout->element[e].components;
        switch (c->layout->element[e].semantic) {
        case 0: for (int i = 0; i < n && i < 4; i++) pos[i] = src[i]; break;
        case 1: for (int i = 0; i < n && i < 4; i++) col[i] = src[i]; break;
        case 2: for (int i = 0; i < n && i < 2; i++) uv[i] = src[i]; break;
        default: break;
        }
    }

    glColor4f(col[0], col[1], col[2], col[3]);
    glTexCoord2f(uv[0], uv[1]);
    glVertex4f(pos[0], pos[1], pos[2], pos[3]);
}

static void d11_draw(ID3D11DeviceContext *self, UINT vertex_count, UINT start) {
    d3d11_context_t *c = (d3d11_context_t *)self;
    if (!c->vertex_buffer) return;

    glBegin(d11_topology(c->topology));
    for (UINT i = 0; i < vertex_count; i++) d11_emit_vertex(c, start + i);
    glEnd();
}

static void d11_draw_indexed(ID3D11DeviceContext *self, UINT index_count,
                             UINT start_index, INT base_vertex) {
    d3d11_context_t *c = (d3d11_context_t *)self;
    if (!c->vertex_buffer || !c->index_buffer) return;

    const unsigned char *idx = (const unsigned char *)c->index_buffer->data +
                               c->index_offset;

    glBegin(d11_topology(c->topology));
    for (UINT i = 0; i < index_count; i++) {
        UINT k = start_index + i;
        UINT v = (c->index_format == DXGI_FORMAT_R16_UINT)
               ? ((const uint16_t *)idx)[k] : ((const uint32_t *)idx)[k];
        d11_emit_vertex(c, (UINT)((INT)v + base_vertex));
    }
    glEnd();
}

static void d11_set_transform(ID3D11DeviceContext *self, const FLOAT *matrix) {
    (void)self;
    if (!matrix) return;
    glMatrixMode(GL_MODELVIEW);
    gl_set_clip_depth_zero_to_one(true);          /* glMatrixMode clears it */
    glLoadMatrixf(matrix);
}

static HRESULT d11_context_release(ID3D11DeviceContext *self) {
    free(self);
    return S_OK;
}

static const ID3D11DeviceContextVtbl d11_context_vtbl = {
    d11_context_release, d11_clear_rtv, d11_om_set_targets, d11_rs_set_viewports,
    d11_ia_set_layout, d11_ia_set_vertex_buffers, d11_ia_set_index_buffer,
    d11_ia_set_topology, d11_draw, d11_draw_indexed, d11_set_transform,
};

static HRESULT d11_create_buffer(ID3D11Device *self, const D3D11_BUFFER_DESC *desc,
                                 const D3D11_SUBRESOURCE_DATA *data,
                                 ID3D11Buffer **out) {
    (void)self;
    if (!desc || !out || !desc->ByteWidth) return E_INVALIDARG;

    ID3D11Buffer *b = calloc(1, sizeof *b);
    if (!b) return E_OUTOFMEMORY;
    b->data = malloc(desc->ByteWidth);
    if (!b->data) { free(b); return E_OUTOFMEMORY; }
    b->size = desc->ByteWidth;
    if (data && data->pSysMem) memcpy(b->data, data->pSysMem, desc->ByteWidth);
    else                       memset(b->data, 0, desc->ByteWidth);

    *out = b;
    return S_OK;
}

static HRESULT d11_create_input_layout(ID3D11Device *self,
                                       const D3D11_INPUT_ELEMENT_DESC *elements,
                                       UINT count, const void *bytecode,
                                       size_t bytecode_length,
                                       ID3D11InputLayout **out) {
    (void)self; (void)bytecode; (void)bytecode_length;
    if (!elements || !out) return E_INVALIDARG;

    ID3D11InputLayout *l = calloc(1, sizeof *l);
    if (!l) return E_OUTOFMEMORY;

    for (UINT i = 0; i < count && l->count < D3D11_MAX_ELEMENTS; i++) {
        int comps = format_components_d3d(elements[i].Format);
        if (!comps) continue;
        l->element[l->count].format = elements[i].Format;
        l->element[l->count].offset = elements[i].AlignedByteOffset;
        l->element[l->count].semantic = semantic_of(elements[i].SemanticName);
        l->element[l->count].components = comps;
        l->count++;
    }

    *out = l;
    return S_OK;
}

static HRESULT d11_create_rtv(ID3D11Device *self, void *colour_surface,
                              ID3D11RenderTargetView **out) {
    (void)self;
    if (!colour_surface || !out) return E_INVALIDARG;
    ID3D11RenderTargetView *v = calloc(1, sizeof *v);
    if (!v) return E_OUTOFMEMORY;
    v->surface = colour_surface;
    *out = v;
    return S_OK;
}

static HRESULT d11_device_release(ID3D11Device *self) { (void)self; return S_OK; }

static const ID3D11DeviceVtbl d11_device_vtbl = {
    d11_device_release, d11_create_buffer, d11_create_input_layout, d11_create_rtv,
};

static d3d11_device_t g_d3d11 = { { &d11_device_vtbl } };

HRESULT D3D11CreateDevice(void *adapter, UINT driver_type, void *software,
                          UINT flags, const void *feature_levels,
                          UINT feature_level_count, UINT sdk_version,
                          ID3D11Device **device, UINT *feature_level,
                          ID3D11DeviceContext **context) {
    (void)adapter; (void)driver_type; (void)software; (void)flags;
    (void)feature_levels; (void)feature_level_count; (void)sdk_version;

    if (device) *device = &g_d3d11.base;
    /* 0xa000 is feature level 10.0, which is what a fixed-function pipeline
     * honestly corresponds to; claiming 11.0 would promise compute and
     * tessellation that are not here. */
    if (feature_level) *feature_level = 0xa000;

    if (context) {
        d3d11_context_t *c = calloc(1, sizeof *c);
        if (!c) return E_OUTOFMEMORY;
        c->base.lpVtbl = &d11_context_vtbl;
        c->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        *context = &c->base;
    }
    return S_OK;
}

void D3D11ReleaseBufferKESTREL(ID3D11Buffer *b) {
    if (!b) return;
    free(b->data);
    free(b);
}

void D3D11ReleaseInputLayoutKESTREL(ID3D11InputLayout *l) { free(l); }
void D3D11ReleaseRenderTargetViewKESTREL(ID3D11RenderTargetView *v) { free(v); }
