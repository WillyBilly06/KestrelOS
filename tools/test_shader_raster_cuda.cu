/* Independent command-major coverage, production CPU VM, and host depth/blend
 * reference against the real pixel-major GPU shader. No device helper is used
 * in the reference. Guard words/padding and per-lane execution counts checked.
 */
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "cpu_reference.h"
#include "shader_raster.cu"
extern "C" __global__ void shader_raster_fast(unsigned *,float *,const unsigned *,float *,
    ksh_status_t *,const kshr_job_t *,unsigned long long,unsigned long long,
    unsigned long long,unsigned long long,unsigned long long,unsigned long long);
static bool register_mode;
static unsigned register_cases;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL line %d case %u: %s\n",__LINE__,cases,#x);std::exit(1);}}while(0)
#define CUDA(x) CHECK((x)==cudaSuccess)
static unsigned cases=0;
static unsigned long long fragments=0;
static constexpr unsigned W=31,H=19,P=36,D=34,G=64;
static const kshr_job_t *active;
static const std::vector<unsigned> *atlas;
static double clamp(double x){return std::max(0.0,std::min(1.0,x));}
static int wrap(int x,int n,unsigned m){return m?std::max(0,std::min(n-1,x)):(x%n+n)%n;}
static void sample(int unit,float s,float t,float *out){
    out[0]=out[1]=out[2]=0;out[3]=1;
    if(unit<0||unit>=4)return;
    out[0]=out[1]=out[2]=1;if((unsigned)unit>=active->texture_count)return;
    const auto &d=active->textures[unit];if(!d.width&&!d.height)return;
    double u=d.wrap_s?clamp(s):s-std::floor(s),v=d.wrap_t?clamp(t):t-std::floor(t);
    u=u*d.width-.5;v=(1-v)*d.height-.5;
    int ix=(int)std::floor(u),iy=(int)std::floor(v);double fx=u-ix,fy=v-iy;
    if(!d.filter){ix=(int)std::floor(u+.5);iy=(int)std::floor(v+.5);fx=fy=0;}
    for(int k=0;k<4;k++){
        unsigned shift=k==3?24:16-k*8;double sum=0;
        for(int y=0;y<2;y++)for(int x=0;x<2;x++){
            auto p=(*atlas)[d.offset/4+wrap(iy+y,d.height,d.wrap_t)*d.pitch+wrap(ix+x,d.width,d.wrap_s)];
            sum+=((p>>shift)&255)/255.0*(x?fx:1-fx)*(y?fy:1-fy);
        }out[k]=(float)sum;
    }
}
static bool compare(unsigned op,float a,float b){
    switch(op){case 0:return false;case 1:return a<b;case 2:return a==b;case 3:return a<=b;
    case 4:return a>b;case 5:return a!=b;case 6:return a>=b;default:return true;}
}
static double factor(unsigned op,double a){const double f[]={0,1,a,1-a,1,0};return f[op];}
static unsigned pack(const float *c){return (unsigned)(clamp(c[0])*255+.5)*65536+
    (unsigned)(clamp(c[1])*255+.5)*256+(unsigned)(clamp(c[2])*255+.5);}
static void fragment(const kshr_job_t &job,const kshr_command_t &cmd,unsigned a,unsigned b,unsigned c,
    float l1,float l2,float iw,float z,unsigned x,unsigned y,
    std::vector<unsigned> &pixels,std::vector<float> &depth,std::vector<unsigned> &steps){
    const auto &r=cmd.raster;unsigned pi=G+y*P+x,di=G+y*D+x;
    if(!std::isfinite(z)||z<0||z>1||iw<=0||!std::isfinite(iw)||
       ((r.flags&KG3D_DEPTH_TEST)&&!compare(r.depth_func,z,depth[di])))return;
    sh_machine_t machine={};machine.sample=sample;
    sh_shader_t program={};program.compiled=true;program.count=job.code_count;
    std::memcpy(program.code,job.code,sizeof(program.code));
    for(unsigned reg=0;reg<SR_REGISTERS;reg++)if((reg>=SR_UNIFORM&&reg<SR_UNIFORM+SR_UNIFORM_N)||
        (reg>=SR_CONST&&reg<SR_CONST+SR_CONST_N))for(unsigned k=0;k<4;k++)machine.reg[reg][k]=job.seed[reg][k];
    for(unsigned reg=0;reg<job.varying_count;reg++)for(unsigned k=0;k<4;k++){
        float av=cmd.varying[a][reg][k];
        machine.reg[SR_VARYING+reg][k]=(av+(cmd.varying[b][reg][k]-av)*l1+(cmd.varying[c][reg][k]-av)*l2)/iw;
    }
    machine.reg[SR_FRAGCOORD][0]=x+.5f;
    machine.reg[SR_FRAGCOORD][1]=job.origin_lower_left?H-y-.5f:y+.5f;
    machine.reg[SR_FRAGCOORD][2]=z;machine.reg[SR_FRAGCOORD][3]=iw;
    machine.reg[SR_FRAGCOLOR][3]=1;
    sh_run(&machine,&program);steps[(y-job.clip_y)*job.clip_w+x-job.clip_x]+=machine.executed;fragments++;
    if(machine.discarded)return;
    float *rgba=machine.reg[SR_FRAGCOLOR];
    for(unsigned k=0;k<4;k++)if(!std::isfinite(rgba[k]))return;
    if((r.flags&KG3D_ALPHA_TEST)&&!compare(r.alpha_func,rgba[3],r.alpha_ref))return;
    if(r.flags&KG3D_BLEND){
        double sf=factor(r.blend_src,clamp(rgba[3])),df=factor(r.blend_dst,clamp(rgba[3]));
        for(unsigned k=0;k<3;k++)rgba[k]=(float)(rgba[k]*sf+((pixels[pi]>>(16-8*k))&255)/255.0*df);
    }
    pixels[pi]=pack(rgba);if(r.flags&KG3D_DEPTH_WRITE)depth[di]=z;
}
struct point{long long x,y;};
static long long determinant(point a,point b,point c){return a.x*(b.y-c.y)+b.x*(c.y-a.y)+c.x*(a.y-b.y);}
static bool inside(long long e,point a,point b){return e>0||(e==0&&(b.y<a.y||(b.y==a.y&&b.x<a.x)));}
static void reference(const kshr_job_t &job,std::vector<unsigned>&pixels,std::vector<float>&depth,std::vector<unsigned>&steps){
    for(unsigned i=0;i<job.command_count;i++){
        const auto &cmd=job.commands[i];const auto &r=cmd.raster;
        auto emit=[&](unsigned a,unsigned b,unsigned c,float l1,float l2,float iw,float z,int x,int y){
            if(x<(int)job.clip_x||y<(int)job.clip_y||x>=(int)(job.clip_x+job.clip_w)||y>=(int)(job.clip_y+job.clip_h)||
               x<r.x||y<r.y||x>=r.x+r.width||y>=r.y+r.height)return;
            fragment(job,cmd,a,b,c,l1,l2,iw,z,x,y,pixels,depth,steps);
        };
        if(r.flags&(KG3D_LINE|KG3D_POINT)){
            unsigned b=(r.flags&KG3D_POINT)?0:1;auto &a=r.v[0];auto &v=r.v[b];
            float dx=v.x-a.x,dy=v.y-a.y;int n=(int)std::max(std::fabs(dx),std::fabs(dy));if(n<1)n=1;
            for(int s=0;s<=((r.flags&KG3D_POINT)?0:n);s++){
                float t=(r.flags&KG3D_POINT)?0:(float)s/n;
                volatile float ox=dx*t,oy=dy*t,ow=(v.inv_w-a.inv_w)*t,oz=(v.z-a.z)*t;
                emit(0,b,0,t,0,a.inv_w+ow,a.z+oz,(int)(a.x+ox),(int)(a.y+oy));
            }continue;
        }
        point v[3];for(unsigned j=0;j<3;j++)v[j]={(long long)std::nearbyint(r.v[j].x*16.0),(long long)std::nearbyint(r.v[j].y*16.0)};
        long long area=determinant(v[0],v[1],v[2]);if(!area)continue;
        unsigned b=1,c=2;if(area<0){std::swap(v[1],v[2]);b=2;c=1;area=-area;}
        for(unsigned y=job.clip_y;y<job.clip_y+job.clip_h;y++)for(unsigned x=job.clip_x;x<job.clip_x+job.clip_w;x++){
            point p={(long long)x*16+8,(long long)y*16+8};
            long long e0=determinant(v[1],v[2],p),e1=determinant(v[2],v[0],p),e2=determinant(v[0],v[1],p);
            if(!inside(e0,v[1],v[2])||!inside(e1,v[2],v[0])||!inside(e2,v[0],v[1]))continue;
            float l1=(float)e1/(float)area,l2=(float)e2/(float)area;
            emit(0,b,c,l1,l2,r.v[0].inv_w+(r.v[b].inv_w-r.v[0].inv_w)*l1+(r.v[c].inv_w-r.v[0].inv_w)*l2,
                 r.v[0].z+(r.v[b].z-r.v[0].z)*l1+(r.v[c].z-r.v[0].z)*l2+r.depth_bias,x,y);
        }
    }
}
static sh_instruction_t ins(unsigned op,unsigned dst=0,unsigned a=0,unsigned b=0,unsigned c=0){
    sh_instruction_t r={};r.op=op;r.mask=15;r.dst=dst;r.src[0]=a;r.src[1]=b;r.src[2]=c;
    r.swizzle[0]=r.swizzle[1]=r.swizzle[2]=SH_SWIZZLE_XYZW;return r;
}
static kshr_job_t base(){
    kshr_job_t j={};j.width=W;j.height=H;j.pitch=P;j.depth_pitch=D;j.clip_w=W;j.clip_h=H;
    j.command_count=2;j.code_count=2;j.budget=32;j.varying_count=8;
    j.code[0]=ins(SH_MOV,SR_FRAGCOLOR,SR_VARYING);j.code[1]=ins(SH_END);
    const float xy[2][3][2]={{{0,0},{W,0},{0,H}},{{W,0},{W,H},{0,H}}};
    for(unsigned i=0;i<2;i++){
        auto &cmd=j.commands[i];auto &r=cmd.raster;r.width=W;r.height=H;
        r.flags=KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE;r.depth_func=KG3D_LESS;r.alpha_func=KG3D_ALWAYS;
        r.blend_src=KG3D_SRC_ALPHA;r.blend_dst=KG3D_ONE_MINUS_SRC_ALPHA;
        for(unsigned v=0;v<3;v++){
            r.v[v].x=xy[i][v][0];r.v[v].y=xy[i][v][1];r.v[v].z=.5f;r.v[v].inv_w=1;
            for(unsigned reg=0;reg<8;reg++)for(unsigned k=0;k<4;k++)cmd.varying[v][reg][k]=(k+reg+1)/16.f;
        }
    }return j;
}
static size_t upload_packet(kshr_job_t *device,const kshr_job_t &job){
    size_t bytes=sizeof job;
    if(job.command_count>=1&&job.command_count<=KG3D_MAX_COMMANDS)
        bytes=offsetof(kshr_job_t,commands)+(size_t)job.command_count*sizeof(job.commands[0]);
    // Inactive records deliberately contain malformed flags and NaNs. The
    // production kernel transfers only the validated prefix into a full slot.
    CUDA(cudaMemset(device,0xff,sizeof job));
    CUDA(cudaMemcpy(device,&job,bytes,cudaMemcpyHostToDevice));
    return bytes;
}
static void check_packet(kshr_job_t *device,const kshr_job_t &job,size_t bytes){
    kshr_job_t actual;
    CUDA(cudaMemcpy(&actual,device,sizeof actual,cudaMemcpyDeviceToHost));
    CHECK(!std::memcmp(&actual,&job,bytes));
    const unsigned char *tail=(const unsigned char*)&actual;
    for(size_t i=bytes;i<sizeof actual;i++)CHECK(tail[i]==0xff);
}
static void run(kshr_job_t job,unsigned expected_error=0,unsigned short_resource=0,bool sparse_reset=false){
    cases++;unsigned lanes=job.clip_w*job.clip_h;
    std::vector<unsigned> initial(G+P*H+G,0x17395b),pixels=initial,expected=initial,texture(96);
    for(unsigned i=0;i<texture.size();i++)texture[i]=0xff000000u|(i*719323u&0xffffffu);
    std::vector<float> initial_z(G+D*H+G,1.0f),depth=initial_z,expected_z=initial_z;
    std::vector<float> scratch(G+(size_t)lanes*SR_REGISTERS*4+G,-123.25f);
    std::vector<ksh_status_t> statuses(G+lanes+G,{0xabababab,0xcdcdcdcd});
    std::vector<unsigned> steps(lanes);
    if(!expected_error){active=&job;atlas=&texture;reference(job,expected,expected_z,steps);}
    unsigned *pd,*pt;float *pz,*ps;ksh_status_t *st;kshr_job_t *pj;
    CUDA(cudaMalloc(&pd,pixels.size()*4));CUDA(cudaMalloc(&pz,depth.size()*4));
    CUDA(cudaMalloc(&pt,texture.size()*4));CUDA(cudaMalloc(&ps,scratch.size()*4));
    CUDA(cudaMalloc(&st,statuses.size()*sizeof(*st)));CUDA(cudaMalloc(&pj,sizeof(job)));
    CUDA(cudaMemcpy(pd,pixels.data(),pixels.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(pz,depth.data(),depth.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(pt,texture.data(),texture.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(ps,scratch.data(),scratch.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(st,statuses.data(),statuses.size()*sizeof(*st),cudaMemcpyHostToDevice));
    size_t packet_upload=upload_packet(pj,job);
    bool fast=register_mode&&kshr_register_fast_eligible(&job);
    if(fast){
    shader_raster_fast<<<(lanes+63)/64,64>>>(pd+G,short_resource==4?nullptr:pz+G,pt,ps+G,st+G,pj,
        short_resource==1?4:P*H*4, D*H*4,texture.size()*4,
        short_resource==2?4:0,
        (unsigned long long)lanes*sizeof(*st),sizeof(job));register_cases++;
    }else shader_raster<<<(lanes+63)/64,64>>>(pd+G,short_resource==4?nullptr:pz+G,pt,ps+G,st+G,pj,
        short_resource==1?4:P*H*4, D*H*4,texture.size()*4,
        short_resource==2?4:(unsigned long long)lanes*SR_REGISTERS*16,
        (unsigned long long)lanes*sizeof(*st),sizeof(job));
    CUDA(cudaDeviceSynchronize());CUDA(cudaGetLastError());
    check_packet(pj,job,packet_upload);
    CUDA(cudaMemcpy(pixels.data(),pd,pixels.size()*4,cudaMemcpyDeviceToHost));
    CUDA(cudaMemcpy(depth.data(),pz,depth.size()*4,cudaMemcpyDeviceToHost));
    CUDA(cudaMemcpy(scratch.data(),ps,scratch.size()*4,cudaMemcpyDeviceToHost));
    if(fast)for(float v:scratch)CHECK(v==-123.25f); // no global register writes, including padding
    CUDA(cudaMemcpy(statuses.data(),st,statuses.size()*sizeof(*st),cudaMemcpyDeviceToHost));
    if(sparse_reset)for(unsigned i=0;i<lanes*4;i++)
        CHECK(scratch[G+50u*4u*lanes+i]==-123.25f); // provably unused register never cleared
    for(unsigned i=0;i<pixels.size();i++){
        if(expected_error)CHECK(pixels[i]==initial[i]);
        else for(unsigned k=0;k<3;k++)CHECK(std::abs(int((pixels[i]>>(8*k))&255)-int((expected[i]>>(8*k))&255))<=1);
    }
    for(unsigned i=0;i<depth.size();i++)CHECK(std::fabs(depth[i]-(expected_error?initial_z[i]:expected_z[i]))<1e-5f);
    for(unsigned i=0;i<lanes;i++){
        if(statuses[G+i].result!=(expected_error?expected_error:KSH_COMPLETE)){
            std::fprintf(stderr,"lane %u result %u steps %u expected %u\n",i,statuses[G+i].result,statuses[G+i].executed,expected_error);CHECK(false);
        }
        if(!expected_error)CHECK(statuses[G+i].executed==steps[i]);
    }
    for(unsigned i=0;i<G;i++){
        CHECK(pixels[i]==initial[i]&&pixels[pixels.size()-1-i]==initial.back());
        CHECK(scratch[i]==-123.25f&&scratch[scratch.size()-1-i]==-123.25f);
        CHECK(statuses[i].result==0xabababab&&statuses[statuses.size()-1-i].result==0xabababab);
    }
    CUDA(cudaFree(pd));CUDA(cudaFree(pz));CUDA(cudaFree(pt));CUDA(cudaFree(ps));CUDA(cudaFree(st));CUDA(cudaFree(pj));
}
static void run_strided(unsigned phase){
    cases++;
    constexpr unsigned w=137,h=293,pitch=144,zpitch=149;
    kshr_job_t job={};job.width=w;job.height=h;job.pitch=pitch;job.depth_pitch=zpitch;
    job.clip_x=3;job.clip_y=2;job.clip_w=131;job.clip_h=289;
    job.command_count=2;job.code_count=2;job.budget=8;job.origin_lower_left=phase==1;
    job.code[0]=ins(SH_MUL,SR_FRAGCOLOR,SR_FRAGCOORD,SR_UNIFORM);job.code[1]=ins(SH_END);
    job.seed[SR_UNIFORM][0]=1.f/w;job.seed[SR_UNIFORM][1]=1.f/h;
    job.seed[SR_UNIFORM][2]=job.seed[SR_UNIFORM][3]=1;
    for(unsigned i=0;i<2;i++){
        auto &r=job.commands[i].raster;r.width=w;r.height=h;
        r.flags=KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE;r.depth_func=KG3D_LESS;
        for(auto &v:r.v){v.z=.25f;v.inv_w=1;}
        if(!i){r.v[1].x=w;r.v[2].x=w;r.v[2].y=h;}
        else {r.v[1].x=w;r.v[1].y=h;r.v[2].y=h;}
    }
    if(phase>=2&&phase<=4){
        job.seed[SR_UNIFORM][0]=33;
        job.seed[SR_CONST][0]=.25f;job.seed[SR_CONST][1]=.5f;
        job.seed[SR_CONST][2]=.75f;job.seed[SR_CONST][3]=1;
        job.seed[0][0]=99; // unused seed/temp must not leak into another pixel
        job.code[0]=ins(phase==4?SH_SGE:SH_SLT,1,SR_FRAGCOORD,SR_UNIFORM);
        job.code[0].mask=1;job.code[0].swizzle[0]=0x55;
        job.code[1]=ins(phase==4?SH_JMPNZ:SH_JMPZ,0,1);
        if(phase==2){
            job.code_count=5;job.code[1].target=3;
            job.code[2]=ins(SH_MOV,0,SR_CONST);job.code[3]=ins(SH_MOV,SR_FRAGCOLOR,0);job.code[4]=ins(SH_END);
        }else if(phase==3){
            job.code_count=5;job.code[1].target=4;
            job.code[2]=ins(SH_MOV,SR_FRAGCOLOR,SR_CONST);job.code[3]=ins(SH_END);job.code[4]=ins(SH_DISCARD);
        }else{
            job.code_count=4;job.code[1].target=3;
            job.code[2]=ins(SH_END);job.code[3]=ins(SH_JMP);job.code[3].target=3;
        }
    }
    if(phase==7)job.budget=KSH_MAX_STEPS; // exceeds original total-work limit
    bool fast=register_mode&&kshr_register_fast_eligible(&job);
    unsigned total=job.clip_w*job.clip_h,lanes=fast?KSHR_FAST_LANES(total):KSHR_LANES(total);
    CHECK(total>2*lanes&&total%lanes);
    std::vector<unsigned> pixels(G+pitch*h+G,0x17395b),expected=pixels;
    std::vector<float> depth(G+zpitch*h+G,1.f),wanted_depth=depth;
    std::vector<float> scratch(G+(size_t)lanes*SR_REGISTERS*4+G,-123.25f);
    std::vector<ksh_status_t> states(G+lanes+G,{0xabababab,0xcdcdcdcd});
    std::vector<unsigned> steps(lanes);
    if(phase<5)for(unsigned lane=0;lane<lanes;lane++){
        for(unsigned pixel=lane;pixel<total;pixel+=lanes){
            unsigned x=job.clip_x+pixel%job.clip_w,y=job.clip_y+pixel/job.clip_w;
            bool low=y<33;
            if(phase==4&&!low){steps[lane]+=8;break;} // prior pixels stay written, later ones untouched
            steps[lane]+=phase==4?3:phase==3?(low?4:3):phase==2?(low?5:4):2;
            if(phase==3&&!low)continue;
            float rgba[4]={0,0,0,1};
            if(phase<2){rgba[0]=(x+.5f)/w;rgba[1]=(phase==1?h-y-.5f:y+.5f)/h;rgba[2]=.25f;}
            else if(phase!=4&&low){rgba[0]=.25f;rgba[1]=.5f;rgba[2]=.75f;}
            expected[G+y*pitch+x]=pack(rgba);wanted_depth[G+y*zpitch+x]=.25f;
        }
    }
    unsigned *pd;float *pz,*ps;ksh_status_t *st;kshr_job_t *pj;
    CUDA(cudaMalloc(&pd,pixels.size()*4));CUDA(cudaMalloc(&pz,depth.size()*4));
    CUDA(cudaMalloc(&ps,scratch.size()*4));CUDA(cudaMalloc(&st,states.size()*sizeof(*st)));CUDA(cudaMalloc(&pj,sizeof job));
    CUDA(cudaMemcpy(pd,pixels.data(),pixels.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(pz,depth.data(),depth.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(ps,scratch.data(),scratch.size()*4,cudaMemcpyHostToDevice));
    CUDA(cudaMemcpy(st,states.data(),states.size()*sizeof(*st),cudaMemcpyHostToDevice));
    size_t packet_upload=upload_packet(pj,job);
    if(fast){
    shader_raster_fast<<<(lanes+63)/64+1,64>>>(pd+G,pz+G,nullptr,ps+G,st+G,pj,
        pitch*h*4,zpitch*h*4,0,phase==5?1:0,
        (unsigned long long)lanes*sizeof(*st)-(phase==6?1:0),sizeof job);register_cases++;
    }else shader_raster<<<(lanes+63)/64+1,64>>>(pd+G,pz+G,nullptr,ps+G,st+G,pj,
        pitch*h*4,zpitch*h*4,0,KSHR_SCRATCH_BYTES(lanes)-(phase==5?1:0),
        (unsigned long long)lanes*sizeof(*st)-(phase==6?1:0),sizeof job);
    CUDA(cudaDeviceSynchronize());CUDA(cudaGetLastError());
    check_packet(pj,job,packet_upload);
    CUDA(cudaMemcpy(pixels.data(),pd,pixels.size()*4,cudaMemcpyDeviceToHost));
    CUDA(cudaMemcpy(depth.data(),pz,depth.size()*4,cudaMemcpyDeviceToHost));
    CUDA(cudaMemcpy(scratch.data(),ps,scratch.size()*4,cudaMemcpyDeviceToHost));
    CUDA(cudaMemcpy(states.data(),st,states.size()*sizeof(*st),cudaMemcpyDeviceToHost));
    if(fast)for(float v:scratch)CHECK(v==-123.25f);
    for(unsigned i=0;i<pixels.size();i++)for(unsigned k=0;k<24;k+=8)
        CHECK(std::abs(int((pixels[i]>>k)&255)-int((expected[i]>>k)&255))<=1);
    for(unsigned i=0;i<depth.size();i++)CHECK(depth[i]==wanted_depth[i]);
    for(unsigned i=0;i<lanes;i++){
        unsigned result=phase==4?KSH_STEP_LIMIT:phase==5?KSH_BAD_RESOURCE:phase==6?0xabababab:phase==7?KSH_BAD_PROGRAM:KSH_COMPLETE;
        CHECK(states[G+i].result==result);
        CHECK(states[G+i].executed==(phase==6?0xcdcdcdcd:steps[i]));
    }
    for(unsigned i=0;i<G;i++){
        CHECK(scratch[i]==-123.25f&&scratch[scratch.size()-1-i]==-123.25f);
        CHECK(states[i].result==0xabababab&&states[states.size()-1-i].result==0xabababab);
    }
    CUDA(cudaFree(pd));CUDA(cudaFree(pz));CUDA(cudaFree(ps));CUDA(cudaFree(st));CUDA(cudaFree(pj));
    std::printf("PASS strided phase %u: %u pixels / %u workers, exact aggregate steps and guarded colour/depth/scratch/status\n",phase,total,lanes);
}
int main(int argc,char **){
    register_mode=argc>1;
    cudaDeviceProp prop;CUDA(cudaGetDeviceProperties(&prop,0));auto j=base();run(j,0,0,true);
    // Every register class and both sides of each 64-bit mask boundary. The
    // independent CPU VM starts from zero plus permitted uniform/constant seed;
    // GPU scratch starts poisoned, so a missed input reset changes the pixels.
    for(unsigned reg=0;reg<SR_REGISTERS;reg++){
        j=base();j.code[0]=ins(SH_MOV,SR_FRAGCOLOR,reg);
        for(unsigned r=0;r<SR_REGISTERS;r++)for(unsigned k=0;k<4;k++)j.seed[r][k]=(k+1)*.125f;
        run(j);
    }
    for(unsigned reg: {0u,61u,63u,64u,109u,125u,127u,128u,133u,136u,189u,192u,196u}){
        j=base();j.code[0]=ins(SH_MATMUL,SR_FRAGCOLOR,reg,SR_UNIFORM);
        for(unsigned r=0;r<SR_REGISTERS;r++)for(unsigned k=0;k<4;k++)j.seed[r][k]=(k+1)*.125f;
        run(j);
    }
    j=base();j.code_count=3;j.code[2]=ins(SH_MATMUL,65535,65535,65535,65535);
    run(j); // unreachable malformed instruction must not become an eager rejection
    std::puts("PASS sparse reset: all 200 registers, implicit matrix columns across mask boundaries, untouched unused scratch and unreachable malformed instruction");
    // Every register-resident ALU operation, all eight mutable temporaries,
    // component masks and aliased vector swizzles. Move results into [0,1]
    // before readback so negative ALU mistakes cannot hide behind clamping.
    for(unsigned op: {SH_MOV,SH_ADD,SH_SUB,SH_MUL,SH_MAD,SH_MIN,SH_MAX,
                      SH_NEG,SH_ABS,SH_SLT,SH_SLE,SH_SGT,SH_SGE,SH_SEQ,
                      SH_SNE,SH_AND,SH_OR,SH_NOT})
    for(unsigned temp=0;temp<8;temp++)for(unsigned mask=1;mask<16;mask+=2){
        j=base();j.code_count=6;
        const float a[4]={-.5f,0,.25f,.5f},b[4]={.5f,0,-.25f,.25f};
        for(unsigned c=0;c<4;c++){
            j.seed[SR_UNIFORM][c]=a[c];j.seed[SR_UNIFORM+1][c]=b[c];
            j.seed[SR_UNIFORM+2][c]=.25f;j.seed[SR_UNIFORM+3][c]=.5f;
        }
        j.code[0]=ins(SH_MOV,temp,SR_UNIFORM);
        j.code[1]=ins(op,temp,temp,SR_UNIFORM+1,SR_UNIFORM+2);
        j.code[1].mask=mask;
        j.code[1].swizzle[0]=(temp&1)?0x1b:0xe4;
        j.code[1].swizzle[1]=(temp&2)?0x4e:0xe4;
        j.code[1].swizzle[2]=(temp&4)?0xb1:0xe4;
        j.code[2]=ins(SH_MUL,temp,temp,SR_UNIFORM+2);
        j.code[3]=ins(SH_ADD,temp,temp,SR_UNIFORM+3);
        j.code[4]=ins(SH_MOV,SR_FRAGCOLOR,temp);j.code[5]=ins(SH_END);
        CHECK(kshr_register_fast_eligible(&j));run(j);
    }
    std::puts("PASS register ALU: 18 ops, all eight temp destinations, 8 masks, source/destination aliases and vector swizzles against CPU VM");
    for(unsigned count=1;count<=KG3D_MAX_COMMANDS;count++){
        j=base();j.command_count=count;
        for(unsigned i=2;i<count;i++)j.commands[i]=j.commands[i&1];
        run(j);
    }
    std::puts("PASS all 64 active-command prefix sizes with malformed inactive tail; packet immutable");
    // Perspective-correct eight varying registers, both winding orders and tails.
    for(unsigned reg=0;reg<8;reg++)for(unsigned reverse=0;reverse<2;reverse++){
        j=base();j.code[0].src[0]=SR_VARYING+reg;
        for(unsigned i=0;i<2;i++)for(unsigned v=0;v<3;v++){
            auto &c=j.commands[i];float iw=(v+1)*.5f;c.raster.v[v].inv_w=iw;
            for(unsigned r=0;r<8;r++)for(unsigned k=0;k<4;k++)c.varying[v][r][k]=((r+k+v+1)/16.f)*iw;
        }
        if(reverse)for(auto &c:j.commands){std::swap(c.raster.v[1],c.raster.v[2]);for(unsigned r=0;r<8;r++)for(unsigned k=0;k<4;k++)std::swap(c.varying[1][r][k],c.varying[2][r][k]);}
        run(j);
    }
    for(unsigned origin=0;origin<2;origin++){
        j=base();j.origin_lower_left=origin;j.code[0]=ins(SH_MUL,SR_FRAGCOLOR,SR_FRAGCOORD,SR_UNIFORM);
        j.seed[SR_UNIFORM][0]=1.f/W;j.seed[SR_UNIFORM][1]=1.f/H;j.seed[SR_UNIFORM][2]=j.seed[SR_UNIFORM][3]=1;run(j);
    }
    // Shared-edge blending, all depth/alpha comparisons and factors.
    for(unsigned f=0;f<8;f++)for(unsigned b=0;b<6;b++){
        j=base();for(auto &c:j.commands){c.raster.flags|=KG3D_BLEND|KG3D_ALPHA_TEST;c.raster.alpha_func=f;
            c.raster.alpha_ref=.25f;c.raster.blend_src=b;c.raster.blend_dst=5-b;}run(j);
        j=base();j.command_count=4;j.commands[2]=j.commands[0];j.commands[3]=j.commands[1];
        for(unsigned i=2;i<4;i++){j.commands[i].raster.depth_func=f;j.commands[i].raster.flags|=KG3D_BLEND;
            j.commands[i].raster.blend_src=b;j.commands[i].raster.blend_dst=5-b;}run(j);
    }
    // Discard must not occlude the later triangles; masked writes reset alpha.
    j=base();j.command_count=4;j.commands[2]=j.commands[0];j.commands[3]=j.commands[1];
    for(unsigned i=0;i<4;i++)for(unsigned v=0;v<3;v++)j.commands[i].varying[v][1][0]=i<2?1:0;
    j.code_count=4;j.code[0]=ins(SH_JMPZ,0,SR_VARYING+1);j.code[0].target=2;
    j.code[1]=ins(SH_DISCARD);j.code[2]=ins(SH_MOV,SR_FRAGCOLOR,SR_VARYING);j.code[2].mask=7;j.code[3]=ins(SH_END);run(j);
    // No temp/output state leaks between primitives even when the seed is dirty.
    j=base();j.command_count=4;j.commands[2]=j.commands[0];j.commands[3]=j.commands[1];
    for(unsigned i=0;i<4;i++){j.commands[i].raster.flags=KG3D_BLEND;for(unsigned v=0;v<3;v++)j.commands[i].varying[v][1][0]=i<2?1:0;}
    j.seed[0][0]=99;j.seed[SR_FRAGCOLOR][3]=99;
    j.code_count=5;j.code[0]=ins(SH_JMPZ,0,SR_VARYING+1);j.code[0].target=2;
    j.code[1]=ins(SH_MOV,0,SR_VARYING);j.code[2]=ins(SH_MOV,SR_FRAGCOLOR,0);j.code[2].mask=7;
    j.code[3]=ins(SH_MOV,0,SR_VARYING);j.code[4]=ins(SH_END);run(j);
    for(unsigned unit=0;unit<4;unit++)for(unsigned filter=0;filter<2;filter++)for(unsigned wrap=0;wrap<2;wrap++){
        j=base();j.texture_count=4;j.textures[unit]={unit*64ull,3,3,4,wrap,1-wrap,filter};
        j.seed[SR_UNIFORM][0]=(float)unit;j.code[0]=ins(SH_TEX,SR_FRAGCOLOR,SR_VARYING,SR_UNIFORM);
        for(unsigned i=0;i<2;i++)for(unsigned v=0;v<3;v++){j.commands[i].varying[v][0][0]=v*.75f-.5f;j.commands[i].varying[v][0][1]=i+v*.25f;}
        run(j);
    }
    // Forward reference DDA vs inverse GPU candidates: endpoints, slopes, repeats.
    for(unsigned i=0;i<40;i++){
        j=base();j.command_count=1;auto &c=j.commands[0];c.raster.flags=KG3D_LINE|KG3D_BLEND;
        c.raster.v[0].x=(i%3)*.25f;c.raster.v[0].y=(i%7)*.5f;
        c.raster.v[1].x=(i&1)?W-1.25f:c.raster.v[0].x+.1f;c.raster.v[1].y=(i%4)?H-1.25f:c.raster.v[0].y+.1f;
        c.raster.v[1].inv_w=.5f;if(i&2)std::swap(c.raster.v[0],c.raster.v[1]);
        run(j);c.raster.flags=KG3D_POINT|KG3D_BLEND;run(j);
    }
    j=base();j.clip_x=5;j.clip_y=3;j.clip_w=17;j.clip_h=11;run(j);
    // Global resource/admission errors and every covered pixel's VM failures.
    j=base();run(j,KSH_BAD_RESOURCE,1);run(j,KSH_BAD_RESOURCE,2);run(j,KSH_BAD_RESOURCE,4);
    j.reserved[0]=1;run(j,KSH_BAD_PROGRAM);
    j=base();j.budget=KSH_MAX_STEPS;j.command_count=64;
    for(unsigned i=2;i<64;i++)j.commands[i]=j.commands[i&1];run(j,KSH_BAD_PROGRAM);
    j=base();j.code[0].dst=SR_REGISTERS;run(j,KSH_BAD_PROGRAM);
    j=base();j.code_count=1;j.budget=3;j.code[0]=ins(SH_JMP);j.code[0].target=0;run(j,KSH_STEP_LIMIT);
    j=base();j.code[0]=ins(SH_TEX,SR_FRAGCOLOR,SR_VARYING,SR_UNIFORM);j.texture_count=1;j.textures[0]={0,400,3,400,0,0,0};run(j,KSH_BAD_RESOURCE);
    // A malformed later command must roll back this lane's earlier local result.
    j=base();j.command_count=3;j.commands[2]=j.commands[0];j.commands[2].raster.flags=0xffffffff;run(j,KSH_BAD_PROGRAM);
    for(unsigned phase=0;phase<8;phase++)run_strided(phase);
    if(register_mode){CHECK(register_cases>400);std::printf("PASS %u register-resident cases: entire global scratch unchanged\n",register_cases);}
    std::printf("PASS %s: %u programmable GPU raster cases, %llu CPU-VM fragment references plus analytical strided cases; varying/perspective, coverage, order, discard, textures, lines/points, late-pixel errors, aggregate status, guards/failures (Windows CUDA, not native OS proof)\n",prop.name,cases,fragments);
}
