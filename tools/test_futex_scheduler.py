#!/usr/bin/env python3
"""Execute production futex control flow with serial VM/context-switch edges.

This is a CPU-only model, not real parallel scheduling or GPU/VM execution.
Use --compile-only to link without running assertions.
"""
import sys
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    sched = (ROOT / 'kernel/sched.c').read_text()
    vmm = (ROOT / 'kernel/vmm.c').read_text()
    futex = (ROOT / 'kernel/futex.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef uint64_t u64;
#define PTE_ADDR 0x000ffffffffff000ull
#define PAGE_SIZE 4096u
#define PROC_MAX 80
#define E_INVAL 22
#define E_AGAIN 11
#define E_BUSY 16
#define E_FAULT 14
#define E_INTR 4
#define E_TIMEDOUT 110
enum {PROC_UNUSED,PROC_SETUP,PROC_READY,PROC_RUNNING,PROC_SLEEPING,PROC_BLOCKED,PROC_ZOMBIE,PROC_DYING};
typedef struct {int state;u64 pml4,wake_at_ms,futex_key,futex_space;u32 futex_mask;bool futex_waiting,futex_woken,stop_requested;} proc_t;
static proc_t procs[PROC_MAX],*current,*idle;
static bool irq_on=true,queue_owned,vm_owned,online=true,readable=true;
static u64 g_uptime_ms,loaded_cr3=0x1000,mapping_key=0x42000,other_key=0x43000;
static u32 word,alias_word,other_word;
static unsigned switches,mode;
static bool scheduler_local_online(void){return online;}
static bool queue_lock(void){assert(!queue_owned && !vm_owned);bool irq=irq_on;irq_on=false;queue_owned=true;return irq;}
static void queue_unlock(bool irq){assert(queue_owned && !vm_owned);queue_owned=false;irq_on=irq;}
static bool vmm_lock(void){assert(!vm_owned);bool irq=irq_on;irq_on=false;vm_owned=true;return irq;}
static void vmm_unlock(bool irq){assert(vm_owned);vm_owned=false;irq_on=irq;}
static u64 read_cr3(void){return loaded_cr3;}
static bool user_range_locked(u64 pml4,u64 va,size_t size,bool write){
    assert(vm_owned && !write && size==4 && pml4==0x1000);
    return readable && (va==(u64)&word || va==(u64)&alias_word || va==(u64)&other_word);
}
static u64 translate_locked(u64 pml4,u64 va){
    assert(vm_owned && pml4==0x1000);return va==(u64)&other_word?other_key:mapping_key;
}
static proc_t *pick_next(void){assert(queue_owned);return idle;}
static void switch_to(proc_t *next);
'''
    c += function(vmm, 'vmm_user_word_key') + '\n'
    c += function(sched, 'sched_futex_wait_ex') + '\n'
    c += function(sched, 'sched_futex_wait') + '\n'
    c += function(sched, 'futex_wake_key_locked') + '\n'
    c += function(sched, 'sched_futex_wake_ex') + '\n'
    c += function(sched, 'sched_futex_wake') + '\n'
    c += '\n'.join(s for s in futex.splitlines() if not s.startswith('#include'))
    c += r'''
static void switch_to(proc_t *next){
    assert(next==idle && queue_owned && !vm_owned && current->futex_waiting);
    proc_t *waiter=current;
    assert(waiter->state==PROC_BLOCKED || waiter->state==PROC_SLEEPING);
    switches++;queue_unlock(false);
    if(mode==1){g_uptime_ms=waiter->wake_at_ms;waiter->state=PROC_READY;}
    else if(mode==3){waiter->stop_requested=true;waiter->state=PROC_READY;}
    else if(mode==2 && switches==1){waiter->state=PROC_READY;} // generic spurious wake
    else {
        current=&procs[2];current->state=PROC_RUNNING;current->pml4=0x1000;
        if(mode==4){
            assert(waiter->futex_space==0x1000&&waiter->futex_key==(u64)&word);
            assert(sched_futex_wake_ex((u64)&alias_word,1,~0u,true)==0);
            assert(sched_futex_wake_ex((u64)&word,1,~0u,false)==0);
            assert(sched_futex_wake_ex((u64)&word,1,~0u,true)==1);
        }else if(mode==5){
            assert(waiter->futex_mask==1);
            assert(sched_futex_wake_ex((u64)&word,1,2,false)==0);
            assert(sched_futex_wake_ex((u64)&word,1,1,false)==1);
        }else{
            assert(futex_wake((u64)&alias_word,1)==1); // different VA, same backing key
            assert(futex_wake((u64)&word,1)==0); // selected waiter cannot consume twice
        }
        current=waiter;
    }
    assert(waiter->state==PROC_READY);waiter->state=PROC_RUNNING;
}
static void reset(void){
    memset(procs,0,sizeof procs);current=&procs[1];idle=&procs[0];
    current->state=PROC_RUNNING;current->pml4=0x1000;
    irq_on=online=readable=true;queue_owned=vm_owned=false;
    loaded_cr3=0x1000;mapping_key=0x42000;other_key=0x42004;
    word=alias_word=other_word=7;g_uptime_ms=100;switches=mode=0;
}
int main(void){
    reset();assert(futex_wait((u64)&word,6,-1)==-E_AGAIN && switches==0);
    assert(futex_wait((u64)&word,6,0)==-E_AGAIN && switches==0);
    assert(futex_wait((u64)&word,7,0)==-E_BUSY && switches==0);
    assert(!current->futex_waiting && irq_on && !queue_owned && !vm_owned);
    assert(futex_wait((u64)&word,7,-1)==0 && switches==1);
    assert(!current->futex_waiting && !current->futex_woken && !current->futex_key);
    reset();current->stop_requested=true;
    assert(futex_wait((u64)&word,7,-1)==-E_BUSY&&!switches&&!current->futex_waiting);
    reset();mode=3;assert(futex_wait((u64)&word,7,-1)==-E_BUSY&&switches==1);
    assert(!current->futex_waiting&&!current->futex_key&&irq_on&&!queue_owned);
    reset();mode=2;assert(futex_wait((u64)&word,7,-1)==0 && switches==2);
    reset();mode=1;assert(futex_wait((u64)&word,7,15)==-E_BUSY && g_uptime_ms==115);
    assert(switches==1 && !current->futex_waiting && irq_on);
    reset();mapping_key=0;assert(futex_wait((u64)&word,7,-1)==0); // PA zero is not absence
    reset();irq_on=false;assert(futex_wait((u64)&word,7,-1)==0 && !irq_on);
    reset();readable=false;assert(futex_wait((u64)&word,7,-1)==-E_INVAL);
    assert(futex_wake((u64)&word,1)==-E_INVAL);
    reset();loaded_cr3=0x2000;assert(futex_wait((u64)&word,7,-1)==-E_INVAL);
    reset();assert(futex_wait((u64)&word+1,7,-1)==-E_INVAL);
    current->futex_waiting=true;assert(futex_wait((u64)&word,7,-1)==-E_INVAL);
    reset();current=idle;assert(futex_wait((u64)&word,7,-1)==-E_INVAL);
    reset();online=false;assert(futex_wait((u64)&word,7,-1)==-E_INVAL);
    reset();
    for(unsigned i=3;i<PROC_MAX;i++){
        procs[i].state=PROC_BLOCKED;procs[i].futex_waiting=true;procs[i].futex_key=mapping_key;procs[i].futex_mask=~0u;
    }
    procs[3].state=PROC_ZOMBIE;procs[4].state=PROC_DYING;procs[5].state=PROC_SETUP;
    procs[6].state=PROC_UNUSED;procs[7].futex_key=other_key;
    procs[9].state=PROC_READY;procs[10].state=PROC_RUNNING; // resumed before unregister
    assert(futex_wake((u64)&word,0)==0 && !procs[8].futex_woken);
    assert(futex_wake((u64)&word,-1)==0 && !procs[8].futex_woken);
    assert(futex_wake((u64)&word,1)==1 && procs[8].futex_woken);
    assert(futex_wake((u64)&word,80)==71); // no former 64-waiter table ceiling
    assert(procs[9].futex_woken && procs[10].futex_woken);
    assert(procs[9].state==PROC_READY && procs[10].state==PROC_RUNNING);
    assert(!procs[3].futex_woken && !procs[4].futex_woken && !procs[5].futex_woken);
    assert(!procs[6].futex_woken && !procs[7].futex_woken);
    assert(futex_wake((u64)&word,80)==0);
    assert(futex_wake((u64)&other_word,1)==1);
    memset(&procs[3],0,sizeof procs[3]);procs[3].state=PROC_BLOCKED; // reused slot
    assert(futex_wake((u64)&word,1)==0 && !procs[3].futex_woken);
    assert(!queue_owned && !vm_owned && irq_on);
    reset();mode=4;assert(!sched_futex_wait_ex((u64)&word,7,~0ull,~0u,true));
    assert(!current->futex_space&&!current->futex_mask&&!current->futex_waiting);
    reset();mode=5;assert(!sched_futex_wait_ex((u64)&word,7,~0ull,1,false));
    reset();mode=1;assert(sched_futex_wait_ex((u64)&word,7,115,1,false)==-E_TIMEDOUT&&g_uptime_ms==115);
    reset();assert(sched_futex_wait_ex((u64)&word,7,100,1,false)==-E_TIMEDOUT&&!switches);
    assert(sched_futex_wait_ex((u64)&word,6,100,1,false)==-E_AGAIN); // word mismatch precedes expired timeout
    assert(sched_futex_wait_ex((u64)&word,7,~0ull,0,false)==-E_INVAL);
    assert(sched_futex_wake_ex((u64)&word,1,0,false)==-E_INVAL);
    assert(sched_futex_wake_ex((u64)&word,-1,1,false)==-E_INVAL);
    reset();current->stop_requested=true;
    assert(sched_futex_wait_ex((u64)&word,7,~0ull,1,false)==-E_INTR);
    reset();readable=false;assert(sched_futex_wait_ex((u64)&word,7,~0ull,1,false)==-E_FAULT);
    reset();mode=1;g_uptime_ms=UINT64_MAX-5;
    assert(futex_wait((u64)&word,7,100)==-E_BUSY&&g_uptime_ms==UINT64_MAX-1);
    reset();procs[3]=(proc_t){.state=PROC_BLOCKED,.futex_waiting=true,.futex_key=(u64)&word,.futex_space=0x2000,.futex_mask=1};
    assert(sched_futex_wake_ex((u64)&word,1,1,true)==0&&!procs[3].futex_woken);
    procs[3].futex_space=0x1000;assert(sched_futex_wake_ex((u64)&word,1,1,true)==1);
    reset();readable=false;
    procs[3]=(proc_t){.state=PROC_BLOCKED,.futex_waiting=true,.futex_key=(u64)&word,.futex_space=0x1000,.futex_mask=1};
    assert(sched_futex_wake_ex((u64)&word,1,1,true)==1); // no user load/translation for private wake
    assert(sched_futex_wake_ex(1ull<<47,1,1,true)==-E_FAULT);
    puts("PASS futex model: scoped private/shared keys, bitset filtering, atomic registration, physical aliases/zero, absolute deadlines/error codes, saturation, spurious wakes, dead-slot exclusion, counts and VM guard");
}
'''
    run_test(c, 'futex-scheduler', compile_only='--compile-only' in sys.argv)


if __name__ == '__main__':
    main()
