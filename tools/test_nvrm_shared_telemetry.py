"""Host scaffold for actual RUSD helpers and telemetry worker.

RM allocation/mapping and scheduler time are mocked. Default is compile-only;
--run executes assertions, not hardware/codec workloads. Official ABI headers
are used. Neither mode proves the GPU's actual shared-memory polling behavior.
"""
import argparse
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
OFFICIAL = ROOT / 'out/nvidia-open-595.99.02/src/common'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    src = (ROOT / 'kernel/nvrm_os.c').read_text()
    telemetry = (ROOT / 'kernel/nv_telemetry.c').read_text()
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "nvtypes.h"
#include "nvstatus.h"
#include "class/cl00de.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
typedef uint32_t u32; typedef uint64_t u64;
#define kinfo(...) ((void)0)
'''
    prefix += '#include "' + (ROOT / 'kernel/nvrm_telemetry.h').as_posix() + '"\n'
    c = prefix + r'''
static bool nvrm_adapter_ready;
static unsigned allocs,maps,controls;
static u32 alloc_status,map_status,control_status,capacity_kib;
static bool map_null;
static NV00DE_SHARED_DATA mock_shared;
static u32 nvrm_host_control_object(u32 client,u32 object,u32 cmd,void *p,size_t bytes) {
    assert(client && object && cmd==NV2080_CTRL_CMD_FB_GET_INFO_V2);
    assert(bytes==sizeof(NV2080_CTRL_FB_GET_INFO_V2_PARAMS));controls++;
    NV2080_CTRL_FB_GET_INFO_V2_PARAMS *v=p;
    assert(v->fbInfoListSize==1 && v->fbInfoList[0].index==NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE);
    v->fbInfoList[0].data=capacity_kib;return control_status;
}
static u32 nvrm_host_alloc_object(u32 client,u32 parent,u32 handle,u32 cls,void *p,size_t bytes) {
    assert(client && parent && handle>=0x006e0000u && handle<0x006e0002u);
    assert(cls==RM_USER_SHARED_DATA && bytes==sizeof(NV00DE_ALLOC_PARAMETERS));
    assert(((NV00DE_ALLOC_PARAMETERS *)p)->polledDataMask==
           (NV00DE_RUSD_POLL_PERF|NV00DE_RUSD_POLL_THERMAL));allocs++;return alloc_status;
}
static u32 nvrm_host_map_memory(u32 client,u32 device,u32 handle,u64 offset,u64 bytes,void **address,u32 flags) {
    assert(client && device && handle>=0x006e0000u && handle<0x006e0002u);
    assert(!offset && bytes==sizeof(NV00DE_SHARED_DATA) && flags==1);maps++;
    if(!map_status && !map_null)*address=&mock_shared;return map_status;
}
/* Simulate a firmware write during the actual snapshot copy, never a thread. */
static unsigned mutate_copies,copy_calls;
static void *snapshot_memcpy(void *dst,const void *source,size_t bytes) {
    copy_calls++;void *result=memcpy(dst,source,bytes);
    if(mutate_copies){mutate_copies--;(*(volatile NvU64 *)(uintptr_t)source)++;}
    return result;
}
'''
    start = src.index('typedef struct {', src.index('/* Official RM_USER_SHARED_DATA:'))
    end = src.index('NvU32 nvrm_gpu_id', start)
    c += '#define memcpy snapshot_memcpy\n' + src[start:end] + '\n#undef memcpy\n'
    c += r'''
static void reset(void) {
    memset(nvrm_shared_state,0,sizeof nvrm_shared_state);
    memset(&mock_shared,0,sizeof mock_shared);
    allocs=maps=controls=alloc_status=map_status=control_status=copy_calls=mutate_copies=0;
    capacity_kib=16u*1024u*1024u;map_null=false;nvrm_adapter_ready=true;
}
int main(void) {
    nvrm_shared_telemetry_t out;
    reset();nvrm_adapter_ready=false;
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==NV_ERR_INVALID_STATE);
    assert(!out.vram_bytes && !out.timestamp && out.utilization[0]==UINT32_MAX);
    assert(nvrm_host_read_shared_telemetry(1,2,3,NULL)==NV_ERR_INVALID_ARGUMENT);
    assert(!allocs && !maps && !controls);
    reset();alloc_status=NV_ERR_NOT_SUPPORTED;
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==alloc_status);
    assert(out.vram_bytes==16ull*1024*1024*1024 && allocs==1 && !maps && controls==1);
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==alloc_status);
    assert(allocs==1 && controls==1); /* failed boot-lifetime handle is not retried */
    reset();map_null=true;
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==NV_ERR_INVALID_STATE);
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==NV_ERR_INVALID_STATE);
    assert(allocs==1 && maps==1);
    reset();map_status=NV_ERR_GENERIC;
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==map_status);
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==map_status && maps==1);
    reset();capacity_kib=1;
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==NV_OK && !out.vram_bytes);
    reset();mock_shared.perfDevUtil.lastModifiedTimestamp=100;
    assert(nvrm_host_peek_shared_telemetry(1,2,3,&out)==NV_ERR_INVALID_STATE);
    assert(!allocs && !maps && !controls && !out.timestamp);
    assert(nvrm_host_peek_shared_telemetry(1,2,3,NULL)==NV_ERR_INVALID_ARGUMENT);
    mock_shared.perfDevUtil.info.gpuPercentBusy=13;
    mock_shared.perfDevUtil.info.engUtil[0]=(RUSD_ENG_UTILIZATION){27,10000};
    mock_shared.perfDevUtil.info.engUtil[1]=(RUSD_ENG_UTILIZATION){99,0};
    mock_shared.temperatures[0].lastModifiedTimestamp=123;
    mock_shared.temperatures[0].temperature=(NvTemp)(42*256);
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==NV_OK);
    assert(out.timestamp==100 && out.utilization[0]==1300 && out.utilization[1]==2700);
    assert(out.utilization[2]==UINT32_MAX && out.temperature_c==42 && out.temperature_timestamp==123);
    assert(nvrm_host_peek_shared_telemetry(1,2,3,&out)==NV_OK);
    assert(out.timestamp==100 && out.utilization[0]==1300 && out.temperature_c==42);
    assert(nvrm_host_peek_shared_telemetry(1,2,99,&out)==NV_ERR_INVALID_STATE);
    assert(!out.timestamp && out.utilization[0]==UINT32_MAX);
    assert(allocs==1 && maps==1 && controls==1); /* peek never invokes RM */
    mock_shared.perfDevUtil.info.gpuPercentBusy=101;
    mock_shared.perfDevUtil.info.engUtil[0].clkPercentBusy=UINT32_MAX;
    assert(nvrm_host_read_shared_telemetry(1,2,3,&out)==NV_OK);
    assert(out.utilization[0]==UINT32_MAX && out.utilization[1]==UINT32_MAX);
    assert(allocs==1 && maps==1 && controls==1);
    assert(nvrm_host_read_shared_telemetry(4,5,6,&out)==NV_OK);
    assert(nvrm_host_read_shared_telemetry(7,8,9,&out)==NV_ERR_INSUFFICIENT_RESOURCES);
    RUSD_PERF_DEVICE_UTILIZATION sample={0},copy;
    memset(&copy,0xa5,sizeof copy);assert(!nvrm_shared_copy(&sample,&copy,sizeof copy));
    for(size_t i=0;i<sizeof copy;i++)assert(((unsigned char *)&copy)[i]==0);
    sample.lastModifiedTimestamp=UINT64_MAX;assert(!nvrm_shared_copy(&sample,&copy,sizeof copy));
    sample.lastModifiedTimestamp=RUSD_SEQ_START;assert(!nvrm_shared_copy(&sample,&copy,sizeof copy));
    sample.lastModifiedTimestamp=100;sample.info.gpuPercentBusy=44;mutate_copies=1;copy_calls=0;
    assert(nvrm_shared_copy(&sample,&copy,sizeof copy) && copy_calls==2);
    assert(copy.lastModifiedTimestamp==101 && copy.info.gpuPercentBusy==44);
    mutate_copies=10;copy_calls=0;
    assert(!nvrm_shared_copy(&sample,&copy,sizeof copy) && copy_calls==10);
    for(size_t i=0;i<sizeof copy;i++)assert(((unsigned char *)&copy)[i]==0);
    puts("PASS RUSD helpers: one-time guarded setup, RM-free tuple-bound snapshots, timestamp consistency and invalid data refusal (mock firmware)");
    return 0;
}
'''
    includes = [OFFICIAL / 'inc', OFFICIAL / 'sdk/nvidia/inc']
    run_test(c, 'nvrm_shared_telemetry', includes, compile_only=not args.run)

    # Actual worker, with deterministic mocked samples and sleeping scheduler.
    c = prefix + r'''
enum {NV_ENGINE_3D,NV_ENGINE_COPY,NV_ENGINE_ENCODE,NV_ENGINE_DECODE,NV_ENGINE_COUNT};
#define NV_MAX_CARDS 2
#define NV_ENGINE_UNMEASURED (-1)
typedef struct {u64 vram_bytes;bool vram_exact;} nv_card_t;
static u64 g_uptime_ms;
static jmp_buf worker_exit;
static unsigned steps,step_limit,scenario,gpumon_calls,render_attempts,setups;
static bool render_owned,mapped;
static bool nv_render_try_begin(void){
    assert(!render_owned);render_attempts++;
    if(scenario==3 || (scenario==4 && mapped))return false;
    render_owned=true;return true;
}
static void nv_render_end(void){assert(render_owned);render_owned=false;}
static bool irq_save(void){return true;}
static void irq_restore(bool irq){(void)irq;}
static void sched_sleep_ms(u64 ms){
    if(ms==250 && ++steps>step_limit)longjmp(worker_exit,1);
    g_uptime_ms+=ms;
}
static u32 sample_shared(u32 client,u32 device,u32 subdevice,nvrm_shared_telemetry_t *out){
    assert(client==1 && device==2 && subdevice==3);memset(out,0,sizeof *out);
    out->timestamp=(scenario==1 || scenario==3 || scenario==4)?100+steps:100;
    if(scenario==2 && steps>=9)out->timestamp=100+steps;
    out->utilization[0]=1300;out->utilization[1]=0;out->utilization[2]=UINT32_MAX;
    out->temperature_timestamp=100+steps;out->temperature_c=42;out->vram_bytes=16ull<<30;
    return NV_OK;
}
u32 nvrm_host_read_shared_telemetry(u32 client,u32 device,u32 subdevice,nvrm_shared_telemetry_t *out){
    assert(render_owned);setups++;mapped=true;return sample_shared(client,device,subdevice,out);
}
u32 nvrm_host_peek_shared_telemetry(u32 client,u32 device,u32 subdevice,nvrm_shared_telemetry_t *out){
    assert(!render_owned);
    if(!mapped){memset(out,0,sizeof *out);return NV_ERR_INVALID_STATE;}
    return sample_shared(client,device,subdevice,out);
}
u32 nvrm_host_read_utilization(u32 client,u32 subdevice,u32 util[3],u64 *timestamp){
    assert(render_owned);
    assert(client==1 && subdevice==3);gpumon_calls++;
    *timestamp=500+steps;util[0]=2400;util[1]=100;util[2]=300;return NV_OK;
}
'''
    start = telemetry.index('typedef struct {', telemetry.index('/* Firmware owns modern engine counters.'))
    end = telemetry.index('void nv_telemetry_bind_host', start)
    c += telemetry[start:end]
    c += r'''
static host_history_t run_worker(unsigned which,unsigned count){
    static nv_card_t card;
    memset(host_history,0,sizeof host_history);memset(&card,0,sizeof card);
    host_history_t *h=&host_history[0];h->card=&card;h->client=1;h->device=2;h->subdevice=3;h->bound=true;
    for(unsigned e=0;e<NV_ENGINE_COUNT;e++)h->percent[e]=NV_ENGINE_UNMEASURED;
    steps=gpumon_calls=render_attempts=setups=0;g_uptime_ms=0;scenario=which;step_limit=count;
    render_owned=false;mapped=which!=4;
    if(!setjmp(worker_exit))telemetry_worker(h);
    assert(card.vram_exact && card.vram_bytes==(16ull<<30));return *h;
}
int main(void){
    host_history_t h=run_worker(1,1);assert(h.source==1 && !h.have_fresh);
    h=run_worker(1,3);assert(h.source==1 && h.have_fresh && h.percent[NV_ENGINE_3D]==13);
    assert(h.percent[NV_ENGINE_COPY]==NV_ENGINE_UNMEASURED && h.percent[NV_ENGINE_DECODE]==NV_ENGINE_UNMEASURED);
    h=run_worker(0,8);assert(h.source==2 && h.have_fresh && gpumon_calls>=3);
    assert(h.percent[NV_ENGINE_3D]==24 && h.percent[NV_ENGINE_ENCODE]==1 && h.percent[NV_ENGINE_DECODE]==3);
    h=run_worker(2,9);assert(h.source==1 && !h.have_fresh); /* resumed source must establish baseline */
    h=run_worker(2,10);assert(h.source==1 && h.have_fresh && h.percent[NV_ENGINE_3D]==13);
    /* Renderer stays busy for five seconds. Fresh mapped telemetry must keep
     * advancing without attempting to take its lock even once. */
    h=run_worker(3,20);assert(h.have_fresh && h.percent[NV_ENGINE_3D]==13);
    assert(h.changed_ms==5000 && !render_attempts && !setups && !gpumon_calls);
    /* Cold start does guarded setup once; the renderer then owns the lock. */
    h=run_worker(4,20);assert(h.have_fresh && h.changed_ms==5000);
    assert(setups==1 && render_attempts==1 && !render_owned && !gpumon_calls);
    puts("PASS telemetry worker: fresh RUSD under continuous render contention, guarded setup/fallback, independent freshness and source changes (serial mocks)");
    return 0;
}
'''
    run_test(c, 'nvrm_telemetry_worker', includes, compile_only=not args.run)


if __name__ == '__main__':
    main()
