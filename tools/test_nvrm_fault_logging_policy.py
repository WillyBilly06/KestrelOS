#!/usr/bin/env python3
"""Check the native registry fault-log policy against NVIDIA's Linux default.

Executes only registry calls against host mocks; no RM, GPU, codec or VM runs.
"""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    upstream = ROOT / 'out/nvidia-open-595.99.02'
    linux = (upstream / 'kernel-open/nvidia/nv-reg.h').read_text()
    registry = (upstream / 'src/nvidia/interface/nvrm_registry.h').read_text()
    rc = (upstream / 'src/nvidia/src/kernel/gpu/rc/kernel_rc.c').read_text()
    assert '#define __NV_RM_LOGON_RC RmLogonRC' in linux
    assert 'NV_DEFINE_REG_ENTRY(__NV_RM_LOGON_RC, 1);' in linux
    assert 'NV_DEFINE_PARAMS_TABLE_ENTRY(__NV_RM_LOGON_RC)' in linux
    assert '#define NV_REG_STR_RM_DO_LOG_RC_DEFAULT' in registry
    assert 'NV_REG_STR_RM_DO_LOG_RC_DISABLE' in registry.split(
        '#define NV_REG_STR_RM_DO_LOG_RC_DEFAULT', 1)[1].splitlines()[0]
    report = rc[rc.index('krcReportXid_IMPL'):]
    assert 'if (GPU_GET_KERNEL_RC(pGpu)->bLogEvents)' in report
    assert 'portDbgPrintf("NVRM: Xid (' in report
    src = (ROOT / 'kernel/nvrm_os.c').read_text()
    c = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t NvU32;
typedef unsigned NV_STATUS;
#define NV_OK 0
static unsigned calls, warnings, fail_logging;
static const char *names[]={"EnableGpuFirmwareLogs","RmGspPreserveUnloadLogs",
    "RmLogonRC","RMForceStaticBar1","RMInstLoc"};
static const NvU32 values[]={1,3,1,0,0x10000};
static NV_STATUS RmWriteRegistryString(void *nv,const char *name,const char *value,NvU32 size) {
    assert(!nv && !calls && !strcmp(name,"RmMsg"));
    assert(size==strlen(value)+1);
    assert(!strcmp(value,"disp,kdisp,Head,Core,Supervisor,Promote,nvkms,evo,Notifier,Imp"));
    calls++; return NV_OK;
}
static NV_STATUS RmWriteRegistryDword(void *nv,const char *name,NvU32 value) {
    assert(!nv && calls>=1 && calls<=5);
    unsigned i=calls++-1;
    assert(!strcmp(name,names[i]) && value==values[i]);
    return i==2 && fail_logging ? 0x51 : NV_OK;
}
static void warning(const char *tag,const char *fmt,NV_STATUS status) {
    assert(!strcmp(tag,"nvrm"));
    assert(strstr(fmt,"could not enable RC/Xid detail logging"));
    assert(status==0x51); warnings++;
}
#define kwarn warning
'''+function(src, 'os_registry_init')+r'''
int main(void) {
    assert(os_registry_init()==NV_OK && calls==6 && !warnings);
    calls=0;fail_logging=1;
    assert(os_registry_init()==NV_OK && calls==6 && warnings==1);
    puts("PASS native RC logging policy: Linux default enabled globally, existing policies preserved, write failure reported without blocking GPU initialization");
}
'''
    run_test(c, 'nvrm_fault_logging_policy')


if __name__ == '__main__':
    main()
