/* Runs the production compaction shader on the host's CUDA GPU. This proves
 * neither Kestrel's native launcher nor desktop frame timing. */
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../include/kestrel/shader_setup.h"

extern "C" __global__ void shader_setup_compact(const kshr_command_t *,const kshs_primitive_t *,
    kshr_command_t *,kshs_rect_t *,kshs_compact_status_t *,unsigned long long,
    unsigned long long,unsigned long long,unsigned long long,unsigned long long,unsigned);
extern "C" __global__ void shader_setup_compact_small(const kshr_command_t *,const kshs_primitive_t *,
    kshr_command_t *,kshs_rect_t *,kshs_compact_status_t *,unsigned long long,
    unsigned long long,unsigned long long,unsigned long long,unsigned long long,unsigned);

#define CU(call) do { cudaError_t e=(call); if(e!=cudaSuccess) { std::fprintf(stderr,"CUDA FAIL line %d: %s\n",__LINE__,cudaGetErrorString(e)); std::exit(1); } } while(0)

static unsigned cases;
static void check(unsigned n,unsigned pattern,unsigned fault,unsigned position,bool timing=false) {
    const unsigned capacity=n*KSHS_OUTPUT_TRIANGLES;
    std::vector<kshs_primitive_t> setup(n);
    std::vector<kshr_command_t> input(capacity),output(capacity+8),expected(capacity+8);
    std::vector<kshs_rect_t> rects(capacity+8),expected_rects(capacity+8);
    std::vector<kshs_compact_status_t> status(n+64),expected_status(n+64);
    // Distinct payload words detect record reordering and partial/cross-record
    // copies, not only rectangle mismatches. All values encode finite floats.
    for(size_t i=0;i<input.size();i++)for(size_t word=0;word<sizeof(input[0])/4;word++) {
        unsigned value=0x3e000000u+(unsigned)((i*(sizeof(input[0])/4)+word)%0x7fffffu);
        std::memcpy((unsigned char*)&input[i]+word*4,&value,4);
    }
    std::memset(expected.data(),0xa5,expected.size()*sizeof(expected[0]));
    std::memset(expected_rects.data(),0xa5,expected_rects.size()*sizeof(expected_rects[0]));
    std::memset(expected_status.data(),0xa5,expected_status.size()*sizeof(expected_status[0]));
    for(unsigned i=0;i<n;i++) {
        auto &p=setup[i];p.result=KSH_COMPLETE;
        p.count=pattern<9?pattern:(i*17u+i/7u)%9u;
        for(unsigned j=0;j<p.count;j++) {
            p.bounds[j]={(int)(i%127),(int)(j*3),5,7};
            auto &c=input[i*8+j].raster;
            c.x=p.bounds[j].x;c.y=p.bounds[j].y;c.width=5;c.height=7;
            c.flags=KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND;
        }
    }
    unsigned long long ib=KSHS_COMMAND_BYTES(n),sb=KSHS_RESULT_BYTES(n);
    unsigned long long ob=ib,rb=KSHS_COMPACT_RECT_BYTES(capacity),tb=KSHS_COMPACT_STATUS_BYTES(n);
    if(fault==1)setup[position].result=KSH_PENDING;
    if(fault==2)setup[position].count=9;
    if(fault==3)setup[position].bounds[0].width=0;
    if(fault==4)input[position*8].raster.x++;
    if(fault==5)input[position*8].raster.flags|=KG3D_LINE;
    if(fault==6)ib--;
    if(fault==7)sb--;
    if(fault==8)ob=0;
    if(fault==9)rb=0;
    if(fault==10)tb--;
    if(fault==11)setup[position].bounds[0].x=32768;
    if(fault==12)setup[position].bounds[0].height=-1;
    const unsigned threads=fault==13?16u:64u;
    // Independent serial reference: validate counts once, then compact in input
    // order, with the original per-primitive validation/failure semantics.
    bool resources=ib>=KSHS_COMMAND_BYTES(n)&&sb>=KSHS_RESULT_BYTES(n)&&threads%32==0&&fault<14;
    unsigned total=0;bool counts_ok=true;
    for(const auto &p:setup) {
        if(p.result!=KSH_COMPLETE||p.count>8)counts_ok=false;
        else total+=p.count;
    }
    unsigned first=0;
    if(tb>=KSHS_COMPACT_STATUS_BYTES(n))for(unsigned i=0;i<n;i++) {
        auto &s=expected_status[i];s={};s.result=KSH_BAD_RESOURCE;
        if(!resources)continue;
        if(!counts_ok){s.result=KSH_BAD_PROGRAM;continue;}
        if((unsigned long long)total*sizeof(kshr_command_t)>ob||KSHS_COMPACT_RECT_BYTES(total)>rb)continue;
        const auto &p=setup[i];bool valid=true;
        for(unsigned j=0;j<p.count;j++) {
            const auto &r=p.bounds[j];const auto &c=input[i*8+j].raster;
            if(r.x<0||r.y<0||r.width<=0||r.height<=0||r.x>=32768||r.y>=32768||
               r.width>32768-r.x||r.height>32768-r.y||c.x!=r.x||c.y!=r.y||
               c.width!=r.width||c.height!=r.height||
               (c.flags&~(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND)))valid=false;
        }
        if(valid) {
            for(unsigned j=0;j<p.count;j++){expected[first+j]=input[i*8+j];expected_rects[first+j]=p.bounds[j];}
            s.result=KSH_COMPLETE;s.count=p.count;s.first=first;s.total=total;
        } else s.result=KSH_BAD_PROGRAM;
        first+=p.count;
    }
    kshr_command_t *di,*dout;kshs_primitive_t *ds;kshs_rect_t *dr;kshs_compact_status_t *dt;
    CU(cudaMalloc(&di,input.size()*sizeof(input[0])));CU(cudaMalloc(&ds,setup.size()*sizeof(setup[0])));
    CU(cudaMalloc(&dout,output.size()*sizeof(output[0])));CU(cudaMalloc(&dr,rects.size()*sizeof(rects[0])));
    CU(cudaMalloc(&dt,status.size()*sizeof(status[0])));
    CU(cudaMemcpy(di,input.data(),input.size()*sizeof(input[0]),cudaMemcpyHostToDevice));
    CU(cudaMemcpy(ds,setup.data(),setup.size()*sizeof(setup[0]),cudaMemcpyHostToDevice));
    CU(cudaMemset(dout,0xa5,output.size()*sizeof(output[0])));CU(cudaMemset(dr,0xa5,rects.size()*sizeof(rects[0])));
    CU(cudaMemset(dt,0xa5,status.size()*sizeof(status[0])));
    const kshr_command_t *launch_input=fault==14?(const kshr_command_t*)((const char*)di+4):di;
    kshr_command_t *launch_output=fault==15?(kshr_command_t*)((char*)dout+4):dout;
    shader_setup_compact<<<(n+threads-1)/threads,threads>>>(launch_input,ds,launch_output,dr,dt,ib,sb,ob,rb,tb,n);
    CU(cudaGetLastError());CU(cudaDeviceSynchronize());
    CU(cudaMemcpy(output.data(),dout,output.size()*sizeof(output[0]),cudaMemcpyDeviceToHost));
    CU(cudaMemcpy(rects.data(),dr,rects.size()*sizeof(rects[0]),cudaMemcpyDeviceToHost));
    CU(cudaMemcpy(status.data(),dt,status.size()*sizeof(status[0]),cudaMemcpyDeviceToHost));
    if(std::memcmp(output.data(),expected.data(),output.size()*sizeof(output[0]))||
       std::memcmp(rects.data(),expected_rects.data(),rects.size()*sizeof(rects[0]))||
       std::memcmp(status.data(),expected_status.data(),status.size()*sizeof(status[0]))) {
        std::fprintf(stderr,"FAIL n=%u pattern=%u fault=%u position=%u\n",n,pattern,fault,position);std::exit(1);
    }
    if(timing) {
        // A captured chain of 200 launches removes per-kernel host-enqueue gaps.
        // CUDA event duration still includes device scheduling/graph overhead,
        // and excludes Kestrel submission, waits and display.
        cudaEvent_t begin,end;CU(cudaEventCreate(&begin));CU(cudaEventCreate(&end));
        cudaStream_t stream;CU(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        cudaGraph_t graphs[2];cudaGraphExec_t runs[2];
        for(unsigned version=0;version<2;version++) {
            CU(cudaStreamBeginCapture(stream,cudaStreamCaptureModeGlobal));
            for(unsigned repeat=0;repeat<200;repeat++) {
                if(version&&n>32)shader_setup_compact<<<(n+63)/64,64,0,stream>>>(di,ds,dout,dr,dt,ib,sb,ob,rb,tb,n);
                else shader_setup_compact_small<<<(n+63)/64,64,0,stream>>>(di,ds,dout,dr,dt,ib,sb,ob,rb,tb,n);
            }
            CU(cudaStreamEndCapture(stream,&graphs[version]));
            CU(cudaGraphInstantiate(&runs[version],graphs[version],nullptr,nullptr,0));
            CU(cudaGraphLaunch(runs[version],stream));CU(cudaStreamSynchronize(stream));
        }
        float elapsed[2][5];
        for(unsigned trial=0;trial<5;trial++)for(unsigned order=0;order<2;order++) {
            unsigned version=order^(trial&1u);CU(cudaEventRecord(begin,stream));
            CU(cudaGraphLaunch(runs[version],stream));
            CU(cudaGetLastError());CU(cudaEventRecord(end,stream));CU(cudaEventSynchronize(end));
            CU(cudaEventElapsedTime(&elapsed[version][trial],begin,end));
            CU(cudaMemcpy(output.data(),dout,output.size()*sizeof(output[0]),cudaMemcpyDeviceToHost));
            CU(cudaMemcpy(rects.data(),dr,rects.size()*sizeof(rects[0]),cudaMemcpyDeviceToHost));
            CU(cudaMemcpy(status.data(),dt,status.size()*sizeof(status[0]),cudaMemcpyDeviceToHost));
            if(std::memcmp(output.data(),expected.data(),output.size()*sizeof(output[0]))||
               std::memcmp(rects.data(),expected_rects.data(),rects.size()*sizeof(rects[0]))||
               std::memcmp(status.data(),expected_status.data(),status.size()*sizeof(status[0]))) {
                std::fprintf(stderr,"FAIL timed version=%u n=%u pattern=%u\n",version,n,pattern);std::exit(1);
            }
        }
        for(unsigned version=0;version<2;version++)for(unsigned i=0;i<5;i++)for(unsigned j=i+1;j<5;j++)
            if(elapsed[version][j]<elapsed[version][i]){float t=elapsed[version][i];elapsed[version][i]=elapsed[version][j];elapsed[version][j]=t;}
        std::printf("TIMING n=%u outputs/primitive=%u baseline=%.3f us selected=%.3f us (median of 5 CUDA graphs x 200 launches)\n",
            n,pattern,elapsed[0][2]*5,elapsed[1][2]*5);
        CU(cudaEventDestroy(end));CU(cudaEventDestroy(begin));
        for(unsigned version=0;version<2;version++){CU(cudaGraphExecDestroy(runs[version]));CU(cudaGraphDestroy(graphs[version]));}
        CU(cudaStreamDestroy(stream));
    }
    CU(cudaFree(dt));CU(cudaFree(dr));CU(cudaFree(dout));CU(cudaFree(ds));CU(cudaFree(di));cases++;
}

int main(int argc,char **argv) {
    bool timing=true;
    if(argc==2 && !std::strcmp(argv[1],"--no-timing"))timing=false;
    else if(argc!=1)return 2;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    cudaDeviceProp props;CU(cudaGetDeviceProperties(&props,0));
    std::printf("CUDA device: %s, SM %d.%d; not the Kestrel native launcher\n",props.name,props.major,props.minor);
    const unsigned sizes[]={1,2,7,31,32,33,63,64,65,127,128,129,1023,1024,1025,KSHS_MAX_TRIANGLES};
    for(unsigned n:sizes) {
        for(unsigned pattern=0;pattern<=9;pattern++)check(n,pattern,0,0);
        for(unsigned fault=1;fault<=15;fault++)for(unsigned position:{0u,n/2,n-1})check(n,3,fault,position);
    }
    for(unsigned n=1;n<=KSHS_MAX_TRIANGLES;n++)check(n,9,0,0);
    if(timing)for(unsigned n:{12u,32u,128u,1024u,KSHS_MAX_TRIANGLES})for(unsigned count:{1u,8u})check(n,count,0,0,true);
    std::printf("PASS: %u physical CUDA compaction cases; stable commands/rectangles, global status errors, per-record errors, capacities, partial warps and untouched tails\n",cases);
    return 0;
}
