#!/usr/bin/env python3
"""Real NVDEC job planner + real Blackwell VMM pool/reuse integration test.

Uses the production 1-MiB page-table budget, observed 4-KiB Falcon context,
rings, the existing boot fixture and both dynamic decode aliases. This
checks page-table contents/capacity; it does not emulate RM TLB invalidation.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def typedef(src, name):
    end = src.index('} ' + name + ';') + len('} ' + name + ';')
    return src[src.rfind('typedef struct {', 0, end):end] + '\n'


def main():
    chan = (ROOT / 'kernel/nv_chan.c').read_text()
    header = (ROOT / 'kernel/nv.h').read_text()
    vmm = (ROOT / 'kernel/nv_vmm.c').read_text()
    assert 'const u32 tables_bytes = POOL_BYTES / 2;' in chan
    assert 'tables_bytes / PAGE_SIZE, true)' in chan
    log = (ROOT / 'out/stick/_logs/20260909-005445/KERNEL.LOG').read_text()
    assert 'NVDEC/decode: mapped RM Falcon ctx at VA 0x400000000 (0x1000 bytes' in log
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define PAGE_SIZE 4096u
#define kwarn(...) ((void)0)
'''
    c += '#include "' + (ROOT / 'kernel/nvdec_h264_job.h').as_posix() + '"\n'
    c += typedef(header, 'nv_vmm_t') + typedef(header, 'nv_vmm_level_t')
    for name in ('POOL_BYTES', 'GPFIFO_ENTRIES', 'GPFIFO_BYTES', 'PUSHBUF_BYTES',
                 'SEM_BYTES', 'VA_GPFIFO', 'VA_PUSHBUF', 'VA_SEM', 'VA_HOST_CTX_BASE',
                 'VA_NVDEC', 'VA_NVDEC_TILED', 'NVDEC_VRAM_BYTES',
                 'VA_NVDEC_USER', 'VA_NVDEC_USER_TILED',
                 'NV_VIDEO_PTE_KIND'):
        c += re.search(r'^#define ' + name + r'\s+[^\n]+', chan, re.M)[0] + '\n'
    # Compile the actual allocator, map/unmap and independent production walker.
    vmm = vmm[:vmm.index('/* ------------------------------------------------------------------- test */')]
    c += re.sub(r'^#include [^\n]*\n', '', vmm, flags=re.M)
    c += r'''
static u64 table_storage[(POOL_BYTES/2)/sizeof(u64)];
static nv_vmm_t v;
typedef struct {u64 va,pa,bytes; bool vram;u32 kind;} mapping;
static const mapping existing[]={
    {VA_GPFIFO,0x100000000ull,GPFIFO_BYTES,false,0},
    {VA_PUSHBUF,0x100100000ull,PUSHBUF_BYTES,false,0},
    {VA_SEM,0x100200000ull,SEM_BYTES,false,0},
    {VA_HOST_CTX_BASE,0x100300000ull,0x1000,true,0},
    {VA_NVDEC,0x100400000ull,NVDEC_VRAM_BYTES,true,0},
    {VA_NVDEC_TILED,0x100400000ull,NVDEC_VRAM_BYTES,true,NV_VIDEO_PTE_KIND}
};
static kh264_decode_desc desc(unsigned w,unsigned h,unsigned refs,unsigned bytes){
    kh264_decode_desc d={0}; d.profile_idc=66;
    d.coded_width=d.display_width=w;d.coded_height=d.display_height=h;
    d.width_mbs=w/16;d.height_mbs=h/16;d.max_num_ref_frames=refs;
    d.slice_start=0;d.slice_end=bytes;return d;
}
/* Decode the terminal PTE separately to inspect KIND; the production walker
 * already independently checks the physical address/aperture/permissions. */
static u64 terminal(u64 va){
    u64 phys=v.root_gpu;
    const unsigned shifts[]={56,47,38,29,21};
    const unsigned masks[]={1,511,511,511,255};
    for(unsigned i=0;i<5;i++){
        assert(phys>=v.pool_gpu && phys-v.pool_gpu+4096<=sizeof table_storage);
        const u64 *table=(const u64*)(v.pool+phys-v.pool_gpu);
        unsigned index=(unsigned)(va>>shifts[i])&masks[i];
        u64 entry=table[i==4?index*2+1:index];
        if(!((entry>>1)&3))return 0;
        phys=entry&0x000ffffffffff000ull;
    }
    assert(phys>=v.pool_gpu && phys-v.pool_gpu+4096<=sizeof table_storage);
    return ((const u64*)(v.pool+phys-v.pool_gpu))[(va>>12)&511];
}
static void check(const mapping *m,bool present){
    for(u64 offset=0;offset<m->bytes;offset+=4096){
        nv_vmm_walk_t w;
        nv_vmm_walk(&v,m->va+offset+0xabc,&w);
        assert(w.present==present);
        u64 raw=terminal(m->va+offset);
        if(!present){assert(!raw);continue;}
        assert(w.physical==m->pa+offset+0xabc && w.levels_walked==6);
        assert(w.aperture==(m->vram?0u:2u) && !w.read_only);
        assert(w.volatile_==!m->vram && ((raw>>8)&15)==m->kind);
    }
}
static void check_existing(void){
    for(unsigned i=0;i<sizeof existing/sizeof existing[0];i++)check(&existing[i],true);
}
static void setup(void){
    assert(nv_vmm_init(&v,(u8*)table_storage,0x500000000ull,
                       (POOL_BYTES/2)/PAGE_SIZE,true));
    assert(v.pages==256);
    /* The host path also reserves the shared 4..4.5-GiB upper directory chain.
     * Its RM-owned descendants are outside this client's allocation pool. */
    nv_vmm_level_t reserved[4];
    assert(nv_vmm_reserve_512m_levels(&v,0x100000000ull,reserved));
    for(unsigned i=0;i<sizeof existing/sizeof existing[0];i++){
        const mapping *m=&existing[i];
        assert(nv_vmm_map_kind(&v,m->va,m->pa,m->bytes,m->vram,false,false,m->kind));
    }
    check_existing();
}
static void cycle(u32 bytes,u64 physical,u32 largest,u32 base_mapped){
    mapping linear={VA_NVDEC_USER,physical,bytes,true,0};
    mapping tiled={VA_NVDEC_USER_TILED,physical,bytes,true,NV_VIDEO_PTE_KIND};
    assert(nv_vmm_map(&v,linear.va,linear.pa,bytes,true,false,false));
    assert(nv_vmm_map_kind(&v,tiled.va,tiled.pa,bytes,true,false,false,NV_VIDEO_PTE_KIND));
    assert(v.mapped==base_mapped+2u*bytes/4096);
    check(&linear,true);check(&tiled,true);check_existing();
    /* After shrinking a previously larger job, all retained tail PTEs are zero. */
    mapping tail={linear.va+bytes,0,largest-bytes,true,0};check(&tail,false);
    tail.va=tiled.va+bytes;check(&tail,false);
    assert(nv_vmm_unmap(&v,tiled.va,bytes));
    check(&tiled,false);check(&linear,true);
    assert(nv_vmm_unmap(&v,linear.va,bytes));
    linear.bytes=tiled.bytes=largest;check(&linear,false);check(&tiled,false);
    assert(v.mapped==base_mapped);check_existing();
}
int main(void){
    kh264_decode_desc d=desc(4096,4096,16,16u*1024*1024);
    nvdec_h264_job_layout maxjob;
    assert(nvdec_h264_job_plan(&maxjob,&d,16u*1024*1024)==KH264_OK);
    setup();u32 baseline=v.used,base_mapped=v.mapped,largest=0;
    /* Increasing both coded geometry and compressed input grows the same VA
     * paths. No old object stays mapped, but directory pages remain retained. */
    for(unsigned i=1;i<=64;i++){
        unsigned dimension=i*64,stream=i*256*1024;
        d=desc(dimension,dimension,16,stream);
        nvdec_h264_job_layout j;assert(nvdec_h264_job_plan(&j,&d,stream)==KH264_OK);
        assert(j.total_bytes>=largest);largest=j.total_bytes;
        cycle(j.total_bytes,0x100000000ull+(u64)i*0x10000000,largest,base_mapped);
        assert(v.used<=v.pages);
    }
    assert(largest==maxjob.total_bytes);u32 peak=v.used;
    for(unsigned i=0;i<128;i++){
        unsigned dimension=i%2?4096:64+(i%63)*64;
        unsigned stream=i%2?16u*1024*1024:1+i*32768;
        d=desc(dimension,dimension,i%17,stream);
        nvdec_h264_job_layout j;assert(nvdec_h264_job_plan(&j,&d,stream)==KH264_OK);
        cycle(j.total_bytes,0x200000000ull+(u64)i*0x10000000,largest,base_mapped);
        assert(v.used==peak); /* No page-table leak after high-water allocation. */
    }
    printf("PASS actual NVDEC VMM: job=%u bytes; baseline=%u tables; peak=%u/256 tables; 192 grow/shrink cycles, all alias PTEs checked\n",
           maxjob.total_bytes,baseline,peak);
    return 0;
}
'''
    run_test(c, 'nvdec_user_vmm')


if __name__ == '__main__':
    main()
