/* Native off-screen VRAM surfaces. Kernel implementation interface.
 * The caller must hold nv_render_try_begin()/nv_render_end() across these
 * operations and related scanout/VMM changes. Every successful operation is
 * synchronously retired. Never recursively acquire the transaction guard.
 * Owner is a non-recycled address-space identity, not a user-supplied handle.
 * GPUOP_SURFACE validates/snapshots user requests; exited owners are reaped
 * under the same guard. No userspace pointer or raw user GPU VA reaches here.
 */
#ifndef KESTREL_NV_SURFACE_H
#define KESTREL_NV_SURFACE_H
#include "kernel.h"
#include "../include/kestrel/gpu2d.h"
#include "../include/kestrel/gpu3d.h"
#include "../include/kestrel/shader_vm.h"
#include "../include/kestrel/shader_raster.h"

typedef struct { u32 width, height, pitch; u64 bytes; } nv_surface_info_t;
/* Pure allocation-layout check; does not require the render guard. */
bool nv_surface_query_dimensions(u32 width, u32 height, nv_surface_info_t *out);
u64 nv_surface_create(u64 owner, u32 width, u32 height);
/* Set pool_exhausted only for a full reusable-slot pool, never for an unknown
 * RM, mapping or channel failure. The render transaction serializes this test. */
u64 nv_surface_create_with_pressure(u64 owner, u32 width, u32 height,
                                    bool *pool_exhausted);
bool nv_surface_destroy(u64 owner, u64 handle);
bool nv_surface_release_owner(u64 owner);
void nv_surface_reap_exited(void); /* caller holds render guard */
bool nv_surface_info(u64 owner, u64 handle, nv_surface_info_t *out);
bool nv_surface_transfer(u64 owner, u64 handle, u64 offset,
                         void *data, u32 bytes, bool read);
bool nv_surface_draw(u64 owner, u64 destination, u64 source,
                     const kg2d_command_t *commands, u32 count,
                     u32 x, u32 y, u32 width, u32 height);
bool nv_surface_draw3d(u64 owner, u64 destination, u64 depth, u64 texture,
                       const kg3d_command_t *commands, u32 count,
                       u32 x, u32 y, u32 width, u32 height);
/* Copy/scale a source viewport into the currently bound native scanout on the
 * GPU. Damage is in physical output coordinates; sampling remains relative to
 * the full output. Does not read back or update a CPU framebuffer shadow. */
bool nv_surface_present(u64 owner, u64 source,
                        u32 view_x, u32 view_y, u32 view_w, u32 view_h,
                        u32 x, u32 y, u32 width, u32 height, u32 rotation);
/* True means retired, not that every invocation succeeded. The caller must
 * inspect ksh_status_t results before treating any shader output as valid. */
bool nv_surface_shader(u64 owner, u64 registers, u64 status, u64 texture,
                       const ksh_dispatch_t *job);
bool nv_surface_shader_raster(u64 owner, u64 destination, u64 depth, u64 texture,
                              const kshr_submission_t *submission);
#endif
