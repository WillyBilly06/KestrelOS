#!/usr/bin/env python3
"""Compile-only modeled coverage for production GPU geometry metadata helpers.

Extracts the actual clear/planning functions. COPY retirement and allocation
edges are modeled; this is neither GPU execution nor measured performance.
The default command only compiles/links: the generated executable is NOT run.
"""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/nv_chan.c").read_text()


def main():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
'''
    source += '#include "' + (ROOT / 'include/kestrel/shader_setup.h').as_posix() + '"\n'
    source += r'''
typedef struct { bool open,submit_failed; } nv_channel_t;
typedef struct { u64 bytes,va; unsigned state; bool copy_mapping_attempted; } nv_surface_t;
enum { CH_COPY,CH_GFX,NS_READY=7 };
static nv_channel_t channels[2]={{true,false},{true,false}};
static unsigned g_render_transaction=1;
static unsigned fill_calls,draw_calls;
static bool fill_retire=true;
static u64 fill_dst,fill_span;
static u32 fill_pitch,fill_width,fill_height;
static unsigned char memory[256000];
static const u64 memory_va=0x30000000000ull;

/* COPY is a bounded memory-effect/retirement model, not an encoder emulator. */
static bool ce_fill32(nv_channel_t *ch,u64 dst,u32 pitch,u32 width,
                      u32 height,u32 colour,u32 signal) {
    assert(ch==&channels[CH_COPY] && ch->open && !ch->submit_failed);
    assert(width && width<=32768 && height && height<=32768);
    assert(!(dst&3) && !(pitch&3) && pitch>=width*4 && colour==0 && signal==0x535a0000u);
    fill_calls++;fill_dst=dst;fill_pitch=pitch;fill_width=width;fill_height=height;
    fill_span=(u64)(height-1)*pitch+width*4;
    assert(dst>=memory_va && dst-memory_va<=sizeof memory && fill_span<=sizeof memory-(dst-memory_va));
    if(!fill_retire){ch->submit_failed=true;return false;}
    for(u32 row=0;row<height;row++)memset(memory+(size_t)(dst-memory_va)+(size_t)row*pitch,0,width*4);
    return true;
}

typedef struct { u32 first,count; kshs_rect_t rect; u64 work; } nv_setup_window_t;
typedef struct { const char *stage; u32 index,result; u64 detail; } nv_setup_failure_t;
static void *allocation;
static size_t allocation_bytes;
static unsigned allocation_calls,fail_allocation_call,live_allocations,allocation_frees;

/* Models the actual heap.c contract: failure retains old storage. Success
 * deliberately moves it, catching reliance on pointers into the old plan. */
static void *krealloc(void *old,size_t bytes) {
    assert(old==allocation && bytes && draw_calls==0);
    allocation_calls++;
    if(allocation_calls==fail_allocation_call)return NULL;
    void *next=malloc(bytes);assert(next);
    if(old){memcpy(next,old,allocation_bytes<bytes?allocation_bytes:bytes);free(old);}
    else live_allocations++;
    allocation=next;allocation_bytes=bytes;return next;
}
static void release_plan(nv_setup_window_t *plan) {
    if(plan){assert(plan==allocation && live_allocations==1);free(plan);live_allocations--;allocation_frees++;}
    else assert(!allocation);
    allocation=NULL;allocation_bytes=0;
}
'''
    for name in ('nv_surface_clear_records', 'nv_setup_region_cost', 'nv_setup_plan'):
        source += '\n' + function(SRC, name) + '\n'
    source += r'''
static void clear_span_case(u32 record,u32 records,u32 expected_width,u32 expected_height) {
    const u64 offset=64,bytes=(u64)record*records;
    nv_surface_t surface={offset+bytes,memory_va,NS_READY,true};
    memset(memory,0xa5,sizeof memory);unsigned before=fill_calls;
    assert(nv_surface_clear_records(&surface,offset,record,records));
    assert(fill_calls==before+1 && fill_dst==memory_va+offset && fill_span==bytes);
    assert(fill_width==expected_width && fill_height==expected_height && fill_pitch==expected_width*4);
    for(size_t i=0;i<sizeof memory;i++)assert(memory[i]==(i>=offset && i<offset+bytes?0:0xa5));
}
static void clear_cases(void) {
    assert(KSHS_MAX_TRIANGLES==1365 && sizeof(kshs_primitive_t)==136);
    clear_span_case(136,1365,34,1365); /* 185640 bytes, exact typed rows */
    clear_span_case(16,1365,5460,1);   /* compact status: one line */
    clear_span_case(8,16384,32768,1); /* exact 131072-byte line boundary */
    clear_span_case(8,16385,2,16385); /* first size above the single-line limit */
    clear_span_case(136,1,34,1);
    clear_span_case(4,1,1,1);
    nv_surface_t surface={sizeof memory,memory_va,NS_READY,true};
    unsigned before=fill_calls;
    assert(!nv_surface_clear_records(NULL,0,8,1));
    assert(!nv_surface_clear_records(&surface,UINT64_MAX-3,8,1));
    assert(!nv_surface_clear_records(&surface,surface.bytes+4,8,1));
    assert(!nv_surface_clear_records(&surface,surface.bytes,8,1));
    assert(!nv_surface_clear_records(&surface,surface.bytes-4,8,1));
    assert(!nv_surface_clear_records(&surface,1,8,1));
    assert(!nv_surface_clear_records(&surface,0,6,1));
    assert(!nv_surface_clear_records(&surface,0,0,1));
    assert(!nv_surface_clear_records(&surface,0,8,0));
    assert(!nv_surface_clear_records(&surface,0,131076,1));
    assert(!nv_surface_clear_records(&surface,0,4,32769));
    assert(!nv_surface_clear_records(&surface,0,UINT32_MAX,UINT32_MAX));
    g_render_transaction=0;assert(!nv_surface_clear_records(&surface,0,8,1));g_render_transaction=1;
    surface.state=0;assert(!nv_surface_clear_records(&surface,0,8,1));surface.state=NS_READY;
    surface.copy_mapping_attempted=false;assert(!nv_surface_clear_records(&surface,0,8,1));surface.copy_mapping_attempted=true;
    channels[CH_COPY].open=false;assert(!nv_surface_clear_records(&surface,0,8,1));channels[CH_COPY].open=true;
    channels[CH_GFX].submit_failed=true;assert(!nv_surface_clear_records(&surface,0,8,1));channels[CH_GFX].submit_failed=false;
    assert(fill_calls==before);
    fill_retire=false;assert(!nv_surface_clear_records(&surface,0,8,1));
    assert(channels[CH_COPY].submit_failed && fill_calls==before+1);
    assert(!nv_surface_clear_records(&surface,0,8,1) && fill_calls==before+1);
    fill_retire=true;channels[CH_COPY].submit_failed=false;
}

static kshr_job_t job;
static nv_surface_t scratch,status;
static void reset_plan(void) {
    assert(!allocation && !live_allocations);allocation_calls=allocation_frees=fail_allocation_call=draw_calls=0;
    memset(&job,0,sizeof job);job.code_count=1;job.code[0].op=SH_END;job.budget=1;
    scratch=(nv_surface_t){sizeof(job),0,NS_READY,true};
    status=(nv_surface_t){KSHR_FAST_MAX_LANES*sizeof(ksh_status_t),0,NS_READY,true};
    assert(kshr_register_fast_eligible(&job));
}

/* Simulated transaction boundary, not the complete production GPUOP body:
 * no modeled draw is admitted until production planning has fully succeeded. */
static bool modeled_transaction(const kshs_rect_t *rects,u32 count,nv_setup_window_t **plan,
                                u32 *plans,u32 *lanes,nv_setup_failure_t *why) {
    bool ok=nv_setup_plan(rects,count,&job,&scratch,&status,plan,plans,lanes,why);
    if(ok)draw_calls+=*plans;
    return ok;
}
static void plan_cases(void) {
    static kshs_rect_t rects[576];nv_setup_window_t *plan=NULL;
    u32 plans=0,lanes=0;nv_setup_failure_t why={0};
    reset_plan();
    for(unsigned i=0;i<65;i++)rects[i]=(kshs_rect_t){0,0,i<64?128:1,i<64?128:1};
    assert(nv_setup_plan(rects,65,&job,&scratch,&status,&plan,&plans,&lanes,&why));
    assert(plans==2 && lanes==16384 && kshr_job_lanes(&job)==1 && allocation_calls==1);
    assert(plan[0].count==64 && plan[1].first==64 && plan[1].count==1 && draw_calls==0);
    assert(plan[0].work==1048576 && plan[1].work==1);
    release_plan(plan);plan=NULL;

    reset_plan();for(unsigned i=0;i<576;i++)rects[i]=(kshs_rect_t){0,0,1,1};
    assert(nv_setup_plan(rects,576,&job,&scratch,&status,&plan,&plans,&lanes,&why));
    assert(plans==9 && lanes==1 && allocation_calls==2 && allocation_bytes==16*sizeof(*plan));
    for(unsigned i=0;i<plans;i++)assert(plan[i].first==i*64 && plan[i].count==64 && plan[i].work==64);
    release_plan(plan);plan=NULL;

    reset_plan();fail_allocation_call=1;
    assert(!modeled_transaction(rects,576,&plan,&plans,&lanes,&why));
    assert(!plan && !allocation && !live_allocations && !draw_calls);
    assert(!strcmp(why.stage,"admission-plan-allocation"));release_plan(plan);

    reset_plan();fail_allocation_call=2;
    assert(!modeled_transaction(rects,576,&plan,&plans,&lanes,&why));
    assert(plan==allocation && live_allocations==1 && allocation_bytes==8*sizeof(*plan) && !draw_calls);
    assert(!strcmp(why.stage,"admission-plan-allocation") && why.result==8);
    for(unsigned i=0;i<8;i++)assert(plan[i].first==i*64 && plan[i].count==64);
    release_plan(plan);plan=NULL;assert(!live_allocations && allocation_frees==1);

    reset_plan();rects[0]=(kshs_rect_t){0,0,32768,32768};
    assert(!modeled_transaction(rects,1,&plan,&plans,&lanes,&why));
    assert(!strcmp(why.stage,"admission-whole-work") && why.detail>KSHS_MAX_RASTER_WORK && !draw_calls);
    release_plan(plan);plan=NULL;

    reset_plan();rects[0]=(kshs_rect_t){0,0,65,65};status.bytes=sizeof(ksh_status_t);
    assert(!modeled_transaction(rects,1,&plan,&plans,&lanes,&why));
    assert(!strcmp(why.stage,"admission-dispatch-limit") && why.result==KSHS_MAX_RASTER_DISPATCHES && !draw_calls);
    assert(allocation_bytes==KSHS_MAX_RASTER_DISPATCHES*sizeof(*plan));release_plan(plan);plan=NULL;

    reset_plan();rects[0]=(kshs_rect_t){0,0,1,1};scratch.bytes=sizeof(job)-1;
    assert(!modeled_transaction(rects,1,&plan,&plans,&lanes,&why));
    assert(!strcmp(why.stage,"admission-minimum-tile-capacity") && !draw_calls && !plan);
    release_plan(plan);
}
int main(void) {
    clear_cases();plan_cases();
    assert(!live_allocations);
    puts("PASS modeled clear spans, admission growth/failure ownership, peak lanes and pre-draw limits");
    return 0;
}
'''
    # Compilation is deliberately the only default action. The model assertions
    # above are NOT claimed as executed or passed by this command.
    run_test(source, "nv_geometry_metadata", compile_only=True)


if __name__ == '__main__':
    main()
