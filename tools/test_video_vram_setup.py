#!/usr/bin/env python3
"""Exercise actual codec VRAM setup with failed RM/map/commit operations.

No GPU emulation: verifies allocation lifetime, address/kind/channel contracts,
and no retry over an ambiguous partially established mapping.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'kernel/nv_chan.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;typedef uint64_t u64;
typedef struct {int owner;} vmm_t;
typedef struct {void *card,*rm;vmm_t vmm;} nv_channel_t;
#define CH_NVENC 3
#define CH_NVDEC 4
#define CH_GFX 1
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
static u64 g_nvenc_vram_fb,g_nvdec_vram_fb;
static bool g_nvenc_vram_ready,g_nvenc_vram_attempted;
static bool g_nvdec_vram_ready,g_nvdec_vram_attempted;
static u64 g_compute_vram_fb;
static bool g_compute_vram_ready,g_compute_vram_attempted;
static unsigned step,fail_at,allocations,linear_maps,tiled_maps,commits;
static unsigned expected_handle,expected_bytes,expected_channel;
static u64 expected_linear,expected_tiled;
static int card,rm;
static bool stage(void){++step;return step!=fail_at;}
static u32 h_vaspace(int channel){assert((unsigned)channel==expected_channel);return 0x9000+channel;}
static bool nv_vram_alloc(void *c,void *r,u32 handle,u64 bytes,u64 *fb){
    assert(c==&card&&r==&rm&&handle==expected_handle&&bytes==expected_bytes);
    assert(step==0);allocations++;
    *fb=0x12340000; /* Even failure may have changed partial allocation state. */
    return stage();
}
static bool nv_vmm_map(vmm_t *v,u64 va,u64 fb,u64 bytes,bool writable,bool a,bool b){
    assert((unsigned)v->owner==expected_channel&&step==1);
    assert(va==expected_linear&&fb==0x12340000&&bytes==expected_bytes&&writable&&!a&&!b);
    linear_maps++;return stage();
}
static bool nv_vmm_map_kind(vmm_t *v,u64 va,u64 fb,u64 bytes,bool writable,bool a,bool b,u32 kind){
    assert((unsigned)v->owner==expected_channel&&step==2);
    assert(va==expected_tiled&&fb==0x12340000&&bytes==expected_bytes&&writable&&!a&&!b&&kind==6);
    tiled_maps++;return stage();
}
static bool nv_vmm_commit(void *c,void *r,vmm_t *v,u32 vaspace,const char *label){
    assert(c==&card&&r==&rm&&(unsigned)v->owner==expected_channel&&step==(expected_tiled?3u:2u));
    assert(vaspace==0x9000+expected_channel&&label);commits++;return stage();
}
'''
    for name in ('VA_NVENC','VA_NVENC_TILED','H_NVENC_VRAM','NVENC_VRAM_BYTES',
                 'VA_NVDEC','VA_NVDEC_TILED','H_NVDEC_VRAM','NVDEC_VRAM_BYTES',
                  'NV_VIDEO_PTE_KIND',
                  'VA_COMPUTE','H_COMPUTE_VRAM','NV_COMPUTE_VRAM_BYTES'):
        c += re.search(r'^#define '+name+r'\s+[^\n]+', src, re.M)[0]+'\n'
    for name in ('ensure_nvenc_vram','ensure_nvdec_vram','ensure_compute_vram'):
        c += function(src,name)+'\n'
    c += r'''
int main(void){
    bool (*setup[3])(nv_channel_t *)={ensure_nvenc_vram,ensure_nvdec_vram,ensure_compute_vram};
    bool *ready[3]={&g_nvenc_vram_ready,&g_nvdec_vram_ready,&g_compute_vram_ready};
    bool *attempted[3]={&g_nvenc_vram_attempted,&g_nvdec_vram_attempted,&g_compute_vram_attempted};
    u64 *fb[3]={&g_nvenc_vram_fb,&g_nvdec_vram_fb,&g_compute_vram_fb};
    /* Independent expected addresses, not derived from the macros under test. */
    const u64 linear[3]={0x800000000ull,0x600000000ull,0x500000000ull};
    const u64 tiled[3]={0x880000000ull,0x680000000ull,0};
    /* 576 KiB includes triangle setup and both resident compaction programs. */
    const u32 handles[3]={0x4c0000,0x4b0000,0x490000},sizes[3]={0xc0000,0x20000,0x90000};
    for(unsigned path=0;path<3;path++)for(fail_at=0;fail_at<=(path==2?3u:4u);fail_at++){
        for(unsigned i=0;i<3;i++){*ready[i]=false;*attempted[i]=false;*fb[i]=0;}
        step=allocations=linear_maps=tiled_maps=commits=0;
        expected_linear=linear[path];expected_tiled=tiled[path];
        expected_handle=handles[path];expected_bytes=sizes[path];expected_channel=path==2?CH_GFX:path?CH_NVDEC:CH_NVENC;
        nv_channel_t ch={&card,&rm,{(int)expected_channel}};
        assert(setup[path](&ch)==(fail_at==0));
        assert(*attempted[path]&&*ready[path]==(fail_at==0));
        unsigned last=path==2?3:4,observed=step;assert(observed==(fail_at?fail_at:last));
        assert(allocations==1&&linear_maps==(observed>=2)&&tiled_maps==(path!=2&&observed>=3)&&commits==(observed>=last));
        assert(*fb[path]==0x12340000);
        for(unsigned repeat=0;repeat<100;repeat++)assert(setup[path](&ch)==(fail_at==0));
        assert(step==observed&&allocations==1); /* No second RM operation on any retry. */
        for(unsigned i=0;i<3;i++)if(i!=path)assert(!*attempted[i]&&!*ready[i]&&!*fb[i]);
    }
    puts("PASS actual codec/compute VRAM setup: 14 success/failure cases, 1400 retries, exact linear/tiled aliases, complete 576KiB compute allocation and owner channel; partial setup retained (modeled RM/VMM)");
}
'''
    run_test(c, 'video_vram_setup')


if __name__ == '__main__':
    main()
