/* Ordered GPU programmable raster: no CPU pixel/shader execution, implicit
 * local memory, atomics or heap. One lane owns a strided set of pixels across
 * the full batch, reusing its scratch only after each preceding pixel finishes.
 * A bounded tile reuses its explicit VM scratch only after retirement.
 */
#include "../include/kestrel/shader_raster.h"
#define KSH_DEVICE_ONLY
#ifndef KSHR_REGISTER_FAST
#include "shader_vm.cu"
#endif
#define GLGPU_HELPERS_ONLY
#include "gl_raster.cu"

static __device__ __forceinline__ void kshr_use_register(unsigned reg,
    unsigned long long &a,unsigned long long &b,unsigned long long &c,unsigned long long &d) {
    if(reg<64)a|=1ull<<reg;
    else if(reg<128)b|=1ull<<(reg-64);
    else if(reg<192)c|=1ull<<(reg-128);
    else if(reg<SR_REGISTERS)d|=1ull<<(reg-192);
}
static __device__ __forceinline__ void kshr_reset_registers(const kshr_job_t &job,
    float *scratch,unsigned lanes,unsigned id,unsigned base,unsigned long long used) {
    while(used){
        unsigned reg=base+(unsigned)(__ffsll((long long)used)-1);
        used&=used-1;
        #pragma unroll
        for(unsigned k=0;k<4;k++){
            float value=0;
            if((reg>=SR_UNIFORM&&reg<SR_UNIFORM+SR_UNIFORM_N)||
               (reg>=SR_CONST&&reg<SR_CONST+SR_CONST_N))value=job.seed[reg][k];
            scratch[(reg*4+k)*lanes+id]=value;
        }
    }
}

static __device__ __forceinline__ bool kshr_fragment(
    const kshr_job_t &job,const kshr_command_t &cmd,unsigned a,unsigned b,unsigned c,
    float l1,float l2,float iw,float z,unsigned x,unsigned y,
    float *scratch,ksh_status_t *status,unsigned lanes,unsigned id,
    const unsigned *texels,unsigned long long texture_bytes,
    unsigned &pixel,float &stored,bool &colour_dirty,bool &depth_dirty,unsigned &executed,
    unsigned long long used0,unsigned long long used1,unsigned long long used2,unsigned long long used3) {
    const kg3d_command_t &r=cmd.raster;
    if(!isfinite(z)||z<0||z>1||!(iw>0)||!isfinite(iw)||
       ((r.flags&KG3D_DEPTH_TEST)&&!glgpu_test(r.depth_func,z,stored)))return true;
#ifdef KSHR_REGISTER_FAST
    kshr_registers registers(job,cmd,a,b,c,l1,l2,iw,z,x,y);
#else
    // Reset every register the immutable bytecode can access, including partial
    // destinations and implicit matrix columns. Registers it cannot read/write
    // need no 3.2KiB-per-fragment blanket clear. Masks are derived on the GPU,
    // not supplied by a caller, and include both branches and loop bodies.
    kshr_reset_registers(job,scratch,lanes,id,0,used0);
    kshr_reset_registers(job,scratch,lanes,id,64,used1);
    kshr_reset_registers(job,scratch,lanes,id,128,used2);
    kshr_reset_registers(job,scratch,lanes,id,192,used3);
    float w=__fdiv_rn(1.0f,iw);
    #pragma unroll 1
    for(unsigned reg=0;reg<job.varying_count;reg++){
        #pragma unroll
        for(unsigned k=0;k<4;k++){
            float av=cmd.varying[a][reg][k];
            scratch[((SR_VARYING+reg)*4+k)*lanes+id]=
                (av+(cmd.varying[b][reg][k]-av)*l1+(cmd.varying[c][reg][k]-av)*l2)*w;
        }
    }
    scratch[(SR_FRAGCOORD*4+0)*lanes+id]=(float)x+0.5f;
    scratch[(SR_FRAGCOORD*4+1)*lanes+id]=job.origin_lower_left?
        (float)(job.height-y)-0.5f:(float)y+0.5f;
    scratch[(SR_FRAGCOORD*4+2)*lanes+id]=z;
    scratch[(SR_FRAGCOORD*4+3)*lanes+id]=iw;
    scratch[(SR_FRAGCOLOR*4+3)*lanes+id]=1;
    float *registers=scratch;
#endif
    ksh_execute(registers,job.code,status,texels,job.textures,
        (unsigned long long)lanes*SR_REGISTERS*4,sizeof(job.code),
        (unsigned long long)lanes*sizeof(*status),texture_bytes,sizeof(job.textures),
        lanes,job.code_count,job.budget,job.texture_count,id);
    executed+=status[id].executed;
    if(status[id].result==KSH_DISCARDED)return true;
    if(status[id].result!=KSH_COMPLETE)return false;
    float rgba[4];
    #pragma unroll
    for(unsigned k=0;k<4;k++)rgba[k]=ksh_read(registers,lanes,id,SR_FRAGCOLOR,SH_SWIZZLE_XYZW,k);
    glgpu_fragment(r,rgba,z,pixel,stored,colour_dirty,depth_dirty);
    return true;
}

extern "C" __global__ void shader_raster(
    unsigned *dst,float *depth,const unsigned *texels,float *scratch,
    ksh_status_t *status,const kshr_job_t *packet,
    unsigned long long dst_bytes,unsigned long long depth_bytes,
    unsigned long long texture_bytes,unsigned long long scratch_bytes,
    unsigned long long status_bytes,unsigned long long packet_bytes) {
    unsigned long long lane=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;
    if(!packet||packet_bytes<sizeof(*packet))return;
    const kshr_job_t &job=*packet;
    unsigned long long total=(unsigned long long)job.clip_w*job.clip_h;
#ifdef KSHR_REGISTER_FAST
    unsigned lanes=KSHR_FAST_LANES(total);
#else
    unsigned lanes=KSHR_LANES(total);
#endif
    if(lane>=lanes||!status||status_bytes<(unsigned long long)lanes*sizeof(*status))return;
    unsigned id=(unsigned)lane,executed=0;
    status[id].result=KSH_BAD_RESOURCE;status[id].executed=0;
    if(!dst||!scratch||!job.width||!job.height||job.width>32768||job.height>32768||
       job.pitch<job.width||job.clip_x>=job.width||job.clip_y>=job.height||
       job.clip_w>job.width-job.clip_x||job.clip_h>job.height-job.clip_y||
       ((unsigned long long)(job.height-1)*job.pitch+job.width)>dst_bytes/4||
       (depth&&(job.depth_pitch<job.width||
         ((unsigned long long)(job.height-1)*job.depth_pitch+job.width)>depth_bytes/4))||
       job.texture_count>KSH_MAX_TEXTURES)return;
#ifndef KSHR_REGISTER_FAST
    if(scratch_bytes<(unsigned long long)lanes*SR_REGISTERS*4*sizeof(float))return;
#else
    if(scratch_bytes)return; // fast jobs have no writable global register file
#endif
    status[id].result=KSH_BAD_PROGRAM;
    if(total>KSH_MAX_WORK||!job.command_count||job.command_count>KG3D_MAX_COMMANDS||
       !job.code_count||job.code_count>SH_MAX_INSTRUCTIONS||
       !job.budget||job.budget>KSH_MAX_STEPS||job.varying_count>SR_VARYING_N||
       job.origin_lower_left>1||job.reserved[0]||job.reserved[1])return;
    if(kshr_region_cost(job.commands,job.command_count,job.budget,
                       job.clip_x,job.clip_y,job.clip_w,job.clip_h)>KSH_MAX_WORK)return;
    // One scan per worker/job, amortized over all assigned pixels/primitives.
    // Invalid *unreached* instructions retain the VM's existing semantics: do
    // not reject them here or shift an out-of-range register into a mask.
    unsigned long long used0=0,used1=0,used2=0,used3=1ull<<(SR_FRAGCOLOR-192);
#ifdef KSHR_REGISTER_FAST
    // Recheck on-device: a raw packet must never turn an unsupported mutable
    // register into a silently ignored write.
    if(!kshr_register_fast_eligible(&job))return;
#else
    #pragma unroll 1
    for(unsigned pc=0;pc<job.code_count;pc++){
        const sh_instruction_t &in=job.code[pc];
        #pragma unroll 1
        for(unsigned operand=0;operand<4;operand++)
            kshr_use_register(operand?in.src[operand-1]:in.dst,used0,used1,used2,used3);
        if(in.op==SH_MATMUL){
            #pragma unroll 1
            for(unsigned column=1;column<4;column++)
                kshr_use_register((unsigned)in.src[0]+column,used0,used1,used2,used3);
        }
    }
#endif
    for(unsigned long long pixel_index=id;pixel_index<total;pixel_index+=lanes){
    unsigned x=job.clip_x+(unsigned)(pixel_index%job.clip_w),y=job.clip_y+(unsigned)(pixel_index/job.clip_w);
    unsigned long long index=(unsigned long long)y*job.pitch+x,zi=(unsigned long long)y*job.depth_pitch+x;
    unsigned pixel=dst[index];float stored=depth?depth[zi]:1;
    bool colour_dirty=false,depth_dirty=false;
    for(unsigned i=0;i<job.command_count;i++){
        const kshr_command_t &cmd=job.commands[i];const kg3d_command_t &r=cmd.raster;
        unsigned primitive=r.flags&(KG3D_LINE|KG3D_POINT);
        if((r.flags&~(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND|KG3D_LINE|KG3D_POINT))||
           primitive==(KG3D_LINE|KG3D_POINT)||r.depth_func>KG3D_ALWAYS||r.alpha_func>KG3D_ALWAYS||
           r.blend_src>KG3D_ONE_MINUS_DST_ALPHA||r.blend_dst>KG3D_ONE_MINUS_DST_ALPHA||
           !isfinite(r.depth_bias)||!isfinite(r.alpha_ref)||r.reserved[0]||r.reserved[1]||r.reserved[2])goto failed;
        if((r.flags&(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE))&&!depth){status[id].result=KSH_BAD_RESOURCE;goto failed;}
        long long rx=(long long)x-r.x,ry=(long long)y-r.y;
        if(r.width<=0||r.height<=0||rx<0||ry<0||rx>=r.width||ry>=r.height)continue;
        for(unsigned v=0;v<3;v++)
            if(!(fabsf(r.v[v].x)<=1048576.0f)||!(fabsf(r.v[v].y)<=1048576.0f)||
               !isfinite(r.v[v].z)||!(r.v[v].inv_w>0)||!isfinite(r.v[v].inv_w))goto failed;
        unsigned b=1,c=2;
        int steps=1,first=0,last=0;
        float l1=0,l2=0,dx=0,dy=0;
        if(primitive){
            b=primitive==KG3D_POINT?0:1;c=0;
            const kg3d_vertex_t &a=r.v[0],&v=r.v[b];
            dx=v.x-a.x;dy=v.y-a.y;
            float extent=fmaxf(fabsf(dx),fabsf(dy));
            if(primitive==KG3D_LINE&&extent>=8193)goto failed;
            steps=(int)extent;if(steps<1)steps=1;
            last=primitive==KG3D_POINT?0:steps;
            if(primitive==KG3D_LINE&&extent>=1){
                bool horizontal=fabsf(dx)>=fabsf(dy);
                float delta=horizontal?dx:dy,origin=horizontal?a.x:a.y;
                int at=(int)floorf(((float)(horizontal?x:y)+0.5f-origin)*steps/delta);
                first=at-2;if(first<0)first=0;last=at+3;if(last>steps)last=steps;
            }
        }else{
            long long ax=__float2ll_rn(r.v[0].x*16),ay=__float2ll_rn(r.v[0].y*16);
            long long bx=__float2ll_rn(r.v[1].x*16),by=__float2ll_rn(r.v[1].y*16);
            long long cx=__float2ll_rn(r.v[2].x*16),cy=__float2ll_rn(r.v[2].y*16);
            long long area=glgpu_edge(ax,ay,bx,by,cx,cy);
            if(!area)continue;
            if(area<0){b=2;c=1;long long t=bx;bx=cx;cx=t;t=by;by=cy;cy=t;area=-area;}
            long long px=(long long)x*16+8,py=(long long)y*16+8;
            long long e0=glgpu_edge(bx,by,cx,cy,px,py),e1=glgpu_edge(cx,cy,ax,ay,px,py),e2=glgpu_edge(ax,ay,bx,by,px,py);
            if(e0<0||(!e0&&!glgpu_top_left(bx,by,cx,cy))||e1<0||(!e1&&!glgpu_top_left(cx,cy,ax,ay))||
               e2<0||(!e2&&!glgpu_top_left(ax,ay,bx,by)))continue;
            float inv_area=1.0f/(float)area;l1=(float)e1*inv_area;l2=(float)e2*inv_area;
        }
        // One executor call site for triangles/lines/points: inlining the VM
        // separately for each primitive doubles SASS and instruction-cache use.
        for(int step=first;step<=last;step++){
            float z,iw;
            if(primitive){
                l1=primitive==KG3D_POINT?0:__fdiv_rn((float)step,(float)steps);
                float sx=__fadd_rn(r.v[0].x,__fmul_rn(dx,l1)),sy=__fadd_rn(r.v[0].y,__fmul_rn(dy,l1));
                if((int)sx!=(int)x||(int)sy!=(int)y)continue;
                iw=__fadd_rn(r.v[0].inv_w,__fmul_rn(r.v[b].inv_w-r.v[0].inv_w,l1));
                z=__fadd_rn(r.v[0].z,__fmul_rn(r.v[b].z-r.v[0].z,l1));
            }else{
                z=r.v[0].z+(r.v[b].z-r.v[0].z)*l1+(r.v[c].z-r.v[0].z)*l2+r.depth_bias;
                iw=r.v[0].inv_w+(r.v[b].inv_w-r.v[0].inv_w)*l1+(r.v[c].inv_w-r.v[0].inv_w)*l2;
            }
            if(!kshr_fragment(job,cmd,0,b,c,l1,l2,iw,z,x,y,scratch,status,lanes,id,
                texels,texture_bytes,pixel,stored,colour_dirty,depth_dirty,executed,
                used0,used1,used2,used3))goto failed;
        }
    }
    if(colour_dirty)dst[index]=pixel;
    if(depth_dirty)depth[zi]=stored;
    }
    status[id].result=KSH_COMPLETE;status[id].executed=executed;return;
failed:
    // A VM failure leaves its specific status; malformed later primitives must
    // not inherit COMPLETE/DISCARDED from an earlier invocation.
    if(status[id].result==KSH_COMPLETE||status[id].result==KSH_DISCARDED)status[id].result=KSH_BAD_PROGRAM;
    status[id].executed=executed;
}
