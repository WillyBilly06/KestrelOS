#!/usr/bin/env python3
"""Execute RM host synchronization primitives, not RM/GPU/codec workloads.

Uses production nvrm_os.c code with real Windows host threads. The scheduler
event queue, IRQ/TLB edges and allocator are substituted. Native interrupt
entry and the RM binary are NOT exercised.
"""
from test_gpu_stable_candidate import ROOT, run_test

src = (ROOT / 'kernel/nvrm_os.c').read_text()
body = src[src.index('typedef struct { volatile NvU32 held;'):src.index('static nvidia_stack_t *nvrm_stack_alloc')]
code = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t NvU32;typedef int32_t NvS32;typedef uint64_t NvU64,u64;
typedef uint32_t u32;
typedef int NV_STATUS,NvBool;
enum {NV_OK=0,NV_TRUE=1,NV_FALSE=0,NV_ERR_INVALID_ARGUMENT=2,
      NV_ERR_NO_MEMORY=3,NV_ERR_INVALID_REQUEST=4,NV_ERR_TIMEOUT_RETRY=5,NV_ERR_INVALID_STATE=6};
#define ARRAY_LEN(x) (sizeof(x)/sizeof(*(x)))
static volatile NvU32 nvrm_in_isr[64];
static _Thread_local u32 mock_cpu;
static u32 cpu_current_slot(void){return mock_cpu;}
static _Thread_local bool irq=true,task=true;
static unsigned allocations;
static bool alloc_fail;
static void *kzalloc(size_t n){if(alloc_fail)return NULL;void *p=calloc(1,n);if(p)allocations++;return p;}
static void kfree(void *p){if(p){allocations--;free(p);}}
static u64 read_flags(void){return irq?0x200:0;}
static void *proc_current(void){return task?(void*)1:NULL;}
static bool irq_save(void){bool old=irq;irq=false;return old;}
static void irq_restore(bool on){irq=on;}
static void smp_tlb_poll(void){}
static SRWLOCK queue=SRWLOCK_INIT;
static CONDITION_VARIABLE changed=CONDITION_VARIABLE_INIT;
static void (*before_wait)(void);
static bool cancel_once;
static unsigned yields;
static volatile LONG waits;
static void sched_yield(void){assert(irq&&task);yields++;SwitchToThread();}
static int sched_wait_event(u64 *seq,u64 expected,u64 deadline){
    assert(irq&&task&&deadline==~0ull);InterlockedIncrement(&waits);
    if(before_wait){void(*fn)(void)=before_wait;before_wait=NULL;fn();}
    if(cancel_once){cancel_once=false;return -1;}
    AcquireSRWLockExclusive(&queue);
    while(__atomic_load_n(seq,__ATOMIC_ACQUIRE)==expected)
        assert(SleepConditionVariableSRW(&changed,&queue,5000,0));
    ReleaseSRWLockExclusive(&queue);return 1;
}
static void sched_signal_event(u64 *seq){
    AcquireSRWLockExclusive(&queue);__atomic_fetch_add(seq,1ull,__ATOMIC_RELEASE);
    WakeAllConditionVariable(&changed);ReleaseSRWLockExclusive(&queue);
}
'''
code += body
code += r'''
static void *mutex,*sema;
static unsigned counter,inside;
static void release_mutex_before_wait(void){os_release_mutex(mutex);}
static void release_sema_before_wait(void){assert(os_release_semaphore(sema)==NV_OK);}
static DWORD WINAPI worker(void *unused){
    (void)unused;
    for(unsigned i=0;i<1000;i++){
        assert(os_acquire_mutex(mutex)==NV_OK);assert(!inside++);
        unsigned n=counter;SwitchToThread();counter=n+1;assert(inside--==1);
        os_release_mutex(mutex);
    }
    return 0;
}
static DWORD WINAPI consumer(void *unused){
    (void)unused;for(unsigned i=0;i<1000;i++)assert(os_acquire_semaphore(sema)==NV_OK);return 0;
}
int main(void){
    assert(os_alloc_mutex(NULL)==NV_ERR_INVALID_ARGUMENT);alloc_fail=true;
    assert(os_alloc_mutex(&mutex)==NV_ERR_NO_MEMORY&&!mutex&&!os_alloc_semaphore(1));alloc_fail=false;
    assert(os_alloc_mutex(&mutex)==NV_OK);sema=os_alloc_semaphore(0);assert(sema);
    assert(os_acquire_mutex(NULL)==NV_ERR_INVALID_ARGUMENT);
    assert(os_cond_acquire_mutex(NULL)==NV_ERR_INVALID_ARGUMENT);
    assert(os_acquire_semaphore(NULL)==NV_ERR_INVALID_ARGUMENT); // formerly infinite yield
    assert(os_cond_acquire_semaphore(NULL)==NV_ERR_INVALID_ARGUMENT);
    assert(os_release_semaphore(NULL)==NV_ERR_INVALID_ARGUMENT);
    assert(os_cond_acquire_semaphore(sema)==NV_ERR_TIMEOUT_RETRY);
    assert(os_cond_acquire_mutex(mutex)==NV_OK);
    assert(os_cond_acquire_mutex(mutex)==NV_ERR_TIMEOUT_RETRY);
    before_wait=release_mutex_before_wait;
    assert(os_acquire_mutex(mutex)==NV_OK);os_release_mutex(mutex);
    before_wait=release_sema_before_wait;assert(os_acquire_semaphore(sema)==NV_OK);
    assert(os_cond_acquire_semaphore(sema)==NV_ERR_TIMEOUT_RETRY);

    for(unsigned context=0;context<3;context++){
        irq=context!=0;task=context!=1;nvrm_in_isr[0]=context==2;
        assert(!os_semaphore_may_sleep());
        assert(os_acquire_mutex(mutex)==NV_ERR_INVALID_REQUEST);
        assert(os_cond_acquire_mutex(mutex)==NV_ERR_INVALID_REQUEST);
        assert(os_acquire_semaphore(sema)==NV_ERR_INVALID_REQUEST);
        // Linux permits semaphore try/release from IRQ; mutex try is different.
        assert(os_release_semaphore(sema)==NV_OK&&os_cond_acquire_semaphore(sema)==NV_OK);
        assert(os_cond_acquire_semaphore(sema)==NV_ERR_TIMEOUT_RETRY);
    }
    irq=task=true;nvrm_in_isr[0]=0;assert(os_semaphore_may_sleep());
    nvrm_in_isr[1]=1;assert(!nvrm_current_isr()&&os_semaphore_may_sleep());
    mock_cpu=1;assert(nvrm_current_isr()&&!os_semaphore_may_sleep());
    mock_cpu=64;assert(nvrm_current_isr()&&!os_semaphore_may_sleep());
    mock_cpu=0;nvrm_in_isr[1]=0;assert(irq&&os_semaphore_may_sleep());
    assert(os_acquire_mutex(mutex)==NV_OK);before_wait=release_mutex_before_wait;cancel_once=true;
    assert(os_acquire_mutex(mutex)==NV_OK&&yields==1);os_release_mutex(mutex);
    // RM waits do not falsely succeed or abandon lock ownership on process stop.
    before_wait=release_sema_before_wait;cancel_once=true;
    assert(os_acquire_semaphore(sema)==NV_OK&&yields==2);
    NvU64 flags=os_acquire_spinlock(mutex);assert(!irq&&flags);
    os_release_spinlock(mutex,flags);assert(irq); // spin release must not sleep/signal
    void *wide=os_alloc_semaphore(0xffffffffu);assert(wide);
    assert(os_release_semaphore(wide)==NV_ERR_INVALID_STATE);
    assert(os_cond_acquire_semaphore(wide)==NV_OK&&((nvrm_sema_t*)wide)->count==0xfffffffeu);
    os_free_semaphore(wide);
    HANDLE threads[8];
    for(unsigned i=0;i<8;i++){threads[i]=CreateThread(NULL,0,worker,NULL,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(8,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<8;i++)CloseHandle(threads[i]);assert(counter==8000&&!inside);
    for(unsigned i=0;i<4;i++){threads[i]=CreateThread(NULL,0,consumer,NULL,0,NULL);assert(threads[i]);}
    for(unsigned i=0;i<4000;i++)assert(os_release_semaphore(sema)==NV_OK);
    assert(WaitForMultipleObjects(4,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;i++)CloseHandle(threads[i]);assert(!((nvrm_sema_t*)sema)->count);
    os_free_semaphore(sema);os_free_mutex(mutex);assert(!allocations&&waits);
    puts("PASS production RM host synchronization: Linux sleep/try status contracts, null refusal, wake-before-sleep, stop retry, IRQ-safe semaphore try/release, unsigned count/overflow, 8-thread mutex and 4-consumer semaphore stress; no RM/GPU/codec execution");
}
'''
if __name__ == '__main__':
    run_test(code, 'nvrm-sync')
