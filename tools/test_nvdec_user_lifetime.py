#!/usr/bin/env python3
"""Exercise actual application NVDEC allocation/cleanup with faulted RM/VMM.

Models CPU page tables separately from GPU-visible mappings, including partial
failed maps/unmaps. This proves resource lifetime control flow, not GPU execution.
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
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
#define CH_NVDEC 4
typedef struct { unsigned marker; } vmm_t;
typedef struct { bool submit_failed; void *card,*rm; vmm_t vmm; } nv_channel_t;
'''
    for name in ('RM_DEVICE', 'NVOS32_TYPE_IMAGE', 'NVOS32_ATTR_VIDMEM_CONTIGUOUS',
                 'NVOS32_ATTR2_GPU_CACHEABLE_NO', 'NVOS32_ALLOC_FLAGS_NO_SCANOUT',
                 'NVOS32_ALLOC_FLAGS_ALIGN_FORCE', 'NV01_MEMORY_LOCAL_USER',
                 'NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR', 'NV0041_APERTURE_VIDMEM',
                 'NV_VIDEO_PTE_KIND', 'VA_NVDEC_USER', 'VA_NVDEC_USER_TILED',
                 'H_NVDEC_USER_VRAM'):
        c += re.search(r'^#define ' + name + r'\s+[^\n]+', src, re.M)[0] + '\n'
    for name in ('mem_alloc_params_t', 'phys_attr_params_t', 'nvdec_user_buffer_t'):
        end = src.index('} ' + name + ';') + len('} ' + name + ';')
        start = src.rfind('typedef struct {', 0, end)
        c += src[start:end] + '\n'
    c += 'static nvdec_user_buffer_t g_nvdec_user_buffer;\n'
    c += r'''
enum fault {
    NONE, ALLOC, PHYS, SHORT_PHYS, APERTURE, CONTIG, ALIGNMENT,
    LINEAR_MAP, TILED_MAP, MAP_COMMIT, TILED_UNMAP, TILED_COMMIT,
    LINEAR_UNMAP, LINEAR_COMMIT, FREE
};
static enum fault primary, cleanup;
static nv_channel_t ch;
static unsigned object, pte[2], visible[2], calls, allocs, frees, expected_bytes;
static u64 phys;
static char events[128];
static bool bad(enum fault f) { return primary==f || cleanup==f; }
static void event(char code) {
    assert(calls+1<sizeof events); events[calls++]=code; events[calls]=0;
}
static void owner(void *card,void *rm) { assert(card==ch.card && rm==ch.rm); }
static u32 h_vaspace(unsigned n) { assert(n==CH_NVDEC); return 0xdec0; }
static bool nv_rm_alloc(void *card,void *rm,u32 parent,u32 handle,u32 cls,
                        void *params,u32 size) {
    owner(card,rm); event('A'); allocs++;
    assert(!object && parent==RM_DEVICE && handle==H_NVDEC_USER_VRAM);
    assert(cls==NV01_MEMORY_LOCAL_USER && size==sizeof(mem_alloc_params_t));
    mem_alloc_params_t want={0}, *p=params;
    want.owner=0x4b455354u; want.type=NVOS32_TYPE_IMAGE;
    want.attr=NVOS32_ATTR_VIDMEM_CONTIGUOUS;
    want.attr2=NVOS32_ATTR2_GPU_CACHEABLE_NO<<2;
    want.flags=NVOS32_ALLOC_FLAGS_NO_SCANOUT|NVOS32_ALLOC_FLAGS_ALIGN_FORCE;
    want.size=expected_bytes; want.alignment=0x10000;
    assert(!memcmp(p,&want,sizeof want));
    /* Failed RM allocation may still have created the fixed-handle object. */
    object=1; return !bad(ALLOC);
}
static bool nv_rm_control(void *card,void *rm,u32 handle,u32 cmd,void *in,u32 size,
                          void *out,u32 capacity,u32 *got) {
    owner(card,rm); event('P'); assert(object && handle==H_NVDEC_USER_VRAM);
    assert(cmd==NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR && in==out);
    assert(size==sizeof(phys_attr_params_t) && capacity==size);
    phys_attr_params_t *p=out, zero={0}; assert(!memcmp(p,&zero,sizeof zero));
    p->mem_offset=phys+(bad(ALIGNMENT)?4096:0);
    p->mem_aperture=bad(APERTURE)?1:NV0041_APERTURE_VIDMEM;
    p->contig_segment_size=expected_bytes-(bad(CONTIG)?1:0);
    *got=bad(SHORT_PHYS)?size-1:size;
    return !bad(PHYS);
}
static bool map(vmm_t *v,u64 va,u64 fb,u64 bytes,bool vid,bool a,bool b,
                unsigned index) {
    assert(v==&ch.vmm && object && bytes==expected_bytes && fb==phys);
    assert(vid && !a && !b && va==(index?VA_NVDEC_USER_TILED:VA_NVDEC_USER));
    assert(!pte[index] && !visible[index]); event(index?'T':'L');
    /* Count 1 means partial map; 2 means whole range. Both require unmap. */
    bool failed=bad(index?TILED_MAP:LINEAR_MAP);
    pte[index]=failed?1:2;
    return !failed;
}
static bool nv_vmm_map(vmm_t *v,u64 va,u64 fb,u64 bytes,bool vid,bool a,bool b) {
    return map(v,va,fb,bytes,vid,a,b,0);
}
static bool nv_vmm_map_kind(vmm_t *v,u64 va,u64 fb,u64 bytes,bool vid,bool a,
                            bool b,u32 kind) {
    assert(kind==NV_VIDEO_PTE_KIND); return map(v,va,fb,bytes,vid,a,b,1);
}
static bool nv_vmm_unmap(vmm_t *v,u64 va,u64 bytes) {
    assert(v==&ch.vmm && object && bytes==expected_bytes);
    assert(va==VA_NVDEC_USER || va==VA_NVDEC_USER_TILED);
    unsigned i=va==VA_NVDEC_USER_TILED;
    event(i?'t':'l');
    if (bad(i?TILED_UNMAP:LINEAR_UNMAP)) {
        pte[i]=1; return false; /* Possible partially completed unmap. */
    }
    pte[i]=0; return true;
}
static bool nv_vmm_commit(void *card,void *rm,vmm_t *v,u32 h,const char *why) {
    owner(card,rm); assert(v==&ch.vmm && h==0xdec0 && object);
    enum fault f;
    if (!strcmp(why,"application NVDEC buffers")) { event('C'); f=MAP_COMMIT; }
    else if (!strcmp(why,"video tiled unmap")) { event('U'); f=TILED_COMMIT; }
    else { assert(!strcmp(why,"video linear unmap")); event('V'); f=LINEAR_COMMIT; }
    if (bad(f)) {
        /* Failure does not guarantee that an invalidate did or didn't occur. */
        visible[0]|=pte[0]; visible[1]|=pte[1]; return false;
    }
    visible[0]=pte[0]; visible[1]=pte[1]; return true;
}
static bool nv_rm_free(void *card,void *rm,u32 parent,u32 handle) {
    owner(card,rm); event('F'); frees++;
    assert(object && parent==RM_DEVICE && handle==H_NVDEC_USER_VRAM);
    /* Never free with either CPU or stale GPU-visible mapping still present. */
    assert(!pte[0] && !pte[1] && !visible[0] && !visible[1]);
    if (bad(FREE)) return false;
    object=0; return true;
}
'''
    c += function(src, 'nvdec_user_release') + '\n'
    c += function(src, 'nvdec_user_allocate') + '\n'
    c += r'''
static void reset(enum fault f) {
    memset(&g_nvdec_user_buffer,0,sizeof g_nvdec_user_buffer);
    memset(pte,0,sizeof pte); memset(visible,0,sizeof visible);
    primary=f; cleanup=NONE; object=calls=allocs=frees=0; events[0]=0;
    ch=(nv_channel_t){.card=(void*)1,.rm=(void*)2};
    expected_bytes=0x30000; phys=0x1234560000ull;
}
static void clear_trace(void) { calls=0; events[0]=0; }
static void trace(const char *want) { assert(!strcmp(events,want)); }
static void empty(void) {
    nvdec_user_buffer_t zero={0};
    assert(!memcmp(&g_nvdec_user_buffer,&zero,sizeof zero));
    assert(!object && !pte[0] && !pte[1] && !visible[0] && !visible[1]);
}
static void quarantined(void) {
    assert(g_nvdec_user_buffer.quarantined && object);
    unsigned before=calls, allocations=allocs, deallocations=frees;
    assert(!nvdec_user_release(&ch));
    assert(!nvdec_user_allocate(&ch,expected_bytes));
    assert(calls==before && allocs==allocations && frees==deallocations);
}
int main(void) {
    reset(NONE);
    /* Dynamic-size reuse must release both aliases before reusing the handle. */
    for (unsigned n=0;n<1024;n++) {
        expected_bytes=(1u+((n*73u)%2047u))*0x10000u;
        phys=0x1234560000ull+(u64)n*0x10000u;
        clear_trace(); assert(nvdec_user_allocate(&ch,expected_bytes)); trace("APLTC");
        assert(g_nvdec_user_buffer.allocated && !g_nvdec_user_buffer.quarantined);
        assert(g_nvdec_user_buffer.linear_attempted && g_nvdec_user_buffer.tiled_attempted);
        assert(g_nvdec_user_buffer.fb==phys && g_nvdec_user_buffer.bytes==expected_bytes);
        assert(pte[0]==2 && pte[1]==2 && visible[0]==2 && visible[1]==2);
        unsigned before=calls;
        assert(!nvdec_user_allocate(&ch,expected_bytes+0x10000)); assert(calls==before);
        clear_trace(); assert(nvdec_user_release(&ch)); trace("tUlVF"); empty();
        assert(allocs==n+1 && frees==n+1);
        clear_trace(); assert(nvdec_user_release(&ch)); trace(""); empty();
    }
    const u32 invalid[]={0,1,0xffff,0x10001,0x7ffffff,0x80000000,0xffff0000,0xffffffff};
    for (unsigned n=0;n<sizeof invalid/sizeof invalid[0];n++) {
        reset(NONE); assert(!nvdec_user_allocate(&ch,invalid[n])); trace(""); empty();
    }
    /* The final valid slot below the two virtual aliases may be allocated. */
    reset(NONE); expected_bytes=(u32)(VA_NVDEC_USER_TILED-VA_NVDEC_USER)-0x10000;
    assert(nvdec_user_allocate(&ch,expected_bytes)); assert(nvdec_user_release(&ch)); empty();
    for (unsigned flag=0;flag<4;flag++) {
        reset(NONE);
        if(flag==0)g_nvdec_user_buffer.allocated=true;
        if(flag==1)g_nvdec_user_buffer.linear_attempted=true;
        if(flag==2)g_nvdec_user_buffer.tiled_attempted=true;
        if(flag==3)g_nvdec_user_buffer.quarantined=true;
        assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("");
    }
    for (enum fault f=PHYS;f<=ALIGNMENT;f++) {
        reset(f); assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("APF"); empty();
        primary=NONE; clear_trace(); assert(nvdec_user_allocate(&ch,expected_bytes));
        assert(nvdec_user_release(&ch)); empty();
    }
    reset(LINEAR_MAP); assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("APLlVF"); empty();
    reset(TILED_MAP); assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("APLTtUlVF"); empty();
    reset(MAP_COMMIT); assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("APLTCtUlVF"); empty();
    reset(ALLOC); assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("A");
    assert(!g_nvdec_user_buffer.allocated && !frees); quarantined();
    const char *failed_release[]={"t","tU","tUl","tUlV","tUlVF"};
    for(enum fault f=TILED_UNMAP;f<=FREE;f++) {
        reset(NONE); assert(nvdec_user_allocate(&ch,expected_bytes));
        clear_trace(); cleanup=f; assert(!nvdec_user_release(&ch));
        trace(failed_release[f-TILED_UNMAP]); quarantined();
        assert(g_nvdec_user_buffer.allocated);
        assert(g_nvdec_user_buffer.tiled_attempted==(f<=TILED_COMMIT));
        assert(g_nvdec_user_buffer.linear_attempted==(f<=LINEAR_COMMIT));
    }
    /* Every rollback stage must also quarantine when allocation failed first. */
    for(enum fault f=TILED_UNMAP;f<=FREE;f++) {
        reset(MAP_COMMIT); cleanup=f;
        assert(!nvdec_user_allocate(&ch,expected_bytes)); quarantined();
        char want[64]="APLTC";
        const char *suffix=failed_release[f-TILED_UNMAP];
        memcpy(want+5,suffix,strlen(suffix)+1); trace(want);
    }
    reset(PHYS); cleanup=FREE; assert(!nvdec_user_allocate(&ch,expected_bytes));
    trace("APF"); quarantined();
    reset(LINEAR_MAP); cleanup=LINEAR_COMMIT;
    assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("APLlV"); quarantined();
    reset(TILED_MAP); cleanup=TILED_COMMIT;
    assert(!nvdec_user_allocate(&ch,expected_bytes)); trace("APLTtU"); quarantined();
    /* A timed-out GPU might access either alias: do not touch/free either one. */
    reset(NONE); assert(nvdec_user_allocate(&ch,expected_bytes)); clear_trace();
    ch.submit_failed=true; assert(!nvdec_user_release(&ch)); trace(""); quarantined();
    assert(pte[0]==2 && pte[1]==2 && visible[0]==2 && visible[1]==2 && !frees);
    ch.submit_failed=false; /* A changed channel flag alone is NOT resource recovery. */
    assert(!nvdec_user_release(&ch)); trace(""); quarantined();
    reset(NONE); ch.submit_failed=true; assert(!nvdec_user_release(&ch)); trace("");
    assert(g_nvdec_user_buffer.quarantined);
    puts("PASS actual NVDEC job lifetime: 1024 varying-size reuses, partial maps, rollback faults, quarantine");
    return 0;
}
'''
    # Allocation is private; its only entrypoint rejects a failed channel first.
    caller = function(src, 'nv_nvdec_decode_idr')
    assert caller.index('ch->submit_failed || g_nvdec_user_buffer.quarantined') < caller.index('nvdec_user_allocate(')
    run_test(c, 'nvdec_user_lifetime')


if __name__ == '__main__':
    main()
