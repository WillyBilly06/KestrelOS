/* nv_disp.c - the display engine, on the cards that have one you can reach.
 *
 * Up to and including Pascal, an NVIDIA card's display engine is driven by a
 * command channel: the driver writes a stream of method-and-value pairs into a
 * ring in memory, and moves a pointer register to say how far it has written.
 * The engine reads them and applies them all at once when it sees the update
 * method - so a mode change is atomic, and a frame is never shown half in the
 * old mode and half in the new.
 *
 * That model is why this is a channel rather than a set of registers: setting a
 * mode means changing a dozen things together, and there is no order in which
 * they can be written one at a time without the display tearing or blanking.
 *
 * From Turing onward this engine is behind the co-processor instead, and this
 * file does not apply.  nv_gsp.c is that half.
 *
 * ---------------------------------------------------------------------------
 * On testing.  Nothing available has an NVIDIA card, so this runs against the
 * model in nv_model.c, which reads back the channel the driver builds.  That
 * establishes the channel mechanics, the method encoding and the ordering; it
 * cannot establish that the method numbers are the ones the engine uses.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

/* ------------------------------------------------------------ the channel
 *
 * A push buffer is an array of 32-bit words.  A header word says which method
 * to start at, how many values follow and whether they go to consecutive
 * methods or all to the same one; the values follow it.
 */
#define NV_PUSH_WORDS 1024

typedef struct {
    u32 *buffer;
    u64  buffer_phys;
    u32  put;              /* how far the driver has written                */
    int  channel;
} nv_push_t;

/* The header that introduces a run of values.  The encoding has been the same
 * since NV50: the method number is stored divided by four, because methods are
 * always word-aligned and dropping the low bits leaves room for the count. */
static u32 push_header(u32 method, u32 count, bool same_method) {
    return (count << 18) | (same_method ? (1u << 16) : (2u << 16)) | (method >> 2);
}

static void push_method(nv_push_t *p, u32 method, u32 value) {
    if (p->put + 2 > NV_PUSH_WORDS) return;
    p->buffer[p->put++] = push_header(method, 1, false);
    p->buffer[p->put++] = value;
}

/* Ringing the doorbell.  The engine reads up to here and stops. */
static void push_kick(nv_card_t *c, nv_push_t *p) {
    nv_wr32(c, NV_PDISP_CHAN_PUT(p->channel), p->put * 4);
}

/* Wait for the engine to catch up, so the buffer can be reused. */
static bool push_wait(nv_card_t *c, nv_push_t *p, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 10; i++) {
        u32 got = nv_rd32(c, NV_PDISP_CHAN_GET(p->channel));
        if (got >= p->put * 4) return true;
        timer_udelay(100);
    }
    return false;
}

/* --------------------------------------------------------------- the methods
 *
 * The core channel's methods, as they have been numbered since NV50.  Only the
 * ones a mode change needs are named.
 */
#define NV_CORE_UPDATE               0x0080  /* apply everything written     */
#define NV_CORE_HEAD_OFFSET          0x0400  /* per-head block, 0x300 apart  */
#define NV_CORE_HEAD_STRIDE          0x0300

#define NV_HEAD_SET_PIXEL_CLOCK      0x0000
#define NV_HEAD_SET_CONTROL_OUTPUT   0x0004
#define NV_HEAD_SET_OVERSCAN_COLOR   0x0008
#define NV_HEAD_SET_RASTER_SIZE      0x000C
#define NV_HEAD_SET_RASTER_SYNC_END  0x0010
#define NV_HEAD_SET_RASTER_BLANK_END 0x0014
#define NV_HEAD_SET_RASTER_BLANK_START 0x0018
#define NV_HEAD_SET_VIEWPORT_SIZE    0x0020
#define NV_HEAD_SET_SURFACE_ADDRESS  0x0030
#define NV_HEAD_SET_SURFACE_FORMAT   0x0034
#define NV_HEAD_SET_SURFACE_PITCH    0x0038

#define NV_SURFACE_FORMAT_A8R8G8B8   0xCF

/* --------------------------------------------------------------- the timing
 *
 * A display mode is not a width and a height: it is a rectangle of visible
 * pixels inside a larger rectangle that includes the intervals the beam used
 * to need to move back.  Those intervals are still there, and a monitor
 * identifies a mode by them, so they have to be right.
 */
typedef struct {
    u32 pixel_clock_khz;
    u16 hactive, hblank, hsync_offset, hsync_width;
    u16 vactive, vblank, vsync_offset, vsync_width;
} nv_mode_t;

/* The timing a monitor asked for, out of its identification block. */
static bool mode_from_edid(const u8 *edid, nv_mode_t *out) {
    const u8 *d = edid + 54;
    u32 clock = (u32)((d[1] << 8) | d[0]) * 10u;      /* in kilohertz */
    if (!clock) return false;

    out->pixel_clock_khz = clock;
    out->hactive = (u16)(d[2] | ((d[4] & 0xF0) << 4));
    out->hblank  = (u16)(d[3] | ((d[4] & 0x0F) << 8));
    out->vactive = (u16)(d[5] | ((d[7] & 0xF0) << 4));
    out->vblank  = (u16)(d[6] | ((d[7] & 0x0F) << 8));
    out->hsync_offset = (u16)(d[8] | ((d[11] & 0xC0) << 2));
    out->hsync_width  = (u16)(d[9] | ((d[11] & 0x30) << 4));
    out->vsync_offset = (u16)((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    out->vsync_width  = (u16)((d[10] & 0x0F) | ((d[11] & 0x03) << 4));

    return out->hactive && out->vactive;
}

/* ------------------------------------------------------------- setting one */

static nv_push_t core_push;
static u32 push_storage[NV_PUSH_WORDS] __attribute__((aligned(4096)));

/* Build the whole mode change into the channel, then let it go in one piece.
 * Everything between the first method and the update is applied at the same
 * moment, which is what stops the display showing an intermediate state. */
static bool set_mode(nv_card_t *c, int head, const nv_mode_t *m,
                     u64 surface, u32 pitch) {
    nv_push_t *p = &core_push;
    p->put = 0;

    u32 base = NV_CORE_HEAD_OFFSET + (u32)head * NV_CORE_HEAD_STRIDE;

    push_method(p, base + NV_HEAD_SET_PIXEL_CLOCK, m->pixel_clock_khz);

    /* The total rectangle, then where inside it the sync pulse and the visible
     * area sit.  Each is packed as height in the high half and width in the
     * low, which is how the engine takes them. */
    u32 htotal = (u32)m->hactive + m->hblank;
    u32 vtotal = (u32)m->vactive + m->vblank;
    push_method(p, base + NV_HEAD_SET_RASTER_SIZE, (vtotal << 16) | htotal);

    u32 hsync_end = m->hsync_offset + m->hsync_width;
    u32 vsync_end = m->vsync_offset + m->vsync_width;
    push_method(p, base + NV_HEAD_SET_RASTER_SYNC_END, (vsync_end << 16) | hsync_end);
    push_method(p, base + NV_HEAD_SET_RASTER_BLANK_END,
                ((u32)m->vblank << 16) | m->hblank);
    push_method(p, base + NV_HEAD_SET_RASTER_BLANK_START,
                (((u32)m->vactive + m->vblank) << 16) | ((u32)m->hactive + m->hblank));

    push_method(p, base + NV_HEAD_SET_VIEWPORT_SIZE,
                ((u32)m->vactive << 16) | m->hactive);

    /* Where the pixels are read from.  The address is in units of 256 bytes,
     * which is the alignment the engine requires anyway. */
    push_method(p, base + NV_HEAD_SET_SURFACE_ADDRESS, (u32)(surface >> 8));
    push_method(p, base + NV_HEAD_SET_SURFACE_FORMAT, NV_SURFACE_FORMAT_A8R8G8B8);
    push_method(p, base + NV_HEAD_SET_SURFACE_PITCH, pitch);

    push_method(p, base + NV_HEAD_SET_OVERSCAN_COLOR, 0);
    push_method(p, base + NV_HEAD_SET_CONTROL_OUTPUT, 1);      /* switched on */

    /* And apply the lot. */
    push_method(p, NV_CORE_UPDATE, 0);
    push_kick(c, p);

    if (!push_wait(c, p, 100)) {
        kwarn("nvidia", "the display engine did not finish the mode change");
        return false;
    }

    kinfo("nvidia", "head %d set to %ux%u, pixel clock %u.%03u MHz",
          head, m->hactive, m->vactive,
          m->pixel_clock_khz / 1000, m->pixel_clock_khz % 1000);
    return true;
}

/* --------------------------------------------------------------- bringing up */

bool nv_display_init(nv_card_t *c) {
    /* The engine has to be switched on before its channel answers. */
    u32 enable = nv_rd32(c, NV_PMC_ENABLE);
    nv_wr32(c, NV_PMC_ENABLE, enable | NV_PMC_ENABLE_PDISP);

    core_push.buffer = push_storage;
    core_push.buffer_phys = virt_to_phys(push_storage);
    core_push.channel = 0;
    core_push.put = 0;

    /* Which connector has something on it decides which head is worth setting
     * up; a card with four sockets and one monitor should not light four. */
    int lit = 0;
    for (int i = 0; i < c->outputs && lit < 2; i++) {
        nv_output_t *out = &c->output[i];
        if (!out->monitor_present || !out->edid_valid) continue;

        nv_mode_t mode;
        if (!mode_from_edid(out->edid, &mode)) {
            kwarn("nvidia", "%s: the monitor's preferred timing could not be read",
                  nv_output_type_name(out->type));
            continue;
        }

        kinfo("nvidia", "%s: \"%s\" wants %ux%u at %u.%03u MHz",
              nv_output_type_name(out->type), out->monitor_name,
              mode.hactive, mode.vactive,
              mode.pixel_clock_khz / 1000, mode.pixel_clock_khz % 1000);

        /* The surface is the framebuffer the machine's own firmware set up.
         * Re-pointing the head at somewhere else would work, but there is
         * nothing else there yet - and taking over a working display to show
         * nothing would be a poor trade. */
        u64 surface = 0;
        u32 pitch = (u32)mode.hactive * 4;

        if (set_mode(c, lit, &mode, surface, pitch)) lit++;
    }

    if (!lit) {
        kinfo("nvidia", "no monitor to drive, so the display engine was left as "
                        "the firmware set it up");
        return false;
    }
    return true;
}

/* What the driver built, so a test can read it back. */
const u32 *nv_disp_push_buffer(u32 *words_out) {
    if (words_out) *words_out = core_push.put;
    return push_storage;
}
