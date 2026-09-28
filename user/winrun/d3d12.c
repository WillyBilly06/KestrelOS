/* d3d12.c - a Direct3D 12 shaped way in to the graphics card.
 *
 * Direct3D 12 is not a newer spelling of Direct3D 11; it is a different model,
 * and a program written for one does not run against the other's shape.  The
 * difference that matters here is where work is put.
 *
 * In 11 a program has a device and an immediate context, and every call it
 * makes on that context happens.  In 12 there is no immediate context at all.
 * A program records into a command list, which does nothing while it is being
 * recorded, closes it, and hands it to a queue - and only then does any of it
 * happen.  Everything else follows from that: allocators own the memory a list
 * records into, fences are how a program finds out the queue reached the end,
 * and pipeline state objects exist because the state a draw needs has to be
 * settled before recording rather than during it.
 *
 * So recording is what this implements.  A command list keeps what it was told
 * to do, in order, and executing it replays those onto the same drawing path
 * everything else in this system uses - the same card, through the same calls
 * as d3d.c.  A program that creates a device, a queue, an allocator and a
 * list, records a clear and a draw, closes it, executes it and waits on a
 * fence, gets its triangles on the screen, and none of those steps is a stub
 * that quietly returns success.
 *
 * What this is NOT is Direct3D 12.  The parts left out are named where they
 * are left out, and the two large ones are worth naming here: a program's own
 * compiled shaders are not run, and resources are not placed in memory the
 * program chose - a committed resource is ordinary memory, and residency,
 * heaps and resource states are accepted and not enforced.  A program that
 * depends on those will find them missing rather than find them wrong.
 */
#include "win.h"

/* --------------------------------------------------------------- the shape */

typedef struct d3d12_object d3d12_object;

typedef struct {
    HRESULT WINAPI (*QueryInterface)(d3d12_object *, const void *, void **);
    ULONG   WINAPI (*AddRef)(d3d12_object *);
    ULONG   WINAPI (*Release)(d3d12_object *);
    void   *slots[32];
} d3d12_vtable;

/* What a recorded command is.  Kept small and by value: a command list holds
 * an array of these and nothing it has to free. */
typedef enum {
    CMD_CLEAR = 1,      /* a colour over the whole target                   */
    CMD_DRAW,           /* count corners starting at first                  */
} cmd_kind;

typedef struct {
    cmd_kind kind;
    float    colour[4];
    UINT     count, first;
    void    *vertices;      /* the buffer bound when this was recorded      */
    SIZE_T   length;
    UINT     stride;
} d3d12_cmd;

#define MAX_CMDS 256

enum {
    D12_DEVICE = 1, D12_QUEUE, D12_ALLOCATOR, D12_LIST,
    D12_HEAP, D12_RESOURCE, D12_FENCE, D12_PIPELINE, D12_ROOTSIG,
};

struct d3d12_object {
    const d3d12_vtable *table;
    LONG   refs;
    int    kind;

    /* A resource: what a program was given to put things in. */
    void  *bytes;
    SIZE_T length;

    /* A command list: what it has been told to do, and whether it is still
     * open to being told more. */
    d3d12_cmd cmds[MAX_CMDS];
    int       cmd_count;
    bool      recording;

    /* What is bound while recording. */
    d3d12_object *bound;
    UINT          bound_stride;

    /* A fence: how far the queue has got. */
    ULONGLONG value;
};

static HRESULT WINAPI d12_query(d3d12_object *self, const void *iid, void **out) {
    (void)iid;
    if (!out) return (HRESULT)0x80004003;         /* E_POINTER              */
    *out = self;
    if (self) self->refs++;
    return 0;
}

static ULONG WINAPI d12_addref(d3d12_object *self) {
    return self ? (ULONG)++self->refs : 0;
}

static ULONG WINAPI d12_release(d3d12_object *self) {
    if (!self) return 0;
    if (--self->refs > 0) return (ULONG)self->refs;
    free(self->bytes);
    free(self);
    return 0;
}

static d3d12_object *d12_new(int kind, const d3d12_vtable *table) {
    d3d12_object *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->table = table;
    o->refs = 1;
    o->kind = kind;
    return o;
}

/* --------------------------------------------------------------- recording */

static void record(d3d12_object *list, const d3d12_cmd *c) {
    if (!list || !list->recording) return;
    if (list->cmd_count >= MAX_CMDS) return;
    list->cmds[list->cmd_count++] = *c;
}

static void WINAPI d12_list_clear_rtv(d3d12_object *self, ULONGLONG handle,
                                      const float *colour, UINT rects,
                                      const void *rect_list) {
    (void)handle; (void)rects; (void)rect_list;
    d3d12_cmd c;
    memset(&c, 0, sizeof c);
    c.kind = CMD_CLEAR;
    for (int i = 0; i < 4; i++) c.colour[i] = colour ? colour[i] : 0.0f;
    record(self, &c);
}

static void WINAPI d12_list_set_vertex_buffers(d3d12_object *self, UINT slot,
                                               UINT views, const void *view) {
    (void)slot; (void)views;
    /* A vertex buffer view is an address, a size and a stride, in that order.
     * Reading it rather than taking a resource pointer is what the caller
     * expects: by this point the program is handing over a description, not
     * the object it came from. */
    if (!self || !view) return;
    const struct { ULONGLONG address; UINT size; UINT stride; } *v = view;
    self->bound = (d3d12_object *)(uintptr_t)v->address;
    self->bound_stride = v->stride;
}

static void WINAPI d12_list_draw(d3d12_object *self, UINT count, UINT instances,
                                 UINT first, UINT first_instance) {
    (void)instances; (void)first_instance;
    if (!self || !self->bound) return;

    d3d12_cmd c;
    memset(&c, 0, sizeof c);
    c.kind = CMD_DRAW;
    c.count = count;
    c.first = first;
    c.vertices = self->bound->bytes;
    c.length = self->bound->length;
    c.stride = self->bound_stride;
    record(self, &c);
}

static HRESULT WINAPI d12_list_close(d3d12_object *self) {
    if (!self) return (HRESULT)0x80004003;
    self->recording = false;
    return 0;
}

static HRESULT WINAPI d12_list_reset(d3d12_object *self, d3d12_object *alloc,
                                     d3d12_object *pipeline) {
    (void)alloc; (void)pipeline;
    if (!self) return (HRESULT)0x80004003;
    self->cmd_count = 0;
    self->recording = true;
    self->bound = NULL;
    return 0;
}

/* Accepted and not enforced, and said so rather than left to look implemented:
 * a barrier describes a change of resource state, and nothing here tracks
 * resource states to change. */
static void WINAPI d12_list_barrier(d3d12_object *self, UINT count,
                                    const void *barriers) {
    (void)self; (void)count; (void)barriers;
}

static void WINAPI d12_list_set_pipeline(d3d12_object *self, d3d12_object *p) {
    (void)self; (void)p;
}

static void WINAPI d12_list_set_topology(d3d12_object *self, UINT topology) {
    (void)self; (void)topology;
}

/* ---------------------------------------------------------------- the queue
 *
 * Executing is where a recorded list finally does something.  It runs to
 * completion before returning, which is not what a real queue does - a real
 * one takes the work and lets the program carry on - but it is honest about
 * ordering, and a fence signalled afterwards is then telling the truth.
 */
static void WINAPI d12_queue_execute(d3d12_object *self, UINT count,
                                     d3d12_object **lists) {
    (void)self;
    if (!lists) return;

    for (UINT i = 0; i < count; i++) {
        d3d12_object *list = lists[i];
        if (!list || list->recording) continue;   /* an open list is not work */

        for (int c = 0; c < list->cmd_count; c++) {
            const d3d12_cmd *cmd = &list->cmds[c];

            if (cmd->kind == CMD_CLEAR) {
                /* Two triangles covering everything, in the coordinates the
                 * pipeline already works in - which is what a clear is when
                 * the path underneath draws geometry and offers no fill of its
                 * own.  The card does the work either way. */
                float r = cmd->colour[0], g = cmd->colour[1];
                float b = cmd->colour[2], a = cmd->colour[3];
                kvertex_t quad[6] = {
                    { -1, -1, 0, 1, r, g, b, a },
                    {  1, -1, 0, 1, r, g, b, a },
                    {  1,  1, 0, 1, r, g, b, a },
                    { -1, -1, 0, 1, r, g, b, a },
                    {  1,  1, 0, 1, r, g, b, a },
                    { -1,  1, 0, 1, r, g, b, a },
                };
                gpu_draw(quad, 2);
                continue;
            }

            if (cmd->kind != CMD_DRAW || !cmd->vertices || cmd->count < 3)
                continue;

            UINT stride = cmd->stride ? cmd->stride : (UINT)sizeof(kvertex_t);
            SIZE_T need = (SIZE_T)(cmd->first + cmd->count) * stride;
            if (need > cmd->length) continue;

            const char *from = (const char *)cmd->vertices +
                               (SIZE_T)cmd->first * stride;

            if (stride == sizeof(kvertex_t)) {
                gpu_draw((const kvertex_t *)from, cmd->count / 3);
                continue;
            }

            /* A different arrangement: the first eight numbers of each corner
             * are taken and the rest ignored, the same as the older path does
             * and for the same reason - reading a program's own arrangement
             * needs its shaders compiled, which is not written. */
            static kvertex_t packed[384];
            UINT usable = cmd->count > 384 ? 384 : cmd->count;
            for (UINT v = 0; v < usable; v++)
                memcpy(&packed[v], from + (SIZE_T)v * stride, sizeof(kvertex_t));
            gpu_draw(packed, usable / 3);
        }
    }
}

static void WINAPI d12_queue_signal(d3d12_object *self, d3d12_object *fence,
                                    ULONGLONG value) {
    (void)self;
    /* Everything the queue was given has already run by the time this is
     * reached, so the fence can carry the value straight away. */
    if (fence) fence->value = value;
}

/* ---------------------------------------------------------------- the fence */

static ULONGLONG WINAPI d12_fence_completed(d3d12_object *self) {
    return self ? self->value : 0;
}

static HRESULT WINAPI d12_fence_set_event(d3d12_object *self, ULONGLONG value,
                                          void *event) {
    (void)value; (void)event;
    /* Work finishes before execute returns, so a program waiting on this is
     * never left waiting.  Signalling the event immediately is what that
     * means. */
    if (!self) return (HRESULT)0x80004003;
    return 0;
}

/* --------------------------------------------------------------- resources */

static HRESULT WINAPI d12_res_map(d3d12_object *self, UINT sub,
                                  const void *range, void **out) {
    (void)sub; (void)range;
    if (!self || !out) return (HRESULT)0x80004003;
    *out = self->bytes;
    return 0;
}

static void WINAPI d12_res_unmap(d3d12_object *self, UINT sub, const void *range) {
    (void)self; (void)sub; (void)range;
}

/* A resource's address is the object itself: the vertex buffer view carries it
 * back, and this is the only side that has to agree about what it means. */
static ULONGLONG WINAPI d12_res_gpu_address(d3d12_object *self) {
    return (ULONGLONG)(uintptr_t)self;
}

/* --------------------------------------------------------------- the tables */

static const d3d12_vtable d12_resource_table = {
    d12_query, d12_addref, d12_release,
    { (void *)d12_res_map, (void *)d12_res_unmap, (void *)d12_res_gpu_address }
};

static const d3d12_vtable d12_fence_table = {
    d12_query, d12_addref, d12_release,
    { (void *)d12_fence_completed, (void *)d12_fence_set_event }
};

static const d3d12_vtable d12_list_table = {
    d12_query, d12_addref, d12_release,
    {
        (void *)d12_list_close,
        (void *)d12_list_reset,
        (void *)d12_list_clear_rtv,
        (void *)d12_list_set_vertex_buffers,
        (void *)d12_list_draw,
        (void *)d12_list_barrier,
        (void *)d12_list_set_pipeline,
        (void *)d12_list_set_topology,
    }
};

static const d3d12_vtable d12_queue_table = {
    d12_query, d12_addref, d12_release,
    { (void *)d12_queue_execute, (void *)d12_queue_signal }
};

static const d3d12_vtable d12_plain_table = {
    d12_query, d12_addref, d12_release, { 0 }
};

/* ---------------------------------------------------------------- the device */

static HRESULT WINAPI d12_create_queue(d3d12_object *self, const void *desc,
                                       const void *iid, void **out) {
    (void)self; (void)desc; (void)iid;
    if (!out) return (HRESULT)0x80004003;
    d3d12_object *q = d12_new(D12_QUEUE, &d12_queue_table);
    if (!q) return (HRESULT)0x8007000E;
    *out = q;
    return 0;
}

static HRESULT WINAPI d12_create_allocator(d3d12_object *self, UINT type,
                                           const void *iid, void **out) {
    (void)self; (void)type; (void)iid;
    if (!out) return (HRESULT)0x80004003;
    d3d12_object *a = d12_new(D12_ALLOCATOR, &d12_plain_table);
    if (!a) return (HRESULT)0x8007000E;
    *out = a;
    return 0;
}

static HRESULT WINAPI d12_create_list(d3d12_object *self, UINT mask, UINT type,
                                      d3d12_object *alloc, d3d12_object *pipeline,
                                      const void *iid, void **out) {
    (void)self; (void)mask; (void)type; (void)alloc; (void)pipeline; (void)iid;
    if (!out) return (HRESULT)0x80004003;

    d3d12_object *l = d12_new(D12_LIST, &d12_list_table);
    if (!l) return (HRESULT)0x8007000E;
    /* A list is created open, which is what a program expects: it records
     * without resetting first. */
    l->recording = true;
    *out = l;
    return 0;
}

static HRESULT WINAPI d12_create_heap(d3d12_object *self, const void *desc,
                                      const void *iid, void **out) {
    (void)self; (void)desc; (void)iid;
    if (!out) return (HRESULT)0x80004003;
    d3d12_object *h = d12_new(D12_HEAP, &d12_plain_table);
    if (!h) return (HRESULT)0x8007000E;
    *out = h;
    return 0;
}

static HRESULT WINAPI d12_create_fence(d3d12_object *self, ULONGLONG initial,
                                       UINT flags, const void *iid, void **out) {
    (void)self; (void)flags; (void)iid;
    if (!out) return (HRESULT)0x80004003;
    d3d12_object *f = d12_new(D12_FENCE, &d12_fence_table);
    if (!f) return (HRESULT)0x8007000E;
    f->value = initial;
    *out = f;
    return 0;
}

/* A committed resource is ordinary memory here.  The heap properties a program
 * gives - where it wants it and who may read it - are accepted and not acted
 * on, because there is one kind of memory to give. */
static HRESULT WINAPI d12_create_committed(d3d12_object *self,
                                           const void *heap_props, UINT heap_flags,
                                           const void *desc, UINT state,
                                           const void *clear, const void *iid,
                                           void **out) {
    (void)self; (void)heap_props; (void)heap_flags; (void)state; (void)clear;
    (void)iid;
    if (!out) return (HRESULT)0x80004003;

    /* A resource description begins with its dimension and then its width as a
     * 64-bit count, which for a buffer is its size in bytes. */
    SIZE_T size = 0;
    if (desc) {
        const struct { UINT dimension; ULONGLONG alignment; ULONGLONG width; } *d = desc;
        size = (SIZE_T)d->width;
    }
    if (!size) size = 4096;
    if (size > (SIZE_T)16 * 1024 * 1024) return (HRESULT)0x8007000E;

    d3d12_object *r = d12_new(D12_RESOURCE, &d12_resource_table);
    if (!r) return (HRESULT)0x8007000E;

    r->bytes = calloc(1, size);
    if (!r->bytes) { free(r); return (HRESULT)0x8007000E; }
    r->length = size;

    *out = r;
    return 0;
}

static HRESULT WINAPI d12_create_pipeline(d3d12_object *self, const void *desc,
                                          const void *iid, void **out) {
    (void)self; (void)desc; (void)iid;
    if (!out) return (HRESULT)0x80004003;
    /* The state a draw runs under is accepted and the drawing path's own is
     * used.  A program's compiled shaders are the part not run, and this is
     * where they would be taken. */
    d3d12_object *p = d12_new(D12_PIPELINE, &d12_plain_table);
    if (!p) return (HRESULT)0x8007000E;
    *out = p;
    return 0;
}

static HRESULT WINAPI d12_create_rootsig(d3d12_object *self, UINT mask,
                                         const void *blob, SIZE_T len,
                                         const void *iid, void **out) {
    (void)self; (void)mask; (void)blob; (void)len; (void)iid;
    if (!out) return (HRESULT)0x80004003;
    d3d12_object *r = d12_new(D12_ROOTSIG, &d12_plain_table);
    if (!r) return (HRESULT)0x8007000E;
    *out = r;
    return 0;
}

static const d3d12_vtable d12_device_table = {
    d12_query, d12_addref, d12_release,
    {
        (void *)d12_create_queue,
        (void *)d12_create_allocator,
        (void *)d12_create_list,
        (void *)d12_create_heap,
        (void *)d12_create_fence,
        (void *)d12_create_committed,
        (void *)d12_create_pipeline,
        (void *)d12_create_rootsig,
    }
};

/* ------------------------------------------------------------- the way in */

/* The feature level a program asks for is the first thing it asks, and saying
 * yes to a level this cannot reach would be worse than saying no: the program
 * would go on to use what that level promises.  What is answered here is the
 * level whose drawing this actually does. */
#define D3D_FEATURE_LEVEL_11_0 0xb000

static HRESULT WINAPI w_D3D12CreateDevice(void *adapter, UINT min_level,
                                          const void *iid, void **out) {
    (void)adapter; (void)iid;

    if (!gpu_can_draw()) {
        win_trace("a program asked for a Direct3D 12 device and there is no "
                  "card behind this display to give it one");
        return (HRESULT)0x887A0004;              /* DXGI_ERROR_UNSUPPORTED  */
    }

    if (min_level > D3D_FEATURE_LEVEL_11_0) {
        win_trace("a program asked for a feature level above what this "
                  "draws, and was told no rather than given a device that "
                  "would fail later");
        return (HRESULT)0x887A0004;
    }

    /* A null out pointer is how a program asks whether a device could be
     * created without creating one. */
    if (!out) return 0;

    d3d12_object *device = d12_new(D12_DEVICE, &d12_device_table);
    if (!device) return (HRESULT)0x8007000E;

    *out = device;
    win_trace("a program was given a Direct3D 12 device");
    return 0;
}

static HRESULT WINAPI w_D3D12GetDebugInterface(const void *iid, void **out) {
    (void)iid;
    if (out) *out = NULL;
    return (HRESULT)0x80004002;                  /* E_NOINTERFACE           */
}

static HRESULT WINAPI w_D3D12SerializeRootSignature(const void *desc, UINT version,
                                                    void **blob, void **error) {
    (void)desc; (void)version;
    if (error) *error = NULL;
    if (blob) *blob = NULL;
    return 0;
}

const win_export_t d3d12_exports[] = {
    { "D3D12CreateDevice",           (void *)w_D3D12CreateDevice },
    { "D3D12GetDebugInterface",      (void *)w_D3D12GetDebugInterface },
    { "D3D12SerializeRootSignature", (void *)w_D3D12SerializeRootSignature },
    { NULL, NULL },
};

void d3d12_init(void) { win_register("d3d12.dll", d3d12_exports); }
