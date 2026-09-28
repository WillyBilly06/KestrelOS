#!/usr/bin/env python3
"""Execute actual video syscall admission, snapshots and publication gates."""
from pathlib import Path
import re
from test_gpu_stable_candidate import run_test, function

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT/'kernel/syscall.c').read_text()
    assert 'return gpu_video_request(p, a1, a2);' in src
    mm = (ROOT/'kernel/mm.h').read_text()
    c = re.search(r'^#define VMM_USER_COPY_MAX\s+[^\n]+', mm, re.M)[0]+'\n'
    c += r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;typedef int64_t s64;
#define E_INVAL 22
#define E_NOMEM 12
#define E_IO 5
#define E_BUSY 16
typedef struct proc {u64 gpu_owner_id;bool resource_closing;struct proc *leader;} proc_t;
static proc_t *proc_shared(proc_t *p){return p->leader?p->leader:p;}
static bool locked,busy,readonly_request,invalid_input,invalid_output;
static bool irq_enabled=true,revoke_input_on_alloc;
static bool irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void irq_restore(bool old){assert(!irq_enabled);irq_enabled=old;}
static unsigned locks,unlocks,allocs,frees,malloc_calls,fail_malloc_at,inspects,decodes;
static unsigned backend_mode;
static u8 input[64],pixels[6144],request_space[8192];
static bool nv_render_try_begin(void){if(busy)return false;assert(!locked);locked=true;locks++;return true;}
static void nv_render_end(void){assert(locked);locked=false;unlocks++;}
static void *kmalloc(size_t n){assert(locked&&irq_enabled);malloc_calls++;if(malloc_calls==fail_malloc_at)return NULL;allocs++;if(revoke_input_on_alloc)invalid_input=true;return malloc(n);}
static void kfree(void *p){assert(locked);frees++;free(p);}
static bool span(u64 a,size_t n,const void *p,size_t cap){
    uintptr_t start=(uintptr_t)p;return a>=start && a-start<=cap && n<=cap-(a-start);
}
static bool user_range_ok(u64 a,size_t n,bool write){
    assert(locked&&irq_enabled); /* VM locking belongs to copy/range helpers. */
    if(span(a,n,request_space,sizeof request_space))return !write||!readonly_request;
    if(span(a,n,input,sizeof input))return !invalid_input;
    if(span(a,n,pixels,sizeof pixels))return !invalid_output;
    return false;
}
/* Model the VM-locked access boundary while retaining the production
 * chunking/snapshot logic and permission-revocation checks on every copy. */
static bool user_copy(void *buffer,u64 address,size_t bytes,bool write){
    assert(buffer&&bytes<=VMM_USER_COPY_MAX);
    if(!user_range_ok(address,bytes,write))return false;
    if(write)memcpy((void*)address,buffer,bytes);
    else memcpy(buffer,(const void*)address,bytes);
    return true;
}
'''
    c += '#include "'+(ROOT/'include/kestrel/video.h').as_posix()+'"\n'
    c += r'''
/* This harness retains its decoder-only coverage after the API dispatch
 * extension. Encoder admission/submission needs its own additional coverage. */
static int nv_nvenc_encode_idr(const unsigned char *data,unsigned bytes,
    unsigned char *output,unsigned capacity,kvideo_request_t *r){
    (void)data;(void)bytes;(void)output;(void)capacity;(void)r;
    assert(!"unexpected encode in decoder syscall harness");return -E_INVAL;
}
static int nv_nvdec_decode_idr(const unsigned char *data,unsigned bytes,
    unsigned char *output,unsigned capacity,kvideo_request_t *r){
    assert(locked && irq_enabled && bytes==64 && data!=input && r!=(void*)request_space);
    for(unsigned i=0;i<64;i++)assert(data[i]==0x5a); /* immutable across both calls */
    if(r->operation==KVIDEO_H264_INSPECT){
        inspects++;assert(!output&&!capacity);
        /* Mutate original user bytes/request after the kernel snapshot. */
        memset(input,0,sizeof input);
        ((kvideo_request_t*)request_space)->input=1;
        ((kvideo_request_t*)request_space)->output=1;
        r->written_bytes=0;r->phase=KVIDEO_PHASE_COMPLETE;
        r->coded_width=r->coded_height=r->pitch=64;
        r->display_width=r->display_height=64;r->required_bytes=6144;
        if(backend_mode==1){r->phase=KVIDEO_PHASE_HEADERS;r->parse_status=3;return -E_INVAL;}
        if(backend_mode==2)r->required_bytes=KVIDEO_OUTPUT_MAX+1u;
        if(backend_mode==3)r->required_bytes=0;
        return 0;
    }
    assert(r->operation==KVIDEO_H264_DECODE_IDR && output && output!=pixels && capacity==6144);
    decodes++;memset(output,0x39,6144);
    r->phase=KVIDEO_PHASE_COMPLETE;r->written_bytes=6144;
    if(backend_mode==4){r->phase=KVIDEO_PHASE_STATUS;r->firmware_error=7;return -E_IO;}
    if(backend_mode==5)r->written_bytes=6143;
    if(backend_mode==6)r->phase=KVIDEO_PHASE_RELEASE;
    if(backend_mode==7)r->written_bytes=r->required_bytes=6145;
    if(backend_mode==8)invalid_output=true;
    if(backend_mode==9)readonly_request=true;
    return 0;
}
'''
    start=src.index('static s64 gpu_video_request(')
    c += function(src,'gpu_user_access')+'\n'+src[start:src.index('\n}',start)+2]
    c += r'''
static proc_t leader={.gpu_owner_id=111},thread={.gpu_owner_id=0,.leader=&leader};
static kvideo_request_t *r=(kvideo_request_t*)request_space;
static void setup(unsigned operation){
    assert(!locked && allocs==frees && locks==unlocks);
    locks=unlocks=allocs=frees=malloc_calls=fail_malloc_at=inspects=decodes=backend_mode=0;
    busy=readonly_request=invalid_input=invalid_output=revoke_input_on_alloc=false;leader.gpu_owner_id=111;leader.resource_closing=false;
    memset(input,0x5a,sizeof input);memset(pixels,0xa5,sizeof pixels);memset(request_space,0,sizeof request_space);
    *r=(kvideo_request_t){.version=1,.operation=operation,.input=(uintptr_t)input,.input_bytes=64,
        .output=(uintptr_t)pixels,.output_capacity=6144,.written_bytes=12345,.phase=99};
}
static int call(void){int rc=(int)gpu_video_request(&thread,(uintptr_t)r,sizeof *r);assert(!locked&&locks==unlocks&&allocs==frees);return rc;}
static void untouched(void){for(unsigned i=0;i<sizeof pixels;i++)assert(pixels[i]==0xa5);}
int main(void){
    setup(0);assert(call()==0&&inspects==1&&!decodes&&r->written_bytes==0&&r->required_bytes==6144);untouched();
    setup(1);assert(call()==0&&inspects==1&&decodes==1&&r->written_bytes==6144&&r->phase==10);
    assert(r->input==(uintptr_t)input&&r->output==(uintptr_t)pixels);
    for(unsigned i=0;i<sizeof pixels;i++)assert(pixels[i]==0x39);
    setup(1);assert(gpu_video_request(&thread,(uintptr_t)r,111)==-E_INVAL&&!locks);
    setup(1);busy=true;assert(call()==-E_BUSY&&!locks&&!inspects);
    setup(1);readonly_request=true;assert(call()==-E_INVAL&&!inspects&&!allocs);
    setup(1);assert(gpu_video_request(&thread,1,sizeof *r)==-E_INVAL&&locks==unlocks);
    for(unsigned which=0;which<4;which++){
        setup(1);if(which==0)r->version=99;if(which==1)r->operation=99;
        if(which==2)r->reserved=1;if(which==3)leader.resource_closing=true;
        assert(call()==-E_INVAL&&!inspects&&!allocs);untouched();
    }
    setup(1);leader.gpu_owner_id=0;assert(call()==-E_INVAL&&!inspects);
    for(unsigned which=0;which<4;which++){
        setup(1);if(which==0)r->input_bytes=0;if(which==1)r->input_bytes=KVIDEO_INPUT_MAX+1ull;
        if(which==2)r->input=UINT64_MAX;if(which==3)invalid_input=true;
        assert(call()==-E_INVAL&&!inspects&&!allocs&&r->written_bytes==0);untouched();
    }
    for(unsigned which=0;which<3;which++){
        setup(1);if(which==0)r->output_capacity=6143;if(which==1)r->output=UINT64_MAX;
        if(which==2)invalid_output=true;
        assert(call()==-E_INVAL&&inspects==1&&!decodes&&r->written_bytes==0);untouched();
    }
    for(unsigned offset=0;offset<112;offset+=16){
        setup(1);r->output=(uintptr_t)r+offset;
        assert(call()==-E_INVAL&&inspects==1&&!decodes&&r->written_bytes==0);untouched();
    }
    for(unsigned which=1;which<=2;which++){
        setup(1);fail_malloc_at=which;
        assert(call()==-E_NOMEM&&!decodes&&r->written_bytes==0&&r->phase==KVIDEO_PHASE_ALLOCATE);untouched();
    }
    for(unsigned which=1;which<=7;which++){
        setup(1);backend_mode=which;
        assert(call()==(which<4?-E_INVAL:-E_IO));assert(r->written_bytes==0);untouched();
        if(which==4)assert(r->firmware_error==7&&r->phase==KVIDEO_PHASE_STATUS);
    }
    for(unsigned i=0;i<100;i++){setup(1);assert(call()==0&&allocs==2&&frees==2);}
    setup(1);revoke_input_on_alloc=true;
    assert(call()==-E_INVAL&&!inspects&&!decodes);untouched();
    setup(1);backend_mode=8;
    assert(call()==-E_INVAL&&decodes==1&&r->written_bytes==0);untouched();
    setup(1);backend_mode=9;
    assert(call()==-E_INVAL&&decodes==1); // revoked metadata not published
    assert(r->phase==99&&irq_enabled);
    puts("PASS actual video syscall: immutable snapshots, pinned transaction, owner/ranges/ABI, metadata inspect, complete-only pixel publication, errors, allocation failures and 100 balanced calls (modeled backend)");
    return 0;
}
'''
    run_test(c, 'gpu_video_syscall')


if __name__=='__main__':
    main()
