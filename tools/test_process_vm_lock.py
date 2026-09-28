#!/usr/bin/env python3
"""Run production VM mutex/pin control flow on real Windows host threads.

The host SRW lock/condition variable substitutes for runqueue/context switching.
This exercises concurrent ownership and wakeups, NOT native AP/IRQ/CR3 safety.
"""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/sched.c').read_text()
code = r'''
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32; typedef uint64_t u64;
#define PROC_MAX 24
enum {PROC_UNUSED,PROC_READY,PROC_RUNNING,PROC_BLOCKED,PROC_ZOMBIE};
typedef struct proc {
    int state; bool is_kernel,resource_closing,vm_waiting;
    u64 pml4; u32 resource_pins;
    struct proc *leader,*vm_owner;
} proc_t;
static proc_t procs[PROC_MAX];
static _Thread_local proc_t *current;
static _Thread_local bool irq_on=true,queue_owned;
static SRWLOCK queue=SRWLOCK_INIT;
static CONDITION_VARIABLE changed=CONDITION_VARIABLE_INIT;
static HANDLE start;
static bool sched_running=true;
static unsigned counter,entered,max_entered;
static void panic(const char *s){fprintf(stderr,"%s\n",s);abort();}
static u32 cpu_current_slot(void){return current ? (u32)(current-procs) : 0;}
static bool scheduler_local_online(void){return current!=NULL;}
static proc_t *proc_shared(proc_t *p){return p&&p->leader?p->leader:p;}
static bool queue_lock(void){
    assert(!queue_owned);bool old=irq_on;irq_on=false;
    AcquireSRWLockExclusive(&queue);queue_owned=true;return old;
}
static void queue_unlock(bool irq){
    assert(queue_owned);queue_owned=false;
    WakeAllConditionVariable(&changed);ReleaseSRWLockExclusive(&queue);irq_on=irq;
}
static proc_t *pick_next(void){assert(queue_owned);return NULL;}
static void switch_to(proc_t *unused){
    (void)unused;assert(queue_owned&&!irq_on&&current->state==PROC_BLOCKED);
    assert(current->resource_pins&&current->vm_waiting);
    while(current->state==PROC_BLOCKED)
        assert(SleepConditionVariableSRW(&changed,&queue,5000,0));
    assert(current->state==PROC_READY);current->state=PROC_RUNNING;
    queue_unlock(false); // incoming context handoff releases production queue
}
'''
for name in ('proc_resource_pin', 'proc_resource_unpin', 'proc_vm_begin', 'proc_vm_end'):
    code += function(src, name) + '\n'
code += r'''
static DWORD WINAPI worker(void *arg){
    current=arg;assert(WaitForSingleObject(start,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<400;i++){
        proc_t *token=NULL;assert(proc_vm_begin(&token)&&token==current&&irq_on);
        assert(current->resource_pins==1&&proc_shared(current)->vm_owner==current);
        entered++;if(entered>max_entered)max_entered=entered;
        unsigned before=counter;
        if(!(i%7))Sleep(0); // force overlap while owning shared metadata
        counter=before+1;assert(entered==1);entered--;
        proc_vm_end(token);assert(!current->resource_pins&&irq_on);
    }
    return 0;
}
int main(void){
    HANDLE threads[16];start=CreateEvent(NULL,TRUE,FALSE,NULL);assert(start);
    for(unsigned i=0;i<16;i++){
        procs[i].state=PROC_RUNNING;procs[i].pml4=0x1000;
        procs[i].leader=i?&procs[0]:NULL;
        threads[i]=CreateThread(NULL,0,worker,&procs[i],0,NULL);assert(threads[i]);
    }
    SetEvent(start);assert(WaitForMultipleObjects(16,threads,TRUE,15000)==WAIT_OBJECT_0);
    assert(counter==6400&&max_entered==1&&!procs[0].vm_owner);
    for(unsigned i=0;i<16;i++){
        assert(!procs[i].resource_pins&&!procs[i].vm_waiting);CloseHandle(threads[i]);
    }
    CloseHandle(start);current=&procs[1];proc_t *token=(proc_t*)1;
    procs[0].resource_closing=true;
    assert(!proc_vm_begin(&token)&&!token&&!current->resource_pins);
    procs[0].resource_closing=false;current->resource_closing=true;
    assert(!proc_vm_begin(&token)&&!token&&!current->resource_pins);
    current->resource_closing=false;current->is_kernel=true;
    assert(!proc_vm_begin(&token)&&!token);
    current->is_kernel=false;current->pml4=0;
    assert(!proc_vm_begin(&token)&&!token);
    current=NULL;assert(!proc_vm_begin(&token)&&!token);assert(!proc_vm_begin(NULL));
    puts("PASS production VM metadata mutex: 16 concurrent host threads, 6400 serialized transactions, pin retention, wakeups, closing and invalid-caller refusal; native AP switching NOT emulated");
}
'''
if __name__ == '__main__':
    run_test(code, 'process-vm-lock')
