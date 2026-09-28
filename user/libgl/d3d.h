/* d3d.h - Direct3D for KestrelOS.
 *
 * Two interfaces, both real, both subsets, and both on the same rasteriser as
 * the OpenGL and Vulkan layers.
 *
 * Direct3D 9 is the one that fits: its fixed-function device - SetTransform,
 * SetRenderState, SetTexture, DrawPrimitiveUP - maps onto a fixed-function
 * pipeline exactly, so a D3D9 program that does not write shaders runs here
 * unchanged in substance.  That is why it is the fuller of the two.
 *
 * Direct3D 11 is the modern shape, and its shape is programmable: a device and
 * an immediate context, buffers, views, and a draw that means nothing without
 * compiled HLSL.  What is here is the object model and the draw path, with the
 * input layout standing in for the vertex shader.  A program using it will
 * render; a program relying on its own shaders will not, and the header says so
 * rather than the program discovering it.
 *
 * The COM calling convention is kept - an interface pointer whose first member
 * is a vtable - so code written against these reads like the real thing.
 */
#ifndef KESTREL_D3D_H
#define KESTREL_D3D_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t  HRESULT;
typedef uint32_t UINT;
typedef uint32_t DWORD;
typedef int32_t  INT;
typedef float    FLOAT;
typedef int      BOOL;

#define S_OK          ((HRESULT)0)
#define E_FAIL        ((HRESULT)0x80004005L)
#define E_INVALIDARG  ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr)    ((hr) < 0)

#define D3D_SDK_VERSION 32

/* --------------------------------------------------------------- Direct3D 9 */

typedef enum {
    D3DPT_POINTLIST     = 1,
    D3DPT_LINELIST      = 2,
    D3DPT_LINESTRIP     = 3,
    D3DPT_TRIANGLELIST  = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN   = 6,
} D3DPRIMITIVETYPE;

typedef enum {
    D3DTS_VIEW       = 2,
    D3DTS_PROJECTION = 3,
    D3DTS_WORLD      = 256,
} D3DTRANSFORMSTATETYPE;

typedef enum {
    D3DRS_ZENABLE           = 7,
    D3DRS_FILLMODE          = 8,
    D3DRS_SHADEMODE         = 9,
    D3DRS_ZWRITEENABLE      = 14,
    D3DRS_ALPHATESTENABLE   = 15,
    D3DRS_SRCBLEND          = 19,
    D3DRS_DESTBLEND         = 20,
    D3DRS_CULLMODE          = 22,
    D3DRS_ZFUNC             = 23,
    D3DRS_ALPHAREF          = 24,
    D3DRS_ALPHAFUNC         = 25,
    D3DRS_ALPHABLENDENABLE  = 27,
    D3DRS_LIGHTING          = 137,
    D3DRS_AMBIENT           = 139,
} D3DRENDERSTATETYPE;

typedef enum { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3 } D3DCULL;
typedef enum { D3DCMP_NEVER = 1, D3DCMP_LESS = 2, D3DCMP_EQUAL = 3,
               D3DCMP_LESSEQUAL = 4, D3DCMP_GREATER = 5, D3DCMP_NOTEQUAL = 6,
               D3DCMP_GREATEREQUAL = 7, D3DCMP_ALWAYS = 8 } D3DCMPFUNC;
typedef enum { D3DBLEND_ZERO = 1, D3DBLEND_ONE = 2, D3DBLEND_SRCALPHA = 5,
               D3DBLEND_INVSRCALPHA = 6 } D3DBLEND;
typedef enum { D3DSHADE_FLAT = 1, D3DSHADE_GOURAUD = 2 } D3DSHADEMODE;

#define D3DCLEAR_TARGET  0x00000001
#define D3DCLEAR_ZBUFFER 0x00000002

/* The flexible vertex format: which fields a vertex carries, in the fixed
 * order Direct3D lays them out. */
#define D3DFVF_XYZ      0x0002
#define D3DFVF_XYZRHW   0x0004
#define D3DFVF_NORMAL   0x0010
#define D3DFVF_DIFFUSE  0x0040
#define D3DFVF_TEX1     0x0100

typedef struct { FLOAT m[4][4]; } D3DMATRIX;
typedef struct { INT x, y; UINT Width, Height; FLOAT MinZ, MaxZ; } D3DVIEWPORT9;
typedef uint32_t D3DCOLOR;

#define D3DCOLOR_ARGB(a, r, g, b) \
    ((D3DCOLOR)((((a) & 0xFF) << 24) | (((r) & 0xFF) << 16) | \
                (((g) & 0xFF) << 8) | ((b) & 0xFF)))
#define D3DCOLOR_XRGB(r, g, b) D3DCOLOR_ARGB(0xFF, r, g, b)

typedef struct IDirect3DDevice9 IDirect3DDevice9;
typedef struct IDirect3D9 IDirect3D9;
typedef struct IDirect3DTexture9 IDirect3DTexture9;

typedef struct {
    HRESULT (*Release)(IDirect3DDevice9 *self);

    HRESULT (*Clear)(IDirect3DDevice9 *self, DWORD count, const void *rects,
                     DWORD flags, D3DCOLOR colour, float z, DWORD stencil);
    HRESULT (*BeginScene)(IDirect3DDevice9 *self);
    HRESULT (*EndScene)(IDirect3DDevice9 *self);
    HRESULT (*Present)(IDirect3DDevice9 *self, const void *src, const void *dst,
                       void *window, const void *dirty);

    HRESULT (*SetTransform)(IDirect3DDevice9 *self, D3DTRANSFORMSTATETYPE which,
                            const D3DMATRIX *matrix);
    HRESULT (*SetRenderState)(IDirect3DDevice9 *self, D3DRENDERSTATETYPE state,
                              DWORD value);
    HRESULT (*SetViewport)(IDirect3DDevice9 *self, const D3DVIEWPORT9 *vp);
    HRESULT (*SetFVF)(IDirect3DDevice9 *self, DWORD fvf);
    HRESULT (*SetTexture)(IDirect3DDevice9 *self, DWORD stage,
                          IDirect3DTexture9 *texture);

    HRESULT (*DrawPrimitiveUP)(IDirect3DDevice9 *self, D3DPRIMITIVETYPE type,
                               UINT primitive_count, const void *vertices,
                               UINT stride);
    HRESULT (*DrawIndexedPrimitiveUP)(IDirect3DDevice9 *self, D3DPRIMITIVETYPE type,
                                      UINT min_index, UINT vertex_count,
                                      UINT primitive_count, const void *indices,
                                      UINT index_size, const void *vertices,
                                      UINT stride);
} IDirect3DDevice9Vtbl;

struct IDirect3DDevice9 { const IDirect3DDevice9Vtbl *lpVtbl; };

typedef struct {
    HRESULT (*Release)(IDirect3D9 *self);
    HRESULT (*CreateDevice)(IDirect3D9 *self, UINT adapter, DWORD type,
                            void *focus_window, DWORD behaviour,
                            void *present_params, IDirect3DDevice9 **out);
    UINT    (*GetAdapterCount)(IDirect3D9 *self);
} IDirect3D9Vtbl;

struct IDirect3D9 { const IDirect3D9Vtbl *lpVtbl; };

IDirect3D9 *Direct3DCreate9(UINT sdk_version);

/* Direct3D takes its target from a swap chain bound to a window.  There is no
 * window system underneath here, so the target is set directly. */
HRESULT D3D9SetTargetKESTREL(IDirect3DDevice9 *device, void *colour_surface);

/* Textures, kept small: create from pixels, bind, release. */
IDirect3DTexture9 *D3D9CreateTextureKESTREL(IDirect3DDevice9 *device,
                                            UINT width, UINT height,
                                            const void *rgba);
void D3D9ReleaseTextureKESTREL(IDirect3DTexture9 *texture);

/* -------------------------------------------------------------- Direct3D 11 */

typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Buffer ID3D11Buffer;
typedef struct ID3D11RenderTargetView ID3D11RenderTargetView;
typedef struct ID3D11InputLayout ID3D11InputLayout;

typedef enum {
    DXGI_FORMAT_UNKNOWN            = 0,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT    = 6,
    DXGI_FORMAT_R32G32_FLOAT       = 16,
    DXGI_FORMAT_R8G8B8A8_UNORM     = 28,
    DXGI_FORMAT_R16_UINT           = 57,
    DXGI_FORMAT_R32_UINT           = 42,
} DXGI_FORMAT;

typedef enum {
    D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED     = 0,
    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST     = 1,
    D3D11_PRIMITIVE_TOPOLOGY_LINELIST      = 2,
    D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP     = 3,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST  = 4,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
} D3D11_PRIMITIVE_TOPOLOGY;

typedef enum { D3D11_USAGE_DEFAULT = 0, D3D11_USAGE_IMMUTABLE = 1,
               D3D11_USAGE_DYNAMIC = 2 } D3D11_USAGE;
#define D3D11_BIND_VERTEX_BUFFER 0x1
#define D3D11_BIND_INDEX_BUFFER  0x2

typedef struct {
    UINT ByteWidth; D3D11_USAGE Usage; UINT BindFlags;
    UINT CPUAccessFlags, MiscFlags, StructureByteStride;
} D3D11_BUFFER_DESC;

typedef struct { const void *pSysMem; UINT SysMemPitch, SysMemSlicePitch; } D3D11_SUBRESOURCE_DATA;

typedef struct {
    const char *SemanticName; UINT SemanticIndex; DXGI_FORMAT Format;
    UINT InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

typedef struct { FLOAT TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; } D3D11_VIEWPORT;

typedef struct {
    HRESULT (*Release)(ID3D11DeviceContext *self);
    void (*ClearRenderTargetView)(ID3D11DeviceContext *self,
                                  ID3D11RenderTargetView *rtv, const FLOAT rgba[4]);
    void (*OMSetRenderTargets)(ID3D11DeviceContext *self, UINT count,
                               ID3D11RenderTargetView *const *rtvs, void *depth);
    void (*RSSetViewports)(ID3D11DeviceContext *self, UINT count,
                           const D3D11_VIEWPORT *viewports);
    void (*IASetInputLayout)(ID3D11DeviceContext *self, ID3D11InputLayout *layout);
    void (*IASetVertexBuffers)(ID3D11DeviceContext *self, UINT slot, UINT count,
                               ID3D11Buffer *const *buffers, const UINT *strides,
                               const UINT *offsets);
    void (*IASetIndexBuffer)(ID3D11DeviceContext *self, ID3D11Buffer *buffer,
                             DXGI_FORMAT format, UINT offset);
    void (*IASetPrimitiveTopology)(ID3D11DeviceContext *self,
                                   D3D11_PRIMITIVE_TOPOLOGY topology);
    void (*Draw)(ID3D11DeviceContext *self, UINT vertex_count, UINT start);
    void (*DrawIndexed)(ID3D11DeviceContext *self, UINT index_count,
                        UINT start_index, INT base_vertex);
    /* Not Direct3D: with no vertex shader to run, the transform has to come
     * from somewhere, and this is where. */
    void (*SetTransformKESTREL)(ID3D11DeviceContext *self, const FLOAT *matrix4x4);
} ID3D11DeviceContextVtbl;

struct ID3D11DeviceContext { const ID3D11DeviceContextVtbl *lpVtbl; };

typedef struct {
    HRESULT (*Release)(ID3D11Device *self);
    HRESULT (*CreateBuffer)(ID3D11Device *self, const D3D11_BUFFER_DESC *desc,
                            const D3D11_SUBRESOURCE_DATA *data, ID3D11Buffer **out);
    HRESULT (*CreateInputLayout)(ID3D11Device *self,
                                 const D3D11_INPUT_ELEMENT_DESC *elements,
                                 UINT count, const void *shader_bytecode,
                                 size_t bytecode_length, ID3D11InputLayout **out);
    HRESULT (*CreateRenderTargetViewFromSurfaceKESTREL)(ID3D11Device *self,
                                                        void *colour_surface,
                                                        ID3D11RenderTargetView **out);
} ID3D11DeviceVtbl;

struct ID3D11Device { const ID3D11DeviceVtbl *lpVtbl; };

HRESULT D3D11CreateDevice(void *adapter, UINT driver_type, void *software,
                          UINT flags, const void *feature_levels,
                          UINT feature_level_count, UINT sdk_version,
                          ID3D11Device **device, UINT *feature_level,
                          ID3D11DeviceContext **context);

void D3D11ReleaseBufferKESTREL(ID3D11Buffer *buffer);
void D3D11ReleaseInputLayoutKESTREL(ID3D11InputLayout *layout);
void D3D11ReleaseRenderTargetViewKESTREL(ID3D11RenderTargetView *rtv);

#endif
