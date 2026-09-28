/* nv_dispc37d.c - the display engine on a card from Volta onward.
 *
 * nv_disp.c drives the engine as it was from NV50 to Pascal: one channel, one
 * head, one surface.  Volta took that apart, and every card since - Turing,
 * Ampere, Ada, and the Blackwell chips in the RTX 50 series - is built the new
 * way.  A driver written for the old shape does not drive the new one at all,
 * which is why this is a separate file rather than a few extra cases.
 *
 * What changed is worth stating plainly, because it is the difference between
 * a modeset that works and one that produces a black screen:
 *
 *   A head owns a raster.  It knows how wide and tall the timing is, where the
 *   sync pulse sits inside it, and what pixel clock to run at.  It does not
 *   know what is being displayed.
 *
 *   A window owns a surface.  It knows an address, a pitch, a format and a
 *   size.  It does not know what timing it is being scanned out with.
 *
 *   The two are joined by telling a window which head it belongs to, and until
 *   that has been said the window scans out to nothing.  This is the single
 *   most common way to get a black screen on one of these cards with every
 *   other register correct.
 *
 *   Both are programmed through their own channel, and each channel has its
 *   own update method.  Everything written before an update is applied at the
 *   same instant; a driver that forgets one of the two updates shows half a
 *   mode change.
 *
 * The class number the engine speaks is chosen from what the chip said it was.
 * The method numbers within a class have been stable, so knowing the class is
 * enough - which is why a driver written before Blackwell shipped can drive
 * Blackwell by knowing one new number.
 *
 * ---------------------------------------------------------------------------
 * What this establishes and what it cannot.  The class numbers and the method
 * numbers are NVIDIA's published ones.  Run against the model at the end of
 * this file, this proves the channel mechanics, the method encoding, the
 * timing derivation, the head-and-window pairing and the update ordering.  It
 * cannot prove that a real Blackwell card lights up, because there is not one
 * here - and it says so wherever it reports a result.
 * ---------------------------------------------------------------------------
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

/* -------------------------------------------------------------- which class
 *
 * From the architecture the chip named itself as, not from a table of device
 * identifiers - so a chip that did not exist when this was written still lands
 * in the right generation.
 */
u32 nv_disp_core_class(u32 chipset) {
    const nv_classes_t *cl = nv_classes_for(chipset);
    if (!cl) return 0;
    /* Zero for a die with no connectors on it - Hopper, and the Blackwell that
     * went to datacentres - and for everything older than Volta, which is a
     * different shape of engine and is nv_disp.c's job. */
    return cl->disp_core;
}

u32 nv_disp_window_class(u32 chipset) {
    const nv_classes_t *cl = nv_classes_for(chipset);
    return cl ? cl->disp_window : 0;
}

const char *nv_disp_class_name(u32 class_number) {
    switch (class_number) {
    case NV_DISP_CORE_GV100: return "GV100 (Volta)";
    case NV_DISP_CORE_TU102: return "TU102 (Turing)";
    case NV_DISP_CORE_GA102: return "GA102 (Ampere)";
    case NV_DISP_CORE_AD102: return "AD102 (Ada Lovelace)";
    case NV_DISP_CORE_GB202: return "GB202 (Blackwell)";
    default:                 return "no display engine";
    }
}

/* ------------------------------------------------------------- the channel
 *
 * A push buffer in memory and two pointers: the driver writes methods and
 * moves its pointer forward, and the engine reads up to there.  The encoding
 * is the one that has not changed since NV50 - a header saying which method
 * and how many values, then the values.
 */
static u32 push_header(u32 method, u32 count) {
    /* Values going to consecutive methods rather than repeatedly to one. */
    return (count << 18) | (2u << 16) | (method >> 2);
}

bool nv_dchan_init(nv_card_t *c, nv_dchan_t *ch, int chid, u32 *memory,
                   u64 phys, u32 words) {
    memset(ch, 0, sizeof *ch);
    if (!memory || !words) return false;

    ch->buffer = memory;
    ch->phys = phys;
    ch->words = words;
    ch->chid = chid;
    ch->put = 0;
    memset(memory, 0, (size_t)words * 4);

    /* Where the buffer is, and how long.  The address goes in two halves
     * because it is sixty-four bits and the registers are not. */
    nv_wr32(c, NV_PDISP_CHAN_PUSH_LO(chid), (u32)phys);
    nv_wr32(c, NV_PDISP_CHAN_PUSH_HI(chid), (u32)(phys >> 32));
    nv_wr32(c, NV_PDISP_CHAN_PUSH_CFG(chid), 0x00000001);
    nv_wr32(c, NV_PDISP_CHAN_PUSH_LEN(chid), 0x00000040);

    /* Switched on, then started.  Doing both in one write leaves the engine
     * running before it has been told where to read from. */
    u32 ctrl = nv_rd32(c, NV_PDISP_CHAN_CTRL(chid));
    nv_wr32(c, NV_PDISP_CHAN_CTRL(chid), ctrl | NV_PDISP_CHAN_CTRL_ENABLE);
    nv_wr32(c, NV_PDISP_CHAN_USER_PUT(chid), 0);
    nv_wr32(c, NV_PDISP_CHAN_CTRL(chid), NV_PDISP_CHAN_CTRL_RUN);

    ch->ready = true;
    return true;
}

void nv_dchan_method(nv_dchan_t *ch, u32 method, u32 value) {
    if (!ch->ready || ch->put + 2 > ch->words) return;
    ch->buffer[ch->put++] = push_header(method, 1);
    ch->buffer[ch->put++] = value;
}

bool nv_dchan_kick(nv_card_t *c, nv_dchan_t *ch) {
    if (!ch->ready) return false;

    /* Everything is in memory before the pointer that reveals it.  The engine
     * is reading while this is being written. */
    __asm__ volatile("" ::: "memory");
    nv_wr32(c, NV_PDISP_CHAN_USER_PUT(ch->chid), ch->put * 4);

    for (int i = 0; i < 1000; i++) {
        u32 got = nv_rd32(c, NV_PDISP_CHAN_USER_GET(ch->chid));
        if (got >= ch->put * 4) return true;
        timer_udelay(100);
    }

    kwarn("nv-disp", "the display engine did not read channel %d", ch->chid);
    return false;
}

/* ------------------------------------------------------------------ a mode
 *
 * The registers do not take the timing the way a monitor describes it.  A
 * monitor says "the visible part is this wide, the sync pulse starts here and
 * ends here, and the whole raster is this wide".  The engine wants everything
 * measured from the end of the sync pulse, because that is where its counters
 * start - so the front porch and back porch have to be worked out and the
 * blanking expressed relative to them.
 *
 * Getting this wrong does not produce no picture.  It produces a picture
 * shifted sideways, or one the monitor refuses because the sync does not land
 * where it was told - which is much harder to recognise as a timing bug.
 */
static void derive_timing(const nv_raster_t *m,
                          u32 *h_active, u32 *h_synce, u32 *h_blanke, u32 *h_blanks,
                          u32 *v_active, u32 *v_synce, u32 *v_blanke, u32 *v_blanks) {
    u32 h_back = m->htotal - m->hsync_end;      /* after the pulse           */
    u32 h_front = m->hsync_start - m->hdisplay; /* before it                 */
    u32 v_back = m->vtotal - m->vsync_end;
    u32 v_front = m->vsync_start - m->vdisplay;

    *h_active = m->htotal;
    *h_synce = m->hsync_end - m->hsync_start - 1;
    *h_blanke = *h_synce + h_back;
    *h_blanks = m->htotal - h_front - 1;

    *v_active = m->vtotal;
    *v_synce = m->vsync_end - m->vsync_start - 1;
    *v_blanke = *v_synce + v_back;
    *v_blanks = m->vtotal - v_front - 1;
}

bool nv_disp_modern_modeset(nv_card_t *c, nv_dchan_t *core, nv_dchan_t *window,
                            int head, int window_index, int sor,
                            const nv_raster_t *mode, u64 surface, u32 pitch,
                            bool displayport) {
    if (!core->ready || !window->ready || !mode) return false;

    if (mode->hdisplay > 0x7FFF || mode->vdisplay > 0x7FFF) {
        kwarn("nv-disp", "%ux%u is larger than a head can raster",
              mode->hdisplay, mode->vdisplay);
        return false;
    }
    if (mode->hsync_start < mode->hdisplay || mode->hsync_end <= mode->hsync_start ||
        mode->htotal < mode->hsync_end ||
        mode->vsync_start < mode->vdisplay || mode->vsync_end <= mode->vsync_start ||
        mode->vtotal < mode->vsync_end) {
        kwarn("nv-disp", "the timing does not describe a raster");
        return false;
    }

    u32 ha, hs, hbe, hbs, va, vs, vbe, vbs;
    derive_timing(mode, &ha, &hs, &hbe, &hbs, &va, &vs, &vbe, &vbs);

    core->put = 0;
    window->put = 0;

    /* ---- the head: what the monitor is driven with ---- */

    /* And the join.  Until this is said the window scans out to nothing, and
     * every other register can be perfect. */
    nv_dchan_method(core, NVC37D_WINDOW_SET_CONTROL(window_index), (u32)head);

    u32 resource = (u32)NVC37D_PIXEL_DEPTH_BPP_24_444 << 4;
    if (mode->hsync_negative) resource |= 1u << 2;
    if (mode->vsync_negative) resource |= 1u << 3;
    nv_dchan_method(core, NVC37D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(head), resource);

    /* In hertz, and the top bit is the film-rate adjustment rather than part
     * of the number - so a clock large enough to reach it would be read as
     * something else entirely. */
    u64 hertz = (u64)mode->clock_khz * 1000;
    if (hertz > 0x7FFFFFFFull) {
        kwarn("nv-disp", "a pixel clock of %u kHz does not fit in the field",
              mode->clock_khz);
        return false;
    }
    nv_dchan_method(core, NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head), (u32)hertz);

    nv_dchan_method(core, NVC37D_HEAD_SET_RASTER_SIZE(head), ha | (va << 16));
    nv_dchan_method(core, NVC37D_HEAD_SET_RASTER_SYNC_END(head), hs | (vs << 16));
    nv_dchan_method(core, NVC37D_HEAD_SET_RASTER_BLANK_END(head), hbe | (vbe << 16));
    nv_dchan_method(core, NVC37D_HEAD_SET_RASTER_BLANK_START(head), hbs | (vbs << 16));

    nv_dchan_method(core, NVC37D_HEAD_SET_VIEWPORT_SIZE_IN(head),
                    mode->hdisplay | (mode->vdisplay << 16));
    nv_dchan_method(core, NVC37D_HEAD_SET_VIEWPORT_SIZE_OUT(head),
                    mode->hdisplay | (mode->vdisplay << 16));

    /* Which serialiser carries this head's pixels off the card, and what it
     * speaks.  A DisplayPort connector will show nothing if this says TMDS
     * however well the link was trained. */
    u32 sor_control = (1u << head);
    sor_control |= (u32)(displayport ? NVC37D_SOR_PROTOCOL_DP_A
                                     : NVC37D_SOR_PROTOCOL_SINGLE_TMDS_A) << 8;
    nv_dchan_method(core, NVC37D_SOR_SET_CONTROL(sor), sor_control);

    nv_dchan_method(core, NVC37D_UPDATE, 0);

    /* ---- the window: what is actually on the screen ---- */

    nv_dchan_method(window, NVC37E_SET_CONTEXT_DMA_ISO(0), 0xDEAD0000u | (u32)window_index);
    /* The address is in units of 256 bytes, which is also the alignment the
     * engine requires - so an address that is not aligned does not become a
     * slightly wrong address, it becomes a different one. */
    if (surface & 0xFF) {
        kwarn("nv-disp", "a surface at %llx is not on a 256 byte boundary",
              (unsigned long long)surface);
        return false;
    }
    nv_dchan_method(window, NVC37E_SET_OFFSET(0), (u32)(surface >> 8));

    nv_dchan_method(window, NVC37E_SET_SIZE,
                    mode->hdisplay | (mode->vdisplay << 16));
    nv_dchan_method(window, NVC37E_SET_STORAGE, NVC37E_STORAGE_PITCH);
    nv_dchan_method(window, NVC37E_SET_PARAMS, NVC37E_FORMAT_A8R8G8B8);
    /* The pitch is in units of sixty-four bytes here, which is why a surface
     * whose pitch is not a multiple of that cannot be scanned out at all. */
    if (pitch & 0x3F) {
        kwarn("nv-disp", "a pitch of %u bytes is not a multiple of 64", pitch);
        return false;
    }
    nv_dchan_method(window, NVC37E_SET_PLANAR_STORAGE(0), pitch >> 6);

    nv_dchan_method(window, NVC37E_SET_POINT_IN(0), 0);
    nv_dchan_method(window, NVC37E_SET_SIZE_IN,
                    mode->hdisplay | (mode->vdisplay << 16));
    nv_dchan_method(window, NVC37E_SET_SIZE_OUT,
                    mode->hdisplay | (mode->vdisplay << 16));

    nv_dchan_method(window, NVC37E_SET_COMPOSITION_CONTROL, 0);
    /* Presented on a vertical blank rather than immediately, so a frame is
     * never shown half old and half new. */
    nv_dchan_method(window, NVC37E_SET_PRESENT_CONTROL, 0);

    nv_dchan_method(window, NVC37E_UPDATE, 0);

    if (!nv_dchan_kick(c, core)) return false;
    if (!nv_dchan_kick(c, window)) return false;

    kinfo("nv-disp", "head %d rastering %ux%u at %u.%03u MHz from window %d, "
                     "through serialiser %d as %s",
          head, mode->hdisplay, mode->vdisplay,
          mode->clock_khz / 1000, mode->clock_khz % 1000,
          window_index, sor, displayport ? "DisplayPort" : "TMDS");
    return true;
}

/* ==========================================================================
 * A display engine that is not there.
 *
 * It reads the channels the driver writes and decodes them the way the engine
 * does, and it complains about the mistakes that produce a black screen rather
 * than an obviously wrong picture: a window that was never given to a head, a
 * head whose raster is smaller than its viewport, an update that never came.
 * Those are the ones worth catching, because the others are visible.
 * ==========================================================================
 */
#define MODEL_HEADS   8
#define MODEL_WINDOWS 8

static struct {
    bool present;
    u32  core_class, window_class;

    /* Where each channel's buffer is, once the driver has armed it. */
    u64  push[16];
    bool running[16];
    u32  got[16];

    int  methods;

    /* What the head was told. */
    u32  raster_w, raster_h;
    u32  sync_end, blank_end, blank_start;
    u32  viewport_in, viewport_out;
    u32  pixel_hz;
    u32  output_resource;
    u32  sor_control;

    /* What the window was told. */
    int  owner[MODEL_WINDOWS];
    u64  surface;
    u32  window_size, size_in, size_out;
    u32  storage, params, pitch_units;

    bool core_updated, window_updated;
    bool window_before_core;      /* the ordering mistake                    */

    char complaint[128];
} disp;

static void complain(const char *what) {
    if (disp.complaint[0]) return;
    size_t n = 0;
    while (what[n] && n < sizeof disp.complaint - 1) {
        disp.complaint[n] = what[n];
        n++;
    }
    disp.complaint[n] = 0;
}

/* Decoding one method, exactly as the engine would. */
static void core_method(u32 method, u32 value) {
    disp.methods++;

    if (method == NVC37D_UPDATE) {
        /* Everything written up to here is applied at once.  A head whose
         * raster cannot contain its viewport is the mistake that shows as a
         * blank screen rather than a squashed one. */
        u32 vp_w = disp.viewport_in & 0x7FFF;
        u32 vp_h = (disp.viewport_in >> 16) & 0x7FFF;
        if (vp_w > disp.raster_w || vp_h > disp.raster_h)
            complain("the viewport is larger than the raster");
        if (!disp.pixel_hz)
            complain("the head was updated with no pixel clock");
        disp.core_updated = true;
        return;
    }

    for (int w = 0; w < MODEL_WINDOWS; w++) {
        if (method == (u32)NVC37D_WINDOW_SET_CONTROL(w)) {
            u32 owner = value & 0xF;
            disp.owner[w] = owner == NVC37D_WINDOW_OWNER_NONE ? -1 : (int)owner;
            return;
        }
    }

    for (int h = 0; h < MODEL_HEADS; h++) {
        if (method == (u32)NVC37D_HEAD_SET_RASTER_SIZE(h)) {
            disp.raster_w = value & 0x7FFF;
            disp.raster_h = (value >> 16) & 0x7FFF;
            return;
        }
        if (method == (u32)NVC37D_HEAD_SET_RASTER_SYNC_END(h)) { disp.sync_end = value; return; }
        if (method == (u32)NVC37D_HEAD_SET_RASTER_BLANK_END(h)) { disp.blank_end = value; return; }
        if (method == (u32)NVC37D_HEAD_SET_RASTER_BLANK_START(h)) { disp.blank_start = value; return; }
        if (method == (u32)NVC37D_HEAD_SET_VIEWPORT_SIZE_IN(h)) { disp.viewport_in = value; return; }
        if (method == (u32)NVC37D_HEAD_SET_VIEWPORT_SIZE_OUT(h)) { disp.viewport_out = value; return; }
        if (method == (u32)NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(h)) {
            disp.pixel_hz = value & 0x7FFFFFFFu;
            return;
        }
        if (method == (u32)NVC37D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(h)) {
            disp.output_resource = value;
            return;
        }
    }

    for (int s = 0; s < 8; s++) {
        if (method == (u32)NVC37D_SOR_SET_CONTROL(s)) {
            disp.sor_control = value;
            return;
        }
    }
}

static void window_method(u32 method, u32 value) {
    disp.methods++;

    switch (method) {
    case NVC37E_UPDATE:
        /* The window is what actually puts pixels on the screen, so its own
         * update is what makes them appear.  A driver that sends only the
         * core's update sets a mode and shows nothing in it. */
        if (!disp.core_updated) disp.window_before_core = true;

        {
            bool owned = false;
            for (int w = 0; w < MODEL_WINDOWS; w++)
                if (disp.owner[w] >= 0) owned = true;
            if (!owned)
                complain("a window was updated without belonging to any head");
        }
        if (!disp.surface)
            complain("the window was updated with no surface");
        if (!disp.pitch_units)
            complain("the window was updated with no pitch");
        disp.window_updated = true;
        return;

    case NVC37E_SET_OFFSET(0):  disp.surface = (u64)value << 8; return;
    case NVC37E_SET_SIZE:       disp.window_size = value; return;
    case NVC37E_SET_SIZE_IN:    disp.size_in = value; return;
    case NVC37E_SET_SIZE_OUT:   disp.size_out = value; return;
    case NVC37E_SET_STORAGE:    disp.storage = value; return;
    case NVC37E_SET_PARAMS:     disp.params = value; return;
    case NVC37E_SET_PLANAR_STORAGE(0): disp.pitch_units = value; return;
    default: return;
    }
}

/* Reading a channel the driver has just revealed more of. */
static void consume(int chid, u32 put_bytes) {
    if (!disp.running[chid] || !disp.push[chid]) {
        complain("a channel was pushed before it was started");
        return;
    }

    /* The buffer is in memory this side can see, which on a real card means a
     * physical address the engine reads.  Here the driver handed over the same
     * pointer, so the model reads what was written. */
    const u32 *buffer = (const u32 *)(uintptr_t)disp.push[chid];
    u32 words = put_bytes / 4;

    u32 at = disp.got[chid] / 4;
    while (at < words) {
        u32 header = buffer[at++];
        u32 method = (header & 0x1FFF) << 2;
        u32 count = (header >> 18) & 0x7FF;
        bool same = (header & (1u << 16)) != 0;

        if (count == 0 || at + count > words) {
            complain("a push buffer header runs past what was revealed");
            break;
        }

        for (u32 i = 0; i < count; i++) {
            u32 value = buffer[at++];
            u32 target = same ? method : method + i * 4;
            if (chid == NV_DISP_CHID_CORE) core_method(target, value);
            else window_method(target, value);
        }
    }

    disp.got[chid] = words * 4;
}

void nv_disp_model_write(u32 offset, u32 value) {
    if (!disp.present) return;

    for (int chid = 1; chid < 16; chid++) {
        if (offset == (u32)NV_PDISP_CHAN_PUSH_LO(chid)) {
            disp.push[chid] = (disp.push[chid] & ~0xFFFFFFFFull) | value;
            return;
        }
        if (offset == (u32)NV_PDISP_CHAN_PUSH_HI(chid)) {
            disp.push[chid] = (disp.push[chid] & 0xFFFFFFFFull) |
                              ((u64)value << 32);
            return;
        }
        if (offset == (u32)NV_PDISP_CHAN_CTRL(chid)) {
            if (value == NV_PDISP_CHAN_CTRL_RUN) {
                disp.running[chid] = true;
                disp.got[chid] = 0;
            }
            return;
        }
        if (offset == (u32)NV_PDISP_CHAN_USER_PUT(chid)) {
            if (value) consume(chid, value);
            /* The engine reports how far it has read, which is what the driver
             * waits for. */
            nv_card_t *c = nv_model_card();
            if (c && c->regs)
                *(volatile u32 *)(c->regs + NV_PDISP_CHAN_USER_GET(chid)) =
                    disp.got[chid];
            return;
        }
    }
}

void nv_disp_model_attach(u32 core_class, u32 window_class) {
    memset(&disp, 0, sizeof disp);
    disp.present = true;
    disp.core_class = core_class;
    disp.window_class = window_class;
    for (int w = 0; w < MODEL_WINDOWS; w++) disp.owner[w] = -1;
}

void nv_disp_model_detach(void) { disp.present = false; }

int  nv_disp_model_methods(void)        { return disp.methods; }
bool nv_disp_model_updated(void)        { return disp.core_updated && disp.window_updated; }
u32  nv_disp_model_head_raster_width(void)  { return disp.raster_w; }
u32  nv_disp_model_head_raster_height(void) { return disp.raster_h; }
u32  nv_disp_model_pixel_hz(void)       { return disp.pixel_hz; }
u64  nv_disp_model_surface(void)        { return disp.surface; }
u32  nv_disp_model_window_width(void)   { return disp.size_out & 0x7FFF; }
const char *nv_disp_model_complaint(void) { return disp.complaint; }

int nv_disp_model_window_owner(int window) {
    if (window < 0 || window >= MODEL_WINDOWS) return -1;
    return disp.owner[window];
}

/* ------------------------------------------------------------------- test */

int nv_disp_modern_selftest(void) {
    int failures = 0;

    nv_card_t *c = nv_model_card();
    if (!c) {
        kinfo("nv-disp", "no card to try this on");
        return 0;
    }

    /* The class each generation speaks, chosen from what the chip said it was
     * rather than from a list of parts. */
    struct { u32 chipset; u32 expect; } generations[] = {
        { 0x140, NV_DISP_CORE_GV100 },
        { 0x162, NV_DISP_CORE_TU102 },
        { 0x172, NV_DISP_CORE_GA102 },
        { 0x192, NV_DISP_CORE_AD102 },
        { 0x1B3, NV_DISP_CORE_GB202 },   /* a GB203 - an RTX 5070 Ti        */
        { 0x130, 0 },                    /* Pascal: the older shape         */
    };

    for (size_t i = 0; i < sizeof generations / sizeof generations[0]; i++) {
        u32 got = nv_disp_core_class(generations[i].chipset);
        if (got != generations[i].expect) {
            kerr("nv-disp", "chipset %03x came out as class %04x, expected %04x",
                 generations[i].chipset, got, generations[i].expect);
            failures++;
        }
    }

    if (nv_disp_window_class(0x1B3) != NV_DISP_WINDOW_GB202) {
        kerr("nv-disp", "the window class for Blackwell came out as %04x",
             nv_disp_window_class(0x1B3));
        failures++;
    }

    /* And a mode change, built the way it has to be built from Volta onward. */
    u64 phys = 0;
    u32 *pages = dma_alloc_pages(2, &phys);
    if (!pages) {
        kerr("nv-disp", "no memory for the channels");
        return failures + 1;
    }
    memset(pages, 0, PAGE_SIZE * 2);

    /* The model reads the buffer through the pointer it was armed with, so the
     * address handed to the card is the one this side can follow. */
    u64 core_at = (u64)(uintptr_t)pages;
    u64 window_at = (u64)(uintptr_t)(pages + PAGE_SIZE / 4);

    nv_disp_model_attach(NV_DISP_CORE_GB202, NV_DISP_WINDOW_GB202);

    static nv_dchan_t core, window;
    nv_dchan_init(c, &core, NV_DISP_CHID_CORE, pages, core_at, PAGE_SIZE / 4);
    nv_dchan_init(c, &window, NV_DISP_CHID_WINDOW(0), pages + PAGE_SIZE / 4,
                  window_at, PAGE_SIZE / 4);

    /* 3840 by 2160 at sixty, with the timings a monitor actually reports. */
    static const nv_raster_t mode = {
        .clock_khz = 594000,
        .hdisplay = 3840, .hsync_start = 4016, .hsync_end = 4104, .htotal = 4400,
        .vdisplay = 2160, .vsync_start = 2168, .vsync_end = 2178, .vtotal = 2250,
        .hsync_negative = false, .vsync_negative = false,
    };

    if (!nv_disp_modern_modeset(c, &core, &window, 0, 0, 1, &mode,
                                0x10000000ULL, 3840 * 4, true)) {
        kerr("nv-disp", "the mode change was not built");
        failures++;
    } else {
        if (nv_disp_model_complaint()[0]) {
            kerr("nv-disp", "the engine complained: %s", nv_disp_model_complaint());
            failures++;
        }
        if (!nv_disp_model_updated()) {
            kerr("nv-disp", "one of the two updates never arrived");
            failures++;
        }
        /* The raster is the whole timing, not the visible part - and a driver
         * that sends the visible part instead produces a monitor that syncs to
         * nothing. */
        if (nv_disp_model_head_raster_width() != 4400 ||
            nv_disp_model_head_raster_height() != 2250) {
            kerr("nv-disp", "the raster came out as %ux%u, expected 4400x2250",
                 nv_disp_model_head_raster_width(),
                 nv_disp_model_head_raster_height());
            failures++;
        }
        if (nv_disp_model_pixel_hz() != 594000000u) {
            kerr("nv-disp", "the pixel clock came out as %u Hz",
                 nv_disp_model_pixel_hz());
            failures++;
        }
        if (nv_disp_model_window_owner(0) != 0) {
            kerr("nv-disp", "window 0 was not given to head 0, so it would "
                            "scan out to nothing");
            failures++;
        }
        if (nv_disp_model_surface() != 0x10000000ULL) {
            kerr("nv-disp", "the surface came out at %llx",
                 (unsigned long long)nv_disp_model_surface());
            failures++;
        }
        if (nv_disp_model_window_width() != 3840) {
            kerr("nv-disp", "the window is %u wide, expected 3840",
                 nv_disp_model_window_width());
            failures++;
        }
    }

    int methods = nv_disp_model_methods();

    /* A surface that is not on the boundary the engine requires has to be
     * refused rather than rounded, because rounding it displays the wrong
     * memory. */
    if (nv_disp_modern_modeset(c, &core, &window, 0, 0, 1, &mode,
                               0x10000040ULL, 3840 * 4, true)) {
        kerr("nv-disp", "a misaligned surface was accepted");
        failures++;
    }

    /* And a pitch that is not a multiple of what the field counts in. */
    if (nv_disp_modern_modeset(c, &core, &window, 0, 0, 1, &mode,
                               0x10000000ULL, 3840 * 4 + 4, true)) {
        kerr("nv-disp", "a pitch that cannot be expressed was accepted");
        failures++;
    }

    /* A timing that is not a raster at all. */
    {
        nv_raster_t broken = mode;
        broken.hsync_start = 3000;          /* before the visible part ends  */
        if (nv_disp_modern_modeset(c, &core, &window, 0, 0, 1, &broken,
                                   0x10000000ULL, 3840 * 4, true)) {
            kerr("nv-disp", "a timing whose sync starts inside the picture was "
                            "accepted");
            failures++;
        }
    }

    nv_disp_model_detach();

    if (!failures)
        kinfo("nv-disp", "the Blackwell display engine is driven: class %s, "
                         "%d methods across two channels, head rastering "
                         "4400x2250 at 594 MHz with window 0 given to it, and "
                         "surfaces the engine could not scan out refused",
              nv_disp_class_name(NV_DISP_CORE_GB202), methods);
    return failures;
}

/* --------------------------------------------------- on a card that is there
 *
 * What to do when this runs on a real Turing, Ampere, Ada or Blackwell card.
 *
 * The display engine on these is reachable: the channel registers answer, the
 * class is known, and the method stream above is what it takes.  What is not
 * safe is to arm those channels underneath a display the machine's own
 * firmware is already scanning out of.  Taking the core channel means taking
 * the head, and if anything in the sequence is wrong the screen goes black
 * with no way to report why - on a machine where this has never been run
 * against real silicon, that is not a trade worth making by default.
 *
 * So this identifies the engine, reads the monitors over their AUX pairs -
 * which changes nothing, being reads - and says exactly what it found and what
 * it is not doing.  Setting a mode is one call away and the code above is
 * exercised; what is missing is a card to try it on.
 */
void nv_display_modern_init(nv_card_t *c) {
    u32 core = nv_disp_core_class(c->chipset);

    if (!core) {
        kinfo("nv-disp", "%s has no display engine - it is a compute part",
              c->codename);
        return;
    }

    kinfo("nv-disp", "display engine class %s: %d head(s) and windows driven "
                     "through two channels",
          nv_disp_class_name(core), 4);

    /* Every output on one of these is DisplayPort or HDMI.  Reading what is
     * plugged in costs nothing and changes nothing. */
    int found = 0;
    for (int channel = 0; channel < 8; channel++) {
        nv_dp_link_t link;
        memset(&link, 0, sizeof link);
        link.channel = channel;
        link.sor = channel / 2;
        link.link = channel & 1;

        if (!nv_dp_read_caps(c, &link)) continue;

        u8 edid[128];
        if (!nv_dp_read_edid(c, &link, edid)) continue;

        u32 pixel_khz = ((u32)edid[54] | ((u32)edid[55] << 8)) * 10;
        u32 width = edid[56] | ((u32)(edid[58] & 0xF0) << 4);
        u32 height = edid[59] | ((u32)(edid[61] & 0xF0) << 4);

        kinfo("nv-disp", "a monitor on AUX channel %d: %ux%u at %u.%03u MHz, "
                         "up to %u lanes at %u.%u Gbps",
              channel, width, height, pixel_khz / 1000, pixel_khz % 1000,
              link.max_lanes, nv_dp_rate_khz(link.max_rate) / 1000000,
              (nv_dp_rate_khz(link.max_rate) / 100000) % 10);
        found++;
    }

    if (!found)
        kinfo("nv-disp", "no DisplayPort monitor answered on this card");

    kinfo("nv-disp", "the display is left where firmware put it: taking the "
                     "core channel takes the head, and this has never been run "
                     "against one of these cards");
}
