/* nv_fifo.c - giving the card work to do.
 *
 * Everything so far has been about finding out what the card is and getting a
 * picture onto a monitor.  This is the other half: submitting work to the
 * engines, which is how anything is drawn faster than the processor can draw
 * it.
 *
 * The shape has been the same since Fermi and is still the shape on Blackwell:
 *
 *   A push buffer is a stream of method-and-value pairs, exactly like the
 *   display channel's, but addressed to one of eight subchannels rather than
 *   to a single engine.  A subchannel is bound to a class - two-dimensional
 *   drawing, three-dimensional drawing, copying, video - and from then on the
 *   methods in that subchannel mean whatever that class says they mean.
 *
 *   A GPFIFO is a ring of pointers to push buffers.  The driver does not tell
 *   the card about methods; it tells the card where a run of methods is and
 *   how long it is.  That indirection is what lets a buffer be built once and
 *   submitted many times, and it is why the ring is small and the buffers are
 *   not.
 *
 *   Completion is a semaphore.  There is no interrupt that says "that
 *   submission is done"; the driver appends a release of a value to an address
 *   it can see, and watches for it.  Everything above - fences, swap chains,
 *   whether a buffer can be reused - is built on that one mechanism.
 *
 * The three mistakes worth naming, because each produces a card that hangs
 * rather than one that draws wrongly:
 *
 *   A packet header's count must match the number of values that follow it.
 *   The card uses the count to find the next header, so a wrong one does not
 *   corrupt that packet - it makes every packet after it garbage.
 *
 *   The push buffer must be complete in memory before the GPFIFO entry that
 *   points at it becomes visible, and the entry before the pointer that
 *   reveals it.  Two orderings, both easy to get right and impossible to
 *   debug when wrong.
 *
 *   A semaphore release has to come after the work it is reporting on, and the
 *   engine has to have been told to wait for the pipeline to drain first.  A
 *   release that overtakes its own work is a fence that fires early, and what
 *   it produces is a frame that is sometimes torn.
 *
 * ---------------------------------------------------------------------------
 * On what runs here.  Up to Pascal the engines take this directly and no
 * firmware is involved, which is the generation the model stands in for.  From
 * Turing on the same submission format is used, but the channel it goes into
 * is set up through the resource manager in nv_gsp_rm.c rather than by writing
 * registers - so this layer is unchanged and the layer under it is not.
 *
 * Run against the model at the end of this file, this proves the packet
 * encoding, the ring mechanics, the subchannel binding, the ordering and the
 * semaphore - and the model executes the drawing methods into real pixels, so
 * the check is that the right rectangle came out in the right colour rather
 * than that the right numbers were written.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

/* --------------------------------------------------------------- packets */

static u32 push_header(u32 op, int subchannel, u32 method, u32 count) {
    return (op << 29) | ((count & 0x1FFF) << 16) |
           (((u32)subchannel & 7) << 13) | ((method >> 2) & 0xFFF);
}

void nv_push_begin(nv_fifo_t *f, int subchannel, u32 method, u32 count) {
    if (!f->ready || f->push_at + 1 + count > f->push_words) {
        f->overflowed = true;
        return;
    }
    f->push[f->push_at++] = push_header(NV_PUSH_OP_INC, subchannel, method, count);
    f->expected = count;
    f->emitted = 0;
}

/* The same, for a run of values that all go to one method - which is how a
 * list of vertices or a block of data is pushed without repeating the header. */
void nv_push_begin_same(nv_fifo_t *f, int subchannel, u32 method, u32 count) {
    if (!f->ready || f->push_at + 1 + count > f->push_words) {
        f->overflowed = true;
        return;
    }
    f->push[f->push_at++] = push_header(NV_PUSH_OP_NINC, subchannel, method, count);
    f->expected = count;
    f->emitted = 0;
}

void nv_push_data(nv_fifo_t *f, u32 value) {
    if (!f->ready || f->push_at >= f->push_words) { f->overflowed = true; return; }
    f->push[f->push_at++] = value;
    f->emitted++;
}

/* A method whose value is small enough to travel inside the header itself.
 * Half the methods a driver sends are a single small number, and this halves
 * what the card has to fetch for them. */
void nv_push_immediate(nv_fifo_t *f, int subchannel, u32 method, u32 data) {
    if (!f->ready || f->push_at >= f->push_words) { f->overflowed = true; return; }
    if (data > 0x1FFF) {
        /* Too large for the field.  Sent the long way rather than truncated,
         * because truncating it would send a different value silently. */
        nv_push_begin(f, subchannel, method, 1);
        nv_push_data(f, data);
        nv_push_end(f);
        return;
    }
    f->push[f->push_at++] = push_header(NV_PUSH_OP_IMMD, subchannel, method, data);
}

bool nv_push_end(nv_fifo_t *f) {
    if (f->emitted != f->expected) {
        kerr("nv-fifo", "a packet declared %u values and wrote %u; every packet "
                        "after it would be read from the wrong place",
             f->expected, f->emitted);
        /* Backed out.  A wrong packet must never reach the card, because
         * nothing downstream can recover from one. */
        f->push_at -= f->emitted + 1;
        f->expected = f->emitted = 0;
        f->bad_packets++;
        return false;
    }
    f->expected = f->emitted = 0;
    return true;
}

/* ------------------------------------------------------------- the ring */

bool nv_fifo_init(nv_card_t *c, nv_fifo_t *f, int chid,
                  u64 *entries, u64 entries_gpu, u32 entry_count,
                  u32 *push, u64 push_gpu, u32 push_words,
                  volatile u32 *semaphore, u64 semaphore_gpu) {
    memset(f, 0, sizeof *f);

    if (!entries || !entry_count || (entry_count & (entry_count - 1))) {
        kwarn("nv-fifo", "a ring of %u entries cannot be masked", entry_count);
        return false;
    }

    f->entries = entries;
    f->entries_gpu = entries_gpu;
    f->count = entry_count;
    f->push = push;
    f->push_gpu = push_gpu;
    f->push_words = push_words;
    f->semaphore = semaphore;
    f->semaphore_gpu = semaphore_gpu;
    f->chid = chid;

    memset(entries, 0, (size_t)entry_count * 8);
    memset(push, 0, (size_t)push_words * 4);
    if (semaphore) *semaphore = 0;

    /* Where the ring is and how long, then which runlist the channel is on.
     * The instance block that describes the rest of the channel is built by
     * whatever set the channel up - registers on Pascal and earlier, the
     * resource manager from Turing on - and is not this layer's business. */
    nv_wr32(c, NV_PFIFO_GPFIFO_BASE_LO(chid), (u32)entries_gpu);
    nv_wr32(c, NV_PFIFO_GPFIFO_BASE_HI(chid), (u32)(entries_gpu >> 32));
    nv_wr32(c, NV_PFIFO_GPFIFO_SIZE(chid), entry_count);
    nv_wr32(c, NV_PFIFO_CHANNEL_GP_PUT(chid), 0);

    f->ready = true;
    return true;
}

/* Everything pushed since the last submission becomes one entry in the ring. */
bool nv_fifo_submit(nv_card_t *c, nv_fifo_t *f) {
    if (!f->ready) return false;

    if (f->overflowed) {
        kerr("nv-fifo", "the push buffer overflowed; nothing was submitted");
        f->overflowed = false;
        f->push_at = 0;
        return false;
    }
    if (f->push_at == f->submitted_to) return true;      /* nothing new */

    u32 words = f->push_at - f->submitted_to;
    u64 at = f->push_gpu + (u64)f->submitted_to * 4;

    /* The length field counts words and sits at bit ten, so a run longer than
     * this cannot be described by one entry. */
    if (words > 0x1FFFFF) {
        kerr("nv-fifo", "a run of %u words is too long for one entry", words);
        return false;
    }

    /* How far the card has read.  A driver that assumes rather than asks fills
     * the ring once and then never submits again. */
    f->got = nv_rd32(c, NV_PFIFO_CHANNEL_GP_GET(f->chid)) & (f->count - 1);

    u32 next = (f->put + 1) & (f->count - 1);
    if (next == f->got) {
        kwarn("nv-fifo", "the ring is full");
        return false;
    }

    /* Everything in memory before the entry that points at it. */
    __asm__ volatile("" ::: "memory");

    f->entries[f->put] = (u64)(u32)at |
                         ((u64)((u32)(at >> 32) & 0xFF) << 32) |
                         ((u64)words << 42);
    f->put = next;

    /* And the entry before the pointer that reveals it. */
    __asm__ volatile("" ::: "memory");
    nv_wr32(c, NV_PFIFO_CHANNEL_GP_PUT(f->chid), f->put);

    f->submitted_to = f->push_at;
    f->submissions++;
    return true;
}

/* Telling a subchannel what its methods mean.  Until this is sent, every
 * method in that subchannel goes nowhere. */
bool nv_fifo_bind(nv_card_t *c, nv_fifo_t *f, int subchannel, u32 class_number) {
    (void)c;
    nv_push_begin(f, subchannel, NV_FIFO_SET_OBJECT, 1);
    nv_push_data(f, class_number);
    if (!nv_push_end(f)) return false;
    f->bound[subchannel & 7] = class_number;
    return true;
}

/* --------------------------------------------------------- knowing it is done
 *
 * A release of a value to an address, appended after the work.  The operation
 * field says release rather than acquire, and the flags say to wait for the
 * pipeline to drain first - without which the value can land before the
 * drawing it is reporting on.
 */
bool nv_fifo_fence(nv_card_t *c, nv_fifo_t *f, u32 value) {
    (void)c;
    nv_push_begin(f, 0, NV_FIFO_SEMAPHOREA, 4);
    nv_push_data(f, (u32)(f->semaphore_gpu >> 32) & 0xFF);
    nv_push_data(f, (u32)f->semaphore_gpu);
    nv_push_data(f, value);
    nv_push_data(f, NV_FIFO_SEMAPHORED_RELEASE | NV_FIFO_SEMAPHORED_AWAKEN);
    return nv_push_end(f);
}

bool nv_fifo_wait(nv_fifo_t *f, u32 value, int timeout_ms) {
    if (!f->semaphore) return false;
    for (int i = 0; i < timeout_ms * 100; i++) {
        if (*f->semaphore >= value) return true;
        timer_udelay(10);
    }
    return false;
}

/* ------------------------------------------------------------ drawing
 *
 * The two-dimensional class, which has been the same since Fermi and is what
 * every window system's fills and copies go through.  It is worth having even
 * on a card that can do far more: a solid rectangle through this costs the
 * processor nothing, and a window system does thousands of them a second.
 */
static void set_destination(nv_fifo_t *f, int subchannel, u64 surface,
                            u32 pitch, u32 width, u32 height) {
    nv_push_begin(f, subchannel, NV902D_SET_DST_FORMAT, 1);
    nv_push_data(f, NV902D_FORMAT_A8R8G8B8);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_DST_MEMORY_LAYOUT, 1);
    nv_push_data(f, NV902D_LAYOUT_PITCH);
    nv_push_end(f);

    /* Pitch, width and height are consecutive methods, so one packet does all
     * three - which is the whole reason the encoding has a count. */
    nv_push_begin(f, subchannel, NV902D_SET_DST_PITCH, 3);
    nv_push_data(f, pitch);
    nv_push_data(f, width);
    nv_push_data(f, height);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_DST_OFFSET_UPPER, 2);
    nv_push_data(f, (u32)(surface >> 32));
    nv_push_data(f, (u32)surface);
    nv_push_end(f);
}

static void set_source(nv_fifo_t *f, int subchannel, u64 surface,
                       u32 pitch, u32 width, u32 height) {
    nv_push_begin(f, subchannel, NV902D_SET_SRC_FORMAT, 1);
    nv_push_data(f, NV902D_FORMAT_A8R8G8B8);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_SRC_MEMORY_LAYOUT, 1);
    nv_push_data(f, NV902D_LAYOUT_PITCH);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_SRC_PITCH, 3);
    nv_push_data(f, pitch);
    nv_push_data(f, width);
    nv_push_data(f, height);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_SRC_OFFSET_UPPER, 2);
    nv_push_data(f, (u32)(surface >> 32));
    nv_push_data(f, (u32)surface);
    nv_push_end(f);
}

/* Copy a rectangle from one surface to another, or from a surface to itself.
 *
 * This is the other half of what a window system asks a card for.  Fills paint
 * the space between things; copies move the things themselves - a window
 * dragged across the screen, a list scrolled by one row, a buffer presented to
 * the display.  Both are memory bandwidth the processor does not have to
 * spend, and a copy saves more of it than a fill because it reads as well as
 * writes.
 *
 * The engine can scale while it copies, so it is told the step to take through
 * the source per destination pixel.  One-to-one is a step of exactly one,
 * which is written as an integer part of 1 and a fraction of 0; anything else
 * here would resample a straightforward copy and blur it.
 */
bool nv_2d_copy(nv_card_t *c, nv_fifo_t *f, int subchannel,
                u64 dst, u32 dst_pitch, u32 dst_width, u32 dst_height,
                u64 src, u32 src_pitch, u32 src_width, u32 src_height,
                u32 dx, u32 dy, u32 sx, u32 sy, u32 w, u32 h) {
    (void)c;
    if (f->bound[subchannel & 7] != NV_CLASS_TWOD) {
        kwarn("nv-fifo", "subchannel %d is not bound to the drawing class",
              subchannel);
        return false;
    }
    if (!w || !h) return true;                   /* nothing to do, not a fault */
    if (dx + w > dst_width || dy + h > dst_height ||
        sx + w > src_width || sy + h > src_height) {
        kwarn("nv-fifo", "a %ux%u copy from %u,%u to %u,%u leaves one of the "
                         "surfaces (%ux%u source, %ux%u destination)",
              w, h, sx, sy, dx, dy, src_width, src_height, dst_width, dst_height);
        return false;
    }

    set_destination(f, subchannel, dst, dst_pitch, dst_width, dst_height);
    set_source(f, subchannel, src, src_pitch, src_width, src_height);

    nv_push_begin(f, subchannel, NV902D_SET_CLIP_ENABLE, 1);
    nv_push_data(f, 0);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_OPERATION, 1);
    nv_push_data(f, NV902D_OPERATION_SRCCOPY);
    nv_push_end(f);

    /* Tell the engine the two rectangles may overlap unless they demonstrably
     * cannot.  Getting this wrong is only visible when they do overlap, and
     * then it tears a dragged window into repeated strips. */
    bool same_surface = (dst == src);
    nv_push_begin(f, subchannel, NV902D_SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP, 1);
    nv_push_data(f, same_surface ? 1 : 0);
    nv_push_end(f);

    /* Point sampling: this is a copy, not a rescale. */
    nv_push_begin(f, subchannel, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, 1);
    nv_push_data(f, 0);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, 4);
    nv_push_data(f, dx);
    nv_push_data(f, dy);
    nv_push_data(f, w);
    nv_push_data(f, h);
    nv_push_end(f);

    /* One source pixel per destination pixel, in both directions. */
    nv_push_begin(f, subchannel, NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC, 4);
    nv_push_data(f, 0);
    nv_push_data(f, 1);
    nv_push_data(f, 0);
    nv_push_data(f, 1);
    nv_push_end(f);

    /* The starting point in the source.  The last of these four is what
     * commits the blit, so the packet ends the sequence. */
    nv_push_begin(f, subchannel, NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC, 4);
    nv_push_data(f, 0);
    nv_push_data(f, sx);
    nv_push_data(f, 0);
    nv_push_data(f, sy);
    return nv_push_end(f);
}

bool nv_2d_fill(nv_card_t *c, nv_fifo_t *f, int subchannel, u64 surface,
                u32 pitch, u32 width, u32 height,
                u32 x, u32 y, u32 w, u32 h, u32 colour) {
    (void)c;
    if (f->bound[subchannel & 7] != NV_CLASS_TWOD) {
        kwarn("nv-fifo", "subchannel %d is not bound to the drawing class",
              subchannel);
        return false;
    }
    if (x + w > width || y + h > height) {
        kwarn("nv-fifo", "a %ux%u rectangle at %u,%u does not fit in %ux%u",
              w, h, x, y, width, height);
        return false;
    }

    set_destination(f, subchannel, surface, pitch, width, height);

    nv_push_begin(f, subchannel, NV902D_SET_CLIP_ENABLE, 1);
    nv_push_data(f, 0);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_OPERATION, 1);
    nv_push_data(f, NV902D_OPERATION_SRCCOPY);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_SET_RENDER_SOLID_PRIM_COLOR, 1);
    nv_push_data(f, colour);
    nv_push_end(f);

    nv_push_begin(f, subchannel, NV902D_RENDER_SOLID_PRIM_MODE, 1);
    nv_push_data(f, NV902D_PRIM_MODE_RECTS);
    nv_push_end(f);

    /* Two corners, as four consecutive methods.  The engine draws when the
     * second point's y arrives - the point is committed by its y, which is why
     * these have to go in this order and why an odd number of points draws
     * nothing. */
    nv_push_begin(f, subchannel, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 4);
    nv_push_data(f, x);
    nv_push_data(f, y);
    nv_push_data(f, x + w);
    nv_push_data(f, y + h);
    return nv_push_end(f);
}

/* ==========================================================================
 * An engine that is not there.
 *
 * It reads the ring, follows each entry into the push buffer it points at,
 * decodes the packets the way the card decodes them, and - for the drawing
 * class - actually draws.  That last part is what makes this worth having: the
 * check at the end is which pixels changed and to what, not which numbers were
 * written.
 * ==========================================================================
 */
static struct {
    bool present;
    u64  entries_gpu;
    u32  count;
    u32  got;

    u32  bound[8];
    int  packets, methods, draws;
    int  complaints;
    char complaint[128];

    /* The drawing class's state, as the engine holds it. */
    u64  dst;
    u32  dst_pitch, dst_width, dst_height, dst_format, dst_layout;
    u32  operation, colour, prim_mode;
    u32  point_x[2], point_y[2];
    int  points;

    /* The source surface, and the blit being set up against it. */
    u64  src;
    u32  src_pitch, src_width, src_height, src_format, src_layout;
    u32  blit_dx, blit_dy, blit_w, blit_h;
    u32  blit_sx, blit_sy;
    u32  blit_du_dx, blit_dv_dy;
    u32  safe_overlap;
    int  blits;

    u64  semaphore_gpu;
    u32  semaphore_upper, semaphore_lower, semaphore_payload;

    /* What the engine sees is a graphics address, not a host one - a real card
     * reaches memory through page tables the driver built for it.  These are
     * that translation, and having it here rather than passing host pointers
     * around is what makes the entry format above the real one: it only
     * carries forty bits, which no host pointer would survive. */
    u64  gpu_base;
    u8  *host_base;
    u32  span;
    int  chid;
    u32  three_d_class;
} fifo;

static u8 *translate(u64 gpu, u32 length) {
    if (!fifo.host_base) return NULL;
    if (gpu < fifo.gpu_base) return NULL;
    u64 into = gpu - fifo.gpu_base;
    if (into + length > fifo.span) return NULL;
    return fifo.host_base + into;
}

static void fifo_complain(const char *what) {
    fifo.complaints++;
    if (fifo.complaint[0]) return;
    size_t n = 0;
    while (what[n] && n < sizeof fifo.complaint - 1) {
        fifo.complaint[n] = what[n];
        n++;
    }
    fifo.complaint[n] = 0;
}

/* The rectangle, actually drawn.  Everything above exists so that this happens
 * in the right place. */
static void draw_rectangle(void) {
    if (fifo.prim_mode != NV902D_PRIM_MODE_RECTS) {
        fifo_complain("a solid primitive was drawn without a mode");
        return;
    }
    if (!fifo.dst || !fifo.dst_pitch) {
        fifo_complain("a rectangle was drawn with no destination");
        return;
    }
    if (fifo.dst_format != NV902D_FORMAT_A8R8G8B8) {
        fifo_complain("the destination format was never set");
        return;
    }
    if (fifo.dst_layout != NV902D_LAYOUT_PITCH) {
        fifo_complain("the destination layout was never set");
        return;
    }

    u32 x0 = fifo.point_x[0], y0 = fifo.point_y[0];
    u32 x1 = fifo.point_x[1], y1 = fifo.point_y[1];

    if (x1 > fifo.dst_width || y1 > fifo.dst_height) {
        fifo_complain("a rectangle ran off the destination");
        return;
    }

    u8 *base = translate(fifo.dst, fifo.dst_pitch * fifo.dst_height);
    if (!base) {
        fifo_complain("the destination is not memory this engine can reach");
        return;
    }
    for (u32 y = y0; y < y1; y++) {
        u32 *row = (u32 *)(base + (size_t)y * fifo.dst_pitch);
        for (u32 x = x0; x < x1; x++) row[x] = fifo.colour;
    }

    fifo.draws++;
}

/* The blit, actually performed.
 *
 * A copy is allowed to overlap itself - dragging a window is exactly that -
 * so the direction of the walk matters.  Copying forwards through a region
 * that overlaps ahead of itself overwrites source rows before they are read,
 * which on real hardware is what the overlap flag exists to prevent and here
 * is what makes the model reproduce the same corruption if the flag is wrong.
 */
static void do_blit(void) {
    if (!fifo.dst || !fifo.dst_pitch) {
        fifo_complain("a blit was made with no destination");
        return;
    }
    if (!fifo.src || !fifo.src_pitch) {
        fifo_complain("a blit was made with no source");
        return;
    }
    if (fifo.src_format != NV902D_FORMAT_A8R8G8B8 ||
        fifo.dst_format != NV902D_FORMAT_A8R8G8B8) {
        fifo_complain("a blit was made before both formats were set");
        return;
    }
    if (fifo.src_layout != NV902D_LAYOUT_PITCH ||
        fifo.dst_layout != NV902D_LAYOUT_PITCH) {
        fifo_complain("a blit was made before both layouts were set");
        return;
    }
    if (fifo.blit_du_dx != 1 || fifo.blit_dv_dy != 1) {
        fifo_complain("this model only performs unscaled copies");
        return;
    }
    if (fifo.blit_dx + fifo.blit_w > fifo.dst_width ||
        fifo.blit_dy + fifo.blit_h > fifo.dst_height) {
        fifo_complain("a blit ran off the destination");
        return;
    }
    if (fifo.blit_sx + fifo.blit_w > fifo.src_width ||
        fifo.blit_sy + fifo.blit_h > fifo.src_height) {
        fifo_complain("a blit ran off the source");
        return;
    }

    u8 *dbase = translate(fifo.dst, fifo.dst_pitch * fifo.dst_height);
    u8 *sbase = translate(fifo.src, fifo.src_pitch * fifo.src_height);
    if (!dbase || !sbase) {
        fifo_complain("a blit named memory this engine cannot reach");
        return;
    }

    /* Walk away from the overlap.  When the destination is below the source
     * the rows have to be copied from the bottom up, and likewise within a row
     * when it is to the right. */
    bool overlapping = fifo.safe_overlap && (fifo.dst == fifo.src);
    bool backwards_rows = overlapping && fifo.blit_dy > fifo.blit_sy;
    bool backwards_cols = overlapping && fifo.blit_dx > fifo.blit_sx;

    for (u32 i = 0; i < fifo.blit_h; i++) {
        u32 row = backwards_rows ? fifo.blit_h - 1 - i : i;
        const u32 *sr = (const u32 *)(sbase + (size_t)(fifo.blit_sy + row) * fifo.src_pitch);
        u32 *dr = (u32 *)(dbase + (size_t)(fifo.blit_dy + row) * fifo.dst_pitch);

        for (u32 j = 0; j < fifo.blit_w; j++) {
            u32 col = backwards_cols ? fifo.blit_w - 1 - j : j;
            dr[fifo.blit_dx + col] = sr[fifo.blit_sx + col];
        }
    }

    fifo.blits++;
    fifo.draws++;
}

static void class_method(int subchannel, u32 method, u32 value) {
    fifo.methods++;

    if (method == NV_FIFO_SET_OBJECT) {
        fifo.bound[subchannel & 7] = value;
        return;
    }

    /* The channel's own methods - the semaphore - live in subchannel zero and
     * belong to no class. */
    if (fifo.bound[subchannel & 7] == 0) {
        switch (method) {
        case NV_FIFO_SEMAPHOREA: fifo.semaphore_upper = value; return;
        case NV_FIFO_SEMAPHOREB: fifo.semaphore_lower = value; return;
        case NV_FIFO_SEMAPHOREC: fifo.semaphore_payload = value; return;
        case NV_FIFO_SEMAPHORED: {
            if ((value & 0xF) != (NV_FIFO_SEMAPHORED_RELEASE & 0xF)) return;
            u64 at = ((u64)fifo.semaphore_upper << 32) | fifo.semaphore_lower;
            if (at != fifo.semaphore_gpu) {
                fifo_complain("a semaphore was released to the wrong address");
                return;
            }
            u32 *where = (u32 *)translate(at, 4);
            if (!where) {
                fifo_complain("a semaphore address the engine cannot reach");
                return;
            }
            /* After the drawing, because the release said to wait for the
             * pipeline - which here means it is simply written last. */
            *where = fifo.semaphore_payload;
            return;
        }
        default: return;
        }
    }

    /* A subchannel bound to the drawing class goes to the engine that draws
     * triangles, which is a different engine with a different method set. */
    if (fifo.bound[subchannel & 7] == fifo.three_d_class) {
        nv_3d_model_method(subchannel, method, value);
        return;
    }

    if (fifo.bound[subchannel & 7] != NV_CLASS_TWOD) {
        fifo_complain("a method went to a subchannel bound to nothing this "
                      "engine knows");
        return;
    }

    switch (method) {
    case NV902D_SET_DST_FORMAT:        fifo.dst_format = value; return;
    case NV902D_SET_DST_MEMORY_LAYOUT: fifo.dst_layout = value; return;
    case NV902D_SET_DST_PITCH:         fifo.dst_pitch = value; return;
    case NV902D_SET_DST_WIDTH:         fifo.dst_width = value; return;
    case NV902D_SET_DST_HEIGHT:        fifo.dst_height = value; return;
    case NV902D_SET_DST_OFFSET_UPPER:
        fifo.dst = (fifo.dst & 0xFFFFFFFFull) | ((u64)value << 32);
        return;
    case NV902D_SET_DST_OFFSET_LOWER:
        fifo.dst = (fifo.dst & ~0xFFFFFFFFull) | value;
        return;
    case NV902D_SET_SRC_FORMAT:        fifo.src_format = value; return;
    case NV902D_SET_SRC_MEMORY_LAYOUT: fifo.src_layout = value; return;
    case NV902D_SET_SRC_PITCH:         fifo.src_pitch = value; return;
    case NV902D_SET_SRC_WIDTH:         fifo.src_width = value; return;
    case NV902D_SET_SRC_HEIGHT:        fifo.src_height = value; return;
    case NV902D_SET_SRC_OFFSET_UPPER:
        fifo.src = (fifo.src & 0xFFFFFFFFull) | ((u64)value << 32);
        return;
    case NV902D_SET_SRC_OFFSET_LOWER:
        fifo.src = (fifo.src & ~0xFFFFFFFFull) | value;
        return;

    case NV902D_SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP: fifo.safe_overlap = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE:  return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DST_X0:     fifo.blit_dx = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DST_Y0:     fifo.blit_dy = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DST_WIDTH:  fifo.blit_w  = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DST_HEIGHT: fifo.blit_h  = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC: return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_INT:  fifo.blit_du_dx = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_FRAC: return;
    case NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_INT:  fifo.blit_dv_dy = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC: return;
    case NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_INT: fifo.blit_sx = value; return;
    case NV902D_SET_PIXELS_FROM_MEMORY_SRC_Y0_FRAC: return;
    /* Writing the source's starting row is what starts the blit. */
    case NV902D_PIXELS_FROM_MEMORY_SRC_Y0_INT:
        fifo.blit_sy = value;
        do_blit();
        return;

    case NV902D_SET_OPERATION:               fifo.operation = value; return;
    case NV902D_SET_RENDER_SOLID_PRIM_COLOR: fifo.colour = value; return;
    case NV902D_RENDER_SOLID_PRIM_MODE:
        fifo.prim_mode = value;
        fifo.points = 0;
        return;
    default: break;
    }

    /* The points, and the draw that the second one's y triggers. */
    for (int i = 0; i < 2; i++) {
        if (method == (u32)NV902D_RENDER_SOLID_PRIM_POINT_SET_X(i)) {
            fifo.point_x[i] = value;
            return;
        }
        if (method == (u32)NV902D_RENDER_SOLID_PRIM_POINT_Y(i)) {
            fifo.point_y[i] = value;
            if (i == 1) draw_rectangle();
            return;
        }
    }
}

static void run_push_buffer(u64 at, u32 words) {
    const u32 *buffer = (const u32 *)translate(at, words * 4);
    if (!buffer) {
        fifo_complain("an entry points at memory this engine cannot reach");
        return;
    }
    u32 i = 0;

    while (i < words) {
        u32 header = buffer[i++];
        u32 op = header >> 29;
        u32 method = (header & 0xFFF) << 2;
        int subchannel = (int)((header >> 13) & 7);
        u32 count = (header >> 16) & 0x1FFF;

        fifo.packets++;

        switch (op) {
        case NV_PUSH_OP_IMMD:
            /* The value travelled inside the header. */
            class_method(subchannel, method, count);
            continue;

        case NV_PUSH_OP_INC:
            if (i + count > words) {
                fifo_complain("a packet claims more values than were submitted");
                return;
            }
            for (u32 k = 0; k < count; k++)
                class_method(subchannel, method + k * 4, buffer[i++]);
            continue;

        case NV_PUSH_OP_NINC:
            if (i + count > words) {
                fifo_complain("a packet claims more values than were submitted");
                return;
            }
            for (u32 k = 0; k < count; k++)
                class_method(subchannel, method, buffer[i++]);
            continue;

        case NV_PUSH_OP_1INC:
            if (i + count > words) {
                fifo_complain("a packet claims more values than were submitted");
                return;
            }
            for (u32 k = 0; k < count; k++)
                class_method(subchannel, k ? method + 4 : method, buffer[i++]);
            continue;

        default:
            fifo_complain("a packet header is not one of the encodings");
            return;
        }
    }
}

static void consume_ring(u32 put) {
    if (!fifo.entries_gpu) {
        fifo_complain("the ring was rung before it was armed");
        return;
    }

    const u64 *entries = (const u64 *)translate(fifo.entries_gpu,
                                               fifo.count * 8);
    if (!entries) {
        fifo_complain("the ring is not memory this engine can reach");
        return;
    }

    while (fifo.got != (put & (fifo.count - 1))) {
        u64 entry = entries[fifo.got];
        /* Forty bits of address, which is all the field holds. */
        u64 at = entry & 0x000000FFFFFFFFFFull;
        u32 words = (u32)((entry >> 42) & 0x1FFFFF);

        if (!words) {
            fifo_complain("an entry describes no work");
        } else {
            run_push_buffer(at, words);
        }

        fifo.got = (fifo.got + 1) & (fifo.count - 1);
    }

    /* And report it, which is the only way the driver learns an entry is free. */
    nv_card_t *c = nv_model_card();
    if (c && c->regs)
        *(volatile u32 *)(c->regs + NV_PFIFO_CHANNEL_GP_GET(fifo.chid)) =
            fifo.got;
}

void nv_fifo_model_write(u32 offset, u32 value) {
    if (!fifo.present) return;

    for (int chid = 0; chid < 8; chid++) {
        if (offset == (u32)NV_PFIFO_GPFIFO_BASE_LO(chid)) {
            fifo.entries_gpu = (fifo.entries_gpu & ~0xFFFFFFFFull) | value;
            return;
        }
        if (offset == (u32)NV_PFIFO_GPFIFO_BASE_HI(chid)) {
            fifo.entries_gpu = (fifo.entries_gpu & 0xFFFFFFFFull) |
                               ((u64)value << 32);
            return;
        }
        if (offset == (u32)NV_PFIFO_GPFIFO_SIZE(chid)) {
            fifo.count = value;
            fifo.got = 0;
            fifo.chid = chid;
            return;
        }
        if (offset == (u32)NV_PFIFO_CHANNEL_GP_PUT(chid)) {
            if (fifo.count) consume_ring(value);
            return;
        }
    }
}

void nv_fifo_model_set_3d_class(u32 class_number) {
    fifo.three_d_class = class_number;
}

void nv_fifo_model_attach(u64 semaphore_gpu, u64 gpu_base, u8 *host_base,
                          u32 span) {
    u32 three_d = fifo.three_d_class;
    memset(&fifo, 0, sizeof fifo);
    fifo.three_d_class = three_d;
    fifo.present = true;
    fifo.semaphore_gpu = semaphore_gpu;
    fifo.gpu_base = gpu_base;
    fifo.host_base = host_base;
    fifo.span = span;
}

void nv_fifo_model_detach(void) { fifo.present = false; }

int  nv_fifo_model_packets(void) { return fifo.packets; }
int  nv_fifo_model_methods(void) { return fifo.methods; }
int  nv_fifo_model_draws(void)   { return fifo.draws; }
const char *nv_fifo_model_complaint(void) { return fifo.complaint; }

/* ------------------------------------------------------------------- test */

#define RING_ENTRIES 16
#define PUSH_WORDS   512
#define SURFACE_W    64
#define SURFACE_H    64

int nv_fifo_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) return 0;

    u64 phys = 0;
    u8 *pages = dma_alloc_pages(8, &phys);
    if (!pages) {
        kerr("nv-fifo", "no memory for a channel");
        return 1;
    }
    memset(pages, 0, PAGE_SIZE * 8);

    /* The ring, the push buffer, the semaphore and the surface, laid out in
     * one region.  The model follows the same addresses the driver hands the
     * card, so what it draws into is what is checked below. */
    u64 *ring = (u64 *)pages;
    u32 *push = (u32 *)(pages + PAGE_SIZE);
    volatile u32 *semaphore = (volatile u32 *)(pages + PAGE_SIZE * 3);
    u32 *surface = (u32 *)(pages + PAGE_SIZE * 4);

    /* What the engine is told is a graphics address.  Here that is the
     * physical address, which is what a card sees when nothing is translating
     * for it - and it is what the entry format can actually carry. */
    u64 ring_gpu = phys;
    u64 push_gpu = phys + PAGE_SIZE;
    u64 semaphore_gpu = phys + PAGE_SIZE * 3;
    u64 surface_gpu = phys + PAGE_SIZE * 4;

    nv_fifo_model_attach(semaphore_gpu, phys, pages, PAGE_SIZE * 8);

    static nv_fifo_t f;
    if (!nv_fifo_init(c, &f, 0, ring, ring_gpu, RING_ENTRIES,
                      push, push_gpu, PUSH_WORDS, semaphore, semaphore_gpu)) {
        kerr("nv-fifo", "the channel would not come up");
        nv_fifo_model_detach();
        return 1;
    }

    /* A method sent to a subchannel that was never bound goes nowhere, which
     * is what the engine does - so the drawing call refuses rather than
     * building a stream that silently does nothing. */
    if (nv_2d_fill(c, &f, 1, surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                   0, 0, 8, 8, 0xFFFFFFFF)) {
        kerr("nv-fifo", "a drawing call was built for an unbound subchannel");
        failures++;
    }

    if (!nv_fifo_bind(c, &f, 1, NV_CLASS_TWOD)) {
        kerr("nv-fifo", "the drawing class would not bind");
        failures++;
    }

    /* One rectangle, and a fence that says when it is done. */
    if (!nv_2d_fill(c, &f, 1, surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                    8, 4, 16, 12, 0x00FF8040)) {
        kerr("nv-fifo", "the rectangle was not built");
        failures++;
    }
    nv_fifo_fence(c, &f, 1);

    if (!nv_fifo_submit(c, &f)) {
        kerr("nv-fifo", "the work was not submitted");
        failures++;
    } else if (!nv_fifo_wait(&f, 1, 50)) {
        kerr("nv-fifo", "the fence never landed");
        failures++;
    }

    /* And the point of all of it: the right pixels changed. */
    {
        int wrong = 0, filled = 0;
        for (u32 y = 0; y < SURFACE_H; y++) {
            for (u32 x = 0; x < SURFACE_W; x++) {
                u32 got = surface[y * SURFACE_W + x];
                bool inside = x >= 8 && x < 24 && y >= 4 && y < 16;
                if (inside) {
                    if (got == 0x00FF8040) filled++;
                    else wrong++;
                } else if (got != 0) {
                    wrong++;
                }
            }
        }
        if (wrong || filled != 16 * 12) {
            kerr("nv-fifo", "%d pixel(s) wrong and %d of %d filled", wrong,
                 filled, 16 * 12);
            failures++;
        } else {
            kinfo("nv-fifo", "the engine drew it: %d pixels of a 16x12 "
                             "rectangle at 8,4, and nothing outside it", filled);
        }
    }

    /* A packet whose declared count does not match what was written has to be
     * caught here, because the card would read every packet after it from the
     * wrong place. */
    {
        u32 before = f.push_at;
        nv_push_begin(&f, 1, NV902D_SET_DST_PITCH, 3);
        nv_push_data(&f, 1);
        nv_push_data(&f, 2);
        if (nv_push_end(&f)) {
            kerr("nv-fifo", "a packet that declared 3 values and wrote 2 was "
                            "accepted");
            failures++;
        } else if (f.push_at != before) {
            kerr("nv-fifo", "the short packet was rejected but left %u words "
                            "behind", f.push_at - before);
            failures++;
        }
    }

    /* A rectangle that runs off the surface: refused rather than drawn over
     * whatever is next in memory. */
    if (nv_2d_fill(c, &f, 1, surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                   56, 56, 16, 16, 0xFFFFFFFF)) {
        kerr("nv-fifo", "a rectangle that runs off the surface was accepted");
        failures++;
    }

    /* Several more, going round the ring, each with its own fence - which is
     * where a pointer treated as bytes rather than entries comes apart. */
    int laps = 0;
    for (int i = 0; i < 40; i++) {
        f.push_at = 0;
        f.submitted_to = 0;
        u32 colour = 0x00101010u * (u32)(i + 1);
        if (!nv_2d_fill(c, &f, 1, surface_gpu, SURFACE_W * 4, SURFACE_W,
                        SURFACE_H, 0, 0, 4, 4, colour)) break;
        if (!nv_fifo_fence(c, &f, (u32)(i + 2))) break;
        if (!nv_fifo_submit(c, &f)) break;
        if (!nv_fifo_wait(&f, (u32)(i + 2), 50)) break;
        if (surface[0] != colour) break;
        laps++;
    }
    if (laps != 40) {
        kerr("nv-fifo", "only %d of 40 submissions went round the ring", laps);
        failures++;
    }

    if (nv_fifo_model_complaint()[0]) {
        kerr("nv-fifo", "the engine complained: %s", nv_fifo_model_complaint());
        failures++;
    }

    int packets = nv_fifo_model_packets();
    int methods = nv_fifo_model_methods();
    /* --- copies, including the overlapping kind ---------------------------
     *
     * A window dragged across the screen copies a surface onto itself, and the
     * region it moves into is usually part of the region it moves out of.  A
     * copy that walks the wrong way through that smears the first row down the
     * whole rectangle, which is a bug that only appears once something is
     * actually dragged - so it is worth catching here instead. */
    {
        /* A recognisable pattern, so a wrong copy is obvious rather than
         * plausible. */
        for (u32 y = 0; y < SURFACE_H; y++)
            for (u32 x = 0; x < SURFACE_W; x++)
                surface[y * SURFACE_W + x] = (y << 16) | x;

        /* Move an 8x8 block down and right by four, which overlaps itself. */
        if (!nv_2d_copy(c, &f, 1,
                        surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                        surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                        8, 8, 4, 4, 8, 8)) {
            kerr("nv-fifo", "the overlapping copy was not built");
            failures++;
        }
        nv_fifo_fence(c, &f, 2);
        if (!nv_fifo_submit(c, &f) || !nv_fifo_wait(&f, 2, 50)) {
            kerr("nv-fifo", "the copy never completed");
            failures++;
        }

        int wrong = 0;
        for (u32 dy = 0; dy < 8; dy++) {
            for (u32 dx = 0; dx < 8; dx++) {
                u32 want = ((4 + dy) << 16) | (4 + dx);
                if (surface[(8 + dy) * SURFACE_W + (8 + dx)] != want) wrong++;
            }
        }
        if (wrong) {
            kerr("nv-fifo", "%d pixel(s) of the overlapping copy landed wrong - "
                            "the engine walked into its own source", wrong);
            failures++;
        }

        /* A copy that leaves the surface must be refused rather than clamped:
         * silently drawing something smaller than asked for is worse than
         * saying no. */
        if (nv_2d_copy(c, &f, 1,
                       surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                       surface_gpu, SURFACE_W * 4, SURFACE_W, SURFACE_H,
                       SURFACE_W - 2, 0, 0, 0, 8, 8)) {
            kerr("nv-fifo", "a copy running off the surface was accepted");
            failures++;
        }
    }

    int draws = nv_fifo_model_draws();
    nv_fifo_model_detach();

    if (!failures)
        kinfo("nv-fifo", "work is submitted and done: %d packets carrying %d "
                         "methods across %d submissions, %d rectangles drawn "
                         "into memory, each reported by its own semaphore, and "
                         "a packet whose length was wrong refused",
              packets, methods, f.submissions, draws);
    return failures;
}
