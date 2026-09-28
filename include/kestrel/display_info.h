/* Versioned per-output state. Legacy ENUM_DISPLAY describes only firmware. */
#ifndef KESTREL_DISPLAY_INFO_H
#define KESTREL_DISPLAY_INFO_H
#include <stdint.h>
#define KDISPLAY_INFO_VERSION 1u
#define KDISPLAY_MAX_OUTPUTS 8u
#define KDISPLAY_MAX_MODES 1024u
#define KDISPLAY_CONNECTED 1u
#define KDISPLAY_ENABLED 2u
#define KDISPLAY_PRIMARY 4u
#define KDISPLAY_MODES_TRUNCATED 8u
#define KDISPLAY_STALE 16u /* display event pending; do not apply cached choices */
#define KDISPLAY_DETECTED_ONLY 32u /* connected but not part of the committed desktop */
#define KDISPLAY_OUTPUT_SCALED 64u /* output timing differs from retained desktop pixels */
#define KDISPLAY_MODE_PREFERRED 1u
#define KDISPLAY_MODE_INTERLACED 2u
#define KDISPLAY_MODE_CURRENT 4u

typedef struct {
    uint32_t width, height, refresh_millihz, flags;
} kdisplay_mode_t;

typedef struct {
    uint32_t version, output_id, connector_id, flags;
    uint32_t width, height, refresh_millihz, mode_count;
    int32_t x, y;
    uint32_t logical_width, logical_height;
    uint32_t rotation_degrees, group_mode;
    char manufacturer[4], model[32], connector[16];
    uint32_t reserved; /* topology generation; 0 = legacy boot-only snapshot */
} kdisplay_output_t;

/* Proposed active outputs, identified independently of enumeration order.
 * Omitted outputs are disabled. One entry represents "only this screen";
 * group_mode is 1 (extend) or 2 (duplicate). All reserved fields must be zero.
 * mode_index is the original ENUM_DISPLAY_OUTPUT_MODE index, not a sorted UI
 * row. generation must match every output/mode used to construct the request. */
#define KDISPLAY_CONFIGURATION_VERSION 1u
typedef struct {
    uint32_t output_id, connector_id, mode_index, rotation_degrees;
    int32_t x, y;
    uint32_t reserved[2];
} kdisplay_selection_t;
typedef struct {
    uint32_t version, generation, count, primary_output_id;
    uint32_t group_mode, reserved[3];
    kdisplay_selection_t outputs[KDISPLAY_MAX_OUTPUTS];
} kdisplay_configuration_t;

/* Geometry/cached-mode preflight only: no allocation, atomic bandwidth/DSC
 * validation, modeset, persistent save or promise that Apply will succeed. */
typedef struct {
    uint32_t version, generation, width, height;
    uint32_t gpu_pitch, output_count;
    int32_t origin_x, origin_y;
    uint64_t gpu_bytes;
    uint32_t reserved[2];
} kdisplay_configuration_check_t;
/* PREPARE retains private replacement scanouts with a 30-second lease. It
 * validates through NVIDIA but never commits, maps or publishes them. The
 * token is owner-bound; cancel it when abandoning the request. Expiry, owner
 * exit or a topology change also cancels it; reclamation waits for the render
 * guard. Commit is not exposed yet. */
typedef struct {
    kdisplay_configuration_check_t configuration;
    uint64_t token, expires_us;
} kdisplay_prepared_configuration_t;
/* Live physical-output timing only. Desktop canvas/mappings remain unchanged;
 * NVIDIA scales the retained scanout to the selected, validated output mode.
 * Apply returns a positive owner-bound token; confirm within 20 seconds or
 * the kernel restores the previous timing. No startup preference is saved. */
typedef struct {
    uint32_t generation, output_id, connector_id, mode_index;
} kdisplay_output_mode_request_t;
_Static_assert(sizeof(kdisplay_output_mode_request_t)==16,"output mode ABI");
_Static_assert(sizeof(kdisplay_selection_t)==32,"display selection ABI");
_Static_assert(sizeof(kdisplay_configuration_t)==288,"display configuration ABI");
_Static_assert(sizeof(kdisplay_configuration_check_t)==48,"display check ABI");
_Static_assert(sizeof(kdisplay_prepared_configuration_t)==64,"display prepare ABI");
#endif
