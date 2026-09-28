/* edid.h - reading a monitor's EDID: what it is and what modes it accepts.
 *
 * EDID is the 128-byte block every monitor reports over the display cable.  It
 * is where the real answers to "what resolutions?" (#6), "what refresh rates?"
 * (#5) and "which monitor is this?" (#7, multi-display) come from - not
 * guesses, the panel's own description of itself.  The parsing is the same
 * whatever adapter read the bytes (Intel, NVIDIA, AMD, or the firmware), so it
 * lives here once rather than inside each driver.
 *
 * Verified against real monitors (edid_host_test.c) - the EDIDs of the panels
 * on the target machine, an Acer XV271U, an Acer X27U, an AOC Q27G4ZD and an
 * MSI MAG 272Q X24, all 2560x1440 with refresh ranges to 180-240 Hz.
 */
#ifndef KESTREL_EDID_H
#define KESTREL_EDID_H

#if defined(EDID_HOST_TEST) || defined(INTEL_HOST_TEST)
#include <stddef.h>          /* host tests provide u8/u16/u32/u64 themselves */
#else
#include "kernel.h"
#endif

#define EDID_BLOCK_SIZE   128
#define EDID_MAX_MODES    128  /* base + CTA SVD/DTD + DisplayID timings */

typedef struct {
    u32 pixel_clock_khz;    /* the dot clock, in kHz                          */
    u16 hactive, vactive;   /* the visible resolution                        */
    u16 htotal, vtotal;     /* including blanking, so refresh can be derived  */
    u16 hsync_off, hsync_w; /* front porch and sync pulse, in pixels          */
    u16 vsync_off, vsync_w; /* the same vertically, in lines                  */
    u32 refresh_mhz;        /* millihertz: 59940 == 59.94 Hz                  */
    u8  flags;              /* detailed-timing byte 17 (sync/interlace bits)  */
} edid_mode_t;

typedef struct {
    u8 present, scdc_present, lte_340_scramble;
    u8 max_frl_rate;
    u32 max_tmds_clock_khz;
    u8 dsc_1p2, dsc_all_bpp, dsc_native_420;
    u8 dsc_max_frl_rate, dsc_max_slices;
    u16 dsc_max_pclk_per_slice_mhz, dsc_total_chunk_kbytes;
} edid_hdmi_forum_t;

typedef struct {
    int  valid;             /* the block had a good header and checksum       */
    char manufacturer[4];   /* three-letter PNP id, e.g. "ACR", null-terminated */
    u16  product_code;
    char model[16];         /* the monitor's name, from its 0xFC descriptor   */
    u8   refresh_min_hz;    /* the 0xFD range descriptor: the panel will take  */
    u8   refresh_max_hz;    /*   any refresh between these (this is what makes  */
                            /*   a refresh-rate CHOICE meaningful - #5)         */
    u8   hfreq_min_khz, hfreq_max_khz;
    int  block_count;       /* 1 for a base-only EDID, more with extensions   */
    int  mode_count;        /* detailed timings across the base + CTA blocks  */
    edid_mode_t modes[EDID_MAX_MODES];
    edid_mode_t native;                  /* the preferred (first) detailed one */
    /* These are deliberately separate.  A monitor can advertise its largest
     * raster at one rate and its fastest rate at a smaller raster.  The third
     * value is the fastest *advertised timing at the largest resolution* -- a
     * real resolution/rate pair, never two unrelated maxima spliced together. */
    edid_mode_t highest_resolution;
    edid_mode_t highest_refresh;
    edid_mode_t highest_resolution_refresh;
    u32  max_refresh_mhz_at_native;      /* highest rate seen at the native
                                          * resolution - the top of the refresh
                                          * choice a panel really offers (#5)  */
    edid_hdmi_forum_t hdmi_forum;
} edid_info_t;

/* Parse one 128-byte EDID base block.  Returns non-zero and fills `out` when
 * the header and checksum are good; returns 0 (out->valid = 0) otherwise. */
int edid_parse(const u8 *block, u32 len, edid_info_t *out);

#endif
