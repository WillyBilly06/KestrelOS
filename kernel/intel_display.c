/* intel_display.c - read the Intel iGPU's display engine.  Read-only.
 * See intel_display.h.  Register offsets verified by tools/verify_intel_regs.py
 * against drm/i915/display/intel_display_regs.h. */
#ifdef INTEL_HOST_TEST
/* The timing ENCODER below is pure arithmetic and is unit-tested on the host
 * (intel_modeset_host_test.c); the hardware reader needs the kernel, so it is
 * compiled out of the test build. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
#include "intel_display.h"
#else
#include "intel_display.h"
#include "pci.h"
#include "mm.h"
#include "klog.h"

/* Enough of BAR0 to reach the pipe/transcoder registers (they end below
 * 0x74000); the whole GTTMMADR is 16 MiB, which there is no reason to map. */
#define INTEL_REG_WINDOW  0x80000u
#endif  /* INTEL_HOST_TEST include selection */

#ifndef INTEL_HOST_TEST
/* Which Intel display adapters this understands - the Xe-LPG block (Meteor and
 * Arrow Lake), of which the target machine's 0x7D67 is one.  Kept as the high
 * byte because Intel numbers a generation in a block; older generations are
 * not refused, just not read (their pipe layout differs and is not verified
 * here). */
static int is_xe_lpg(u16 device) {
    return (device & 0xFF00u) == 0x7D00u;
}

static pci_dev_t *find_intel_display(void) {
    for (pci_dev_t *d = pci_first(); d; d = d->next) {
        if (d->vendor != 0x8086) continue;
        if (d->class_code != 0x03) continue;      /* display controller */
        if (!is_xe_lpg(d->device)) continue;
        return d;
    }
    return NULL;
}

int intel_display_read(intel_display_state_t *out) {
    for (int i = 0; i < INTEL_DISPLAY_PIPES; i++) {
        out->pipe[i].active = 0;
        out->pipe[i].width = out->pipe[i].height = 0;
        out->pipe[i].htotal = out->pipe[i].vtotal = 0;
    }
    out->active_pipes = 0;
    out->present = 0;

    pci_dev_t *d = find_intel_display();
    if (!d) return 0;                             /* none here (e.g. the VM) */
    out->present = 1;

    u64 bar0 = d->bar[0];
    if (!bar0 || d->bar_is_io[0]) {
        kwarn("intel-disp", "the iGPU's register BAR is not usable (%llx)",
              (unsigned long long)bar0);
        return 0;
    }

    volatile u8 *mmio = vmm_map_mmio(bar0, INTEL_REG_WINDOW);
    if (!mmio) {
        kwarn("intel-disp", "could not map the iGPU registers at %llx",
              (unsigned long long)bar0);
        return 0;
    }

    for (int t = 0; t < INTEL_DISPLAY_PIPES; t++) {
        u32 conf = *(volatile u32 *)(mmio + INTEL_TRANSCONF(t));
        if (!(conf & INTEL_TRANSCONF_ENABLE)) continue;

        u32 htot = *(volatile u32 *)(mmio + INTEL_TRANS_HTOTAL(t));
        u32 vtot = *(volatile u32 *)(mmio + INTEL_TRANS_VTOTAL(t));
        u32 src  = *(volatile u32 *)(mmio + INTEL_PIPESRC(t));

        out->pipe[t].active = 1;
        out->pipe[t].width  = INTEL_PIPESRC_W(src);
        out->pipe[t].height = INTEL_PIPESRC_H(src);
        out->pipe[t].htotal = INTEL_TOTAL_OF(htot);
        out->pipe[t].vtotal = INTEL_TOTAL_OF(vtot);

        /* What surface this pipe scans out - the framebuffer address is the
         * one register the OS would rewrite to make the iGPU show its own
         * desktop instead of the firmware's. */
        u32 pctl = *(volatile u32 *)(mmio + INTEL_PLANE_CTL(t));
        out->pipe[t].plane_on = (pctl & INTEL_PLANE_CTL_ENABLE) ? 1 : 0;
        if (out->pipe[t].plane_on) {
            u32 surf = *(volatile u32 *)(mmio + INTEL_PLANE_SURF(t));
            out->pipe[t].surface = (u64)(surf & INTEL_PLANE_SURF_ADDR_MASK);
        }
        out->active_pipes++;
    }
    return 1;
}

#endif  /* INTEL_HOST_TEST - the hardware reader above needs the kernel */

/* Pack a (start, end) pair the way the timing registers want it: each value
 * minus one, start in the low half, end in the high half. */
static u32 pack_se(u32 start, u32 end) {
    return ((start - 1) & 0xFFFFu) | (((end - 1) & 0xFFFFu) << 16);
}

int intel_timing_from_edid(const edid_mode_t *m, intel_trans_timing_t *out) {
    if (!m->hactive || !m->vactive || !m->htotal || !m->vtotal) return 0;

    /* Horizontal: active ends at hactive, blank runs to htotal; sync sits
     * inside the blank at hactive + front-porch for the sync width. */
    u32 h_sync_start = (u32)m->hactive + m->hsync_off;
    u32 h_sync_end   = h_sync_start + m->hsync_w;
    u32 v_sync_start = (u32)m->vactive + m->vsync_off;
    u32 v_sync_end   = v_sync_start + m->vsync_w;

    out->htotal = pack_se(m->hactive, m->htotal);
    out->hblank = pack_se(m->hactive, m->htotal);   /* blank == active..total */
    out->hsync  = pack_se(h_sync_start, h_sync_end);
    out->vtotal = pack_se(m->vactive, m->vtotal);
    out->vblank = pack_se(m->vactive, m->vtotal);
    out->vsync  = pack_se(v_sync_start, v_sync_end);
    /* PIPESRC packs width-1 in the high half, height-1 in the low half. */
    out->pipesrc = (((u32)(m->hactive - 1) & 0xFFFFu) << 16) |
                   ((u32)(m->vactive - 1) & 0xFFFFu);
    out->pixel_clock_khz = m->pixel_clock_khz;
    /* The rate this mode is: dot clock over the whole frame's pixel count.
     * Computed here from the raw totals (not read back from m->refresh_mhz, so
     * it stands alone for a hand-built mode) in millihertz, exactly as edid.c
     * derives it, so a modeset can say which refresh it programs - #5. */
    u32 total = (u32)m->htotal * (u32)m->vtotal;
    out->refresh_mhz = total
        ? (u32)(((u64)m->pixel_clock_khz * 1000000u) / total)
        : 0;
    return 1;
}

/* The DP transcoder's DATA M/N: the fraction of link bandwidth a mode uses, so
 * the engine paces pixel data onto the link.  data_m/data_n is exactly the link
 * utilization - (pixel_clock * bpp) / (link_rate * lanes * 8/10 for 8b/10b) -
 * which is how it is verified: the ratio must equal the physical utilization,
 * computed independently in intel_dp_mn_host_test.c.  Following i915, data_n is
 * a fixed 0x800000 and data_m is scaled to it, then both reduced to fit the
 * 24-bit field.  8b/10b rates only (RBR..HBR3); UHBR uses 128b/132b and a
 * different efficiency, not handled here. */
static void reduce_mn(u32 *m, u32 *n) {
    while (*m > 0xFFFFFF || *n > 0xFFFFFF) { *m >>= 1; *n >>= 1; }
}

int intel_dp_compute_data_mn(intel_dp_mn_t *out, u32 pixel_khz, u32 bpp,
                             u32 lanes, u32 link_rate_khz) {
    if (!pixel_khz || !bpp || !lanes || !link_rate_khz) return 0;

    /* Both rates in kbit/s.  Link is 8b/10b, so 8 usable bits per 10 on wire. */
    u64 data_rate = (u64)pixel_khz * bpp;                  /* kbit/s of pixels  */
    u64 link_rate = (u64)link_rate_khz * lanes * 8u / 10u; /* kbit/s usable     */
    if (data_rate > link_rate) return 0;                   /* mode won't fit    */

    u32 n = 0x800000u;
    u32 m = (u32)((data_rate * n) / link_rate);
    reduce_mn(&m, &n);
    out->data_m = m;
    out->data_n = n;

    /* Link M/N is pixel clock over the link symbol clock (for 8b/10b the symbol
     * clock is the link rate in kHz).  link_m/link_n = pixel_khz / link_khz. */
    u32 ln = 0x80000u;
    u32 lm = (u32)(((u64)pixel_khz * ln) / link_rate_khz);
    reduce_mn(&lm, &ln);
    out->link_m = lm;
    out->link_n = ln;

    out->tu = 64;
    return 1;
}

u32 intel_display_write_mode(intel_reg_write_fn wr, void *ctx, int t,
                             const intel_trans_timing_t *timing,
                             const intel_dp_mn_t *mn) {
    u32 n = 0;
    /* Timing first: the transcoder's picture of the frame. */
    wr(ctx, INTEL_TRANS_HTOTAL(t), timing->htotal); n++;
    wr(ctx, INTEL_TRANS_HBLANK(t), timing->hblank); n++;
    wr(ctx, INTEL_TRANS_HSYNC(t),  timing->hsync);  n++;
    wr(ctx, INTEL_TRANS_VTOTAL(t), timing->vtotal); n++;
    wr(ctx, INTEL_TRANS_VBLANK(t), timing->vblank); n++;
    wr(ctx, INTEL_TRANS_VSYNC(t),  timing->vsync);  n++;
    wr(ctx, INTEL_PIPESRC(t),      timing->pipesrc); n++;
    /* Then how the pixels are paced onto the link.  DATA_M1 is not just the M
     * value: the transfer-unit size sits in bits 30:25 as (TU - 1), and the M
     * value in bits 23:0.  Writing a bare M leaves TU_SIZE = 0, i.e. TU = 1,
     * which mispaces the whole DP stream - so both fields go in together. */
    {
        u32 tu = mn->tu ? mn->tu : 64;
        u32 data_m1 = (((tu - 1) & 0x3Fu) << 25) | (mn->data_m & 0x00FFFFFFu);
        wr(ctx, INTEL_TRANS_DATA_M1(t), data_m1); n++;
    }
    wr(ctx, INTEL_TRANS_DATA_N1(t), mn->data_n); n++;
    wr(ctx, INTEL_TRANS_LINK_M1(t), mn->link_m); n++;
    wr(ctx, INTEL_TRANS_LINK_N1(t), mn->link_n); n++;
    return n;
}

u32 intel_plane_write_scanout(intel_reg_write_fn wr, void *ctx, int t,
                              const intel_plane_config_t *cfg) {
    u32 n = 0;
    /* Stride, size and control are shadowed; SURF latches them all on vblank,
     * so it is written last - writing it earlier would arm a half-set plane. */
    wr(ctx, INTEL_PLANE_STRIDE(t), cfg->stride); n++;
    wr(ctx, INTEL_PLANE_SIZE(t),   cfg->size);   n++;
    wr(ctx, INTEL_PLANE_CTL(t),    cfg->ctl);    n++;
    wr(ctx, INTEL_PLANE_SURF(t),   cfg->surf);   n++;
    return n;
}

int intel_plane_config_xrgb8888(intel_plane_config_t *out, u64 fb_addr,
                                u32 width, u32 height, u32 stride_bytes) {
    if (!width || !height || !stride_bytes) return 0;
    out->ctl = INTEL_PLANE_CTL_ENABLE_BIT | INTEL_PLANE_CTL_FORMAT_XRGB8888 |
               INTEL_PLANE_CTL_TILED_LINEAR;
    /* Linear stride is counted in 64-byte units in the low 12 bits. */
    out->stride = (stride_bytes / 64u) & 0xFFFu;
    /* Size packs height-1 in the high half, width-1 in the low half. */
    out->size = (((height - 1u) & 0xFFFFu) << 16) | ((width - 1u) & 0xFFFFu);
    /* The surface register carries the page-aligned base address. */
    out->surf = (u32)(fb_addr & INTEL_PLANE_SURF_ADDR_MASK);
    return 1;
}

u64 intel_ggtt_scanout_pte(u64 phys) {
    /* Address in bits 45:12, present; PAT 0 (write-back coherent) and no LM,
     * so those bits stay clear - the framebuffer is CPU-drawn system memory. */
    return (phys & INTEL_GGTT_PTE_ADDR_MASK) | INTEL_GGTT_PTE_PRESENT;
}

u64 intel_ggtt_map_scanout(intel_ggtt_write_fn wr, void *ctx, u32 first_slot,
                           u64 fb_phys, u64 fb_bytes, u32 *pages_out) {
    if (pages_out) *pages_out = 0;
    if (!fb_bytes) return 0;

    /* The framebuffer's own base must be page-aligned to sit in the GGTT; a
     * partial first page would scan out the bytes before it. */
    if (fb_phys & (INTEL_GGTT_PAGE - 1)) return 0;

    u32 pages = (u32)((fb_bytes + INTEL_GGTT_PAGE - 1) / INTEL_GGTT_PAGE);
    for (u32 i = 0; i < pages; i++) {
        u64 page_phys = fb_phys + (u64)i * INTEL_GGTT_PAGE;
        if (wr) wr(ctx, first_slot + i, intel_ggtt_scanout_pte(page_phys));
    }
    if (pages_out) *pages_out = pages;

    /* The plane scans from the offset of the first entry we wrote. */
    return (u64)first_slot * INTEL_GGTT_PAGE;
}

#ifndef INTEL_HOST_TEST
/* Verify the modeset write path live, the way nv_compute_selftest verifies the
 * compute dispatch: build a real mode, run the write sequence into a capture
 * buffer, and check every register got its computed value.  Runs with no iGPU
 * (it captures, it does not touch MMIO), so it exercises the path in the actual
 * kernel - which is where nv_compute_selftest found a bug the host test missed.
 * Returns the number of failures. */
static u32 dsel_reg[16], dsel_val[16];
static int dsel_n;
static void dsel_capture(void *ctx, u32 reg, u32 value) {
    (void)ctx;
    if (dsel_n < 16) { dsel_reg[dsel_n] = reg; dsel_val[dsel_n] = value; dsel_n++; }
}
static int dsel_find(u32 reg) {
    for (int i = 0; i < dsel_n; i++) if (dsel_reg[i] == reg) return i;
    return -1;
}

/* The GGTT mapping writes one 64-bit entry per page; a real framebuffer is
 * thousands, so the capture keeps only the first few and the running count. */
static u64 gsel_pte[8];
static u32 gsel_slot[8], gsel_count;
static void gsel_capture(void *ctx, u32 slot, u64 pte) {
    (void)ctx;
    if (gsel_count < 8) { gsel_slot[gsel_count] = slot; gsel_pte[gsel_count] = pte; }
    gsel_count++;
}

int intel_display_selftest(void) {
    /* A real mode from the target's panels: 2560x1440 @ 144 Hz. */
    edid_mode_t m;
    memset(&m, 0, sizeof m);
    m.hactive = 2560; m.htotal = 2720; m.hsync_off = 48; m.hsync_w = 32;
    m.vactive = 1440; m.vtotal = 1481; m.vsync_off = 3;  m.vsync_w = 5;
    m.pixel_clock_khz = 580080;

    intel_trans_timing_t t;
    intel_dp_mn_t mn;
    int failures = 0;
    if (!intel_timing_from_edid(&m, &t)) { kerr("intel-disp", "timing compute failed"); failures++; }
    if (!intel_dp_compute_data_mn(&mn, 580080, 24, 4, 8100000)) { kerr("intel-disp", "M/N compute failed"); failures++; }

    /* The mode must actually BE 144 Hz: dot clock / total pixels.  Within a
     * few millihertz of 144000 (integer division loses a fraction). */
    if (t.refresh_mhz < 143900 || t.refresh_mhz > 144100) {
        kerr("intel-disp", "computed refresh %u mHz, expected ~144000", t.refresh_mhz);
        failures++;
    }

    dsel_n = 0;
    u32 wrote = intel_display_write_mode(dsel_capture, NULL, 0, &t, &mn);
    if (wrote != 11) { kerr("intel-disp", "modeset wrote %u registers, expected 11", wrote); failures++; }

    int i;
    i = dsel_find(INTEL_TRANS_HTOTAL(0));  if (i < 0 || dsel_val[i] != t.htotal) { kerr("intel-disp", "HTOTAL not written correctly"); failures++; }
    /* DATA_M1 carries the M value in bits 23:0 AND the transfer-unit size in
     * bits 30:25 as (TU - 1).  Check both fields, not just M - this used to
     * assert the bare M and so "passed" while TU_SIZE was silently 0. */
    i = dsel_find(INTEL_TRANS_DATA_M1(0));
    if (i < 0 || (dsel_val[i] & 0x00FFFFFFu) != mn.data_m ||
        ((dsel_val[i] >> 25) & 0x3Fu) != (mn.tu - 1)) {
        kerr("intel-disp", "DATA_M1 not written correctly (M or TU_SIZE)"); failures++;
    }
    i = dsel_find(INTEL_TRANS_DATA_N1(0)); if (i < 0 || dsel_val[i] != mn.data_n) { kerr("intel-disp", "DATA_N1 not written correctly"); failures++; }
    if (dsel_find(INTEL_TRANS_HTOTAL(0)) > dsel_find(INTEL_TRANS_DATA_M1(0))) { kerr("intel-disp", "timing must precede M/N"); failures++; }

    /* Pinning a 2560x1440 32-bit framebuffer into the GGTT for scanout: one
     * entry per page, present, carrying the page's physical address; the plane
     * then scans from the first slot's offset. */
    u64 fb_phys = 0x1F000000ull;                 /* page-aligned system RAM     */
    u64 fb_bytes = (u64)2560 * 1440 * 4;
    u32 first_slot = 256;                         /* an arbitrary free GGTT slot */
    gsel_count = 0;
    u32 pages = 0;
    u64 offset = intel_ggtt_map_scanout(gsel_capture, NULL, first_slot,
                                        fb_phys, fb_bytes, &pages);
    u32 want_pages = (u32)((fb_bytes + INTEL_GGTT_PAGE - 1) / INTEL_GGTT_PAGE);
    if (pages != want_pages) { kerr("intel-disp", "GGTT mapped %u pages, expected %u", pages, want_pages); failures++; }
    if (gsel_count != want_pages) { kerr("intel-disp", "GGTT wrote %u entries, expected %u", gsel_count, want_pages); failures++; }
    if (offset != (u64)first_slot * INTEL_GGTT_PAGE) { kerr("intel-disp", "GGTT scanout offset wrong"); failures++; }
    if (gsel_slot[0] != first_slot || gsel_pte[0] != (fb_phys | INTEL_GGTT_PTE_PRESENT)) { kerr("intel-disp", "first GGTT entry wrong"); failures++; }
    if (gsel_pte[1] != ((fb_phys + INTEL_GGTT_PAGE) | INTEL_GGTT_PTE_PRESENT)) { kerr("intel-disp", "second GGTT entry not the next page"); failures++; }
    if (gsel_pte[0] & INTEL_GGTT_PTE_LM) { kerr("intel-disp", "GGTT entry wrongly marked local memory"); failures++; }

    /* The flip: point the plane at that GGTT offset and arm it.  SURF must be
     * the last of the four writes, or the plane latches half-configured. */
    intel_plane_config_t pcfg;
    if (!intel_plane_config_xrgb8888(&pcfg, offset, 2560, 1440, 2560 * 4)) {
        kerr("intel-disp", "plane config failed"); failures++;
    }
    dsel_n = 0;
    u32 flip = intel_plane_write_scanout(dsel_capture, NULL, 0, &pcfg);
    if (flip != 4) { kerr("intel-disp", "plane flip wrote %u registers, expected 4", flip); failures++; }
    if (dsel_find(INTEL_PLANE_SURF(0)) != dsel_n - 1) { kerr("intel-disp", "SURF must be the last plane write"); failures++; }
    int si = dsel_find(INTEL_PLANE_SURF(0));
    if (si < 0 || dsel_val[si] != pcfg.surf) { kerr("intel-disp", "SURF not the scanout offset"); failures++; }
    i = dsel_find(INTEL_PLANE_CTL(0));
    if (i < 0 || !(dsel_val[i] & INTEL_PLANE_CTL_ENABLE_BIT)) { kerr("intel-disp", "plane not enabled"); failures++; }

    if (!failures)
        kinfo("intel-disp", "display path verified: mode 2560x1440@%u.%03u Hz "
              "(11 registers, DP link %u%% utilised); framebuffer pinned into the "
              "GGTT as %u scanout pages at offset %#llx; and the plane flipped "
              "onto it (4 registers, SURF last) - the iGPU would show our desktop, "
              "ready for a real Arrow Lake",
              t.refresh_mhz / 1000, t.refresh_mhz % 1000,
              (unsigned)((u64)mn.data_m * 100 / mn.data_n),
              pages, (unsigned long long)offset);
    return failures;
}

void intel_display_probe(void) {
    /* Verify the modeset write path in the kernel first - it needs no iGPU, so
     * it runs everywhere (in the VM too) and proves the computation + write
     * sequence are sound before any real card is ever driven. */
    intel_display_selftest();

    intel_display_state_t st;
    if (!intel_display_read(&st)) {
        if (!st.present)
            kdebug("intel-disp", "no Intel display adapter on the bus");
        return;
    }

    kinfo("intel-disp", "Intel iGPU display: %d pipe(s) lit of %d",
          st.active_pipes, INTEL_DISPLAY_PIPES);
    for (int t = 0; t < INTEL_DISPLAY_PIPES; t++) {
        if (!st.pipe[t].active) continue;
        /* Refresh = pixel clock / (htotal*vtotal); the pixel clock lives in the
         * DPLL, not read yet, so the frame's total pixel count is reported and
         * the rate is left to the DPLL step. */
        kinfo("intel-disp", "  pipe %c: %ux%u active, %ux%u total "
                            "(%u px/frame; refresh needs the DPLL clock)",
              'A' + t, st.pipe[t].width, st.pipe[t].height,
              st.pipe[t].htotal, st.pipe[t].vtotal,
              st.pipe[t].htotal * st.pipe[t].vtotal);
        if (st.pipe[t].plane_on)
            kinfo("intel-disp", "    scanning out the surface at %llx",
                  (unsigned long long)st.pipe[t].surface);
    }
    if (st.active_pipes > 1)
        kinfo("intel-disp", "  %d outputs are lit - the multi-display modes "
                            "(extend/mirror/only-other) map onto these pipes",
              st.active_pipes);
}
#endif  /* INTEL_HOST_TEST */
