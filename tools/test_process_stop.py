#!/usr/bin/env python3
"""Production stop/drain/exit control flow; synthetic saved-context transitions.

No native AP switch, IPI, user instruction, driver or CR3 operation is executed.
"""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/sched.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef uint32_t u32;typedef uint64_t u64;
#define PROC_MAX 8
#define PROC_CPU_NONE UINT32_MAX
enum{PROC_UNUSED,PROC_SETUP,PROC_READY,PROC_RUNNING,PROC_SLEEPING,PROC_BLOCKED,PROC_ZOMBIE,PROC_DYING};
enum{E_NOENT=2,E_BUSY=16,E_PERM=1};
typedef struct proc{
    struct proc*leader;
    int state,pid,parent_pid,waiting_for,stop_code,exit_code;
    u32 running_cpu,resource_pins;
    bool is_kernel,is_idle,kernel_active,resource_closing,stop_requested,group_stop,reapable;
    u64 pml4,clear_child_tid;
    char name[16];
}proc_t;
static proc_t procs[PROC_MAX],*current;
static proc_t *idle;
static bool irq_on=true,queue_owned,drain_model;
static unsigned yields,fd_closes,destroyed,console_takes,thread_exits;
static int exit_code,fb_owner_pid;
static u64 cr3;
static jmp_buf exit_jump;
#define kinfo(...) ((void)0)
static void panic(const char *fmt,...){fprintf(stderr,"%s\n",fmt);abort();}
static bool irq_save(void){bool old=irq_on;irq_on=false;return old;}
static void irq_restore(bool old){irq_on=old;}
static bool queue_lock(void){assert(!queue_owned);bool old=irq_save();queue_owned=true;return old;}
static void queue_unlock(bool old){assert(queue_owned);queue_owned=false;irq_restore(old);}
static bool scheduler_local_online(void){return current!=NULL;}
static u32 cpu_current_slot(void){return 0;}
static void smp_reschedule_cpu(u32 slot){(void)slot;assert(0);}
static u32 idle_wakeup_target(void){return PROC_CPU_NONE;}
static proc_t*proc_current(void){return current;}
static proc_t*proc_shared(proc_t*p){return p&&p->leader?p->leader:p;}
static void thread_clear_tid_locked(proc_t*p){assert(queue_owned&&p==current);p->clear_child_tid=0;}
static proc_t*pick_next(void){assert(queue_owned);return &procs[7];}
static void switch_to(proc_t*p){
    assert(p==&procs[7]&&queue_owned&&current->state==PROC_ZOMBIE&&!current->pml4);
    queue_unlock(false);longjmp(exit_jump,2);
}
static void proc_thread_exit(int code){
    assert(!queue_owned&&!current->kernel_active&&!current->resource_pins);
    thread_exits++;exit_code=code;longjmp(exit_jump,1);
}
static void proc_fd_close_all(proc_t*p){
    assert(p==current&&!p->leader&&!p->resource_pins);fd_closes++;
}
static u64 vmm_kernel_pml4(void){return 0x9000;}
static void write_cr3(u64 p){cr3=p;}
static void vmm_destroy_address_space(u64 p){
    assert(p==0x1000&&cr3==0x9000&&!irq_on);
    for(unsigned i=1;i<PROC_MAX;i++)if(procs[i].leader==current)
        assert(procs[i].running_cpu==PROC_CPU_NONE&&!procs[i].kernel_active&&!procs[i].resource_pins);
    destroyed++;
}
static void console_take_framebuffer(void){console_takes++;}
static void sched_yield(void){
    assert(!queue_owned&&drain_model);proc_t*t=&procs[1];yields++;
    // Ownership must survive until the simulated context save is complete.
    assert(t->state!=PROC_ZOMBIE&&t->pml4==0x1000&&!destroyed);
    if(yields==1)t->kernel_active=true;
    else if(yields==2)t->running_cpu=PROC_CPU_NONE;
    else if(yields==3)t->kernel_active=false;
    else assert(0);
}
'''
for name in ('retire_stopped_threads', 'proc_resource_group_busy', 'proc_resource_drain',
             'request_group_stop', 'proc_stop_requested', 'proc_kernel_enter',
             'proc_kernel_leave', 'stop_sibling_threads', 'proc_exit'):
    code += function(src, name) + '\n'
start = src.index('static proc_t *proc_by_pid(')
code += src[start:src.index('\n}', start)+2] + '\n'
code += function(src, 'proc_kill') + '\n'
code += r'''
static void reset(void){
    memset(procs,0,sizeof procs);current=&procs[0];
    for(unsigned i=0;i<PROC_MAX;i++)procs[i].running_cpu=PROC_CPU_NONE;
    *current=(proc_t){.state=PROC_RUNNING,.pid=100,.parent_pid=900,.running_cpu=0,.pml4=0x1000};
    procs[1]=(proc_t){.leader=current,.state=PROC_READY,.pid=101,.running_cpu=PROC_CPU_NONE,.pml4=0x1000};
    procs[7]=(proc_t){.state=PROC_BLOCKED,.pid=900,.waiting_for=100,.running_cpu=PROC_CPU_NONE};
    irq_on=true;queue_owned=drain_model=false;yields=fd_closes=destroyed=console_takes=thread_exits=0;
    cr3=0x1000;fb_owner_pid=0;
}
int main(void){
    reset();queue_lock();request_group_stop(current,23);request_group_stop(current,99);
    assert(current->group_stop&&current->resource_closing&&current->stop_code==23);
    assert(procs[1].stop_requested&&procs[1].stop_code==23);
    retire_stopped_threads();assert(procs[1].state==PROC_ZOMBIE&&procs[1].reapable&&!procs[1].pml4);
    assert(current->state==PROC_RUNNING&&!current->reapable);queue_unlock(true);
    for(unsigned reason=0;reason<5;reason++){
        reset();proc_t*t=&procs[1];t->stop_requested=true;
        if(reason==0)t->running_cpu=2;
        if(reason==1)t->kernel_active=true;
        if(reason==2)t->resource_pins=1;
        if(reason==3)t->state=PROC_SETUP;
        if(reason==4)t->is_kernel=true;
        queue_lock();retire_stopped_threads();assert(t->state!=PROC_ZOMBIE&&t->pml4==0x1000);
        if(reason<4){assert(proc_resource_group_busy(current));assert(!stop_sibling_threads(current));}
        queue_unlock(true);
    }
    reset();current=&procs[1];proc_kernel_enter();assert(current->kernel_active);
    proc_kernel_leave();assert(!current->kernel_active&&!thread_exits&&irq_on);
    proc_kernel_enter();queue_lock();request_group_stop(&procs[0],42);queue_unlock(true);
    if(!setjmp(exit_jump)){proc_kernel_leave();assert(0);}
    assert(thread_exits==1&&exit_code==42&&!current->kernel_active&&!queue_owned);
    reset();current=&procs[7];assert(proc_kill(100,7)==0);
    assert(!fd_closes&&!destroyed&&procs[0].stop_requested&&procs[0].state==PROC_RUNNING);
    assert(proc_kill(999,0)==-E_NOENT);procs[0].is_kernel=true;assert(proc_kill(100,1)==-E_PERM);
    reset();current->state=PROC_SETUP;current=&procs[7];assert(proc_kill(100,0)==-E_BUSY);
    reset();current=&procs[1];current->kernel_active=true;
    if(!setjmp(exit_jump)){proc_exit(11);assert(0);}
    assert(thread_exits==1&&exit_code==11&&procs[0].stop_requested&&!fd_closes&&!destroyed);
    reset();current->kernel_active=true;fb_owner_pid=100;drain_model=true;
    procs[1].running_cpu=2;procs[1].state=PROC_RUNNING;
    int jump=setjmp(exit_jump);if(!jump){proc_exit(17);assert(0);}
    assert(jump==2&&yields==3&&fd_closes==1&&destroyed==1&&console_takes==1);
    assert(current->state==PROC_ZOMBIE&&current->exit_code==17&&!current->pml4);
    assert(procs[1].state==PROC_ZOMBIE&&procs[7].state==PROC_READY&&!queue_owned);
    puts("PASS production group stop: deferred kill, first-code arbitration, sibling delegation, entry unwind, owned/setup/pinned exclusions, three-phase simulated remote handoff, CR3 switch before destruction and parent publication; native SMP NOT emulated");
}
'''

if __name__ == '__main__':
    run_test(code, 'process-stop')
