#!/usr/bin/env python3
"""Execute direct-triangle syscall validation/snapshots with call spies.

No GPU or VM is accessed. The adapter spy mutates the original user vertices
and verifies it only received the kernel snapshot. VM-copy is a mocked boundary.
"""
from pathlib import Path
import re
from test_gpu_variant_safety import function
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'kernel/syscall.c').read_text()
    abi = (ROOT / 'include/kestrel/syscall.h').read_text()
    mm = (ROOT / 'kernel/mm.h').read_text()
    end = abi.index('} kvertex_t;') + len('} kvertex_t;')
    vertex = abi[abi.rfind('typedef struct {', 0, end):end]
    code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int64_t s64;
#define E_INVAL 22
#define E_NOSYS 38
#define E_NOMEM 12
'''+vertex+'\n'
    code += re.search(r'^#define VMM_USER_COPY_MAX\s+[^\n]+', mm, re.M)[0]+'\n'
    code += r'''
static kvertex_t vertices[128*3];
static unsigned char original[sizeof vertices];
static size_t allowed_bytes,requested_bytes;
static unsigned allocations,frees,checks,copies,draws;
static bool revoked,fail_alloc,revoke_on_alloc,driver_ready=true;
static int driver_result;
static void *allocation;
static bool user_range_ok(u64 address,size_t bytes,bool write) {
    checks++;assert(!write);
    uintptr_t first=(uintptr_t)vertices;
    return !revoked && address>=first && address-first<=allowed_bytes &&
           bytes<=allowed_bytes-(address-first);
}
static bool user_copy(void *buffer,u64 address,size_t bytes,bool write) {
    copies++;assert(!write&&buffer==allocation&&bytes<=VMM_USER_COPY_MAX);
    assert(bytes==requested_bytes);
    if(!user_range_ok(address,bytes,write))return false;
    memcpy(buffer,(const void *)(uintptr_t)address,bytes);return true;
}
static void *kmalloc(size_t bytes) {
    requested_bytes=bytes;
    if(fail_alloc)return NULL;
    assert(!allocation);allocation=malloc(bytes);assert(allocation);allocations++;
    if(revoke_on_alloc)revoked=true;
    return allocation;
}
static void kfree(void *p) {
    assert(p&&p==allocation);free(p);allocation=NULL;frees++;
}
static u32 gpu_accel_draw_mode(void) { return driver_ready?2:0; }
static int gpu_accel_draw_triangles(const float *data,u32 count) {
    draws++;assert(data==allocation && (const void *)data!=(const void *)vertices);
    size_t bytes=count*3*sizeof(kvertex_t);assert(bytes==requested_bytes);
    assert(!memcmp(data,original,bytes));
    // Simulate a sibling modifying and revoking the user mapping while a
    // backend waits for render ownership/completion. It cannot alter snapshot.
    memset(vertices,0xcc,sizeof vertices);revoked=true;
    assert(!memcmp(data,original,bytes));
    return driver_result;
}
'''
    code += function(source, 'gpu_triangle_request')+'\n'
    start = source.index('        case GPUOP_DRAW: {')
    end = source.index('\n        }', start)+len('\n        }')
    arm = source[start:end]
    assert 'gpu_triangle_request(a1, a2)' in arm
    code += '#define GPUOP_DRAW 3\nstatic s64 dispatch(u64 operation,u64 a1,u64 a2) { switch(operation) {\n'
    code += arm + '\ndefault: return -E_NOSYS; } }\n'
    code += r'''
static void reset(void) {
    assert(!allocation&&allocations==frees);
    memset(vertices,0x3e,sizeof vertices);memcpy(original,vertices,sizeof vertices);
    allowed_bytes=sizeof vertices;requested_bytes=0;allocations=frees=checks=copies=draws=0;
    revoked=fail_alloc=revoke_on_alloc=false;driver_ready=true;driver_result=1;
}
int main(void) {
    for(u32 count=1;count<=128;count++) {
        reset();driver_result=(int)count;
        assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,count)==(s64)count);
        assert(draws==1&&copies==1&&allocations==1&&frees==1&&!allocation);
        assert(requested_bytes==count*3*sizeof(kvertex_t));
    }
    const u64 invalid_counts[]={0,129,UINT32_MAX,0x100000001ull,UINT64_MAX};
    for(size_t i=0;i<sizeof invalid_counts/sizeof invalid_counts[0];i++) {
        reset();assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,invalid_counts[i])==-E_INVAL);
        assert(!checks&&!allocations&&!draws);
    }
    reset();allowed_bytes=3*sizeof(kvertex_t)-1;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,1)==-E_INVAL&&!allocations&&!draws);
    reset();assert(dispatch(GPUOP_DRAW,0,1)==-E_INVAL&&!draws);
    reset();assert(dispatch(GPUOP_DRAW,UINT64_MAX-15,1)==-E_INVAL&&!draws);
    reset();driver_ready=false;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,1)==-E_NOSYS&&!allocations&&!draws);
    reset();fail_alloc=true;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,1)==-E_NOMEM&&!copies&&!draws&&!frees);
    reset();revoke_on_alloc=true;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,1)==-E_INVAL&&copies==1&&!draws&&allocations==frees);
    reset();driver_result=-1;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,1)==-E_NOSYS&&draws==1&&allocations==frees);
    reset();driver_result=2;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,3)==2&&draws==1&&allocations==frees);
    reset();driver_result=0;
    assert(dispatch(GPUOP_DRAW,(uintptr_t)vertices,3)==0&&draws==1&&allocations==frees);
    puts("PASS direct triangles: 1..128 counts, full-width rejection, exact bounds, immutable snapshot, revocation, failures, no replay and balanced release");
    return 0;
}
'''
    run_test(code, 'gpu-triangle-syscall')


if __name__ == '__main__':
    main()
