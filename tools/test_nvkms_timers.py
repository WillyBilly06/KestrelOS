#!/usr/bin/env python3
"""NVKMS timer lifecycle from the native port; no display/GPU work executed."""
from test_gpu_stable_candidate import ROOT, run_test

port = (ROOT / 'kernel/nvkms_port.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;typedef uint32_t NvU32;typedef uint64_t NvU64;typedef bool NvBool;
#define NV_TRUE true
#define NV_FALSE false
typedef void nvkms_timer_proc_t(void*,NvU32);
typedef struct nvkms_timer_t nvkms_timer_handle_t;
static NvU64 now_us;
static bool core_locked,irq_enabled=true,fail_alloc;
static unsigned allocs,frees,calls;
static void(*acquire_hook)(void),(*release_hook)(void);
static void*last_freed;
static void*kzalloc(size_t n){if(fail_alloc)return NULL;allocs++;return calloc(1,n);}
static void kfree(void*p){assert(p);last_freed=p;frees++;free(p);}
static NvU64 timer_now_us(void){return now_us;}
static bool irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void irq_restore(bool old){if(old)irq_enabled=true;}
static void nvkms_core_lock_acquire(void){
    assert(!core_locked);core_locked=true;
    if(acquire_hook){void(*fn)(void)=acquire_hook;acquire_hook=NULL;fn();}
}
static void nvkms_core_lock_release(void){
    assert(core_locked);
    // Model another core obtaining the semaphore immediately upon release.
    // The hook retains the core lock while acting as that next owner.
    if(release_hook){void(*fn)(void)=release_hook;release_hook=NULL;fn();}
    core_locked=false;
}
'''
start=port.index('struct nvkms_ref_ptr {')
end=port.index('static void nvkms_timer_thread',start)
code += port[start:end]
code += r'''
static nvkms_timer_handle_t*owned;
static unsigned freed_before;
static void cb(void*p,NvU32 v){assert(core_locked);assert(v==17);assert(p==NULL||p==(void*)0x1234);calls++;}
static void self_cancel(void*p,NvU32 v){cb(p,v);nvkms_free_timer(owned);assert(frees==freed_before);}
static void next_owner_free(void){
    // A completed non-auto timer is now owned solely by its caller. The worker
    // must not read it again after this point.
    assert(owned->complete);nvkms_free_timer(owned);owned=NULL;
}
static void cancel_selected(void){assert(owned&&!owned->complete);nvkms_free_timer(owned);}
static unsigned order_count,order[3];
static nvkms_timer_handle_t*ordered[3];
static void ordered_cb(void*p,NvU32 v){
    (void)p;assert(core_locked&&v<3&&order_count<3);order[order_count++]=v;
    if(v==0)ordered[2]=nvkms_alloc_timer(ordered_cb,NULL,2,0);
    nvkms_free_timer(ordered[v]);
}
int main(void){
    now_us=100;owned=nvkms_alloc_timer(cb,NULL,17,10);assert(owned);
    nvkms_host_run_timers();assert(!calls&&!frees);
    now_us=110;release_hook=next_owner_free;nvkms_host_run_timers();
    assert(calls==1&&frees==1&&!timers&&!owned&&!core_locked);

    owned=nvkms_alloc_timer(self_cancel,(void*)0x1234,17,0);freed_before=frees;
    nvkms_host_run_timers();assert(calls==2&&frees==freed_before+1&&!timers);
    owned=nvkms_alloc_timer(cb,NULL,17,999);core_locked=true;nvkms_free_timer(owned);core_locked=false;
    nvkms_host_run_timers();assert(calls==2&&!timers);
    owned=nvkms_alloc_timer(cb,NULL,17,0);acquire_hook=cancel_selected;
    nvkms_host_run_timers();assert(calls==2&&!timers);

    struct nvkms_ref_ptr*r=nvkms_alloc_ref_ptr((void*)0x1234);
    assert(r&&nvkms_alloc_timer_with_ref_ptr(cb,r,17,0)&&r->refs==2);
    nvkms_host_run_timers();assert(calls==3&&r->refs==1&&!timers);
    core_locked=true;nvkms_free_ref_ptr(r);core_locked=false;
    r=nvkms_alloc_ref_ptr((void*)0x1234);
    assert(nvkms_alloc_timer_with_ref_ptr(cb,r,17,0));
    core_locked=true;nvkms_free_ref_ptr(r);core_locked=false;
    nvkms_host_run_timers();assert(calls==3&&!timers); // dead target suppressed
    r=nvkms_alloc_ref_ptr((void*)0x1234);fail_alloc=true;
    assert(!nvkms_alloc_timer_with_ref_ptr(cb,r,17,0)&&r->refs==1);fail_alloc=false;
    core_locked=true;nvkms_free_ref_ptr(r);core_locked=false;

    now_us=UINT64_MAX-5;owned=nvkms_alloc_timer(cb,NULL,17,10);
    assert(owned&&owned->due_us==UINT64_MAX);
    nvkms_host_run_timers();assert(calls==3);
    now_us=UINT64_MAX;release_hook=next_owner_free;nvkms_host_run_timers();assert(calls==4);
    now_us=1000;ordered[0]=nvkms_alloc_timer(ordered_cb,NULL,0,0);
    ordered[1]=nvkms_alloc_timer(ordered_cb,NULL,1,0);nvkms_host_run_timers();
    assert(order_count==3&&order[0]==0&&order[1]==1&&order[2]==2);
    assert(allocs==frees&&!timers&&irq_enabled&&!core_locked);
    puts("PASS native NVKMS timer lifecycle: deadline/FIFO order, callback-generated work, NULL argument, self-cancel, selected/pending cancel, completion/free inside core lock, ref target destruction, OOM rollback and no leaked handles; clock/locking/hardware edges mocked");
}
'''
if __name__ == '__main__':
    run_test(code,'nvkms-timers')
