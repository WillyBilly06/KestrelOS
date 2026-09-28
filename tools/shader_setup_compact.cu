/* Stable packing of trusted GPU triangle-setup output, without returning
 * vertex/varying records to the CPU. Run only AFTER the setup fence. One lane
 * per input primitive; each warp cooperatively scans immutable small counts.
 * No unordered atomic append, shared storage, block/grid barrier or local scratch.
 *
 * Native kernel integration must resolve/pin all allocations, reject aliases,
 * initialize every status to PENDING, and require COMPLETE from every lane
 * after retirement. This asset alone does not enable a user rendering path. */
#include "../include/kestrel/shader_setup.h"

extern "C" __global__ void shader_setup_compact(
    const kshr_command_t *input, const kshs_primitive_t *setup,
    kshr_command_t *output, kshs_rect_t *rects, kshs_compact_status_t *status,
    unsigned long long input_bytes, unsigned long long setup_bytes,
    unsigned long long output_bytes, unsigned long long rect_bytes,
    unsigned long long status_bytes, unsigned triangles) {
    unsigned long long lane=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;
    const unsigned warp_lane=threadIdx.x&31u;
    const unsigned long long warp_first=lane-warp_lane;
    if (!triangles || triangles>KSHS_MAX_TRIANGLES || warp_first>=triangles ||
        !status || status_bytes<KSHS_COMPACT_STATUS_BYTES(triangles)) return;
    const unsigned id=(unsigned)lane;
    const bool active=lane<triangles;
    if (active) {
        status[id].result=KSH_BAD_RESOURCE;
        status[id].count=0;status[id].first=0;status[id].total=0;
    }
    /* Full warps participate even when the final primitive group is partial.
     * The native launcher uses 64 threads; reject partial hardware warps rather
     * than naming nonexistent lanes in a full-mask shuffle. */
    if ((blockDim.x&31u) || !input || !setup || !output || !rects ||
        (((unsigned long long)input|(unsigned long long)output)&15u) ||
        input_bytes<KSHS_COMMAND_BYTES(triangles) ||
        setup_bytes<KSHS_RESULT_BYTES(triangles)) return;

    unsigned first=0,total=0;
    {
        unsigned before=0,bad=0;
        /* Each warp reads the table once, instead of once per primitive. Reduce
         * the total and counts before this warp, then scan this warp's own counts
         * for a stable exclusive prefix. Every warp independently validates ALL
         * setup statuses; no cross-block synchronization or unchecked count exists. */
        for (unsigned p=warp_lane;p<triangles;p+=32u) {
            unsigned count=setup[p].count;
            if (setup[p].result!=KSH_COMPLETE || count>KSHS_OUTPUT_TRIANGLES) {
                bad=1;count=0;
            }
            if (p<warp_first) before+=count;
            total+=count;
        }
        for (unsigned shift=16;shift;shift>>=1) {
            total+=__shfl_xor_sync(0xffffffffu,total,shift);
            before+=__shfl_xor_sync(0xffffffffu,before,shift);
            bad|=__shfl_xor_sync(0xffffffffu,bad,shift);
        }
        if (bad) {
            if (active) status[id].result=KSH_BAD_PROGRAM;
            return;
        }
        unsigned count=active?setup[id].count:0u;
        unsigned prefix=count;
        for (unsigned shift=1;shift<32u;shift<<=1) {
            unsigned preceding=__shfl_up_sync(0xffffffffu,prefix,shift);
            if (warp_lane>=shift) prefix+=preceding;
        }
        first=before+prefix-count;
    }
    if ((unsigned long long)total*sizeof(kshr_command_t)>output_bytes ||
        KSHS_COMPACT_RECT_BYTES(total)>rect_bytes) return;
    unsigned count=active?setup[id].count:0u;
    bool valid=active && first<=total && count<=total-first;

    /* Validate this entire source span before publishing any of its records.
     * Metadata disagreement invalidates the batch, not a truncated draw.
     * Finite positions/varyings and triangle state are guaranteed by the
     * trusted producer; this is not an API for arbitrary user GPU commands. */
    for (unsigned i=0;valid && i<count;i++) {
        const kshs_rect_t *r=&setup[id].bounds[i];
        const kg3d_command_t *c=&input[id*KSHS_OUTPUT_TRIANGLES+i].raster;
        if (r->x<0 || r->y<0 || r->width<=0 || r->height<=0 ||
            r->x>=32768 || r->y>=32768 || r->width>32768-r->x || r->height>32768-r->y ||
            c->x!=r->x || c->y!=r->y || c->width!=r->width || c->height!=r->height ||
            (c->flags & ~(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND))) {
            status[id].result=KSH_BAD_PROGRAM;valid=false;
        }
    }
    {
        /* Command records are large (576 bytes). One record per lane made
         * adjacent lanes access addresses 4608 bytes apart in the setup array.
         * Cooperate on each valid primitive's contiguous span instead. Prefix
         * order and invalid-record holes remain exactly the same. */
        static_assert(sizeof(kshr_command_t)%sizeof(uint4)==0,"vector-copy record alignment");
        unsigned writers=__ballot_sync(0xffffffffu,valid && count);
        for (unsigned leader=0;leader<32u;leader++) {
            if (!(writers&(1u<<leader))) continue; // Uniform across this warp.
            unsigned records=__shfl_sync(0xffffffffu,count,leader);
            unsigned target=__shfl_sync(0xffffffffu,first,leader);
            const uint4 *src=(const uint4 *)(input+(warp_first+leader)*KSHS_OUTPUT_TRIANGLES);
            uint4 *dst=(uint4 *)(output+target);
            unsigned vectors=records*(sizeof(kshr_command_t)/sizeof(uint4));
            for (unsigned at=warp_lane;at<vectors;at+=32u) dst[at]=src[at];
        }
        if (valid)for (unsigned i=0;i<count;i++)rects[first+i]=setup[id].bounds[i];
        // Do not publish a primitive's COMPLETE before its cooperating lanes
        // have written their pieces. Native retirement is still mandatory.
        __syncwarp(0xffffffffu);
        if (!valid) return;
    }
    status[id].count=count;status[id].first=first;status[id].total=total;
    status[id].result=KSH_COMPLETE;
}
