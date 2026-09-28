/* Real GPU arithmetic/pixel proof, under Windows CUDA, not Kestrel RM/QMD. */
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "gui_present.cu"
#define CUDA_OK(call) do { cudaError_t e=(call); if(e!=cudaSuccess) { \
    std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,cudaGetErrorString(e)); std::exit(1); }} while(0)

static void run(unsigned sw,unsigned sh,unsigned dw,unsigned dh,
                unsigned vx,unsigned vy,unsigned vw,unsigned vh, int mode, unsigned rotation=0) {
    unsigned sp=sw+7, dp=dw+13;
    std::vector<uint32_t> source((size_t)sp*sh);
    for(unsigned y=0;y<sh;y++) for(unsigned x=0;x<sp;x++)
        source[(size_t)y*sp+x]=0xff000000u|((x*1237u+y*7919u)&0xffffffu);
    std::vector<uint32_t> expected((size_t)dp*(dh+2),0xa55a1234), got=expected;
    uint32_t *ds,*dd;
    CUDA_OK(cudaMalloc(&ds,source.size()*4)); CUDA_OK(cudaMalloc(&dd,got.size()*4));
    CUDA_OK(cudaMemcpy(ds,source.data(),source.size()*4,cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(dd,got.data(),got.size()*4,cudaMemcpyHostToDevice));
    bool valid=vw && vh && vx<sw && vy<sh && vw<=sw-vx && vh<=sh-vy &&
        (rotation==0 || rotation==90 || rotation==180 || rotation==270);
    struct Clip {unsigned x,y,w,h;}; std::vector<Clip> clips;
    if(mode==0) clips.push_back({0,0,dw,dh});
    if(mode==1) for(unsigned y=0;y<dh;y+=37) for(unsigned x=0;x<dw;x+=53)
        clips.push_back({x,y,std::min(53u,dw-x),std::min(37u,dh-y)});
    if(mode==2) { clips.push_back({3,5,dw/2,dh/2}); clips.push_back({dw-7,dh-9,7,9}); }
    if(mode==3) clips.push_back({UINT32_MAX,UINT32_MAX,16,16});
    if(mode==4) clips.push_back({0,0,0,0});
    cudaEvent_t begin,end;CUDA_OK(cudaEventCreate(&begin));CUDA_OK(cudaEventCreate(&end));
    CUDA_OK(cudaEventRecord(begin));
    for(auto c:clips) {
        dim3 block(16,16),grid(std::max(1u,(c.w+15)/16),std::max(1u,(c.h+15)/16));
        gui_present<<<grid,block>>>(dd+dp,ds,sw,sh,sp,dw,dh,dp,vx,vy,vw,vh,c.x,c.y,c.w,c.h,rotation);
        CUDA_OK(cudaGetLastError());
    }
    CUDA_OK(cudaEventRecord(end));CUDA_OK(cudaEventSynchronize(end));
    float ms=0;CUDA_OK(cudaEventElapsedTime(&ms,begin,end));
    CUDA_OK(cudaMemcpy(got.data(),dd,got.size()*4,cudaMemcpyDeviceToHost));
    if(valid) for(auto c:clips) {
        uint64_t right=std::min<uint64_t>(dw,uint64_t(c.x)+c.w), bottom=std::min<uint64_t>(dh,uint64_t(c.y)+c.h);
        for(uint64_t y=c.y;y<bottom;y++) for(uint64_t x=c.x;x<right;x++) {
            // Independent full-output-coordinate nearest-neighbour reference.
            uint64_t sx=uint64_t(vx)+x*vw/dw, sy=uint64_t(vy)+y*vh/dh;
            if(rotation==90){sx=vx+y*vw/dh;sy=vy+(dw-x-1)*vh/dw;}
            if(rotation==180){sx=vx+(dw-x-1)*vw/dw;sy=vy+(dh-y-1)*vh/dh;}
            if(rotation==270){sx=vx+(dh-y-1)*vw/dh;sy=vy+x*vh/dw;}
            expected[(y+1)*dp+x]=source[sy*sp+sx];
        }
    }
    for(size_t i=0;i<got.size();i++) if(got[i]!=expected[i]) {
        std::fprintf(stderr,"FAIL source:%ux%u output:%ux%u viewport:%u,%u+%ux%u mode:%d index:%zu got:%08x expected:%08x\n",sw,sh,dw,dh,vx,vy,vw,vh,mode,i,got[i],expected[i]);std::exit(1);
    }
    CUDA_OK(cudaEventDestroy(begin));CUDA_OK(cudaEventDestroy(end));
    CUDA_OK(cudaFree(dd));CUDA_OK(cudaFree(ds));
    std::printf("PASS src:%ux%u -> %ux%u viewport:%u,%u+%ux%u mode:%d rotation:%u launches:%zu GPU %.3f ms (pixels/padding/guards)\n",sw,sh,dw,dh,vx,vy,vw,vh,mode,rotation,clips.size(),ms);
}
int main() {
    cudaDeviceProp p;CUDA_OK(cudaGetDeviceProperties(&p,0));
    std::printf("Presentation shader on %s SM %d.%d; Windows CUDA, NOT Kestrel display validation\n",p.name,p.major,p.minor);
    if(p.major!=12) return 1;
    run(1,1,1,1,0,0,1,1,0);
    run(53,37,101,89,0,0,53,37,1);
    run(101,89,53,37,0,0,101,89,1);
    run(321,239,853,479,17,13,293,211,2);
    run(3840,2160,1920,1080,0,0,3840,2160,0);
    run(1920,1080,3840,2160,0,0,1920,1080,0);
    for(unsigned head=0;head<3;head++) run(7680,1440,2560,1440,head*2560,0,2560,1440,0);
    run(53,37,53,37,0,0,53,37,3);
    run(53,37,53,37,0,0,53,37,4);
    run(53,37,53,37,UINT32_MAX,0,53,37,0);
    run(53,37,53,37,0,0,UINT32_MAX,37,0);
    run(53,37,53,37,0,0,0,37,0);
    for(unsigned rotation=0;rotation<=270;rotation+=90) {
        for(int mode=0;mode<5;mode++)run(321,239,853,479,17,13,293,211,mode,rotation);
        run(1,1,1,1,0,0,1,1,0,rotation);
        run(1080,1920,1920,1080,0,0,1080,1920,0,rotation);
        run(53,37,101,89,0,0,53,37,1,rotation);
    }
    run(53,37,101,89,0,0,53,37,0,45);
    run(53,37,101,89,0,0,53,37,0,UINT32_MAX);
    std::puts("PASS real-GPU viewport presentation arithmetic: 48 cases including all four rotations; Kestrel display configuration remains unverified");
}
