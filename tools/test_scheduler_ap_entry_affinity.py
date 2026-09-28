#!/usr/bin/env python3
"""Check the production kernel-entry guard before AP user admission."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/sched.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef uint32_t u32;
#define PROC_CPU_NONE UINT32_MAX
enum {PROC_READY=2,PROC_RUNNING=3};
typedef struct {bool is_kernel,is_idle,kernel_active;u32 running_cpu,state;} proc_t;
static proc_t task,ap_idle,*current=&task,*idle=&ap_idle;
static u32 cpu;
static bool online=true,owned;
static jmp_buf panic_target;
static unsigned panics,locks;
static unsigned ipis,migrations;
static bool scheduler_local_online(void){return online;}
static u32 cpu_current_slot(void){return cpu;}
static bool queue_lock(void){assert(!owned);owned=true;locks++;return true;}
static void queue_unlock(bool irq){assert(owned&&irq);owned=false;}
static void irq_restore(bool irq){assert(irq);}
static void smp_reschedule_cpu(u32 slot){assert(slot==0);ipis++;}
static proc_t*pick_next(void){assert(owned&&cpu==1);return idle;}
static void switch_to(proc_t*next){
    assert(owned&&next==idle&&task.state==PROC_READY);
    migrations++;owned=false;cpu=0;current=&task;task.state=PROC_RUNNING;task.running_cpu=0;
}
__attribute__((noreturn)) static void panic(const char*why){
    assert(why);panics++;longjmp(panic_target,1);
}
'''
code += function(src, 'proc_kernel_enter') + '\n'
code += r'''
int main(void){
    proc_kernel_enter();
    assert(task.kernel_active&&locks==1&&!owned);
    task.kernel_active=false;cpu=1;online=false;
    proc_kernel_enter();assert(!task.kernel_active&&locks==1);
    online=true;
    idle=NULL;task.running_cpu=1;
    if(!setjmp(panic_target))proc_kernel_enter();
    assert(panics==1&&owned&&locks==2);
    owned=false;task.kernel_active=false;idle=&ap_idle;
    ap_idle.is_idle=true;ap_idle.running_cpu=PROC_CPU_NONE;
    proc_kernel_enter();
    assert(cpu==0&&task.kernel_active&&migrations==1&&ipis==1&&!owned);
    task.is_kernel=true;proc_kernel_enter();
    assert(panics==1&&locks==3);
    puts("PASS production kernel entry: BSP admission, AP fallback refusal, AP continuation handoff to BSP, offline/worker no-op; host switch mocked");
    return 0;
}
'''
run_test(code, 'scheduler-ap-entry-affinity')
