#!/usr/bin/env python3
"""Production Linux clone/TID completion with host queue/VM/stack adapters.

Exercises real host wait/exit threads, not privileged AP context switching.
The VMM word helper is production code; page walks and hardware FPU are mocked.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

sched = (ROOT / 'kernel/sched.c').read_text()
vmm = (ROOT / 'kernel/vmm.c').read_text()
abi = (ROOT / 'kernel/linux_abi.c').read_text()
header = (ROOT / 'kernel/proc.h').read_text()
code = r'''
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef int64_t s64;
#define PROC_MAX 8
#define PROC_CPU_NONE UINT32_MAX
#define PAGE_SIZE 4096
#define PTE_ADDR 0x000ffffffffff000ull
#define VMM_USER_COPY_MAX 16384
enum {E_INVAL=22,E_PERM=1,E_NOMEM=12,E_BUSY=16,E_AGAIN=11,E_NOSYS=38,E_FAULT=14,E_INTR=4,E_TIMEDOUT=110};
enum {PROC_UNUSED,PROC_SETUP,PROC_READY,PROC_RUNNING,PROC_SLEEPING,PROC_BLOCKED,PROC_ZOMBIE,PROC_DYING};
typedef struct {u64 rsp,rax,rip;}regs_t;
typedef struct proc {
    struct proc*leader;int state,pid,parent_pid,exit_code,stop_code;
    u32 resource_pins,running_cpu;
    bool is_kernel,is_idle,kernel_active,resource_closing,stop_requested,reapable,linux_abi;
    bool futex_waiting,futex_woken;
    u64 pml4,kstack,stack_low,fsbase,gsbase,wake_at_ms,futex_key,futex_space;
    u32 futex_mask;
    u64 parent_tid,set_child_tid,clear_child_tid;
    u8 fpu[512];regs_t frame;
}proc_t;
static proc_t procs[PROC_MAX];
static _Thread_local proc_t*current;
static proc_t*idle=&procs[7];
static _Thread_local bool irq_on=true,queue_owned,vm_owned;
static _Thread_local u64 loaded_cr3=0x1000;
static _Thread_local jmp_buf exit_jump;
static SRWLOCK queue_mutex=SRWLOCK_INIT,vm_mutex=SRWLOCK_INIT;
static u64 g_uptime_ms;
static HANDLE waiter_entered,waiter_woken;
static bool fail_stack,copy_fault,observe_publish,first_entry_stop;
static unsigned prepares,aborts,fpu_saves,drains;
static u32 parent_word,child_word,other_word;
static u64 parent_key=0x100,child_key=0x104;
static bool mapped=true,writable=true;
static bool irq_save(void){bool old=irq_on;irq_on=false;return old;}
static void irq_restore(bool old){irq_on=old;}
static bool queue_lock(void){assert(!queue_owned&&!vm_owned);bool old=irq_save();AcquireSRWLockExclusive(&queue_mutex);queue_owned=true;return old;}
static void queue_unlock(bool old){
    assert(queue_owned&&!vm_owned);
    if(observe_publish&&procs[1].state==PROC_READY){
        assert(parent_word==(u32)procs[1].pid&&child_word==777&&!procs[1].parent_tid);
        observe_publish=false;
    }
    if(waiter_woken&&procs[2].futex_woken)SetEvent(waiter_woken);
    queue_owned=false;ReleaseSRWLockExclusive(&queue_mutex);irq_restore(old);
}
static bool vmm_lock(void){assert(!vm_owned);bool old=irq_save();AcquireSRWLockExclusive(&vm_mutex);vm_owned=true;return old;}
static void vmm_unlock(bool old){assert(vm_owned);vm_owned=false;ReleaseSRWLockExclusive(&vm_mutex);irq_restore(old);}
static u64 read_cr3(void){return loaded_cr3;}
static bool user_range_locked(u64 root,u64 va,size_t n,bool write){
    assert(vm_owned&&n==4);return root==0x1000&&mapped&&(!write||writable)&&
        (va==(u64)&parent_word||va==(u64)&child_word||va==(u64)&other_word);
}
static u64 translate_locked(u64 root,u64 va){assert(vm_owned&&root==0x1000);return va==(u64)&parent_word?parent_key:child_key;}
static bool scheduler_local_online(void){return current!=NULL;}
static u32 cpu_current_slot(void){return 0;}
static void smp_reschedule_cpu(u32 slot){(void)slot;assert(0);}
/* This fixture does not enroll a second application CPU. Cross-core IPI
 * publication is exercised by the scheduler idle/wake fixture. */
static u32 idle_wakeup_target(void){return PROC_CPU_NONE;}
static void wake_idle_cpu(u32 target){assert(target==PROC_CPU_NONE);}
static proc_t*proc_shared(proc_t*p){return p&&p->leader?p->leader:p;}
static void panic(const char*message,...){fprintf(stderr,"%s\n",message);abort();}
static proc_t*pick_next(void){assert(queue_owned);return idle;}
static void switch_to(proc_t*next){
    assert(next==idle&&queue_owned&&!vm_owned);
    if(current->state==PROC_ZOMBIE){queue_unlock(false);longjmp(exit_jump,1);}
    assert(current==&procs[2]&&current->futex_waiting&&waiter_entered);
    SetEvent(waiter_entered);queue_unlock(false);
    assert(WaitForSingleObject(waiter_woken,5000)==WAIT_OBJECT_0);
    queue_lock();assert(current->state==PROC_READY);current->state=PROC_RUNNING;queue_unlock(false);
}
static void proc_resource_drain(proc_t*p){assert(p==current&&!p->resource_pins);drains++;}
static void proc_exit(int code){(void)code;assert(0);abort();} // leader-exit/group semantics separate
void proc_thread_exit(int);
'''
start = header.index('typedef struct {', header.index('/* The same, started the way Linux'))
code += header[start:header.index('} proc_clone_options_t;',start)+len('} proc_clone_options_t;')]
for name in ('vmm_user_copy','vmm_user_word_key','vmm_user_store_word_key'):
    code += '\n'+function(vmm,name)+'\n'
for name in ('futex_wake_key_locked','sched_futex_wait_ex','sched_futex_wait','sched_futex_wake_ex','sched_futex_wake',
             'proc_set_tid_address','thread_clear_tid_locked','proc_kernel_enter',
             'proc_kernel_leave','proc_thread_first_entry','proc_thread_exit',
             'retire_stopped_threads','proc_resource_group_busy','stop_sibling_threads',
             'thread_setup_publish'):
    code += function(sched,name)+'\n'
code += r'''
static int thread_setup_begin(proc_t**child,proc_t**creator,bool native){
    assert(!native&&current==&procs[0]);
    current->resource_pins++;*creator=current;*child=&procs[1];
    **child=(proc_t){.pid=101,.leader=current,.pml4=0x1000,.state=PROC_SETUP,.running_cpu=PROC_CPU_NONE};return 0;
}
static void thread_setup_abort(proc_t*t,proc_t*creator,bool user_stack){
    assert(!user_stack&&t->state==PROC_SETUP&&creator->resource_pins);aborts++;
    t->state=PROC_UNUSED;t->kstack=0;creator->resource_pins--;
}
static bool prepare_stack(proc_t*t,const regs_t*frame){
    prepares++;if(fail_stack)return false;t->frame=*frame;t->kstack=0x1234;return true;
}
static void fpu_save(void*storage){assert(current==&procs[0]);memset(storage,0xa5,512);fpu_saves++;}
'''
code += function(sched,'proc_thread_clone')+'\n'
code += '\n'.join(re.findall(r'^#define (?:CLONE_\w+|L_clone|L_set_tid_address|L_getpid|L_gettid)\s+[^\n]+',abi,re.M))+'\n'
code += 'static s64 thread_abi(proc_t*p,regs_t*r,u64 nr,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4){switch(nr){\n'
for a,b in (('    case L_getpid:', '    case L_wait4:'),
            ('    case L_set_tid_address:', '    /* Asked at startup'),
            ('    case L_clone: {', '    /* ---- the ones with no answer')):
    code += abi[abi.index(a):abi.index(b,abi.index(a))]
code += 'default:return -E_NOSYS;}}\n'
code += r'''
static void reset(void){
    memset(procs,0,sizeof procs);current=&procs[0];irq_on=mapped=writable=true;
    loaded_cr3=0x1000;queue_owned=vm_owned=fail_stack=copy_fault=observe_publish=first_entry_stop=false;
    parent_word=555;child_word=777;other_word=999;parent_key=0x100;child_key=0x104;
    prepares=aborts=fpu_saves=drains=0;waiter_entered=waiter_woken=NULL;
    *current=(proc_t){.pid=100,.pml4=0x1000,.state=PROC_RUNNING,.running_cpu=0,.linux_abi=true,.fsbase=0xabc,.gsbase=0xdef};
    for(unsigned i=1;i<PROC_MAX;i++)procs[i].running_cpu=PROC_CPU_NONE;
}
static const u64 flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD;
static DWORD WINAPI joiner(void*unused){
    (void)unused;current=&procs[2];loaded_cr3=0x1000;
    assert(!sched_futex_wait((u64)&child_word,101,-1));
    assert(child_word==0&&!current->futex_waiting&&!queue_owned&&!vm_owned);return 0;
}
static void exit_child(void){
    current=&procs[1];current->state=PROC_RUNNING;current->running_cpu=1;current->kernel_active=true;
    if(!setjmp(exit_jump)){proc_thread_exit(3);assert(0);}
    assert(current->state==PROC_ZOMBIE&&current->reapable&&!current->clear_child_tid&&!queue_owned);
    irq_on=true;
}
int main(void){
    reset();regs_t r={.rsp=0x10000,.rip=0x20000,.rax=56};
    u64 all=flags|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID;
    observe_publish=true;
    assert(thread_abi(current,&r,L_clone,all,0x12348,(u64)&parent_word,(u64)&child_word,0)==101);
    assert(!observe_publish&&parent_word==101&&child_word==777&&procs[1].state==PROC_READY);
    assert(procs[1].frame.rsp==0x12348&&!procs[1].frame.rax&&!procs[1].fsbase&&procs[1].gsbase==0xdef);
    assert(procs[1].clear_child_tid==(u64)&child_word&&fpu_saves==1&&!current->resource_pins);
    for(unsigned i=0;i<512;i++)assert(procs[1].fpu[i]==0xa5);
    current=&procs[1];current->state=PROC_RUNNING;current->running_cpu=1;
    proc_thread_first_entry();assert(child_word==101&&!current->set_child_tid&&!current->kernel_active);
    assert(thread_abi(current,&r,L_getpid,0,0,0,0,0)==100);
    assert(thread_abi(current,&r,L_gettid,0,0,0,0,0)==101);
    assert(thread_abi(current,&r,L_set_tid_address,(u64)&other_word,0,0,0,0)==101&&current->clear_child_tid==(u64)&other_word);
    assert(proc_set_tid_address(0)==101&&!current->clear_child_tid);

    reset();assert(thread_abi(current,&r,L_clone,flags,0x12348,0,0,0)==101&&procs[1].fsbase==0xabc);
    reset();fail_stack=true;assert(thread_abi(current,&r,L_clone,all,0x12348,(u64)&parent_word,(u64)&child_word,0)==-E_NOMEM);
    assert(aborts==1&&!current->resource_pins&&procs[1].state==PROC_UNUSED&&parent_word==555&&child_word==777);
    reset();current->resource_closing=true;
    assert(thread_abi(current,&r,L_clone,all,0x12348,(u64)&parent_word,(u64)&child_word,0)==-E_BUSY);
    assert(aborts==1&&parent_word==555&&child_word==777);
    reset();assert(thread_abi(current,&r,L_clone,all,0,0,0,0)==-E_INVAL&&!prepares);
    assert(thread_abi(current,&r,L_clone,all,0x12348,0,0,1ull<<47)==-E_INVAL&&!prepares);
    assert(thread_abi(current,&r,L_clone,flags&~CLONE_SIGHAND,0x12348,0,0,0)==-E_INVAL);
    assert(thread_abi(current,&r,L_clone,flags&~CLONE_FILES,0x12348,0,0,0)==-E_NOSYS);
    assert(thread_abi(current,&r,L_clone,flags|(1ull<<40),0x12348,0,0,0)==-E_NOSYS);
    // Linux ignores invalid TID stores; no kernel dereference/fault, clone still succeeds.
    assert(thread_abi(current,&r,L_clone,all,0x12348,1,3,0)==101);
    current=&procs[1];proc_thread_first_entry();exit_child();assert(parent_word==555&&child_word==777);

    // Clear and key extraction use one VMM lock and require the owning CR3.
    reset();u64 key=UINT64_MAX;child_key=0;
    assert(vmm_user_store_word_key(0x1000,(u64)&child_word,0,&key)&&!key&&!child_word);
    child_word=777;writable=false;assert(!vmm_user_store_word_key(0x1000,(u64)&child_word,0,&key)&&child_word==777);
    writable=true;loaded_cr3=0x2000;assert(!vmm_user_store_word_key(0x1000,(u64)&child_word,0,&key));
    loaded_cr3=0x1000;assert(!vmm_user_store_word_key(0x1000,(u64)&child_word+1,0,&key));

    // Registration and exit happen on different real host threads.
    reset();assert(thread_abi(current,&r,L_clone,all,0x12348,(u64)&parent_word,(u64)&child_word,0)==101);
    current=&procs[1];proc_thread_first_entry();
    procs[2]=(proc_t){.pid=102,.pml4=0x1000,.leader=&procs[0],.state=PROC_RUNNING,.running_cpu=2};
    waiter_entered=CreateEvent(NULL,TRUE,FALSE,NULL);waiter_woken=CreateEvent(NULL,TRUE,FALSE,NULL);assert(waiter_entered&&waiter_woken);
    HANDLE thread=CreateThread(NULL,0,joiner,NULL,0,NULL);assert(thread);
    assert(WaitForSingleObject(waiter_entered,5000)==WAIT_OBJECT_0);exit_child();
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&child_word==0&&drains==1);
    CloseHandle(thread);CloseHandle(waiter_entered);CloseHandle(waiter_woken);waiter_entered=waiter_woken=NULL;

    // A stopped unowned child with a pending clear must unwind on its own CR3.
    reset();assert(thread_abi(current,&r,L_clone,all,0x12348,(u64)&parent_word,(u64)&child_word,0)==101);
    procs[1].stop_requested=true;bool irq=queue_lock();retire_stopped_threads();
    assert(procs[1].state==PROC_READY&&proc_resource_group_busy(current)&&!stop_sibling_threads(current));queue_unlock(irq);
    current=&procs[1];current->state=PROC_RUNNING;current->running_cpu=1;
    if(!setjmp(exit_jump)){proc_thread_first_entry();assert(0);}
    assert(current->state==PROC_ZOMBIE&&!child_word&&!current->clear_child_tid&&!current->set_child_tid);
    puts("PASS production Linux thread lifecycle: parent-before-publication, child-before-user, explicit zero/inherited TLS, exact stack/FPU snapshot, failed setup, getpid/gettid, clear-address registration, protected exit zero+wake, real host join/exit handoff, stopped-child drain; page walks/stack/FPU/context-switch edges mocked, NOT native SMP/pthreads validation");
}
'''
if __name__ == '__main__':
    run_test(code, 'linux-thread-lifecycle')
