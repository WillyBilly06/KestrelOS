#!/usr/bin/env python3
"""Exercise actual scheduler device-transaction pin/drain/owner routines."""
from pathlib import Path
from test_gpu_stable_candidate import run_test,function
ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/'kernel/sched.c').read_text()

def main():
    c=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32;typedef uint64_t u64;
#define PROC_MAX 4
#define PROC_UNUSED 0
#define PROC_READY 1
#define PROC_ZOMBIE 2
#define PROC_SETUP 3
#define PROC_CPU_NONE UINT32_MAX
typedef struct proc {struct proc *leader;u32 resource_pins,running_cpu;bool resource_closing,kernel_active;int state;u64 gpu_owner_id,clear_child_tid;} proc_t;
static proc_t procs[PROC_MAX],*current;
static bool interrupts=true;
static bool queue_owned,sched_running;
static u32 cpu_current_slot(void){return 0;}
static unsigned yields;
static proc_t *pending;
static proc_t *proc_current(void){return current;}
static proc_t *proc_shared(proc_t *p){return p->leader?p->leader:p;}
static bool irq_save(void){bool was=interrupts;interrupts=false;return was;}
static void irq_restore(bool was){interrupts=was;}
static bool queue_lock(void){assert(!queue_owned);bool irq=irq_save();queue_owned=true;return irq;}
static void queue_unlock(bool irq){assert(queue_owned);queue_owned=false;irq_restore(irq);}
static void retire_stopped_threads(void){} /* stop policy has a separate test */
static void panic(const char *s){puts(s);abort();}
void proc_resource_unpin(proc_t *p);
static void sched_yield(void){assert(interrupts);assert(pending);if(++yields==3){proc_resource_unpin(pending);pending=NULL;}}
'''
    for name in ['proc_resource_pin','proc_resource_unpin','proc_resource_group_busy',
                 'proc_resource_drain','proc_gpu_owner_alive']:
        c+=function(SRC,name)+'\n'
    c+=r'''
int main(void){
    proc_t *token=(void*)1;
    assert(proc_resource_pin(&token)&&!token);proc_resource_unpin(token);assert(interrupts);
    procs[0]=(proc_t){.state=PROC_READY,.gpu_owner_id=55,.running_cpu=PROC_CPU_NONE};current=&procs[0];
    assert(proc_resource_pin(&token)&&token==current&&current->resource_pins==1);
    assert(proc_resource_group_busy(current));proc_resource_unpin(token);
    assert(!proc_resource_group_busy(current)&&proc_gpu_owner_alive(55));
    procs[1]=(proc_t){.state=PROC_READY,.leader=&procs[0],.gpu_owner_id=66,.running_cpu=PROC_CPU_NONE};current=&procs[1];
    assert(!proc_gpu_owner_alive(66));
    assert(proc_resource_pin(&token)&&token==current&&proc_resource_group_busy(&procs[0]));
    pending=token;current=&procs[0];proc_resource_drain(current);
    assert(yields==3&&!pending&&current->resource_closing&&!proc_resource_group_busy(current));
    assert(!proc_gpu_owner_alive(55));
    current=&procs[1];assert(!proc_resource_pin(&token)&&!current->resource_pins);
    current->resource_closing=true;assert(proc_resource_pin(&token));proc_resource_unpin(token);
    current->state=PROC_ZOMBIE;assert(!proc_resource_pin(&token));
    current->state=PROC_UNUSED;assert(!proc_resource_pin(&token));
    current->state=PROC_READY;current->resource_pins=UINT32_MAX;assert(!proc_resource_pin(&token));
    assert(interrupts);
    puts("PASS actual process render lifetime: early boot, exact-thread pins, group drain, closing sibling rejection, dead owners, overflow and IRQ restoration");
    return 0;
}
'''
    run_test(c,'proc_render_lifetime')
if __name__=='__main__':main()
