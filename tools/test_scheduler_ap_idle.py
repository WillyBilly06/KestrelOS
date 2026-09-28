#!/usr/bin/env python3
"""Exercise production AP idle preparation with host memory/queue adapters.

This proves reservation/refusal and slot ownership, not AP stack switching or
application execution on another CPU.
"""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/sched.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
#define PROC_MAX 8
#define ACPI_MAX_CPUS 4
#define PROC_CPU_NONE UINT32_MAX
#define KSTACK_SIZE (64 * 1024)
#define PAGE_SIZE 4096
#define PROC_SETUP 1
typedef struct {
    u32 state,running_cpu;bool is_kernel,is_idle;
    u64 kstack,kstack_top;char name[32];
} proc_t;
static proc_t procs[PROC_MAX],application_idle[ACPI_MAX_CPUS];
'''
start = src.index('typedef struct {\n    proc_t *running;')
code += src[start:src.index('} __attribute__((aligned(64))) sched_cpu_t;',start)+len('} __attribute__((aligned(64))) sched_cpu_t;')]
code += r'''
static sched_cpu_t sched_cpus[ACPI_MAX_CPUS];
static u64 saved_rsp[PROC_MAX+ACPI_MAX_CPUS];
static bool sched_running=true,online[ACPI_MAX_CPUS],fail_alloc,queue_owned,race_publish;
static u32 active_cpu,allocations,releases;
static u32 race_slot;
bool sched_prepare_application_cpu(u32 slot);
static u32 cpu_current_slot(void){return active_cpu;}
static bool smp_worker_slot_online(u32 slot){return slot<ACPI_MAX_CPUS&&online[slot];}
static u64 pmm_alloc_pages(size_t pages){
    assert(pages==KSTACK_SIZE/PAGE_SIZE);if(fail_alloc)return 0;
    if(race_publish){race_publish=false;assert(sched_prepare_application_cpu(race_slot));}
    void*p=malloc(pages*PAGE_SIZE);assert(p);allocations++;return (u64)p;
}
static void*phys_to_virt(u64 phys){return (void*)phys;}
static void pmm_free_pages(u64 phys,size_t pages){
    assert(pages==KSTACK_SIZE/PAGE_SIZE);free((void*)phys);releases++;
}
static bool queue_lock(void){assert(!queue_owned);queue_owned=true;return true;}
static void queue_unlock(bool prior){assert(queue_owned&&prior);queue_owned=false;}
static size_t strlcpy(char*dst,const char*src,size_t cap){
    size_t n=strlen(src);if(cap){size_t m=n<cap-1?n:cap-1;memcpy(dst,src,m);dst[m]=0;}return n;
}
__attribute__((noreturn)) static void panic(const char*why){fprintf(stderr,"%s\n",why);abort();}
'''
code += function(src, 'sched_application_cpu_prepared') + '\n'
start = src.index('static inline int slot_of(')
code += src[start:src.index('\n}',start)+2] + '\n'
code += function(src, 'sched_prepare_application_cpu') + '\n'
code += r'''
int main(void){
    online[1]=online[2]=true;
    assert(!sched_prepare_application_cpu(0));
    assert(!sched_prepare_application_cpu(ACPI_MAX_CPUS));
    assert(!sched_prepare_application_cpu(3));
    active_cpu=1;assert(!sched_prepare_application_cpu(1));active_cpu=0;
    sched_running=false;assert(!sched_prepare_application_cpu(1));sched_running=true;
    fail_alloc=true;assert(!sched_prepare_application_cpu(1));fail_alloc=false;
    assert(!allocations&&!releases&&!sched_application_cpu_prepared(1));
    assert(sched_prepare_application_cpu(1));
    assert(sched_application_cpu_prepared(1));
    assert(sched_cpus[1].idle_task==&application_idle[1]);
    assert(sched_cpus[1].running==NULL&&!sched_cpus[1].application_online);
    assert(application_idle[1].state==PROC_SETUP&&application_idle[1].is_idle&&
           application_idle[1].is_kernel&&application_idle[1].running_cpu==PROC_CPU_NONE);
    assert(application_idle[1].kstack_top-application_idle[1].kstack==KSTACK_SIZE);
    assert(!strcmp(application_idle[1].name,"ap-idle"));
    assert(slot_of(&procs[3])==3&&slot_of(&application_idle[1])==PROC_MAX+1);
    assert(saved_rsp[slot_of(&application_idle[1])]==0);
    assert(sched_prepare_application_cpu(1)&&allocations==1&&!releases);
    assert(sched_prepare_application_cpu(2)&&allocations==2&&!releases);
    assert(application_idle[1].kstack!=application_idle[2].kstack);
    assert(slot_of(&application_idle[2])==PROC_MAX+2);
    sched_cpus[2].application_online=1;
    assert(!sched_application_cpu_prepared(2));
    assert(!sched_prepare_application_cpu(2)&&allocations==2);
    online[3]=true;race_slot=3;race_publish=true;
    assert(!sched_prepare_application_cpu(3));
    assert(sched_application_cpu_prepared(3));
    assert(allocations==4&&releases==1);
    for(unsigned i=0;i<PROC_MAX;i++)assert(!procs[i].state);
    assert(!queue_owned);
    puts("PASS AP idle preparation: private stacks/indices, reserved state, duplicate and OOM refusal, boot lifetime, zero consumed process slots; no AP execution");
    return 0;
}
'''
run_test(code, 'scheduler-ap-idle')
