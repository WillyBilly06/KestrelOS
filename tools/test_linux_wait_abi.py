#!/usr/bin/env python3
"""Production Linux clock/futex translation; scheduler/user-copy edges mocked."""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

abi = (ROOT / 'kernel/linux_abi.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef int32_t s32;typedef uint64_t u64;typedef int64_t s64;
typedef struct{s64 sec,nsec;}linux_timespec_t;
typedef struct{s64 sec,usec;}linux_timeval_t;
typedef struct{int unused;}proc_t;
enum {E_INVAL=22,E_FAULT=14,E_NOSYS=38,E_INTR=4,E_TIMEDOUT=110,SYS_SLEEP=999};
static volatile u64 g_uptime_ms;
static u64 boot_unix=1700000000;
static bool copy_failure,mutate_after_read;
static unsigned waits,wakes,sleeps,copies;
static u64 got_address,got_deadline,got_sleep;
static u32 got_value,got_mask;
static int got_count,wait_result;
static bool got_private;
static bool syscall_user_copy(void*b,u64 a,size_t n,bool out){
    copies++;if(copy_failure||a<4096)return false;
    assert(n==8||n==16);
    if(out)memcpy((void*)(uintptr_t)a,b,n);
    else {memcpy(b,(void*)(uintptr_t)a,n);if(mutate_after_read)memset((void*)(uintptr_t)a,0xff,n);}
    return true;
}
static int sched_futex_wait_ex(u64 a,u32 value,u64 deadline,u32 mask,bool private_key){
    waits++;got_address=a;got_value=value;got_deadline=deadline;got_mask=mask;got_private=private_key;return wait_result;
}
static int sched_futex_wake_ex(u64 a,int count,u32 mask,bool private_key){
    wakes++;got_address=a;got_count=count;got_mask=mask;got_private=private_key;
    return !mask||count<0?-E_INVAL:count;
}
static s64 syscall_native(proc_t*p,u64 call,u64 ms,u64 a1,u64 a2,u64 a3,u64 a4){
    (void)p;assert(call==SYS_SLEEP&&!a1&&!a2&&!a3&&!a4);sleeps++;got_sleep=ms;return 0;
}
'''
code += '\n'.join(re.findall(r'^#define (?:FUTEX_\w+|L_futex|L_clock_gettime|L_clock_getres|L_nanosleep|L_gettimeofday)\s+[^\n]+',abi,re.M))+'\n'
code += function((ROOT / 'kernel/timer.c').read_text(),'time_clock_snapshot')+'\n'
code += (ROOT / 'kernel/linux_wait.h').read_text()+'\n'
code += 'static s64 dispatch(u64 nr,u64 a0,u64 a1,u64 a2,u64 a3,u64 a5){proc_t*p=NULL;switch(nr){\n'
a = abi.index('    case L_nanosleep: {')
code += abi[a:abi.index('    /* What system this is.',a)]
a = abi.index('    /* Scoped keys, bitsets')
code += abi[a:abi.index('    /* A new thread.',a)]
code += 'default:return -E_NOSYS;}}\n'
code += r'''
static void reset(void){
    g_uptime_ms=12789;boot_unix=1700000000;copy_failure=mutate_after_read=false;
    waits=wakes=sleeps=copies=0;wait_result=0;got_deadline=got_sleep=0;
}
int main(void){
    reset();linux_timespec_t t={0};
    assert(!dispatch(L_clock_gettime,1,(u64)&t,0,0,0)&&t.sec==12&&t.nsec==789000000);
    assert(!dispatch(L_clock_gettime,0,(u64)&t,0,0,0)&&t.sec==1700000012&&t.nsec==789000000);
    assert(dispatch(L_clock_gettime,2,(u64)&t,0,0,0)==-E_INVAL); // not fake process CPU time
    assert(!dispatch(L_clock_getres,1,(u64)&t,0,0,0)&&!t.sec&&t.nsec==1000000);
    assert(!dispatch(L_clock_getres,0,0,0,0,0));
    assert(dispatch(L_clock_getres,2,0,0,0,0)==-E_INVAL);
    linux_timeval_t tv;s32 tz[2]={123,456};
    assert(!dispatch(L_gettimeofday,(u64)&tv,(u64)tz,0,0,0)&&tv.sec==1700000012&&tv.usec==789000&&!tz[0]&&!tz[1]);
    copy_failure=true;assert(dispatch(L_clock_gettime,1,(u64)&t,0,0,0)==-E_FAULT);
    assert(dispatch(L_nanosleep,(u64)&t,0,0,0,0)==-E_FAULT);

    reset();t=(linux_timespec_t){2,1500001};
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT|FUTEX_PRIVATE_FLAG,7,(u64)&t,0));
    assert(waits==1&&got_address==0x1000&&got_value==7&&got_deadline==14791&&got_mask==~0u&&got_private);
    reset();t=(linux_timespec_t){15,1500001};
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT_BITSET,9,(u64)&t,4));
    assert(got_deadline==15002&&got_mask==4&&!got_private);
    reset();t=(linux_timespec_t){1700000015,1500001};
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME,9,(u64)&t,2));
    assert(got_deadline==15002&&got_mask==2);
    t.sec=1700000010;
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME,9,(u64)&t,2)&&got_deadline==g_uptime_ms);
    reset();t=(linux_timespec_t){1,0};
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT_BITSET,9,(u64)&t,2)&&got_deadline==1000); // expired, NOT now+1 second
    reset();assert(!dispatch(L_futex,0x1000,FUTEX_WAIT,9,0,0)&&got_deadline==UINT64_MAX);
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT_BITSET,9,0,1)&&got_deadline==UINT64_MAX);
    assert(dispatch(L_futex,0x1000,FUTEX_WAIT_BITSET,9,0,0)==-E_INVAL&&waits==2);
    assert(dispatch(L_futex,0x1000,FUTEX_WAIT|FUTEX_CLOCK_REALTIME,9,0,1)==-E_NOSYS);
    assert(dispatch(L_futex,0x1000,FUTEX_WAKE|FUTEX_CLOCK_REALTIME,9,0,1)==-E_NOSYS);
    assert(dispatch(L_futex,0x1000,FUTEX_WAIT|512,9,0,1)==-E_NOSYS);
    assert(dispatch(L_futex,0x1000,127,9,0,1)==-E_NOSYS);
    reset();t=(linux_timespec_t){-1,0};assert(dispatch(L_futex,0x1000,FUTEX_WAIT,9,(u64)&t,1)==-E_INVAL&&!waits);
    t=(linux_timespec_t){0,-1};assert(dispatch(L_futex,0x1000,FUTEX_WAIT,9,(u64)&t,1)==-E_INVAL);
    t.nsec=1000000000;assert(dispatch(L_futex,0x1000,FUTEX_WAIT,9,(u64)&t,1)==-E_INVAL);
    copy_failure=true;assert(dispatch(L_futex,0x1000,FUTEX_WAIT,9,(u64)&t,1)==-E_FAULT);
    reset();t=(linux_timespec_t){INT64_MAX,999999999};
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT,9,(u64)&t,1)&&got_deadline==UINT64_MAX-1);
    reset();t=(linux_timespec_t){1,1};mutate_after_read=true;
    assert(!dispatch(L_futex,0x1000,FUTEX_WAIT,9,(u64)&t,1)&&got_deadline==13790&&t.sec==-1);
    wait_result=-E_INTR;assert(dispatch(L_futex,0x1000,FUTEX_WAIT,9,0,1)==-E_INTR);
    wait_result=-E_TIMEDOUT;assert(dispatch(L_futex,0x1000,FUTEX_WAIT,9,0,1)==-E_TIMEDOUT);
    reset();assert(dispatch(L_futex,0x1000,FUTEX_WAKE_BITSET|FUTEX_PRIVATE_FLAG,3,1,8)==3);
    assert(wakes==1&&got_count==3&&got_mask==8&&got_private&&!copies);
    assert(dispatch(L_futex,0x1000,FUTEX_WAKE,0,1,0)==0&&got_mask==~0u&&!got_private);
    assert(dispatch(L_futex,0x1000,FUTEX_WAKE_BITSET,1,0,0)==-E_INVAL);
    assert(dispatch(L_futex,0x1000,FUTEX_WAKE,UINT32_MAX,0,0)==-E_INVAL);
    reset();t=(linux_timespec_t){0,1};assert(!dispatch(L_nanosleep,(u64)&t,0,0,0,0)&&sleeps==1&&got_sleep==1);
    t=(linux_timespec_t){0,0};assert(!dispatch(L_nanosleep,(u64)&t,0,0,0,0)&&sleeps==1);
    t.sec=-1;assert(dispatch(L_nanosleep,(u64)&t,0,0,0,0)==-E_INVAL&&sleeps==1);
    u64 mono,real;boot_unix=UINT64_MAX;time_clock_snapshot(&mono,&real);assert(mono==12789&&real==UINT64_MAX);
    puts("PASS production Linux wait/time ABI: protected snapshots, relative versus absolute monotonic/realtime, bitset/private flags, invalid timeout/flags, no int truncation, finite saturation, error propagation, clock domains/resolution and bounded output; scheduler/user-copy mocked");
}
'''
if __name__ == '__main__':
    run_test(code, 'linux-wait-abi')
