/* Programmable raster packet. No GPU/user addresses occur inside this record.
 * One immutable linked fragment stage per ordered batch; clip-space clipping
 * and vertex execution precede this stage. Varyings are already divided by w.
 * Native admission/dispatch and programmable GL array/indexed routing share it.
 */
#ifndef KESTREL_SHADER_RASTER_H
#define KESTREL_SHADER_RASTER_H
#include "gpu3d.h"
#include "shader_vm.h"

typedef struct {
    kg3d_command_t raster;
    float varying[3][SR_VARYING_N][4];
} kshr_command_t;

typedef struct {
    unsigned width,height,pitch,depth_pitch;
    unsigned clip_x,clip_y,clip_w,clip_h;
    unsigned command_count,code_count,budget,texture_count;
    unsigned varying_count,origin_lower_left,reserved[2];
    ksh_texture_t textures[KSH_MAX_TEXTURES];
    float seed[SR_REGISTERS][4]; /* immutable constants/uniforms; other regs zero */
    sh_instruction_t code[SH_MAX_INSTRUCTIONS];
    kshr_command_t commands[KG3D_MAX_COMMANDS];
} kshr_job_t;

/* Bound expensive shader invocations by each primitive's clipped bounding
 * box, not by the entire union of unrelated primitives. Also bound the cheap
 * coverage-test loop separately. Both are conservative; neither depends on
 * triangle coverage, depth rejection or execution of an application shader. */
#if defined(__CUDACC__)
__host__ __device__
#endif
static inline unsigned long long kshr_region_cost(const kshr_command_t *commands,
    unsigned count,unsigned budget,int x,int y,unsigned width,unsigned height) {
    if(!commands || !count || count>KG3D_MAX_COMMANDS || !budget || budget>KSH_MAX_STEPS ||
       !width || !height || width>32768u || height>32768u)return ~0ull;
    unsigned long long covered=0,work=0;
    for(unsigned i=0;i<count;i++){
        const kg3d_command_t *r=&commands[i].raster;
        unsigned weight=(r->flags&KG3D_LINE)?KG3D_LINE_WORK:1u;
        work+=weight;
        long long left=r->x>x?r->x:x,top=r->y>y?r->y:y;
        long long right=(long long)r->x+r->width,bottom=(long long)r->y+r->height;
        if(right>(long long)x+width)right=(long long)x+width;
        if(bottom>(long long)y+height)bottom=(long long)y+height;
        if(right<=left || bottom<=top)continue;
        covered+=(unsigned long long)(right-left)*(bottom-top)*weight;
    }
    unsigned long long tests=(unsigned long long)width*height*work,steps=covered*budget;
    return tests>steps?tests:steps;
}

/* General register-resident fragment variant. Only mutable temporaries 0..7
 * and the fragment output need physical registers; all other inputs retain
 * their immutable fragment-entry values. Unsupported/malformed code uses the
 * full GPU VM, never CPU rendering. Inspect the complete program, including
 * untaken branches, so a later write cannot invalidate an immutable input. */
#if defined(__CUDACC__)
__host__ __device__
#endif
static inline int kshr_register_fast_eligible(const kshr_job_t *job) {
    if(!job || !job->code_count || job->code_count>SH_MAX_INSTRUCTIONS)return 0;
    for(unsigned i=0;i<job->code_count;i++){
        const sh_instruction_t *in=&job->code[i];
        if(in->op>SH_DISCARD || in->dst>=SR_REGISTERS || (in->mask&~15u) ||
           in->src[0]>=SR_REGISTERS || in->src[1]>=SR_REGISTERS || in->src[2]>=SR_REGISTERS)return 0;
        switch(in->op){
        case SH_END:case SH_MOV:case SH_ADD:case SH_SUB:case SH_MUL:case SH_MAD:
        case SH_MIN:case SH_MAX:case SH_NEG:case SH_ABS:
        case SH_SLT:case SH_SLE:case SH_SGT:case SH_SGE:case SH_SEQ:case SH_SNE:
        case SH_AND:case SH_OR:case SH_NOT:case SH_TEX:
        case SH_JMP:case SH_JMPZ:case SH_JMPNZ:case SH_DISCARD:break;
        default:return 0;
        }
        if(in->op==SH_END || in->op>=SH_JMP || !in->mask)continue;
        if(in->dst>=8 && in->dst!=SR_FRAGCOLOR)return 0;
    }
    return 1;
}

typedef struct {
    unsigned long long scratch,status; /* owner-scoped surface handles */
    kshr_job_t job;
} kshr_submission_t;
#define KSHR_SCRATCH_BYTES(lanes) ((unsigned long long)(lanes)*SR_REGISTERS*16u)
#define KSHR_PACKET_OFFSET(lanes) ((KSHR_SCRATCH_BYTES(lanes)+255u)&~255ull)
#define KSHR_ALLOCATION_BYTES(lanes) (KSHR_PACKET_OFFSET(lanes)+sizeof(kshr_job_t))
#define KSHR_LANES(pixels) ((unsigned)((pixels)>KSH_MAX_LANES?KSH_MAX_LANES:(pixels)))
#define KSHR_FAST_MAX_LANES 16384u
#define KSHR_FAST_LANES(pixels) ((unsigned)((pixels)>KSHR_FAST_MAX_LANES?KSHR_FAST_MAX_LANES:(pixels)))
#if defined(__CUDACC__)
__host__ __device__
#endif
static inline unsigned kshr_job_lanes(const kshr_job_t *job) {
    unsigned long long pixels=(unsigned long long)job->clip_w*job->clip_h;
    return kshr_register_fast_eligible(job)?KSHR_FAST_LANES(pixels):KSHR_LANES(pixels);
}
#if defined(__CUDACC__)
__host__ __device__
#endif
static inline unsigned long long kshr_job_scratch_bytes(const kshr_job_t *job) {
    return kshr_register_fast_eligible(job)?0:KSHR_SCRATCH_BYTES(kshr_job_lanes(job));
}
#define KSHR_JOB_PACKET_OFFSET(job) ((kshr_job_scratch_bytes(job)+255u)&~255ull)
#define KSHR_JOB_ALLOCATION_BYTES(job) (KSHR_JOB_PACKET_OFFSET(job)+sizeof(kshr_job_t))
/* Only use with nonzero pixels and lane < KSHR_LANES(pixels). */
#define KSHR_LANE_PIXELS(pixels,lane) (1u+((pixels)-1u-(lane))/KSHR_LANES(pixels))

/* A job can cover more pixels than its at-most-KSH_MAX_LANES workers. Worker i
 * owns pixels i, i+lanes, ... and reuses its explicit scratch slice sequentially.
 * Status counts aggregate all assigned pixels; COMPLETE/executed==0 means none
 * invoked the shader. Jobs <= KSH_MAX_LANES retain their original wire semantics.
 * Any lane error invalidates the entire output batch, even if other lanes wrote.
 * No depth/colour write from the failing pixel is committed; earlier pixels may
 * already have written, so no consumer may accept any part of the failed job.
 * DISCARDED is consumed
 * internally and does not update colour/depth; final success is COMPLETE.
 * Work admission bounds both primitive iteration and conservative covered
 * fragment work via kshr_region_cost(); sparse geometry need not charge every
 * primitive's shader budget over the entire union rectangle.
 */
#if defined(__cplusplus)
static_assert(sizeof(kshr_command_t)==576,"programmable raster command ABI");
static_assert(sizeof(kshr_job_t)==56640,"programmable raster job ABI");
static_assert(sizeof(kshr_submission_t)==56656,"programmable raster submission ABI");
#else
_Static_assert(sizeof(kshr_command_t)==576,"programmable raster command ABI");
_Static_assert(sizeof(kshr_job_t)==56640,"programmable raster job ABI");
_Static_assert(sizeof(kshr_submission_t)==56656,"programmable raster submission ABI");
#endif
#endif
