#!/usr/bin/env python3
"""Host-only checks of shader/layout input snapshots; no GPU execution."""
from pathlib import Path
import re
from test_gpu_variant_safety import function
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def definition(text, name):
    end = text.index('} '+name+';')+len('} '+name+';')
    return text[text.rfind('typedef struct {', 0, end):end]+'\n'


def main():
    src = (ROOT/'kernel/syscall.c').read_text()
    abi = (ROOT/'include/kestrel/syscall.h').read_text()
    gpu = (ROOT/'kernel/gpu.h').read_text()
    mm = (ROOT/'kernel/mm.h').read_text()
    code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint64_t u64;typedef uint32_t u32;typedef int64_t s64;
#define E_INVAL 22
#define E_NOSYS 38
#define E_NOMEM 12
'''
    for text, name in ((abi, 'KSIG_MAX'), (abi, 'KLAYOUT_MAX'),
                       (gpu, 'GPU_PROGRAM_SVGA_DX'), (mm, 'VMM_USER_COPY_MAX')):
        code += re.search(r'^#define '+name+r'\s+[^\n]+', text, re.M)[0]+'\n'
    for text, name in ((abi, 'kelement_t'), (abi, 'klayout_t'), (abi, 'ksigline_t'),
                       (abi, 'kshaders_t'), (gpu, 'gpu_shader_program_t')):
        code += definition(text, name)
    code += r'''
static kshaders_t request,original;
static klayout_t layout,original_layout;
static u32 vertex[512],pixel[512],vertex_original[512],pixel_original[512];
static void *allocation;
static size_t allocated_bytes;
static unsigned allocations,frees,copies,shader_calls,layout_calls,fail_copy;
static bool fail_alloc,mutate_metadata;
static int backend_result;
static bool span(u64 address,size_t bytes,const void *base,size_t size) {
    u64 start=(uintptr_t)base;
    return address>=start&&address-start<=size&&bytes<=size-(address-start);
}
static bool user_copy(void *dst,u64 address,size_t bytes,bool write) {
    assert(!write&&bytes<=VMM_USER_COPY_MAX);copies++;
    if(copies==fail_copy)return false;
    if(!span(address,bytes,&request,sizeof request)&&!span(address,bytes,&layout,sizeof layout)&&
       !span(address,bytes,vertex,sizeof vertex)&&!span(address,bytes,pixel,sizeof pixel))return false;
    memcpy(dst,(const void *)(uintptr_t)address,bytes);
    // A sibling changes the metadata as soon as the kernel has its snapshot.
    if(mutate_metadata&&address==(uintptr_t)&request)memset(&request,0xcc,sizeof request);
    return true;
}
static void *kmalloc(size_t bytes) {
    assert(!allocation);allocated_bytes=bytes;
    if(fail_alloc)return NULL;
    allocation=malloc(bytes);assert(allocation);allocations++;return allocation;
}
static void kfree(void *p) {
    assert(p&&p==allocation);free(p);allocation=NULL;frees++;
}
static int gpu_accel_set_shaders(u32 format,const gpu_shader_program_t *vs,const gpu_shader_program_t *ps) {
    shader_calls++;assert(format==GPU_PROGRAM_SVGA_DX);
    assert(vs->code==allocation&&ps->code==(u32*)allocation+original.vertex_words);
    assert(allocated_bytes==(original.vertex_words+original.pixel_words)*sizeof(u32));
    // Backend waits must not retain any user-memory code/signature pointers.
    memset(vertex,0xdd,sizeof vertex);memset(pixel,0xee,sizeof pixel);
    memset(&request,0xcc,sizeof request);
    assert(vs->words==original.vertex_words&&ps->words==original.pixel_words);
    assert(!memcmp(vs->code,vertex_original,vs->words*4));
    assert(!memcmp(ps->code,pixel_original,ps->words*4));
    assert(vs->input_count==original.vertex_takes_count&&vs->output_count==original.vertex_gives_count);
    assert(ps->input_count==original.pixel_takes_count&&ps->output_count==original.pixel_gives_count);
    assert(!memcmp(vs->inputs,original.vertex_takes,vs->input_count*sizeof(ksigline_t)));
    assert(!memcmp(vs->outputs,original.vertex_gives,vs->output_count*sizeof(ksigline_t)));
    assert(!memcmp(ps->inputs,original.pixel_takes,ps->input_count*sizeof(ksigline_t)));
    assert(!memcmp(ps->outputs,original.pixel_gives,ps->output_count*sizeof(ksigline_t)));
    return backend_result;
}
static int gpu_accel_set_layout(u32 format,const u32 *elements,u32 count,u32 stride) {
    layout_calls++;assert(format==GPU_PROGRAM_SVGA_DX);
    assert((const void*)elements!=(const void*)layout.elements);
    memset(&layout,0xcc,sizeof layout);
    assert(count==original_layout.count&&stride==original_layout.stride);
    assert(!memcmp(elements,original_layout.elements,count*sizeof(kelement_t)));
    return backend_result;
}
'''
    code += function(src, 'gpu_shaders_request')+'\n'+function(src, 'gpu_layout_request')+'\n'
    for op in ('SHADERS', 'LAYOUT'):
        start = src.index('        case GPUOP_'+op+': {')
        end = src.index('\n        }', start)+len('\n        }')
        assert 'return gpu_'+op.lower()+'_request(a1);' in src[start:end]
    code += r'''
static void reset(void) {
    assert(!allocation&&allocations==frees);
    copies=shader_calls=layout_calls=allocations=frees=fail_copy=0;
    fail_alloc=mutate_metadata=false;backend_result=0;
    for(unsigned i=0;i<512;i++){vertex[i]=0x12340000u+i;pixel[i]=0xaabb0000u+i;}
    memcpy(vertex_original,vertex,sizeof vertex);memcpy(pixel_original,pixel,sizeof pixel);
    request=(kshaders_t){.vertex=vertex,.pixel=pixel,.vertex_words=512,.pixel_words=512,
        .vertex_takes_count=16,.vertex_gives_count=15,.pixel_takes_count=14,.pixel_gives_count=13};
    for(unsigned i=0;i<16;i++) {
        request.vertex_takes[i]=(ksigline_t){i,i+100};request.vertex_gives[i]=(ksigline_t){i+16,i+200};
        request.pixel_takes[i]=(ksigline_t){i+32,i+300};request.pixel_gives[i]=(ksigline_t){i+48,i+400};
    }
    original=request;layout=(klayout_t){.count=16,.stride=32};
    for(unsigned i=0;i<16;i++)layout.elements[i]=(kelement_t){i,i+10,i+20,i+30};
    original_layout=layout;
}
int main(void) {
    for(unsigned words=1;words<=512;words++) {
        reset();request.vertex_words=words;request.pixel_words=513-words;original=request;
        mutate_metadata=true;
        assert(!gpu_shaders_request((uintptr_t)&request)&&shader_calls==1&&copies==3);
        assert(allocations==1&&frees==1);
    }
    for(unsigned count=0;count<=KSIG_MAX;count++) {
        reset();request.vertex_takes_count=request.vertex_gives_count=count;
        request.pixel_takes_count=request.pixel_gives_count=count;original=request;
        assert(!gpu_shaders_request((uintptr_t)&request)&&shader_calls==1);
    }
    for(unsigned fault=0;fault<8;fault++) {
        reset();
        if(fault==0)request.vertex_words=0;if(fault==1)request.pixel_words=0;
        if(fault==2)request.vertex_words=513;if(fault==3)request.pixel_words=UINT32_MAX;
        if(fault==4)request.vertex_takes_count=17;if(fault==5)request.vertex_gives_count=17;
        if(fault==6)request.pixel_takes_count=17;if(fault==7)request.pixel_gives_count=17;
        assert(gpu_shaders_request((uintptr_t)&request)==-E_INVAL&&!allocations&&!shader_calls);
    }
    for(unsigned fault=1;fault<=3;fault++) {
        reset();fail_copy=fault;
        assert(gpu_shaders_request((uintptr_t)&request)==-E_INVAL&&!shader_calls&&allocations==frees);
    }
    reset();fail_alloc=true;
    assert(gpu_shaders_request((uintptr_t)&request)==-E_NOMEM&&!shader_calls&&!frees&&copies==1);
    reset();backend_result=-1;
    assert(gpu_shaders_request((uintptr_t)&request)==-E_NOSYS&&shader_calls==1&&allocations==frees);
    reset();request.pixel=(const u32*)(UINT64_MAX-7);
    assert(gpu_shaders_request((uintptr_t)&request)==-E_INVAL&&!shader_calls&&allocations==frees);
    reset();assert(gpu_shaders_request(0)==-E_INVAL&&!allocations);
    for(unsigned count=1;count<=KLAYOUT_MAX;count++) {
        reset();layout.count=count;original_layout=layout;
        assert(!gpu_layout_request((uintptr_t)&layout)&&layout_calls==1&&copies==1&&!allocations);
    }
    for(unsigned fault=0;fault<4;fault++) {
        reset();
        if(fault==0)layout.count=0;if(fault==1)layout.count=17;
        if(fault==2)layout.stride=0;if(fault==3)fail_copy=1;
        assert(gpu_layout_request((uintptr_t)&layout)==-E_INVAL&&!layout_calls);
    }
    reset();backend_result=-1;
    assert(gpu_layout_request((uintptr_t)&layout)==-E_NOSYS&&layout_calls==1);
    reset();assert(gpu_layout_request(UINT64_MAX-7)==-E_INVAL&&!layout_calls);
    puts("PASS shader/layout snapshots: 512 code lengths, signatures, 16 layouts, metadata mutation, code mutation, copy/allocation/backend failures, balanced release");
    return 0;
}
'''
    run_test(code, 'gpu-program-syscall')


if __name__ == '__main__':
    main()
