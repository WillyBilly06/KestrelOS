/* d3dtest - reaching the graphics card the way a Windows program does.
 *
 * A Windows program does not call functions to draw.  It asks for a device,
 * and what comes back is a pointer to a pointer to a table of function
 * pointers; every method is a load, a load and a call, with the object itself
 * as the first argument.  That arrangement is most of what makes graphics code
 * on Windows look the way it does, and a program built for it does not run at
 * all without it.
 *
 * So this checks the shape as much as the drawing: that a device comes back,
 * that its table has the three every interface has, that asking it for a
 * buffer works through that table, and that the buffer can be handed back and
 * drawn.  Where there is no card behind the display, the test is that the
 * program is told so plainly rather than given a device that quietly does
 * nothing - which is the difference between a program that can fall back and
 * one that shows a blank window.
 */
#include "winapi.h"
#include "shaders_blob.h"

typedef long HRESULT;

/* What every interface starts with, and what these two add after it. */
typedef struct d3d_iface d3d_iface;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    /* device */
    HRESULT (__stdcall *CreateBuffer)(d3d_iface *, const void *, const void *,
                                      d3d_iface **);
    HRESULT (__stdcall *CreateVertexShader)(d3d_iface *, const void *,
                                            unsigned long long, void *,
                                            d3d_iface **);
    HRESULT (__stdcall *CreatePixelShader)(d3d_iface *, const void *,
                                           unsigned long long, void *,
                                           d3d_iface **);
    HRESULT (__stdcall *CreateInputLayout)(d3d_iface *, const void *, unsigned,
                                           const void *, unsigned long long,
                                           d3d_iface **);
} d3d_device_vtable;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    /* context */
    void (__stdcall *SetVertexBuffers)(d3d_iface *, unsigned, unsigned,
                                       d3d_iface *const *, const unsigned *,
                                       const unsigned *);
    void (__stdcall *Draw)(d3d_iface *, unsigned, unsigned);
    HRESULT (__stdcall *Present)(d3d_iface *, unsigned, unsigned);
    void (__stdcall *VSSetShader)(d3d_iface *, d3d_iface *, void *, unsigned);
    void (__stdcall *PSSetShader)(d3d_iface *, d3d_iface *, void *, unsigned);
    void (__stdcall *IASetInputLayout)(d3d_iface *, d3d_iface *);
} d3d_context_vtable;

struct d3d_iface { const void *table; };

typedef struct {
    unsigned ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags, Stride;
} BUFFER_DESC;

typedef struct { const void *pSysMem; unsigned pitch, slice; } SUBRESOURCE;

/* What is inside a vertex, named the way the shader names it.  A program is
 * entitled to lay these out however it likes; matching them to the shader's
 * inputs is the system's job, not the program's. */
typedef struct {
    const char *SemanticName;
    unsigned SemanticIndex;
    unsigned Format;
    unsigned InputSlot;
    unsigned AlignedByteOffset;
    unsigned InputSlotClass;
    unsigned InstanceDataStepRate;
} INPUT_ELEMENT;

#define FORMAT_FOUR_FLOATS 2       /* DXGI_FORMAT_R32G32B32A32_FLOAT */

/* One corner: where it is, and what colour it carries. */
typedef struct { float x, y, z, w, r, g, b, a; } corner;

__declspec(dllimport) HRESULT __stdcall D3DCreateDevice(d3d_iface **, d3d_iface **);

/* Direct3D 12, which is a different shape rather than a newer one: no
 * immediate context, and nothing happens until a recorded list is executed. */
__declspec(dllimport) HRESULT __stdcall D3D12CreateDevice(void *, unsigned,
                                                          const void *, void **);

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    HRESULT (__stdcall *CreateCommandQueue)(d3d_iface *, const void *,
                                            const void *, void **);
    HRESULT (__stdcall *CreateCommandAllocator)(d3d_iface *, unsigned,
                                                const void *, void **);
    HRESULT (__stdcall *CreateCommandList)(d3d_iface *, unsigned, unsigned,
                                           d3d_iface *, d3d_iface *,
                                           const void *, void **);
    HRESULT (__stdcall *CreateDescriptorHeap)(d3d_iface *, const void *,
                                              const void *, void **);
    HRESULT (__stdcall *CreateFence)(d3d_iface *, unsigned long long, unsigned,
                                     const void *, void **);
    HRESULT (__stdcall *CreateCommittedResource)(d3d_iface *, const void *,
                                                 unsigned, const void *,
                                                 unsigned, const void *,
                                                 const void *, void **);
    HRESULT (__stdcall *CreateGraphicsPipelineState)(d3d_iface *, const void *,
                                                     const void *, void **);
    HRESULT (__stdcall *CreateRootSignature)(d3d_iface *, unsigned, const void *,
                                             unsigned long long, const void *,
                                             void **);
} d12_device_vtable;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    HRESULT (__stdcall *Close)(d3d_iface *);
    HRESULT (__stdcall *Reset)(d3d_iface *, d3d_iface *, d3d_iface *);
    void (__stdcall *ClearRenderTargetView)(d3d_iface *, unsigned long long,
                                            const float *, unsigned, const void *);
    void (__stdcall *IASetVertexBuffers)(d3d_iface *, unsigned, unsigned,
                                         const void *);
    void (__stdcall *DrawInstanced)(d3d_iface *, unsigned, unsigned, unsigned,
                                    unsigned);
} d12_list_vtable;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    void (__stdcall *ExecuteCommandLists)(d3d_iface *, unsigned, d3d_iface **);
    void (__stdcall *Signal)(d3d_iface *, d3d_iface *, unsigned long long);
} d12_queue_vtable;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    HRESULT (__stdcall *Map)(d3d_iface *, unsigned, const void *, void **);
    void (__stdcall *Unmap)(d3d_iface *, unsigned, const void *);
    unsigned long long (__stdcall *GetGPUVirtualAddress)(d3d_iface *);
} d12_resource_vtable;

typedef struct {
    HRESULT (__stdcall *QueryInterface)(d3d_iface *, const void *, void **);
    unsigned (__stdcall *AddRef)(d3d_iface *);
    unsigned (__stdcall *Release)(d3d_iface *);
    unsigned long long (__stdcall *GetCompletedValue)(d3d_iface *);
    HRESULT (__stdcall *SetEventOnCompletion)(d3d_iface *, unsigned long long,
                                              void *);
} d12_fence_vtable;

/* What a program actually does with Direct3D 12, in the order it does it.
 *
 * The point of checking this separately from the older interface is that
 * nothing here happens when it is called: the clear and the draw are recorded,
 * and only executing the list makes them real.  A layer that did the work
 * during recording would pass every check that looks at return values and
 * still be the wrong shape.
 */
static void check_d3d12(void) {
    d3d_iface *device = 0;
    HRESULT hr = D3D12CreateDevice(0, 0, 0, (void **)&device);

    if (hr != 0 || !device) {
        check(hr != 0, "a Direct3D 12 device is refused when there is no card");
        return;
    }
    check(1, "a Direct3D 12 device is created");

    const d12_device_vtable *dev = (const d12_device_vtable *)device->table;

    d3d_iface *queue = 0, *alloc = 0, *list = 0, *fence = 0, *buffer = 0;

    check(dev->CreateCommandQueue(device, 0, 0, (void **)&queue) == 0 && queue,
          "and a queue to give work to");
    check(dev->CreateCommandAllocator(device, 0, 0, (void **)&alloc) == 0 && alloc,
          "and an allocator for a list to record into");
    check(dev->CreateCommandList(device, 0, 0, alloc, 0, 0, (void **)&list) == 0
          && list, "and a command list, open and recording");
    check(dev->CreateFence(device, 0, 0, 0, (void **)&fence) == 0 && fence,
          "and a fence to find out when the queue is done");

    /* A buffer of corners, described the way Direct3D 12 describes one: a
     * dimension, an alignment, and a width in bytes. */
    struct { unsigned dimension; unsigned long long alignment, width; } desc12;
    desc12.dimension = 1;              /* buffer */
    desc12.alignment = 0;
    desc12.width = sizeof(corner) * 3;

    check(dev->CreateCommittedResource(device, 0, 0, &desc12, 0, 0, 0,
                                       (void **)&buffer) == 0 && buffer,
          "and memory to put them in");

    if (!queue || !list || !fence || !buffer) return;

    const d12_list_vtable *cl = (const d12_list_vtable *)list->table;
    const d12_queue_vtable *q = (const d12_queue_vtable *)queue->table;
    const d12_resource_vtable *res = (const d12_resource_vtable *)buffer->table;
    const d12_fence_vtable *fn = (const d12_fence_vtable *)fence->table;

    /* Corners go in through a mapping, which is how a program writes to a
     * resource rather than being handed one already full. */
    static const corner three[3] = {
        {  0.0f,  0.6f, 0.0f, 1.0f,  1.0f, 0.35f, 0.35f, 1.0f },
        {  0.6f, -0.5f, 0.0f, 1.0f,  0.35f, 1.0f, 0.45f, 1.0f },
        { -0.6f, -0.5f, 0.0f, 1.0f,  0.4f, 0.55f, 1.0f, 1.0f },
    };

    void *mapped = 0;
    check(res->Map(buffer, 0, 0, &mapped) == 0 && mapped,
          "the program maps that memory and writes its corners");
    if (mapped) {
        memcpy(mapped, three, sizeof three);
        res->Unmap(buffer, 0, 0);
    }

    /* Record: a clear, then the corners, then a draw.  None of this draws. */
    static const float grey[4] = { 0.1f, 0.12f, 0.16f, 1.0f };
    cl->ClearRenderTargetView(list, 0, grey, 0, 0);

    struct { unsigned long long address; unsigned size, stride; } view;
    view.address = res->GetGPUVirtualAddress(buffer);
    view.size = sizeof(corner) * 3;
    view.stride = sizeof(corner);
    cl->IASetVertexBuffers(list, 0, 1, &view);
    cl->DrawInstanced(list, 3, 1, 0, 0);

    check(cl->Close(list) == 0, "the recorded list closes");

    /* And only now does any of it happen. */
    q->ExecuteCommandLists(queue, 1, &list);
    q->Signal(queue, fence, 1);

    check(fn->GetCompletedValue(fence) == 1,
          "the queue runs the list and the fence says so");

    check(cl->Reset(list, alloc, 0) == 0,
          "and the list resets, ready to record again");

    fn->Release(fence);
    res->Release(buffer);
    cl->Release(list);
    q->Release(queue);
    dev->Release(device);
}

#define UNSUPPORTED ((HRESULT)0x887A0004)

int main(void) {
    d3d_iface *device = 0, *context = 0;
    HRESULT hr = D3DCreateDevice(&device, &context);

    if (hr == UNSUPPORTED) {
        /* No card behind this display.  Being told so is the correct answer
         * and the one a program can do something about. */
        check(device == 0 && context == 0,
              "with no card, no device is handed out either");
        /* The newer interface is asked the same question, because refusing
         * correctly is the behaviour that matters here and it has its own way
         * of doing it. */
        check_d3d12();
        printf("d3dtest: no card here, and the program was told so\n");
        return report("d3dtest");
    }

    check(hr == 0, "a drawing device is created");
    check(device != 0 && context != 0, "and so is something to draw with");
    if (!device || !context) return report("d3dtest");

    const d3d_device_vtable *dev = (const d3d_device_vtable *)device->table;
    const d3d_context_vtable *ctx = (const d3d_context_vtable *)context->table;
    check(dev != 0 && ctx != 0, "each has a table of methods behind it");

    /* The three every interface has, exercised rather than assumed. */
    unsigned refs = dev->AddRef(device);
    check(refs >= 2, "holding on to one is counted");
    refs = dev->Release(device);
    check(refs >= 1, "and letting go of it is counted too");

    /* Corners, put where the pipeline expects them: across and up from the
     * middle, from minus one to one. */
    static const corner triangle[3] = {
        {  0.00f,  0.70f, 0.5f, 1.0f,  1.0f, 0.2f, 0.2f, 1.0f },
        {  0.65f, -0.60f, 0.5f, 1.0f,  0.2f, 1.0f, 0.2f, 1.0f },
        { -0.65f, -0.60f, 0.5f, 1.0f,  0.2f, 0.2f, 1.0f, 1.0f },
    };

    BUFFER_DESC desc;
    memset(&desc, 0, sizeof desc);
    desc.ByteWidth = sizeof triangle;

    SUBRESOURCE initial;
    initial.pSysMem = triangle;
    initial.pitch = 0;
    initial.slice = 0;

    /* The program's own shaders, as its compiler produced them.  Nothing
     * here wrote these instructions: they came out of Microsoft's compiler
     * from ordinary HLSL, and what is handed over is the whole blob, container
     * and all, exactly as a program would hand it to Direct3D. */
    d3d_iface *vshader = 0, *pshader = 0;
    hr = dev->CreateVertexShader(device, shader_vertex_blob,
                                 sizeof shader_vertex_blob, 0, &vshader);
    check(hr == 0 && vshader != 0,
          "a shader compiled elsewhere is taken from its container");
    hr = dev->CreatePixelShader(device, shader_pixel_blob,
                                sizeof shader_pixel_blob, 0, &pshader);
    check(hr == 0 && pshader != 0, "and so is the one that colours pixels");

    if (vshader && pshader) {
        ctx->VSSetShader(context, vshader, 0, 0);
        ctx->PSSetShader(context, pshader, 0, 0);
    }

    /* And the arrangement of a vertex, described rather than assumed.  These
     * names are the ones the shader uses; nothing here says which input each
     * becomes, because the shader already said. */
    static const INPUT_ELEMENT arrangement[2] = {
        { "POSITION", 0, FORMAT_FOUR_FLOATS, 0,  0, 0, 0 },
        { "COLOR",    0, FORMAT_FOUR_FLOATS, 0, 16, 0, 0 },
    };
    d3d_iface *layout = 0;
    hr = dev->CreateInputLayout(device, arrangement, 2, shader_vertex_blob,
                                sizeof shader_vertex_blob, &layout);
    check(hr == 0 && layout != 0,
          "the program describes how its vertices are laid out");
    if (layout) ctx->IASetInputLayout(context, layout);

    d3d_iface *buffer = 0;
    hr = dev->CreateBuffer(device, &desc, &initial, &buffer);
    check(hr == 0 && buffer != 0, "the device makes a buffer of corners");

    if (buffer) {
        unsigned stride = sizeof(corner), offset = 0;
        ctx->SetVertexBuffers(context, 0, 1, &buffer, &stride, &offset);
        ctx->Draw(context, 3, 0);
        check(ctx->Present(context, 0, 0) == 0, "and the card draws them");

        d3d_iface *b = buffer;
        const d3d_device_vtable *bt = (const d3d_device_vtable *)b->table;
        check(bt->Release(b) == 0, "and the buffer is given back");
    }

    if (layout) { const d3d_device_vtable *t = (const d3d_device_vtable *)layout->table; t->Release(layout); }
    if (vshader) { const d3d_device_vtable *t = (const d3d_device_vtable *)vshader->table; t->Release(vshader); }
    if (pshader) { const d3d_device_vtable *t = (const d3d_device_vtable *)pshader->table; t->Release(pshader); }
    ctx->Release(context);
    dev->Release(device);

    check_d3d12();
    return report("d3dtest");
}
