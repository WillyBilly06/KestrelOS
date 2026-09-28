#!/usr/bin/env python3
"""Production bounded user-copy helper, with unmap at preemption boundaries."""
from test_gpu_stable_candidate import ROOT, function, run_test
import re


def main():
    src = (ROOT / 'kernel/syscall.c').read_text()
    bound = re.search(r'^#define VMM_USER_COPY_MAX\s+[^\n]+', (ROOT / 'kernel/mm.h').read_text(), re.M)
    assert bound
    c = bound[0] + '\n' + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;typedef uint8_t u8;
static u8 user[65537],snapshot[65537];
static bool irq_enabled=true,mapped=true,writable=true;
static unsigned regions,restores,revoke_at;
static bool irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void irq_restore(bool old){
    assert(!irq_enabled);irq_enabled=old;restores++;
    if(old&&revoke_at==restores)mapped=false;
}
static bool vm_range_locked(u64 a,size_t n,bool write){
    assert(!irq_enabled&&n>0&&n<=VMM_USER_COPY_MAX);regions++;
    u64 start=(uintptr_t)user;
    return mapped&&(!write||writable)&&a>=start&&a-start<=sizeof user&&n<=sizeof user-(a-start);
}
/* The VM helpers now own the lock/IRQ boundary, not gpu_user_access. */
static bool user_range_ok(u64 a,size_t n,bool write){
    bool old=irq_save(),valid=vm_range_locked(a,n,write);
    irq_restore(old);return valid;
}
static bool user_copy(void *buffer,u64 a,size_t n,bool write){
    bool old=irq_save(),valid=vm_range_locked(a,n,write);
    if(valid){if(write)memcpy((void *)(uintptr_t)a,buffer,n);
              else memcpy(buffer,(void *)(uintptr_t)a,n);}
    irq_restore(old);return valid;
}
'''
    c += function(src, 'gpu_user_access') + r'''
static void reset(void){
    irq_enabled=mapped=writable=true;regions=restores=revoke_at=0;
    memset(user,0xa5,sizeof user);memset(snapshot,0x39,sizeof snapshot);
}
int main(void){
    u64 address=(uintptr_t)user;
    reset();assert(gpu_user_access(NULL,address,sizeof user,false));assert(regions==5&&irq_enabled);
    assert(user[0]==0xa5&&snapshot[0]==0x39);
    reset();assert(gpu_user_access(snapshot,address,sizeof user,false));assert(!memcmp(user,snapshot,sizeof user)&&regions==5);
    reset();assert(gpu_user_access(snapshot,address,sizeof user,true));assert(!memcmp(user,snapshot,sizeof user)&&regions==5);
    reset();revoke_at=1;
    assert(!gpu_user_access(snapshot,address,sizeof user,false)&&regions==2&&irq_enabled);
    for(unsigned i=0;i<sizeof snapshot;i++)assert(snapshot[i]==(i<16384?0xa5:0x39));
    reset();revoke_at=2;
    assert(!gpu_user_access(snapshot,address,sizeof user,true)&&regions==3&&irq_enabled);
    for(unsigned i=0;i<sizeof user;i++)assert(user[i]==(i<32768?0x39:0xa5));
    reset();writable=false;assert(!gpu_user_access(snapshot,address,100,true)&&user[0]==0xa5);
    assert(gpu_user_access(snapshot,address,100,false));
    reset();irq_enabled=false;assert(gpu_user_access(snapshot,address,100,false)&&!irq_enabled);
    reset();assert(!gpu_user_access(snapshot,UINT64_MAX-4,8,false)&&!regions);
    assert(gpu_user_access(snapshot,address,0,false)&&!regions);
    assert(!gpu_user_access(snapshot,address+sizeof user,1,false));
    puts("PASS actual GPU user-copy helper: 16KiB IRQ bounds, preflight/read/write, revoked mapping between chunks, readonly output, wrap/bounds and IRQ-state restoration");
}
'''
    run_test(c, 'gpu_user_access')


if __name__ == '__main__':
    main()
