#!/usr/bin/env python3
"""CPU equivalence checks for the exact-size GPU presentation fast path.

Executes production selection/bounds code. A CE call spy copies host rows;
the reference executes the actual gui_present CUDA source with host builtins.
No GPU, hardware timing measurement, codec model, or VM is involved.
"""
from pathlib import Path
from test_gpu_variant_safety import function
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    chan = (ROOT / 'kernel/nv_chan.c').read_text()
    shader = (ROOT / 'tools/gui_present.cu').read_text()
    code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
typedef struct { bool open,submit_failed;void *card; } nv_channel_t;
typedef struct { u32 width,height,pitch;u64 bytes,va;bool copy_mapping_attempted; } nv_surface_t;
enum { CH_GFX,CH_COPY };
static nv_channel_t channels[2];
static nv_surface_t surface;
static bool g_scanout_bound,g_compute_scanout_bound,g_render_transaction;
static u32 g_runtime_scanout_width,g_runtime_scanout_height,g_runtime_scanout_pitch;
static u64 g_runtime_scanout_va=0x200000000ull,g_runtime_scanout_bytes;
static u32 source_words[256*128],target_words[256*128],copied_words[256*128];
static unsigned copies,dispatches;
static bool copy_success=true;
static nv_surface_t *nv_surface_lookup(u64 owner,u64 handle) {
    return owner==7&&handle==9?&surface:NULL;
}
static struct {unsigned x,y,z;} blockIdx,blockDim,threadIdx;
'''
    code += shader.replace('extern "C" __global__', '')+'\n'
    code += '#include "'+(ROOT/'tools/shader_gui_present.h').as_posix()+'"\n'
    code += r'''
static bool ce_copy(nv_channel_t *ch,u64 src,u64 dst,u32 sp,u32 dp,u32 bytes,u32 rows,u32 signal) {
    assert(ch==&channels[CH_COPY]&&ch->open&&!ch->submit_failed);
    assert(signal==0x47500000u&&sp==surface.pitch&&dp==g_runtime_scanout_pitch);
    assert(src>=surface.va&&dst>=g_runtime_scanout_va&&bytes&&rows&&!(bytes&3u));
    u64 so=src-surface.va,to=dst-g_runtime_scanout_va;
    assert(so+(u64)(rows-1)*sp+bytes<=surface.bytes);
    assert(to+(u64)(rows-1)*dp+bytes<=g_runtime_scanout_bytes);
    copies++;
    if(!copy_success){ch->submit_failed=true;return false;}
    for(u32 y=0;y<rows;y++)memcpy((u8*)target_words+to+(u64)y*dp,
                                 (u8*)source_words+so+(u64)y*sp,bytes);
    return true;
}
static bool nv_compute_dispatch_one(nv_channel_t *ch,const u8 *sass,u32 bytes,u32 regs,u32 off,
                                    const u32 *a,u32 n,const u32 *grid,const u32 *block,
                                    u32 signal,const char *label) {
    assert(ch==&channels[CH_GFX]&&sass==gui_present_sass&&bytes==sizeof gui_present_sass);
    assert(regs==gui_present_sass_reg_count&&off==0xc000u&&n==19u&&signal==0x47500000u);
    assert(!strcmp(label,"surface/present"));
    assert(((u64)a[0]|((u64)a[1]<<32))==g_runtime_scanout_va);
    assert(((u64)a[2]|((u64)a[3]<<32))==surface.va);
    assert(grid[0]==(a[16]+15)/16&&grid[1]==(a[17]+15)/16&&grid[2]==1);
    assert(block[0]==16&&block[1]==16&&block[2]==1);
    blockDim.x=block[0];blockDim.y=block[1];blockDim.z=1;
    dispatches++;
    for(u32 by=0;by<grid[1];by++)for(u32 bx=0;bx<grid[0];bx++)
    for(u32 ty=0;ty<16;ty++)for(u32 tx=0;tx<16;tx++) {
        blockIdx.x=bx;blockIdx.y=by;threadIdx.x=tx;threadIdx.y=ty;
        gui_present(target_words,source_words,a[4],a[5],a[6],a[7],a[8],a[9],
                     a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18]);
    }
    return true;
}
'''
    code += function(chan,'nv_surface_present')+'\n'
    code += r'''
static void reset(u32 w,u32 h,u32 vx,u32 vy) {
    surface=(nv_surface_t){.width=w+vx+7,.height=h+vy+5,.pitch=256*4,
        .bytes=sizeof source_words,.va=0x300000000ull,.copy_mapping_attempted=true};
    g_runtime_scanout_width=w;g_runtime_scanout_height=h;g_runtime_scanout_pitch=(w+3)*4;
    g_runtime_scanout_bytes=(u64)g_runtime_scanout_pitch*h;
    assert(g_runtime_scanout_bytes<=sizeof target_words);
    g_scanout_bound=g_compute_scanout_bound=g_render_transaction=true;
    channels[0]=(nv_channel_t){true,false,&surface};channels[1]=channels[0];
    copies=dispatches=0;copy_success=true;memset(target_words,0x5a,sizeof target_words);
}
int main(void) {
    // Distinct words including alpha, padding, and source viewport borders.
    for(u32 i=0;i<256*128;i++)source_words[i]=i*0x1020305u+0x17181920u;
    unsigned cases=0;
    for(u32 w=1;w<=113;w+=7)for(u32 h=1;h<=71;h+=7)
    for(u32 origin=0;origin<3;origin++)for(u32 damage=0;damage<3;damage++) {
        u32 vx=origin*13,vy=origin*9;reset(w,h,vx,vy);
        u32 x=damage==0?0:damage==1?w/2:w-1;
        u32 y=damage==0?0:damage==1?h/2:h-1;
        u32 cw=w-x,ch=h-y;
        assert(nv_surface_present(7,9,vx,vy,w,h,x,y,cw,ch,0));
        assert(copies==1&&!dispatches);memcpy(copied_words,target_words,sizeof target_words);
        memset(target_words,0x5a,sizeof target_words);surface.copy_mapping_attempted=false;
        assert(nv_surface_present(7,9,vx,vy,w,h,x,y,cw,ch,0));
        assert(copies==1&&dispatches==1&&!memcmp(target_words,copied_words,sizeof target_words));
        // The reference and copy must also leave every byte outside damage alone.
        for(u32 i=0;i<256*128;i++) {
            u32 dx=i%(w+3),dy=i/(w+3);
            bool hit=dy<h&&dx>=x&&dx<x+cw&&dy>=y&&dy<y+ch;
            assert(copied_words[i]==(hit?source_words[(vy+dy)*256+vx+dx]:0x5a5a5a5au));
        }
        cases++;
    }
    for(u32 rotation=90;rotation<=270;rotation+=90) {
        reset(100,50,3,4);
        assert(nv_surface_present(7,9,3,4,100,50,0,0,100,50,rotation));
        assert(!copies&&dispatches==1);
    }
    reset(100,50,3,4);
    assert(nv_surface_present(7,9,3,4,99,49,7,8,9,10,0)&&!copies&&dispatches==1);
    reset(100,50,3,4);g_render_transaction=false;
    assert(nv_surface_present(7,9,3,4,100,50,7,8,9,10,0)&&!copies&&dispatches==1);
    reset(100,50,3,4);copy_success=false;
    assert(!nv_surface_present(7,9,3,4,100,50,7,8,9,10,0)&&copies==1&&!dispatches);
    assert(!nv_surface_present(7,9,3,4,100,50,7,8,9,10,0)&&copies==1&&!dispatches);
    for(unsigned fault=0;fault<8;fault++) {
        reset(100,50,3,4);
        if(fault==0)channels[CH_COPY].open=false;
        if(fault==1)channels[CH_COPY].card=NULL;
        if(fault==2)channels[CH_GFX].submit_failed=true;
        if(fault==3)g_runtime_scanout_bytes--;
        if(fault==4)g_runtime_scanout_pitch=399;
        if(fault==5)g_scanout_bound=false;
        if(fault==6)g_compute_scanout_bound=false;
        assert(!nv_surface_present(fault==7?8:7,9,3,4,100,50,7,8,9,10,0));
        assert(!copies&&!dispatches);
    }
    reset(100,50,3,4);
    assert(!nv_surface_present(7,9,3,4,100,50,99,0,2,1,0));
    assert(!nv_surface_present(7,9,UINT32_MAX,4,100,50,0,0,1,1,0));
    assert(!copies&&!dispatches);
    printf("PASS: %u exact-size CE/shader pixel equivalence cases; scale/rotation, damage, pitch, ownership and terminal-copy faults\n",cases);
    return 0;
}
'''
    run_test(code,'nv-present-copy')


if __name__ == '__main__':
    main()
