/* Original serial-per-lane compaction, retained for <=32-primitive native draws
 * where cooperative shuffle overhead does not amortize. Also the CUDA timing
 * baseline for larger draws. Same ABI/validation as cooperative compaction. */
#include "../include/kestrel/shader_setup.h"
extern "C" __global__ void shader_setup_compact_small(
    const kshr_command_t *input, const kshs_primitive_t *setup,
    kshr_command_t *output, kshs_rect_t *rects, kshs_compact_status_t *status,
    unsigned long long input_bytes, unsigned long long setup_bytes,
    unsigned long long output_bytes, unsigned long long rect_bytes,
    unsigned long long status_bytes, unsigned triangles) {
    unsigned long long lane=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;
    if (!triangles || triangles>KSHS_MAX_TRIANGLES || lane>=triangles ||
        !status || status_bytes<KSHS_COMPACT_STATUS_BYTES(triangles)) return;
    const unsigned id=(unsigned)lane;
    status[id].result=KSH_BAD_RESOURCE;
    status[id].count=0;status[id].first=0;status[id].total=0;
    if (!input || !setup || !output || !rects ||
        input_bytes<KSHS_COMMAND_BYTES(triangles) || setup_bytes<KSHS_RESULT_BYTES(triangles)) return;
    unsigned first=0,total=0;
    for (unsigned p=0;p<triangles;p++) {
        unsigned count=setup[p].count;
        if (setup[p].result!=KSH_COMPLETE || count>KSHS_OUTPUT_TRIANGLES) {
            status[id].result=KSH_BAD_PROGRAM;return;
        }
        if(p<id)first+=count;
        total+=count;
    }
    if ((unsigned long long)total*sizeof(kshr_command_t)>output_bytes ||
        KSHS_COMPACT_RECT_BYTES(total)>rect_bytes) return;
    unsigned count=setup[id].count;
    if(first>total || count>total-first)return;
    for(unsigned i=0;i<count;i++) {
        const kshs_rect_t *r=&setup[id].bounds[i];
        const kg3d_command_t *c=&input[id*KSHS_OUTPUT_TRIANGLES+i].raster;
        if(r->x<0 || r->y<0 || r->width<=0 || r->height<=0 ||
           r->x>=32768 || r->y>=32768 || r->width>32768-r->x || r->height>32768-r->y ||
           c->x!=r->x || c->y!=r->y || c->width!=r->width || c->height!=r->height ||
           (c->flags & ~(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND))) {
            status[id].result=KSH_BAD_PROGRAM;return;
        }
    }
    for(unsigned i=0;i<count;i++) {
        output[first+i]=input[id*KSHS_OUTPUT_TRIANGLES+i];rects[first+i]=setup[id].bounds[i];
    }
    status[id].count=count;status[id].first=first;status[id].total=total;status[id].result=KSH_COMPLETE;
}
