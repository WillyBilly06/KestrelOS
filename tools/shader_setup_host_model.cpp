/* HOST-ONLY syscall-boundary model. This never represents GPU execution or
 * validates native SASS/fences/compaction/raster pixels. Kept out of OS builds.
 * Reuse setup control flow to retain the GL harness's topology/clip assertions
 * after triangle arrays no longer perform CPU primitive assembly in libgl. */
#include <cstring>
#define __device__
#define __global__
#define __forceinline__ inline
struct model_dim { unsigned x; };
static model_dim blockIdx,blockDim,threadIdx;
static inline float __fadd_rn(float a,float b){volatile float r=a+b;return r;}
static inline float __fsub_rn(float a,float b){volatile float r=a-b;return r;}
static inline float __fmul_rn(float a,float b){volatile float r=a*b;return r;}
static inline float __fdiv_rn(float a,float b){volatile float r=a/b;return r;}
static inline void __threadfence(){} // sequential host model, NOT a device fence
#include "shader_setup.cu"

extern "C" int test_model_setup(const kshs_submission_t *s,const float *registers,
    const ksh_status_t *status,void *workspace,kshr_command_t *out,
    unsigned capacity,unsigned *count) {
    *count=0;
    kshs_storage_layout_t layout;
    if(!kshs_storage_layout(s->setup.triangle_count,&layout))return 0;
    unsigned char *base=(unsigned char*)workspace;
    std::memset(base,0,(size_t)layout.bytes);
    auto *commands=(kshr_command_t*)(base+layout.commands);
    auto *results=(kshs_primitive_t*)(base+layout.primitives);
    auto *scratch=(kshs_vertex_t*)(base+layout.clip);
    blockDim.x=1;threadIdx.x=0;
    for(blockIdx.x=0;blockIdx.x<s->setup.triangle_count;blockIdx.x++)
        shader_setup(registers,status,commands,results,scratch,&s->setup,
            KSHS_REGISTER_BYTES(s->setup.lanes),KSHS_VERTEX_STATUS_BYTES(s->setup.lanes),
            KSHS_COMMAND_BYTES(s->setup.triangle_count),KSHS_RESULT_BYTES(s->setup.triangle_count),
            KSHS_SCRATCH_BYTES(s->setup.triangle_count),sizeof s->setup);
    unsigned total=0;
    for(unsigned i=0;i<s->setup.triangle_count;i++){
        if(results[i].result!=KSH_COMPLETE||results[i].count>KSHS_OUTPUT_TRIANGLES||
           results[i].count>capacity-total)return 0;
        total+=results[i].count;
    }
    // Stable host copy models only the syscall's command result. Native
    // compaction shader and kernel admission require their own later checks.
    unsigned at=0;
    for(unsigned i=0;i<s->setup.triangle_count;i++){
        std::memcpy(out+at,commands+i*KSHS_OUTPUT_TRIANGLES,results[i].count*sizeof(*out));
        at+=results[i].count;
    }
    *count=total;return 1;
}
