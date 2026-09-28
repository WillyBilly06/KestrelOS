#!/usr/bin/env python3
"""Check production idle scheduling with conservative, serial ownership mocks.

--compile-only builds the extracted production functions without running them.
This scaffold does not exercise context-switch assembly, device probes, multiple
simultaneous CPUs, or prove concurrency correctness.
"""
import argparse
from test_gpu_stable_candidate import ROOT, run_test


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    src = (ROOT / 'kernel/sched.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct {int marker;} regs_t;
enum {PROC_UNUSED,PROC_SLEEPING,PROC_READY,PROC_RUNNING,PROC_BLOCKED};
typedef struct {int state;u64 wake_at_ms,cpu_ms;u32 running_cpu;bool is_idle,is_kernel,kernel_active,stop_requested;} proc_t;
#define PROC_MAX 4
#define PROC_CPU_NONE UINT32_MAX
#define ACPI_MAX_CPUS 2
static proc_t procs[PROC_MAX];
static u32 active_cpu;
static bool interrupts_enabled=true;
static u64 g_uptime_ms,g_cpu_idle_ms,g_cpu_busy_ms;
static unsigned switches;
static unsigned reschedule_ipis;
static u32 last_ipi_target;
static void smp_reschedule_cpu(u32 slot){reschedule_ipis++;last_ipi_target=slot;}
static void retire_stopped_threads(void){} /* stop lifecycle tested separately */
static u32 cpu_current_slot(void){return active_cpu;}
#define panic(message) do {(void)(message);assert(!"unexpected scheduler panic");} while(0)
'''
    start = src.index('typedef struct {\n    proc_t *running;')
    end = src.index('\n}', src.index('static bool scheduler_local_online(', start)) + 2
    c += src[start:end] + r'''
static int slot_of(proc_t *p){assert(p>=procs&&p<procs+PROC_MAX);return (int)(p-procs);}
static void irq_restore(bool enabled){interrupts_enabled=enabled;}
static bool queue_lock(void){
    sched_cpu_t *local=scheduler_local();
    assert(!local->queue_locked&&!local->switch_from);
    bool prior=interrupts_enabled;interrupts_enabled=false;
    local->queue_locked=true;return prior;
}
static void queue_unlock(bool prior){
    sched_cpu_t *local=scheduler_local();
    assert(local->queue_locked&&!local->switch_from&&!interrupts_enabled);
    local->queue_locked=false;irq_restore(prior);
}
/* Device-probe behavior has its own scaffold. No device waiters here. */
static proc_t *poll_device_waiters(void){
    assert(scheduler_local()->queue_locked);return NULL;
}
/* Serial completion stand-in only. Preserve ownership until the simulated
 * incoming context completes its handoff and releases the runqueue lock. */
static void switch_to(proc_t *next){
    sched_cpu_t *local=scheduler_local();
    proc_t *prev=current;
    assert(scheduler_local_online()&&next&&prev);
    assert(local->queue_locked&&!local->switch_from&&!interrupts_enabled);
    assert(prev->running_cpu==active_cpu);
    if(next==prev){prev->state=PROC_RUNNING;queue_unlock(false);return;}
    assert(next->running_cpu==PROC_CPU_NONE);
    assert(next->state==PROC_READY||next->is_idle);
    local->switch_from=prev;next->running_cpu=active_cpu;
    next->state=PROC_RUNNING;current=next;switches++;
    if(prev->state==PROC_RUNNING)prev->state=PROC_READY;
    prev->running_cpu=PROC_CPU_NONE;local->switch_from=NULL;
    queue_unlock(false);
}
static void enter_idle(void){
    assert(!scheduler_local()->queue_locked&&!scheduler_local()->switch_from);
    assert(current->running_cpu==active_cpu);
    current->running_cpu=PROC_CPU_NONE;
    current=idle;idle->running_cpu=active_cpu;idle->state=PROC_RUNNING;
}
'''
    start = src.index('static u32 idle_wakeup_target(')
    c += src[start:src.index('\n}', start)+2] + '\n'
    start = src.index('static void wake_idle_cpu(')
    c += src[start:src.index('\n}', start)+2] + '\n'
    start = src.index('static proc_t *pick_next(')
    predicate = src.index('static bool sched_can_run_on_cpu(')
    c += src[predicate:src.index('\n}', predicate)+2] + '\n'
    c += src[start:src.index('\n}', start)+2] + '\n'
    start = src.index('void sched_wake(')
    c += src[start:src.index('\n}', start)+2] + '\n'
    start = src.index('regs_t *sched_tick(')
    c += src[start:src.index('\n}', start)+2] + '\n'
    start = src.index('regs_t *sched_reschedule(')
    c += src[start:src.index('\n}', start)+2] + r'''
int main(void){
    regs_t r={123};
    memset(procs,0,sizeof procs);
    for(unsigned i=0;i<PROC_MAX;i++)procs[i].running_cpu=PROC_CPU_NONE;
    sched_cpus[0].running=sched_cpus[0].idle_task=&procs[0];
    sched_cpus[0].application_online=1;
    idle->is_idle=true;idle->state=PROC_RUNNING;idle->running_cpu=0;
    procs[1]=(proc_t){.state=PROC_SLEEPING,.wake_at_ms=1,.running_cpu=PROC_CPU_NONE};
    g_uptime_ms=1;assert(sched_tick(&r)==&r);
    assert(current==&procs[1]&&switches==1&&g_cpu_idle_ms==1);
    assert(!scheduler_local()->queue_locked&&interrupts_enabled);
    assert(idle->running_cpu==PROC_CPU_NONE&&current->running_cpu==0);
    // Runnable publication sees an enrolled idle remote CPU and sends one
    // scheduler nudge. Repeated wake on an already-ready task sends none.
    sched_cpus[1].application_online=1;
    sched_cpus[1].idle_task=sched_cpus[1].running=&procs[3];
    procs[2].state=PROC_SLEEPING;
    sched_wake(&procs[2]);
    assert(procs[2].state==PROC_READY&&reschedule_ipis==1&&last_ipi_target==1);
    sched_wake(&procs[2]);assert(reschedule_ipis==1);
    sched_cpus[1].application_online=0;
    sched_cpus[1].running=sched_cpus[1].idle_task=NULL;
    procs[2].state=PROC_READY;
    for(g_uptime_ms=2;g_uptime_ms<=10;g_uptime_ms++)sched_tick(&r);
    assert(current==&procs[1]&&switches==1&&g_cpu_busy_ms==9);
    sched_tick(&r);assert(g_uptime_ms==11&&current==&procs[2]&&switches==2);
    // A blocked task made runnable by input/device wake resumes on the very
    // next idle tick, despite the previous switch being just one tick ago.
    enter_idle();procs[1].state=PROC_UNUSED;procs[2].state=PROC_READY;
    g_uptime_ms=12;sched_tick(&r);assert(current==&procs[2]&&switches==3);
    // No runnable task: stay idle, don't invent work or count it as CPU busy.
    enter_idle();procs[2].state=PROC_UNUSED;g_uptime_ms=13;sched_tick(&r);
    assert(current==idle&&switches==3&&g_cpu_idle_ms==3);
    // A slot still owned by another CPU must not be selected, even if ready.
    procs[3].state=PROC_READY;procs[3].running_cpu=1;
    bool prior=queue_lock();assert(pick_next()==idle);queue_unlock(prior);
    assert(procs[3].running_cpu==1);
    // A remote wake interrupt dispatches READY work immediately, without
    // inventing another millisecond of CPU usage or advancing a time slice.
    procs[2].state=PROC_READY;
    u64 before_busy=g_cpu_busy_ms,before_idle=g_cpu_idle_ms;
    assert(sched_reschedule(&r)==&r);
    assert(current==&procs[2]&&switches==4);
    assert(g_cpu_busy_ms==before_busy&&g_cpu_idle_ms==before_idle);
    enter_idle();procs[2].state=PROC_UNUSED;
    // An unenrolled AP must neither borrow BSP identity nor enter its runqueue.
    active_cpu=1;u64 busy=g_cpu_busy_ms,idle_time=g_cpu_idle_ms;
    assert(sched_tick(&r)==&r&&current==NULL);
    assert(sched_reschedule(&r)==&r&&current==NULL);
    assert(g_cpu_busy_ms==busy&&g_cpu_idle_ms==idle_time);
    assert(!scheduler_local()->queue_locked&&interrupts_enabled);
    // AP selection admits user compute but refuses parked kernel continuations,
    // kernel threads and stopped contexts. No AP user entry is attempted here.
    sched_cpus[1].application_online=1;
    sched_cpus[1].running=sched_cpus[1].idle_task=&procs[3];
    procs[3]=(proc_t){.state=PROC_RUNNING,.is_idle=true,.running_cpu=1};
    procs[1]=(proc_t){.state=PROC_READY,.running_cpu=PROC_CPU_NONE,.kernel_active=true};
    procs[2]=(proc_t){.state=PROC_READY,.running_cpu=PROC_CPU_NONE};
    prior=queue_lock();assert(pick_next()==&procs[2]);queue_unlock(prior);
    procs[2].stop_requested=true;
    prior=queue_lock();assert(pick_next()==idle);queue_unlock(prior);
    procs[1].kernel_active=false;procs[1].is_kernel=true;
    prior=queue_lock();assert(pick_next()==idle);queue_unlock(prior);
    sched_cpus[1].application_online=0;
    sched_cpus[1].running=sched_cpus[1].idle_task=NULL;
    active_cpu=0;sched_cpus[0].application_online=0;g_uptime_ms=14;sched_tick(&r);
    assert(current==idle&&switches==4);
    assert(!scheduler_local()->queue_locked&&interrupts_enabled);
    puts("PASS scheduler tick/reschedule with serial ownership mocks: idle wake, no false tick charge, busy 10ms fairness, ownership exclusion and offline-CPU guard; not a concurrency proof");
}
'''
    run_test(c, 'scheduler_idle_wake', compile_only=args.compile_only)


if __name__ == '__main__':
    main()
