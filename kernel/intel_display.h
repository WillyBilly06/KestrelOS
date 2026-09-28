/* intel_display.h - reading the Intel iGPU's display engine.
 *
 * The machine this targets has an Intel Arrow Lake-S iGPU (8086:7D67, Xe-LPG)
 * beside its RTX 5070 Ti.  Unlike the NVIDIA card there is no signed-firmware
 * co-processor between the driver and the hardware: the display engine is a
 * block of memory-mapped registers in BAR0, documented in Intel's PRMs and in
 * the i915 driver.  This is the first, entirely READ-ONLY step - find out what
 * the firmware left the display doing: which pipes are lit, at what size, with
 * what timing.  It writes nothing, so it is safe to run against the panel that
 * is showing the desktop.
 *
 * The register offsets are within BAR0 and are verified against the kernel's
 * drm/i915/display/intel_display_regs.h by tools/verify_intel_regs.py.  A wrong
 * display-register offset is the silent kind of wrong (see the memory note on
 * verifying transcribed constants), so it is checked mechanically, not by eye.
 */
#ifndef KESTREL_INTEL_DISPLAY_H
#define KESTREL_INTEL_DISPLAY_H

#ifndef INTEL_HOST_TEST
#include "kernel.h"
#endif
#include "edid.h"

/* Gen12+ has four pipe/transcoder pairs, A..D.  The main-transcoder timing
 * block starts at 0x60000 with a 0x1000 stride; the transcoder configuration
 * lives in the pipe block at 0x70008 with the same stride.  (The EDP and DSI
 * transcoders sit at their own offsets; the four here are the ones a desktop
 * output uses.) */
#define INTEL_DISPLAY_PIPES        4
#define INTEL_TRANS_STRIDE         0x1000u

#define INTEL_TRANS_TIMING_BASE    0x60000u
#define INTEL_TRANS_HTOTAL(t)      (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x00u)
#define INTEL_TRANS_HBLANK(t)      (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x04u)
#define INTEL_TRANS_HSYNC(t)       (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x08u)
#define INTEL_TRANS_VTOTAL(t)      (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x0cu)
#define INTEL_TRANS_VBLANK(t)      (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x10u)
#define INTEL_TRANS_VSYNC(t)       (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x14u)
#define INTEL_TRANS_DATA_M1(t)     (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x30u)
#define INTEL_TRANS_DATA_N1(t)     (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x34u)
#define INTEL_TRANS_LINK_M1(t)     (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x40u)
#define INTEL_TRANS_LINK_N1(t)     (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x44u)
#define INTEL_PIPESRC(t)           (INTEL_TRANS_TIMING_BASE + (u32)(t)*INTEL_TRANS_STRIDE + 0x1cu)

#define INTEL_TRANSCONF_BASE       0x70008u
#define INTEL_TRANSCONF(t)         (INTEL_TRANSCONF_BASE + (u32)(t)*INTEL_TRANS_STRIDE)
#define INTEL_TRANSCONF_ENABLE     (1u << 31)   /* the transcoder is running   */
#define INTEL_TRANSCONF_STATE_EN   (1u << 30)   /* and has reached that state  */

/* The primary plane per pipe - the surface a pipe actually scans out.  Its
 * SURF register is the framebuffer address; to make the iGPU render this
 * system's desktop, this is the register that would point at our own
 * framebuffer.  Same 0x1000 per-pipe stride. */
#define INTEL_PLANE_CTL_BASE       0x70180u
#define INTEL_PLANE_CTL(t)         (INTEL_PLANE_CTL_BASE + (u32)(t)*INTEL_TRANS_STRIDE)
#define INTEL_PLANE_CTL_ENABLE     (1u << 31)
#define INTEL_PLANE_STRIDE_BASE    0x70188u
#define INTEL_PLANE_STRIDE(t)      (INTEL_PLANE_STRIDE_BASE + (u32)(t)*INTEL_TRANS_STRIDE)
#define INTEL_PLANE_SIZE_BASE      0x70190u
#define INTEL_PLANE_SIZE(t)        (INTEL_PLANE_SIZE_BASE + (u32)(t)*INTEL_TRANS_STRIDE)
#define INTEL_PLANE_SURF_BASE      0x7019cu
#define INTEL_PLANE_SURF(t)        (INTEL_PLANE_SURF_BASE + (u32)(t)*INTEL_TRANS_STRIDE)
#define INTEL_PLANE_SURF_ADDR_MASK 0xFFFFF000u   /* page-aligned base address  */

/* HTOTAL/VTOTAL/PIPESRC pack two 16-bit fields; the hardware stores each count
 * minus one (an active width of 2560 is written as 2559). */
#define INTEL_ACTIVE_OF(v)         (((v) & 0xFFFFu) + 1u)          /* low half  */
#define INTEL_TOTAL_OF(v)          ((((v) >> 16) & 0xFFFFu) + 1u)  /* high half */
#define INTEL_PIPESRC_W(v)         ((((v) >> 16) & 0xFFFFu) + 1u)
#define INTEL_PIPESRC_H(v)         (((v) & 0xFFFFu) + 1u)

typedef struct {
    int  present;                 /* an Intel display adapter was found        */
    int  active_pipes;            /* how many transcoders are running          */
    struct {
        int active;
        u32 width, height;        /* the active resolution this pipe scans out */
        u32 htotal, vtotal;       /* including blanking - for the refresh calc */
        int plane_on;             /* the primary plane is enabled              */
        u64 surface;              /* the framebuffer address it scans out       */
    } pipe[INTEL_DISPLAY_PIPES];
} intel_display_state_t;

/* Find the Intel iGPU, map its registers, read the display state.  Read-only.
 * Returns false (and leaves out->present = 0) when there is no Intel display
 * adapter on the bus - which is the case in the VM, and the honest thing to
 * report there. */
int intel_display_read(intel_display_state_t *out);

/* Find, read, and log the state.  Called from gpu_init. */
void intel_display_probe(void);

/* Verify the modeset write path (timing + M/N + the write sequence) live in the
 * kernel against a capture buffer.  Needs no iGPU.  Returns failure count. */
int intel_display_selftest(void);

/* ---- writing a mode: the timing registers a transcoder needs for a mode ----
 *
 * The six timing registers a modeset programs, computed from a monitor's own
 * timing (an edid_mode_t).  This is the register-value COMPUTATION only - the
 * MMIO write and the DPLL that makes the pixel clock are the parts that need
 * the real card.  Kept separate so the arithmetic can be checked on the host
 * against the monitor's EDID (intel_modeset_host_test.c).  Every value follows
 * the i915 convention: each field is its count minus one, start in [15:0], end
 * or total in [31:16]. */
typedef struct {
    u32 htotal, hblank, hsync;   /* -> INTEL_TRANS_HTOTAL/HBLANK/HSYNC(t)     */
    u32 vtotal, vblank, vsync;   /* -> INTEL_TRANS_VTOTAL/VBLANK/VSYNC(t)     */
    u32 pipesrc;                 /* -> INTEL_PIPESRC(t)                       */
    u32 pixel_clock_khz;         /* the dot clock the DPLL must produce       */
    u32 refresh_mhz;             /* the rate this mode is, in millihertz: the
                                  * dot clock over the total pixel count.  This
                                  * is the refresh a modeset PROGRAMS (#5), and
                                  * what lets the selftest prove it targets a
                                  * specific rate rather than merely some mode. */
} intel_trans_timing_t;

/* Fill `out` from an EDID detailed timing (hactive/htotal/hsync_off/hsync_w...).
 * Pure arithmetic; returns 0 on a degenerate mode. */
int intel_timing_from_edid(const edid_mode_t *m, intel_trans_timing_t *out);

/* ---- the plane: which surface a pipe scans out, and how -------------------
 *
 * To make the iGPU show THIS system's desktop, a plane is pointed at our
 * framebuffer with its format, stride and size.  These are the register values
 * to write; the write itself, and the pipe/DPLL bring-up around it, need the
 * real card.  Field layout from skl_universal_plane_regs.h (verified). */
#define INTEL_PLANE_CTL_ENABLE_BIT       (1u << 31)
#define INTEL_PLANE_CTL_FORMAT_XRGB8888  (4u << 24)   /* format field [27:24]=4 */
#define INTEL_PLANE_CTL_TILED_LINEAR     (0u << 10)   /* tiling  field [12:10]=0 */

typedef struct {
    u32 ctl;      /* -> INTEL_PLANE_CTL(t)    */
    u32 stride;   /* -> INTEL_PLANE_STRIDE(t) : 64-byte units, [11:0]        */
    u32 size;     /* -> INTEL_PLANE_SIZE(t)   : (h-1)<<16 | (w-1)            */
    u32 surf;     /* -> INTEL_PLANE_SURF(t)   : page-aligned framebuffer base */
} intel_plane_config_t;

/* Compute the plane registers for a linear XRGB8888 framebuffer of `width` x
 * `height`, `stride_bytes` per row, at GPU address `fb_addr`.  `fb_addr` is a
 * GGTT offset, NOT a physical address - the display engine reads the plane
 * through the GGTT (see below).  Pure arithmetic; returns 0 on degenerate
 * input. */
int intel_plane_config_xrgb8888(intel_plane_config_t *out, u64 fb_addr,
                                u32 width, u32 height, u32 stride_bytes);

/* ---- the GGTT: how our framebuffer becomes something the iGPU can scan ----
 *
 * Every display access on Intel goes through the Global GTT: a plane's SURF
 * register is a GGTT OFFSET, and the GGTT translates it to a physical page.
 * So to make the iGPU show a framebuffer WE allocated (rather than reusing the
 * firmware's), its pages must be written into GGTT entries and the plane
 * pointed at the offset of the first one.  Bit layout transcribed from i915
 * (intel_gtt.h), checked by tools/verify_intel_regs.py: a GGTT entry is 64-bit,
 * the page-aligned physical address in bits 45:12, presence in bit 0.  Meteor
 * and Arrow Lake (mtl_ggtt_pte_encode) add two PAT-index bits at 52/53; index 0
 * is write-back-coherent, which is what a CPU-drawn framebuffer wants, so they
 * stay clear.  The iGPU has no local memory, so the LM bit (1) stays clear too. */
#define INTEL_GGTT_PTE_PRESENT   (1ull << 0)          /* GEN8_PAGE_PRESENT      */
#define INTEL_GGTT_PTE_LM        (1ull << 1)           /* GEN12_GGTT_PTE_LM      */
#define INTEL_GGTT_PTE_PAT0      (1ull << 52)          /* MTL_GGTT_PTE_PAT0      */
#define INTEL_GGTT_PTE_PAT1      (1ull << 53)          /* MTL_GGTT_PTE_PAT1      */
#define INTEL_GGTT_PTE_ADDR_MASK 0x00003FFFFFFFF000ull /* GENMASK_ULL(45,12)     */
#define INTEL_GGTT_PAGE          4096u

/* One GGTT entry for a system-memory scanout page at physical `phys`: the
 * address masked into place, present, coherent (PAT 0), not local memory. */
u64 intel_ggtt_scanout_pte(u64 phys);

/* A 64-bit register-write sink - GGTT entries are 64-bit, unlike the 32-bit
 * mode registers - so the mapping SEQUENCE can be captured on the host and
 * drive real MMIO on the card, the same shape as intel_display_write_mode. */
typedef void (*intel_ggtt_write_fn)(void *ctx, u32 pte_index, u64 value);

/* Map a `fb_bytes` framebuffer at physical `fb_phys` into the GGTT, one entry
 * per 4 KiB page, starting at entry `first_slot`.  Emits each entry through
 * `wr`.  Returns the GGTT OFFSET (in bytes) to program into the plane's SURF -
 * i.e. first_slot * 4096 - or 0 on degenerate input.  `*pages_out` gets the
 * number of entries written.  The MMIO write itself, and finding where the
 * GGTT lives on the real card, need the iGPU; the mapping is what is verified. */
u64 intel_ggtt_map_scanout(intel_ggtt_write_fn wr, void *ctx, u32 first_slot,
                           u64 fb_phys, u64 fb_bytes, u32 *pages_out);

/* The DP transcoder DATA M/N: data_m/data_n is the link bandwidth a mode uses.
 * Verified by the ratio equalling the physical utilization.  link_m/link_n is
 * the pixel-clock-to-link-symbol-clock ratio (also needed by the transcoder). */
typedef struct { u32 data_m, data_n, link_m, link_n, tu; } intel_dp_mn_t;
int intel_dp_compute_data_mn(intel_dp_mn_t *out, u32 pixel_khz, u32 bpp,
                             u32 lanes, u32 link_rate_khz);

/* A register-write sink, so the modeset write SEQUENCE can be captured and
 * checked on the host and drive real MMIO on the card - the same shape the GSP
 * boot uses to be model-tested before it ever touches silicon. */
typedef void (*intel_reg_write_fn)(void *ctx, u32 reg, u32 value);

/* Emit the transcoder timing registers plus the DP DATA and LINK M/N for a mode
 * on transcoder `t`.  IMPORTANT - this is the pixel-pacing arithmetic only, NOT
 * a complete modeset.  A real refresh (#5) or resolution (#6) change on this
 * hardware is a full modeset: i915 disables the plane and transcoder, reprograms
 * timing, recomputes watermarks/DDB, re-trains the link if the rate crossed a
 * band (e.g. 60 -> 240 Hz can cross HBR2 -> HBR3), and re-enables - poking these
 * registers on a LIVE transcoder does not by itself change what the panel shows.
 * So treat this as a verified building block, not the capability.  Returns the
 * number of registers written; the MMIO and the surrounding modeset need the
 * real iGPU. */
u32 intel_display_write_mode(intel_reg_write_fn wr, void *ctx, int t,
                             const intel_trans_timing_t *timing,
                             const intel_dp_mn_t *mn);

/* Point pipe `t`'s primary plane at a framebuffer, so the iGPU scans out OUR
 * desktop.  Emits STRIDE, SIZE, CTL, then SURF - SURF last, because writing it
 * is what latches the whole plane update on the next vblank (the other three
 * are shadowed until then).  This is the "flip the desktop onto the iGPU" that
 * #2 needs: it reuses the firmware's already-running pipe and only repoints the
 * plane, so no PLL or mode change is involved.  Returns the number of registers
 * written (4).  The MMIO write needs the real iGPU; the sequence is verified. */
u32 intel_plane_write_scanout(intel_reg_write_fn wr, void *ctx, int t,
                              const intel_plane_config_t *cfg);

#endif
