/* intel_modeset_host_test.c - check the Intel modeset ARITHMETIC off-target.
 *
 * kernel/intel_display.c splits a modeset into two halves: register-value
 * computation (pure arithmetic - timing from a monitor's EDID, the DP link's
 * data/link M/N, the refresh a mode is, the plane's format/size/stride) and
 * the MMIO writes + DPLL bring-up that need the real iGPU.  This exercises the
 * first half - the unmodified functions from intel_display.c, compiled with
 * INTEL_HOST_TEST so the hardware reader is left out - and checks every value
 * against an INDEPENDENT computation (floating point here, fixed point there),
 * so a transcription slip in the packing or the link math fails here instead
 * of silently on a screen nobody can attach from a build machine.
 *
 * The modes are the target machine's real panels: 2560x1440 at 60/144/180/240
 * Hz (see the kestrelos-edid-parser memory).  Refresh is checked by round trip
 * - a dot clock is derived from a chosen rate and fed back in - so the rate the
 * driver would PROGRAM (#5) is proven to be the rate asked for.
 *
 * Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra -Wno-unused-function -DINTEL_HOST_TEST \
 *         -I kernel tools/intel_modeset_host_test.c -o intel_modeset_test
 *   ./intel_modeset_test
 */
#ifndef INTEL_HOST_TEST
#define INTEL_HOST_TEST
#endif
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

#include "intel_display.c"   /* the code under test, verbatim */

#include <stdio.h>
#include <string.h>

static int fails = 0, total = 0;
static void ck(const char *what, int cond) {
    total++;
    if (!cond) { fails++; printf("  FAIL: %s\n", what); }
    else        printf("  ok:   %s\n", what);
}

/* Capture the first few GGTT entries the mapping writes. */
static u64 gg_pte[8];
static u32 gg_slot[8];
static int gg_n;
static void gg_capture(void *ctx, u32 slot, u64 pte) {
    (void)ctx;
    if (gg_n < 8) { gg_slot[gg_n] = slot; gg_pte[gg_n] = pte; }
    gg_n++;
}

/* Capture 32-bit register writes in order, for the plane flip sequence. */
static u32 rreg[16], rval[16];
static int rn;
static void rcap(void *ctx, u32 reg, u32 val) {
    (void)ctx;
    if (rn < 16) { rreg[rn] = reg; rval[rn] = val; }
    rn++;
}

/* Decode a pack_se() field back to its (start, end) counts. */
static u32 se_lo(u32 v) { return (v & 0xFFFFu) + 1; }
static u32 se_hi(u32 v) { return ((v >> 16) & 0xFFFFu) + 1; }

/* Build a detailed timing for `w`x`h` at `hz`, deriving the dot clock from the
 * chosen rate and totals so refresh can be checked by round trip. */
static edid_mode_t mode_at(u32 w, u32 h, u32 htot, u32 vtot, double hz) {
    edid_mode_t m;
    memset(&m, 0, sizeof m);
    m.hactive = (u16)w; m.vactive = (u16)h;
    m.htotal = (u16)htot; m.vtotal = (u16)vtot;
    m.hsync_off = 48; m.hsync_w = 32;
    m.vsync_off = 3;  m.vsync_w = 5;
    /* pixel clock (kHz) = hz * total_pixels / 1000, rounded. */
    double pclk = hz * (double)htot * (double)vtot / 1000.0;
    m.pixel_clock_khz = (u32)(pclk + 0.5);
    return m;
}

static void check_timing(const char *label, u32 w, u32 h, u32 htot, u32 vtot, double hz) {
    printf("[%s: %ux%u @ %.3f Hz]\n", label, w, h, hz);
    edid_mode_t m = mode_at(w, h, htot, vtot, hz);
    intel_trans_timing_t t;
    ck("timing computed", intel_timing_from_edid(&m, &t));

    /* Packing: HTOTAL low=hactive, high=htotal; PIPESRC hi=w-1, lo=h-1. */
    ck("HTOTAL packs (hactive, htotal)", se_lo(t.htotal) == w && se_hi(t.htotal) == htot);
    ck("VTOTAL packs (vactive, vtotal)", se_lo(t.vtotal) == h && se_hi(t.vtotal) == vtot);
    ck("HSYNC packs (active+off, +width)",
       se_lo(t.hsync) == w + 48 && se_hi(t.hsync) == w + 48 + 32);
    ck("PIPESRC is (w-1)<<16 | (h-1)",
       t.pipesrc == (((w - 1) << 16) | (h - 1)));

    /* Refresh: within 1 mHz of an independent computation, and within ~0.5 Hz
     * of the rate we asked for (the dot clock was rounded to whole kHz). */
    u64 total = (u64)htot * vtot;
    u32 want = (u32)(((u64)m.pixel_clock_khz * 1000000u) / total);
    ck("refresh matches independent compute", t.refresh_mhz == want);
    double got_hz = t.refresh_mhz / 1000.0;
    ck("refresh is the rate asked for (<=0.5 Hz)", got_hz > hz - 0.5 && got_hz < hz + 0.5);
    printf("        -> driver would program %u.%03u Hz\n",
           t.refresh_mhz / 1000, t.refresh_mhz % 1000);
}

int main(void) {
    /* The target's panels: 2560x1440, one blanking, several rates. */
    check_timing("60Hz",  2560, 1440, 2720, 1481, 60.0);
    check_timing("144Hz", 2560, 1440, 2720, 1481, 144.0);
    check_timing("180Hz", 2560, 1440, 2720, 1481, 180.0);
    check_timing("240Hz", 2560, 1440, 2720, 1481, 240.0);
    /* A 4K panel, to exercise a different geometry. */
    check_timing("4K60",  3840, 2160, 4000, 2222, 60.0);

    /* DP link M/N: the ratio must equal the physical link utilisation. */
    {
        printf("[DP data M/N]\n");
        intel_dp_mn_t mn;
        u32 pclk = 580080, bpp = 24, lanes = 4, link = 8100000; /* HBR3, 4 lanes */
        ck("M/N computed", intel_dp_compute_data_mn(&mn, pclk, bpp, lanes, link));
        double util_want = (double)pclk * bpp / ((double)link * lanes * 8.0 / 10.0);
        double util_got  = (double)mn.data_m / mn.data_n;
        ck("data_m/data_n equals link utilisation (<0.1%)",
           util_got > util_want * 0.999 && util_got < util_want * 1.001);
        ck("M/N fields fit 24 bits", mn.data_m <= 0xFFFFFF && mn.data_n <= 0xFFFFFF);
        ck("tu is 64", mn.tu == 64);
        printf("        -> link %.2f%% utilised\n", util_got * 100.0);

        /* A mode that cannot fit the link must be refused, not wrapped. */
        ck("over-capacity mode refused",
           !intel_dp_compute_data_mn(&mn, 3000000, 24, 1, 1620000));

        /* The DATA_M1 register write must carry BOTH the transfer-unit size
         * (bits 30:25 = TU-1) and the M value (bits 23:0).  A bare M leaves
         * TU_SIZE = 0 (TU = 1), which mispaces the DP stream - the exact bug
         * this check exists to stop from coming back. */
        ck("M/N recomputed for the write", intel_dp_compute_data_mn(&mn, pclk, bpp, lanes, link));
        edid_mode_t wm = mode_at(2560, 1440, 2720, 1481, 240.0);
        intel_trans_timing_t wt;
        ck("timing for the write", intel_timing_from_edid(&wm, &wt));
        rn = 0;
        intel_display_write_mode(rcap, 0, 0, &wt, &mn);
        u32 data_m1 = 0; int saw_m1 = 0;
        for (int i = 0; i < rn && i < 16; i++)
            if (rreg[i] == INTEL_TRANS_DATA_M1(0)) { data_m1 = rval[i]; saw_m1 = 1; }
        ck("DATA_M1 was written", saw_m1);
        ck("DATA_M1 low 24 bits are M", (data_m1 & 0x00FFFFFFu) == mn.data_m);
        ck("DATA_M1 TU_SIZE field is TU-1 (=63)", ((data_m1 >> 25) & 0x3Fu) == (mn.tu - 1));
    }

    /* Plane config for an XRGB8888 framebuffer. */
    {
        printf("[plane config]\n");
        intel_plane_config_t p;
        u32 w = 2560, h = 1440, stride = 2560 * 4;
        ck("plane computed", intel_plane_config_xrgb8888(&p, 0x10000000ull, w, h, stride));
        ck("size is (h-1)<<16 | (w-1)", p.size == (((h - 1) << 16) | (w - 1)));
        ck("stride is bytes/64", p.stride == (stride / 64));
        ck("ctl has enable + XRGB8888", (p.ctl & INTEL_PLANE_CTL_ENABLE_BIT) &&
           (p.ctl & INTEL_PLANE_CTL_FORMAT_XRGB8888));
        ck("surf is the page-aligned base", p.surf == (0x10000000u & INTEL_PLANE_SURF_ADDR_MASK));
    }

    /* GGTT scanout mapping: our framebuffer's pages become GGTT entries so the
     * display engine can read them, and the plane scans from the first slot. */
    {
        printf("[GGTT scanout mapping]\n");
        u64 fb = 0x1F000000ull, bytes = (u64)2560 * 1440 * 4;
        u32 slot = 256, pages = 0;
        gg_n = 0;
        u64 off = intel_ggtt_map_scanout(gg_capture, 0, slot, fb, bytes, &pages);
        ck("page count = ceil(bytes/4096)", pages == (u32)((bytes + 4095) / 4096));
        ck("wrote one entry per page", gg_n == (int)pages);
        ck("plane offset = first_slot * 4096", off == (u64)slot * 4096);
        ck("first entry is at first_slot", gg_slot[0] == slot);
        ck("first entry = address | present",
           gg_pte[0] == (fb | 1ull));
        ck("second entry maps the next page",
           gg_pte[1] == ((fb + 4096) | 1ull));
        /* The encoder itself: present, address in place, no LM, PAT index 0. */
        u64 e = intel_ggtt_scanout_pte(fb);
        ck("PTE has present bit", (e & 1ull) != 0);
        ck("PTE carries the page-aligned address", (e & 0x00003FFFFFFFF000ull) == fb);
        ck("PTE is not local memory", (e & (1ull << 1)) == 0);
        ck("PTE PAT index 0 (write-back coherent)",
           (e & ((1ull << 52) | (1ull << 53))) == 0);
        /* A non-page-aligned framebuffer base is refused. */
        ck("a misaligned fb base is refused",
           intel_ggtt_map_scanout(gg_capture, 0, 0, fb + 1, bytes, &pages) == 0);

        /* The flip: plane pointed at the GGTT offset, SURF written last. */
        printf("[plane flip onto the iGPU]\n");
        intel_plane_config_t pc;
        intel_plane_config_xrgb8888(&pc, off, 2560, 1440, 2560 * 4);
        rn = 0;
        u32 flip = intel_plane_write_scanout(rcap, 0, 0, &pc);
        ck("flip writes 4 registers", flip == 4);
        ck("SURF is the last write", rn == 4 && rreg[3] == INTEL_PLANE_SURF(0));
        ck("SURF carries the GGTT offset", rval[3] == pc.surf);
        ck("CTL enables the plane", rval[2] & INTEL_PLANE_CTL_ENABLE_BIT);
        ck("STRIDE before SURF (shadowed until arm)", rreg[0] == INTEL_PLANE_STRIDE(0));
    }

    printf("\n%d/%d checks passed%s\n", total - fails, total,
           fails ? "  <<< REGRESSION" : "  ALL GOOD");
    return fails ? 1 : 0;
}
