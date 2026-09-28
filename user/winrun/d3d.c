/* d3d.c - a Direct3D-shaped way in to the graphics card.
 *
 * Everything a Windows program does to draw goes through interfaces rather
 * than functions: it asks for a device, and what comes back is a pointer to a
 * pointer to a table of function pointers.  Calling a method means loading the
 * table, loading the slot, and calling it with the object as the first
 * argument.  That arrangement is the whole of COM, and a program built against
 * Direct3D does not work without it - not because the drawing is hard but
 * because the shape is.
 *
 * So the shape is what this provides, on top of the drawing that already
 * works.  A program creates a device, makes a buffer of corners, hands it
 * over and asks for it to be drawn, and the card draws it - the same card,
 * through the same path, as everything else in this system that draws.
 *
 * What this is NOT is Direct3D.  It is the small part of its shape that lets
 * a program get geometry onto the screen, and the two things it leaves out are
 * named where they are left out: a program's own shaders are not compiled, and
 * the surface drawn into is the screen rather than one the program chose.
 */
#include "win.h"

/* --------------------------------------------------------------- the shape */

/* Every interface begins with a pointer to its table, and every method takes
 * the object it was called on first.  Both of those are the caller's
 * expectation rather than a choice made here. */
typedef struct d3d_object d3d_object;

typedef struct {
    /* The three every interface has, whatever else it has. */
    HRESULT WINAPI (*QueryInterface)(d3d_object *, const void *, void **);
    ULONG   WINAPI (*AddRef)(d3d_object *);
    ULONG   WINAPI (*Release)(d3d_object *);
    /* And what this one adds. */
    void   *slots[24];
} d3d_vtable;

struct d3d_object {
    const d3d_vtable *table;
    LONG   refs;
    int    kind;
    /* A buffer of corners: where they are and how many. */
    void  *bytes;
    SIZE_T length;
    UINT   stride;
};

#define D3D_DEVICE  1
#define D3D_CONTEXT 2
#define D3D_BUFFER  3
#define D3D_SHADER  4
#define D3D_LAYOUT  5

static HRESULT WINAPI d3d_query(d3d_object *self, const void *iid, void **out) {
    (void)iid;
    if (!out) return (HRESULT)0x80070057;        /* E_INVALIDARG */
    /* Every interface here is the same object seen from a different angle,
     * which is true of a device and its context in practice as well. */
    *out = self;
    self->refs++;
    return 0;
}

static ULONG WINAPI d3d_addref(d3d_object *self) {
    return (ULONG)++self->refs;
}

static ULONG WINAPI d3d_release(d3d_object *self) {
    if (--self->refs > 0) return (ULONG)self->refs;
    if (self->kind == D3D_BUFFER || self->kind == D3D_SHADER) free(self->bytes);
    free(self);
    return 0;
}

static d3d_object *d3d_new(int kind, const d3d_vtable *table) {
    d3d_object *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->table = table;
    o->refs = 1;
    o->kind = kind;
    return o;
}

/* ------------------------------------------------------------- the drawing */

/* What the program last said its corners were, so that the draw has something
 * to draw.  A real device keeps this per context; there is one context here. */
static d3d_object *bound_corners;
static UINT bound_stride;

/* A description of a buffer, as the caller fills it in.  Only the length and
 * the first bytes matter here. */
typedef struct {
    UINT   ByteWidth;
    UINT   Usage;
    UINT   BindFlags;
    UINT   CPUAccessFlags;
    UINT   MiscFlags;
    UINT   StructureByteStride;
} D3D_BUFFER_DESC;

typedef struct {
    const void *pSysMem;
    UINT SysMemPitch;
    UINT SysMemSlicePitch;
} D3D_SUBRESOURCE_DATA;

static HRESULT WINAPI d3d_create_buffer(d3d_object *self,
                                        const D3D_BUFFER_DESC *desc,
                                        const D3D_SUBRESOURCE_DATA *initial,
                                        d3d_object **out) {
    (void)self;
    if (!desc || !out) return (HRESULT)0x80070057;

    extern const d3d_vtable d3d_buffer_table;
    d3d_object *b = d3d_new(D3D_BUFFER, &d3d_buffer_table);
    if (!b) return (HRESULT)0x8007000E;          /* E_OUTOFMEMORY */

    b->length = desc->ByteWidth;
    b->bytes = malloc(desc->ByteWidth ? desc->ByteWidth : 1);
    if (!b->bytes) { free(b); return (HRESULT)0x8007000E; }
    if (initial && initial->pSysMem)
        memcpy(b->bytes, initial->pSysMem, desc->ByteWidth);
    else
        memset(b->bytes, 0, desc->ByteWidth);

    *out = b;
    return 0;
}

static void WINAPI d3d_set_buffers(d3d_object *self, UINT start, UINT count,
                                   d3d_object *const *buffers,
                                   const UINT *strides, const UINT *offsets) {
    (void)self; (void)start; (void)offsets;
    if (!count || !buffers || !buffers[0]) return;
    bound_corners = buffers[0];
    bound_stride = strides ? strides[0] : 0;
}

/* One corner as this system's graphics understand it: where it is and what
 * colour it carries. */
typedef struct { float x, y, z, w, r, g, b, a; } d3d_corner;

static void WINAPI d3d_draw(d3d_object *self, UINT count, UINT first) {
    (void)self;
    if (!bound_corners || !bound_corners->bytes) return;
    if (count < 3) return;

    UINT stride = bound_stride ? bound_stride : (UINT)sizeof(d3d_corner);
    SIZE_T need = (SIZE_T)(first + count) * stride;
    if (need > bound_corners->length) return;

    /* The corners are handed down as they are.  A program using a different
     * arrangement of its own would need its shaders compiled to read that
     * arrangement, which is the piece not written - so what is drawn is what
     * this system's own drawing understands. */
    const char *from = (const char *)bound_corners->bytes + (SIZE_T)first * stride;
    if (stride == sizeof(d3d_corner)) {
        gpu_draw((const kvertex_t *)from, count / 3);
        return;
    }

    /* A different stride means the extra is ignored and the first eight
     * numbers of each corner are taken. */
    static d3d_corner packed[128];
    UINT usable = count > 384 ? 384 : count;
    for (UINT i = 0; i < usable && i < 384; i++)
        memcpy(&packed[i], from + (SIZE_T)i * stride, sizeof(d3d_corner));
    gpu_draw((const kvertex_t *)packed, usable / 3);
}

static HRESULT WINAPI d3d_present(d3d_object *self, UINT interval, UINT flags) {
    (void)self; (void)interval; (void)flags;
    /* Drawing already shows: the surface drawn into is the one the display
     * reads.  A device with a chain of surfaces to swap between would have
     * something to do here. */
    return 0;
}

/* -------------------------------------------------- a program's own shaders
 *
 * What a program hands over is not instructions but a container: a header, a
 * checksum, and a list of chunks, only one of which is the shader itself.  The
 * others describe what it takes and gives, what constants it expects, and how
 * many of each instruction it contains - useful to a debugger and not needed
 * here.  Finding the one that matters is a walk down the list.
 */
typedef struct {
    char     magic[4];               /* "DXBC" */
    unsigned char checksum[16];
    UINT     one;
    UINT     total;
    UINT     chunks;
    /* followed by one offset per chunk */
} dxbc_header;

/* Where the instructions are inside the blob, and how many words of them. */
static const UINT *dxbc_instructions(const void *blob, SIZE_T length,
                                     UINT *words_out) {
    if (!blob || length < sizeof(dxbc_header)) return NULL;
    const dxbc_header *h = (const dxbc_header *)blob;
    if (h->magic[0] != 'D' || h->magic[1] != 'X' ||
        h->magic[2] != 'B' || h->magic[3] != 'C') return NULL;
    if (h->total > length || h->chunks > 32) return NULL;

    const UINT *offsets = (const UINT *)((const char *)blob + sizeof *h);
    for (UINT i = 0; i < h->chunks; i++) {
        UINT at = offsets[i];
        if (at + 8 > length) continue;
        const char *tag = (const char *)blob + at;
        UINT size = *(const UINT *)(tag + 4);
        if (at + 8 + size > length) continue;

        /* The instructions, under either of the two names the compiler has
         * used for them over the years. */
        if ((tag[0] == 'S' && tag[1] == 'H' && tag[2] == 'D' && tag[3] == 'R') ||
            (tag[0] == 'S' && tag[1] == 'H' && tag[2] == 'E' && tag[3] == 'X')) {
            const UINT *tokens = (const UINT *)(tag + 8);
            /* The second word is the length in words, counting the two of
             * heading - which is what this system wants handed to it. */
            UINT count = tokens[1];
            if (count < 2 || count * 4 > size) return NULL;
            *words_out = count;
            return tokens;
        }
    }
    return NULL;
}

/* What a program is handed and hands on, read out of the blob.
 *
 * The compiler writes this down beside the instructions - which register each
 * value arrives in, and whether it means something to the pipeline itself.
 * Reading it rather than assuming it is what lets a shader that is not shaped
 * like the ones this system writes for itself work at all. */
/* The names alongside the registers, so that a program describing its vertices
 * by name can be told which input each one arrives as. */
static char sig_names[KSIG_MAX][32];
static UINT sig_name_regs[KSIG_MAX];
static UINT sig_name_count;

static UINT dxbc_signature(const void *blob, SIZE_T length, const char *which,
                           ksigline_t *out, UINT room) {
    if (!blob || length < sizeof(dxbc_header)) return 0;
    const dxbc_header *h = (const dxbc_header *)blob;
    if (h->chunks > 32) return 0;
    const UINT *offsets = (const UINT *)((const char *)blob + sizeof *h);

    for (UINT i = 0; i < h->chunks; i++) {
        UINT at = offsets[i];
        if (at + 8 > length) continue;
        const char *tag = (const char *)blob + at;
        if (tag[0] != which[0] || tag[1] != which[1] ||
            tag[2] != which[2] || tag[3] != which[3]) continue;

        const char *body = tag + 8;
        UINT count = *(const UINT *)body;
        if (count > room) count = room;
        for (UINT e = 0; e < count; e++) {
            /* Each line is six words: where its name is, which of that name,
             * what it means, what kind of number, which register, and which
             * parts of it.  Only two of those are needed here. */
            const UINT *line = (const UINT *)(body + 8 + (SIZE_T)e * 24);
            out[e].meaning = line[2];        /* nothing, or a position, ... */
            out[e].reg = line[4];

            /* Only what a program is handed is worth naming: that is what it
             * describes its vertices against. */
            if (which[1] == 'S' && e < KSIG_MAX) {
                const char *name = body + line[0];
                UINT n = 0;
                while (n < sizeof sig_names[0] - 1 && name[n]) {
                    sig_names[e][n] = name[n];
                    n++;
                }
                sig_names[e][n] = 0;
                sig_name_regs[e] = line[4];
                if (e + 1 > sig_name_count) sig_name_count = e + 1;
            }
        }
        return count;
    }
    return 0;
}

/* The two a program supplies, kept until it asks for both to be used. */
static const UINT *held_vertex, *held_pixel;
static UINT held_vertex_words, held_pixel_words;
static ksigline_t held_vs_takes[KSIG_MAX], held_vs_gives[KSIG_MAX];
static ksigline_t held_ps_takes[KSIG_MAX], held_ps_gives[KSIG_MAX];
static UINT held_vs_takes_n, held_vs_gives_n, held_ps_takes_n, held_ps_gives_n;

static HRESULT WINAPI d3d_create_shader(d3d_object *self, const void *blob,
                                        SIZE_T length, void *unused,
                                        d3d_object **out, int is_pixel) {
    (void)self; (void)unused;
    UINT words = 0;
    const UINT *tokens = dxbc_instructions(blob, length, &words);
    if (!tokens) {
        win_trace("a program handed over something that is not a shader");
        return (HRESULT)0x80070057;
    }

    extern const d3d_vtable d3d_buffer_table;
    d3d_object *sh = d3d_new(D3D_SHADER, &d3d_buffer_table);
    if (!sh) return (HRESULT)0x8007000E;

    /* Kept as it arrived; the program owns the blob and may free it. */
    sh->bytes = malloc(words * sizeof(UINT));
    if (!sh->bytes) { free(sh); return (HRESULT)0x8007000E; }
    memcpy(sh->bytes, tokens, words * sizeof(UINT));
    sh->length = words;
    sh->stride = (UINT)is_pixel;

    /* And what it expects, taken from the same blob while it is still here. */
    if (is_pixel) {
        held_ps_takes_n = dxbc_signature(blob, length, "ISGN", held_ps_takes,
                                         KSIG_MAX);
        held_ps_gives_n = dxbc_signature(blob, length, "OSGN", held_ps_gives,
                                         KSIG_MAX);
    } else {
        held_vs_takes_n = dxbc_signature(blob, length, "ISGN", held_vs_takes,
                                         KSIG_MAX);
        held_vs_gives_n = dxbc_signature(blob, length, "OSGN", held_vs_gives,
                                         KSIG_MAX);
    }

    if (out) *out = sh; else d3d_release(sh);
    return 0;
}

static HRESULT WINAPI d3d_create_vertex_shader(d3d_object *self,
                                               const void *blob, SIZE_T length,
                                               void *unused, d3d_object **out) {
    return d3d_create_shader(self, blob, length, unused, out, 0);
}

static HRESULT WINAPI d3d_create_pixel_shader(d3d_object *self,
                                              const void *blob, SIZE_T length,
                                              void *unused, d3d_object **out) {
    return d3d_create_shader(self, blob, length, unused, out, 1);
}

/* Choosing one to use.  Both have to be known before either can be handed
 * over, because the card is told about them together. */
static void d3d_use_shaders(void) {
    if (!held_vertex || !held_pixel) return;
    kshaders_t both;
    memset(&both, 0, sizeof both);
    both.vertex = held_vertex;
    both.vertex_words = held_vertex_words;
    both.pixel = held_pixel;
    both.pixel_words = held_pixel_words;
    memcpy(both.vertex_takes, held_vs_takes, sizeof both.vertex_takes);
    memcpy(both.vertex_gives, held_vs_gives, sizeof both.vertex_gives);
    memcpy(both.pixel_takes, held_ps_takes, sizeof both.pixel_takes);
    memcpy(both.pixel_gives, held_ps_gives, sizeof both.pixel_gives);
    both.vertex_takes_count = held_vs_takes_n;
    both.vertex_gives_count = held_vs_gives_n;
    both.pixel_takes_count = held_ps_takes_n;
    both.pixel_gives_count = held_ps_gives_n;
    if (gpu_shaders(&both) != 0)
        win_trace("the card would not take the program's own shaders");
}

static void WINAPI d3d_set_vertex_shader(d3d_object *self, d3d_object *shader,
                                         void *classes, UINT count) {
    (void)self; (void)classes; (void)count;
    if (!shader || !shader->bytes) return;
    held_vertex = (const UINT *)shader->bytes;
    held_vertex_words = (UINT)shader->length;
    d3d_use_shaders();
}

static void WINAPI d3d_set_pixel_shader(d3d_object *self, d3d_object *shader,
                                        void *classes, UINT count) {
    (void)self; (void)classes; (void)count;
    if (!shader || !shader->bytes) return;
    held_pixel = (const UINT *)shader->bytes;
    held_pixel_words = (UINT)shader->length;
    d3d_use_shaders();
}

/* ---------------------------------------------- a program's own vertices
 *
 * A program says what is inside a vertex by naming each thing the way its
 * shader names it.  Turning that into something the card understands means two
 * translations: the kind of numbers, which both sides number differently, and
 * the name, which has to become the input the shader actually reads it as.
 * The second is why the signature was worth reading - without it there is
 * nothing to match a name against.
 */
typedef struct {
    const char *SemanticName;
    UINT SemanticIndex;
    UINT Format;
    UINT InputSlot;
    UINT AlignedByteOffset;
    UINT InputSlotClass;
    UINT InstanceDataStepRate;
} INPUT_ELEMENT_DESC;

/* How wide each kind is, and what this system calls it. */
static UINT format_from_dxgi(UINT dxgi, UINT *bytes) {
    switch (dxgi) {
    case 2:  *bytes = 16; return 122;   /* four numbers  */
    case 6:  *bytes = 12; return 50;    /* three         */
    case 16: *bytes = 8;  return 125;   /* two           */
    case 41: *bytes = 4;  return 131;   /* one           */
    case 28: *bytes = 4;  return 68;    /* four bytes    */
    default: *bytes = 0;  return 0;
    }
}

static int same_name(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static HRESULT WINAPI d3d_create_input_layout(d3d_object *self,
                                              const INPUT_ELEMENT_DESC *descs,
                                              UINT count, const void *blob,
                                              SIZE_T length,
                                              d3d_object **out) {
    (void)self; (void)blob; (void)length;
    if (!descs || !count || count > KLAYOUT_MAX) return (HRESULT)0x80070057;

    klayout_t layout;
    memset(&layout, 0, sizeof layout);

    UINT furthest = 0;
    for (UINT i = 0; i < count; i++) {
        UINT wide = 0;
        UINT format = format_from_dxgi(descs[i].Format, &wide);
        if (!format) {
            win_trace("a program described a vertex using a kind of number "
                      "this does not know (%u)", descs[i].Format);
            return (HRESULT)0x80070057;
        }

        /* Which input the shader reads this as, found by its name rather than
         * by the order it happens to be written in. */
        UINT reg = i;
        int found = 0;
        for (UINT k = 0; k < sig_name_count; k++) {
            if (same_name(sig_names[k], descs[i].SemanticName)) {
                reg = sig_name_regs[k];
                found = 1;
                break;
            }
        }
        if (!found)
            win_trace("a program named part of a vertex \"%s\", which its "
                      "shader does not read", descs[i].SemanticName);

        layout.elements[i].slot = descs[i].InputSlot;
        layout.elements[i].offset = descs[i].AlignedByteOffset;
        layout.elements[i].format = format;
        layout.elements[i].reg = reg;

        UINT end = descs[i].AlignedByteOffset + wide;
        if (end > furthest) furthest = end;
    }

    layout.count = count;
    layout.stride = furthest;

    if (gpu_layout(&layout) != 0) {
        win_trace("the card would not take the program's arrangement");
        return (HRESULT)0x80004005;
    }

    extern const d3d_vtable d3d_buffer_table;
    d3d_object *l = d3d_new(D3D_LAYOUT, &d3d_buffer_table);
    if (!l) return (HRESULT)0x8007000E;
    l->stride = furthest;
    if (out) *out = l; else d3d_release(l);
    return 0;
}

static void WINAPI d3d_set_input_layout(d3d_object *self, d3d_object *layout) {
    (void)self; (void)layout;
    /* The card was told when it was made; nothing further is needed until a
     * program has more than one to choose between. */
}

/* ---------------------------------------------------------------- the tables */

const d3d_vtable d3d_buffer_table = {
    d3d_query, d3d_addref, d3d_release, { 0 }
};

static const d3d_vtable d3d_context_table = {
    d3d_query, d3d_addref, d3d_release,
    {
        (void *)d3d_set_buffers,      /* where the corners are      */
        (void *)d3d_draw,             /* draw them                  */
        (void *)d3d_present,          /* and show what was drawn    */
        (void *)d3d_set_vertex_shader,/* the program's own, for      */
        (void *)d3d_set_pixel_shader, /* placing and for colouring   */
        (void *)d3d_set_input_layout, /* and its own vertices        */
    }
};

static const d3d_vtable d3d_device_table = {
    d3d_query, d3d_addref, d3d_release,
    {
        (void *)d3d_create_buffer,    /* somewhere to put corners   */
        (void *)d3d_create_vertex_shader,
        (void *)d3d_create_pixel_shader,
        (void *)d3d_create_input_layout,
    }
};

/* ------------------------------------------------------------- the way in */

/* Whether there is a card to draw on at all.  A program is entitled to be
 * told no rather than to be given a device that draws nothing. */
static HRESULT WINAPI w_D3DCreateDevice(d3d_object **device_out,
                                        d3d_object **context_out) {
    if (!gpu_can_draw()) {
        win_trace("a program asked for a drawing device and there is no card "
                  "behind this display to give it one");
        return (HRESULT)0x887A0004;              /* DXGI_ERROR_UNSUPPORTED */
    }

    d3d_object *device = d3d_new(D3D_DEVICE, &d3d_device_table);
    if (!device) return (HRESULT)0x8007000E;

    d3d_object *context = d3d_new(D3D_CONTEXT, &d3d_context_table);
    if (!context) { free(device); return (HRESULT)0x8007000E; }

    if (device_out) *device_out = device; else d3d_release(device);
    if (context_out) *context_out = context; else d3d_release(context);
    win_trace("a program was given a drawing device");
    return 0;
}

const win_export_t d3d_exports[] = {
    { "D3DCreateDevice", (void *)w_D3DCreateDevice },
    { NULL, NULL },
};

void d3d_init(void) { win_register("d3d.dll", d3d_exports); }
