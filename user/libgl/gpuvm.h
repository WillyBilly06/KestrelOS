/* Native programmable-stage transport. Single caller per session, matching GL
 * context ownership. This does not itself connect rasterization or GL routing.
 * Zero-initialize once. Never reset a quarantined session to reuse its handles.
 */
#ifndef KESTREL_GL_GPUVM_H
#define KESTREL_GL_GPUVM_H
#include "../libc/kestrel.h"
#include "../../include/kestrel/shader_vm.h"
#include "../../include/kestrel/shader_raster.h"
#include "../../include/kestrel/shader_setup.h"
typedef struct { uint64_t handle, bytes; } gl_gpu_vm_buffer_t;
typedef struct {
    gl_gpu_vm_buffer_t registers, status, textures, raster_scratch;
    ksh_status_t *results;
    unsigned result_capacity, lanes, bad_lane;
    intptr_t last_error;
    unsigned lane_error;
    bool ready, quarantined, texture_valid, raster_output, vertex_output;
    uint64_t texture_bytes;
    kshr_submission_t *raster_packet;
    /* Small vertex batches amortize CE launch/fence cost with one bounded,
     * fully initialized upload. Not a CPU render target or shader output. */
    unsigned char *vertex_upload;
    unsigned vertex_upload_capacity;
    /* Exact-byte shadow of initialized input planes, never GPU outputs. Only
     * planes not written by the preceding program may remain reusable. */
    unsigned char *vertex_shadow;
    unsigned vertex_shadow_capacity, vertex_shadow_lanes;
    bool vertex_shadow_valid[SR_REGISTERS];
    gl_gpu_vm_buffer_t geometry_workspace,geometry_status;
    kshs_submission_t *geometry_packet;
    unsigned geometry_triangles;
    uint64_t geometry_instructions;
} gl_gpu_vm_t;

/* Shared by staging initialization and transport. Include partial-write
 * destinations, all branch bodies and MATMUL's implicit adjacent columns.
 * Skipping a register here means neither side may read its staging bytes. */
static inline void gl_gpu_vertex_registers(const ksh_dispatch_t *job,bool used[SR_REGISTERS]) {
    for(unsigned r=0;r<SR_REGISTERS;r++)used[r]=false;
    used[SR_POSITION]=true;
    for(unsigned r=SR_VARYING;r<SR_VARYING+SR_VARYING_N;r++)used[r]=true;
    for(unsigned pc=0;pc<job->code_count;pc++){
        const sh_instruction_t *in=&job->code[pc];
        if(in->dst<SR_REGISTERS)used[in->dst]=true;
        for(unsigned k=0;k<3;k++)if(in->src[k]<SR_REGISTERS)used[in->src[k]]=true;
        if(in->op==SH_MATMUL)for(unsigned k=1;k<4;k++)
            if((unsigned)in->src[0]+k<SR_REGISTERS)used[in->src[0]+k]=true;
    }
}

/* Registers are SoA, exactly (200*4*job->lanes) float32 entries, initialized by
 * the caller for this batch. Only GPU execution changes shader outputs. Textures
 * are immutable ARGB input data packed according to job->textures offsets.
 * Returns true only after the fence AND all lane status checks pass. Discard is
 * a valid result: consumers must exclude discarded lanes when composing pixels.
 */
bool gl_gpu_vm_execute(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                       const float *registers, uint64_t register_floats,
                       const void *textures, uint64_t texture_bytes);
/* Explicit immutable atlas upload. Resident calls reuse it until this function
 * replaces/drops it; pointer identity is never treated as texture immutability. */
bool gl_gpu_vm_set_textures(gl_gpu_vm_t *vm, const void *textures, uint64_t bytes);
bool gl_gpu_vm_execute_resident(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                                 const float *registers, uint64_t register_floats);
/* Application vertex path: upload bytecode-accessed register spans and the
 * position/varying outputs, not all 200 register planes. GPU bytecode execution
 * is unchanged; readback is limited to those initialized vertex outputs. */
bool gl_gpu_vm_execute_vertex(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                              const float *registers, uint64_t register_floats);
/* Raster output stays in destination/depth. COMPLETE (including zero executed
 * for uncovered pixels) is required from EVERY lane. False invalidates the
 * destination batch; callers must not present partly written output. */
bool gl_gpu_vm_raster(gl_gpu_vm_t *vm, uint64_t destination, uint64_t depth,
                       const kshr_job_t *job);
/* Consumes the last retired vertex job directly on GPU: setup, compaction and
 * raster. Geometry/status allocations are distinct from vertex input/status.
 * Failure never retries through CPU clipping or presents partial output. */
bool gl_gpu_vm_geometry(gl_gpu_vm_t *vm,uint64_t destination,uint64_t depth,
                        const kshs_config_t *setup,const kshr_job_t *fragment);
/* Read a contiguous register-plane range after execute succeeds. Output remains
 * SoA. A false result invalidates the batch; callers must discard partial data.
 * This supports vertex readback; fragment composition should remain on GPU. */
bool gl_gpu_vm_read(gl_gpu_vm_t *vm, unsigned first, unsigned count,
                    float *out, uint64_t output_floats);
/* Free all idle GPU buffers and CPU statuses. Failed/unfenced buffers are kept
 * quarantined by the kernel; this function never tries to reuse/free them. */
bool gl_gpu_vm_release(gl_gpu_vm_t *vm);
/* Explicit on-device proof, never a software fallback. Not run automatically
 * during desktop startup; callers choose when to exercise the shared GPU. */
bool gl_gpu_vm_validate(void);
#endif
