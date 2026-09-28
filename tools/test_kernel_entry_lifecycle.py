#!/usr/bin/env python3
"""Execute production interrupt/syscall boundary wrappers with unprivileged mocks."""
from test_gpu_stable_candidate import ROOT, run_test

cpu = (ROOT / 'kernel/cpu.c').read_text()
sys = (ROOT / 'kernel/syscall.c').read_text()
assembly = (ROOT / 'kernel/isr.S').read_text()
bootstrap = assembly.split('thread_bootstrap:', 1)[1].split('jmp isr_return', 1)[0]
assert bootstrap.index('call sched_switch_complete') < bootstrap.index('call proc_thread_first_entry')

def extract(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start)+2]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef uint64_t u64;typedef int64_t s64;typedef uint32_t u32;
enum{E_PERM=1,ENTER=1,HANDLER,EOI,LEAVE,ENABLE,DISPATCH,MASK};
typedef struct{u64 cs,vector,rax,rdi,rsi,rdx,r10,r8;}regs_t;
typedef struct{bool linux_abi;}proc_t;
static proc_t proc,*current;
static bool active,irq_on,stop,nested,inside_nested;
static u32 cpu_slot;
static u32 cpu_current_slot(void){return cpu_slot;}
static bool smp_interrupt_local_on_ap(u32 vector){return vector>=0xf0&&vector<=0xf4;}
static u64 read_cr2(void){return 0x12345000;}
static void lapic_eoi(void){assert(0);}
__attribute__((noreturn)) static void panic(const char*why,...){(void)why;assert(0);__builtin_unreachable();}
static unsigned events[32],count,dispatches;
static jmp_buf stopped;
static void event(unsigned x){assert(count<32);events[count++]=x;}
static proc_t*proc_current(void){return current;}
static void proc_kernel_enter(void){assert(!active);active=true;event(ENTER);}
static void proc_kernel_leave(void){assert(active&&!irq_on);active=false;event(LEAVE);if(stop)longjmp(stopped,1);}
static bool proc_stop_requested(void){return stop;}
static void sti(void){irq_on=true;event(ENABLE);}
static void cli(void){irq_on=false;event(MASK);}
static s64 dispatch(proc_t*p,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    assert(p==current&&active&&irq_on);event(DISPATCH);dispatches++;return 123;
}
static s64 linux_dispatch(proc_t*p,regs_t*r){return dispatch(p,r->rax,0,0,0,0,0);}
static regs_t *interrupt_dispatch_inner(regs_t*r,u64 fault_address);
'''
code += extract(cpu, 'regs_t *interrupt_dispatch(regs_t *r) {') + '\n'
code += extract(sys, 'regs_t *syscall_dispatch(regs_t *r) {') + '\n'
code += r'''
static regs_t *interrupt_dispatch_inner(regs_t*r,u64 fault_address){
    if(r->vector==14)assert(fault_address==0x12345000);
    assert(!irq_on);event(HANDLER);
    if(nested&&!inside_nested){
        inside_nested=true;regs_t kernel={.cs=8};interrupt_dispatch(&kernel);inside_nested=false;
    }
    event(EOI);return r;
}
static void reset(void){current=&proc;proc.linux_abi=false;active=irq_on=stop=nested=inside_nested=false;count=dispatches=cpu_slot=0;}
int main(void){
    regs_t user={.cs=0x23},kernel={.cs=8};
    reset();assert(interrupt_dispatch(&user)==&user&&!active);
    assert(count==4&&events[0]==ENTER&&events[1]==HANDLER&&events[2]==EOI&&events[3]==LEAVE);
    reset();stop=true;if(!setjmp(stopped)){interrupt_dispatch(&user);assert(0);}
    assert(count==4&&events[2]==EOI&&events[3]==LEAVE&&!active); // cannot skip EOI
    reset();interrupt_dispatch(&kernel);assert(count==2&&events[0]==HANDLER&&events[1]==EOI);
    reset();nested=true;interrupt_dispatch(&user);
    assert(count==6&&events[0]==ENTER&&events[5]==LEAVE&&!active); // no double entry on nested kernel IRQ
    reset();cpu_slot=1;user.vector=0xf2;interrupt_dispatch(&user);
    assert(count==2&&events[0]==HANDLER&&events[1]==EOI&&!active);
    reset();cpu_slot=1;user.vector=14;interrupt_dispatch(&user);
    assert(count==4&&events[0]==ENTER&&events[3]==LEAVE&&!active);
    user.vector=0;
    for(unsigned linux=0;linux<2;linux++){
        reset();proc.linux_abi=linux;syscall_dispatch(&user);
        assert(user.rax==123&&dispatches==1&&!active&&!irq_on&&count==5);
        assert(events[0]==ENTER&&events[1]==ENABLE&&events[2]==DISPATCH&&events[3]==MASK&&events[4]==LEAVE);
    }
    reset();stop=true;if(!setjmp(stopped)){syscall_dispatch(&user);assert(0);}
    assert(!dispatches&&count==2&&events[0]==ENTER&&events[1]==LEAVE&&!irq_on);
    reset();current=NULL;syscall_dispatch(&user);assert(user.rax==(u64)-E_PERM&&!count);
    puts("PASS production entry wrappers: user entry spans handler/EOI, nested kernel IRQ excluded, native/Linux syscall bracketing and pre-dispatch stop; privileged entry/iret not executed");
}
'''
if __name__ == '__main__':
    run_test(code, 'kernel-entry-lifecycle')
