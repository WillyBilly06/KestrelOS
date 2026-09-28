#!/usr/bin/env python3
"""Production boot enrollment, stack construction and selection; not SMP/USB proof."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT / 'kernel/sched.c').read_text()
    idle_body = function(src, 'scheduler_idle_thread') if 'static void scheduler_idle_thread(' in src else ''
    if idle_body:
        assert '"sti; hlt"' in idle_body
        assert 'klog_persist_flush(' not in idle_body and 'sched_sleep_ms(' not in idle_body
        boot_tail = (ROOT / 'kernel/main.c').read_text().rsplit('for (;;) {', 1)[1]
        assert 'sched_sleep_ms(250);' in boot_tail and 'hlt();' not in boot_tail
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
/* REGS_DEFINITION */
enum {PROC_UNUSED,PROC_SETUP,PROC_SLEEPING,PROC_READY,PROC_RUNNING,PROC_BLOCKED};
typedef struct {
    int pid,state,waiting_for; u32 running_cpu; bool is_idle,is_kernel,kernel_active,stop_requested;
    u64 wake_at_ms,pml4,kstack,kstack_top; char name[32],cwd[32]; regs_t *frame;
} proc_t;
#define PROC_MAX 8
#define ACPI_MAX_CPUS 2
#define PROC_CPU_NONE UINT32_MAX
#define BOOT_STACK_BYTES (32*1024)
#define KSTACK_SIZE (64*1024)
#define PAGE_SIZE 4096
#define SEL_KCODE 8
#define SEL_KDATA 16
static proc_t procs[PROC_MAX];
static u64 saved_rsp[PROC_MAX];
static bool sched_running,irq_enabled=true;
static u64 g_uptime_ms;
u64 g_boot_stack=0x100000;
static u32 cpu_current_slot(void){return 0;}
#define panic(s) do {fprintf(stderr,"%s\n",s);assert(!"unexpected panic");} while(0)
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
#define kerr(...) ((void)0)
/* AP reservation is exercised with allocation/refusal in test_scheduler_ap_idle.py. */
static bool smp_worker_slot_online(u32 slot){(void)slot;return false;}
static bool sched_prepare_application_cpu(u32 slot){(void)slot;assert(0);return false;}
static bool smp_promote_one_application_cpu(void){return false;}
static u32 sched_application_cpu_count(void){return 1;}
static int smp_worker_count(void){return 0;}
static void timer_mdelay(u32 ms){(void)ms;assert(0);}
static void strlcpy(char *d,const char *s,size_t n){snprintf(d,n,"%s",s);}
static bool irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void irq_restore(bool old){irq_enabled=old;}
static void tss_set_rsp0(u64 sp){assert(sp==g_boot_stack+BOOT_STACK_BYTES);}
/* Never execute the privileged idle entry on the host. */
static void scheduler_idle_thread(void *unused){(void)unused;}
static void thread_bootstrap(void){}
static void retire_stopped_threads(void){} /* stop lifecycle tested separately */
static bool allocation_fails;
static _Alignas(PAGE_SIZE) u8 stack_memory[KSTACK_SIZE+2*PAGE_SIZE];
static u64 pmm_alloc_pages(size_t pages){
    assert(pages==KSTACK_SIZE/PAGE_SIZE);
    return allocation_fails ? 0 : (u64)(stack_memory+PAGE_SIZE);
}
static void *phys_to_virt(u64 address){return (void *)address;}
'''
    start = src.index('typedef struct {\n    proc_t *running;')
    end = src.index('\n}', src.index('static bool scheduler_local_online(', start)) + 2
    c += src[start:end] + r'''
static proc_t *proc_alloc(const char *name){
    assert(!sched_running&&!irq_enabled);
    for(int i=0;i<PROC_MAX;i++)if(procs[i].state==PROC_UNUSED){
        procs[i].state=PROC_SETUP;procs[i].pid=i;procs[i].running_cpu=PROC_CPU_NONE;
        strlcpy(procs[i].name,name,sizeof procs[i].name);return &procs[i];
    }
    return NULL;
}
static void publish_ready(proc_t *p){assert(p->state==PROC_SETUP);p->state=PROC_READY;}
static int slot_of(proc_t *p){return (int)(p-procs);}
static proc_t *poll_device_waiters(void){return NULL;}
'''
    cpu = (ROOT / 'kernel/cpu.h').read_text()
    regs_start = cpu.index('typedef struct regs {')
    regs_end = cpu.index('} regs_t;', regs_start) + len('} regs_t;')
    c = c.replace('/* REGS_DEFINITION */', cpu[regs_start:regs_end])
    c += function(src, 'prepare_stack') + '\n'
    c += function(src, 'proc_init') + '\n'
    c += function(src, 'sched_can_run_on_cpu') + '\n'
    start = src.index('static proc_t *pick_next(')
    c += src[start:src.index('\n}', start)+2] + r'''
/* Model only ownership handoff, not machine-register switching. */
static void select_next(void){
    proc_t *prev=current,*next=pick_next();
    assert(next);
    if(next!=prev){
        assert(next->running_cpu==PROC_CPU_NONE);
        if(prev->state==PROC_RUNNING)prev->state=PROC_READY;
        prev->running_cpu=PROC_CPU_NONE;
        current=next;next->running_cpu=0;
    }
    next->state=PROC_RUNNING;
}
int main(void){
    memset(stack_memory,0xa5,sizeof stack_memory);
    proc_init();
    proc_t *boot=current;
    assert(boot==&procs[0]&&boot->state==PROC_RUNNING&&boot->running_cpu==0);
    proc_t *worker=&procs[2];
    *worker=(proc_t){.state=PROC_READY,.running_cpu=PROC_CPU_NONE};
    /* Boot owns the NVKMS lock and yields; its worker remains runnable while
     * yielding for that lock. The boot owner MUST get selected again. */
    select_next();assert(current==worker&&boot->state==PROC_READY);
    select_next();assert(current==boot);
    select_next();assert(current==worker);
    select_next();assert(current==boot);
    assert(boot!=idle&&!boot->is_idle&&idle->is_idle);
    assert(boot->kstack==g_boot_stack&&idle->kstack!=g_boot_stack);
    assert(idle->state==PROC_READY&&idle->running_cpu==PROC_CPU_NONE);
    assert(idle->frame->rsp==idle->kstack_top&&irq_enabled&&scheduler_local_online());
    /* Actual production stack layout, not the previous prepare_stack stub.
     * These are the pops/ret consumed by context_switch and the frame then
     * consumed by thread_bootstrap/isr_return. Privileged assembly isn't run. */
    assert(sizeof(regs_t)==176&&((u64)idle->frame%16)==0);
    u64 *saved=(u64 *)saved_rsp[slot_of(idle)];
    assert(saved[0]==0x002); /* IF stays clear until the first iretq */
    for(int i=1;i<=6;i++)assert(saved[i]==0);
    assert(saved[7]==(u64)thread_bootstrap);
    assert((void *)(saved+8)==(void *)idle->frame);
    assert(idle->frame->rip==(u64)scheduler_idle_thread);
    assert(idle->frame->cs==SEL_KCODE&&idle->frame->ss==SEL_KDATA);
    assert(idle->frame->rflags==0x202&&idle->frame->rdi==0);
    for(int i=0;i<PAGE_SIZE;i++){
        assert(stack_memory[i]==0xa5);
        assert(stack_memory[PAGE_SIZE+KSTACK_SIZE+i]==0xa5);
    }
    for(u8 *b=(u8 *)idle->kstack;b<(u8 *)saved;b++)assert(*b==0);
    allocation_fails=true;
    regs_t rejected={0};
    assert(!prepare_stack(&procs[7],&rejected));
    assert(procs[7].kstack==0&&saved_rsp[7]==0);
    allocation_fails=false;
    /* An actual blocked/sleeping boot must not be used as the idle fallback. */
    worker->state=PROC_BLOCKED;
    boot->state=PROC_SLEEPING;boot->wake_at_ms=100;g_uptime_ms=99;
    select_next();assert(current==idle&&boot->state==PROC_SLEEPING);
    assert(pick_next()==idle);
    g_uptime_ms=100;select_next();assert(current==boot);
    /* Real idle never takes an ordinary runnable worker's turn. */
    boot->state=PROC_BLOCKED;worker->state=PROC_READY;
    select_next();assert(current==worker);
    worker->state=PROC_BLOCKED;select_next();assert(current==idle);
    puts("PASS production boot enrollment/stack/selection: yielding lock owner resumes, real idle frame and stack bounds, allocation refusal, sleep deadline; no privileged assembly or SMP execution");
}
'''
    run_test(c, 'scheduler_boot_progress')


if __name__ == '__main__':
    main()
