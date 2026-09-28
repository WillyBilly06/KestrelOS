#!/usr/bin/env python3
"""Execute scheduler completion-wait control flow with serial device/IRQ mocks.

This is not a GPU/codec workload, context-switch emulator, or SMP safety proof.
"""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT / 'kernel/sched.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64;
#define PROC_MAX 8
#define PROC_CPU_NONE UINT32_MAX
enum {PROC_UNUSED,PROC_READY,PROC_RUNNING,PROC_BLOCKED,PROC_SLEEPING,PROC_ZOMBIE};
typedef struct {int state;u32 running_cpu,resource_pins;} proc_t;
typedef bool (*sched_device_probe_fn)(void *);
static proc_t procs[PROC_MAX],*current,*idle;
static bool online,irq_on,queue_owned,complete;
static u64 g_uptime_ms;
static unsigned switches,mode,probes;
static bool scheduler_local_online(void){return online;}
static u32 cpu_current_slot(void){return 0;}
static u64 read_flags(void){return irq_on ? 0x202 : 2;}
static int slot_of(proc_t *p){return (int)(p-procs);}
static bool queue_lock(void){
    assert(!queue_owned);bool old=irq_on;irq_on=false;queue_owned=true;return old;
}
static void queue_unlock(bool old){assert(queue_owned);queue_owned=false;irq_on=old;}
static void switch_to(proc_t *p);
static proc_t *pick_next(void);
'''
    start = src.index('typedef struct {\n    sched_device_probe_fn probe;')
    end = src.index('static u32 device_wait_count;', start) + len('static u32 device_wait_count;')
    c += src[start:end] + '\n'
    start = src.index('static proc_t *poll_device_waiters(')
    c += src[start:src.index('\n}', start)+2] + '\n'
    c += function(src, 'sched_device_wait_allowed') + '\n'
    c += function(src, 'sched_wait_device') + r'''
static bool probe(void *context){
    assert(context==&complete&&queue_owned&&!irq_on);probes++;return complete;
}
static proc_t *pick_next(void){
    assert(queue_owned);proc_t *p=poll_device_waiters();return p ? p : idle;
}
static void switch_to(proc_t *next){
    proc_t *waiter=current;
    assert(next==idle&&queue_owned&&!irq_on&&waiter->resource_pins==1);
    assert(waiter->state==PROC_BLOCKED&&device_wait_count==1);
    assert(device_waits[slot_of(waiter)].context==&complete);
    /* Simulate the saved-context handoff before an IRQ or timer probes. */
    waiter->running_cpu=PROC_CPU_NONE;current=idle;
    queue_unlock(false);switches++;
    bool old=queue_lock();
    if(mode==1)g_uptime_ms=device_waits[slot_of(waiter)].deadline_ms;
    else if(mode==2&&switches==1){
        g_uptime_ms++;waiter->state=PROC_READY; // unrelated wake
    }else complete=true;
    assert(poll_device_waiters()==waiter);
    assert(waiter->state==PROC_READY&&waiter->running_cpu==PROC_CPU_NONE);
    waiter->running_cpu=0;waiter->state=PROC_RUNNING;current=waiter;
    queue_unlock(old);
}
static void reset(void){
    memset(procs,0,sizeof procs);memset(device_waits,0,sizeof device_waits);
    for(int i=0;i<PROC_MAX;i++)procs[i].running_cpu=PROC_CPU_NONE;
    current=&procs[0];idle=&procs[1]; // boot is no longer idle
    current->state=PROC_RUNNING;current->running_cpu=0;current->resource_pins=1;
    idle->state=PROC_READY;online=irq_on=true;queue_owned=complete=false;
    g_uptime_ms=10;switches=mode=probes=device_wait_count=0;
}
static void clean(void){
    assert(current==&procs[0]&&current->state==PROC_RUNNING);
    assert(current->resource_pins==1&&irq_on&&!queue_owned);
    assert(device_wait_count==0&&!device_waits[0].probe&&!device_waits[0].context);
}
int main(void){
    reset();complete=true;assert(sched_wait_device(probe,&complete,20)==1&&!switches);clean();
    reset();assert(sched_wait_device(probe,&complete,10)==0&&!switches);clean();
    reset();assert(sched_wait_device(probe,&complete,20)==1&&switches==1);clean();
    reset();mode=1;assert(sched_wait_device(probe,&complete,20)==0&&switches==1);
    assert(g_uptime_ms==20);clean();
    reset();mode=2;assert(sched_wait_device(probe,&complete,20)==1&&switches==2);clean();
    reset();current->resource_pins=0;
    assert(sched_wait_device(probe,&complete,20)==-1&&!probes&&!switches);
    reset();irq_on=false;assert(sched_wait_device(probe,&complete,20)==-1&&!irq_on&&!probes);
    reset();online=false;assert(sched_wait_device(probe,&complete,20)==-1&&!probes);
    reset();current=idle;assert(sched_wait_device(probe,&complete,20)==-1&&!probes);
    reset();assert(sched_wait_device(NULL,&complete,20)==-1&&!probes);
    reset();device_waits[0].probe=probe;
    assert(sched_wait_device(probe,&complete,20)==-1&&!probes&&!queue_owned&&irq_on);
    /* A probe may ready a still-owned context, but never select it until its
     * outgoing stack is saved. Dead contexts must not be revived. */
    reset();queue_lock();device_wait_count=1;
    device_waits[0]=(device_wait_t){probe,&complete,20};
    current->state=PROC_BLOCKED;complete=true;
    assert(poll_device_waiters()==NULL&&current->state==PROC_READY);
    current->running_cpu=PROC_CPU_NONE;current=idle;
    assert(poll_device_waiters()==&procs[0]);
    procs[0].state=PROC_ZOMBIE;assert(poll_device_waiters()==NULL);
    queue_unlock(true);
    puts("PASS production scheduler device wait: boot admission, completion/deadline/spurious wake, pin/IF/offline/idle refusal, registration lifetime and ownership exclusion; serial mocks only");
}
'''
    run_test(c, 'scheduler_device_wait')


if __name__ == '__main__':
    main()
