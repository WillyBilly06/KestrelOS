#!/usr/bin/env python3
"""Exercise the actual external-PDB bridge and commit routing, with modeled RM.

This tests root selection, flush status and lock lifetime, not physical hardware.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    start = re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', text).start()
    start = text.rfind('\n', 0, start) + 1
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def test_vendor_root_selection(vas):
    c = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t NvU32; typedef uint8_t NvBool; typedef uint32_t NV_STATUS;
typedef int VAS_PTE_UPDATE_TYPE;
typedef struct {int id;} MEMORY_DESCRIPTOR;
typedef MEMORY_DESCRIPTOR MMU_WALK_MEMDESC;
typedef struct {NvU32 gpuInstance;} OBJGPU;
typedef struct {NvU32 gpuMask;} OBJVASPACE;
typedef struct {void *pRoot;} FMT;
typedef struct {void *pWalk;FMT *pFmt;} GVAS_GPU_STATE;
typedef struct {
    OBJVASPACE base;NvU32 flags;NvBool bIsExternallyOwned;
    MEMORY_DESCRIPTOR *pExternalPDB;GVAS_GPU_STATE state;
} OBJGVASPACE;
typedef int KernelGmmu;
#define NV_OK 0
#define NV_FALSE 0
#define NV_ERR_INVALID_STATE 64
#define NV_ERR_INVALID_ARGUMENT 31
#define GPU_GFID_PF 0
#define NV_GMMU_INVAL_SCOPE_ALL_TLBS 0
#define NV_GMMU_INVAL_SCOPE_LINK_TLBS 1
#define NV_GMMU_INVAL_SCOPE_NON_LINK_TLBS 2
#define VASPACE_FLAGS_INVALIDATE_SCOPE_NVLINK_TLB 0x8000
#define NVBIT(i) (1u<<(i))
#define staticCast(p,t) (&(p)->base)
#define NV_ASSERT_OR_RETURN(c,e) do {if(!(c))return (e);}while(0)
#define NV_ASSERT_OK_OR_RETURN(c) do {NV_STATUS s=(c);if(s)return s;}while(0)
static int hal_calls;
static MEMORY_DESCRIPTOR internal={1},external={2},*walker_root,*flushed;
static KernelGmmu mmu;
static NvBool gpumgrGetBcEnabledStatus(OBJGPU *g){(void)g;return 0;}
static GVAS_GPU_STATE *gvaspaceGetGpuState(OBJGVASPACE *v,OBJGPU *g){(void)g;return &v->state;}
static NV_STATUS vgpuIsCallingContextPlugin(OBJGPU *g,NvBool *p){(void)g;*p=0;return 0;}
static NV_STATUS vgpuGetCallingContextGfid(OBJGPU *g,NvU32 *p){(void)g;*p=0;return 0;}
#define GPU_GET_KERNEL_GMMU(g) ((void)(g),&mmu)
static void mmuWalkGetPageLevelInfo(void *walk,void *fmt,NvU32 index,
                                  const MMU_WALK_MEMDESC **root,NvU32 *size) {
    (void)walk;(void)fmt;assert(!index);*root=walker_root;*size=walker_root?16:0;
}
static NV_STATUS kgmmuInvalidateTlb_HAL(OBJGPU *g,KernelGmmu *k,MEMORY_DESCRIPTOR *root,
                                      NvU32 flags,int type,NvU32 gfid,NvU32 scope,NvBool bar) {
    (void)g;assert(k==&mmu && !flags && type==1 && !gfid && scope==2 && !bar);
    hal_calls++;flushed=root;return 0;
}
'''
    # Compile the vendor function bodies verbatim; only surrounding objects and
    # HAL are modeled. External registration is not hand-reimplemented here.
    c += '\nNV_STATUS\n' + function(vas, '_gvaspaceSetExternalPageDirBase')
    c += '\nNV_STATUS\n' + function(vas, 'gvaspaceInvalidateTlb_IMPL')
    c += r'''
int main(void) {
    OBJGPU gpu={0};FMT fmt={0};OBJGVASPACE vas={.base={1},.bIsExternallyOwned=1,.state={0,&fmt}};
    assert(!_gvaspaceSetExternalPageDirBase(&vas,&gpu,&external));
    assert(vas.pExternalPDB==&external);
    /* Externally owned case: successful return, no actual flush. */
    assert(!gvaspaceInvalidateTlb_IMPL(&vas,&gpu,1));
    assert(!hal_calls && !flushed);
    /* Even an internal walker root does not select the registered external PDB. */
    walker_root=&internal;
    assert(!gvaspaceInvalidateTlb_IMPL(&vas,&gpu,1));
    assert(hal_calls==1 && flushed==&internal && flushed!=vas.pExternalPDB);
    puts("PASS actual NVIDIA root-selection negative control: external PDB registered, success without flush; internal root is not external root (modeled objects/HAL)");
}
'''
    run_test(c, 'vendor_root_selection')


def main():
    os = (ROOT / 'kernel/nvrm_os.c').read_text()
    chan = (ROOT / 'kernel/nv_chan.c').read_text()
    vendor = ROOT / 'out/nvidia-open-595.99.02/src/nvidia'
    vas = (vendor / 'src/kernel/mem_mgr/gpu_vaspace.c').read_text()
    test_vendor_root_selection(vas)
    # Pin the reason not to use the ordinary RM control for external tables:
    # registration only sets pExternalPDB; invalidation asks the internal walker.
    assert 'pGVAS->pExternalPDB = pPDB;' in vas
    inval = vas[vas.index('gvaspaceInvalidateTlb_IMPL'):vas.index('gvaspaceGetVasInfo_IMPL')]
    assert 'mmuWalkGetPageLevelInfo' in inval and 'pExternalPDB' not in inval
    assert '__nvoc_class_id_KernelGmmu 0x29362fu' in (vendor / 'generated/g_kern_gmmu_nvoc.h').read_text()
    assert 'pThis->__kgmmuInvalidateTlb__ = &kgmmuInvalidateTlb_GM107;' in (vendor / 'generated/g_kern_gmmu_nvoc.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t NvU32; typedef uint64_t NvU64; typedef uint32_t NV_STATUS;
typedef uint8_t NvBool; typedef uint32_t u32; typedef uint64_t u64;
#define NV_FALSE 0
#define NV_OK 0
#define NV_ERR_INVALID_ARGUMENT 31
#define NV_ERR_INVALID_OBJECT 41
#define NVRM_NVOC_CLASS_OBJECT 0x497031u
#define NVRM_LOCK_MODULE_MEM 0x2000u
#define kerr(...) ((void)0)
static bool nvrm_adapter_ready=true;
static int gpu, object, child, gmmu, memmgr, descriptor;
static int api_lock, gpu_lock, flushes, fail;
static NvU64 physical=0x120000;
NV_STATUS rmapiLockAcquire(NvU32 flags,NvU32 module) {
    assert(!flags && module==NVRM_LOCK_MODULE_MEM && !api_lock);
    if(fail==1)return 61; api_lock=1;return 0;
}
void rmapiLockRelease(void) {assert(api_lock && !gpu_lock);api_lock=0;}
NV_STATUS rmGpuLocksAcquire(NvU32 flags,NvU32 module) {
    assert(api_lock && !gpu_lock && !flags && module==NVRM_LOCK_MODULE_MEM);
    if(fail==2)return 62;gpu_lock=1;return 0;
}
NvU32 rmGpuLocksRelease(NvU32 flags,void *p) {
    assert(api_lock && gpu_lock && !flags && !p);gpu_lock=0;return 0;
}
void *gpumgrGetSomeGpu(void) {assert(api_lock && gpu_lock);return fail==3?0:&gpu;}
static void *nvrm_memory_manager(void) {return fail==4?0:&memmgr;}
void *objDynamicCastById_IMPL(void *p,NvU32 type) {
    if(type==NVRM_NVOC_CLASS_OBJECT){assert(p==&gpu);return &object;}
    assert(type==0x29362fu && p==&child);return fail==5?0:&gmmu;
}
void *objGetChild_IMPL(void *p) {assert(p==&object);return &child;}
void *objGetSibling_IMPL(void *p) {assert(p==&child);return 0;}
void *memmgrMemUtilsGetMemDescFromHandle_IMPL(void *m,NvU32 client,NvU32 memory) {
    assert(api_lock && gpu_lock && m==&memmgr && client==42 && memory==99);
    return fail==6?0:&descriptor;
}
NvU32 memdescGetAddressSpace(void *p) {assert(p==&descriptor);return fail==7?1:2;}
NvU64 memdescGetPhysAddr(void *p,void *at,NvU64 offset) {
    assert(p==&descriptor && at==(void *)(uintptr_t)1 && !offset);return physical;
}
NV_STATUS kgmmuInvalidateTlb_GM107(void *g,void *mmu,void *root,NvU32 flags,
                                 NvU32 downgrade,NvU32 gfid,NvU32 scope,NvBool bar) {
    assert(api_lock && gpu_lock && g==&gpu && mmu==&gmmu && root==&descriptor);
    assert(!flags && downgrade==1 && !gfid && !scope && !bar);
    flushes++;return fail==8?68:0;
}
'''
    c += function(os, 'nvrm_invalidate_external_root')
    c += r'''
#define CH_COUNT 3
#define NV2080_CTRL_CMD_DMA_INVALIDATE_TLB 123
#define RM_SUBDEVICE 456
typedef struct {u64 root_gpu;} nv_vmm_t;
typedef struct {bool host_api;u32 client;} nv_rm_t;
typedef struct {int unused;} nv_card_t;
static struct {nv_vmm_t vmm;} channels[CH_COUNT];
static u32 nv_last_control_status;
static unsigned controls;
static u32 h_vaspace(int i){return 70+i;}
static u32 h_tablesvram(int i){assert(i==1);return 99;}
static bool nv_rm_control(nv_card_t *c,nv_rm_t *rm,u32 h,u32 cmd,
                          void *p,u32 size,void *out,u32 osize,void *got) {
    (void)c;assert(!rm->host_api && h==456 && cmd==123 && size==16);
    assert(((u32 *)p)[3]==71 && !out && !osize && !got);controls++;return fail!=9;
}
'''
    c += function(chan, 'nv_vmm_invalidate')
    c += r'''
int main(void) {
    assert(!nvrm_invalidate_external_root(42,99,physical));assert(flushes==1);
    assert(nvrm_invalidate_external_root(42,99,physical+1)==31);
    assert(nvrm_invalidate_external_root(42,99,physical+4096)==31);
    assert(nvrm_invalidate_external_root(0,99,physical)==31);
    assert(nvrm_invalidate_external_root(42,0,physical)==31);
    for(fail=1;fail<=8;fail++) {
        int before=flushes;
        assert(nvrm_invalidate_external_root(42,99,physical)!=0);
        assert(!api_lock && !gpu_lock);
        assert(flushes==before+(fail==8));
    }
    fail=0;channels[1].vmm.root_gpu=physical;
    nv_rm_t rm={true,42};nv_card_t card={0};
    assert(nv_vmm_invalidate(&card,&rm,71,"test"));assert(!controls);
    fail=8;assert(!nv_vmm_invalidate(&card,&rm,71,"test"));assert(!controls);
    fail=0;assert(!nv_vmm_invalidate(&card,&rm,999,"test"));
    rm.host_api=false;assert(nv_vmm_invalidate(&card,&rm,71,"direct"));
    fail=9;assert(!nv_vmm_invalidate(&card,&rm,71,"direct"));
    assert(controls==2 && !api_lock && !gpu_lock);
    puts("PASS actual external-root TLB bridge: exact PDB descriptor, all-TLB downgrade, fail-closed errors, balanced locks and host/direct routing (modeled RM)");
}
'''
    run_test(c, 'external_tlb')


if __name__ == '__main__':
    main()
