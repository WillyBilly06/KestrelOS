/* GPU execution of Kestrel GLSL bytecode. Native VM/raster dispatch is exposed;
 * application programmable draws still need userland routing to this executor.
 * Every invocation owns its explicit SoA register slice and status slot.
 * There is no CUDA local/shared allocation, recursion, callback or device heap.
 * A tile of 1024 invocations needs 3.125 MiB register scratch, reusable only
 * after its fence retires. Vertex and fragment inputs use the same register ABI.
 */
#include "../include/kestrel/shader_vm.h"
#include <math.h>

static __device__ __forceinline__ float ksh_rcp(float x) {
    if(x>-1.0e-20f && x<1.0e-20f)return x<0?-1.0e30f:1.0e30f;
    return __fdiv_rn(1.0f,x);
}
static __device__ __forceinline__ float ksh_read(
    const float *r,unsigned lanes,unsigned id,unsigned reg,unsigned swizzle,unsigned c) {
    return r[((unsigned long long)reg*4+((swizzle>>(c*2))&3))*lanes+id];
}
static __device__ __forceinline__ void ksh_write(
    float *r,unsigned lanes,unsigned id,unsigned reg,unsigned c,float value) {
    r[((unsigned long long)reg*4+c)*lanes+id]=value;
}
#ifndef KSH_REGISTER_TYPE
#define KSH_REGISTER_TYPE float *
#endif
static __device__ __forceinline__ int ksh_wrap(int x,unsigned n,unsigned mode) {
    if(!mode){x%=(int)n;return x<0?x+(int)n:x;}
    return x<0?0:(x>=(int)n?(int)n-1:x);
}
static __device__ __forceinline__ unsigned ksh_texel(
    const unsigned *data,const ksh_texture_t &t,int x,int y) {
    return data[t.offset/4+(unsigned long long)ksh_wrap(y,t.height,t.wrap_t)*t.pitch+
                ksh_wrap(x,t.width,t.wrap_s)];
}
static __device__ __forceinline__ bool ksh_sample(
    const unsigned *data,unsigned long long bytes,const ksh_texture_t *textures,
    unsigned ntextures,float unit,float s,float t,float *out) {
    out[0]=out[1]=out[2]=0;out[3]=1;
    // Match the CPU's truncation toward zero before checking the integer unit.
    if(!isfinite(unit)||unit<=-1||unit>=KSH_MAX_TEXTURES)return true;
    unsigned u=(unsigned)(int)unit;
    out[0]=out[1]=out[2]=1;
    if(u>=ntextures)return true;
    const ksh_texture_t &tex=textures[u];
    if(!tex.width&&!tex.height)return true;
    if(!data || !tex.width || !tex.height || tex.width>32768 || tex.height>32768 ||
       tex.pitch<tex.width || tex.wrap_s>1 || tex.wrap_t>1 || tex.filter>1 ||
       (tex.offset&3) || tex.offset>bytes ||
       ((unsigned long long)(tex.height-1)*tex.pitch+tex.width)>(bytes-tex.offset)/4 ||
       !isfinite(s)||!isfinite(t))return false;
    // Reduce before float->integer conversion, including hostile huge coordinates.
    s=tex.wrap_s?fminf(1,fmaxf(0,s)):s-floorf(s);
    t=tex.wrap_t?fminf(1,fmaxf(0,t)):t-floorf(t);
    float x=s*tex.width-0.5f,y=(1-t)*tex.height-0.5f;
    int ix=(int)floorf(x),iy=(int)floorf(y);
    float fx=x-ix,fy=y-iy;
    unsigned a,b,c,d;
    if(!tex.filter){
        a=ksh_texel(data,tex,(int)floorf(x+0.5f),(int)floorf(y+0.5f));
        b=c=d=a;fx=fy=0;
    } else {
        a=ksh_texel(data,tex,ix,iy);b=ksh_texel(data,tex,ix+1,iy);
        c=ksh_texel(data,tex,ix,iy+1);d=ksh_texel(data,tex,ix+1,iy+1);
    }
    #pragma unroll
    for(unsigned k=0;k<4;k++){
        unsigned shift=k==3?24:16-8*k;
        float p=((a>>shift)&255)*(1.0f/255.0f),q=((b>>shift)&255)*(1.0f/255.0f);
        float r=((c>>shift)&255)*(1.0f/255.0f),v=((d>>shift)&255)*(1.0f/255.0f);
        float top=p+(q-p)*fx,bot=r+(v-r)*fx;out[k]=top+(bot-top)*fy;
    }
    return true;
}

static __device__ __forceinline__ void ksh_execute(
    KSH_REGISTER_TYPE registers,const sh_instruction_t *code,ksh_status_t *status,
    const unsigned *texels,const ksh_texture_t *textures,
    unsigned long long register_floats,unsigned long long code_bytes,
    unsigned long long status_bytes,unsigned long long texture_bytes,
    unsigned long long descriptor_bytes,unsigned lanes,unsigned count,
    unsigned budget,unsigned ntextures,unsigned long long lane) {
    if(lane>=lanes || !status || status_bytes<(unsigned long long)lanes*sizeof(*status))return;
    unsigned id=(unsigned)lane,executed=0,result=KSH_BAD_RESOURCE;
    if(!lanes || lanes>KSH_MAX_LANES || !registers ||
       register_floats<(unsigned long long)lanes*SR_REGISTERS*4 ||
       ntextures>KSH_MAX_TEXTURES || (ntextures && !textures) ||
       descriptor_bytes<(unsigned long long)ntextures*sizeof(*textures))goto finish;
    result=KSH_BAD_PROGRAM;
    if(!code || !count || count>SH_MAX_INSTRUCTIONS ||
       code_bytes<(unsigned long long)count*sizeof(*code) ||
       !budget || budget>KSH_MAX_STEPS || (unsigned long long)lanes*budget>KSH_MAX_WORK)goto finish;
    {
        int pc=0;
        result=KSH_STEP_LIMIT;
        while(executed<budget) {
            // Falling off a valid instruction sequence is termination in the CPU VM.
            if(pc==(int)count){result=KSH_COMPLETE;break;}
            if(pc<0 || pc>(int)count){result=KSH_BAD_PROGRAM;break;}
            const sh_instruction_t &in=code[pc];
            if(in.op>SH_DISCARD || in.dst>=SR_REGISTERS || (in.mask&~15u) ||
               in.src[0]>=SR_REGISTERS || in.src[1]>=SR_REGISTERS || in.src[2]>=SR_REGISTERS) {
                result=KSH_BAD_PROGRAM;break;
            }
            executed++;
            if(in.op==SH_END){result=KSH_COMPLETE;break;}
            if(in.op==SH_DISCARD){result=KSH_DISCARDED;break;}
            if(in.op==SH_JMP || in.op==SH_JMPZ || in.op==SH_JMPNZ) {
                if(in.target<0 || in.target>(int)count){result=KSH_BAD_PROGRAM;break;}
                float v=ksh_read(registers,lanes,id,in.src[0],in.swizzle[0],0);
                bool jump=in.op==SH_JMP || (in.op==SH_JMPZ?v==0:v!=0);
                pc=jump?in.target:pc+1;continue;
            }
            float a[4],b[4],c[4],out[4];
            #pragma unroll
            for(unsigned k=0;k<4;k++) {
                a[k]=ksh_read(registers,lanes,id,in.src[0],in.swizzle[0],k);
                b[k]=ksh_read(registers,lanes,id,in.src[1],in.swizzle[1],k);
                c[k]=ksh_read(registers,lanes,id,in.src[2],in.swizzle[2],k);
                out[k]=0;
            }
            if(in.op==SH_TEX) {
                float unit=ksh_read(registers,lanes,id,in.src[1],SH_SWIZZLE_XYZW,0);
                if(!ksh_sample(texels,texture_bytes,textures,ntextures,unit,a[0],a[1],out)){
                    result=KSH_BAD_RESOURCE;break;
                }
            } else if(in.op==SH_MATMUL) {
                if(in.src[0]>SR_REGISTERS-4){result=KSH_BAD_PROGRAM;break;}
                #pragma unroll
                for(unsigned k=0;k<4;k++) {
                    float p=ksh_read(registers,lanes,id,in.src[0],SH_SWIZZLE_XYZW,k);
                    float q=ksh_read(registers,lanes,id,in.src[0]+1,SH_SWIZZLE_XYZW,k);
                    float r=ksh_read(registers,lanes,id,in.src[0]+2,SH_SWIZZLE_XYZW,k);
                    float s=ksh_read(registers,lanes,id,in.src[0]+3,SH_SWIZZLE_XYZW,k);
                    out[k]=p*b[0]+q*b[1]+r*b[2]+s*b[3];
                }
            } else if(in.op==SH_CROSS) {
                out[0]=a[1]*b[2]-a[2]*b[1];out[1]=a[2]*b[0]-a[0]*b[2];
                out[2]=a[0]*b[1]-a[1]*b[0];out[3]=0;
            } else if(in.op>=SH_DP2 && in.op<=SH_DP4) {
                float d=a[0]*b[0]+a[1]*b[1];
                if(in.op>=SH_DP3)d+=a[2]*b[2];if(in.op==SH_DP4)d+=a[3]*b[3];
                #pragma unroll
                for(unsigned k=0;k<4;k++)out[k]=d;
            } else {
                #pragma unroll
                for(unsigned k=0;k<4;k++) {
                    switch(in.op) {
                    case SH_MOV:out[k]=a[k];break;
                    case SH_ADD:out[k]=a[k]+b[k];break;
                    case SH_SUB:out[k]=a[k]-b[k];break;
                    case SH_MUL:out[k]=a[k]*b[k];break;
                    case SH_DIV:out[k]=a[k]*ksh_rcp(b[k]);break;
                    case SH_MAD:out[k]=a[k]*b[k]+c[k];break;
                    case SH_MIN:out[k]=a[k]<b[k]?a[k]:b[k];break;
                    case SH_MAX:out[k]=a[k]>b[k]?a[k]:b[k];break;
                    case SH_RCP:out[k]=ksh_rcp(a[k]);break;
                    case SH_RSQ:out[k]=a[k]>0?__fdiv_rn(1,sqrtf(a[k])):1e30f;break;
                    case SH_SQRT:out[k]=a[k]>0?sqrtf(a[k]):0;break;
                    case SH_ABS:out[k]=fabsf(a[k]);break;
                    case SH_NEG:out[k]=-a[k];break;
                    case SH_FLOOR:out[k]=floorf(a[k]);break;
                    case SH_FRACT:out[k]=a[k]-floorf(a[k]);break;
                    case SH_SIGN:out[k]=a[k]>0?1:(a[k]<0?-1:0);break;
                    case SH_SIN:out[k]=__sinf(a[k]);break;
                    case SH_COS:out[k]=__cosf(a[k]);break;
                    case SH_POW:out[k]=a[k]>0?__powf(a[k],b[k]):0;break;
                    case SH_EXP2:out[k]=exp2f(a[k]);break;
                    case SH_LOG2:out[k]=a[k]>0?__log2f(a[k]):-1e30f;break;
                    case SH_SLT:out[k]=a[k]<b[k];break;
                    case SH_SLE:out[k]=a[k]<=b[k];break;
                    case SH_SGT:out[k]=a[k]>b[k];break;
                    case SH_SGE:out[k]=a[k]>=b[k];break;
                    case SH_SEQ:out[k]=a[k]==b[k];break;
                    case SH_SNE:out[k]=a[k]!=b[k];break;
                    case SH_AND:out[k]=a[k]!=0&&b[k]!=0;break;
                    case SH_OR:out[k]=a[k]!=0||b[k]!=0;break;
                    case SH_NOT:out[k]=a[k]==0;break;
                    }
                }
            }
            #pragma unroll
            for(unsigned k=0;k<4;k++)if(in.mask&(1u<<k))
                ksh_write(registers,lanes,id,in.dst,k,out[k]);
            pc++;
        }
        if(pc==(int)count && result==KSH_STEP_LIMIT)result=KSH_COMPLETE;
    }
finish:
    status[id].executed=executed;status[id].result=result;
}

#ifndef KSH_DEVICE_ONLY
extern "C" __global__ void shader_vm(
    float *registers,const sh_instruction_t *code,ksh_status_t *status,
    const unsigned *texels,const ksh_texture_t *textures,
    unsigned long long register_floats,unsigned long long code_bytes,
    unsigned long long status_bytes,unsigned long long texture_bytes,
    unsigned long long descriptor_bytes,unsigned lanes,unsigned count,
    unsigned budget,unsigned ntextures) {
    ksh_execute(registers,code,status,texels,textures,register_floats,code_bytes,
        status_bytes,texture_bytes,descriptor_bytes,lanes,count,budget,ntextures,
        (unsigned long long)blockIdx.x*blockDim.x+threadIdx.x);
}
#endif
