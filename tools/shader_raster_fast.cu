/* General vector/register-resident fragment executor. Eligibility depends on
 * bytecode capabilities, never a scene name, program ID or shader-source match.
 * Other programs retain the full GPU executor. No CPU pixel/shader fallback. */
#include "../include/kestrel/shader_raster.h"
#define KSH_DEVICE_ONLY
#include "shader_vm.cu"

struct kshr_registers {
    float4 t0,t1,t2,t3,t4,t5,t6,t7,colour;
    const kshr_job_t &job;
    const kshr_command_t &cmd;
    unsigned a,b,c,x,y;
    float l1,l2,w,iw,z;
    __device__ __forceinline__ kshr_registers(const kshr_job_t &j,const kshr_command_t &r,
        unsigned aa,unsigned bb,unsigned cc,float u,float v,float invw,float zz,unsigned xx,unsigned yy)
        :t0{},t1{},t2{},t3{},t4{},t5{},t6{},t7{},colour{0,0,0,1},job(j),cmd(r),
         a(aa),b(bb),c(cc),x(xx),y(yy),l1(u),l2(v),iw(invw),z(zz) {w=__fdiv_rn(1.0f,invw);}
    __device__ __forceinline__ float component(float4 v,unsigned k) const {
        return k==0?v.x:k==1?v.y:k==2?v.z:v.w;
    }
    __device__ __forceinline__ float4 read(unsigned reg) const {
        if(reg<8){
            if(reg==0)return t0;if(reg==1)return t1;if(reg==2)return t2;if(reg==3)return t3;
            if(reg==4)return t4;if(reg==5)return t5;if(reg==6)return t6;return t7;
        }
        if(reg==SR_FRAGCOLOR)return colour;
        if((reg>=SR_UNIFORM&&reg<SR_UNIFORM+SR_UNIFORM_N)||
           (reg>=SR_CONST&&reg<SR_CONST+SR_CONST_N)){
            const float *p=job.seed[reg];return make_float4(p[0],p[1],p[2],p[3]);
        }
        if(reg>=SR_VARYING&&reg<SR_VARYING+job.varying_count){
            unsigned v=reg-SR_VARYING;
            const float *aa=cmd.varying[a][v],*bb=cmd.varying[b][v],*cc=cmd.varying[c][v];
            return make_float4((aa[0]+(bb[0]-aa[0])*l1+(cc[0]-aa[0])*l2)*w,
                (aa[1]+(bb[1]-aa[1])*l1+(cc[1]-aa[1])*l2)*w,
                (aa[2]+(bb[2]-aa[2])*l1+(cc[2]-aa[2])*l2)*w,
                (aa[3]+(bb[3]-aa[3])*l1+(cc[3]-aa[3])*l2)*w);
        }
        if(reg==SR_FRAGCOORD)return make_float4((float)x+0.5f,
            job.origin_lower_left?(float)(job.height-y)-0.5f:(float)y+0.5f,z,iw);
        return make_float4(0,0,0,0);
    }
    __device__ __forceinline__ float4 swizzled(unsigned reg,unsigned swizzle) const {
        float4 v=read(reg);
        return make_float4(component(v,swizzle&3),component(v,(swizzle>>2)&3),
                          component(v,(swizzle>>4)&3),component(v,(swizzle>>6)&3));
    }
    __device__ __forceinline__ void write(unsigned reg,unsigned mask,float4 v) {
        float4 old=read(reg);
        if(!(mask&1))v.x=old.x;if(!(mask&2))v.y=old.y;
        if(!(mask&4))v.z=old.z;if(!(mask&8))v.w=old.w;
        if(reg==0)t0=v;else if(reg==1)t1=v;else if(reg==2)t2=v;else if(reg==3)t3=v;
        else if(reg==4)t4=v;else if(reg==5)t5=v;else if(reg==6)t6=v;else if(reg==7)t7=v;
        else if(reg==SR_FRAGCOLOR)colour=v;
    }
};
static __device__ __forceinline__ float ksh_read(const kshr_registers &r,
    unsigned,unsigned,unsigned reg,unsigned swizzle,unsigned k) {
    return r.component(r.read(reg),(swizzle>>(k*2))&3);
}

static __device__ __forceinline__ void ksh_execute(kshr_registers &r,
    const sh_instruction_t *code,ksh_status_t *status,const unsigned *texels,const ksh_texture_t *textures,
    unsigned long long,unsigned long long,unsigned long long,unsigned long long texture_bytes,
    unsigned long long,unsigned,unsigned count,unsigned budget,unsigned ntextures,unsigned long long lane) {
    unsigned pc=0,executed=0,result=KSH_STEP_LIMIT;
    while(executed<budget){
        if(pc==count){result=KSH_COMPLETE;break;}
        if(pc>count){result=KSH_BAD_PROGRAM;break;}
        const sh_instruction_t &in=code[pc];executed++;
        if(in.op==SH_END){result=KSH_COMPLETE;break;}
        if(in.op==SH_DISCARD){result=KSH_DISCARDED;break;}
        float4 a=r.swizzled(in.src[0],in.swizzle[0]);
        if(in.op>=SH_JMP){
            if(in.target<0||(unsigned)in.target>count){result=KSH_BAD_PROGRAM;break;}
            bool take=in.op==SH_JMP||(in.op==SH_JMPZ?a.x==0:a.x!=0);
            pc=take?(unsigned)in.target:pc+1;continue;
        }
        float4 b=make_float4(0,0,0,0),c=b,out=b;
        if(in.op!=SH_MOV&&in.op!=SH_NEG&&in.op!=SH_ABS&&in.op!=SH_NOT)
            b=r.swizzled(in.src[1],in.swizzle[1]);
        if(in.op==SH_MAD)c=r.swizzled(in.src[2],in.swizzle[2]);
#define KSHR_VECTOR(expr) out=make_float4(expr(x),expr(y),expr(z),expr(w))
#define KSHR_ADD(k) (a.k+b.k)
#define KSHR_SUB(k) (a.k-b.k)
#define KSHR_MUL(k) (a.k*b.k)
#define KSHR_MAD(k) (a.k*b.k+c.k)
#define KSHR_MIN(k) (a.k<b.k?a.k:b.k)
#define KSHR_MAX(k) (a.k>b.k?a.k:b.k)
#define KSHR_NEG(k) (-a.k)
#define KSHR_ABS(k) fabsf(a.k)
#define KSHR_SLT(k) (a.k<b.k)
#define KSHR_SLE(k) (a.k<=b.k)
#define KSHR_SGT(k) (a.k>b.k)
#define KSHR_SGE(k) (a.k>=b.k)
#define KSHR_SEQ(k) (a.k==b.k)
#define KSHR_SNE(k) (a.k!=b.k)
#define KSHR_AND(k) (a.k!=0&&b.k!=0)
#define KSHR_OR(k) (a.k!=0||b.k!=0)
#define KSHR_NOT(k) (a.k==0)
        switch(in.op){
        case SH_MOV:out=a;break;
        case SH_ADD:KSHR_VECTOR(KSHR_ADD);break;case SH_SUB:KSHR_VECTOR(KSHR_SUB);break;
        case SH_MUL:KSHR_VECTOR(KSHR_MUL);break;case SH_MAD:KSHR_VECTOR(KSHR_MAD);break;
        case SH_MIN:KSHR_VECTOR(KSHR_MIN);break;case SH_MAX:KSHR_VECTOR(KSHR_MAX);break;
        case SH_NEG:KSHR_VECTOR(KSHR_NEG);break;case SH_ABS:KSHR_VECTOR(KSHR_ABS);break;
        case SH_SLT:KSHR_VECTOR(KSHR_SLT);break;case SH_SLE:KSHR_VECTOR(KSHR_SLE);break;
        case SH_SGT:KSHR_VECTOR(KSHR_SGT);break;case SH_SGE:KSHR_VECTOR(KSHR_SGE);break;
        case SH_SEQ:KSHR_VECTOR(KSHR_SEQ);break;case SH_SNE:KSHR_VECTOR(KSHR_SNE);break;
        case SH_AND:KSHR_VECTOR(KSHR_AND);break;case SH_OR:KSHR_VECTOR(KSHR_OR);break;
        case SH_NOT:KSHR_VECTOR(KSHR_NOT);break;
        case SH_TEX:{
            float sample[4];float unit=r.read(in.src[1]).x;
            if(!ksh_sample(texels,texture_bytes,textures,ntextures,unit,a.x,a.y,sample)){
                result=KSH_BAD_RESOURCE;goto finish;
            }
            out=make_float4(sample[0],sample[1],sample[2],sample[3]);break;
        }
        default:result=KSH_BAD_PROGRAM;goto finish;
        }
        r.write(in.dst,in.mask,out);pc++;
    }
    if(pc==count&&result==KSH_STEP_LIMIT)result=KSH_COMPLETE;
finish:
    status[lane].result=result;status[lane].executed=executed;
}

#define KSHR_REGISTER_FAST
#define shader_raster shader_raster_fast
#include "shader_raster.cu"
