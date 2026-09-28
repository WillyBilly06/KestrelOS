#!/usr/bin/env python3
"""Run extracted production process-report code with instrumented lock edges.

CPU-only serial model: validates copied fields and lock/IRQ contracts, not real
parallel execution or kernel scheduling. --compile-only skips the assertions.
"""
import sys
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    sched = (ROOT / 'kernel/sched.c').read_text()
    header = (ROOT / 'kernel/proc.h').read_text()
    report_end = header.index('} proc_report_t;') + len('} proc_report_t;')
    report_start = header.rfind('typedef struct {', 0, report_end)
    assert report_start >= 0
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64;
#define PROC_MAX 64
#define PROC_NAME_MAX 32
enum {PROC_UNUSED,PROC_SETUP,PROC_READY,PROC_RUNNING,PROC_SLEEPING,PROC_BLOCKED,PROC_ZOMBIE,PROC_DYING};
typedef struct proc {
    int pid,parent_pid,state;
    char name[PROC_NAME_MAX];
    u64 cpu_ms,image_base,image_end;
    struct proc *leader;
} proc_t;
static proc_t procs[PROC_MAX];
static bool irq_on=true,queue_owned;
static unsigned acquires,releases;
static u32 *paired_processes,*paired_threads,expected_processes,expected_threads;
static bool queue_lock(void){
    assert(!queue_owned);bool saved=irq_on;irq_on=false;queue_owned=true;acquires++;return saved;
}
static void queue_unlock(bool saved){
    assert(queue_owned && !irq_on);
    if(paired_processes){assert(*paired_processes==expected_processes);assert(*paired_threads==expected_threads);}
    queue_owned=false;irq_on=saved;releases++;
}
static void *report_memcpy(void *dst,const void *src,size_t n){
    assert(queue_owned && !irq_on);return memcpy(dst,src,n);
}
static void *report_memset(void *dst,int value,size_t n){
    assert(queue_owned && !irq_on);return memset(dst,value,n);
}
'''
    c += header[report_start:report_end] + '\n'
    c += '#define memcpy report_memcpy\n#define memset report_memset\n'
    for name in ('proc_count_snapshot', 'proc_report_snapshot', 'proc_count', 'proc_thread_count'):
        c += function(sched, name) + '\n'
    c += '#undef memcpy\n#undef memset\n'
    c += r'''
static void reset(void){
    assert(!queue_owned);memset(procs,0,sizeof procs);
    irq_on=true;acquires=releases=0;paired_processes=paired_threads=NULL;
}
static void balanced(bool expected_irq,unsigned calls){
    assert(!queue_owned && irq_on==expected_irq && acquires==calls && releases==calls);
}
int main(void){
    proc_report_t row,old;
    u32 processes=99,threads=99;
    reset();memset(&row,0xa5,sizeof row);old=row;
    assert(!proc_report_snapshot(0,&row) && !memcmp(&row,&old,sizeof row));
    balanced(true,1);
    assert(!proc_report_snapshot(PROC_MAX,&row));
    assert(!proc_report_snapshot(UINT32_MAX,&row));
    assert(!proc_report_snapshot(0,NULL));balanced(true,1); // fast failures take no lock
    proc_count_snapshot(&processes,&threads);assert(processes==0 && threads==0);
    balanced(true,2);

    reset();
    procs[1]=(proc_t){.pid=101,.parent_pid=10,.state=PROC_READY,.cpu_ms=55,.image_base=0x1000,.image_end=0x3000};
    memcpy(procs[1].name,"leader",7);
    procs[3]=(proc_t){.pid=102,.parent_pid=10,.state=PROC_BLOCKED,.leader=&procs[1],.cpu_ms=9};
    memset(procs[3].name,'T',sizeof procs[3].name); // malformed source still yields terminated report
    procs[5]=(proc_t){.pid=103,.parent_pid=11,.state=PROC_SETUP,.image_base=1,.image_end=UINT64_MAX};
    procs[7]=(proc_t){.pid=104,.parent_pid=12,.state=PROC_ZOMBIE,.image_base=0x4000,.image_end=0x2000};
    paired_processes=&processes;paired_threads=&threads;expected_processes=3;expected_threads=4;
    proc_count_snapshot(&processes,&threads);balanced(true,1); // both outputs set before one unlock
    paired_processes=paired_threads=NULL;
    assert(proc_count()==3 && proc_thread_count()==4);balanced(true,3);
    assert(proc_report_snapshot(0,&row));
    assert(row.pid==101 && row.parent_pid==10 && row.state==PROC_READY && row.cpu_ms==55);
    assert(row.image_bytes==0x2000 && !strcmp(row.name,"leader"));
    old=row;memset(&procs[1],0,sizeof procs[1]);
    procs[1]=(proc_t){.pid=900,.state=PROC_RUNNING,.cpu_ms=999};
    assert(!memcmp(&row,&old,sizeof row)); // source destruction/reuse cannot alter copied record
    assert(proc_report_snapshot(0,&row) && row.pid==900 && row.cpu_ms==999);
    assert(proc_report_snapshot(1,&row) && row.pid==102 && row.name[31]==0);
    for(unsigned i=0;i<31;i++)assert(row.name[i]=='T');
    assert(proc_report_snapshot(2,&row) && row.pid==103 && row.state==PROC_SETUP && row.image_bytes==0);
    assert(proc_report_snapshot(3,&row) && row.pid==104 && row.image_bytes==0);
    old=row;assert(!proc_report_snapshot(4,&row) && !memcmp(&row,&old,sizeof row));
    balanced(true,9);
    procs[8]=(proc_t){.pid=105,.state=PROC_DYING,.leader=&procs[1]};
    assert(proc_report_snapshot(4,&row) && row.state==PROC_DYING);
    proc_count_snapshot(&processes,&threads);assert(processes==3 && threads==5);
    irq_on=false;assert(proc_report_snapshot(1,&row));balanced(false,12);
    proc_count_snapshot(NULL,&threads);assert(threads==5);balanced(false,13);
    proc_count_snapshot(&processes,NULL);assert(processes==3);balanced(false,14);
    proc_count_snapshot(NULL,NULL);balanced(false,15);

    reset();for(unsigned i=0;i<PROC_MAX;i++){procs[i].state=PROC_READY;procs[i].pid=(int)i+1000;}
    for(unsigned i=0;i<PROC_MAX;i++)assert(proc_report_snapshot(i,&row) && row.pid==(int)i+1000);
    proc_count_snapshot(&processes,&threads);assert(processes==PROC_MAX && threads==PROC_MAX);
    balanced(true,PROC_MAX+1);
    puts("PASS process snapshots: bounded indexing, paired counts, setup suppression, copied identity, name termination and lock/IRQ contracts");
}
'''
    run_test(c, 'process-snapshots', compile_only='--compile-only' in sys.argv)


if __name__ == '__main__':
    main()
