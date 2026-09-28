#!/usr/bin/env python3
"""Exercise production AP admission reservation with asynchronous workers."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/smp.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define ACPI_MAX_CPUS 8
#define SMP_WAKE_VECTOR 0xf0
typedef struct {u32 apic_id,online,promote_requested;} worker_state_t;
typedef struct {bool held;} spinlock_t;
static worker_state_t worker_state[ACPI_MAX_CPUS];
static spinlock_t batch_lock;
static u32 workers_online,ipis;
static u32 cpu_current_slot(void){return 0;}
static bool spin_lock_irqsave(spinlock_t *l){assert(!l->held);l->held=true;return true;}
static void spin_unlock_irqrestore(spinlock_t *l,bool irq){assert(l->held&&irq);l->held=false;}
static bool smp_worker_slot_online(u32 slot){return worker_state[slot].online;}
static bool sched_application_cpu_prepared(u32 slot){return slot<5;}
static void lapic_send_ipi(u32 apic,u32 vec){assert(apic&&vec==SMP_WAKE_VECTOR&&!batch_lock.held);ipis++;}
'''
code += function(src, 'smp_promote_one_application_cpu') + r'''
int main(void){
    for(u32 s=1;s<=4;s++){worker_state[s].online=1;worker_state[s].apic_id=s;}
    workers_online=4;
    assert(smp_promote_one_application_cpu());
    assert(smp_promote_one_application_cpu());
    assert(smp_promote_one_application_cpu());
    assert(!smp_promote_one_application_cpu());
    assert(ipis==3&&worker_state[4].online&&!worker_state[4].promote_requested);
    worker_state[1].online=0;workers_online--;
    assert(!smp_promote_one_application_cpu());
    worker_state[2].online=0;workers_online--;
    worker_state[3].online=0;workers_online--;
    assert(workers_online==1&&!smp_promote_one_application_cpu());
    puts("PASS production AP promotion: in-flight reservations retain one restricted worker across asynchronous admission");
}
'''
run_test(code, 'smp-promotion-reservation')
