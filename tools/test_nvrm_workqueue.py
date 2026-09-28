#!/usr/bin/env python3
"""Execute RM queue production code with host scheduler/worker-launch edges.

No RM binary, codec, GPU or native AP context switching is executed. Mock
callbacks exercise the actual queue's item ownership and flush barriers.
"""
from test_gpu_stable_candidate import ROOT, run_test

code = r'''
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32;typedef uint64_t u64;typedef int NV_STATUS,NvBool;
enum{NV_OK=0,NV_FALSE=0,NV_TRUE=1,NV_ERR_NO_MEMORY=2,NV_ERR_NOT_READY=3,
     NV_ERR_OPERATING_SYSTEM=4,NV_ERR_INVALID_STATE=5,NV_ERR_ILLEGAL_ACTION=6};
typedef struct {int id;}proc_t;
typedef struct {int dummy;}nvidia_stack_t;
struct os_work_queue{int id;};
static nvidia_stack_t stack;
static _Thread_local proc_t task;
static _Thread_local bool irq=true,sleep_allowed=true;
static volatile LONG allocs,creates,called;
static bool fail_alloc,fail_launch,block_launch;
static HANDLE launch_entered,launch_release;
static void *kzalloc(size_t n){if(fail_alloc)return NULL;void*p=calloc(1,n);if(p)InterlockedIncrement(&allocs);return p;}
static void kfree(void*p){if(p){InterlockedDecrement(&allocs);free(p);}}
static proc_t *proc_current(void){return &task;}
static bool irq_save(void){bool old=irq;irq=false;return old;}
static void irq_restore(bool old){irq=old;}
static void smp_tlb_poll(void){}
static void panic(const char*s){fprintf(stderr,"%s\n",s);abort();}
static bool os_semaphore_may_sleep(void){return irq&&sleep_allowed;}
static nvidia_stack_t *nvrm_stack_alloc(void){return &stack;}
static int kthread_create(const char*name,void(*fn)(void*),void*arg){
    (void)name;(void)fn;(void)arg;assert(irq&&sleep_allowed);InterlockedIncrement(&creates);
    if(block_launch){SetEvent(launch_entered);assert(WaitForSingleObject(launch_release,5000)==WAIT_OBJECT_0);}
    return fail_launch?-1:100;
}
static SRWLOCK event_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE event_changed=CONDITION_VARIABLE_INIT;
static void(*wait_hook)(void);
static void sched_yield(void){assert(irq);SwitchToThread();}
static void sched_sleep_ms(u64 n){Sleep((DWORD)n);}
static int sched_wait_event(u64*seq,u64 expected,u64 deadline){
    assert(irq&&deadline==~0ull);
    if(wait_hook){void(*fn)(void)=wait_hook;wait_hook=NULL;fn();}
    AcquireSRWLockExclusive(&event_lock);
    while(__atomic_load_n(seq,__ATOMIC_ACQUIRE)==expected)
        assert(SleepConditionVariableSRW(&event_changed,&event_lock,5000,0));
    ReleaseSRWLockExclusive(&event_lock);return 1;
}
static void sched_signal_event(u64*seq){
    AcquireSRWLockExclusive(&event_lock);__atomic_fetch_add(seq,1ull,__ATOMIC_RELEASE);
    WakeAllConditionVariable(&event_changed);ReleaseSRWLockExclusive(&event_lock);
}
static void rm_execute_work_item(nvidia_stack_t*,void*);
'''
code += (ROOT / 'kernel/nvrm_workqueue.h').read_text()
code += r'''
static struct os_work_queue key_a={1},key_b={2},key_c={3};
static bool requeue,expect_unload,self_flush;
static unsigned deliveries[1600];
static void rm_execute_work_item(nvidia_stack_t*sp,void*data){
    assert(sp==&stack&&irq); // this thread released its IRQ-disabled lock; another CPU may own it
    unsigned id=(unsigned)(uintptr_t)data;assert(id<1600);
    assert(__atomic_fetch_add(&deliveries[id],1u,__ATOMIC_RELAXED)==0);
    InterlockedIncrement(&called);
    if(expect_unload)assert(os_is_queue_flush_ongoing(&key_a));
    if(self_flush)assert(os_flush_work_queue(&key_a,NV_FALSE)==NV_ERR_ILLEGAL_ACTION);
    if(requeue){requeue=false;assert(os_queue_work_item(&key_a,(void*)2)==NV_OK);}
}
static nvrm_work_queue_t *qa,*qb;
static unsigned passes;
static void pump_a(void){passes++;assert(nvrm_work_process_one(qa,&stack));wait_hook=pump_a;}
static DWORD WINAPI start_a(void*arg){
    NV_STATUS *result=arg;*result=os_queue_work_item(&key_a,(void*)7);return 0;
}
static DWORD WINAPI publish(void*arg){
    unsigned base=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<200;i++)assert(os_queue_work_item(base<800?&key_a:&key_b,(void*)(uintptr_t)(base+i))==NV_OK);
    return 0;
}
static DWORD WINAPI consume(void*arg){
    nvrm_work_queue_t*q=arg;
    for(unsigned done=0;done<800;){if(nvrm_work_process_one(q,&stack))done++;else SwitchToThread();}
    return 0;
}
int main(void){
    assert(os_flush_work_queue(NULL,NV_FALSE)==NV_OK&&!creates);
    sleep_allowed=false;assert(os_flush_work_queue(NULL,NV_FALSE)==NV_ERR_ILLEGAL_ACTION);
    assert(os_queue_work_item(NULL,NULL)==NV_ERR_NOT_READY&&!creates);sleep_allowed=true;
    fail_launch=true;assert(os_queue_work_item(NULL,NULL)==NV_ERR_OPERATING_SYSTEM);
    assert(!nvrm_global_work_queue.started&&!nvrm_global_work_queue.head);fail_launch=false;
    assert(nvrm_work_start(&nvrm_global_work_queue)==NV_OK);
    fail_alloc=true;assert(os_queue_work_item(NULL,NULL)==NV_ERR_NO_MEMORY);fail_alloc=false;
    assert(!nvrm_global_work_queue.submitted);

    // Observe another publisher while worker creation has not yet succeeded.
    launch_entered=CreateEvent(NULL,TRUE,FALSE,NULL);launch_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    block_launch=true;fail_launch=true;NV_STATUS first=-1;
    HANDLE launch=CreateThread(NULL,0,start_a,&first,0,NULL);assert(launch);
    assert(WaitForSingleObject(launch_entered,5000)==WAIT_OBJECT_0);
    qa=nvrm_work_find(&key_a);assert(qa&&qa->started==1&&!qa->head&&!qa->submitted);
    sleep_allowed=false;assert(os_queue_work_item(&key_a,(void*)8)==NV_ERR_NOT_READY);
    assert(!qa->head&&!qa->submitted);sleep_allowed=true;
    SetEvent(launch_release);assert(WaitForSingleObject(launch,5000)==WAIT_OBJECT_0);
    assert(first==NV_ERR_OPERATING_SYSTEM&&!qa->started&&!qa->head);
    CloseHandle(launch);CloseHandle(launch_entered);CloseHandle(launch_release);
    fail_launch=block_launch=false;
    assert(os_queue_work_item(&key_a,(void*)1)==NV_OK&&qa->started==2);
    assert(!nvrm_work_process_one(qa,NULL)&&!qa->completed); // OOM stack cannot consume
    requeue=true;wait_hook=pump_a;
    assert(os_flush_work_queue(&key_a,NV_FALSE)==NV_OK);
    assert(passes==2&&qa->completed==2&&called==2&&!qa->head);wait_hook=NULL;
    assert(!os_is_queue_flush_ongoing(&key_a));
    assert(os_queue_work_item(&key_b,(void*)3)==NV_OK);qb=nvrm_work_find(&key_b);
    assert(os_queue_work_item(&key_a,(void*)4)==NV_OK);expect_unload=true;wait_hook=pump_a;
    assert(os_flush_work_queue(&key_a,NV_TRUE)==NV_OK);wait_hook=NULL;expect_unload=false;
    assert(!os_is_queue_flush_ongoing(&key_a)&&qb->head&&!qb->completed);
    assert(nvrm_work_process_one(qb,&stack));
    qa->runner=proc_current();self_flush=true;
    assert(os_queue_work_item(&key_a,(void*)5)==NV_OK&&nvrm_work_process_one(qa,&stack));
    qa->runner=NULL;self_flush=false;
    irq=false;assert(os_flush_work_queue(&key_a,NV_FALSE)==NV_ERR_ILLEGAL_ACTION);
    assert(os_queue_work_item(&key_a,(void*)6)==NV_OK);irq=true;assert(nvrm_work_process_one(qa,&stack));
    assert(os_flush_work_queue(&key_c,NV_TRUE)==NV_OK&&!os_is_queue_flush_ongoing(&key_c));

    for(unsigned i=0;i<1600;i++)deliveries[i]=0;
    LONG baseline=allocs;HANDLE threads[10];
    for(unsigned i=0;i<8;i++){threads[i]=CreateThread(NULL,0,publish,(void*)(uintptr_t)(i*200),0,NULL);assert(threads[i]);}
    threads[8]=CreateThread(NULL,0,consume,qa,0,NULL);threads[9]=CreateThread(NULL,0,consume,qb,0,NULL);
    assert(WaitForMultipleObjects(10,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<10;i++)CloseHandle(threads[i]);
    for(unsigned i=0;i<1600;i++)assert(deliveries[i]==1);
    assert(qa->completed==qa->submitted&&qb->completed==qb->submitted&&allocs==baseline);
    assert(!nvrm_work_lock&&irq);
    puts("PASS production RM queues: failed/contended startup cannot accept orphan work, normal/unload two-pass flush, requeue barrier, per-queue identity, callback completion, OOM retention, ISR enqueue, self-flush refusal, 8 publishers + 2 consumers/1600 exact deliveries; no RM/GPU/codec execution");
}
'''
if __name__ == '__main__':
    run_test(code, 'nvrm-workqueue')
