#!/usr/bin/env python3
"""Production event/wait control-flow scaffold; not real SMP or IRQ execution.

--compile-only links the model without executing any assertions or workloads.
"""
import sys
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    sched = (ROOT / 'kernel/sched.c').read_text()
    event = (ROOT / 'kernel/eventq.c').read_text()
    event = '\n'.join(s for s in event.splitlines() if not s.startswith('#include'))
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "include/kestrel/input.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef long long ssize_t_k;
#define ARRAY_LEN(x) (sizeof(x)/sizeof(*(x)))
#define E_INVAL 22
#define E_AGAIN 11
#define PROC_MAX 8
#define PROC_CPU_NONE UINT32_MAX
#define VN_CHR 1
#define kinfo(...) ((void)0)
enum {PROC_UNUSED,PROC_SETUP,PROC_READY,PROC_RUNNING,PROC_SLEEPING,PROC_BLOCKED,PROC_ZOMBIE,PROC_DYING};
typedef struct {int state; u64 wake_at_ms; u64 *event_sequence; bool stop_requested;} proc_t;
static proc_t procs[PROC_MAX], *current, *idle;
static bool online=true, irq_on=true, queue_owned, driver_owned;
static unsigned switches, mode;
static u64 g_uptime_ms;
typedef struct {bool held;} spinlock_t;
static bool spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->held && !queue_owned); bool irq=irq_on; irq_on=false;
    lock->held=driver_owned=true; return irq;
}
static void spin_unlock_irqrestore(spinlock_t *lock,bool irq) {
    assert(lock->held && driver_owned); lock->held=driver_owned=false; irq_on=irq;
}
static bool scheduler_local_online(void) {return online;}
static bool queue_lock(void) {
    assert(!queue_owned && !driver_owned); bool irq=irq_on;
    irq_on=false; queue_owned=true; return irq;
}
static void queue_unlock(bool irq) {assert(queue_owned); queue_owned=false;irq_on=irq;}
/* Remote scheduler wake delivery is covered by test_scheduler_idle_wake.py. */
static u32 idle_wakeup_target(void){return PROC_CPU_NONE;}
static void wake_idle_cpu(u32 target){assert(target==PROC_CPU_NONE);}
static proc_t *pick_next(void) {assert(queue_owned);return idle;}
static void switch_to(proc_t *next);
typedef struct {u32 in_bytes,out_bytes;bool required;} vfs_ioctl_shape_t;
typedef struct {
    ssize_t_k (*read)(void*,void*,size_t,u64);
    void *write;
    int (*ioctl)(void*,u32,void*);
    u64 (*size)(void*);
    int (*ioctl_shape)(void*,u32,vfs_ioctl_shape_t*);
} devfs_ops_t;
static void devfs_register(const char *name,int type,const devfs_ops_t *ops,void *ctx) {
    (void)name;(void)type;(void)ops;(void)ctx;
}
'''
    c += function(sched, 'sched_wait_event') + '\n'
    c += function(sched, 'sched_signal_event') + '\n' + event
    c += r'''
/* Model only the boundary after context-save/queue release. Real assembly,
 * concurrent CPUs, IRQ priority and weak-memory interleavings are not emulated. */
static void switch_to(proc_t *next) {
    assert(next==idle && queue_owned && current->event_sequence);
    assert(current->state==PROC_SLEEPING || current->state==PROC_BLOCKED);
    switches++; queue_unlock(false);
    if(mode==1) {g_uptime_ms=current->wake_at_ms;current->state=PROC_READY;}
    else if(mode==4) {current->stop_requested=true;current->state=PROC_READY;}
    else if(mode==2 && switches==1) current->state=PROC_READY; // spurious wake
    else if(mode==3) input_push_key(55,0,true);
    else sched_signal_event(current->event_sequence);
    assert(current->state==PROC_READY);
    current->state=PROC_RUNNING;
}
static void reset(void) {
    memset(procs,0,sizeof procs);current=&procs[1];idle=&procs[0];
    current->state=PROC_RUNNING;idle->state=PROC_READY;
    online=irq_on=true;queue_owned=driver_owned=false;
    switches=mode=0;g_uptime_ms=10;head=tail=0;dropped=event_sequence=0;
    event_lock.held=false;
}
int main(void) {
    u64 seq=1,other=0;
    reset();assert(sched_wait_event(&seq,0,~0ull)==1 && switches==0 && irq_on);
    assert(!current->event_sequence); // signalled before registration
    assert(sched_wait_event(&seq,1,10)==0 && switches==0 && irq_on);
    assert(sched_wait_event(&seq,1,~0ull)==1 && switches==1 && irq_on);
    assert(!current->event_sequence && !queue_owned);
    reset();seq=0;current->stop_requested=true;
    assert(sched_wait_event(&seq,0,~0ull)==-1&&!switches&&!current->event_sequence);
    reset();seq=0;mode=4;
    assert(sched_wait_event(&seq,0,~0ull)==-1&&switches==1&&!current->event_sequence);
    assert(irq_on&&!queue_owned);
    reset();seq=0;mode=2;
    assert(sched_wait_event(&seq,0,~0ull)==1 && switches==2); // spurious then actual
    reset();seq=0;mode=1;
    assert(sched_wait_event(&seq,0,25)==0 && g_uptime_ms==25 && switches==1);
    reset();seq=0;irq_on=false;
    assert(sched_wait_event(&seq,0,~0ull)==1 && !irq_on); // caller IF preserved
    reset();seq=0;
    for(int i=2;i<PROC_MAX;i++)procs[i].event_sequence=&seq;
    procs[2].state=PROC_BLOCKED;procs[3].state=PROC_SLEEPING;
    procs[4].state=PROC_ZOMBIE;procs[5].state=PROC_DYING;procs[6].state=PROC_SETUP;
    procs[7].state=PROC_BLOCKED;procs[7].event_sequence=&other;
    sched_signal_event(&seq);
    assert(procs[2].state==PROC_READY && procs[3].state==PROC_READY);
    assert(procs[4].state==PROC_ZOMBIE && procs[5].state==PROC_DYING);
    assert(procs[6].state==PROC_SETUP && procs[7].state==PROC_BLOCKED);
    online=false;assert(sched_wait_event(&seq,1,~0ull)==-1);
    reset();current=idle;assert(sched_wait_event(&seq,1,~0ull)==-1);
    reset();current=NULL;sched_signal_event(&seq); // pre-process input producer
    assert(irq_on && !queue_owned);

    reset();mode=3;u32 ms=500;
    assert(input_dev_ioctl(NULL,3,&ms)==0 && ms==1 && switches==1);
    kinput_event_t batch[40];
    assert(input_dev_read(NULL,batch,sizeof batch,0)==sizeof(kinput_event_t));
    assert(batch[0].type==KEV_KEY && batch[0].code==55 && queue_count()==0);
    reset();mode=1;ms=8;
    assert(input_dev_ioctl(NULL,3,&ms)==0 && ms==0 && switches==1 && g_uptime_ms==18);
    reset();ms=0;assert(input_dev_ioctl(NULL,3,&ms)==0 && ms==0 && !switches);
    reset();input_push_mouse(10,20,1,2,3,1,1);
    assert(queue_count()==3 && event_sequence==1);
    assert(input_dev_read(NULL,batch,sizeof batch,0)==3*sizeof(kinput_event_t));
    assert(batch[0].type==KEV_MOUSE_MOVE && batch[1].type==KEV_MOUSE_WHEEL);
    assert(batch[2].type==KEV_MOUSE_BUTTON && batch[2].wheel==0);
    reset();for(unsigned i=0;i<300;i++)input_push_key((u16)i,0,true);
    assert(queue_count()==255 && eventq_dropped()==45);
    assert(input_dev_read(NULL,batch,sizeof batch,0)==32*sizeof(kinput_event_t));
    assert(batch[0].code==45 && batch[31].code==76 && queue_count()==223);
    assert(!queue_owned && !driver_owned && irq_on);
    puts("PASS model: sequence wait handoff, spurious/timeout/IF, dead waiters, bounded input batches");
}
'''
    run_test(c, 'input-event-wait', include_dirs=(ROOT,),
             compile_only='--compile-only' in sys.argv)


if __name__ == '__main__':
    main()
