"""Production sampler must not turn a missing/stale GPU reading into 0%."""
from test_gpu_stable_candidate import ROOT, function, run_test
source=(ROOT/'kernel/nv_telemetry.c').read_text()
header=(ROOT/'kernel/nv.h').read_text()
constants='\n'.join(line for line in header.splitlines() if any(line.startswith('#define '+key+' ') for key in [
    'NV_PMC_ENABLE','NV_PMC_ENABLE_PGRAPH','NV_PGRAPH_STATUS','NV_PGRAPH_STATUS_BUSY','NV_MAX_CARDS',
    'NV_ENGINE_ABSENT','NV_ENGINE_UNMEASURED']))
types=source[source.index('#define WINDOW'):source.index('static void telemetry_worker(')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
enum {NV_ENGINE_3D,NV_ENGINE_COPY,NV_ENGINE_ENCODE,NV_ENGINE_DECODE,NV_ENGINE_COUNT};
typedef struct {int index;u32 engines_present,chipset;int temperature_c,fan_percent;u64 vram_bytes,vram_allocated;} nv_card_t;
typedef struct {int engine_percent[4];u32 samples;bool sampled;int temperature_c,fan_percent;u64 vram_bytes,vram_used;} nv_telemetry_t;
static u64 g_uptime_ms;static u32 status,enabled;
'''+constants+'\n'+types+r'''
static void telemetry_worker(void *arg){(void)arg;}
static int kthread_create(const char *name,void (*fn)(void *),void *arg){
    (void)name;(void)fn;(void)arg;return 1;
}
static u32 nv_rd32(nv_card_t *c,u32 address){(void)c;return address==NV_PGRAPH_STATUS?status:enabled;}
'''
for name in ['nv_telemetry_sample','nv_telemetry_probe_engines','nv_telemetry_read']:code+=function(source,name)
code+=r'''
int main(void){
    nv_card_t c={.index=0,.engines_present=15,.chipset=0x1b0};nv_telemetry_t r;
    nv_telemetry_read(&c,&r);assert(!r.sampled && r.engine_percent[0]==-2);
    status=NV_PGRAPH_STATUS_BUSY;nv_telemetry_sample(&c);nv_telemetry_read(&c,&r);
    assert(r.sampled && r.engine_percent[0]==100 && r.engine_percent[1]==-2);
    status=0;nv_telemetry_sample(&c);nv_telemetry_read(&c,&r);assert(r.engine_percent[0]==50);
    status=0xffffffff;nv_telemetry_sample(&c);nv_telemetry_read(&c,&r);assert(!r.sampled&&r.engine_percent[0]==-2);
    status=0xbadf1001;nv_telemetry_sample(&c);nv_telemetry_read(&c,&r);assert(!r.sampled&&r.engine_percent[0]==-2);
    status=NV_PGRAPH_STATUS_BUSY;nv_telemetry_sample(&c);nv_telemetry_read(&c,&r);
    assert(r.samples==3 && r.engine_percent[0]==67); // invalid reads excluded, never idle
    g_uptime_ms=1001;nv_telemetry_read(&c,&r);assert(!r.sampled&&r.engine_percent[0]==-2);
    enabled=NV_PMC_ENABLE_PGRAPH;nv_telemetry_probe_engines(&c);nv_telemetry_read(&c,&r);
    assert(!r.sampled && !history[0].taken); // replacement cannot inherit samples
    status=0;nv_telemetry_sample(&c);nv_telemetry_read(&c,&r);assert(r.sampled&&r.engine_percent[0]==0);
    host_history_t *host=&host_history[0];
    host->bound=true;host->have_fresh=true;host->changed_ms=1001;
    for(int i=0;i<NV_ENGINE_COUNT;i++)host->percent[i]=NV_ENGINE_UNMEASURED;
    host->percent[NV_ENGINE_3D]=13;
    host->temperature_c=42;host->temperature_fresh=true;host->temperature_changed_ms=1001;
    nv_telemetry_read(&c,&r);assert(r.sampled&&r.engine_percent[0]==13&&r.temperature_c==42);
    assert(r.engine_percent[NV_ENGINE_COPY]==NV_ENGINE_UNMEASURED);
    g_uptime_ms=2001;nv_telemetry_read(&c,&r);assert(r.sampled);
    g_uptime_ms=2002;nv_telemetry_read(&c,&r);
    assert(!r.sampled&&r.engine_percent[0]==NV_ENGINE_UNMEASURED&&r.temperature_c==-1000);
    puts("GPU telemetry PASS: unavailable startup, measured busy/idle, lost/blocked registers, valid-only denominator, stale windows, device reset");
}
'''
if __name__=='__main__':run_test(code,'gpu-telemetry-validity')
