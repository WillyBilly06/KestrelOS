#ifndef KESTREL_NVKMS_KAPI_CLIENT_H
#define KESTREL_NVKMS_KAPI_CLIENT_H

#include "kernel.h"
#include "../include/kestrel/display_info.h"

/* Generation-bound capability snapshot; never probes AUX from a UI call. */
bool nvkms_kapi_display_output(u32 index, kdisplay_output_t *out);
bool nvkms_kapi_display_mode(u32 index, u32 mode, kdisplay_mode_t *out);
/* Read-only cached-mode/geometry check. Caller holds the render guard.
 * Returns 0 or a negative kernel errno; no hardware change or allocation. */
int nvkms_kapi_check_configuration(const kdisplay_configuration_t *configuration,
                                  kdisplay_configuration_check_t *out);
/* Caller verifies current framebuffer ownership, holds render guard and
 * supplies the nonrecycled address-space owner. */
int nvkms_kapi_prepare_configuration(u64 owner,const kdisplay_configuration_t *configuration,
                                    kdisplay_prepared_configuration_t *out);
int nvkms_kapi_cancel_configuration(u64 owner,u64 token);
/* Requires the render guard. Retains all scanouts/mappings while scaling the
 * physical output; no replacement framebuffer is exposed. */
s64 nvkms_kapi_apply_output_mode(u64 owner,const kdisplay_output_mode_request_t *request);
int nvkms_kapi_finish_output_mode(u64 owner,u64 token,bool confirm);

/* Kernel-only native compositor path. Caller serializes shared render work;
 * this does not update the legacy CPU framebuffer shadow. */
bool nvkms_kapi_runtime_present_surface(u64 owner, u64 surface,
                                        s32 x, s32 y, s32 w, s32 h);

#define KESTREL_NVKMS_MAX_TEST_DISPLAYS 4

/* Retained independently of the bounded kernel log ring. Source 0 means the
 * EDID was not locally validated, 1 is live AUX data, 2 is an explicit bound
 * override. A valid EDID or GPU output CRC does not prove sink link lock. */
#define KESTREL_NVKMS_EDID_BYTES 2048
typedef struct {
    u32 handle, connector, source, waited_ms, size;
    bool is_dp;
    char manufacturer[4], model[16];
    u8 bytes[KESTREL_NVKMS_EDID_BYTES];
} nvkms_display_evidence_t;

typedef struct {
    bool ran;
    bool table_ok;
    bool device_ok;
    bool ownership_ok;
    bool resources_ok;
    bool validated;
    bool committed;
    /* GPU-side scanout check via the hardware output CRC.  NVIDIA's Linux
     * DRM legacy CRC ioctl deliberately publishes outputCrc32 (the target
     * SF/SOR), not rasterGeneratorCrc32.  "committed" therefore remains only
     * an accepted transaction; scanout_confirmed means the final output stage
     * changed between two deliberately different surfaces. Neither this nor
     * the accepted transaction proves the sink received a usable DP signal. */
    bool crc_supported;
    bool scanout_confirmed;
    /* A changing compositor CRC proves that the primary surface was fetched,
     * even if a later raster/output stage remains black. */
    bool surface_fetch_confirmed;
    /* Sentinel write+read-back through our scanout CPU mapping: proves our
     * writes reach the mapped VRAM (rules out an incoherent BAR1 map).  If this
     * is true but scanout_confirmed is false, the wall is promotion, not writes. */
    bool vram_writeback_ok;
    u32 enumerated_gpus;
    u32 connected_displays;
    u32 active_displays;
    u32 active_heads_mask;
    u32 evidence_count;
    nvkms_display_evidence_t evidence[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
    struct {
        u32 handle;
        u32 head;
        u32 width;
        u32 height;
        u32 refresh_millihz;
        u32 pixel_clock_hz;
        u32 compositor_crc_a;
        u32 compositor_crc_b;
        u32 raster_crc_a;
        u32 raster_crc_b;
        u32 output_crc_a;
        u32 output_crc_b;
        bool compositor_crc_supported;
        bool raster_crc_supported;
        bool output_crc_supported;
    } display[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
    const char *failed_at;
} nvkms_kapi_test_result_t;

typedef struct {
    bool ran;
    bool post_codec;
    bool two_d_pass;
    bool three_d_pass;
    u32 active_displays;
    u32 active_heads_mask;
    u32 two_d_drawn_mask;
    u32 two_d_pixel_mask;
    u32 two_d_crc_mask;
    u32 three_d_drawn_mask;
    u32 three_d_pixel_mask;
    u32 three_d_crc_mask;
    const char *failed_at;
} nvkms_kapi_accel_result_t;

/* Establish a real signal on every connected display using the complete
 * NVIDIA 595.99.02 NVKMS state machine, then show the test pattern sequence.
 * The committed scanouts deliberately remain allocated after this returns. */
bool nvkms_kapi_run_display_test(nvkms_kapi_test_result_t *result);

/* Render full-screen 2D and 3D scenes with CAB5/SM hardware into every active
 * head's inactive VRAM buffer, atomically present them, and require independent
 * pixel-readback plus hardware-CRC proof from every monitor. */
bool nvkms_kapi_run_visible_accel_test(nvkms_kapi_accel_result_t *result,
                                       bool post_codec);

/* Turn the test-owned NVKMS state into the production desktop state.  The
 * framebuffer is a logical system-memory compositor surface; these operations
 * route its damage/2D/3D work to every physical head selected by the layout. */
bool nvkms_kapi_publish_runtime_framebuffer(void);
bool nvkms_kapi_runtime_ready(void);
/* Display ownership, independent of engine health (a failed native engine must
 * not make callers write CPU pixels into a non-scanout shadow framebuffer). */
bool nvkms_kapi_runtime_selected(void);
bool nvkms_kapi_runtime_matches_gpu(u8 bus, u8 slot, u8 func);
/* Exercise the exact post-handoff desktop sequence after an idle interval:
 * GR triangle, small CAB5 upload, fill, and overlapping-safe copy, each checked
 * against the live NVKMS allocation. */
bool nvkms_kapi_runtime_accel_selftest(void);
bool nvkms_kapi_runtime_read_pixel(s32 x, s32 y, u32 *pixel);
bool nvkms_kapi_runtime_present(const u32 *pixels, u32 width, u32 height,
                                u32 source_stride, s32 x, s32 y);
bool nvkms_kapi_runtime_fill(s32 x, s32 y, s32 w, s32 h, u32 colour);
bool nvkms_kapi_runtime_copy(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h);
int  nvkms_kapi_runtime_draw(const float *vertices, u32 triangles);

#endif
