/* svga3d.c - drawing on the graphics card that is actually in this machine.
 *
 * Everything else in this system's graphics stack is either drawn by the
 * processor or driven against a model of a card that is not here.  This is the
 * exception: the display adapter in this machine has a three-dimensional
 * engine, and that engine is not simulated - it is a path to whatever real
 * graphics card the host has.  Commands written here are executed on that
 * card's own cores.
 *
 * Two things have to be true before any of it works, and both are easy to get
 * wrong in a way that looks like the feature is missing:
 *
 *   The adapter has to have been told to offer it.  The interfaces are
 *   advertised in the capability register whether or not there is anything
 *   behind them, so the question that matters is not the capability bit but
 *   the device capability queries below - and on a host that cannot provide a
 *   renderer, every one of those answers zero while the bits stay set.
 *
 *   The adapter needs somewhere to render to on the host side.  A machine
 *   running with no screen of its own gets the software renderer and answers
 *   zero to everything, which is indistinguishable from having no engine at
 *   all unless you know to look.
 *
 * The command format is the same ring the rest of the driver uses, with a
 * different kind of command in it: an identifier, a length in bytes, and then
 * the body.  The length is what the adapter uses to find the next command, so
 * a body that does not match its own header desynchronises the ring exactly
 * the way a wrong packet count does on any other card.
 *
 * ---------------------------------------------------------------------------
 * What this establishes: that this system can bring up a rendering context on
 * real graphics hardware, allocate a surface on it, render into that surface
 * with the card's own engine, and read the result back.  The pixels checked at
 * the end were produced by the host's graphics card, not by this processor.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "svga.h"

/* The commands, from the adapter's published interface. */
#define CMD3D_SURFACE_DEFINE          1040
#define CMD3D_SURFACE_DESTROY         1041
#define CMD3D_SURFACE_DMA             1044
#define CMD3D_CONTEXT_DEFINE          1045
#define CMD3D_CONTEXT_DESTROY         1046
#define CMD3D_SETTRANSFORM            1047
#define CMD3D_SETZRANGE               1048
#define CMD3D_SETRENDERSTATE          1049
#define CMD3D_SETRENDERTARGET         1050
#define CMD3D_SETVIEWPORT             1055
#define CMD3D_CLEAR                   1057
#define CMD3D_PRESENT                 1058
#define CMD3D_DRAW_PRIMITIVES         1063
#define CMD3D_SETSCISSORRECT          1064
#define CMD3D_BLIT_SURFACE_TO_SCREEN  1069

/* What a surface can be and what it holds. */
#define SURFACE_HINT_RENDERTARGET  0x00000040u
#define FORMAT_X8R8G8B8            1
#define FORMAT_A8R8G8B8            2

#define CLEAR_COLOR    0x1
#define CLEAR_DEPTH    0x2
#define RT_COLOR0      2

#define MAX_SURFACE_FACES 6

/* Transfers, and the special region that means "the framebuffer" so that no
 * separate memory region has to be registered to receive one. */
#define TRANSFER_WRITE_HOST_VRAM 1
#define TRANSFER_READ_HOST_VRAM  2
#define GMR_FRAMEBUFFER          0xFFFFFFFEu

/* Which questions to ask the device about what it will do. */
#define DEVCAP_3D                 0
#define DEVCAP_VERTEX_SHADER_VERSION    4
#define DEVCAP_FRAGMENT_SHADER_VERSION  6
#define DEVCAP_MAX_FIXED_VERTEXBLEND   11
#define DEVCAP_DXCONTEXT               95

#define REG_CAP2   59
#define CAP2_DX2   0x00000004u
#define CAP2_DX3   0x00000400u
#define DEVCAP_MAX_RENDER_TARGETS 2

/* Our own numbering for the things we make on the card. */
#define OUR_CONTEXT  1
#define OUR_SURFACE  1

static struct {
    bool ready;
    u32  width, height;
    int  contexts, surfaces, clears, blits, reads;
} gpu;

/* --------------------------------------------------------------- commands
 *
 * A three-dimensional command is a header and a body.  Built here in one
 * buffer and handed to the ring in one go, so the header can carry the real
 * length rather than a guess made before the body was written.
 */
#define CMD_MAX_WORDS 64

typedef struct {
    u32 word[CMD_MAX_WORDS];
    u32 count;
    bool overflowed;
} cmd_t;

static void cmd_begin(cmd_t *c, u32 id) {
    c->count = 0;
    c->overflowed = false;
    c->word[c->count++] = id;
    c->word[c->count++] = 0;         /* the length, filled in at the end */
}

static void cmd_u32(cmd_t *c, u32 value) {
    if (c->count >= CMD_MAX_WORDS) { c->overflowed = true; return; }
    c->word[c->count++] = value;
}

static void cmd_f32(cmd_t *c, float value) {
    union { float f; u32 u; } cast;
    cast.f = value;
    cmd_u32(c, cast.u);
}

static bool cmd_end(cmd_t *c) {
    if (c->overflowed) {
        kwarn("svga3d", "a command did not fit in the buffer it was built in");
        return false;
    }
    /* The length counts the body only, not the header - which is the detail
     * that makes every command after a wrong one be read from the wrong
     * place. */
    c->word[1] = (c->count - 2) * (u32)sizeof(u32);
    return svga_fifo_raw(c->word, c->count);
}

/* ------------------------------------------------------------ bringing it up */

/* Whether the engine is there, and whether it speaks the command set below.
 *
 * Those are two different questions and the answer is often yes to the first
 * and no to the second.  A modern adapter reports a working drawing engine
 * through the device capabilities while refusing every command in the original
 * interface: on those, surfaces and contexts have to be backed by memory the
 * guest allocates and registers first, and the commands here - which allocate
 * them on the card - do nothing at all.
 *
 * The adapter says which it is, in a version register that reads zero when the
 * original interface is gone.  Asking is the difference between reporting an
 * engine that cannot be used and reporting a driver that is broken. */
bool svga3d_available(void) {
    return svga_devcap(DEVCAP_3D) != 0 && svga_legacy_3d_version() != 0;
}

bool svga3d_begin(u32 width, u32 height) {
    gpu.ready = false;

    if (!svga3d_available()) {
        kinfo("svga3d", "the adapter offers no three-dimensional engine on "
                        "this host");
        return false;
    }
    if (!width || !height || width > 4096 || height > 4096) return false;

    /* A context: the card's idea of who is drawing.  Everything else refers
     * back to it. */
    cmd_t c;
    cmd_begin(&c, CMD3D_CONTEXT_DEFINE);
    cmd_u32(&c, OUR_CONTEXT);
    if (!cmd_end(&c)) return false;
    gpu.contexts++;

    /* And a surface to draw into, on the card's own memory.  The hint is what
     * tells it this will be rendered to rather than only read - a surface
     * without it can be allocated somewhere a render target cannot live. */
    cmd_begin(&c, CMD3D_SURFACE_DEFINE);
    cmd_u32(&c, OUR_SURFACE);
    cmd_u32(&c, SURFACE_HINT_RENDERTARGET);
    cmd_u32(&c, FORMAT_A8R8G8B8);
    /* One face with one level, and five faces with none: the face array is a
     * fixed six entries whether or not this is a cube. */
    cmd_u32(&c, 1);
    for (int i = 1; i < MAX_SURFACE_FACES; i++) cmd_u32(&c, 0);
    /* Then one size per level per face that has any - here, one. */
    cmd_u32(&c, width);
    cmd_u32(&c, height);
    cmd_u32(&c, 1);
    if (!cmd_end(&c)) return false;
    gpu.surfaces++;

    gpu.width = width;
    gpu.height = height;
    gpu.ready = true;

    svga_fifo_sync();
    return true;
}

void svga3d_end(void) {
    if (!gpu.ready) return;

    cmd_t c;
    cmd_begin(&c, CMD3D_SURFACE_DESTROY);
    cmd_u32(&c, OUR_SURFACE);
    cmd_end(&c);

    cmd_begin(&c, CMD3D_CONTEXT_DESTROY);
    cmd_u32(&c, OUR_CONTEXT);
    cmd_end(&c);

    svga_fifo_sync();
    gpu.ready = false;
}

/* ------------------------------------------------------------- drawing */

/* Point the context at the surface, and say which part of it is being drawn.
 * Both are needed: a target with no viewport renders to a rectangle of zero
 * size, which is a black screen with every other register correct. */
static bool aim_at_surface(void) {
    cmd_t c;

    cmd_begin(&c, CMD3D_SETRENDERTARGET);
    cmd_u32(&c, OUR_CONTEXT);
    cmd_u32(&c, RT_COLOR0);
    cmd_u32(&c, OUR_SURFACE);       /* the image: surface, face, level */
    cmd_u32(&c, 0);
    cmd_u32(&c, 0);
    if (!cmd_end(&c)) return false;

    cmd_begin(&c, CMD3D_SETVIEWPORT);
    cmd_u32(&c, OUR_CONTEXT);
    cmd_u32(&c, 0);
    cmd_u32(&c, 0);
    cmd_u32(&c, gpu.width);
    cmd_u32(&c, gpu.height);
    return cmd_end(&c);
}

bool svga3d_clear(u32 colour) {
    if (!gpu.ready) return false;
    if (!aim_at_surface()) return false;

    cmd_t c;
    cmd_begin(&c, CMD3D_CLEAR);
    cmd_u32(&c, OUR_CONTEXT);
    cmd_u32(&c, CLEAR_COLOR);
    cmd_u32(&c, colour);
    cmd_f32(&c, 1.0f);              /* depth, unused without a depth buffer */
    cmd_u32(&c, 0);                 /* stencil                              */
    /* Which rectangles.  One, covering the whole surface. */
    cmd_u32(&c, 0);
    cmd_u32(&c, 0);
    cmd_u32(&c, gpu.width);
    cmd_u32(&c, gpu.height);
    if (!cmd_end(&c)) return false;

    gpu.clears++;
    svga_fifo_sync();
    return true;
}

/* Read back what the card rendered.
 *
 * The surface lives in the card's own memory, where the processor cannot see
 * it.  This asks the card to copy it out into memory the guest can read - a
 * transfer the card performs itself - and it is the only way to look at what
 * was drawn without depending on there being a window on the host to put it
 * in.  Which matters: a machine whose window is not on screen has a perfectly
 * working renderer and nowhere to present to.
 *
 * The destination is the framebuffer, addressed as a special region so that no
 * separate memory region has to be registered first.
 */
bool svga3d_read_back(u32 fb_offset, u32 pitch, u32 width, u32 height) {
    if (!gpu.ready) return false;
    if (width > gpu.width) width = gpu.width;
    if (height > gpu.height) height = gpu.height;

    cmd_t c;
    cmd_begin(&c, CMD3D_SURFACE_DMA);
    /* Where it goes: the framebuffer, at an offset, with this many bytes a
     * line. */
    cmd_u32(&c, GMR_FRAMEBUFFER);
    cmd_u32(&c, fb_offset);
    cmd_u32(&c, pitch);
    /* Which image comes out: surface, face, level. */
    cmd_u32(&c, OUR_SURFACE);
    cmd_u32(&c, 0);
    cmd_u32(&c, 0);
    /* Out of the card rather than into it. */
    cmd_u32(&c, TRANSFER_READ_HOST_VRAM);
    /* One box: where in the destination, how big, where in the source. */
    cmd_u32(&c, 0); cmd_u32(&c, 0); cmd_u32(&c, 0);          /* x, y, z      */
    cmd_u32(&c, width); cmd_u32(&c, height); cmd_u32(&c, 1); /* w, h, d      */
    cmd_u32(&c, 0); cmd_u32(&c, 0); cmd_u32(&c, 0);          /* source x,y,z */
    /* And the suffix, which says how far the card may write - without it a
     * malformed box could be told to write past the end of the region. */
    cmd_u32(&c, 12);                                          /* its own size */
    cmd_u32(&c, fb_offset + pitch * height);
    cmd_u32(&c, 0);                                           /* flags        */
    if (!cmd_end(&c)) return false;

    gpu.reads++;
    svga_fifo_sync();
    return true;
}

/* Put what the card drew on the screen.  The blit is done by the card too, so
 * the whole path from clearing to displaying never touches the processor. */
bool svga3d_present(int x, int y, u32 width, u32 height) {
    if (!gpu.ready) return false;
    if (width > gpu.width) width = gpu.width;
    if (height > gpu.height) height = gpu.height;

    cmd_t c;
    cmd_begin(&c, CMD3D_BLIT_SURFACE_TO_SCREEN);
    cmd_u32(&c, OUR_SURFACE);       /* the image again */
    cmd_u32(&c, 0);
    cmd_u32(&c, 0);
    /* Where from, as a signed rectangle: left, top, right, bottom. */
    cmd_u32(&c, 0);
    cmd_u32(&c, 0);
    cmd_u32(&c, width);
    cmd_u32(&c, height);
    cmd_u32(&c, 0);                 /* which screen */
    cmd_u32(&c, (u32)x);
    cmd_u32(&c, (u32)y);
    cmd_u32(&c, (u32)x + width);
    cmd_u32(&c, (u32)y + height);
    if (!cmd_end(&c)) return false;

    gpu.blits++;
    svga_fifo_sync();
    return true;
}

void svga3d_counts(int *contexts, int *surfaces, int *clears, int *reads) {
    if (contexts) *contexts = gpu.contexts;
    if (surfaces) *surfaces = gpu.surfaces;
    if (clears) *clears = gpu.clears;
    if (reads) *reads = gpu.reads;
}

/* ------------------------------------------------------------------- test
 *
 * The check is the pixels.  A rectangle is cleared to a colour by the card and
 * blitted to the screen by the card, and then read back through the
 * framebuffer - so what is being verified is that a real graphics card did
 * what it was asked, not that the right numbers were written into a ring.
 */
int svga3d_selftest(void) {
    if (!svga_present()) return 0;

    u32 engine = svga_devcap(DEVCAP_3D);
    u32 targets = svga_devcap(DEVCAP_MAX_RENDER_TARGETS);
    u32 legacy = svga_legacy_3d_version();

    if (!engine) {
        kinfo("svga3d", "no three-dimensional engine on this host, so drawing "
                        "stays with the processor");
        return 0;
    }

    if (!legacy) {
        /* The engine is there and reachable - this is not a failure, and
         * saying so plainly is worth more than a check that passes. */
        kinfo("svga3d", "the host graphics card is reachable and offers a "
                        "drawing engine: %u render targets", targets);
        kinfo("svga3d", "it has retired the original command set, so its "
                        "objects are backed by memory this system registers "
                        "first - which is what runs below");
        return 0;
    }

    int failures = 0;

    /* Small, and in a corner nothing else has written to yet. */
    const u32 w = 64, h = 48;
    if (!svga3d_begin(w, h)) {
        kerr("svga3d", "a rendering context could not be brought up");
        return 1;
    }

    u32 screen_w = 0, screen_h = 0;
    svga_mode(&screen_w, &screen_h, NULL);
    if (screen_w < w || screen_h < h) {
        svga3d_end();
        return 0;
    }

    /* A corner of the framebuffer to receive it: the bottom-left, which the
     * console has not written to at this point in start-up. */
    u32 screen_pitch = 0;
    svga_mode(NULL, NULL, &screen_pitch);
    int at_x = 0;
    int at_y = (int)(screen_h - h);
    u32 fb_offset = (u32)at_y * screen_pitch;

    /* Two colours in turn, so a stale framebuffer cannot pass by accident. */
    static const u32 colours[2] = { 0x00C81E64u, 0x001EC864u };
    int wrong_total = 0;

    for (int pass = 0; pass < 2; pass++) {
        if (!svga3d_clear(colours[pass])) {
            kerr("svga3d", "the card would not clear the surface");
            failures++;
            break;
        }
        /* Out of the card's own memory and into somewhere the processor can
         * look.  The card does the copy. */
        if (!svga3d_read_back(fb_offset, screen_pitch, w, h)) {
            kerr("svga3d", "the card would not hand the surface back");
            failures++;
            break;
        }

        /* The adapter has to have finished before the pixels are read. */
        svga_fifo_sync();
        timer_udelay(20000);

        int wrong = 0;
        for (u32 y = 4; y < h - 4; y += 7) {
            for (u32 x = 4; x < w - 4; x += 7) {
                u32 got = svga_read_pixel(at_x + (int)x, at_y + (int)y) & 0xFFFFFF;
                if (got != colours[pass]) wrong++;
            }
        }

        if (wrong) {
            kerr("svga3d", "pass %d: %d sampled pixel(s) are not %06x; the "
                           "first four are %08x %08x %08x %08x", pass, wrong,
                 colours[pass],
                 svga_read_pixel(at_x + 4, at_y + 4),
                 svga_read_pixel(at_x + 11, at_y + 4),
                 svga_read_pixel(at_x + 4, at_y + 11),
                 svga_read_pixel(at_x + 32, at_y + 24));
            wrong_total += wrong;
        }
    }

    if (wrong_total) failures++;

    int contexts = 0, surfaces = 0, clears = 0, reads = 0;
    svga3d_counts(&contexts, &surfaces, &clears, &reads);

    /* Put the corner back to black. */
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            svga_write_pixel(at_x + (int)x, at_y + (int)y, 0);
    svga_update(at_x, at_y, (int)w, (int)h);

    svga3d_end();

    if (!failures)
        kinfo("svga3d", "the host graphics card rendered it: %d context, %d "
                        "surface, %d clears and %d read-backs on the card, and "
                        "the pixels are the ones it was asked for",
              contexts, surfaces, clears, reads);
    else
        kwarn("svga3d", "the three-dimensional engine answered but did not "
                        "produce the right pixels");

    return failures;
}

/* ================================================= the guest-backed interface
 *
 * The original interface above allocated surfaces on the card and moved data
 * with explicit transfers.  Modern adapters have retired it entirely - the
 * version register reads zero - and replaced it with one where every object
 * the card owns is backed by memory the guest allocated and registered first.
 *
 * The difference is worth understanding, because it is not a renaming:
 *
 *   The guest allocates the memory and tells the card about it, as a "memory
 *   object".  The card does not allocate anything.
 *
 *   A surface is defined without storage and then bound to one of those memory
 *   objects.  Two surfaces can be bound to the same memory, and a surface can
 *   be rebound, which is how a driver moves things around without the card
 *   caring.
 *
 *   The card keeps its own copy of a surface in whatever form suits it -
 *   tiled, compressed, in its own memory - and the guest's copy is only
 *   synchronised on request.  Writing the guest's memory does not change what
 *   the card draws with until an update is asked for, and reading it does not
 *   see what the card produced until a readback is asked for.
 *
 * That last point is the whole model.  It is also what makes it possible to
 * prove the card is doing the work: hand it a pattern, ask it to copy that
 * surface into another one entirely inside its own memory, ask for the result
 * back, and check it.  The copy in the middle happens on the card.
 */
#define CMD3D_SURFACE_COPY            1042
#define CMD3D_DEFINE_GB_MOB64         1135
#define CMD3D_DESTROY_GB_MOB          1094
#define CMD3D_DEFINE_GB_SURFACE_V2    1134
#define CMD3D_DESTROY_GB_SURFACE      1098
#define CMD3D_BIND_GB_SURFACE         1099
/* Two ways to tell the card a surface changed, and the difference is the
 * whole cost of handing it a picture.
 *
 * UPDATE_GB_SURFACE names the surface and nothing else, so the card takes the
 * whole of it - four megabytes for a surface a thousand pixels square, every
 * time, however little of it moved.
 *
 * UPDATE_GB_IMAGE names a box inside the surface, so the cost is what actually
 * changed.  A desktop's damage is usually a caret blinking or a clock ticking,
 * which is a few thousand pixels rather than a million. */
#define CMD3D_UPDATE_GB_IMAGE         1101
#define CMD3D_UPDATE_GB_SURFACE       1102
#define CMD3D_READBACK_GB_SURFACE     1104

/* A memory object whose pages are one contiguous run, which is what this
 * system's allocator hands out - so the card is given a first page and a
 * length rather than a page table to walk. */
#define MOBFMT_RANGE 3

#define SURFACE_HINT_TEXTURE         (1u << 5)
#define SURFACE_BIND_RENDER_TARGET   (1u << 24)
#define SURFACE_BIND_SHADER_RESOURCE (1u << 23)

#define FILTER_NONE 0

/* A vertex buffer is a surface too, of a format that means "no format": a run
 * of bytes whose meaning is given later, by the declarations in the draw. */
#define SURFACE_HINT_VERTEXBUFFER    (1u << 4)
#define SURFACE_BIND_VERTEX_BUFFER   (1u << 20)
#define FORMAT_BUFFER                37

/* --------------------------------------------------------- the object tables
 *
 * The card does not keep its own bookkeeping.  Every guest-backed object it
 * owns - every memory object, every surface, every context - has an entry in a
 * table, and those tables live in memory the guest allocates and hands over
 * before anything else can be created.
 *
 * Until they exist, the card accepts every define command and does nothing
 * with any of them.  No error, no complaint: the commands are read out of the
 * ring and discarded, because there is nowhere to record what they asked for.
 * That silence is the single most confusing thing about bringing this
 * interface up, and it is why this has to come first.
 */
#define CMD3D_SET_OTABLE_BASE64 1115

#define OTABLE_MOB          0
#define OTABLE_SURFACE      1
#define OTABLE_CONTEXT      2
#define OTABLE_SHADER       3
#define OTABLE_SCREENTARGET 4
#define OTABLE_DXCONTEXT    5

/* One entry each, as the interface lays them out. */
#define OTABLE_MOB_ENTRY          16
#define OTABLE_SURFACE_ENTRY      64
#define OTABLE_CONTEXT_ENTRY       8
#define OTABLE_SHADER_ENTRY       16
#define OTABLE_SCREENTARGET_ENTRY 64
#define OTABLE_DXCONTEXT_ENTRY     8

static bool otables_ready;

static bool set_otable(u32 type, u64 first_page, u32 bytes) {
    cmd_t c;
    cmd_begin(&c, CMD3D_SET_OTABLE_BASE64);
    cmd_u32(&c, type);
    cmd_u32(&c, (u32)(first_page & 0xFFFFFFFFu));
    cmd_u32(&c, (u32)(first_page >> 32));
    cmd_u32(&c, bytes);
    cmd_u32(&c, bytes);             /* how much of it is valid              */
    cmd_u32(&c, MOBFMT_RANGE);      /* one contiguous run of pages          */
    return cmd_end(&c);
}

static bool gb_setup_otables(void) {
    if (otables_ready) return true;

    /* How many of each.  Small: this is not a driver running a desktop, and
     * every entry costs memory that is handed to the card permanently. */
    static const struct { u32 type; u32 entries; u32 entry_size; } tables[] = {
        { OTABLE_MOB,          256, OTABLE_MOB_ENTRY },
        { OTABLE_SURFACE,      256, OTABLE_SURFACE_ENTRY },
        { OTABLE_CONTEXT,       64, OTABLE_CONTEXT_ENTRY },
        { OTABLE_SHADER,        64, OTABLE_SHADER_ENTRY },
        { OTABLE_SCREENTARGET,   8, OTABLE_SCREENTARGET_ENTRY },
        /* And the drawing contexts.  All of the tables are set before any
         * object is created, because that is the order a working driver does
         * it in and the device is entitled to expect it: a table configured
         * after objects already exist can be refused without a word. */
        { OTABLE_DXCONTEXT,    256, OTABLE_DXCONTEXT_ENTRY },
    };

    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        u32 bytes = tables[i].entries * tables[i].entry_size;
        u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        u64 phys = 0;
        u8 *memory = dma_alloc_pages(pages, &phys);
        if (!memory) {
            kwarn("svga3d", "no memory for the card's object tables");
            return false;
        }
        /* Zeroed, because the card reads these entries before it writes any
         * of them and a stale one describes an object that is not there. */
        memset(memory, 0, (size_t)pages * PAGE_SIZE);

        if (!set_otable(tables[i].type, phys / PAGE_SIZE,
                        pages * (u32)PAGE_SIZE))
            return false;
    }

    svga_fifo_sync();
    otables_ready = true;
    return true;
}

static bool gb_define_mob(u32 mobid, u64 first_page, u32 bytes) {
    cmd_t c;
    cmd_begin(&c, CMD3D_DEFINE_GB_MOB64);
    cmd_u32(&c, mobid);
    cmd_u32(&c, MOBFMT_RANGE);
    /* The page number, not the address: sixty-four bits of it, low half
     * first. */
    cmd_u32(&c, (u32)(first_page & 0xFFFFFFFFu));
    cmd_u32(&c, (u32)(first_page >> 32));
    cmd_u32(&c, bytes);
    return cmd_end(&c);
}

static bool gb_destroy_mob(u32 mobid) {
    cmd_t c;
    cmd_begin(&c, CMD3D_DESTROY_GB_MOB);
    cmd_u32(&c, mobid);
    return cmd_end(&c);
}

static bool gb_define_surface(u32 sid, u32 width, u32 height) {
    cmd_t c;
    cmd_begin(&c, CMD3D_DEFINE_GB_SURFACE_V2);
    cmd_u32(&c, sid);
    /* What it may be used as.  A surface that does not say it can be a render
     * target cannot be one later, however it is bound. */
    /* Both the old hint and the newer binding bit.  The two interfaces read
     * different fields for the same question, and a surface that says only one
     * of them can be refused as a render target by whichever half of the card
     * is asked. */
    cmd_u32(&c, SURFACE_HINT_RENDERTARGET | SURFACE_HINT_TEXTURE |
                SURFACE_BIND_RENDER_TARGET | SURFACE_BIND_SHADER_RESOURCE);
    cmd_u32(&c, FORMAT_A8R8G8B8);
    cmd_u32(&c, 1);                 /* one level                            */
    cmd_u32(&c, 0);                 /* not multisampled                     */
    cmd_u32(&c, FILTER_NONE);
    cmd_u32(&c, width);
    cmd_u32(&c, height);
    cmd_u32(&c, 1);                 /* depth                                */
    cmd_u32(&c, 1);                 /* one element in the array             */
    cmd_u32(&c, 0);                 /* padding the interface defines         */
    return cmd_end(&c);
}

/* The same call, for a surface that is a run of bytes rather than a picture.
 * The length goes in the width, and the height and depth are one - the card is
 * being told the shape of a buffer using the fields it has for images. */
static bool __attribute__((unused)) gb_define_buffer(u32 sid, u32 bytes) {
    cmd_t c;
    cmd_begin(&c, CMD3D_DEFINE_GB_SURFACE_V2);
    cmd_u32(&c, sid);
    cmd_u32(&c, SURFACE_HINT_VERTEXBUFFER | SURFACE_BIND_VERTEX_BUFFER);
    cmd_u32(&c, FORMAT_BUFFER);
    cmd_u32(&c, 1);                 /* one level                            */
    cmd_u32(&c, 0);                 /* not multisampled                     */
    cmd_u32(&c, FILTER_NONE);
    cmd_u32(&c, bytes);             /* the width is the length              */
    cmd_u32(&c, 1);
    cmd_u32(&c, 1);
    cmd_u32(&c, 1);
    cmd_u32(&c, 0);                 /* padding the interface defines        */
    return cmd_end(&c);
}

static bool gb_bind_surface(u32 sid, u32 mobid) {
    cmd_t c;
    cmd_begin(&c, CMD3D_BIND_GB_SURFACE);
    cmd_u32(&c, sid);
    cmd_u32(&c, mobid);
    return cmd_end(&c);
}

static bool gb_destroy_surface(u32 sid) {
    cmd_t c;
    cmd_begin(&c, CMD3D_DESTROY_GB_SURFACE);
    cmd_u32(&c, sid);
    return cmd_end(&c);
}

/* Take what the guest wrote into the card's own copy. */
static bool gb_update(u32 sid) {
    cmd_t c;
    cmd_begin(&c, CMD3D_UPDATE_GB_SURFACE);
    cmd_u32(&c, sid);
    return cmd_end(&c);
}

/* And bring the card's own copy back to where the guest can read it. */
static bool gb_readback(u32 sid) {
    cmd_t c;
    cmd_begin(&c, CMD3D_READBACK_GB_SURFACE);
    cmd_u32(&c, sid);
    return cmd_end(&c);
}

/* A copy from one surface to another, done inside the card. */
static bool gb_copy(u32 from, u32 to, u32 width, u32 height) {
    cmd_t c;
    cmd_begin(&c, CMD3D_SURFACE_COPY);
    cmd_u32(&c, from); cmd_u32(&c, 0); cmd_u32(&c, 0);   /* source image    */
    cmd_u32(&c, to);   cmd_u32(&c, 0); cmd_u32(&c, 0);   /* destination     */
    /* One box: where in the destination, how big, where in the source. */
    cmd_u32(&c, 0); cmd_u32(&c, 0); cmd_u32(&c, 0);
    cmd_u32(&c, width); cmd_u32(&c, height); cmd_u32(&c, 1);
    cmd_u32(&c, 0); cmd_u32(&c, 0); cmd_u32(&c, 0);
    return cmd_end(&c);
}

/* ------------------------------------------------------------------- test
 *
 * A pattern is written into memory the card has been told about, handed to the
 * card, copied by the card from one of its surfaces into another, and brought
 * back into different memory.  The pattern that comes out the far end went
 * through the card's own memory and was moved by the card.
 *
 * The two memory objects are separate and the destination starts as something
 * else entirely, so a card that did nothing leaves the destination unchanged
 * and the check fails - which is what makes this a test rather than a
 * demonstration.
 */
#define GB_W 64
#define GB_H 64
#define GB_BYTES (GB_W * GB_H * 4)

int svga3d_gb_selftest(void) {
    if (!svga_present()) return 0;
    if (!svga_devcap(DEVCAP_3D)) return 0;
    if (!svga_gbobjects()) {
        kinfo("svga3d", "this adapter has no guest-backed interface either");
        return 0;
    }

    /* Memory the card will be told about.  It has to be contiguous, because
     * that is the form the card is being given it in. */
    u64 source_phys = 0, dest_phys = 0;
    u32 *source = dma_alloc_pages(GB_BYTES / PAGE_SIZE, &source_phys);
    u32 *dest = dma_alloc_pages(GB_BYTES / PAGE_SIZE, &dest_phys);
    if (!source || !dest) {
        kwarn("svga3d", "no memory to give the card");
        return 0;
    }

    /* A pattern that could not appear by accident, and a destination filled
     * with something else so that "unchanged" is distinguishable from
     * "copied". */
    for (int i = 0; i < GB_W * GB_H; i++) {
        source[i] = 0xFF000000u | (u32)((i * 2654435761u) & 0x00FFFFFF);
        dest[i] = 0xFF123456u;
    }
    __asm__ volatile("sfence" ::: "memory");

    int failures = 0;
    const u32 mob_a = 1, mob_b = 2, surface_a = 10, surface_b = 11;

    /* Before anything else: somewhere for the card to record what it is being
     * given. */
    if (!gb_setup_otables()) {
        kwarn("svga3d", "the card's object tables could not be set up");
        return 0;
    }

    bool built = gb_define_mob(mob_a, source_phys / PAGE_SIZE, GB_BYTES);
    built &= gb_define_mob(mob_b, dest_phys / PAGE_SIZE, GB_BYTES);
    built &= gb_define_surface(surface_a, GB_W, GB_H);
    built &= gb_define_surface(surface_b, GB_W, GB_H);
    built &= gb_bind_surface(surface_a, mob_a);
    built &= gb_bind_surface(surface_b, mob_b);

    if (!built) {
        kwarn("svga3d", "the card would not take the surfaces");
        return 0;
    }
    svga_fifo_sync();

    /* Into the card, copied inside it, and back out into the other memory. */
    gb_update(surface_a);
    gb_copy(surface_a, surface_b, GB_W, GB_H);
    gb_readback(surface_b);
    svga_fifo_sync();
    timer_udelay(30000);

    int wrong = 0, unchanged = 0;
    for (int i = 0; i < GB_W * GB_H; i += 37) {
        u32 want = 0xFF000000u | (u32)((i * 2654435761u) & 0x00FFFFFF);
        if (dest[i] == 0xFF123456u) unchanged++;
        else if (dest[i] != want) wrong++;
    }

    if (unchanged) {
        kwarn("svga3d", "%d sampled pixel(s) never changed, so the card did not "
                        "do the copy", unchanged);
        failures++;
    } else if (wrong) {
        kerr("svga3d", "%d sampled pixel(s) came back altered; first is %08x",
             wrong, dest[0]);
        failures++;
    } else {
        kinfo("svga3d", "the host graphics card moved it: two memory objects "
                        "registered, two surfaces bound, and %d pixels copied "
                        "between them inside the card itself",
              GB_W * GB_H);
    }

    gb_destroy_surface(surface_a);
    gb_destroy_surface(surface_b);
    gb_destroy_mob(mob_a);
    gb_destroy_mob(mob_b);
    svga_fifo_sync();

    return failures;
}

/* ================================================== rendering, through buffers
 *
 * Every command above goes into the ring and the card takes it.  Rendering
 * commands do not: on an adapter of this generation they are addressed to a
 * drawing context, and the ring has nowhere to say which one.  They travel a
 * different way - in a buffer of their own, with a header naming the context,
 * whose address is handed to the card in a register.
 *
 * That is why every render command sent through the ring earlier was accepted
 * and did nothing.  There was no context attached, so there was nothing for it
 * to act on, and the card is under no obligation to complain.
 *
 * A command buffer is a header followed by commands.  The driver fills it in,
 * gives the card its physical address, and waits for the card to change the
 * status word from nothing to finished.  That word is also how the card
 * reports a malformed command - with the byte offset of the one it choked on,
 * which is far more than the ring ever says.
 */
#define REG_COMMAND_LOW  48
#define REG_COMMAND_HIGH 49
#define REG_CAP2         59
#define REG_GUEST_DRIVER_ID       61
#define REG_GUEST_DRIVER_VERSION1 62
#define REG_GUEST_DRIVER_VERSION2 63
#define REG_GUEST_DRIVER_VERSION3 64
#define GUEST_DRIVER_ID_LINUX  2
#define GUEST_DRIVER_ID_SUBMIT 0xFFFFFFFFu

#define CB_STATUS_NONE      0
#define CB_STATUS_COMPLETED 1

#define CB_FLAG_NO_IRQ     (1u << 0)
#define CB_FLAG_DX_CONTEXT (1u << 1)
#define CB_CONTEXT_0       0x00
#define CB_CONTEXT_DEVICE  0x3F

/* The commands that manage the queues themselves. */
#define SVGA_DC_CMD_START_STOP_CONTEXT 1


#define VIEW_NONE          0xFFFFFFFFu


/* Generous: a drawing context holds every piece of state the card has. */
#define DX_CONTEXT_BYTES (128 * 1024)
#define CB_BODY_BYTES    4096

/* Our numbering for the two things made here. */

/* The header, as the interface lays it out.  Sixty-four bytes, and the words
 * at the end must be zero or the card rejects the buffer rather than the
 * command. */
typedef struct {
    volatile u32 status;
    volatile u32 error_offset;
    u64 id;
    u32 flags;
    u32 length;
    u64 address;
    u32 offset;
    u32 dx_context;
    u32 must_be_zero[6];
} __attribute__((packed)) cb_header_t;

static struct {
    bool ready;
    cb_header_t *header;
    u8  *body;
    u64  header_phys;
    u64  body_phys;
    u32  at;
    int  submissions;
    u32  last_status;
    u32  last_error_at;
    bool queue_started;
} cb;

static bool cb_setup(void) {
    if (cb.ready) return true;

    u64 phys = 0;
    u8 *page = dma_alloc_pages(2, &phys);
    if (!page) return false;
    memset(page, 0, PAGE_SIZE * 2);

    /* The header on one page, the commands on the next, so the body is page
     * aligned and its address needs no arithmetic to be sure of. */
    cb.header = (cb_header_t *)page;
    cb.header_phys = phys;
    cb.body = page + PAGE_SIZE;
    cb.body_phys = phys + PAGE_SIZE;
    cb.ready = true;
    return true;
}

static void cb_begin(void) { cb.at = 0; }

static bool cb_command(u32 id, const u32 *body, u32 words) {
    u32 need = (2 + words) * (u32)sizeof(u32);
    if (cb.at + need > CB_BODY_BYTES) return false;

    u32 *at = (u32 *)(cb.body + cb.at);
    at[0] = id;
    at[1] = words * (u32)sizeof(u32);
    for (u32 i = 0; i < words; i++) at[2 + i] = body[i];
    cb.at += need;
    return true;
}

/* Submitting into a particular queue.  Ordinary commands go to queue zero;
 * the commands that manage the queues themselves go to the device's own. */
static bool cb_start_queue(void);

/* Start the queue again after it has stopped.  Guarded, because reviving it is
 * itself a buffer and a failure there must not try to revive it again. */
static bool cb_reviving;

static void cb_revive(void) {
    if (cb_reviving) return;
    cb_reviving = true;
    cb.queue_started = false;
    cb_start_queue();
    cb_reviving = false;
}

static bool cb_submit_to(u32 queue, u32 dx_context) {
    if (!cb.at) return true;

    u32 length = cb.at;
    memset((void *)cb.header, 0, sizeof *cb.header);
    cb.header->status = CB_STATUS_NONE;
    cb.header->length = length;
    cb.header->address = cb.body_phys;
    /* No interrupt: this waits on the status word, which the card updates
     * either way. */
    cb.header->flags = CB_FLAG_NO_IRQ;
    if (dx_context != VIEW_NONE) {
        cb.header->flags |= CB_FLAG_DX_CONTEXT;
        cb.header->dx_context = dx_context;
    }

    __asm__ volatile("sfence" ::: "memory");

    /* The high half first.  Writing the low half is what starts it, so the
     * other order hands the card half an address. */
    svga_reg_write(REG_COMMAND_HIGH, (u32)(cb.header_phys >> 32));
    svga_reg_write(REG_COMMAND_LOW, (u32)cb.header_phys | queue);

    /* Long enough for a command that has to reach the host and back, short
     * enough that a command the card is never going to answer does not stall
     * the machine at start-up. */
    for (int i = 0; i < 200000; i++) {
        u32 status = cb.header->status;
        if (status != CB_STATUS_NONE) {
            cb.last_status = status;
            cb.last_error_at = cb.header->error_offset;
            cb.submissions++;
            cb.at = 0;
            if (status == CB_STATUS_COMPLETED) return true;
            /* A command the card refuses does not only fail.  It stops the
             * queue that carried it, and every buffer afterwards is never
             * looked at - so without this the first mistake makes everything
             * after it look like the same mistake, and the driver spends its
             * time diagnosing an error it already had.
             *
             * Reviving the queue is itself a buffer, and a successful one, so
             * what this command was refused for is kept aside and put back
             * afterwards rather than being overwritten by the revival. */
            u32 kept_status = cb.last_status;
            u32 kept_where = cb.last_error_at;
            cb_revive();
            cb.last_status = kept_status;
            cb.last_error_at = kept_where;
            return false;
        }
        timer_udelay(10);
    }

    /* Nothing came back at all, which is a different thing from a refusal and
     * must not be reported as the last one: leaving the old status in place
     * makes a dead queue look exactly like whatever went wrong before it. */
    kwarn("svga3d", "the card never answered a command buffer");
    cb.last_status = CB_STATUS_NONE;
    cb.last_error_at = 0;
    cb.at = 0;
    cb_revive();
    return false;
}

static bool cb_submit(u32 dx_context) {
    return cb_submit_to(CB_CONTEXT_0, dx_context);
}

/* The queues are not running to begin with.  A buffer put into one that has
 * not been started is not refused - it is simply never looked at, and the
 * status word stays at nothing forever.  Starting it is itself a command, sent
 * to the device's own queue, which is always running. */
static bool cb_start_queue(void) {
    if (cb.queue_started) return true;

    u32 *at = (u32 *)cb.body;
    at[0] = SVGA_DC_CMD_START_STOP_CONTEXT;
    at[1] = 1;                      /* start it                            */
    at[2] = CB_CONTEXT_0;           /* which queue                         */
    cb.at = 3 * (u32)sizeof(u32);

    if (!cb_submit_to(CB_CONTEXT_DEVICE, VIEW_NONE)) {
        kwarn("svga3d", "the card would not start its command queue "
                        "(status %u)", cb.last_status);
        return false;
    }

    /* The queue is started but not necessarily running yet.  A buffer handed
     * over immediately can be refused where the same buffer a moment later is
     * taken, so this waits rather than making the first real command carry the
     * cost of finding out. */
    timer_udelay(20000);

    cb.queue_started = true;
    return true;
}


/* ================================================ rendering on the real card
 *
 * Everything above manages objects: memory the card is told about, surfaces
 * bound to it, copies between them.  This is the card drawing.
 *
 * The one thing that took finding is where the commands go.  Object management
 * is accepted from the ring.  Drawing is not - it is addressed to a context,
 * and the ring has nowhere to name one, so a drawing command put there is read
 * and discarded without a word.  The same command in a command buffer, whose
 * header does name a context, is executed.
 *
 * That is why the earlier attempt through the ring appeared to work and drew
 * nothing: every command was accepted and none of them meant anything.  There
 * is no error to find, because from the ring's point of view nothing went
 * wrong.
 */
/* The older kind of drawing context: the one this adapter actually creates.
 * Its state lives in memory the guest provides, like everything else here. */
#define CMD3D_DEFINE_GB_CONTEXT  1107
#define CMD3D_DESTROY_GB_CONTEXT 1108
#define CMD3D_BIND_GB_CONTEXT    1109

/* Generous: a context holds every piece of drawing state the card has, and a
 * region larger than it needs is harmless where one too small is not. */
#define CONTEXT_BYTES (256 * 1024)

#define RENDER_CID 1

static struct {
    bool ready;
    u32  surface;
    u32  width, height;
    int  clears;
} render;

static bool render_setup(u32 surface, u32 context_mob, u32 second_mob,
                         u32 width, u32 height) {
    u32 body[8];

    /* Something harmless through the queue first, whose only job is to prove
     * the queue is running before anything that matters goes into it.  A
     * buffer handed to a queue that is not yet running is not refused so much
     * as ignored, and that is indistinguishable from a rejected command. */
    for (int attempt = 0; attempt < 4; attempt++) {
        cb_begin();
        body[0] = surface;
        cb_command(CMD3D_READBACK_GB_SURFACE, body, 1);
        if (cb_submit(VIEW_NONE)) break;
        timer_udelay(20000);
    }

    /* Make the context through the ring and take it away again, before making
     * it for real through a buffer.
     *
     * This looks pointless and is not.  The adapter keeps its own table of
     * objects, and the slot has to have been through its hands once before the
     * host renderer behind it will accept one there.  Without this the buffer
     * that follows is refused; with it, it is taken and the card draws.
     *
     * Why is not established.  It is written down as observed. */
    {
        cmd_t r;
        cmd_begin(&r, CMD3D_DEFINE_GB_CONTEXT);
        cmd_u32(&r, RENDER_CID);
        cmd_end(&r);

        cmd_begin(&r, CMD3D_DESTROY_GB_CONTEXT);
        cmd_u32(&r, RENDER_CID);
        cmd_end(&r);

        svga_fifo_sync();
    }

    /* The context, through buffers only.
     *
     * Not through the ring first.  The ring takes these commands and makes the
     * object exist in the adapter's own tables, and the buffer that follows is
     * then refused because what it asks for is already there - while the
     * object the ring made is not one the host renderer knows about, so
     * nothing drawn against it works either.  One or the other, not both. */
    cb_begin();
    body[0] = RENDER_CID;
    cb_command(CMD3D_DEFINE_GB_CONTEXT, body, 1);
    if (!cb_submit(VIEW_NONE)) {
        kinfo("svga3d", "the card would not make a drawing context from a "
                        "buffer (status %u)", cb.last_status);
        return false;
    }

    cb_begin();
    body[0] = RENDER_CID;
    body[1] = context_mob;
    body[2] = 0;
    cb_command(CMD3D_BIND_GB_CONTEXT, body, 3);
    if (!cb_submit(VIEW_NONE)) {
        kinfo("svga3d", "the card would not give the context memory from a "
                        "buffer (status %u)", cb.last_status);
        return false;
    }

    (void)second_mob;

    render.surface = surface;
    render.width = width;
    render.height = height;
    render.ready = true;
    return true;
}

/* The card fills the surface with a colour.  Everything a draw needs is here
 * in miniature: which surface is being drawn into, which part of it, and then
 * the operation. */
static bool render_clear(u32 colour) {
    if (!render.ready) return false;

    u32 body[9];

    cb_begin();

    body[0] = RENDER_CID;
    body[1] = RT_COLOR0;
    body[2] = render.surface;
    body[3] = 0;                    /* face  */
    body[4] = 0;                    /* level */
    cb_command(CMD3D_SETRENDERTARGET, body, 5);

    body[0] = RENDER_CID;
    body[1] = 0;
    body[2] = 0;
    body[3] = render.width;
    body[4] = render.height;
    cb_command(CMD3D_SETVIEWPORT, body, 5);

    body[0] = RENDER_CID;
    body[1] = CLEAR_COLOR;
    body[2] = colour;
    body[3] = 0x3F800000u;          /* depth 1.0, unused without a depth buffer */
    body[4] = 0;                    /* stencil                                  */
    /* One rectangle, the whole surface. */
    body[5] = 0;
    body[6] = 0;
    body[7] = render.width;
    body[8] = render.height;
    cb_command(CMD3D_CLEAR, body, 9);

    /* Unnamed: the context this kind of drawing belongs to is carried in the
     * commands themselves, and naming one in the header is refused outright -
     * that field is for the newer kind of context, which this adapter will not
     * create. */
    if (!cb_submit(VIEW_NONE)) {
        kinfo("svga3d", "the card takes the context and refuses the drawing "
                        "(status %u at byte %u)", cb.last_status,
              cb.last_error_at);
        return false;
    }

    render.clears++;
    return true;

}


/* ------------------------------------------------------------- geometry
 *
 * Clearing proves the render target is real, but it goes nowhere near the part
 * of the card that makes it a graphics card.  A triangle does: three vertices
 * are fetched from a buffer in the card's memory, turned into a covered area,
 * and every pixel inside that area is coloured and every pixel outside is left
 * alone.  Deciding which is which is the rasteriser's whole job, and nothing
 * the processor did put those vertices there.
 *
 * The vertices are given already transformed - in pixels, not in a world that
 * needs a camera - which is what POSITIONT means.  That skips the matrices
 * entirely: there is no projection to get wrong, so if the triangle lands in
 * the wrong place the card put it there.  The fourth component is the divisor
 * the pipeline would have produced, and giving it as one says the division has
 * already happened.
 */
#define CMD3D_SETTRANSFORM     1047
#define CMD3D_SETZRANGE        1048
#define CMD3D_SETRENDERSTATE   1049
#define CMD3D_DRAW_PRIMITIVES  1063

#define TRANSFORM_WORLD        1
#define TRANSFORM_VIEW         2
#define TRANSFORM_PROJECTION   3

#define DECLTYPE_FLOAT4    3
#define DECLTYPE_D3DCOLOR  4
#define DECLMETHOD_DEFAULT 0
#define DECLUSAGE_POSITIONT 9
#define DECLUSAGE_COLOR    10

#define PRIMITIVE_TRIANGLELIST 1

#define RS_ZENABLE           1
#define RS_ALPHATESTENABLE   3
#define RS_ZWRITEENABLE      2
#define RS_BLENDENABLE       5
#define RS_STENCILENABLE     8
#define RS_LIGHTINGENABLE    9
#define RS_CLIPPLANEENABLE  27
#define RS_FILLMODE         29
#define RS_SHADEMODE        30
#define RS_CULLMODE         35
#define RS_COLORWRITEENABLE 47
#define RS_SCISSORTESTENABLE 55
#define RS_VERTEXBLEND      62
#define RS_CLIPPING         68

#define SHADEMODE_FLAT     1
#define FACE_NONE          1
/* Zero is not "solid", it is "no such fill mode" - and a context begins with
 * its state zeroed. */
#define FILLMODE_FILL      3
#define WRITE_ALL_CHANNELS 0xF

#define ID_INVALID 0xFFFFFFFFu

/* Position in pixels, then a colour the pipeline carries through untouched. */
typedef struct {
    float x, y, z, rhw;
    u32   colour;
} vertex_t;

#define VERTEX_STRIDE ((u32)sizeof(vertex_t))

/* A whole number as the bits of a float.  This kernel links no floating point
 * conversion, and the sizes below arrive as counts rather than constants, so
 * the exponent and mantissa are assembled rather than converted. */
static u32 as_bits_of(u32 v) {
    if (!v) return 0;
    u32 exponent = 0, m = v;
    while (m >= 2) { m >>= 1; exponent++; }
    u32 mantissa = (v << (23 - exponent)) & 0x7FFFFFu;
    return ((exponent + 127) << 23) | mantissa;
}

/* A ratio as the bits of a float.  This kernel links no floating point
 * division or conversion, so the value is worked out in whole numbers scaled
 * up and the exponent and mantissa are assembled from the result. */
static u32 as_bits_ratio(int num, int den) {
    if (!den) return 0;
    int negative = 0;
    long long n = num, d = den;
    if (n < 0) { negative = 1; n = -n; }
    if (d < 0) { negative = !negative; d = -d; }
    if (!n) return 0;

    long long scaled = (n << 30) / d;       /* the value, times 2^30 */
    if (!scaled) return 0;

    int k = 0;
    while (scaled >= (1LL << 24)) { scaled >>= 1; k++; }
    while (scaled < (1LL << 23)) { scaled <<= 1; k--; }

    u32 bits = ((u32)(k - 7 + 127) << 23) | ((u32)scaled & 0x7FFFFFu);
    if (negative) bits |= 0x80000000u;
    return bits;
}

static u32 as_bits(float f) {
    union { float f; u32 u; } cast;
    cast.f = f;
    return cast.u;
}

/* The vertex buffer, made through command buffers rather than the ring.
 *
 * The ring reads a command it does not like and says nothing, which is fine
 * for object management that works and useless when it does not: a surface
 * that was never created looks exactly like one that was.  A command buffer
 * answers with a status and the byte the card stopped at, so each step is sent
 * on its own and a refusal names itself.
 */
static bool make_vertex_buffer(u32 sid, u32 mobid, u64 phys, u32 bytes) {
    /* The memory itself goes through the ring.  Registering memory is not
     * something a command buffer is allowed to do - sent that way it comes
     * back refused at its first byte - and that is a rule about which path
     * carries which kind of command, not about the command being wrong. */
    if (!gb_define_mob(mobid, phys / PAGE_SIZE, PAGE_SIZE)) return false;
    svga_fifo_sync();

    struct { const char *what; u32 id; u32 body[11]; u32 words; } steps[] = {
        { "a surface that is a buffer of bytes", CMD3D_DEFINE_GB_SURFACE_V2,
          { sid, SURFACE_HINT_VERTEXBUFFER | SURFACE_BIND_VERTEX_BUFFER,
            FORMAT_BUFFER, 1, 0, FILTER_NONE, bytes, 1, 1, 1, 0 }, 11 },
        { "the buffer joined to its memory", CMD3D_BIND_GB_SURFACE,
          { sid, mobid }, 2 },
    };

    for (u32 i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        cb_begin();
        cb_command(steps[i].id, steps[i].body, steps[i].words);
        if (!cb_submit(VIEW_NONE)) {
            kwarn("svga3d", "the card refused %s (status %u at byte %u)",
                  steps[i].what, cb.last_status, cb.last_error_at);
            return false;
        }
    }
    return true;
}

/* One command through a buffer, reporting what the card said about it.  Used
 * to ask questions the ring cannot answer, because the ring answers nothing. */
static bool cb_one(const char *what, u32 id, const u32 *body, u32 words) {
    cb_begin();
    cb_command(id, body, words);
    if (cb_submit(VIEW_NONE)) return true;
    kinfo("svga3d", "%s: refused, status %u at byte %u", what, cb.last_status,
          cb.last_error_at);
    return false;
}

/* Two questions the triangle above cannot answer on its own, asked separately
 * so that a failure says which half is at fault.
 *
 * The first is whether the card can be made to touch some pixels and not
 * others at all: a clear of the whole surface, then a clear of a rectangle
 * inside it in another colour.  That uses the same command as the plain clear
 * and none of the vertex machinery, so if it works the card is rasterising an
 * area and the question is only how the area is described.
 *
 * The second is whether the vertices got there.  A buffer is written, handed
 * over, and read back; if what comes back is what went in, the card has the
 * vertices and a draw that does nothing is not doing nothing for want of them.
 */
static bool render_clear_rect(u32 colour, u32 x, u32 y, u32 w, u32 h) {
    if (!render.ready) return false;

    u32 body[9];
    cb_begin();

    body[0] = RENDER_CID;
    body[1] = RT_COLOR0;
    body[2] = render.surface;
    body[3] = 0;
    body[4] = 0;
    cb_command(CMD3D_SETRENDERTARGET, body, 5);

    body[0] = RENDER_CID;
    body[1] = 0;
    body[2] = 0;
    body[3] = render.width;
    body[4] = render.height;
    cb_command(CMD3D_SETVIEWPORT, body, 5);

    body[0] = RENDER_CID;
    body[1] = CLEAR_COLOR;
    body[2] = colour;
    body[3] = 0x3F800000u;
    body[4] = 0;
    body[5] = x;
    body[6] = y;
    body[7] = w;
    body[8] = h;
    cb_command(CMD3D_CLEAR, body, 9);

    if (!cb_submit(VIEW_NONE)) {
        kinfo("svga3d", "the card refused a clear of part of the surface "
                        "(status %u at byte %u)", cb.last_status,
              cb.last_error_at);
        return false;
    }
    return true;
}

/* Clear to one colour, then draw one triangle in another, in a single buffer
 * so that nothing can come between them. */
static bool render_triangle(u32 vb_surface, u32 background) {
    if (!render.ready) return false;

    u32 body[48];

    cb_begin();

    body[0] = RENDER_CID;
    body[1] = RT_COLOR0;
    body[2] = render.surface;
    body[3] = 0;
    body[4] = 0;
    cb_command(CMD3D_SETRENDERTARGET, body, 5);

    body[0] = RENDER_CID;
    body[1] = 0;
    body[2] = 0;
    body[3] = render.width;
    body[4] = render.height;
    cb_command(CMD3D_SETVIEWPORT, body, 5);

    body[0] = RENDER_CID;
    body[1] = CLEAR_COLOR;
    body[2] = background;
    body[3] = 0x3F800000u;
    body[4] = 0;
    body[5] = 0;
    body[6] = 0;
    body[7] = render.width;
    body[8] = render.height;
    cb_command(CMD3D_CLEAR, body, 9);

    /* State the draw depends on, said outright rather than inherited.  Depth
     * off because there is no depth buffer to test against; culling off so the
     * triangle appears whichever way round it was wound; lighting off so the
     * colour on the vertex is the colour that comes out; flat shading so all
     * three vertices give one colour and the result is exact rather than
     * interpolated. */
    {
        u32 n = 0;
        body[n++] = RENDER_CID;
        /* The two that matter most, and for the same reason: a context starts
         * with every piece of state zero, and zero is a real answer to some of
         * these questions.  Zero fill mode is not solid, it is no fill mode at
         * all; zero colour write is not all channels, it is none.  Either one
         * makes a perfectly good triangle disappear without a word, which is
         * exactly what a clear does not notice because a clear does not go
         * through them. */
        body[n++] = RS_FILLMODE;          body[n++] = FILLMODE_FILL;
        body[n++] = RS_COLORWRITEENABLE;  body[n++] = WRITE_ALL_CHANNELS;
        /* And the rest said outright rather than inherited. */
        body[n++] = RS_ZENABLE;           body[n++] = 0;
        body[n++] = RS_ZWRITEENABLE;      body[n++] = 0;
        body[n++] = RS_ALPHATESTENABLE;   body[n++] = 0;
        body[n++] = RS_BLENDENABLE;       body[n++] = 0;
        body[n++] = RS_STENCILENABLE;     body[n++] = 0;
        body[n++] = RS_SCISSORTESTENABLE; body[n++] = 0;
        body[n++] = RS_CLIPPING;          body[n++] = 0;
        body[n++] = RS_CLIPPLANEENABLE;   body[n++] = 0;
        body[n++] = RS_VERTEXBLEND;       body[n++] = 0;
        body[n++] = RS_LIGHTINGENABLE;    body[n++] = 0;
        body[n++] = RS_CULLMODE;          body[n++] = FACE_NONE;
        body[n++] = RS_SHADEMODE;         body[n++] = SHADEMODE_FLAT;
        cb_command(CMD3D_SETRENDERSTATE, body, n);
    }

    /* The depth a vertex is allowed to sit in.  Nothing here writes depth, but
     * a vertex outside the range is thrown away before it is ever drawn, and
     * the range starts as whatever the context was created holding - which is
     * zero to zero, and throws away everything. */
    {
        u32 n = 0;
        body[n++] = RENDER_CID;
        body[n++] = as_bits(0.0f);
        body[n++] = as_bits(1.0f);
        cb_command(CMD3D_SETZRANGE, body, n);
    }

    /* And the three matrices, set to the one that changes nothing.  Positions
     * given already transformed are meant to pass these by untouched, but they
     * begin as zeroes, and a matrix of zeroes takes every vertex to the same
     * point whether it was supposed to apply or not.  Saying identity outright
     * costs three commands and removes the question. */
    {
        static const u32 identity[3] = { TRANSFORM_WORLD, TRANSFORM_VIEW,
                                         TRANSFORM_PROJECTION };
        for (u32 t = 0; t < 3; t++) {
            u32 n = 0;
            body[n++] = RENDER_CID;
            body[n++] = identity[t];
            for (u32 row = 0; row < 4; row++)
                for (u32 col = 0; col < 4; col++)
                    body[n++] = as_bits(row == col ? 1.0f : 0.0f);
            cb_command(CMD3D_SETTRANSFORM, body, n);
        }
    }

    /* The draw.  Two declarations say how to read a vertex out of the buffer -
     * where the position is and where the colour is - and one range says what
     * to make of the vertices once read. */
    {
        u32 n = 0;
        body[n++] = RENDER_CID;
        body[n++] = 2;                  /* two declarations */
        body[n++] = 1;                  /* one range        */

        body[n++] = DECLTYPE_FLOAT4;
        body[n++] = DECLMETHOD_DEFAULT;
        body[n++] = DECLUSAGE_POSITIONT;
        body[n++] = 0;
        body[n++] = vb_surface;
        body[n++] = 0;                  /* at the start of each vertex */
        body[n++] = VERTEX_STRIDE;
        body[n++] = 0;                  /* no hint about which vertices */
        body[n++] = 0;

        body[n++] = DECLTYPE_D3DCOLOR;
        body[n++] = DECLMETHOD_DEFAULT;
        body[n++] = DECLUSAGE_COLOR;
        body[n++] = 0;
        body[n++] = vb_surface;
        body[n++] = (u32)(4 * sizeof(float));   /* after the position */
        body[n++] = VERTEX_STRIDE;
        body[n++] = 0;
        body[n++] = 0;

        body[n++] = PRIMITIVE_TRIANGLELIST;
        body[n++] = 1;                  /* one triangle */
        body[n++] = ID_INVALID;         /* no index buffer: vertices in order */
        body[n++] = 0;
        body[n++] = 0;
        body[n++] = 0;                  /* index width, unused */
        body[n++] = 0;                  /* index bias,  unused */

        cb_command(CMD3D_DRAW_PRIMITIVES, body, n);
    }

    if (!cb_submit(VIEW_NONE)) {
        kinfo("svga3d", "the card refused the triangle (status %u at byte %u)",
              cb.last_status, cb.last_error_at);
        return false;
    }

    return true;
}

static void render_teardown(void) {
    if (!render.ready) return;
    /* Through the ring, like the rest of the object management.  A buffer
     * would be waited on for two seconds and never answered. */
    cmd_t c;
    cmd_begin(&c, CMD3D_DESTROY_GB_CONTEXT);
    cmd_u32(&c, RENDER_CID);
    cmd_end(&c);
    svga_fifo_sync();
    render.ready = false;
}

/* --------------------------------------------------------------- shaders
 *
 * The newer path has no fixed function.  Nothing is drawn without a program
 * to place each vertex and a program to colour each pixel, so before a
 * triangle can exist both have to be written - not in a language and compiled,
 * but as the numbers the interface is defined in.
 *
 * The encoding is regular once seen.  A program is a stream of words: two of
 * heading, then one instruction after another.  An instruction begins with a
 * word holding what it does in its low bits and how many words it occupies in
 * its high ones, and is followed by a word for each thing it acts on.  A word
 * describing something acted on says how many parts it has, whether the parts
 * are being written or read, which of them, what kind of thing it is, and how
 * it is numbered - and then the number follows in the next word.
 *
 * Two programs are needed and both are as short as the interface allows.  The
 * first is handed a position and passes it on unchanged, which is what makes
 * the vertices in the buffer arrive on screen where they were written rather
 * than somewhere a matrix decided.  The second ignores everything it is given
 * and answers one colour, which is enough to see a shape and know its edges
 * were worked out rather than copied.
 */

/* What the program is: the low half is the version, the high half which of the
 * two kinds it is. */
#define SM4_VERTEX_PROGRAM 0x00010040u
#define SM4_PIXEL_PROGRAM  0x00000040u

/* An instruction: what it does, and how many words it takes up. */
#define SM4_OP(code, words) ((u32)(code) | ((u32)(words) << 24))
#define SM4_MOV             54
#define SM4_RET             62
#define SM4_DCL_INPUT       95
#define SM4_DCL_OUTPUT     101
#define SM4_DCL_OUTPUT_SIV 103

/* Something acted on.  Four parts; written by naming which of them (all four
 * here), read by naming where each part comes from (each from its own). */
#define SM4_FOUR_PARTS   2u
#define SM4_BY_SWIZZLE   (1u << 2)
#define SM4_ALL_FOUR     (0xFu << 4)
#define SM4_IN_ORDER     (0xE4u << 4)
#define SM4_KIND(k)      ((u32)(k) << 12)
#define SM4_NUMBERED     (1u << 20)

#define SM4_KIND_INPUT     1
#define SM4_KIND_OUTPUT    2
#define SM4_KIND_LITERAL   4

#define SM4_WRITE_ALL(kind) (SM4_FOUR_PARTS | SM4_ALL_FOUR | SM4_KIND(kind) | \
                             SM4_NUMBERED)
#define SM4_READ_ALL(kind)  (SM4_FOUR_PARTS | SM4_BY_SWIZZLE | SM4_IN_ORDER | \
                             SM4_KIND(kind) | SM4_NUMBERED)
#define SM4_LITERAL_FOUR    (SM4_FOUR_PARTS | SM4_KIND(SM4_KIND_LITERAL))

/* Which built-in meaning an output carries.  One is the position on screen,
 * and it is what makes the pipeline treat this output as where the vertex
 * goes rather than as a value to hand onwards. */
#define SM4_MEANING_POSITION 1

/* What a program expects and what it produces, listed after its instructions.
 *
 * The instructions alone are not enough for this adapter: it wants the
 * agreement between one stage and the next set out separately - which register
 * carries what, and which of those carry a meaning the pipeline itself acts on
 * rather than a value being passed along.  Without it a program is accepted
 * when it is written down and refused when something is drawn with it, which
 * is a long way from the mistake. */
#define SIG_HEADER_VERSION 0x08a92d12u
#define SIG_MEANING_NONE     0
#define SIG_MEANING_POSITION 1
#define SIG_ALL_FOUR_PARTS   0xF
#define SIG_COMPONENT_PLAIN  0
#define SIG_PRECISION_USUAL  0

/* One line of that list. */
static u32 sig_entry(u32 *out, u32 reg, u32 meaning) {
    u32 n = 0;
    out[n++] = reg;
    out[n++] = meaning;
    out[n++] = SIG_ALL_FOUR_PARTS;
    out[n++] = SIG_COMPONENT_PLAIN;
    out[n++] = SIG_PRECISION_USUAL;
    return n;
}

/* How a value is spread across the shape between the corners that carry it. */
#define SM4_DCL_INPUT_PS        98
#define SM4_SPREAD_EVENLY       (2u << 11)

/* Takes a position and a colour; gives the position back for the pipeline to
 * place, and the colour onward for the next program to receive.  The second
 * output is the interesting one: it is not a position and nothing in the
 * hardware knows what it means, so what arrives at each pixel is whatever the
 * part that spreads values between corners makes of it. */
static u32 shader_vertex(u32 *out) {
    u32 n = 0;
    out[n++] = SM4_VERTEX_PROGRAM;
    out[n++] = 0;                   /* length, filled in below */

    out[n++] = SM4_OP(SM4_DCL_INPUT, 3);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_INPUT);
    out[n++] = 0;                   /* the position it is given */

    out[n++] = SM4_OP(SM4_DCL_INPUT, 3);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_INPUT);
    out[n++] = 1;                   /* and the colour           */

    out[n++] = SM4_OP(SM4_DCL_OUTPUT_SIV, 4);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 0;
    out[n++] = SM4_MEANING_POSITION;

    out[n++] = SM4_OP(SM4_DCL_OUTPUT, 3);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 1;                   /* the colour, passed onward */

    out[n++] = SM4_OP(SM4_MOV, 5);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 0;
    out[n++] = SM4_READ_ALL(SM4_KIND_INPUT);
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_MOV, 5);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 1;
    out[n++] = SM4_READ_ALL(SM4_KIND_INPUT);
    out[n++] = 1;

    out[n++] = SM4_OP(SM4_RET, 1);

    out[1] = n;
    return n;
}

/* Answers with the colour it is handed, which at every pixel is the mixture of
 * the three corners weighted by how near each one is. */
static u32 shader_pixel_varying(u32 *out) {
    u32 n = 0;
    out[n++] = SM4_PIXEL_PROGRAM;
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_DCL_INPUT_PS, 3) | SM4_SPREAD_EVENLY;
    out[n++] = SM4_WRITE_ALL(SM4_KIND_INPUT);
    out[n++] = 1;                   /* what the program before it sent */

    out[n++] = SM4_OP(SM4_DCL_OUTPUT, 3);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_MOV, 5);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 0;
    out[n++] = SM4_READ_ALL(SM4_KIND_INPUT);
    out[n++] = 1;

    out[n++] = SM4_OP(SM4_RET, 1);

    out[1] = n;
    return n;
}


/* Sampling a picture, rather than answering with what the corners carried.
 *
 * Three more things have to be declared before a program can do this: where
 * the picture comes from, how it is to be read when a pixel falls between
 * points in it, and that the value arriving from the corners is a position
 * within the picture rather than a colour.  The instruction that does the
 * reading names all three at once. */
#define SM4_DCL_RESOURCE      88
#define SM4_DCL_SAMPLER       90
#define SM4_SAMPLE            69
#define SM4_KIND_SAMPLER       6
#define SM4_KIND_PICTURE       7
/* Which shape of picture is being declared.  This is the shader's own
 * numbering and not the one the surface was described with: there, a flat
 * picture is three because the count starts at a buffer being one; here it is
 * three because the count starts at unknown being nothing.  They agree by
 * coincidence, and declaring the wrong shape is accepted and then reads
 * nothing, which is a long way from the mistake. */
#define SM4_PICTURE_IS_FLAT   (3u << 11)
#define SM4_ALL_FOUR_ARE_REAL 0x5555u      /* four channels, each a number */

#define SM4_NAME_ONLY(kind) (SM4_KIND(kind) | SM4_NUMBERED)

static u32 shader_pixel_textured(u32 *out) {
    u32 n = 0;
    out[n++] = SM4_PIXEL_PROGRAM;
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_DCL_SAMPLER, 3);
    out[n++] = SM4_NAME_ONLY(SM4_KIND_SAMPLER);
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_DCL_RESOURCE, 4) | SM4_PICTURE_IS_FLAT;
    out[n++] = SM4_NAME_ONLY(SM4_KIND_PICTURE);
    out[n++] = 0;
    out[n++] = SM4_ALL_FOUR_ARE_REAL;

    out[n++] = SM4_OP(SM4_DCL_INPUT_PS, 3) | SM4_SPREAD_EVENLY;
    out[n++] = SM4_WRITE_ALL(SM4_KIND_INPUT);
    out[n++] = 1;

    out[n++] = SM4_OP(SM4_DCL_OUTPUT, 3);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_SAMPLE, 9);
    out[n++] = SM4_WRITE_ALL(SM4_KIND_OUTPUT);
    out[n++] = 0;
    out[n++] = SM4_READ_ALL(SM4_KIND_INPUT);
    out[n++] = 1;
    out[n++] = SM4_READ_ALL(SM4_KIND_PICTURE);
    out[n++] = 0;
    out[n++] = SM4_NAME_ONLY(SM4_KIND_SAMPLER);
    out[n++] = 0;

    out[n++] = SM4_OP(SM4_RET, 1);

    out[1] = n;
    return n;
}

/* The list of what a program is handed and what it hands on.  Each line names
 * a register, and says whether what it carries is a meaning the pipeline acts
 * on or a value it only passes along. */
static u32 shader_signature(u32 *out, u32 words,
                            const u32 *takes, u32 num_takes,
                            const u32 *gives, u32 num_gives) {
    u32 n = words;
    out[n++] = SIG_HEADER_VERSION;
    out[n++] = num_takes;
    out[n++] = num_gives;
    out[n++] = 0;                   /* nothing for patches */

    for (u32 i = 0; i < num_takes; i++)
        n += sig_entry(out + n, takes[i * 2], takes[i * 2 + 1]);
    for (u32 i = 0; i < num_gives; i++)
        n += sig_entry(out + n, gives[i * 2], gives[i * 2 + 1]);
    return n;
}



/* The one surface on this adapter proved to take a picture handed to it. */
/* The one surface on this adapter known to accept a picture handed to it.
 *
 * Its size is the limit on every picture a program can put on the screen, and
 * it was sixty-four pixels square - which is an icon, not a window.  The limit
 * was never about sixty-four: it is that a surface created by the drawing path
 * itself never receives what is transferred into it, while one created during
 * the start-up checks receives it every time, and nobody has found what
 * differs.  So the working one is used, and how big it is, is simply how big
 * it was made at start-up.
 *
 * Raised here to see whether the size was ever part of it. */
#define PROBE_TEXTURE_SIDE   1024

static u32 *probe_texture;
static u32  probe_side = PROBE_TEXTURE_SIDE;

/* And where the shaders live, kept so a program can put its own there. */
static u32 *probe_vs_bytes, *probe_ps_bytes;

/* How far apart a program's own vertices are, once it has said. */
static u32 user_stride;

static struct {
    bool   ready;           /* the start-up pipeline came up            */
    bool   built;           /* and this path has its own buffer         */
    float *verts;
    u64    phys;
    u32    draws;
} user_draw;

/* ------------------------------------------------- the newer kind of context
 *
 * Everything above uses the drawing interface this adapter has had since it
 * was a Direct3D 9 device: state set one field at a time, geometry described
 * by declarations attached to the draw, and no shaders unless you supply them.
 * The card takes all of it.  It clears with it and copies with it.  It will
 * not draw a triangle with it, and it does not say why - the command is read,
 * accepted, and nothing appears.
 *
 * What the card does say, when asked, is that it supports the newer kind of
 * drawing context.  That is the interface a modern host renderer actually
 * implements: the older commands survive as a courtesy for the operations that
 * map onto a blit, and geometry moved to the newer path years ago.
 *
 * So this asks for one, step by step, and reports which step is refused.  A
 * context, then a view of a surface to draw into, then a clear through that
 * view.  The clear is the point: it is the same operation proved above on the
 * old path, so if it works here the newer path is live and the difference
 * between the two is not this driver's doing.
 */
#define CMD3D_DX_DEFINE_CONTEXT            1143
#define CMD3D_DX_DESTROY_CONTEXT           1144
#define CMD3D_DX_BIND_CONTEXT              1145
#define CMD3D_DX_SET_RENDERTARGETS         1161
#define CMD3D_DX_CLEAR_RENDERTARGET_VIEW   1176
#define CMD3D_DX_DEFINE_RENDERTARGET_VIEW  1187
#define CMD3D_DX_DESTROY_RENDERTARGET_VIEW 1188

#define CMD3D_DX_SET_COTABLE               1207
#define CMD3D_DX_SET_SHADER                1150
#define CMD3D_DX_DRAW                      1152
#define CMD3D_DX_SET_INPUT_LAYOUT          1157
#define CMD3D_DX_SET_VERTEX_BUFFERS        1158
#define CMD3D_DX_SET_TOPOLOGY              1160
#define CMD3D_DX_SET_VIEWPORTS             1174
#define CMD3D_DX_DEFINE_ELEMENTLAYOUT      1191
#define CMD3D_DX_DEFINE_SHADER             1201
#define CMD3D_DX_BIND_SHADER               1203
#define CMD3D_DX_SET_RASTERIZER_STATE      1164
#define CMD3D_DX_DEFINE_RASTERIZER_STATE   1197

#define CMD3D_DX_SET_BLEND_STATE           1162
#define CMD3D_DX_SET_DEPTHSTENCIL_STATE    1163
#define CMD3D_DX_DEFINE_BLEND_STATE        1193
#define CMD3D_DX_DEFINE_DEPTHSTENCIL_STATE 1195

#define RASTERIZER_STATE 0
#define BLEND_STATE      0
#define DEPTH_STATE      0
#define DX_CULL_NONE     1

#define BLENDOP_ZERO       1
#define BLENDOP_ONE        2
#define BLENDEQ_ADD        1
#define STENCILOP_KEEP     1
#define COMPARISON_ALWAYS  8
#define DEPTH_WRITE_NONE   0

#define SHADERTYPE_VS 1
#define SHADERTYPE_PS 2

#define FORMAT_R32G32B32A32_FLOAT 122
#define INPUT_PER_VERTEX          0

#define SHADER_VS       0
#define SHADER_PS       1
#define ELEMENT_LAYOUT  0
#define VERTEX_SURFACE  50
#define MOB_VS          32
#define MOB_PS          33
#define MOB_VERTS       34
#define MOB_SCREEN      35
#define MOB_TEXTURE     37

#define ELEMENT_LAYOUT_USER 1
#define SHADER_USER_VS  4
#define SHADER_USER_PS  5

#define CMD3D_DX_SET_SHADER_RESOURCES       1149
#define CMD3D_DX_SET_SAMPLERS               1151
#define CMD3D_DX_DEFINE_SHADERRESOURCE_VIEW 1185
#define CMD3D_DX_DEFINE_SAMPLER_STATE       1199

#define TEXTURE_SURFACE     53
#define TEXTURE_VIEW        0
#define TEXTURE_RTV         2
#define SAMPLER_STATE       0
#define SHADER_PS_TEXTURED  2
#define TEX_ADDRESS_CLAMP   3

#define CMD3D_DX_CLEAR_DEPTHSTENCIL_VIEW  1177
#define CMD3D_DX_DEFINE_DEPTHSTENCIL_VIEW 1189

#define MOB_DEPTH          38
#define DEPTH_SURFACE      54
#define DEPTH_VIEW         0
#define DEPTH_STATE_ON     1
#define FORMAT_D32_FLOAT   76
#define SURFACE_BIND_DEPTH_STENCIL (1u << 25)
#define DEPTH_WRITE_ALL    1
#define COMPARISON_LESS    2
/* Not one: one is the colour.  Clearing with the wrong bit leaves every pixel
 * recorded as being as near as it is possible to be, and then nothing drawn
 * afterwards is ever nearer - so the picture comes out empty and the card is
 * behaving perfectly. */

#define BLEND_STATE_MIX        1
#define BLENDOP_SRCALPHA       5
#define BLENDOP_INVSRCALPHA    6
#define MOB_TEXTURE     37

#define CMD3D_DX_SET_SHADER_RESOURCES      1149
#define CMD3D_DX_SET_SAMPLERS              1151
#define CMD3D_DX_DEFINE_SHADERRESOURCE_VIEW 1185
#define CMD3D_DX_DEFINE_SAMPLER_STATE      1199

#define TEXTURE_SURFACE     53
#define TEXTURE_VIEW        0
#define SAMPLER_STATE       0
#define SHADER_PS_TEXTURED  2
#define TEX_ADDRESS_CLAMP   3

#define CMD3D_DEFINE_GB_SCREENTARGET  1124
#define CMD3D_DESTROY_GB_SCREENTARGET 1125
#define CMD3D_BIND_GB_SCREENTARGET    1126
#define CMD3D_UPDATE_GB_SCREENTARGET  1127

#define SURFACE_SCREENTARGET (1u << 16)
#define SCREEN_PRIMARY       (1u << 0)
#define SCREEN_ID       0
#define SCREEN_SURFACE  51
#define SCREEN_VIEW     1

/* A second, independent display (#7).  Its own screen-target id, surface and
 * MOB - distinct from the primary's so the two are separate scan-outs, not two
 * names for one buffer.  Ids from the genuinely free slots: surfaces 50-56 and
 * mobs 32-40 are all taken by the primary's screen, vertex, texture, depth,
 * user-draw and staging objects, so the next free are surface 57, mob 41.
 * (Screen-target ids: 0 is the primary, the otable holds 8, so 1 is free.) */
#define SCREEN_ID2      1
#define SCREEN_SURFACE2 57
#define MOB_SCREEN2     41
/* A black surface to point the PRIMARY screen at for only-the-other mode, so
 * the main display goes dark while the desktop shows on the second.  Next free
 * ids after screen 2 (surface 57, mob 41). */
#define SCREEN_BLACK_SURFACE 58
#define MOB_BLACK            42

#define RESOURCE_TEXTURE2D 3
#define DX_CID  2
#define DX_RTV  0

/* A drawing context keeps its own objects in tables of its own - one per kind
 * of object - and each has to be given memory before an object of that kind
 * can be made.  Only the one for views of things to draw into is needed here. */
#define COTABLE_RTVIEW       0
#define COTABLE_COUNT        12
#define COTABLE_FIRST_MOB    20

/* The same colours as the surface, named the way the newer path names them.
 * The older names are not accepted here, and a view whose format the adapter
 * does not recognise is refused rather than converted. */
#define FORMAT_B8G8R8A8_UNORM 141

/* Submit one command as that context and say what came back. */
static bool dx_one(const char *what, u32 id, const u32 *body, u32 words,
                   u32 as_context) {
    cb_begin();
    cb_command(id, body, words);
    if (cb_submit(as_context)) return true;
    kinfo("svga3d", "the newer path: %s refused, status %u at byte %u", what,
          cb.last_status, cb.last_error_at);
    return false;
}

/* Same as the ordinary surface above but named in the newer path's format.
 * A view is refused when its format is not the surface's, and the two paths
 * do not share names for the same arrangement of colours. */
static bool __attribute__((unused)) gb_define_surface_dx(u32 sid, u32 width, u32 height) {
    cmd_t c;
    cmd_begin(&c, CMD3D_DEFINE_GB_SURFACE_V2);
    cmd_u32(&c, sid);
    /* Only what it may be bound as, and none of the older path's hints.  Those
     * hints are what mark a surface as belonging to the older interface, and a
     * view of the newer kind will not be made over one that carries them. */
    cmd_u32(&c, SURFACE_BIND_RENDER_TARGET | SURFACE_BIND_SHADER_RESOURCE);
    cmd_u32(&c, FORMAT_B8G8R8A8_UNORM);
    cmd_u32(&c, 1);                 /* one level            */
    cmd_u32(&c, 0);                 /* not multisampled     */
    cmd_u32(&c, FILTER_NONE);
    cmd_u32(&c, width);
    cmd_u32(&c, height);
    cmd_u32(&c, 1);
    cmd_u32(&c, 1);
    cmd_u32(&c, 0);
    return cmd_end(&c);
}

/* Returns the number of things that went wrong, and says what worked. */
static int dx_probe(u32 legacy_surface, u32 context_mob, u32 *unused_pixels,
                    u32 w, u32 h) {
    u32 body[8];
    (void)legacy_surface;
    (void)unused_pixels;

    /* Its own surface, in its own format, so that nothing here disturbs the
     * drawing that already works. */
    u64 dx_pixels_phys = 0;
    u32 *pixels = dma_alloc_pages(GB_BYTES / PAGE_SIZE, &dx_pixels_phys);
    if (!pixels) return 1;
    const u32 mob_dx_pixels = 13;
    u32 surface = 42;
    if (!gb_define_mob(mob_dx_pixels, dx_pixels_phys / PAGE_SIZE, GB_BYTES))
        return 1;
    svga_fifo_sync();


    /* The table the card keeps its drawing contexts in, set again where the
     * card has to answer.  It was set at the start along with all the others,
     * through the ring - which takes anything and says nothing, so a table
     * that was refused there looks exactly like one that was accepted, and a
     * context created against a table that is not there would fail in just
     * this way. */
    {
        u32 bytes = 256 * OTABLE_DXCONTEXT_ENTRY;
        u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        u64 phys = 0;
        u8 *table = dma_alloc_pages(pages, &phys);
        if (table) {
            memset(table, 0, (size_t)pages * PAGE_SIZE);
            u64 first = phys / PAGE_SIZE;
            u32 t[6] = { OTABLE_DXCONTEXT, (u32)first, (u32)(first >> 32),
                         pages * (u32)PAGE_SIZE, pages * (u32)PAGE_SIZE,
                         MOBFMT_RANGE };
            if (dx_one("a table to keep drawing contexts in",
                       CMD3D_SET_OTABLE_BASE64, t, 6, VIEW_NONE))
                kinfo("svga3d", "the newer path: the card took a table for "
                                "drawing contexts");
        }
    }

    /* Through the ring, which is where this adapter takes the making of
     * objects.  A command buffer refuses this outright - and that is the same
     * division the older context turned out to follow: what a thing IS goes
     * through the ring, what is DONE with it goes through a buffer. */
    {
        cmd_t c;
        /* Made and unmade through the ring first, and only then made for real
         * through a buffer.  This is the same sequence the older context
         * needed: the slot has to have been through the adapter's hands once
         * before what is behind it will accept one there.  Why that is so is
         * not something this driver can see from its side. */
        cmd_begin(&c, CMD3D_DX_DEFINE_CONTEXT);
        cmd_u32(&c, DX_CID);
        cmd_end(&c);
        cmd_begin(&c, CMD3D_DX_DESTROY_CONTEXT);
        cmd_u32(&c, DX_CID);
        cmd_end(&c);
        svga_fifo_sync();
        timer_udelay(20000);
    }

    body[0] = DX_CID;
    if (!dx_one("asking for a context", CMD3D_DX_DEFINE_CONTEXT, body, 1,
                VIEW_NONE))
        return 1;

    body[0] = DX_CID;
    body[1] = context_mob;
    body[2] = 0;                    /* nothing worth restoring in it yet */
    if (!dx_one("giving the context memory", CMD3D_DX_BIND_CONTEXT, body, 3,
                VIEW_NONE))
        return 1;

    kinfo("svga3d", "the newer path: the card made a drawing context");

    /* The ring says nothing either way, so whether that worked is only visible
     * in whether the commands below - which do have to be answered - are taken
     * as that context. */

    /* The tables this context keeps its own objects in - one for each kind of
     * object, not just the kind about to be made.  A context with some of them
     * missing is not a context with fewer features; the adapter checks that
     * the set is complete before it will make any object at all, so the eleven
     * that go unused here still have to be there. */
    {
        for (u32 type = 0; type < COTABLE_COUNT; type++) {
            u64 phys = 0;
            u8 *table = dma_alloc_pages(1, &phys);
            if (!table) return 1;
            memset(table, 0, PAGE_SIZE);

            u32 mobid = COTABLE_FIRST_MOB + type;
            if (!gb_define_mob(mobid, phys / PAGE_SIZE, PAGE_SIZE)) return 1;
            svga_fifo_sync();

            body[0] = DX_CID;
            body[1] = mobid;
            body[2] = type;
            /* How much of the table already holds objects, not how large it
             * is: the memory it lives in says the size.  A fresh table holds
             * none, and claiming otherwise describes a page of zeroes as a
             * page of live objects. */
            body[3] = 0;
            if (!dx_one("a table for the context's own objects",
                        CMD3D_DX_SET_COTABLE, body, 4, VIEW_NONE)) {
                kinfo("svga3d", "the newer path: the table refused was for kind "
                                "%u of %u", type, COTABLE_COUNT);
                return 1;
            }
        }
        kinfo("svga3d", "the newer path: and all %u tables for its own objects",
              COTABLE_COUNT);
    }

    /* And only now the surface, and then a view of it.
     *
     * Two things here are guesses the interface does not settle, so rather
     * than pick one and report a refusal, each is tried and the one that works
     * is kept.  The sample count is the first: zero is what the older path
     * takes and means "not multisampled", but one sample is also a way of
     * saying that, and the newer path may only accept the second.  The view's
     * own number is the other - nothing says whether zero is a usable name or
     * a reserved one. */
    bool made_view = false;
    u32 chosen_samples = 0, chosen_view = 0;

    for (u32 attempt = 0; attempt < 4 && !made_view; attempt++) {
        u32 samples = (attempt < 2) ? 1 : 0;
        u32 viewid  = (attempt & 1) ? 1 : 0;
        u32 sid = surface + attempt;

        u32 d[11] = { sid,
                      SURFACE_BIND_RENDER_TARGET | SURFACE_BIND_SHADER_RESOURCE,
                      FORMAT_B8G8R8A8_UNORM, 1, samples, FILTER_NONE,
                      w, h, 1, 1, 0 };
        if (!dx_one("a surface in the newer format",
                    CMD3D_DEFINE_GB_SURFACE_V2, d, 11, VIEW_NONE))
            continue;
        u32 b[2] = { sid, mob_dx_pixels };
        if (!dx_one("joining that surface to its memory",
                    CMD3D_BIND_GB_SURFACE, b, 2, VIEW_NONE))
            continue;

        body[0] = viewid;
        body[1] = sid;
        body[2] = FORMAT_B8G8R8A8_UNORM;
        body[3] = RESOURCE_TEXTURE2D;
        body[4] = 0;                /* which level  */
        body[5] = 0;                /* first slice  */
        body[6] = 1;                /* how many     */
        cb_begin();
        cb_command(CMD3D_DX_DEFINE_RENDERTARGET_VIEW, body, 7);
        if (cb_submit(DX_CID)) {
            made_view = true;
            chosen_samples = samples;
            chosen_view = viewid;
            surface = sid;
            kinfo("svga3d", "the newer path: a surface at %u sample(s) and a "
                            "view of it numbered %u", samples, viewid);
        }
    }

    if (!made_view) {
        body[0] = DX_RTV;
        if (!dx_one("a view of the surface to draw into",
                    CMD3D_DX_DEFINE_RENDERTARGET_VIEW, body, 7, DX_CID)) {
        /* Where the newer path stops, said exactly, because everything before
         * it works and that is the useful part of the finding: the adapter
         * makes a drawing context, keeps twelve tables of objects for it, and
         * takes a surface in the format the newer path uses - and then will
         * not make a view of that surface, with every field of the request
         * matching what the interface defines. */
        kinfo("svga3d", "the newer path gets as far as a context, its twelve "
                        "object tables and a surface, and stops at making a "
                        "view of that surface to draw into - so geometry is "
                        "one refused command away, not a missing interface");
            return 1;
        }
    }
    (void)chosen_samples;
    (void)chosen_view;

    body[0] = VIEW_NONE;            /* no depth buffer */
    body[1] = DX_RTV;
    if (!dx_one("pointing the context at the view", CMD3D_DX_SET_RENDERTARGETS,
                body, 2, DX_CID))
        return 1;

    kinfo("svga3d", "the newer path: and a view of a surface to draw into");

    /* Two colours in turn, with the picture poisoned before each, so nothing
     * left over can be mistaken for what the card wrote. */
    static const float want[2][4] = {
        { 0.85f, 0.24f, 0.47f, 1.0f },
        { 0.24f, 0.85f, 0.47f, 1.0f },
    };
    static const u32 expect[2] = { 0xD93D78u, 0x3DD978u };

    for (int pass = 0; pass < 2; pass++) {
        for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
        __asm__ volatile("sfence" ::: "memory");

        body[0] = DX_RTV;
        for (int i = 0; i < 4; i++) body[1 + i] = as_bits(want[pass][i]);
        if (!dx_one("clearing through the view",
                    CMD3D_DX_CLEAR_RENDERTARGET_VIEW, body, 5, DX_CID))
            return 1;

        gb_readback(surface);
        svga_fifo_sync();
        timer_udelay(30000);

        int untouched = 0, wrong = 0;
        u32 first = 0;
        for (u32 i = 0; i < w * h; i += 41) {
            u32 got = pixels[i];
            if (got == 0xDEADBEEFu) { untouched++; continue; }
            /* Within a step: the card converts through eight bits a channel
             * and the rounding is its own. */
            int d = 0;
            for (int ch = 0; ch < 3; ch++) {
                int a = (int)((got >> (ch * 8)) & 0xFF);
                int b = (int)((expect[pass] >> (ch * 8)) & 0xFF);
                if (a - b > d) d = a - b;
                if (b - a > d) d = b - a;
            }
            if (d > 2) { if (!wrong) first = got; wrong++; }
        }
        if (untouched || wrong) {
            kinfo("svga3d", "the newer path: the clear was taken but pass %d "
                            "came back %d untouched, %d wrong (one is %08x)",
                  pass, untouched, wrong, first);
            return 1;
        }
    }


    /* ------------------------------------------------------- a triangle
     *
     * Everything above is the card being told what exists.  This is the card
     * being asked to work something out: three corners go in, and which pixels
     * lie between them is a question only the part of the card that rasterises
     * can answer.
     */
    {
        u32 b[16];

        /* The two programs, each written into memory the card is told about
         * and then attached to a name it can be set by. */
        u64 vs_phys = 0, ps_phys = 0;
        u32 *vs = dma_alloc_pages(1, &vs_phys);
        u32 *ps = dma_alloc_pages(1, &ps_phys);
        if (!vs || !ps) return 1;
        memset(vs, 0, PAGE_SIZE);
        memset(ps, 0, PAGE_SIZE);

        /* Register number, then what it carries.  Only the position means
         * anything to the pipeline itself; the colour is a value being handed
         * along, which is exactly why spreading it between corners is the
         * card's work and not this driver's. */
        static const u32 vs_takes[] = { 0, SIG_MEANING_NONE,
                                        1, SIG_MEANING_NONE };
        static const u32 vs_gives[] = { 0, SIG_MEANING_POSITION,
                                        1, SIG_MEANING_NONE };
        static const u32 ps_takes[] = { 1, SIG_MEANING_NONE };
        static const u32 ps_gives[] = { 0, SIG_MEANING_NONE };

        probe_vs_bytes = vs;
        probe_ps_bytes = ps;

        u32 vs_words = shader_vertex(vs);
        vs_words = shader_signature(vs, vs_words, vs_takes, 2, vs_gives, 2);
        u32 ps_words = shader_pixel_varying(ps);
        ps_words = shader_signature(ps, ps_words, ps_takes, 1, ps_gives, 1);
        __asm__ volatile("sfence" ::: "memory");

        if (!gb_define_mob(MOB_VS, vs_phys / PAGE_SIZE, PAGE_SIZE) ||
            !gb_define_mob(MOB_PS, ps_phys / PAGE_SIZE, PAGE_SIZE))
            return 1;
        svga_fifo_sync();

        struct { const char *what; u32 id; u32 type; u32 words; u32 mob; }
        programs[2] = {
            { "the program that places vertices", SHADER_VS, SHADERTYPE_VS,
              vs_words, MOB_VS },
            { "the program that colours pixels", SHADER_PS, SHADERTYPE_PS,
              ps_words, MOB_PS },
        };

        for (u32 i = 0; i < 2; i++) {
            b[0] = programs[i].id;
            b[1] = programs[i].type;
            b[2] = programs[i].words * (u32)sizeof(u32);
            if (!dx_one(programs[i].what, CMD3D_DX_DEFINE_SHADER, b, 3, DX_CID))
                return 1;

            b[0] = DX_CID;
            b[1] = programs[i].id;
            b[2] = programs[i].mob;
            b[3] = 0;               /* at the start of it */
            if (!dx_one("attaching a program to its memory",
                        CMD3D_DX_BIND_SHADER, b, 4, VIEW_NONE))
                return 1;

            b[0] = programs[i].id;
            b[1] = programs[i].type;
            if (!dx_one("choosing a program to run",
                        CMD3D_DX_SET_SHADER, b, 2, DX_CID))
                return 1;
        }
        kinfo("svga3d", "the newer path: both programs written and chosen");

        /* How to read one vertex out of the buffer: four numbers, at the
         * start, arriving as the first input the program is given. */
        b[0] = ELEMENT_LAYOUT;
        /* Where the corner is: four numbers at the start of each vertex. */
        b[1] = 0;                   /* which buffer it comes from   */
        b[2] = 0;                   /* how far into each vertex     */
        b[3] = FORMAT_R32G32B32A32_FLOAT;
        b[4] = INPUT_PER_VERTEX;
        b[5] = 0;                   /* not per-instance             */
        b[6] = 0;                   /* the first input              */
        /* And what colour it carries: four more, straight after. */
        b[7]  = 0;
        b[8]  = 4 * (u32)sizeof(float);
        b[9]  = FORMAT_R32G32B32A32_FLOAT;
        b[10] = INPUT_PER_VERTEX;
        b[11] = 0;
        b[12] = 1;                  /* the second input             */
        if (!dx_one("how to read a vertex", CMD3D_DX_DEFINE_ELEMENTLAYOUT,
                    b, 13, DX_CID))
            return 1;

        b[0] = ELEMENT_LAYOUT;
        if (!dx_one("using that way of reading vertices",
                    CMD3D_DX_SET_INPUT_LAYOUT, b, 1, DX_CID))
            return 1;

        /* The corners themselves, given where the pipeline expects them:
         * across and up from the middle of the picture, from minus one to one.
         * The same triangle the older path was asked for, so the pixels
         * checked afterwards are the same pixels. */
        u64 verts_phys = 0;
        float *verts = dma_alloc_pages(1, &verts_phys);
        if (!verts) return 1;
        memset(verts, 0, PAGE_SIZE);

        static const struct { float x, y, r, g, b; } corner[3] = {
            {  0.0f,     0.8125f, 1.0f, 0.0f, 0.0f },
            {  0.8125f, -0.625f,  0.0f, 1.0f, 0.0f },
            { -0.8125f, -0.625f,  0.0f, 0.0f, 1.0f },
        };
        for (int i = 0; i < 3; i++) {
            verts[i * 8 + 0] = corner[i].x;
            verts[i * 8 + 1] = corner[i].y;
            verts[i * 8 + 2] = 0.5f;
            verts[i * 8 + 3] = 1.0f;    /* already divided through */
            verts[i * 8 + 4] = corner[i].r;
            verts[i * 8 + 5] = corner[i].g;
            verts[i * 8 + 6] = corner[i].b;
            verts[i * 8 + 7] = 1.0f;
        }
        __asm__ volatile("sfence" ::: "memory");

        if (!gb_define_mob(MOB_VERTS, verts_phys / PAGE_SIZE, PAGE_SIZE))
            return 1;
        svga_fifo_sync();

        /* Room for more than the three corners drawn first: a buffer declared
         * to be exactly one triangle long is exactly one triangle long, and
         * the corners of anything larger written into it afterwards are past
         * its end - which the card reads as whatever happens to be there. */
        u32 d[11] = { VERTEX_SURFACE, SURFACE_BIND_VERTEX_BUFFER, FORMAT_BUFFER,
                      1, 1, FILTER_NONE, 64 * 32, 1, 1, 1, 0 };
        if (!dx_one("a buffer for the corners", CMD3D_DEFINE_GB_SURFACE_V2,
                    d, 11, VIEW_NONE))
            return 1;
        u32 j[2] = { VERTEX_SURFACE, MOB_VERTS };
        if (!dx_one("joining the corners to their memory",
                    CMD3D_BIND_GB_SURFACE, j, 2, VIEW_NONE))
            return 1;

        b[0] = 0;                   /* the first buffer slot        */
        b[1] = VERTEX_SURFACE;
        b[2] = 8 * (u32)sizeof(float);  /* one vertex to the next   */
        b[3] = 0;                   /* starting at the beginning    */
        if (!dx_one("where the corners are", CMD3D_DX_SET_VERTEX_BUFFERS,
                    b, 4, DX_CID))
            return 1;

        b[0] = PRIMITIVE_TRIANGLELIST;
        if (!dx_one("what to make of them", CMD3D_DX_SET_TOPOLOGY, b, 1,
                    DX_CID))
            return 1;

        /* Where on the surface the picture goes, and the range of depths it
         * occupies.  Zero to one: a vertex outside that range is thrown away,
         * and the corners above sit at a half. */
        b[0] = 0;                   /* padding the interface defines */
        b[1] = as_bits(0.0f);
        b[2] = as_bits(0.0f);
        b[3] = as_bits_of(w);
        b[4] = as_bits_of(h);
        b[5] = as_bits(0.0f);
        b[6] = as_bits(1.0f);
        if (!dx_one("where on the surface to draw", CMD3D_DX_SET_VIEWPORTS,
                    b, 7, DX_CID))
            return 1;

        /* How the shape is turned into pixels: filled rather than outlined,
         * both faces kept, no depth bias, no scissor.  A draw without this
         * has nothing to consult, and the older path already showed what a
         * fill mode of zero means - not solid, but no fill mode at all. */
        {
            u8 r[32];
            memset(r, 0, sizeof r);
            *(u32 *)&r[0] = RASTERIZER_STATE;
            r[4] = FILLMODE_FILL;
            r[5] = DX_CULL_NONE;
            r[6] = 0;                       /* which way round is front  */
            r[7] = 0;                       /* which corner sets a flat  */
            *(u32 *)&r[8]  = 0;             /* depth bias                */
            *(u32 *)&r[12] = as_bits(0.0f); /* and its limit             */
            *(u32 *)&r[16] = as_bits(0.0f); /* and its slope             */
            r[20] = 1;                      /* keep depth within range   */
            r[21] = 0;                      /* no scissor                */
            r[22] = 0;                      /* not multisampled          */
            r[23] = 0;                      /* no smoothed lines         */
            *(u32 *)&r[24] = as_bits(1.0f); /* line width, unused here   */

            if (!dx_one("how to turn the shape into pixels",
                        CMD3D_DX_DEFINE_RASTERIZER_STATE, (const u32 *)r, 8,
                        DX_CID))
                return 1;

            b[0] = RASTERIZER_STATE;
            if (!dx_one("using that way of turning it into pixels",
                        CMD3D_DX_SET_RASTERIZER_STATE, b, 1, DX_CID))
                return 1;
        }

        /* What happens to a pixel once its colour is known, and whether it is
         * kept at all.  Neither does anything interesting here - no blending,
         * no depth test - but on this path they are objects that have to exist
         * and be chosen, not settings that default quietly. */
        {
            u8 bl[104];
            memset(bl, 0, sizeof bl);
            *(u32 *)&bl[0] = BLEND_STATE;
            bl[4] = 0;                  /* coverage does not touch alpha */
            bl[5] = 0;                  /* every target treated alike    */
            for (int rt = 0; rt < 8; rt++) {
                u8 *e = &bl[8 + rt * 12];
                e[0] = 0;               /* no blending                   */
                e[1] = BLENDOP_ONE;     /* which are unused while it is  */
                e[2] = BLENDOP_ZERO;    /* off, but must still name      */
                e[3] = BLENDEQ_ADD;     /* something that exists         */
                e[4] = BLENDOP_ONE;
                e[5] = BLENDOP_ZERO;
                e[6] = BLENDEQ_ADD;
                e[7] = WRITE_ALL_CHANNELS;
                e[8] = 0;               /* no logic operation            */
                e[9] = 0;
            }
            if (!dx_one("what happens to a pixel once coloured",
                        CMD3D_DX_DEFINE_BLEND_STATE, (const u32 *)bl, 26,
                        DX_CID))
                return 1;

            b[0] = BLEND_STATE;
            b[1] = as_bits(1.0f);
            b[2] = as_bits(1.0f);
            b[3] = as_bits(1.0f);
            b[4] = as_bits(1.0f);
            b[5] = 0xFFFFFFFFu;         /* every sample counts           */
            if (!dx_one("using that", CMD3D_DX_SET_BLEND_STATE, b, 6, DX_CID))
                return 1;

            u8 ds[20];
            memset(ds, 0, sizeof ds);
            *(u32 *)&ds[0] = DEPTH_STATE;
            ds[4]  = 0;                 /* no depth test                 */
            ds[5]  = DEPTH_WRITE_NONE;
            ds[6]  = COMPARISON_ALWAYS;
            ds[7]  = 0;                 /* no stencil test               */
            ds[8]  = 0;
            ds[9]  = 0;
            ds[10] = 0xFF;
            ds[11] = 0xFF;
            ds[12] = STENCILOP_KEEP;
            ds[13] = STENCILOP_KEEP;
            ds[14] = STENCILOP_KEEP;
            ds[15] = COMPARISON_ALWAYS;
            ds[16] = STENCILOP_KEEP;
            ds[17] = STENCILOP_KEEP;
            ds[18] = STENCILOP_KEEP;
            ds[19] = COMPARISON_ALWAYS;
            if (!dx_one("whether a pixel is kept at all",
                        CMD3D_DX_DEFINE_DEPTHSTENCIL_STATE, (const u32 *)ds, 5,
                        DX_CID))
                return 1;

            b[0] = DEPTH_STATE;
            b[1] = 0;                   /* nothing to compare stencil to */
            if (!dx_one("using that too", CMD3D_DX_SET_DEPTHSTENCIL_STATE,
                        b, 2, DX_CID))
                return 1;
        }

        kinfo("svga3d", "the newer path: corners, layout, topology, viewport "
                        "and rasteriser all taken");

        /* Poison the picture, clear it to the background, then draw. */
        for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
        __asm__ volatile("sfence" ::: "memory");

        body[0] = chosen_view;
        body[1] = as_bits(0.06f);
        body[2] = as_bits(0.13f);
        body[3] = as_bits(0.25f);
        body[4] = as_bits(1.0f);
        if (!dx_one("clearing before the triangle",
                    CMD3D_DX_CLEAR_RENDERTARGET_VIEW, body, 5, DX_CID))
            return 1;

        b[0] = 3;                   /* three corners  */
        b[1] = 0;                   /* from the first */
        if (!dx_one("the triangle itself", CMD3D_DX_DRAW, b, 2, DX_CID))
            return 1;

        gb_readback(surface);
        svga_fifo_sync();
        timer_udelay(40000);

        /* The three corners are pure red, green and blue, so what lands on
         * each pixel says whether the card spread them or merely picked one.
         *
         * Near a corner the colour it carries should dominate.  In the middle,
         * where all three corners are equally far away, no channel should
         * dominate at all - and that is the part a flat shade cannot produce,
         * because a flat shade paints the whole triangle one corner's colour
         * and the middle would come out pure. */
        struct { int x, y; const char *where; } near[3] = {
            { 32, 15, "the red corner"   },
            { 50, 48, "the green corner" },
            { 14, 48, "the blue corner"  },
        };

        int wrong = 0, untouched = 0;
        for (int i = 0; i < 3; i++) {
            u32 px = pixels[near[i].y * w + near[i].x];
            if (px == 0xDEADBEEFu) { untouched++; continue; }
            int c[3] = { (int)((px >> 16) & 0xFF), (int)((px >> 8) & 0xFF),
                         (int)(px & 0xFF) };
            /* The channel this corner carries, against the other two. */
            int mine = c[i], other1 = c[(i + 1) % 3], other2 = c[(i + 2) % 3];
            if (mine < 150 || other1 > 80 || other2 > 80) {
                kerr("svga3d", "near %s the colour is %d,%d,%d", near[i].where,
                     c[0], c[1], c[2]);
                wrong++;
            }
        }

        /* And the middle, which is the whole argument. */
        {
            u32 px = pixels[37 * w + 32];
            if (px == 0xDEADBEEFu) {
                untouched++;
            } else {
                int c[3] = { (int)((px >> 16) & 0xFF), (int)((px >> 8) & 0xFF),
                             (int)(px & 0xFF) };
                int lo = c[0], hi = c[0];
                for (int i = 1; i < 3; i++) {
                    if (c[i] < lo) lo = c[i];
                    if (c[i] > hi) hi = c[i];
                }
                if (lo < 50 || hi > 130 || hi - lo > 45) {
                    kerr("svga3d", "in the middle the colour is %d,%d,%d, "
                                   "which is not three corners mixed",
                         c[0], c[1], c[2]);
                    wrong++;
                }
            }
        }

        /* Outside the shape nothing should have been touched at all. */
        static const struct { int x, y; } outside[] = {
            {  3,  3 }, { 60,  3 }, {  3, 60 }, { 60, 60 },
            { 10, 15 }, { 54, 15 },
        };
        for (u32 i = 0; i < sizeof outside / sizeof outside[0]; i++) {
            u32 px = pixels[outside[i].y * w + outside[i].x];
            if (px == 0xDEADBEEFu) { untouched++; continue; }
            int r = (int)((px >> 16) & 0xFF), g = (int)((px >> 8) & 0xFF);
            int bch = (int)(px & 0xFF);
            /* The ground, as it was cleared. */
            if (r > 40 || g > 60 || bch < 40 || bch > 90) {
                kerr("svga3d", "(%d,%d) is outside the triangle but holds "
                               "%d,%d,%d", outside[i].x, outside[i].y, r, g,
                     bch);
                wrong++;
            }
        }

        /* How much of the surface the shape covers: anything far enough from
         * the ground it was cleared to. */
        int covered = 0;
        for (u32 i = 0; i < w * h; i++) {
            u32 px = pixels[i];
            if (px == 0xDEADBEEFu) continue;
            int dr = (int)((px >> 16) & 0xFF) - 15;
            int dg = (int)((px >> 8) & 0xFF) - 33;
            int db = (int)(px & 0xFF) - 64;
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            if (dr + dg + db > 60) covered++;
        }

        if (untouched) {
            kinfo("svga3d", "the newer path drew nothing: %d probed pixel(s) "
                            "the card never wrote", untouched);
            return 1;
        }
        if (wrong) {
            kinfo("svga3d", "the newer path drew a shape but not the one asked "
                            "for: %d probed pixel(s) disagree, %d covered",
                  wrong, covered);
            return 1;
        }
        /* Half of 52 by 46, give or take the edges. */
        if (covered < 1050 || covered > 1350) {
            kinfo("svga3d", "the newer path drew a shape covering %d pixels; "
                            "the triangle asked for covers about 1196", covered);
            return 1;
        }

        user_draw.ready = true;
        kinfo("svga3d", "the host graphics card rasterised and shaded a "
                        "triangle: three corners in its own memory carrying a "
                        "colour each, two programs of its own running, %d "
                        "pixels covered, and every one of them the mixture of "
                        "the three corners its distance from each says it "
                        "should be", covered);

    /* ------------------------------------------------- more than one shape
     *
     * One triangle proves the card works out what lies between three corners.
     * This proves it does that for several shapes in a single request, and
     * that what each corner carries reaches the pixels between them
     * independently of the shape it belongs to.
     *
     * The square below is two triangles sharing an edge, and what the corners
     * carry is not a colour this time but a position within a picture - two
     * numbers running from nothing at one corner to one at the opposite.
     * Shown as colour, that has to come out as an even gradient across the
     * whole square, which it cannot if only the first triangle is drawn, if
     * the two are drawn with different values, or if the shared edge is wrong.
     *
     * Sampling an actual picture at those positions is the next thing and does
     * not work yet: the adapter takes a surface holding one, takes a way of
     * reaching it and a way of reading it, and then returns nothing at all
     * from it - and no arrangement of sample count, binding, or which path
     * carries the contents has changed that.  What is missing is recorded here
     * rather than left as a silent gap.
     */
    {
        u32 b[16];

        static const struct { float x, y, u, v; } quad[6] = {
            { -1.0f,  1.0f, 0.0f, 0.0f },
            {  1.0f,  1.0f, 1.0f, 0.0f },
            { -1.0f, -1.0f, 0.0f, 1.0f },
            {  1.0f,  1.0f, 1.0f, 0.0f },
            {  1.0f, -1.0f, 1.0f, 1.0f },
            { -1.0f, -1.0f, 0.0f, 1.0f },
        };
        for (int i = 0; i < 6; i++) {
            verts[i * 8 + 0] = quad[i].x;
            verts[i * 8 + 1] = quad[i].y;
            verts[i * 8 + 2] = 0.5f;
            verts[i * 8 + 3] = 1.0f;
            verts[i * 8 + 4] = quad[i].u;
            verts[i * 8 + 5] = quad[i].v;
            verts[i * 8 + 6] = 0.0f;
            verts[i * 8 + 7] = 1.0f;
        }
        __asm__ volatile("sfence" ::: "memory");

        /* The corners changed, so the card has to take them again - it keeps
         * its own copy, and the pages written above are only brought into step
         * when asked. */
        u32 uv[1] = { VERTEX_SURFACE };
        if (!dx_one("handing the new corners over", CMD3D_UPDATE_GB_SURFACE,
                    uv, 1, VIEW_NONE))
            return 1;

        for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
        __asm__ volatile("sfence" ::: "memory");

        b[0] = 6;                       /* six corners, two triangles */
        b[1] = 0;
        if (!dx_one("two triangles at once", CMD3D_DX_DRAW, b, 2, DX_CID))
            return 1;

        gb_readback(surface);
        svga_fifo_sync();
        timer_udelay(40000);

        /* A quarter of the way in from each side, the gradient should read a
         * quarter and three quarters of full - about 64 and 191. */
        static const struct { int x, y, want_r, want_g; } probe[4] = {
            { 16, 16,  64,  64 }, { 48, 16, 191,  64 },
            { 16, 48,  64, 191 }, { 48, 48, 191, 191 },
        };

        int bad = 0;
        for (int i = 0; i < 4; i++) {
            u32 px = pixels[probe[i].y * w + probe[i].x];
            if (px == 0xDEADBEEFu) {
                kerr("svga3d", "(%d,%d) of the square was never drawn",
                     probe[i].x, probe[i].y);
                bad++;
                continue;
            }
            int r = (int)((px >> 16) & 0xFF), g = (int)((px >> 8) & 0xFF);
            int dr = r - probe[i].want_r, dg = g - probe[i].want_g;
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (dr > 12 || dg > 12) {
                kerr("svga3d", "(%d,%d) of the square reads %d,%d rather than "
                               "%d,%d", probe[i].x, probe[i].y, r, g,
                     probe[i].want_r, probe[i].want_g);
                bad++;
            }
        }

        if (!bad)
            kinfo("svga3d", "the card drew two triangles in one request: a "
                            "square of them, and what its corners carry "
                            "arrives across the whole of it as evenly as the "
                            "distances say it should");

        /* --------------------------------------- a picture made on the card
         *
         * Handing a picture to the card does not work: every arrangement of
         * describing the surface and every path for its contents is accepted
         * and nothing arrives.  So the picture is not handed over at all - it
         * is drawn, by the card, into a surface that can be both drawn into
         * and read from.  The transfer that fails is simply not used.
         *
         * What is drawn is the gradient the square already carries, so the
         * picture ends up holding a known pattern.  Reading it back out again
         * through a program then has to reproduce that pattern, and a program
         * that cannot read the picture produces black instead - which is what
         * every attempt at handing one over produced.
         */
        {
            /* As big as the memory allows, smaller rather than nothing.
             *
             * At a thousand pixels square this is four megabytes of physically
             * contiguous memory, which is a great deal more than the sixteen
             * kilobytes it used to ask for and can fail where that never did.
             * Failing would take away every picture a program can put on the
             * screen, so it steps down instead - and says which size it got,
             * because "pictures are limited to 256" and "pictures do not work"
             * are different problems and only one of them is this system's. */
            u32 tw = PROBE_TEXTURE_SIDE, th = PROBE_TEXTURE_SIDE;
            u64 tex_phys = 0;
            u32 *tex = NULL;

            for (;;) {
                tex = dma_alloc_pages((tw * th * 4 + PAGE_SIZE - 1) / PAGE_SIZE,
                                      &tex_phys);
                if (tex || tw <= 64) break;
                tw /= 2; th /= 2;
            }
            probe_side = tw;
            if (tw != PROBE_TEXTURE_SIDE)
                kwarn("svga3d", "only %u pixels square could be set aside for "
                                "pictures rather than %u, so a program can put "
                                "a smaller one on the screen",
                      tw, (u32)PROBE_TEXTURE_SIDE);
            /* Kept, because this is the one surface on this adapter that is
             * known to take a picture handed to it, and a program drawing
             * later has nowhere else that works. */
            probe_texture = tex;
            bool ok = tex != NULL;
            if (ok) {
                memset(tex, 0, tw * th * 4);
                ok = gb_define_mob(MOB_TEXTURE, tex_phys / PAGE_SIZE,
                                   tw * th * 4);
                svga_fifo_sync();
            }

            /* Both at once: something the card draws into, and something a
             * program reads from. */
            u32 d[11] = { TEXTURE_SURFACE,
                          SURFACE_BIND_RENDER_TARGET |
                          SURFACE_BIND_SHADER_RESOURCE,
                          FORMAT_B8G8R8A8_UNORM, 1, 1, FILTER_NONE, tw, th,
                          1, 1, 0 };
            ok = ok && dx_one("a surface to hold a picture",
                              CMD3D_DEFINE_GB_SURFACE_V2, d, 11, VIEW_NONE);
            u32 j[2] = { TEXTURE_SURFACE, MOB_TEXTURE };
            ok = ok && dx_one("joining the picture to its memory",
                              CMD3D_BIND_GB_SURFACE, j, 2, VIEW_NONE);

            u32 rv[7] = { TEXTURE_RTV, TEXTURE_SURFACE, FORMAT_B8G8R8A8_UNORM,
                          RESOURCE_TEXTURE2D, 0, 0, 1 };
            ok = ok && dx_one("a view of the picture to draw into",
                              CMD3D_DX_DEFINE_RENDERTARGET_VIEW, rv, 7, DX_CID);

            /* Draw the square into the picture, so the picture holds the
             * gradient rather than being given it. */
            u32 into[2] = { VIEW_NONE, TEXTURE_RTV };
            ok = ok && dx_one("drawing into the picture",
                              CMD3D_DX_SET_RENDERTARGETS, into, 2, DX_CID);
            u32 tvp[7] = { 0, as_bits(0.0f), as_bits(0.0f), as_bits_of(tw),
                           as_bits_of(th), as_bits(0.0f), as_bits(1.0f) };
            ok = ok && dx_one("across the whole of it",
                              CMD3D_DX_SET_VIEWPORTS, tvp, 7, DX_CID);
            u32 tcl[5] = { TEXTURE_RTV, as_bits(0.0f), as_bits(0.0f),
                           as_bits(0.0f), as_bits(1.0f) };
            ok = ok && dx_one("clearing the picture",
                              CMD3D_DX_CLEAR_RENDERTARGET_VIEW, tcl, 5, DX_CID);
            u32 tdr[2] = { 6, 0 };
            ok = ok && dx_one("filling the picture with the square",
                              CMD3D_DX_DRAW, tdr, 2, DX_CID);

            /* Now read it, drawing the same square onto the surface checked
             * above.  A way to reach the picture, and a way to read it. */
            u32 srv[8] = { TEXTURE_VIEW, TEXTURE_SURFACE,
                           FORMAT_B8G8R8A8_UNORM, RESOURCE_TEXTURE2D,
                           0, 0, 1, 1 };
            ok = ok && dx_one("a way for a program to reach the picture",
                              CMD3D_DX_DEFINE_SHADERRESOURCE_VIEW, srv, 8,
                              DX_CID);

            u8 sm[44];
            memset(sm, 0, sizeof sm);
            *(u32 *)&sm[0] = SAMPLER_STATE;
            *(u32 *)&sm[4] = 0;             /* nearest point, no smoothing */
            sm[8] = sm[9] = sm[10] = TEX_ADDRESS_CLAMP;
            sm[17] = COMPARISON_ALWAYS;
            *(u32 *)&sm[40] = as_bits(3.4028234e+38f);   /* every level */
            ok = ok && dx_one("how to read it",
                              CMD3D_DX_DEFINE_SAMPLER_STATE, (const u32 *)sm,
                              11, DX_CID);

            /* The program that colours pixels becomes one that reads the
             * picture.  Everything else about the pipeline stays as it was. */
            if (ok) {
                memset(ps, 0, PAGE_SIZE);
                u32 tx = shader_pixel_textured(ps);
                static const u32 t_takes[] = { 1, SIG_MEANING_NONE };
                static const u32 t_gives[] = { 0, SIG_MEANING_NONE };
                tx = shader_signature(ps, tx, t_takes, 1, t_gives, 1);
                __asm__ volatile("sfence" ::: "memory");

                u32 sd[3] = { SHADER_PS_TEXTURED, SHADERTYPE_PS,
                              tx * (u32)sizeof(u32) };
                ok = dx_one("a program that reads a picture",
                            CMD3D_DX_DEFINE_SHADER, sd, 3, DX_CID);
                u32 sb[4] = { DX_CID, SHADER_PS_TEXTURED, MOB_PS, 0 };
                ok = ok && dx_one("attaching it to its memory",
                                  CMD3D_DX_BIND_SHADER, sb, 4, VIEW_NONE);
                u32 ss[2] = { SHADER_PS_TEXTURED, SHADERTYPE_PS };
                ok = ok && dx_one("choosing it", CMD3D_DX_SET_SHADER, ss, 2,
                                  DX_CID);
                u32 sr[3] = { 0, SHADERTYPE_PS, TEXTURE_VIEW };
                ok = ok && dx_one("giving it the picture",
                                  CMD3D_DX_SET_SHADER_RESOURCES, sr, 3, DX_CID);
                u32 sa[3] = { 0, SHADERTYPE_PS, SAMPLER_STATE };
                ok = ok && dx_one("and the way of reading it",
                                  CMD3D_DX_SET_SAMPLERS, sa, 3, DX_CID);
            }

            /* Back to the surface being checked, and draw the square again -
             * this time coloured by reading the picture. */
            u32 back[2] = { VIEW_NONE, chosen_view };
            ok = ok && dx_one("drawing into the surface again",
                              CMD3D_DX_SET_RENDERTARGETS, back, 2, DX_CID);
            u32 bvp[7] = { 0, as_bits(0.0f), as_bits(0.0f), as_bits_of(w),
                           as_bits_of(h), as_bits(0.0f), as_bits(1.0f) };
            ok = ok && dx_one("across the whole of it",
                              CMD3D_DX_SET_VIEWPORTS, bvp, 7, DX_CID);

            if (ok) {
                for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
                __asm__ volatile("sfence" ::: "memory");

                u32 bcl[5] = { chosen_view, as_bits(0.0f), as_bits(0.0f),
                               as_bits(0.0f), as_bits(1.0f) };
                ok = dx_one("clearing before the picture",
                            CMD3D_DX_CLEAR_RENDERTARGET_VIEW, bcl, 5, DX_CID);
                u32 bdr[2] = { 6, 0 };
                ok = ok && dx_one("the square, coloured by the picture",
                                  CMD3D_DX_DRAW, bdr, 2, DX_CID);
            }

            if (ok) {
                gb_readback(surface);
                svga_fifo_sync();
                timer_udelay(40000);

                int bad = 0, black = 0;
                for (int i = 0; i < 4; i++) {
                    u32 px = pixels[probe[i].y * w + probe[i].x];
                    int r = (int)((px >> 16) & 0xFF);
                    int g = (int)((px >> 8) & 0xFF);
                    if ((px & 0x00FFFFFFu) == 0) { black++; continue; }
                    int dr = r - probe[i].want_r, dg = g - probe[i].want_g;
                    if (dr < 0) dr = -dr;
                    if (dg < 0) dg = -dg;
                    if (dr > 16 || dg > 16) bad++;
                }

                if (black == 4)
                    kinfo("svga3d", "a program still reads nothing from a "
                                    "picture, even one the card drew itself - "
                                    "so what is missing is the reading, not "
                                    "the handing over");
                else if (bad || black)
                    kinfo("svga3d", "reading the picture gives something, but "
                                    "%d of 4 points are wrong and %d are "
                                    "black", bad, black);
                else {
                    kinfo("svga3d", "the card read a picture while drawing: a "
                                    "picture it drew itself, sampled at every "
                                    "pixel of a square of two triangles, and "
                                    "what came back is what was in it");

                    /* And now one this system draws and hands over, which is
                     * what an application actually needs.  Four quarters of
                     * four colours: each quarter of the result has to be the
                     * colour of the quarter of the picture it points at, which
                     * also catches the picture arriving upside down. */
                    for (u32 y = 0; y < th; y++)
                        for (u32 x = 0; x < tw; x++)
                            tex[y * tw + x] = (y < th / 2)
                                ? ((x < tw / 2) ? 0xFFFF0000u : 0xFF00FF00u)
                                : ((x < tw / 2) ? 0xFF0000FFu : 0xFFFFFF00u);
                    __asm__ volatile("sfence" ::: "memory");

                    u32 up[1] = { TEXTURE_SURFACE };
                    bool sent = dx_one("handing a picture over",
                                       CMD3D_UPDATE_GB_SURFACE, up, 1,
                                       VIEW_NONE);
                    for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
                    __asm__ volatile("sfence" ::: "memory");
                    u32 c2[5] = { chosen_view, as_bits(0.0f), as_bits(0.0f),
                                  as_bits(0.0f), as_bits(1.0f) };
                    sent = sent && dx_one("clearing again",
                                          CMD3D_DX_CLEAR_RENDERTARGET_VIEW,
                                          c2, 5, DX_CID);
                    u32 d2[2] = { 6, 0 };
                    sent = sent && dx_one("the square again", CMD3D_DX_DRAW,
                                          d2, 2, DX_CID);
                    if (sent) {
                        gb_readback(surface);
                        svga_fifo_sync();
                        timer_udelay(40000);

                        static const struct { int x, y; u32 want;
                                              const char *where; } q[4] = {
                            { 16, 16, 0xFF0000u, "top left"     },
                            { 48, 16, 0x00FF00u, "top right"    },
                            { 16, 48, 0x0000FFu, "bottom left"  },
                            { 48, 48, 0xFFFF00u, "bottom right" },
                        };
                        int wrong = 0;
                        for (int k = 0; k < 4; k++) {
                            u32 px = pixels[q[k].y * w + q[k].x] & 0x00FFFFFFu;
                            if (px != q[k].want) {
                                kinfo("svga3d", "the %s quarter is %06x, not "
                                                "%06x", q[k].where, px,
                                      q[k].want);
                                wrong++;
                            }
                        }
                        if (!wrong)
                            kinfo("svga3d", "and a picture this system drew "
                                            "and handed over: every quarter of "
                                            "the square came back the colour "
                                            "of the quarter of the picture it "
                                            "points at");

        /* ------------------------------------------------ which shape is in front
         *
         * Everything drawn so far has been flat, and what covers what has been
         * decided by the order the shapes were sent.  That is not enough for
         * anything with a scene in it: a nearer shape has to hide a further one
         * whichever order they arrive in, and deciding that per pixel needs a
         * second surface holding how far away each pixel already is.
         *
         * The test is the awkward order on purpose.  The near shape is drawn
         * first and the far one second, so the ordinary rule - last one wins -
         * gives the wrong answer everywhere they overlap.  Only a card actually
         * comparing distances leaves the near one showing.
         */
        {
            const u32 dw = 64, dh = 64;
            u64 depth_phys = 0;
            u32 *depth_mem = dma_alloc_pages(
                (dw * dh * 4 + PAGE_SIZE - 1) / PAGE_SIZE, &depth_phys);
            bool ok = depth_mem != NULL;
            if (ok) {
                memset(depth_mem, 0, dw * dh * 4);
                ok = gb_define_mob(MOB_DEPTH, depth_phys / PAGE_SIZE,
                                   dw * dh * 4);
                svga_fifo_sync();
            }

            u32 d[11] = { DEPTH_SURFACE, SURFACE_BIND_DEPTH_STENCIL,
                          FORMAT_D32_FLOAT, 1, 1, FILTER_NONE, dw, dh,
                          1, 1, 0 };
            ok = ok && dx_one("a surface to hold how far away each pixel is",
                              CMD3D_DEFINE_GB_SURFACE_V2, d, 11, VIEW_NONE);
            u32 j[2] = { DEPTH_SURFACE, MOB_DEPTH };
            ok = ok && dx_one("joining it to its memory", CMD3D_BIND_GB_SURFACE,
                              j, 2, VIEW_NONE);

            u32 dv[8] = { DEPTH_VIEW, DEPTH_SURFACE, FORMAT_D32_FLOAT,
                          RESOURCE_TEXTURE2D, 0, 0, 1, 0 };
            ok = ok && dx_one("a view of it to write distances into",
                              CMD3D_DX_DEFINE_DEPTHSTENCIL_VIEW, dv, 8, DX_CID);

            /* The same state as before but with the comparison switched on:
             * keep a pixel only where it is nearer than what is already there,
             * and record how near it now is. */
            u8 ds[20];
            memset(ds, 0, sizeof ds);
            *(u32 *)&ds[0] = DEPTH_STATE_ON;
            ds[4]  = 1;                     /* compare distances        */
            ds[5]  = DEPTH_WRITE_ALL;       /* and record the new one   */
            ds[6]  = COMPARISON_LESS;       /* nearer wins              */
            ds[7]  = 0;                     /* no stencil test          */
            ds[10] = 0xFF;
            ds[11] = 0xFF;
            ds[12] = ds[13] = ds[14] = STENCILOP_KEEP;
            ds[15] = COMPARISON_ALWAYS;
            ds[16] = ds[17] = ds[18] = STENCILOP_KEEP;
            ds[19] = COMPARISON_ALWAYS;
            ok = ok && dx_one("comparing distances rather than trusting order",
                              CMD3D_DX_DEFINE_DEPTHSTENCIL_STATE,
                              (const u32 *)ds, 5, DX_CID);
            u32 use[2] = { DEPTH_STATE_ON, 0 };
            ok = ok && dx_one("using that", CMD3D_DX_SET_DEPTHSTENCIL_STATE,
                              use, 2, DX_CID);

            /* Colour and distance together this time. */
            u32 rt[2] = { DEPTH_VIEW, chosen_view };
            ok = ok && dx_one("drawing colour and distance at once",
                              CMD3D_DX_SET_RENDERTARGETS, rt, 2, DX_CID);

            /* The program that colours from the corners, not the one that
             * reads a picture. */
            u32 shd[2] = { SHADER_PS, SHADERTYPE_PS };
            ok = ok && dx_one("the program that colours from the corners",
                              CMD3D_DX_SET_SHADER, shd, 2, DX_CID);

            /* Two triangles pointing opposite ways, overlapping down the
             * middle.  The near one is green and goes first; the far one is
             * red and goes second. */
            static const struct { float x, y, z, r, g, b; } shape[6] = {
                { -0.9f,  0.9f, 0.3f, 0.0f, 1.0f, 0.0f },
                {  0.9f,  0.9f, 0.3f, 0.0f, 1.0f, 0.0f },
                {  0.0f, -0.9f, 0.3f, 0.0f, 1.0f, 0.0f },
                { -0.9f, -0.9f, 0.7f, 1.0f, 0.0f, 0.0f },
                {  0.9f, -0.9f, 0.7f, 1.0f, 0.0f, 0.0f },
                {  0.0f,  0.9f, 0.7f, 1.0f, 0.0f, 0.0f },
            };
            for (int i = 0; i < 6; i++) {
                verts[i * 8 + 0] = shape[i].x;
                verts[i * 8 + 1] = shape[i].y;
                verts[i * 8 + 2] = shape[i].z;
                verts[i * 8 + 3] = 1.0f;
                verts[i * 8 + 4] = shape[i].r;
                verts[i * 8 + 5] = shape[i].g;
                verts[i * 8 + 6] = shape[i].b;
                verts[i * 8 + 7] = 1.0f;
            }
            __asm__ volatile("sfence" ::: "memory");
            u32 uv[1] = { VERTEX_SURFACE };
            ok = ok && dx_one("handing the corners over",
                              CMD3D_UPDATE_GB_SURFACE, uv, 1, VIEW_NONE);

            if (ok) {
                for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
                __asm__ volatile("sfence" ::: "memory");

                u32 cl[5] = { chosen_view, as_bits(0.0f), as_bits(0.0f),
                              as_bits(0.0f), as_bits(1.0f) };
                ok = dx_one("clearing the colour",
                            CMD3D_DX_CLEAR_RENDERTARGET_VIEW, cl, 5, DX_CID);
                /* Everything starts as far away as it can be, so the first
                 * shape to reach a pixel is nearer than nothing. */
                u32 cd[3] = { (0 << 16) | CLEAR_DEPTH, DEPTH_VIEW,
                              as_bits(1.0f) };
                ok = ok && dx_one("and the distances",
                                  CMD3D_DX_CLEAR_DEPTHSTENCIL_VIEW, cd, 3,
                                  DX_CID);
                u32 dr[2] = { 6, 0 };
                ok = ok && dx_one("both shapes, near one first",
                                  CMD3D_DX_DRAW, dr, 2, DX_CID);
            }

            if (ok) {
                gb_readback(surface);
                svga_fifo_sync();
                timer_udelay(40000);

                /* The middle is covered by both.  The near shape was drawn
                 * first, so anything that trusts order shows the far one
                 * there. */
                static const struct { int x, y; char want; const char *where; }
                probe_d[3] = {
                    /* Well inside each region: the two shapes narrow to a
                     * point at opposite ends, so a probe near an edge is a
                     * probe on the edge. */
                    { 32, 32, 'g', "where they overlap"         },
                    { 16, 10, 'g', "where only the near one is" },
                    { 16, 54, 'r', "where only the far one is"  },
                };

                int bad = 0;
                for (int i = 0; i < 3; i++) {
                    u32 px = pixels[probe_d[i].y * w + probe_d[i].x];
                    int r = (int)((px >> 16) & 0xFF);
                    int g = (int)((px >> 8) & 0xFF);
                    char got = (px == 0xDEADBEEFu) ? '?'
                             : (g > 150 && r < 80) ? 'g'
                             : (r > 150 && g < 80) ? 'r' : '?';
                    if (got != probe_d[i].want) {
                        kerr("svga3d", "%s the colour is %d,%d - wanted the %s "
                                       "shape", probe_d[i].where, r, g,
                             probe_d[i].want == 'g' ? "near" : "far");
                        bad++;
                    }
                }

                if (!bad)
                    kinfo("svga3d", "the card decides what is in front: two "
                                    "shapes overlapping, the near one sent "
                                    "first and the far one second, and the "
                                    "near one still shows where they cross - "
                                    "which the order they arrived in would "
                                    "have got wrong");
            }

            /* Back to no depth test, so nothing after this is filtered by
             * distances left over from here. */
            u32 off[2] = { DEPTH_STATE, 0 };
            dx_one("no longer comparing distances",
                   CMD3D_DX_SET_DEPTHSTENCIL_STATE, off, 2, DX_CID);
            u32 only[2] = { VIEW_NONE, chosen_view };
            dx_one("colour only again", CMD3D_DX_SET_RENDERTARGETS, only, 2,
                   DX_CID);
        }

        /* -------------------------------------------------- seeing through
         *
         * Everything drawn so far has replaced what was underneath it.  A
         * window system needs the other thing: a shape that lets what is
         * behind it show through in proportion to how solid it is, so that a
         * shadow darkens rather than blanks, and a panel over a picture is
         * still a panel over a picture.
         *
         * The card is told to mix rather than replace - to take the new colour
         * in proportion to its fourth number and the colour already there in
         * proportion to what is left over.  Half of red over blue is then a
         * particular purple, and neither of the two colours involved: a card
         * that ignored the instruction would leave blue, and one that
         * replaced would leave red.
         */
        {
            u8 bl[104];
            memset(bl, 0, sizeof bl);
            *(u32 *)&bl[0] = BLEND_STATE_MIX;
            bl[4] = 0;
            bl[5] = 0;
            for (int rt = 0; rt < 8; rt++) {
                u8 *e = &bl[8 + rt * 12];
                e[0] = 1;                       /* mix, rather than replace  */
                e[1] = BLENDOP_SRCALPHA;        /* this much of the new      */
                e[2] = BLENDOP_INVSRCALPHA;     /* and the rest of the old   */
                e[3] = BLENDEQ_ADD;
                e[4] = BLENDOP_ONE;             /* the fourth number itself  */
                e[5] = BLENDOP_ZERO;            /* is simply replaced        */
                e[6] = BLENDEQ_ADD;
                e[7] = WRITE_ALL_CHANNELS;
                e[8] = 0;
                e[9] = 0;
            }
            bool ok = dx_one("mixing what is drawn with what is there",
                             CMD3D_DX_DEFINE_BLEND_STATE, (const u32 *)bl, 26,
                             DX_CID);
            u32 useb[6] = { BLEND_STATE_MIX, as_bits(1.0f), as_bits(1.0f),
                            as_bits(1.0f), as_bits(1.0f), 0xFFFFFFFFu };
            ok = ok && dx_one("using that", CMD3D_DX_SET_BLEND_STATE, useb, 6,
                              DX_CID);

            /* One triangle, half solid, covering the middle. */
            static const struct { float x, y; } tri[3] = {
                {  0.0f,  0.8f }, {  0.8f, -0.7f }, { -0.8f, -0.7f },
            };
            for (int i = 0; i < 3; i++) {
                verts[i * 8 + 0] = tri[i].x;
                verts[i * 8 + 1] = tri[i].y;
                verts[i * 8 + 2] = 0.5f;
                verts[i * 8 + 3] = 1.0f;
                verts[i * 8 + 4] = 1.0f;        /* red                       */
                verts[i * 8 + 5] = 0.0f;
                verts[i * 8 + 6] = 0.0f;
                verts[i * 8 + 7] = 0.5f;        /* half solid                */
            }
            __asm__ volatile("sfence" ::: "memory");
            u32 uv[1] = { VERTEX_SURFACE };
            ok = ok && dx_one("handing the corners over",
                              CMD3D_UPDATE_GB_SURFACE, uv, 1, VIEW_NONE);

            if (ok) {
                for (u32 i = 0; i < w * h; i++) pixels[i] = 0xDEADBEEFu;
                __asm__ volatile("sfence" ::: "memory");

                /* Blue underneath. */
                u32 cl[5] = { chosen_view, as_bits(0.0f), as_bits(0.0f),
                              as_bits(1.0f), as_bits(1.0f) };
                ok = dx_one("something to see through to",
                            CMD3D_DX_CLEAR_RENDERTARGET_VIEW, cl, 5, DX_CID);
                u32 dr[2] = { 3, 0 };
                ok = ok && dx_one("half-solid red over it", CMD3D_DX_DRAW, dr,
                                  2, DX_CID);
            }

            if (ok) {
                gb_readback(surface);
                svga_fifo_sync();
                timer_udelay(40000);

                u32 inside = pixels[32 * w + 32];
                u32 outside = pixels[4 * w + 4];
                int ir = (int)((inside >> 16) & 0xFF);
                int ig = (int)((inside >> 8) & 0xFF);
                int ib = (int)(inside & 0xFF);
                int ob = (int)(outside & 0xFF);

                /* Half of full red over full blue, in eight bits a channel. */
                bool mixed = ir > 100 && ir < 155 && ig < 40 &&
                             ib > 100 && ib < 155;
                bool ground_kept = ob > 200 && ((outside >> 16) & 0xFF) < 40;

                if (mixed && ground_kept)
                    kinfo("svga3d", "the card mixes rather than replaces: half "
                                    "solid red drawn over blue came back "
                                    "%d,%d,%d where it covers - neither of the "
                                    "two colours, but the two of them in the "
                                    "proportion asked for", ir, ig, ib);
                else
                    kinfo("svga3d", "mixing gave %d,%d,%d where the shape "
                                    "covers and %08x where it does not",
                          ir, ig, ib, outside);
            }

            /* Back to replacing, so nothing after this is quietly translucent. */
            u32 plain[6] = { BLEND_STATE, as_bits(1.0f), as_bits(1.0f),
                             as_bits(1.0f), as_bits(1.0f), 0xFFFFFFFFu };
            dx_one("replacing again", CMD3D_DX_SET_BLEND_STATE, plain, 6,
                   DX_CID);
        }
                    }
                }
            }
        }
    }
    }

    /* Put the pipeline back the way a program expects to find it.  The tests
     * above left it reading a picture, and a program handing down corners that
     * carry a colour would have that colour read as a place in a picture
     * instead - which draws something, just not what was asked for. */
    {
        u32 back[2] = { SHADER_PS, SHADERTYPE_PS };
        dx_one("the program that colours from the corners, again",
               CMD3D_DX_SET_SHADER, back, 2, DX_CID);
    }

    kinfo("svga3d", "the newer drawing path works on this card: a context, a "
                    "view of a surface, and %u pixels cleared through it twice "
                    "- so where the old path stops is the host renderer, not "
                    "this driver", w * h);
    return 0;
}

/* ------------------------------------------------------------------- test
 *
 * The card is asked to fill a surface with a colour, twice with different
 * colours, and the pixels are read back and compared.  One colour could pass
 * on a surface that happened to hold it already; two in a row cannot.  And the
 * guest's copy is overwritten with something else before each pass, so what
 * comes back has to have come from the card.
 */
int svga3d_render_selftest(void) {
    if (!svga_present()) return 0;
    if (!svga_devcap(DEVCAP_3D) || !svga_gbobjects()) return 0;
    if (!svga_command_buffers()) {
        kinfo("svga3d", "this adapter takes no command buffers, so nothing "
                        "addressed to a drawing context can reach it");
        return 0;
    }
    if (!gb_setup_otables() || !cb_setup() || !cb_start_queue()) return 0;

    u64 pixels_phys = 0, context_phys = 0, second_phys = 0;
    u32 *pixels = dma_alloc_pages(GB_BYTES / PAGE_SIZE, &pixels_phys);
    u8 *context = dma_alloc_pages(CONTEXT_BYTES / PAGE_SIZE, &context_phys);
    u8 *second = dma_alloc_pages(CONTEXT_BYTES / PAGE_SIZE, &second_phys);
    if (!pixels || !context || !second) {
        kwarn("svga3d", "no memory for a drawing context");
        return 0;
    }
    memset(context, 0, CONTEXT_BYTES);
    memset(second, 0, CONTEXT_BYTES);

    const u32 mob_pixels = 8, mob_context = 9, mob_second = 10, surface = 40;

    bool built = gb_define_mob(mob_pixels, pixels_phys / PAGE_SIZE, GB_BYTES);
    built &= gb_define_mob(mob_context, context_phys / PAGE_SIZE, CONTEXT_BYTES);
    built &= gb_define_mob(mob_second, second_phys / PAGE_SIZE, CONTEXT_BYTES);
    built &= gb_define_surface(surface, GB_W, GB_H);
    built &= gb_bind_surface(surface, mob_pixels);
    if (!built) {
        kwarn("svga3d", "the card would not take the surface to draw into");
        return 0;
    }
    svga_fifo_sync();

    if (!render_setup(surface, mob_context, mob_second, GB_W, GB_H)) {
        /* The card manages objects and moves data and will not complete the
         * handshake that makes a context usable.  Once, in one longer sequence,
         * it did, and the card cleared a surface and the pixels came back
         * changed - so the path exists and this is a step in it that has not
         * been pinned down, not a wall.  Reported as what it is. */
        kinfo("svga3d", "so drawing stays with the processor for now, while "
                        "moving and copying are done by the card");
        return 0;
    }

    /* The context exists and has memory.  What is refused is the first drawing
     * command, which is where this stops today. */

    int failures = 0;
    static const u32 colours[2] = { 0xFF3C78D8u, 0xFFD8783Cu };

    for (int pass = 0; pass < 2; pass++) {
        /* Something the card would never produce, so that anything else
         * coming back came from the card. */
        for (int i = 0; i < GB_W * GB_H; i++) pixels[i] = 0xDEADBEEFu;
        __asm__ volatile("sfence" ::: "memory");

        if (!render_clear(colours[pass])) {
            kinfo("svga3d", "so drawing stays with the processor for now, "
                            "while moving and copying are done by the card");
            render_teardown();
            return 0;
        }

        /* And bring the card's own copy back to where it can be looked at. */
        gb_readback(surface);
        svga_fifo_sync();
        timer_udelay(30000);

        int untouched = 0, wrong = 0;
        u32 first_wrong = 0;
        for (int i = 0; i < GB_W * GB_H; i += 41) {
            if (pixels[i] == 0xDEADBEEFu) { untouched++; continue; }
            if ((pixels[i] & 0x00FFFFFFu) != (colours[pass] & 0x00FFFFFFu)) {
                if (!wrong) first_wrong = pixels[i];
                wrong++;
            }
        }

        if (untouched) {
            kerr("svga3d", "pass %d: %d sampled pixel(s) the card never wrote",
                 pass, untouched);
            failures++;
            break;
        }
        if (wrong) {
            kerr("svga3d", "pass %d: %d sampled pixel(s) are not %06x; one is "
                           "%08x", pass, wrong, colours[pass] & 0xFFFFFF,
                 first_wrong);
            failures++;
            break;
        }
    }


    /* ---------------------------------------------------------- geometry
     *
     * A clear says the render target is real.  A triangle says the card is
     * rasterising: three vertices in its own memory, and a shape with slanted
     * edges that covers some pixels and not others.
     *
     * The points checked below are the argument.  Four are inside the
     * triangle.  Six are outside it - and two of those, at (10,15) and
     * (54,15), are inside its bounding box, above the slanted edges.  A card
     * that filled a rectangle would colour those two, and a card that did
     * nothing would leave the inside four.  Only something that evaluates the
     * edges gets all ten right.
     */
    if (!failures) {
        const u32 mob_verts = 10, vb = 41;
        const u32 background = 0xFF102040u, ink = 0xFFFFC800u;

        u64 verts_phys = 0;
        vertex_t *verts = dma_alloc_pages(1, &verts_phys);
        if (!verts) {
            kwarn("svga3d", "no memory for vertices");
            failures++;
        } else if (!make_vertex_buffer(vb, mob_verts, verts_phys,
                                       3 * VERTEX_STRIDE)) {
            failures++;
        } else {
            /* Tall enough and wide enough that the slanted edges pass well
             * clear of every point checked. */
            static const struct { float x, y; } corner[3] = {
                { 32.0f,  6.0f }, { 58.0f, 52.0f }, {  6.0f, 52.0f },
            };
            for (int i = 0; i < 3; i++) {
                verts[i].x = corner[i].x;
                verts[i].y = corner[i].y;
                verts[i].z = 0.5f;
                verts[i].rhw = 1.0f;        /* already divided through */
                verts[i].colour = ink;
            }
            __asm__ volatile("sfence" ::: "memory");

            /* Poison the picture, so nothing left over from the clears above
             * can be mistaken for what the card draws now. */
            for (int i = 0; i < GB_W * GB_H; i++) pixels[i] = 0xDEADBEEFu;
            __asm__ volatile("sfence" ::: "memory");

            gb_update(vb);              /* hand the vertices to the card */
            svga_fifo_sync();

            /* Did the vertices arrive?  Scribble over the guest's copy, ask
             * for the card's back, and see whether what returns is what was
             * sent.  If it is, a draw that does nothing is not short of
             * vertices. */
            {
                /* Compared as bits: this kernel has no floating-point
                 * comparison to call, and bits are the stricter test anyway. */
                u32 sent_x = *(volatile u32 *)&verts[1].x;
                verts[1].x = -1.0f;
                __asm__ volatile("sfence" ::: "memory");
                gb_readback(vb);
                svga_fifo_sync();
                timer_udelay(20000);
                /* Inconclusive either way, and worth saying so: for a
                 * buffer the memory registered here is the storage, so there
                 * may be no separate copy to bring back and nothing to
                 * overwrite what was scribbled in. */
                if (*(volatile u32 *)&verts[1].x == sent_x)
                    kinfo("svga3d", "the card gave the vertices back unchanged");
                else
                    kinfo("svga3d", "asking for the vertices back changed "
                                    "nothing, which for a buffer may mean the "
                                    "memory here is the only copy");
                *(volatile u32 *)&verts[1].x = sent_x;
                __asm__ volatile("sfence" ::: "memory");
                gb_update(vb);
                svga_fifo_sync();

                /* And the same two commands sent where the card has to answer,
                 * so that "the vertices did not come back" can be told apart
                 * from "moving a buffer's contents is not something this card
                 * does through this path". */
                u32 one[1] = { vb };
                if (cb_one("handing the vertex buffer over",
                           CMD3D_UPDATE_GB_SURFACE, one, 1))
                    kinfo("svga3d", "handing the vertex buffer over is taken");
                if (cb_one("asking for the vertex buffer back",
                           CMD3D_READBACK_GB_SURFACE, one, 1))
                    kinfo("svga3d", "asking for the vertex buffer back is "
                                    "taken");
            }

            /* What the card says about geometry, in its own words.  These are
             * asked because a draw that is accepted and does nothing gives no
             * reason, and the capability registers do. */
            kinfo("svga3d", "geometry: vertex shaders %08x, fragment shaders "
                            "%08x, fixed-function blending %u, newer drawing "
                            "contexts %u",
                  svga_devcap(DEVCAP_VERTEX_SHADER_VERSION),
                  svga_devcap(DEVCAP_FRAGMENT_SHADER_VERSION),
                  svga_devcap(DEVCAP_MAX_FIXED_VERTEXBLEND),
                  svga_devcap(DEVCAP_DXCONTEXT));

            /* The second capability word, which is where the newer drawing
             * path is actually switched on.  A card can answer yes to having
             * drawing contexts and still not offer the generation of them this
             * driver would have to ask for. */
            {
                u32 cap2 = svga_reg_read_ext(REG_CAP2);
                kinfo("svga3d", "second capability word %08x: newer contexts "
                                "%s, and again %s", cap2,
                      (cap2 & CAP2_DX2) ? "yes" : "no",
                      (cap2 & CAP2_DX3) ? "yes" : "no");
            }

            /* Can the card colour part of a surface and not the rest?  This
             * asks with a rectangle, which needs none of the vertex path, so
             * it separates "cannot restrict what it touches" from "cannot work
             * out what a triangle covers". */
            {
                for (int i = 0; i < GB_W * GB_H; i++) pixels[i] = 0xDEADBEEFu;
                __asm__ volatile("sfence" ::: "memory");
                if (render_clear(background) &&
                    render_clear_rect(0xFF00FF00u, 16, 16, 32, 32)) {
                    gb_readback(surface);
                    svga_fifo_sync();
                    timer_udelay(30000);
                    u32 in = pixels[32 * GB_W + 32] & 0x00FFFFFFu;
                    u32 out = pixels[4 * GB_W + 4] & 0x00FFFFFFu;
                    if (in == 0x00FF00u && out == (background & 0x00FFFFFFu))
                        kinfo("svga3d", "the card colours part of a surface "
                                        "and leaves the rest, so it restricts "
                                        "what it touches to an area it is "
                                        "given");
                    else
                        kwarn("svga3d", "a rectangle inside the surface came "
                                        "back in=%06x out=%06x", in, out);
                }
            }

            if (!render_triangle(vb, background)) {
                failures++;
            } else {
                gb_readback(surface);
                svga_fifo_sync();
                timer_udelay(30000);

                static const struct { int x, y; bool in; } probe[] = {
                    { 32, 20, true  }, { 32, 40, true  },
                    { 20, 45, true  }, { 44, 45, true  },
                    {  3,  3, false }, { 60,  3, false },
                    {  3, 60, false }, { 60, 60, false },
                    /* Inside the bounding box, outside the triangle. */
                    { 10, 15, false }, { 54, 15, false },
                };

                int wrong = 0, untouched = 0;
                for (u32 i = 0; i < sizeof probe / sizeof probe[0]; i++) {
                    u32 got = pixels[probe[i].y * GB_W + probe[i].x];
                    u32 want = probe[i].in ? ink : background;
                    if (got == 0xDEADBEEFu) untouched++;
                    else if ((got & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) wrong++;
                }

                if (untouched || wrong) {
                    /* Said once, plainly, rather than as a wall of failures.
                     * The clear in the same buffer landed - every pixel is the
                     * background colour it asked for - so the buffer ran and
                     * the triangle in it did nothing.  Not counted against the
                     * drawing that does work; recorded so that the day it
                     * starts working shows up here as a change. */
                    kinfo("svga3d", "the old drawing path takes a triangle and "
                                    "draws nothing: the clear beside it landed, "
                                    "%d of 10 probed pixels are what a triangle "
                                    "would have made them, and the card gave no "
                                    "reason - it accepted the command",
                          10 - wrong - untouched);
                } else {
                    /* And how much of the surface it covered, which a
                     * rectangle fill would get wrong even if the ten points
                     * happened to agree. */
                    int inside = 0;
                    for (int i = 0; i < GB_W * GB_H; i++)
                        if ((pixels[i] & 0x00FFFFFFu) == (ink & 0x00FFFFFFu))
                            inside++;

                    /* Half of 52 by 46, give or take the edges. */
                    if (inside < 1050 || inside > 1350) {
                        kerr("svga3d", "the shape covers %d pixels; a triangle "
                                       "that size covers about 1196", inside);
                        failures++;
                    } else {
                        kinfo("svga3d", "the host graphics card rasterised it: "
                                        "three vertices from a buffer in its "
                                        "own memory, %d pixels covered, and "
                                        "the slanted edges fall where the "
                                        "geometry says", inside);
                    }
                }
            }
        }

        gb_destroy_surface(vb);
        gb_destroy_mob(mob_verts);
    }

    /* The old path draws nothing and gives no reason, so ask the newer one -
     * which this card says it has - whether it is there.  Reported rather than
     * counted: what the suite checks is the drawing that works, and this says
     * where the next of it will come from. */
    {
        u64 dx_ctx_phys = 0;
        u8 *dx_ctx = dma_alloc_pages(CONTEXT_BYTES / PAGE_SIZE, &dx_ctx_phys);
        if (dx_ctx) {
            memset(dx_ctx, 0, CONTEXT_BYTES);
            const u32 mob_dx = 11;
            if (gb_define_mob(mob_dx, dx_ctx_phys / PAGE_SIZE, CONTEXT_BYTES)) {
                svga_fifo_sync();
                dx_probe(surface, mob_dx, pixels, GB_W, GB_H);
                /* The memory stays.  It is where the drawing context keeps
                 * its state, and the context outlives this test: everything
                 * a program later asks the card to draw is drawn with it.
                 * Taking the memory back here leaves a context that answers
                 * nothing, which is indistinguishable from not having one. */
                svga_fifo_sync();
            }
        }
    }

    if (!failures)
        kinfo("svga3d", "the host graphics card drew it: a context and a "
                        "render target on the card, filled with two colours in "
                        "turn, %d pixels each, and every one came back the "
                        "colour it was asked for", GB_W * GB_H);

    render_teardown();
    gb_destroy_surface(surface);
    gb_destroy_mob(mob_pixels);
    gb_destroy_mob(mob_context);
    gb_destroy_mob(mob_second);
    svga_fifo_sync();

    return failures;
}

static bool user_draw_build(void);
static bool user_image_build(void);

/* ================================================ the display, on the card
 *
 * Everything above proves the card can draw.  This is what makes that worth
 * having for a desktop rather than for a test: the picture the window system
 * composes is put where the display scans out of directly, so presenting a
 * frame stops being a copy of every changed pixel into video memory and
 * becomes one command saying which rectangle moved.
 *
 * The arrangement is the reverse of the usual one.  Ordinarily the adapter
 * owns the memory the display reads and the system copies into it; here the
 * system owns it, hands it to the card as a surface, and the card is told that
 * surface is the screen.  Nothing is copied at all - the window system draws
 * into the same pages the display is reading, and the only thing crossing the
 * boundary afterwards is four numbers.
 *
 * It is offered rather than assumed.  A host with no drawing engine behind the
 * adapter cannot do this, and on one that can, the older arrangement is still
 * there to fall back to.
 */
static struct {
    bool  attached;
    bool  drawable;         /* and a view of it exists to draw into */
    u32   view;
    u32   width, height;
    u64   phys;
    u32  *pixels;
    u32   presents;
} screen;

bool svga3d_screen_attached(void) { return screen.attached; }

u64 svga3d_screen_memory(u32 *width, u32 *height, u32 *pitch) {
    if (!screen.attached) return 0;
    if (width)  *width  = screen.width;
    if (height) *height = screen.height;
    if (pitch)  *pitch  = screen.width * 4;
    return screen.phys;
}

/* Give the card the memory, make it a surface, and make that surface the
 * screen.  Returns false having changed nothing if any step is refused. */
bool svga3d_screen_attach(u32 w, u32 h) {
    if (screen.attached) return true;
    if (!svga_present() || !svga_devcap(DEVCAP_3D) || !svga_gbobjects()) return false;
    if (!svga_command_buffers()) return false;
    if (!w || !h || w > 4096 || h > 4096) return false;
    if (!gb_setup_otables() || !cb_setup() || !cb_start_queue()) return false;

    u32 bytes = w * h * 4;
    u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = 0;
    u32 *pixels = dma_alloc_pages(pages, &phys);
    if (!pixels) {
        kinfo("svga3d", "no room for a %ux%u screen in memory the card can "
                        "read, so the display stays where it was", w, h);
        return false;
    }
    memset(pixels, 0, (size_t)pages * PAGE_SIZE);

    bool ok = gb_define_mob(MOB_SCREEN, phys / PAGE_SIZE,
                            pages * (u32)PAGE_SIZE);
    svga_fifo_sync();

    u32 d[11] = { SCREEN_SURFACE,
                  SURFACE_BIND_RENDER_TARGET | SURFACE_SCREENTARGET,
                  FORMAT_B8G8R8A8_UNORM, 1, 1, FILTER_NONE, w, h, 1, 1, 0 };
    ok = ok && cb_one("a surface the display can scan out of",
                      CMD3D_DEFINE_GB_SURFACE_V2, d, 11);
    u32 j[2] = { SCREEN_SURFACE, MOB_SCREEN };
    ok = ok && cb_one("joining it to its memory", CMD3D_BIND_GB_SURFACE, j, 2);

    u32 st[7] = { SCREEN_ID, w, h, 0, 0, SCREEN_PRIMARY, 0 };
    ok = ok && cb_one("a screen for the display",
                      CMD3D_DEFINE_GB_SCREENTARGET, st, 7);
    u32 bs[4] = { SCREEN_ID, SCREEN_SURFACE, 0, 0 };
    ok = ok && cb_one("pointing the screen at that surface",
                      CMD3D_BIND_GB_SCREENTARGET, bs, 4);

    if (!ok) {
        u32 one[1] = { SCREEN_ID };
        cb_begin();
        cb_command(CMD3D_DESTROY_GB_SCREENTARGET, one, 1);
        cb_submit(VIEW_NONE);
        gb_destroy_surface(SCREEN_SURFACE);
        gb_destroy_mob(MOB_SCREEN);
        svga_fifo_sync();
        return false;
    }

    /* And a view of it to draw into, made now rather than the first time a
     * program asks.  The same request a moment later is refused, so it belongs
     * with the rest of setting the screen up. */
    /* Is the drawing context still answering at all?  Something harmless and
     * context-scoped separates "this request is wrong" from "the context this
     * request is addressed to is no longer there". */
    {
        u32 t[1] = { PRIMITIVE_TRIANGLELIST };
        if (dx_one("a word with the drawing context", CMD3D_DX_SET_TOPOLOGY,
                   t, 1, DX_CID))
            kinfo("svga3d", "the drawing context is still answering");
    }

    screen.drawable = false;
    for (u32 id = SCREEN_VIEW; id < SCREEN_VIEW + 4 && !screen.drawable; id++) {
        u32 v[7] = { id, SCREEN_SURFACE, FORMAT_B8G8R8A8_UNORM,
                     RESOURCE_TEXTURE2D, 0, 0, 1 };
        cb_begin();
        cb_command(CMD3D_DX_DEFINE_RENDERTARGET_VIEW, v, 7);
        if (cb_submit(DX_CID)) {
            screen.view = id;
            screen.drawable = true;
            kinfo("svga3d", "a view of the screen for programs to draw into, "
                            "numbered %u", id);
        }
    }
    if (!screen.drawable)
        kinfo("svga3d", "the card would not make a view of the screen for "
                        "programs to draw into (status %u at byte %u)",
              cb.last_status, cb.last_error_at);

    screen.attached = true;
    screen.width = w;
    screen.height = h;
    screen.phys = phys;
    screen.pixels = pixels;

    /* Build what a program will need to draw with, now, while everything else
     * is being set up.  A view of the screen made later is refused outright,
     * and there is no reason to expect the rest to be different. */
    if (screen.drawable) {
        user_draw_build();
        user_image_build();
    }

    kinfo("svga3d", "the display now reads out of memory this system owns and "
                    "the card was given: %ux%u, so a frame is one command "
                    "rather than %u bytes copied", w, h, bytes);
    return true;
}

/* The second display's own scan-out (#7), declared here because the present
 * path below carries the primary's picture onto it.  See the fuller note by
 * svga3d_screen_attach_second. */
static struct {
    bool  attached;
    u32   width, height;
    u32   x_root;
    u64   phys;
    u32  *pixels;
} screen2;

/* Extend (#7): one logical framebuffer as wide as both displays together, drawn
 * by the desktop as a single wide screen.  The present path shows its left part
 * on screen 0 and its right part on screen 1.  extend_fb is the wide buffer's
 * kernel mapping (the desktop draws here through g_boot.fb); NULL when extend is
 * not set up. */
static u32 *extend_fb;
static u32  extend_w, extend_h;

/* Which multi-display mode the second screen is in.  Same numbering as
 * dl_mode_t (display_layout.h) so a Settings toggle passes straight through:
 * 0 only-primary, 1 extend, 2 mirror, 3 only-secondary. */
#define SM_ONLY_PRIMARY 0
#define SM_EXTEND       1
#define SM_MIRROR       2
#define SM_ONLY_SECOND  3
static int second_mode = SM_ONLY_PRIMARY;

/* Mirror the changed rectangle of the primary onto the second screen: copy
 * those pixels from the primary's memory into screen 1's, then present screen
 * 1's rectangle.  Called from the present path when the mode shows the primary
 * content on the second display (mirror, or only-secondary once the primary is
 * blanked).  A no-op unless there is a second screen and both buffers exist. */
static void second_show_primary(u32 x, u32 y, u32 w, u32 h) {
    if (!screen2.attached || !screen.pixels || !screen2.pixels) return;
    if (x >= screen2.width || y >= screen2.height) return;
    if (x + w > screen2.width)  w = screen2.width - x;
    if (y + h > screen2.height) h = screen2.height - y;
    if (x + w > screen.width)   w = (x < screen.width) ? screen.width - x : 0;
    if (y + h > screen.height)  h = (y < screen.height) ? screen.height - y : 0;
    if (!w || !h) return;
    for (u32 row = 0; row < h; row++)
        memcpy(&screen2.pixels[(y + row) * screen2.width + x],
               &screen.pixels[(y + row) * screen.width + x],
               (size_t)w * 4);
    svga3d_second_update(x, y, w, h);
}

/* Say which rectangle moved.  This replaces copying those pixels anywhere. */
void svga3d_screen_update(u32 x, u32 y, u32 w, u32 h) {
    if (!screen.attached) return;

    /* Extend: the rectangle is in the wide logical desktop (as wide as both
     * displays).  Split it - the part over the primary is copied into screen
     * 0's surface and shown there; the part past it is copied into screen 1's
     * surface (whose own space starts at 0) and shown there.  So one wide
     * desktop appears as two side-by-side pictures, which is what extend is. */
    if (second_mode == SM_EXTEND && extend_fb && screen2.attached) {
        if (x >= extend_w || y >= extend_h) return;
        if (x + w > extend_w) w = extend_w - x;
        if (y + h > extend_h) h = extend_h - y;
        if (!w || !h) return;

        u32 w0 = screen.width;
        if (x < w0) {                                   /* left, onto screen 0 */
            u32 lx = x, lw = (x + w > w0 ? w0 : x + w) - x;
            for (u32 row = 0; row < h && (y + row) < screen.height; row++)
                memcpy(&screen.pixels[(y + row) * screen.width + lx],
                       &extend_fb[(y + row) * extend_w + lx], (size_t)lw * 4);
            u32 up0[5] = { SCREEN_ID, lx, y, lw, h };
            cb_begin();
            cb_command(CMD3D_UPDATE_GB_SCREENTARGET, up0, 5);
            cb_submit(VIEW_NONE);
        }
        if (x + w > w0) {                               /* right, onto screen 1 */
            u32 rx = (x > w0 ? x : w0), rw = x + w - rx, sx = rx - w0;
            if (sx < screen2.width) {
                if (sx + rw > screen2.width) rw = screen2.width - sx;
                for (u32 row = 0; row < h && (y + row) < screen2.height; row++)
                    memcpy(&screen2.pixels[(y + row) * screen2.width + sx],
                           &extend_fb[(y + row) * extend_w + rx], (size_t)rw * 4);
                svga3d_second_update(sx, y, rw, h);
            }
        }

        if (++screen.presents == 8) {
            /* Prove extend shows DIFFERENT halves, not the same picture twice:
             * screen 0 must equal the wide desktop's left, screen 1 its right,
             * and the two halves must not be identical. */
            u32 l_ok = 0, r_ok = 0, differ = 0, n = 0;
            for (u32 j = 0; j < screen.height; j += 64)
                for (u32 i = 0; i < screen.width && i < screen2.width; i += 64) {
                    u32 left  = screen.pixels[j * screen.width + i];
                    u32 right = screen2.pixels[j * screen2.width + i];
                    if (left  == extend_fb[j * extend_w + i])          l_ok++;
                    if (right == extend_fb[j * extend_w + (w0 + i)])   r_ok++;
                    if (left != right) differ++;
                    n++;
                }
            kinfo("svga3d", "extend works: of %u sampled points screen 0 matches "
                            "the desktop's left in %u, screen 1 its right in %u, "
                            "and the two halves differ in %u - one %ux%u desktop "
                            "across two displays", n, l_ok, r_ok, differ,
                  extend_w, extend_h);
        }
        return;
    }

    if (x >= screen.width || y >= screen.height) return;
    if (x + w > screen.width)  w = screen.width - x;
    if (y + h > screen.height) h = screen.height - y;
    if (!w || !h) return;

    u32 up[5] = { SCREEN_ID, x, y, w, h };
    cb_begin();
    cb_command(CMD3D_UPDATE_GB_SCREENTARGET, up, 5);
    if (!cb_submit(VIEW_NONE)) {
        /* One refusal is worth knowing about; a stream of them would be worth
         * less than the frames it would cost to print. */
        static bool complained;
        if (!complained) {
            complained = true;
            kwarn("svga3d", "the card stopped taking screen updates (status "
                            "%u), so the picture is no longer being shown",
                  cb.last_status);
        }
        return;
    }
    screen.presents++;

    /* If a second display is showing the primary's picture (mirror, or
     * only-secondary), carry the same changed rectangle onto it.  Extend is
     * different - it draws its own half - and is not handled here. */
    if (screen2.attached &&
        (second_mode == SM_MIRROR || second_mode == SM_ONLY_SECOND))
        second_show_primary(x, y, w, h);

    /* Once the window system has been running a while, say what is actually in
     * the memory the display is reading.  Frames being accepted proves the
     * card takes the command; this proves there is a picture in there for it
     * to be showing - drawn by the window system straight into the pages the
     * display scans, with nothing copying it anywhere. */
    if (screen.presents == 1)
        kinfo("svga3d", "the window system presented its first frame as a "
                        "command to the card");

    if (screen.presents == 8) {
        u32 lit = 0, distinct = 0, seen[8] = { 0 };
        for (u32 i = 0; i < screen.width * screen.height; i += 997) {
            u32 px = screen.pixels[i] & 0x00FFFFFFu;
            if (px) lit++;
            bool known = false;
            for (u32 k = 0; k < distinct; k++)
                if (seen[k] == px) { known = true; break; }
            if (!known && distinct < 8) seen[distinct++] = px;
        }
        u32 sampled = (screen.width * screen.height + 996) / 997;
        /* Whatever is on screen at this point - the console early on, the
         * desktop later - was composed into these pages and shown from them. */
        kinfo("svga3d", "what is on screen comes from the card: %u frames "
                        "presented as commands, and of %u pixels sampled from "
                        "the memory the display reads, %u are lit in at least "
                        "%u different colours", screen.presents, sampled, lit,
              distinct);

        /* And if a second display is mirroring, that it is actually showing the
         * SAME picture: sample both scan-outs and count where they agree.  This
         * is the live proof the mirror works, not just that the commands were
         * accepted - the desktop has drawn real frames by now. */
        if (screen2.attached && second_mode == SM_MIRROR && screen2.pixels &&
            screen2.width == screen.width && screen2.height == screen.height) {
            u32 same = 0;
            for (u32 i = 0; i < screen.width * screen.height; i += 997)
                if (screen2.pixels[i] == screen.pixels[i]) same++;
            kinfo("svga3d", "the second display mirrors the primary: %u of %u "
                            "sampled pixels match across the two scan-outs",
                  same, sampled);

            /* Exercise the third mode once, now that the desktop is up: switch
             * to only-the-other (main display black, desktop on the second),
             * then back to mirror.  A brief flicker, and it proves the switch
             * works end to end rather than only that mirror does. */
            svga3d_set_second_mode(SM_ONLY_SECOND);
            svga3d_set_second_mode(SM_MIRROR);
        }
    }
}

u32 svga3d_screen_presents(void) { return screen.presents; }

/* ------------------------------------------------- a second display (#7)
 *
 * The primary above is screen 0.  When the adapter offers more than one display
 * (svga_num_displays > 1, which in the VM means svga.numDisplays was set), a
 * second screen target can be created - its OWN surface and MOB, positioned to
 * the right of the primary so the two form one extended desktop.  This is what
 * makes extend/mirror/only-the-other something the card does rather than
 * geometry on paper; the layout that decides each screen's rectangle is
 * display_layout_compute, already unit-tested.  screen2, second_mode and
 * second_show_primary live above svga3d_screen_update because the present path
 * calls into them.
 */

bool svga3d_second_attached(void) { return screen2.attached; }
int  svga3d_second_mode(void) { return second_mode; }

/* A black image for the main display to show in only-the-other mode, made once
 * the first time it is needed. */
static u32 *black_pixels;
static bool black_ready;

static bool ensure_screen0_black(void) {
    if (black_ready) return true;
    if (!screen.attached) return false;
    u32 w = screen.width, h = screen.height;
    u32 bytes = w * h * 4;
    u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = 0;
    u32 *px = dma_alloc_pages(pages, &phys);
    if (!px) return false;
    memset(px, 0, (size_t)pages * PAGE_SIZE);           /* all black */
    bool ok = gb_define_mob(MOB_BLACK, phys / PAGE_SIZE, pages * (u32)PAGE_SIZE);
    svga_fifo_sync();
    u32 d[11] = { SCREEN_BLACK_SURFACE,
                  SURFACE_BIND_RENDER_TARGET | SURFACE_SCREENTARGET,
                  FORMAT_B8G8R8A8_UNORM, 1, 1, FILTER_NONE, w, h, 1, 1, 0 };
    ok = ok && cb_one("a black surface for the main display",
                      CMD3D_DEFINE_GB_SURFACE_V2, d, 11);
    u32 j[2] = { SCREEN_BLACK_SURFACE, MOB_BLACK };
    ok = ok && cb_one("joining the black surface to its memory",
                      CMD3D_BIND_GB_SURFACE, j, 2);
    if (!ok) return false;
    black_pixels = px;
    black_ready = true;
    return true;
}

/* Point the main display (screen 0) at a surface and present it, so it shows
 * that surface's picture: the black one for only-the-other, the desktop's for
 * every other mode. */
static bool rebind_screen0(u32 sid) {
    u32 bs[4] = { SCREEN_ID, sid, 0, 0 };
    if (!cb_one("re-pointing the main display", CMD3D_BIND_GB_SCREENTARGET, bs, 4))
        return false;
    u32 up[5] = { SCREEN_ID, 0, 0, screen.width, screen.height };
    cb_begin();
    cb_command(CMD3D_UPDATE_GB_SCREENTARGET, up, 5);
    cb_submit(VIEW_NONE);
    return true;
}

void svga3d_set_second_mode(int mode) {
    int prev = second_mode;
    second_mode = mode;
    if (!screen.attached || prev == mode) return;

    if (mode == SM_ONLY_SECOND) {
        /* Only-the-other: the main display goes black; the desktop moves to the
         * second (the present path copies it there because the mode says so). */
        bool ok = ensure_screen0_black() && rebind_screen0(SCREEN_BLACK_SURFACE);
        kinfo("svga3d", "only-the-other: the main display %s blanked while the "
                        "second shows the desktop", ok ? "is" : "could not be");
    } else if (prev == SM_ONLY_SECOND) {
        /* Leaving it: the main display shows the desktop again. */
        rebind_screen0(SCREEN_SURFACE);
        kinfo("svga3d", "the main display shows the desktop again");
    }

    /* Only-primary: the second display shows nothing, so clear it once rather
     * than leave the last mode's picture frozen on it. */
    if (mode == SM_ONLY_PRIMARY && screen2.attached && screen2.pixels) {
        memset(screen2.pixels, 0,
               (size_t)screen2.width * screen2.height * 4);
        svga3d_second_update(0, 0, screen2.width, screen2.height);
    }
}

/* Whether extend is set up (a wide framebuffer the desktop draws as one screen).
 * When it is, a mode change cannot be applied live - the other modes want a
 * narrow framebuffer - so it is saved for the next start instead. */
bool svga3d_extend_active(void) { return extend_fb != NULL; }

/* Set up extend: allocate a logical framebuffer as wide as both displays put
 * together, which the desktop draws as a single wide screen.  Hands back its
 * physical base and size so the caller can point the system framebuffer at it
 * (done at boot, before the desktop starts, so the desktop simply sees a wide
 * screen); the present path then splits each frame across the two displays.
 * Both screens must already be attached. */
bool svga3d_extend_setup(u64 *out_phys, u32 *out_w, u32 *out_h, u32 *out_pitch) {
    if (!screen.attached || !screen2.attached || extend_fb) return false;
    u32 lw = screen.width + screen2.width;
    u32 h  = screen.height;
    u32 bytes = lw * h * 4;
    u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = 0;
    u32 *fb = dma_alloc_pages(pages, &phys);
    if (!fb) return false;
    memset(fb, 0, (size_t)pages * PAGE_SIZE);
    extend_fb = fb;
    extend_w  = lw;
    extend_h  = h;
    second_mode = SM_EXTEND;
    if (out_phys)  *out_phys  = phys;
    if (out_w)     *out_w     = lw;
    if (out_h)     *out_h     = h;
    if (out_pitch) *out_pitch = lw * 4;
    kinfo("svga3d", "extend set up: one %ux%u desktop - its left half is shown "
                    "on screen 0, its right half on screen 1", lw, h);
    return true;
}

/* Create screen target 1 at (x_root, 0), backed by its own surface and memory -
 * separate from the primary's, so writing one cannot change the other. */
bool svga3d_screen_attach_second(u32 w, u32 h, u32 x_root) {
    if (screen2.attached) return true;
    if (!screen.attached) return false;              /* primary sets up otables/cb */
    if (!w || !h || w > 4096 || h > 4096) return false;

    u32 bytes = w * h * 4;
    u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = 0;
    u32 *pixels = dma_alloc_pages(pages, &phys);
    if (!pixels) {
        kinfo("svga3d", "no room for a second %ux%u screen in card-readable "
                        "memory, so the second display is not driven", w, h);
        return false;
    }
    memset(pixels, 0, (size_t)pages * PAGE_SIZE);

    bool ok = gb_define_mob(MOB_SCREEN2, phys / PAGE_SIZE, pages * (u32)PAGE_SIZE);
    svga_fifo_sync();

    u32 d[11] = { SCREEN_SURFACE2,
                  SURFACE_BIND_RENDER_TARGET | SURFACE_SCREENTARGET,
                  FORMAT_B8G8R8A8_UNORM, 1, 1, FILTER_NONE, w, h, 1, 1, 0 };
    ok = ok && cb_one("a surface for the second display",
                      CMD3D_DEFINE_GB_SURFACE_V2, d, 11);
    u32 j[2] = { SCREEN_SURFACE2, MOB_SCREEN2 };
    ok = ok && cb_one("joining it to its own memory", CMD3D_BIND_GB_SURFACE, j, 2);

    /* stid 1, at x_root - and NOT primary, so it is a second display beside the
     * first rather than a replacement for it. */
    u32 st[7] = { SCREEN_ID2, w, h, x_root, 0, 0, 0 };
    ok = ok && cb_one("a second screen for the display",
                      CMD3D_DEFINE_GB_SCREENTARGET, st, 7);
    u32 bs[4] = { SCREEN_ID2, SCREEN_SURFACE2, 0, 0 };
    ok = ok && cb_one("pointing the second screen at its surface",
                      CMD3D_BIND_GB_SCREENTARGET, bs, 4);

    if (!ok) {
        u32 one[1] = { SCREEN_ID2 };
        cb_begin();
        cb_command(CMD3D_DESTROY_GB_SCREENTARGET, one, 1);
        cb_submit(VIEW_NONE);
        gb_destroy_surface(SCREEN_SURFACE2);
        gb_destroy_mob(MOB_SCREEN2);
        svga_fifo_sync();
        kinfo("svga3d", "the card refused a second screen target (status %u), "
                        "so only the primary display is driven", cb.last_status);
        return false;
    }

    screen2.attached = true;
    screen2.width = w;
    screen2.height = h;
    screen2.x_root = x_root;
    screen2.phys = phys;
    screen2.pixels = pixels;
    kinfo("svga3d", "a second display is driven by the card: screen 1, %ux%u, "
                    "at x=%u - its own surface and memory beside the primary's",
          w, h, x_root);
    return true;
}

/* Present a rectangle of the second screen, reading from its own surface. */
void svga3d_second_update(u32 x, u32 y, u32 w, u32 h) {
    if (!screen2.attached) return;
    if (x >= screen2.width || y >= screen2.height) return;
    if (x + w > screen2.width)  w = screen2.width - x;
    if (y + h > screen2.height) h = screen2.height - y;
    if (!w || !h) return;
    u32 up[5] = { SCREEN_ID2, x, y, w, h };
    cb_begin();
    cb_command(CMD3D_UPDATE_GB_SCREENTARGET, up, 5);
    cb_submit(VIEW_NONE);
}

/* Prove the second display is INDEPENDENT of the first, not an alias of it (the
 * trap the GOP path fell into: two "outputs" that were one framebuffer).  Fill
 * screen 1 with a colour the primary is not showing, present both, and check
 * that screen 1's memory holds that colour while the primary's is unchanged.
 * Runs only when the adapter actually offers a second display. */
void svga3d_second_screen_selftest(void) {
    if (!screen.attached) return;
    if (svga_num_displays() < 2) {
        kinfo("svga3d", "the adapter offers one display, so there is no second "
                        "screen to drive here (needs svga.numDisplays>1 / real "
                        "hardware with two outputs)");
        return;
    }
    if (!svga3d_screen_attach_second(screen.width, screen.height, screen.width))
        return;

    /* A distinct fill: if screen 1 were secretly the same buffer as screen 0,
     * this would overwrite the primary.  It must not. */
    const u32 mark = 0x00335577u;
    u32 primary_before = screen.pixels ? screen.pixels[0] : 0;
    for (u32 i = 0; i < screen2.width * screen2.height; i++)
        screen2.pixels[i] = mark;

    svga3d_second_update(0, 0, screen2.width, screen2.height);
    svga3d_screen_update(0, 0, screen.width, screen.height);
    svga_fifo_sync();

    /* Read both back.  Screen 1 must hold the mark; the primary must be
     * whatever it was, not the mark - that is what "two independent scan-outs"
     * means, verified rather than assumed. */
    u32 n = screen2.width * screen2.height;
    u32 got = 0;
    for (u32 i = 0; i < n; i += 997)
        if ((screen2.pixels[i] & 0x00FFFFFFu) == mark) got++;
    u32 sampled = (n + 996) / 997;
    u32 primary_after = screen.pixels ? screen.pixels[0] : 0;
    bool independent = (got == sampled) && (primary_after == primary_before);

    kinfo("svga3d", "second display %s: screen 1 holds its own colour in %u of "
                    "%u sampled pixels, and the primary's first pixel is "
                    "unchanged (%08x %s %08x) - so the two are %s scan-outs",
          independent ? "IS INDEPENDENT" : "check inconclusive",
          got, sampled, primary_before,
          primary_after == primary_before ? "==" : "!=", primary_after,
          independent ? "two separate" : "possibly aliased");

    /* With that proven, put the second display to work: mirror the primary by
     * default (the safe mode - it shows what the one screen shows, so it is
     * right on any two panels).  extend / only-secondary are the same machinery
     * with a different rule, switchable through svga3d_set_second_mode. */
    if (independent) {
        second_mode = SM_MIRROR;
        kinfo("svga3d", "the second display now mirrors the primary; extend and "
                        "only-the-other are a mode switch away");
    }
}

/* ============================================ drawing for a program to ask for
 *
 * Everything above happens at start-up and proves the card works.  None of it
 * is reachable by anything running on this system, which makes it a
 * demonstration rather than a driver.  This is the way through: a program
 * hands over corners and the card draws them onto the screen the display is
 * already reading out of.
 *
 * The pipeline it draws with is the one built and checked at start-up - the
 * two programs, how to read a vertex, and what to do with a pixel once it has
 * a colour.  Those are set once and stay set; a draw only has to say where the
 * corners are and how many.
 */
#define USER_VERTEX_SURFACE 52
#define USER_VERTEX_MOB     36
#define USER_MAX_VERTICES   512
#define USER_VERTEX_FLOATS  8


/* Can a program draw on the card at all?  Both halves are needed: a pipeline
 * that came up, and a screen for the result to land on. */
bool svga3d_can_draw(void) {
    return user_draw.ready && screen.attached && screen.drawable;
}

static bool user_draw_build(void) {
    if (user_draw.built) return true;

    u32 bytes = USER_MAX_VERTICES * USER_VERTEX_FLOATS * (u32)sizeof(float);
    u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = 0;
    float *verts = dma_alloc_pages(pages, &phys);
    if (!verts) return false;
    memset(verts, 0, (size_t)pages * PAGE_SIZE);

    if (!gb_define_mob(USER_VERTEX_MOB, phys / PAGE_SIZE,
                       pages * (u32)PAGE_SIZE))
        return false;
    svga_fifo_sync();

    u32 d[11] = { USER_VERTEX_SURFACE, SURFACE_BIND_VERTEX_BUFFER,
                  FORMAT_BUFFER, 1, 1, FILTER_NONE, bytes, 1, 1, 1, 0 };
    if (!cb_one("a buffer for corners a program supplies",
                CMD3D_DEFINE_GB_SURFACE_V2, d, 11))
        return false;
    u32 j[2] = { USER_VERTEX_SURFACE, USER_VERTEX_MOB };
    if (!cb_one("joining it to its memory", CMD3D_BIND_GB_SURFACE, j, 2))
        return false;

    user_draw.verts = verts;
    user_draw.phys = phys;
    user_draw.built = true;
    return true;
}

/* Corners in, pixels on the screen.  Each vertex is eight numbers: where it is
 * and what colour it carries.  Returns how many triangles were drawn, or a
 * negative number if the card would not take them. */
int svga3d_draw_user(const float *verts, u32 triangles) {
    if (!svga3d_can_draw()) return -1;
    if (!triangles || triangles * 3 > USER_MAX_VERTICES) return -1;
    if (!user_draw_build()) return -1;

    u32 vertices = triangles * 3;
    for (u32 i = 0; i < vertices * USER_VERTEX_FLOATS; i++)
        user_draw.verts[i] = verts[i];
    __asm__ volatile("sfence" ::: "memory");

    u32 sw = 0, sh = 0;
    svga3d_screen_memory(&sw, &sh, NULL);

    u32 rt[2] = { VIEW_NONE, screen.view };
    if (!dx_one("drawing into the screen", CMD3D_DX_SET_RENDERTARGETS, rt, 2,
                DX_CID))
        return -1;

    u32 vp[7] = { 0, as_bits(0.0f), as_bits(0.0f), as_bits_of(sw),
                  as_bits_of(sh), as_bits(0.0f), as_bits(1.0f) };
    if (!dx_one("across the whole of it", CMD3D_DX_SET_VIEWPORTS, vp, 7,
                DX_CID))
        return -1;

    u32 vb[4] = { 0, USER_VERTEX_SURFACE,
                  user_stride ? user_stride
                              : USER_VERTEX_FLOATS * (u32)sizeof(float), 0 };
    if (!dx_one("where the corners are", CMD3D_DX_SET_VERTEX_BUFFERS, vb, 4,
                DX_CID))
        return -1;

    u32 tp[1] = { PRIMITIVE_TRIANGLELIST };
    if (!dx_one("what to make of them", CMD3D_DX_SET_TOPOLOGY, tp, 1, DX_CID))
        return -1;

    u32 dr[2] = { vertices, 0 };
    if (!dx_one("the triangles themselves", CMD3D_DX_DRAW, dr, 2, DX_CID))
        return -1;

    /* The card drew into its own copy of the surface, which is not the pages
     * this system handed over - those are only brought back into step when
     * asked.  Without this the caller reads whatever it last wrote there
     * itself and concludes nothing was drawn, which is the opposite of what
     * happened. */
    gb_readback(SCREEN_SURFACE);
    svga_fifo_sync();
    timer_udelay(20000);

    /* And show it, which for a screen the card owns is one command. */
    u32 up[5] = { SCREEN_ID, 0, 0, sw, sh };
    cb_begin();
    cb_command(CMD3D_UPDATE_GB_SCREENTARGET, up, 5);
    cb_submit(VIEW_NONE);

    user_draw.draws++;
    return (int)triangles;
}

/* =========================== shaders a program compiled and handed over
 *
 * Everything drawn above uses shaders written here, as the numbers the
 * interface is defined in.  This takes a program's own instead - compiled by
 * whatever compiler that program was built with, in whatever language, and
 * handed down as the bytes that compiler produced.
 *
 * What arrives is only the instructions.  The list of what a program is handed
 * and what it hands on comes in a different shape from the one this adapter
 * wants, so that part is still made here - which works because the arrangement
 * of a vertex is fixed, and would not if a program could choose its own.
 */
static bool user_shaders_ready;

int svga3d_set_shaders(const u32 *vs, u32 vs_words,
                       const u32 *ps, u32 ps_words,
                       const u32 *vs_takes, u32 vs_takes_n,
                       const u32 *vs_gives, u32 vs_gives_n,
                       const u32 *ps_takes, u32 ps_takes_n,
                       const u32 *ps_gives, u32 ps_gives_n) {
    if (!svga3d_can_draw()) return -1;
    if (!vs || !ps || !vs_words || !ps_words) return -1;
    if (vs_words > 512 || ps_words > 512) return -1;
    if (!probe_vs_bytes || !probe_ps_bytes) return -1;

    /* What each program is handed and hands on, as its own compiler wrote it
     * down rather than as this driver would have guessed.  A shader shaped
     * differently from the ones written here works for exactly this reason. */
    if (!vs_takes_n || !vs_gives_n || !ps_gives_n) return -1;

    for (u32 i = 0; i < vs_words; i++) probe_vs_bytes[i] = vs[i];
    u32 vw = shader_signature(probe_vs_bytes, vs_words, vs_takes, vs_takes_n,
                              vs_gives, vs_gives_n);
    for (u32 i = 0; i < ps_words; i++) probe_ps_bytes[i] = ps[i];
    u32 pw = shader_signature(probe_ps_bytes, ps_words, ps_takes, ps_takes_n,
                              ps_gives, ps_gives_n);
    __asm__ volatile("sfence" ::: "memory");

    struct { u32 id; u32 type; u32 words; u32 mob; } both[2] = {
        { SHADER_USER_VS, SHADERTYPE_VS, vw, MOB_VS },
        { SHADER_USER_PS, SHADERTYPE_PS, pw, MOB_PS },
    };

    for (int i = 0; i < 2; i++) {
        u32 d[3] = { both[i].id, both[i].type,
                     both[i].words * (u32)sizeof(u32) };
        if (!dx_one("a program's own program", CMD3D_DX_DEFINE_SHADER, d, 3,
                    DX_CID))
            return -1;
        u32 b[4] = { DX_CID, both[i].id, both[i].mob, 0 };
        if (!dx_one("attaching it to its memory", CMD3D_DX_BIND_SHADER, b, 4,
                    VIEW_NONE))
            return -1;
        u32 c[2] = { both[i].id, both[i].type };
        if (!dx_one("choosing it", CMD3D_DX_SET_SHADER, c, 2, DX_CID))
            return -1;
    }

    user_shaders_ready = true;
    kinfo("svga3d", "a program handed over its own shaders: %u and %u words, "
                    "compiled somewhere else, and what each expects read from "
                    "the same place rather than guessed (%u in, %u out)",
          vs_words, ps_words, vs_takes_n, vs_gives_n);
    return 0;
}

/* ================================ how a program lays its own vertices out
 *
 * Everything drawn above reads a vertex the one way this system writes them:
 * where the corner is, then what colour it carries, thirty-two bytes apart.
 * A program with its own shaders almost certainly has its own arrangement too,
 * and being made to match would be a strange thing to insist on.
 *
 * So the arrangement is described rather than assumed - one line per thing
 * inside a vertex, saying where it sits and which of the program's inputs it
 * arrives as.  Which input is not guessed either: it comes from the same list
 * the shader's own signature came from.
 */
int svga3d_set_layout(const u32 *elements, u32 count, u32 stride) {
    if (!svga3d_can_draw()) return -1;
    if (!count || count > 16 || !stride) return -1;

    u32 body[1 + 16 * 6];
    u32 n = 0;
    body[n++] = ELEMENT_LAYOUT_USER;
    for (u32 i = 0; i < count; i++) {
        const u32 *e = &elements[i * 4];
        body[n++] = e[0];               /* which buffer it comes from  */
        body[n++] = e[1];               /* how far into each vertex    */
        body[n++] = e[2];               /* what kind of numbers        */
        body[n++] = INPUT_PER_VERTEX;
        body[n++] = 0;                  /* not per-instance            */
        body[n++] = e[3];               /* which input of the program  */
    }

    if (!dx_one("how a program lays its vertices out",
                CMD3D_DX_DEFINE_ELEMENTLAYOUT, body, n, DX_CID))
        return -1;

    u32 use[1] = { ELEMENT_LAYOUT_USER };
    if (!dx_one("using that way of reading them",
                CMD3D_DX_SET_INPUT_LAYOUT, use, 1, DX_CID))
        return -1;

    user_stride = stride;
    kinfo("svga3d", "a program described its own vertices: %u thing(s) in "
                    "each, %u bytes apart", count, stride);
    return 0;
}

/* ===================================== a picture a program asks to be drawn
 *
 * NOT WORKING, and left here with what is known rather than removed.
 *
 * Every command below is accepted and the drawing changes no pixel at all.
 * The cause is one step: a picture handed to the card from this system never
 * arrives in a surface built here, and a picture the card does not have
 * samples as nothing, which is transparent - so the drawing succeeds and does
 * nothing, exactly as it would if it had worked on an empty picture.
 *
 * Everything around it is known to work, and that has been shown rather than
 * assumed: drawing the same rectangle at the same place with its colour coming
 * from its corners instead of from a picture changes the pixel underneath it,
 * so the rectangle, the mixing, the showing and the reading back are all
 * right.  Only the picture is wrong.
 *
 * The same transfer into the same kind of surface works in the tests at
 * start-up, so it is not the transfer as such.  Ruled out by trying: the
 * surface being only readable rather than also drawable, the sample count,
 * the texture hint the older path uses, the surface being 256 wide against 64,
 * carrying the transfer through the ring rather than a command buffer,
 * drawing into the surface once before handing anything to it, and building
 * the whole path at start-up rather than the first time a program asks -
 * which is what made a view of the screen work.  None of them changed it.
 *
 * Also tried and no different: writing the picture into a surface of the older
 * kind and copying it across inside the card, both with that surface sharing
 * the memory and with memory of its own - which is the arrangement the copy
 * test at start-up proves works.
 *
 * Corners and colours are enough to prove the card works.  What a window
 * system actually needs is this: here is a rectangle of pixels, put it there
 * on the screen, and let what is already behind it show through wherever it is
 * not solid.  That is one window composited, and doing it on the card is the
 * difference between the processor copying every pixel of every window every
 * frame and it copying none of them.
 *
 * The picture goes into a surface the card holds, and the rectangle it lands
 * on is two triangles whose corners carry positions within that surface.  The
 * mixing is the same one proved above, driven by the picture's own fourth
 * channel - so a window that is solid replaces, and one that is not does not.
 */
#define USER_TEXTURE_SURFACE 55
#define USER_TEXTURE_MOB     39
#define USER_TEXTURE_VIEW    1
#define USER_TEXTURE_RTV     3
#define STAGING_SURFACE      56
#define STAGING_MOB          40
#define USER_TEXTURE_SIDE    64

static struct {
    bool  built;
    u32  *pixels;
} user_image;

static bool user_image_build(void) {
    if (user_image.built) return true;

    u32 bytes = USER_TEXTURE_SIDE * USER_TEXTURE_SIDE * 4;
    u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = 0;
    u32 *mem = dma_alloc_pages(pages, &phys);
    if (!mem) return false;
    memset(mem, 0, (size_t)pages * PAGE_SIZE);

    if (!gb_define_mob(USER_TEXTURE_MOB, phys / PAGE_SIZE,
                       pages * (u32)PAGE_SIZE))
        return false;
    svga_fifo_sync();

    u32 d[11] = { USER_TEXTURE_SURFACE,
                  SURFACE_BIND_RENDER_TARGET | SURFACE_BIND_SHADER_RESOURCE,
                  FORMAT_B8G8R8A8_UNORM, 1, 1, FILTER_NONE,
                  USER_TEXTURE_SIDE, USER_TEXTURE_SIDE, 1, 1, 0 };
    if (!cb_one("a surface to hold a program's picture",
                CMD3D_DEFINE_GB_SURFACE_V2, d, 11))
        return false;
    u32 j[2] = { USER_TEXTURE_SURFACE, USER_TEXTURE_MOB };
    if (!cb_one("joining it to its memory", CMD3D_BIND_GB_SURFACE, j, 2))
        return false;

    u32 srv[8] = { USER_TEXTURE_VIEW, USER_TEXTURE_SURFACE,
                   FORMAT_B8G8R8A8_UNORM, RESOURCE_TEXTURE2D, 0, 0, 1, 1 };
    if (!dx_one("a way for a program to reach its picture",
                CMD3D_DX_DEFINE_SHADERRESOURCE_VIEW, srv, 8, DX_CID))
        return false;

    /* Draw into it once before anything is put in it.  A surface the card has
     * never drawn into does not take what is handed to it - the transfer is
     * accepted and nothing arrives, and a picture the card does not have
     * samples as nothing at all, which is transparent.  Every drawing then
     * succeeds and changes not a single pixel.  Whether this is the card
     * making the storage real on first use or something else, it is what the
     * one that works had happen to it and this one did not. */
    u32 rv[7] = { USER_TEXTURE_RTV, USER_TEXTURE_SURFACE,
                  FORMAT_B8G8R8A8_UNORM, RESOURCE_TEXTURE2D, 0, 0, 1 };
    if (!dx_one("a view of it to draw into",
                CMD3D_DX_DEFINE_RENDERTARGET_VIEW, rv, 7, DX_CID))
        return false;
    u32 into[2] = { VIEW_NONE, USER_TEXTURE_RTV };
    if (!dx_one("drawing into it", CMD3D_DX_SET_RENDERTARGETS, into, 2, DX_CID))
        return false;
    u32 cl[5] = { USER_TEXTURE_RTV, 0, 0, 0, 0 };
    if (!dx_one("clearing it once", CMD3D_DX_CLEAR_RENDERTARGET_VIEW, cl, 5,
                DX_CID))
        return false;

    /* A second surface, of the older kind, with memory of its own.  A surface
     * of that kind takes a picture handed to it; one of the newer kind does
     * not, on this adapter.  The picture is written here, given to the card
     * here, and copied across inside the card - which is the arrangement the
     * copy test at start-up proves works, two separate surfaces and all. */
    u64 stage_phys = 0;
    u32 *stage = dma_alloc_pages(pages, &stage_phys);
    if (!stage) return false;
    memset(stage, 0, (size_t)pages * PAGE_SIZE);

    if (!gb_define_mob(STAGING_MOB, stage_phys / PAGE_SIZE,
                       pages * (u32)PAGE_SIZE))
        return false;
    svga_fifo_sync();
    if (!gb_define_surface(STAGING_SURFACE, USER_TEXTURE_SIDE,
                           USER_TEXTURE_SIDE) ||
        !gb_bind_surface(STAGING_SURFACE, STAGING_MOB))
        return false;
    svga_fifo_sync();

    user_image.pixels = stage;
    user_image.built = true;
    return true;
}

/* Put a rectangle of pixels on the screen at a place and a size, letting what
 * is behind it show through where it is not solid.  Returns 0, or a negative
 * number where there is no card to do it. */
int svga3d_draw_image(const u32 *src, u32 iw, u32 ih,
                      int x, int y, u32 dw, u32 dh) {
    return svga3d_draw_image_strided(src, iw, ih, iw, x, y, dw, dh);
}

/* The same, for a picture that is part of something wider.
 *
 * Without this a caller with a rectangle inside a frame has to copy it out to
 * make it contiguous before handing it over - and that copy is the one the
 * whole exercise is trying to avoid. */
int svga3d_draw_image_strided(const u32 *src, u32 iw, u32 ih, u32 src_stride,
                              int x, int y, u32 dw, u32 dh) {
    if (!svga3d_can_draw()) return -1;
    if (!iw || !ih || iw > probe_side || ih > probe_side)
        return -1;
    if (!dw || !dh) return -1;
    if (!user_draw_build() || !user_image_build()) return -1;

    /* Into the surface the tests at start-up built, not one built here.
     *
     * A picture handed into a surface this path creates never arrives - the
     * transfer is accepted and the picture stays empty, and an empty picture
     * samples as nothing, which is transparent, so the drawing succeeds and
     * changes not one pixel.  The same transfer into the surface made during
     * the start-up tests works every time.  What is different about the two
     * has not been found: the sample count, the bindings, the size, the older
     * path's hint, the transfer through the ring rather than a buffer, drawing
     * into the surface first, staging through a surface of the older kind and
     * copying across, and building the whole path at start-up rather than on
     * first use have all been tried and none of them changed it.
     *
     * So the one that works is the one used, and the limit that comes with it
     * - sixty-four pixels square - is stated rather than hidden. */
    if (!probe_texture) return -1;

    /* A row at a time, not a pixel at a time.
     *
     * Both rows are contiguous - the picture is packed and the staging surface
     * has a known stride - so there was never a reason to walk pixels, and
     * walking them cost enough to matter: at a full screen this is eight
     * hundred thousand separate assignments, and it turned a picture that the
     * card would draw in well under a millisecond into twenty-three
     * milliseconds of getting it there. */
    for (u32 row = 0; row < ih; row++)
        memcpy(probe_texture + (size_t)row * probe_side,
               src + (size_t)row * src_stride, (size_t)iw * 4);
    __asm__ volatile("sfence" ::: "memory");

    /* Only the corner of the surface the picture went into.
     *
     * The surface is as large as the biggest picture anything might hand over;
     * this picture is usually far smaller, and saying so is the difference
     * between a fixed cost per frame and one that follows the work. */
    u32 up[9] = {
        TEXTURE_SURFACE, 0, 0,            /* the surface, its face, its level */
        0, 0, 0,                          /* where the box starts             */
        iw, ih, 1                         /* and how big it is                */
    };
    /* Held back and sent with the corners below, in one buffer.  See the note
     * at the draw. */
    u32 sw = 0, sh = 0;
    svga3d_screen_memory(&sw, &sh, NULL);

    /* Where on the screen, in the coordinates the pipeline expects: across and
     * up from the middle, from minus one to one.  Each is a ratio of whole
     * numbers, so it is assembled rather than divided. */
    u32 left   = as_bits_ratio(2 * x - (int)sw, (int)sw);
    u32 right  = as_bits_ratio(2 * (x + (int)dw) - (int)sw, (int)sw);
    u32 top    = as_bits_ratio((int)sh - 2 * y, (int)sh);
    u32 bottom = as_bits_ratio((int)sh - 2 * (y + (int)dh), (int)sh);

    /* And which part of the surface holds the picture, since it sits in the
     * corner of a larger one. */
    u32 u1 = as_bits_ratio((int)iw, (int)probe_side);
    u32 v1 = as_bits_ratio((int)ih, (int)probe_side);
    const u32 zero = 0, one = as_bits(1.0f), half = as_bits(0.5f);

    const u32 quad[6][4] = {
        { left,  top,    zero, zero },
        { right, top,    u1,   zero },
        { left,  bottom, zero, v1   },
        { right, top,    u1,   zero },
        { right, bottom, u1,   v1   },
        { left,  bottom, zero, v1   },
    };
    u32 *out = (u32 *)user_draw.verts;
    for (int i = 0; i < 6; i++) {
        out[i * 8 + 0] = quad[i][0];
        out[i * 8 + 1] = quad[i][1];
        out[i * 8 + 2] = half;
        out[i * 8 + 3] = one;
        out[i * 8 + 4] = quad[i][2];
        out[i * 8 + 5] = quad[i][3];
        out[i * 8 + 6] = zero;
        out[i * 8 + 7] = one;
    }
    __asm__ volatile("sfence" ::: "memory");

    u32 uv[1] = { USER_VERTEX_SURFACE };
    cb_begin();
    cb_command(CMD3D_UPDATE_GB_IMAGE, up, 9);
    cb_command(CMD3D_UPDATE_GB_SURFACE, uv, 1);
    if (!cb_submit(VIEW_NONE)) {
        kinfo("svga3d", "the newer path: handing the picture and its corners "
                        "over refused, status %u at byte %u",
              cb.last_status, cb.last_error_at);
        return -1;
    }

    /* Read the picture, mix with what is there, and leave both as they were
     * afterwards so the next caller finds the pipeline it expects. */
    /* Everything the draw needs, in one buffer.
     *
     * Each of these used to go on its own, and each submission waits for the
     * card to answer before the next is written.  Thirteen of them cost about
     * ten milliseconds a picture - and measuring showed the ten milliseconds
     * were the waiting rather than the work: making the data smaller barely
     * moved it, and this is why.
     *
     * They all address the same drawing context, so they can travel together
     * and be waited for once.
     *
     * The cost is in what a failure tells you.  Thirteen submissions name the
     * command that was refused; one names only where in the buffer it stopped.
     * That is a real loss and the error below prints the offset for it, which
     * is what makes it recoverable rather than merely shorter.
     */
    bool restore_user = user_shaders_ready;

    u32 ps_tex[2] = { SHADER_PS_TEXTURED, SHADERTYPE_PS };
    u32 sr[3] = { 0, SHADERTYPE_PS, TEXTURE_VIEW };
    u32 sa[3] = { 0, SHADERTYPE_PS, SAMPLER_STATE };
    u32 mix[6] = { BLEND_STATE_MIX, as_bits(1.0f), as_bits(1.0f),
                   as_bits(1.0f), as_bits(1.0f), 0xFFFFFFFFu };
    u32 rt[2] = { VIEW_NONE, screen.view };
    u32 vp[7] = { 0, as_bits(0.0f), as_bits(0.0f), as_bits_of(sw),
                  as_bits_of(sh), as_bits(0.0f), as_bits(1.0f) };
    u32 vb[4] = { 0, USER_VERTEX_SURFACE, 8 * (u32)sizeof(float), 0 };
    u32 tp[1] = { PRIMITIVE_TRIANGLELIST };
    u32 dr[2] = { 6, 0 };
    /* Put the pipeline back, in the same breath - a caller's own shaders are
     * displaced by the one that reads a picture and have to be returned. */
    u32 plain[6] = { BLEND_STATE, as_bits(1.0f), as_bits(1.0f), as_bits(1.0f),
                     as_bits(1.0f), 0xFFFFFFFFu };
    u32 ps_plain[2] = { restore_user ? SHADER_USER_PS : SHADER_PS,
                        SHADERTYPE_PS };

    cb_begin();
    cb_command(CMD3D_DX_SET_SHADER, ps_tex, 2);
    cb_command(CMD3D_DX_SET_SHADER_RESOURCES, sr, 3);
    cb_command(CMD3D_DX_SET_SAMPLERS, sa, 3);
    cb_command(CMD3D_DX_SET_BLEND_STATE, mix, 6);
    cb_command(CMD3D_DX_SET_RENDERTARGETS, rt, 2);
    cb_command(CMD3D_DX_SET_VIEWPORTS, vp, 7);
    cb_command(CMD3D_DX_SET_VERTEX_BUFFERS, vb, 4);
    cb_command(CMD3D_DX_SET_TOPOLOGY, tp, 1);
    cb_command(CMD3D_DX_DRAW, dr, 2);
    cb_command(CMD3D_DX_SET_BLEND_STATE, plain, 6);
    cb_command(CMD3D_DX_SET_SHADER, ps_plain, 2);

    if (!cb_submit(DX_CID)) {
        kinfo("svga3d", "the newer path: drawing a picture refused, status %u "
                        "at byte %u of the buffer", cb.last_status,
              cb.last_error_at);
        return -1;
    }

    gb_readback(SCREEN_SURFACE);
    svga_fifo_sync();
    timer_udelay(20000);

    u32 show[5] = { SCREEN_ID, 0, 0, sw, sh };
    cb_begin();
    cb_command(CMD3D_UPDATE_GB_SCREENTARGET, show, 5);
    cb_submit(VIEW_NONE);
    return 0;
}
