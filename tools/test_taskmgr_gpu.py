"""Per-adapter sampling and responsive graph-layout tests (modeled devices)."""
from test_gpu_stable_candidate import ROOT, function, run_test

source=(ROOT/'user/desktop/app_taskmgr.c').read_text()
layout=(ROOT/'user/desktop/taskmgr_gpu.h').read_text()
abi=(ROOT/'include/kestrel/syscall.h').read_text()
end=abi.index('} kgpuinfo_t;')+len('} kgpuinfo_t;')
gpu_type=abi[abi.rfind('typedef struct {',0,end):end]
start=source.rfind('typedef struct {',0,source.index('} series_t;'))
types=source[start:source.index('} gpu_history_t;')+len('} gpu_history_t;')]
layout_type=layout[layout.index('typedef struct {'):layout.index('} taskmgr_gpu_layout_t;')+len('} taskmgr_gpu_layout_t;')]
code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define HISTORY 120
typedef struct {int x,y,w,h;} rect_t;
static rect_t rect_make(int x,int y,int w,int h){return (rect_t){x,y,w,h};}
'''+gpu_type+types+layout_type+r'''
typedef struct {
    gpu_history_t gpus[TASKMGR_MAX_GPUS];
    unsigned gpu_count,selected_gpu,gpu_order[TASKMGR_MAX_GPUS];
    bool have_gpu;kgpuinfo_t gpu;
} taskmgr_t;
static kgpuinfo_t devices[8];static unsigned device_count;
static int enum_gpu(unsigned i,kgpuinfo_t *out){if(i>=device_count)return -1;*out=devices[i];return 0;}
'''
for name in ['series_push','gpu_series_push','same_gpu','sample_gpus']:
    code+=function(source,name)
# function() deliberately supports only a small return-type list.
code+=function(layout.replace('static taskmgr_gpu_layout_t taskmgr_gpu_layout', 'static rect_t taskmgr_gpu_layout'),
               'taskmgr_gpu_layout').replace('static rect_t taskmgr_gpu_layout','static taskmgr_gpu_layout_t taskmgr_gpu_layout')
code+=r'''
static bool disjoint(rect_t a,rect_t b){return a.x+a.w<=b.x||b.x+b.w<=a.x||a.y+a.h<=b.y||b.y+b.h<=a.y;}
int main(void){
    static taskmgr_t t;device_count=2;
    devices[0]=(kgpuinfo_t){.pci_vendor=0x8086,.slot=2,.temperature_c=-1000};
    devices[1]=(kgpuinfo_t){.pci_vendor=0x10de,.slot=1,.boot_display=1,.engines_sampled=1,
        .engine_percent={30,0,-2,7},.vram_exact=1,.vram_bytes=16ull<<30,.vram_used=1ull<<30};
    memcpy(devices[1].driver_version,"test",5);
    sample_gpus(&t);assert(t.gpu_count==2 && t.selected_gpu==1 && t.gpu_order[0]==1 && t.gpu_order[1]==0);
    assert(!t.gpus[0].engine_valid[0] && t.gpus[0].engine[0].v[0]<0);
    assert(t.gpus[1].engine_valid[0] && t.gpus[1].engine_valid[1] && !t.gpus[1].engine_valid[2]);
    assert(t.gpus[1].memory_valid && !t.gpus[0].memory_valid);
    t.selected_gpu=0;devices[1].engine_percent[0]=50;sample_gpus(&t);
    assert(t.selected_gpu==0 && t.gpu.pci_vendor==0x8086 && t.gpus[1].engine[0].count==2);
    assert(t.gpus[1].engine[0].v[0]==0.3f && t.gpus[1].engine[0].v[1]==0.5f);
    devices[1].engine_percent[0]=101;sample_gpus(&t);assert(!t.gpus[1].engine_valid[0] && t.gpus[1].engine[0].v[2]<0);
    devices[1].engine_percent[0]=-2;devices[1].vram_used=17ull<<30;sample_gpus(&t);assert(!t.gpus[1].memory_valid);
    devices[1].pci_device=42;sample_gpus(&t);assert(t.gpus[1].engine[0].count==1); // replacement cannot inherit history
    device_count=1;t.selected_gpu=1;sample_gpus(&t);assert(t.selected_gpu==0 && !t.gpus[1].engine[0].count);
    device_count=0;sample_gpus(&t);assert(!t.have_gpu && !t.gpu_count);
    unsigned cases=0;
    for(int sc=1;sc<=4;sc++)for(int width=200*sc;width<=2000*sc;width+=17*sc)
    for(int h=200*sc;h<=1800*sc;h+=31*sc){
        taskmgr_gpu_layout_t l=taskmgr_gpu_layout(27,43,width,h,sc,20*sc);
        for(int i=0;i<4;i++){
            rect_t r=l.engine[i];assert(r.w>0&&r.h>0&&r.x>=27&&r.x+r.w<=27+width);
            assert(r.y+r.h<l.dedicated.y);
            for(int j=0;j<i;j++)assert(disjoint(r,l.engine[j]));
        }
        assert(l.dedicated.y+l.dedicated.h<l.shared.y && l.shared.y+l.shared.h<l.metrics_y);
        assert(l.columns==(width>=480*sc?2:1));cases++;
    }
    printf("Task Manager GPU PASS: two independent adapters, boot ordering, retained selection/history, missing/invalid counters, replacement/removal, %u nonoverlapping responsive layouts\n",cases);
}
'''
if __name__=='__main__':run_test(code,'taskmgr-gpu')
