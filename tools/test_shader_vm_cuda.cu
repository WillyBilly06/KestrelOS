#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <cstring>
#include "cpu_reference.h"
#include "shader_vm.cu"

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
#define CUDA(x) do {cudaError_t e=(x);if(e!=cudaSuccess){std::fprintf(stderr,"CUDA %d: %s\n",__LINE__,cudaGetErrorString(e));std::abort();}}while(0)
static constexpr unsigned N=67,G=64,R=SR_REGISTERS*4*N;
static unsigned cases,comparisons;
static ksh_texture_t host_tex[4];
static std::vector<unsigned> pixels(64,0x12345678);
static unsigned wrap_ref(int x,unsigned n,unsigned mode){
    if(mode)return x<0?0:(x>=(int)n?n-1:x);
    x%=(int)n;return x<0?x+n:x;
}
static void fetch_ref(const ksh_texture_t &t,int x,int y,float *out){
    unsigned v=pixels[t.offset/4+wrap_ref(y,t.height,t.wrap_t)*t.pitch+wrap_ref(x,t.width,t.wrap_s)];
    for(unsigned k=0;k<4;k++)out[k]=((v>>(k==3?24:16-k*8))&255)*(1.0f/255.0f);
}
static void sample_ref(int unit,float s,float t,float *out){
    out[0]=out[1]=out[2]=0;out[3]=1;
    if(unit<0||unit>=4)return;
    out[0]=out[1]=out[2]=1;
    const auto &tex=host_tex[unit];if(!tex.width)return;
    float u=s*tex.width-0.5f,v=(1-t)*tex.height-0.5f;
    if(!tex.filter){fetch_ref(tex,(int)floorf(u+0.5f),(int)floorf(v+0.5f),out);return;}
    int x=(int)floorf(u),y=(int)floorf(v);float fx=u-x,fy=v-y;
    float a[4],b[4],c[4],d[4];fetch_ref(tex,x,y,a);fetch_ref(tex,x+1,y,b);
    fetch_ref(tex,x,y+1,c);fetch_ref(tex,x+1,y+1,d);
    for(unsigned k=0;k<4;k++){float top=a[k]+(b[k]-a[k])*fx,bot=c[k]+(d[k]-c[k])*fx;out[k]=top+(bot-top)*fy;}
}
static sh_instruction_t ins(unsigned op,unsigned dst=10,unsigned a=1,unsigned b=2,unsigned c=3){
    sh_instruction_t s={};s.op=op;s.mask=15;s.dst=dst;s.src[0]=a;s.src[1]=b;s.src[2]=c;
    s.swizzle[0]=s.swizzle[1]=s.swizzle[2]=SH_SWIZZLE_XYZW;return s;
}
static float initial(unsigned reg,unsigned component,unsigned lane){
    // Include positive, negative, zero and fractional values, differing in every lane.
    return ((int)((reg*7+component*13+lane*3)%31)-15)*0.125f;
}
static void run(std::vector<sh_instruction_t> program,unsigned expected=KSH_COMPLETE,
                unsigned budget=128,int mutation=0){
    static float *device=nullptr;static sh_instruction_t *dc=nullptr;
    static ksh_status_t *ds=nullptr;static unsigned *dp=nullptr;static ksh_texture_t *dt=nullptr;
    if(!device){CUDA(cudaMalloc(&device,(R+2*G)*sizeof(float)));
        CUDA(cudaMalloc(&dc,SH_MAX_INSTRUCTIONS*sizeof(*dc)));
        CUDA(cudaMalloc(&ds,(N+2*G)*sizeof(*ds)));
        CUDA(cudaMalloc(&dp,pixels.size()*4));CUDA(cudaMalloc(&dt,sizeof host_tex));}
    std::vector<float> input(R+2*G,1234567),got;
    for(unsigned r=0;r<SR_REGISTERS;r++)for(unsigned k=0;k<4;k++)for(unsigned id=0;id<N;id++)
        input[G+(r*4+k)*N+id]=initial(r,k,id);
    for(unsigned id=0;id<N;id++){
        input[G+(2*4)*N+id]=(float)((int)(id%6)-1)+(mutation==6?0.5f:0.0f);
        input[G+(4*4)*N+id]=(float)(id%2); // divergent branch condition
    }
    std::vector<ksh_status_t> status(N+2*G,{0xabcddcba,0x12344321});
    CUDA(cudaMemcpy(device,input.data(),input.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(ds,status.data(),status.size()*sizeof(*ds),cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(dc,program.data(),program.size()*sizeof(*dc),cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(dp,pixels.data(),pixels.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(dt,host_tex,sizeof host_tex,cudaMemcpyHostToDevice));
    shader_vm<<<3,32>>>(device+G,dc,ds+G,dp,dt,
        mutation==1?R-1:R,mutation==2?0:program.size()*sizeof(*dc),
        mutation==3?0:N*sizeof(*ds),mutation==4?0:pixels.size()*4,
        mutation==5?0:sizeof host_tex,N,(unsigned)program.size(),budget,4);
    CUDA(cudaGetLastError());CUDA(cudaDeviceSynchronize());
    got.resize(input.size());CUDA(cudaMemcpy(got.data(),device,got.size()*4,cudaMemcpyDeviceToHost));
    CUDA(cudaMemcpy(status.data(),ds,status.size()*sizeof(*ds),cudaMemcpyDeviceToHost));
    for(unsigned i=0;i<G;i++){
        CHECK(got[i]==1234567 && got[G+R+i]==1234567);
        CHECK(status[i].result==0xabcddcba && status[G+N+i].result==0xabcddcba);
        CHECK(status[i].executed==0x12344321 && status[G+N+i].executed==0x12344321);
    }
    if(mutation==1||mutation==2||mutation==3||mutation==5)
        CHECK(!memcmp(input.data(),got.data(),input.size()*sizeof(float)));
    static sh_shader_t shader;shader={};shader.compiled=true;shader.count=(int)program.size();
    memcpy(shader.code,program.data(),program.size()*sizeof(*dc));
    for(unsigned id=0;id<N;id++){
        unsigned want=expected;
        // Two out-of-range units do not access malformed texture resources.
        if(mutation==4 && ((int)(id%6)-1<0 || (int)(id%6)-1>=4))want=KSH_COMPLETE;
        CHECK(status[G+id].result==want);
        if(expected!=KSH_COMPLETE && expected!=KSH_DISCARDED)continue;
        sh_machine_t cpu={};cpu.sample=sample_ref;
        for(unsigned r=0;r<SR_REGISTERS;r++)for(unsigned k=0;k<4;k++)cpu.reg[r][k]=input[G+(r*4+k)*N+id];
        sh_run(&cpu,&shader);
        CHECK(status[G+id].executed==(unsigned)cpu.executed);
        CHECK((want==KSH_DISCARDED)==cpu.discarded);
        for(unsigned r=0;r<SR_REGISTERS;r++)for(unsigned k=0;k<4;k++){
            float a=cpu.reg[r][k],b=got[G+(r*4+k)*N+id];
            bool same=(std::isnan(a)&&std::isnan(b)) || a==b ||
                (std::isfinite(a)&&std::isfinite(b)&&fabsf(a-b)<=0.0001f*fmaxf(1,fabsf(a)));
            if(!same){std::fprintf(stderr,"case=%u op=%u lane=%u reg=%u comp=%u CPU=%g GPU=%g\n",
                cases,program[0].op,id,r,k,a,b);std::abort();}
            comparisons++;
        }
    }
    cases++;
}
int main(){
    cudaDeviceProp prop;CUDA(cudaGetDeviceProperties(&prop,0));CHECK(prop.major==12);
    for(unsigned u=0;u<4;u++){
        host_tex[u]={u*64ull,3,2,4,u&1,(u>>1)&1,0};
        for(unsigned y=0;y<2;y++)for(unsigned x=0;x<3;x++)
            pixels[u*16+y*4+x]=((128+u*20)<<24)|((x*90+u*5)<<16)|((y*160+u*7)<<8)|(u*40);
    }
    for(unsigned op=SH_MOV;op<=SH_MATMUL;op++)for(unsigned mask=0;mask<16;mask++){
        auto p=ins(op);p.mask=mask;p.swizzle[0]=(unsigned char)(mask*17);
        p.swizzle[1]=(unsigned char)(255-mask*11);p.swizzle[2]=(unsigned char)(mask*13);
        run({p,ins(SH_END)});
    }
    // Alias-safe matrix and cross product reads, vector masks and multiple instructions.
    run({ins(SH_MATMUL,1,1,8),ins(SH_CROSS,8,8,1),ins(SH_MAD,8,8,1,2),ins(SH_END)});
    for(unsigned op=SH_JMP;op<=SH_JMPNZ;op++){
        auto jump=ins(op,0,4);jump.target=2;
        run({jump,ins(SH_MUL),ins(SH_ADD),ins(SH_END)});
        jump.target=4;run({jump,ins(SH_MUL),ins(SH_ADD),ins(SH_END)});
    }
    run({ins(SH_DISCARD)},KSH_DISCARDED);
    auto loop=ins(SH_JMP);loop.target=0;run({loop},KSH_STEP_LIMIT,7);
    auto bad=ins(SH_MOV);bad.src[0]=200;run({bad},KSH_BAD_PROGRAM);
    bad=ins(SH_MOV);bad.dst=65535;run({bad},KSH_BAD_PROGRAM);
    bad=ins(SH_MOV);bad.mask=16;run({bad},KSH_BAD_PROGRAM);
    bad=ins(SH_MATMUL,10,198);run({bad},KSH_BAD_PROGRAM);
    bad=ins(255);run({bad},KSH_BAD_PROGRAM);
    bad=ins(SH_JMP);bad.target=-1;run({bad},KSH_BAD_PROGRAM);
    bad.target=2;run({bad},KSH_BAD_PROGRAM);
    run({ins(SH_MOV)},KSH_BAD_RESOURCE,128,1);
    run({ins(SH_MOV)},KSH_BAD_PROGRAM,128,2);
    run({ins(SH_MOV)},0xabcddcba,128,3);
    run({ins(SH_MOV)},KSH_BAD_RESOURCE,128,5);
    run({ins(SH_MOV)},KSH_BAD_PROGRAM,0);
    run({ins(SH_MOV)},KSH_BAD_PROGRAM,KSH_MAX_STEPS+1);
    for(auto &t:host_tex)t.filter=1;
    run({ins(SH_TEX),ins(SH_END)});
    run({ins(SH_TEX),ins(SH_END)},KSH_COMPLETE,128,6);
    run({ins(SH_TEX),ins(SH_END)},KSH_BAD_RESOURCE,128,4);
    for(auto &t:host_tex)t.width=t.height=0;
    run({ins(SH_TEX),ins(SH_END)});
    // Exactly exhausted budget after the final instruction is normal completion.
    run({ins(SH_ADD)},KSH_COMPLETE,1);
    std::printf("PASS %s: %u GPU bytecode cases, %u CPU-VM component comparisons; guards, masks, branches, textures and failures (Windows CUDA, not native OS proof)\n",prop.name,cases,comparisons);
}
