#!/usr/bin/env python3
"""Exercise production kernel-return re-admission with a mocked saved switch."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/sched.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define PROC_CPU_NONE UINT32_MAX
enum {PROC_READY=2,PROC_RUNNING=3};
typedef struct {bool is_kernel,kernel_active,stop_requested;u32 state,resource_pins;int stop_code;} proc_t;
static proc_t task,ap_idle,*current=&task;
static u32 cpu,target=PROC_CPU_NONE;
static bool owned,online=true;
static unsigned ipis,switches,exits;
static bool scheduler_local_online(void){return online;}
static u32 cpu_current_slot(void){return cpu;}
static bool queue_lock(void){assert(!owned);owned=true;return true;}
static void queue_unlock(bool irq){assert(owned&&irq);owned=false;}
static void irq_restore(bool irq){assert(irq);}
static u32 idle_wakeup_target(void){assert(owned);return target;}
static void smp_reschedule_cpu(u32 slot){assert(owned&&slot==target);ipis++;}
static proc_t *pick_next(void){assert(owned&&task.state==PROC_READY);return &ap_idle;}
static void switch_to(proc_t *next){
    assert(owned&&next==&ap_idle&&task.state==PROC_READY);
    switches++;owned=false;cpu=target;task.state=PROC_RUNNING;current=&task;
}
static void proc_thread_exit(int code){(void)code;exits++;assert(0);}
static void panic(const char *why){fprintf(stderr,"%s\n",why);assert(0);}
'''
code += function(src, 'proc_kernel_leave') + r'''
int main(void){
    task.state=PROC_RUNNING;task.kernel_active=true;
    proc_kernel_leave();
    assert(!task.kernel_active&&task.state==PROC_RUNNING&&!ipis&&!switches&&!owned);
    cpu=0;target=1;task.kernel_active=true;
    proc_kernel_leave();
    assert(cpu==1&&!task.kernel_active&&task.state==PROC_RUNNING);
    assert(ipis==1&&switches==1&&!owned&&!exits);
    puts("PASS production kernel return: BSP-only fast path and idle-AP return-to-user handoff after kernel_active clears; native context switch mocked");
}
'''
run_test(code, 'scheduler-ap-return-affinity')
