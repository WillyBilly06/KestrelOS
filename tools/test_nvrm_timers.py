#!/usr/bin/env python3
"""Production RM timer control flow with host threads and mocked callbacks.

No NVIDIA binary, device/codec work, VM or native timer interrupts are executed.
"""
from test_gpu_stable_candidate import ROOT, run_test

code = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32;typedef uint64_t u64,NvU64;typedef int32_t NvS32;
typedef int NV_STATUS;enum{NV_OK=0};
typedef struct {int id;}proc_t;
typedef struct {int rc_timer_enabled,id;}nv_state_t;
typedef struct {int dummy;}nvidia_stack_t;
typedef struct nv_nano_timer nv_nano_timer_t;
static _Thread_local proc_t task;
static _Thread_local bool irq=true,task_context=true;
static volatile LONG allocations,stack_allocations;
static bool fail_alloc,fail_stack;
static u64 now_us=100;
static HANDLE callback_entered,callback_release,cancel_waiting,cancel_done;
static void *kzalloc(size_t n){if(fail_alloc)return NULL;void*p=calloc(1,n);if(p)InterlockedIncrement(&allocations);return p;}
static void kfree(void*p){if(p){InterlockedDecrement(&allocations);free(p);}}
static nvidia_stack_t *nvrm_stack_alloc(void){InterlockedIncrement(&stack_allocations);return fail_stack?NULL:kzalloc(sizeof(nvidia_stack_t));}
static bool irq_save(void){bool old=irq;irq=false;return old;}
static void irq_restore(bool old){irq=old;}
static proc_t *proc_current(void){return &task;}
static bool os_semaphore_may_sleep(void){return irq&&task_context;}
static u64 timer_now_us(void){return now_us;}
static void smp_tlb_poll(void){}
static void panic(const char*s){fprintf(stderr,"%s\n",s);abort();}
static void sched_yield(void){assert(irq);SwitchToThread();}
static SRWLOCK event_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE changed=CONDITION_VARIABLE_INIT;
static int sched_wait_event(u64*seq,u64 expected,u64 deadline){
    assert(irq&&deadline==~0ull);if(cancel_waiting)SetEvent(cancel_waiting);
    AcquireSRWLockExclusive(&event_lock);
    while(__atomic_load_n(seq,__ATOMIC_ACQUIRE)==expected)
        assert(SleepConditionVariableSRW(&changed,&event_lock,5000,0));
    ReleaseSRWLockExclusive(&event_lock);return 1;
}
static void sched_signal_event(u64*seq){
    AcquireSRWLockExclusive(&event_lock);__atomic_fetch_add(seq,1ull,__ATOMIC_RELEASE);
    WakeAllConditionVariable(&changed);ReleaseSRWLockExclusive(&event_lock);
}
static NV_STATUS rm_run_nano_timer_callback(nvidia_stack_t*,nv_state_t*,void*);
static NV_STATUS rm_run_rc_callback(nvidia_stack_t*,nv_state_t*);
'''
code += (ROOT / 'kernel/nvrm_timers.h').read_text()
code += r'''
static unsigned hits[4],rc_hits;
static bool keep_rearming,block_callback;
static NV_STATUS rc_status;
static nv_state_t gpu={.id=1},gpu2={.id=2};
static nv_nano_timer_t *timer;
static NV_STATUS rm_run_nano_timer_callback(nvidia_stack_t*sp,nv_state_t*nv,void*event){
    assert(sp&&nv==&gpu&&irq);unsigned id=(unsigned)(uintptr_t)event;assert(id<4);hits[id]++;
    if(block_callback){
        SetEvent(callback_entered);assert(WaitForSingleObject(callback_release,5000)==WAIT_OBJECT_0);
        // A cancelling thread must retain both this timer and RM's event owner.
        assert(timer->running&&timer->nv==nv&&timer->event==event);
        nv_start_nano_timer(nv,timer,0); // cancellation in flight must suppress rearm
    }
    if(keep_rearming&&id==0)nv_start_nano_timer(nv,timer,0);
    nvrm_os_pump(); // nested pump must be harmless
    return NV_OK;
}
static NV_STATUS rm_run_rc_callback(nvidia_stack_t*sp,nv_state_t*nv){assert(sp&&irq&&(nv==&gpu||nv==&gpu2));rc_hits++;return rc_status;}
static DWORD WINAPI pump(void*unused){(void)unused;nvrm_os_pump();return 0;}
static DWORD WINAPI cancel(void*arg){
    if(arg)nv_destroy_nano_timer(&gpu,timer);else nv_cancel_nano_timer(&gpu,timer);
    SetEvent(cancel_done);return 0;
}
static void overlap(bool destroy){
    nv_create_nano_timer(&gpu,(void*)2,&timer);assert(timer);nv_start_nano_timer(&gpu,timer,0);
    callback_entered=CreateEvent(NULL,TRUE,FALSE,NULL);callback_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    cancel_waiting=CreateEvent(NULL,TRUE,FALSE,NULL);cancel_done=CreateEvent(NULL,TRUE,FALSE,NULL);
    block_callback=true;
    HANDLE a=CreateThread(NULL,0,pump,NULL,0,NULL);assert(a);
    assert(WaitForSingleObject(callback_entered,5000)==WAIT_OBJECT_0);
    HANDLE b=CreateThread(NULL,0,cancel,(void*)(uintptr_t)destroy,0,NULL);assert(b);
    assert(WaitForSingleObject(cancel_waiting,5000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(cancel_done,0)==WAIT_TIMEOUT); // cannot return/free while callback runs
    SetEvent(callback_release);
    assert(WaitForSingleObject(a,5000)==WAIT_OBJECT_0&&WaitForSingleObject(b,5000)==WAIT_OBJECT_0);
    CloseHandle(a);CloseHandle(b);CloseHandle(callback_entered);CloseHandle(callback_release);
    CloseHandle(cancel_waiting);CloseHandle(cancel_done);cancel_waiting=NULL;
    block_callback=false;
    if(!destroy){assert(!timer->active&&!timer->running&&!timer->cancellers);nv_destroy_nano_timer(&gpu,timer);}
    assert(!allocations&&!nvrm_nano_timers&&!nvrm_pumping&&!nvrm_timer_lock);
}
int main(void){
    nvrm_os_pump();assert(!stack_allocations);
    fail_alloc=true;nv_create_nano_timer(&gpu,NULL,&timer);assert(!timer);fail_alloc=false;
    nv_create_nano_timer(&gpu,(void*)1,&timer);assert(timer);
    nv_start_nano_timer(&gpu,timer,999);assert(timer->due_us==101);
    nvrm_os_pump();assert(!hits[1]&&!stack_allocations);now_us=101;
    fail_stack=true;nvrm_os_pump();assert(timer->active&&!timer->running&&!nvrm_pumping);fail_stack=false;
    irq=false;nvrm_os_pump();irq=true;assert(!hits[1]);
    nvrm_os_pump();assert(hits[1]==1&&!timer->active&&!timer->running);
    nv_start_nano_timer(&gpu,timer,~0ull);
    assert(timer->due_us==now_us+(~0ull)/1000u+1u);
    now_us=~0ull-5;nv_start_nano_timer(&gpu,timer,10000);assert(timer->due_us==~0ull);
    nv_cancel_nano_timer(&gpu,timer);now_us=~0ull;nvrm_os_pump();assert(hits[1]==1);
    nv_destroy_nano_timer(&gpu,timer);assert(!allocations&&!nvrm_nano_timers);
    now_us=100;nv_create_nano_timer(&gpu,NULL,&timer);nv_nano_timer_t *other;
    nv_create_nano_timer(&gpu,(void*)3,&other);
    nv_start_nano_timer(&gpu,other,0);nv_start_nano_timer(&gpu,timer,0);
    keep_rearming=true;nvrm_os_pump();keep_rearming=false;
    assert(hits[0]==63&&hits[3]==1); // bounded batch with round-robin fairness
    nv_destroy_nano_timer(&gpu,other);nv_destroy_nano_timer(&gpu,timer);assert(!allocations);
    overlap(false);overlap(true);
    assert(nv_start_rc_timer(&gpu)==0&&nv_start_rc_timer(&gpu)==-1);
    assert(nv_start_rc_timer(&gpu2)==0);now_us+=1000000;
    nvrm_os_pump();assert(rc_hits==2&&gpu.rc_timer_enabled&&gpu2.rc_timer_enabled);
    rc_status=9;now_us+=1000000;nvrm_os_pump();assert(rc_hits==4);
    now_us+=1000000;nvrm_os_pump();assert(rc_hits==4); // failed RC callback never rearmed
    assert(nv_stop_rc_timer(&gpu)==0&&nv_stop_rc_timer(&gpu)==-1&&nv_stop_rc_timer(&gpu2)==0);
    assert(!allocations&&!nvrm_nano_timers&&!nvrm_pumping&&!nvrm_timer_lock&&irq);
    puts("PASS production RM timers: rounded/saturating deadlines, due-only stack allocation, OOM retention, IRQ/nested pump refusal, bounded fair rearm, concurrent cancel/destroy drains callback and blocks rearm, per-device RC success/failure/stop; no RM/GPU/codec execution");
}
'''
if __name__ == '__main__':
    run_test(code, 'nvrm-timers')
