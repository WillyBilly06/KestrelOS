#!/usr/bin/env python3
"""Actual OS registry policy and official private USERD allocation switches.

Validates USERD-only coherent-system placement for RM scrub/watchdog channels;
does not claim that physical hardware forward progress is proven offline.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT=Path(__file__).resolve().parents[1]
OFFICIAL=ROOT/'out/nvidia-open-595.99.02/src/nvidia'


def switch_after(src, marker):
    start=src.index('switch (',src.index(marker))
    brace=src.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(src[end]=='{')-(src[end]=='}');end+=1
    return src[start:end]


def main():
    os=(ROOT/'kernel/nvrm_os.c').read_text()
    regs=(OFFICIAL/'interface/nvrm_registry.h').read_text()
    registry=(OFFICIAL/'arch/nvalloc/unix/src/registry.c').read_text()
    gpu=(OFFICIAL/'src/kernel/gpu/gpu_registry.c').read_text()
    mem=(OFFICIAL/'src/kernel/gpu/mem_mgr/arch/maxwell/mem_utils_gm107.c').read_text()
    watchdog=(OFFICIAL/'src/kernel/gpu/rc/kernel_rc_watchdog.c').read_text()
    assert re.search(r'#define NV_REG_STR_RM_INST_LOC_USERD\s+17:16',regs)
    assert re.search(r'#define NV_REG_STR_RM_INST_LOC_COH\s+\(0x00000001\)',regs)
    # Local overrides take precedence, default global policy is still visible
    # to each GPU, and global entries are included in the packed GSP registry.
    find=registry[registry.index('static nv_reg_entry_t* regFindRegistryEntry'):registry.index('NV_STATUS RmWriteRegistryDword')]
    assert find.index('tmp = nvp->pRegistry')<find.index('tmp = the_registry')
    assert 'regCountEntriesAndSize(&numEntries, &totalSize, the_registry);' in registry
    assert re.search(r'regCopyEntriesToPackedBuffer\(pRegTable,\s*the_registry,',registry)
    assert 'osReadRegistryDword(pGpu, NV_REG_STR_RM_INST_LOC,   &pGpu->instLocOverrides);' in gpu
    # Confine this platform policy to one sparse key; no all-instance relocation.
    init=function(os.replace('NV_STATUS os_registry_init(', 'int os_registry_init('),'os_registry_init')
    c=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t NvU32;typedef int NV_STATUS;
#define NV_OK 0
#define kwarn(...) ((void)0)
static unsigned dwords,strings;static NvU32 instance;
static NV_STATUS RmWriteRegistryDword(void *gpu,const char *name,NvU32 value){
    assert(!gpu);dwords++;
    if(!strcmp(name,"EnableGpuFirmwareLogs"))assert(value==1);
    else if(!strcmp(name,"RmGspPreserveUnloadLogs"))assert(value==3);
    else if(!strcmp(name,"RmLogonRC"))assert(value==1);
    else if(!strcmp(name,"RMForceStaticBar1"))assert(value==0);
    else {assert(!strcmp(name,"RMInstLoc") && value==0x10000u);instance=value;}
    return NV_OK;
}
static NV_STATUS RmWriteRegistryString(void *gpu,const char *name,const char *value,NvU32 size){
    assert(!gpu && !strcmp(name,"RmMsg") && value && size==strlen(value)+1);strings++;return NV_OK;
}
'''+init+r'''
#define DRF_VAL(a,b,c,value) (((value)>>16)&3u)
#define DRF_DEF(a,b,c,value) DRF_VALUE##value
#define DRF_VALUE_PCI 0x111u
#define DRF_VALUE_VIDMEM 0x222u
#define NV01_MEMORY_SYSTEM 0x3eu
#define NV01_MEMORY_LOCAL_USER 0x40u
#define ADDR_SYSMEM 1u
#define ADDR_FBMEM 2u
#define NV_REG_STR_RM_INST_LOC_USERD_DEFAULT 0u
#define NV_REG_STR_RM_INST_LOC_USERD_COH 1u
#define NV_REG_STR_RM_INST_LOC_USERD_NCOH 2u
#define NV_REG_STR_RM_INST_LOC_USERD_VID 3u
#define NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM 1u
#define IS_MIG_IN_USE(gpu) 0
#define DRF_VALUE_YES 0x400u
typedef struct {NvU32 instLocOverrides;} Gpu;
typedef struct {NvU32 attr,flags;} Memory;
static NvU32 scrub_address(NvU32 value){
    Gpu state={value},*pGpu=&state;NvU32 userdAddrSpace;
'''+switch_after(mem,'//Fetch the physical location of userD')+r'''
    return userdAddrSpace;
}
static NvU32 scrub_allocation(NvU32 value){
    Gpu state={value},*pGpu=&state;Memory memAllocParams={0};
    NvU32 userdMemClass=NV01_MEMORY_LOCAL_USER;
'''+switch_after(mem,'static NV_STATUS\n_memUtilsAllocateUserD')+r'''
    if((value>>16&3u)==1u || (value>>16&3u)==2u){
        assert(memAllocParams.attr==DRF_VALUE_PCI && !memAllocParams.flags);
    }
    return userdMemClass;
}
static NvU32 watchdog_allocation(NvU32 value){
    Gpu state={value},*pGpu=&state;Memory alloc={0},*pMem=&alloc;
    NvU32 userdMemClass=NV01_MEMORY_LOCAL_USER;
'''+switch_after(watchdog,'// Apply registry overrides to USERD.')+r'''
    if((value>>16&3u)==1u || (value>>16&3u)==2u)assert(pMem->attr==DRF_VALUE_PCI);
    return userdMemClass;
}
int main(void){
    assert(os_registry_init()==NV_OK && dwords==5 && strings==1);
    assert((instance&~(3u<<16))==0); /* every unrelated field remains DEFAULT */
    assert(scrub_address(instance)==ADDR_SYSMEM);
    assert(scrub_allocation(instance)==NV01_MEMORY_SYSTEM);
    assert(watchdog_allocation(instance)==NV01_MEMORY_SYSTEM);
    assert(scrub_address(0)==ADDR_FBMEM);
    assert(scrub_allocation(0)==NV01_MEMORY_LOCAL_USER);
    assert(watchdog_allocation(0)==NV01_MEMORY_LOCAL_USER);
    assert(scrub_address(3u<<16)==ADDR_FBMEM);
    assert(scrub_allocation(3u<<16)==NV01_MEMORY_LOCAL_USER);
    assert(watchdog_allocation(3u<<16)==NV01_MEMORY_LOCAL_USER);
    puts("PASS actual USERD-only registry policy and official scrub/watchdog sysmem allocation branches; host/GSP registry path checked");
    return 0;
}
'''
    run_test(c,'nvrm_userd_policy')


if __name__=='__main__':main()
