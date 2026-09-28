/* CUDA-backed syscall boundary for the unmodified application shader tests.
 * This tests real sm_120 pixels, NOT Kestrel's native QMD/fence/VM setup. */
#include <cuda_runtime.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <chrono>
#include "../include/kestrel/gpu2d.h"
#include "../include/kestrel/shader_raster.h"

extern "C" __global__ void shader_vm(float *,const sh_instruction_t *,ksh_status_t *,
    const unsigned *,const ksh_texture_t *,unsigned long long,unsigned long long,
    unsigned long long,unsigned long long,unsigned long long,unsigned,unsigned,unsigned,unsigned);
extern "C" __global__ void shader_raster(unsigned *,float *,const unsigned *,float *,
    ksh_status_t *,const kshr_job_t *,unsigned long long,unsigned long long,
    unsigned long long,unsigned long long,unsigned long long,unsigned long long);
extern "C" __global__ void shader_raster_fast(unsigned *,float *,const unsigned *,float *,
    ksh_status_t *,const kshr_job_t *,unsigned long long,unsigned long long,
    unsigned long long,unsigned long long,unsigned long long,unsigned long long);
extern "C" __global__ void gl_raster(unsigned *,float *,const unsigned *,const kg3d_command_t *,
    unsigned long long,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,
    unsigned,unsigned,unsigned,unsigned);

static void check(cudaError_t result) {
    if(result!=cudaSuccess){fprintf(stderr,"CUDA: %s\n",cudaGetErrorString(result));abort();}
}
struct memory {uint64_t handle,bytes;unsigned char *data;unsigned width,height,pitch;};
static memory pool[32];
static uint64_t serial;
static unsigned vertices,rasters,clears,downloads;
static unsigned register_rasters;
static uint64_t uploads,upload_bytes;
static cudaEvent_t start_event,stop_event;
static double kernel_ms[3];
static memory *get(uint64_t handle) {
    for(auto &m:pool)if(handle && m.handle==handle)return &m;
    return nullptr;
}
static void *upload(const void *data,size_t bytes) {
    void *gpu=nullptr;check(cudaMalloc(&gpu,bytes));
    check(cudaMemcpy(gpu,data,bytes,cudaMemcpyHostToDevice));return gpu;
}
static void begin_kernel() {
    if(!start_event){check(cudaEventCreate(&start_event));check(cudaEventCreate(&stop_event));}
    check(cudaEventRecord(start_event));
}
static void retired(unsigned category) {
    check(cudaGetLastError());check(cudaEventRecord(stop_event));check(cudaEventSynchronize(stop_event));
    float ms=0;check(cudaEventElapsedTime(&ms,start_event,stop_event));kernel_ms[category]+=ms;
}
static void release_timers() {
    if(start_event){check(cudaEventDestroy(start_event));check(cudaEventDestroy(stop_event));start_event=stop_event=nullptr;}
}
extern "C" void test_cuda_timing(uint64_t out[3]) {
    for(unsigned i=0;i<3;i++)out[i]=(uint64_t)(kernel_ms[i]*1000+.5);
}
extern "C" void test_cuda_counts(unsigned out[3]) {
    out[0]=vertices;out[1]=rasters;out[2]=clears;
}
extern "C" void test_cuda_uploads(uint64_t out[2]) {
    out[0]=uploads;out[1]=upload_bytes;
}
extern "C" uint64_t test_wall_us(void) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

extern "C" intptr_t test_cuda_request(kg2d_request_t *r) {
    assert(r->version==KG2D_ABI);
    if(r->operation==KG2D_CREATE){
        for(auto &m:pool)if(!m.handle){
            m.handle=++serial;m.width=r->width;m.height=r->height;
            m.pitch=(r->width*4+255)&~255u;m.bytes=(uint64_t)m.pitch*m.height;
            check(cudaMalloc((void**)&m.data,(size_t)m.bytes));check(cudaMemset(m.data,0,(size_t)m.bytes));
            r->handle=m.handle;r->pitch=m.pitch;r->bytes=m.bytes;return 0;
        }return -ENOMEM;
    }
    memory *m=get(r->handle);assert(m);
    if(r->operation==KG2D_DESTROY){check(cudaFree(m->data));*m=memory{};return 0;}
    if(r->operation==KG2D_UPLOAD||r->operation==KG2D_DOWNLOAD){
        assert(r->offset<=m->bytes&&r->bytes<=m->bytes-r->offset);
        if(r->operation==KG2D_UPLOAD){
            check(cudaMemcpy(m->data+r->offset,(void*)(uintptr_t)r->data,(size_t)r->bytes,cudaMemcpyHostToDevice));
            uploads++;upload_bytes+=r->bytes;
        }
        else {check(cudaMemcpy((void*)(uintptr_t)r->data,m->data+r->offset,(size_t)r->bytes,cudaMemcpyDeviceToHost));downloads++;}
        return 0;
    }
    memory *depth=get(r->offset),*texture=get(r->source);
    if(r->operation==KG2D_DRAW3D){
        assert(r->count&&r->count<=KG3D_MAX_COMMANDS);
        const kg3d_command_t *commands=(const kg3d_command_t *)(uintptr_t)r->data;
        // Programmable tests use this for clears; fixed-vertex tests also send
        // real untextured triangles through the same production raster kernel.
        assert(!texture);
        auto *gpu=(kg3d_command_t*)upload(commands,r->count*sizeof(*commands));
        begin_kernel();
        gl_raster<<<dim3((r->width+7)/8,(r->height+7)/8),dim3(8,8)>>>(
            (unsigned*)m->data,depth?(float*)depth->data:nullptr,nullptr,gpu,0,
            m->width,m->height,m->pitch/4,depth?depth->pitch/4:0,0,0,0,r->count,r->x,r->y,r->width,r->height);
        retired(2);check(cudaFree(gpu));clears++;return 0;
    }
    if(r->operation==KG2D_SHADER_VM){
        assert(r->bytes==sizeof(ksh_dispatch_t)&&depth);
        const auto *j=(const ksh_dispatch_t *)(uintptr_t)r->data;
        auto *gpu=(ksh_dispatch_t*)upload(j,sizeof(*j));
        check(cudaMemset(depth->data,0,(size_t)depth->bytes));
        begin_kernel();
        shader_vm<<<(j->lanes+63)/64,64>>>((float*)m->data,gpu->code,(ksh_status_t*)depth->data,
            texture?(unsigned*)texture->data:nullptr,gpu->textures,m->bytes/4,
            j->code_count*sizeof(sh_instruction_t),depth->bytes,texture?texture->bytes:0,
            j->texture_count*sizeof(ksh_texture_t),j->lanes,j->code_count,j->budget,j->texture_count);
        retired(0);check(cudaFree(gpu));vertices++;return 0;
    }
    assert(r->operation==KG2D_SHADER_RASTER&&r->bytes==sizeof(kshr_submission_t));
    const auto *s=(const kshr_submission_t *)(uintptr_t)r->data;const auto *j=&s->job;
    memory *scratch=get(s->scratch),*status=get(s->status);assert(scratch&&status);
    unsigned lanes=kshr_job_lanes(j);
    uint64_t offset=KSHR_JOB_PACKET_OFFSET(j);assert(scratch->bytes>=KSHR_JOB_ALLOCATION_BYTES(j));
    check(cudaMemcpy(scratch->data+offset,j,sizeof(*j),cudaMemcpyHostToDevice));
    check(cudaMemset(status->data,0,(size_t)status->bytes));
    begin_kernel();
    bool fast=kshr_register_fast_eligible(j);
    if(fast){
    shader_raster_fast<<<(lanes+63)/64,64>>>((unsigned*)m->data,depth?(float*)depth->data:nullptr,
        texture?(unsigned*)texture->data:nullptr,(float*)scratch->data,(ksh_status_t*)status->data,
        (kshr_job_t*)(scratch->data+offset),m->bytes,depth?depth->bytes:0,texture?texture->bytes:0,
        kshr_job_scratch_bytes(j),status->bytes,sizeof(*j));
    register_rasters++;
    }else shader_raster<<<(lanes+63)/64,64>>>((unsigned*)m->data,depth?(float*)depth->data:nullptr,
        texture?(unsigned*)texture->data:nullptr,(float*)scratch->data,(ksh_status_t*)status->data,
        (kshr_job_t*)(scratch->data+offset),m->bytes,depth?depth->bytes:0,texture?texture->bytes:0,
        kshr_job_scratch_bytes(j),status->bytes,sizeof(*j));
    retired(1);rasters++;return 0;
}

extern "C" void test_cuda_finished(void) {
    release_timers();
    for(const auto &m:pool)assert(!m.handle);
    assert(vertices==13&&rasters>=12&&clears==12&&downloads>12);
    printf("PASS actual application compiler/API -> CUDA vertex VM -> CUDA fragment raster -> pixel readback: %u vertex / %u raster / %u clear launches; %u downloads; no CPU shader/pixel fallback; all GPU buffers released\n",
           vertices,rasters,clears,downloads);
}

extern "C" void test_cuda_fixed_finished(void) {
    release_timers();
    for(const auto &m:pool)assert(!m.handle);
    assert(vertices==131 && clears==6 && rasters==0 && downloads>131);
    printf("PASS real sm_120 fixed-function vertex transforms/lighting and triangle pixels: %u vertex launches, %u 3D launches, %u downloads; all GPU buffers released (CUDA, not native QMD proof)\n",
        vertices,clears,downloads);
}

extern "C" void test_cuda_failed(int quarantine) {
    release_timers();
    unsigned live=0;for(const auto &m:pool)live+=m.handle!=0;
    // Unfenced transport storage is deliberately retained until process exit.
    assert(quarantine ? live==2 : live==0);
    assert(vertices<=1&&rasters<=1&&clears<=1);
}

extern "C" void test_cuda_scene_finished(void) {
    release_timers();
    for(const auto &m:pool)assert(!m.handle);
    // Every tested scene has one object draw and one ground draw. Array
    // batching now fits each into one dispatch; a minimum of 100 rewarded
    // the former transport overhead rather than checking rendered work.
    assert(vertices==2*clears&&rasters>100&&clears>=12);
    printf("PASS desktop scene CUDA launches: %u vertex / %u fragment / %u clear; %u downloads; all GPU buffers released\n",
        vertices,rasters,clears,downloads);
    printf("Register-resident fragment batches: %u/%u\n",register_rasters,rasters);
}
