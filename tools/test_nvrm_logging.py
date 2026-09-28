#!/usr/bin/env python3
"""Execute actual NVIDIA log forwarding at the real klog message bound."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'kernel/nvrm_os.c').read_text()
    header = (ROOT / 'kernel/klog.h').read_text()
    limit = int(re.search(r'#define KLOG_MSG_MAX\s+(\d+)', header)[1])
    official = (ROOT / 'out/nvidia-open-595.99.02/kernel-open/common/inc/os-interface.h').read_text()
    upstream = ROOT / 'out/nvidia-open-595.99.02/src/nvidia'
    trace = (upstream / 'src/kernel/gpu/disp/disp_objs.c').read_text()
    port = (upstream / 'inc/libraries/nvport/inline/debug_unix_kernel_os.h').read_text()
    assert 'NV_PRINTF(LEVEL_INFO, "class: 0x%x cmd 0x%x\\n"' in trace
    assert '#define portDbgPrintf(fmt, ...) nv_printf(0xFFFFFFFF, fmt, ##__VA_ARGS__)' in port
    forcing=(upstream/'src/kernel/diagnostics/nvlog_printf.c').read_text()
    levels=(upstream/'inc/libraries/utils/nvprintf_level.h').read_text()
    assert 'return bForce ? LEVEL_FATAL : level;' in forcing
    assert 'portDbgExPrintfLevel(_nvDbgForceLevel(force, debuglevel)' in forcing
    assert re.search(r'#define LEVEL_FATAL\s+0x6\b',levels)
    for name, value in [('INFO',0),('SETUP',1),('USERERRORS',2),('WARNINGS',3),('ERRORS',4)]:
        assert int(re.search(r'#define NV_DBG_'+name+r'\s+(0x[0-9a-f]+)',official)[1],16)==value
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint32_t NvU32;
typedef unsigned NV_STATUS;
typedef struct { int unused; } nv_state_t;
#define NV_OK 0
#define NV_ERR_INVALID_ARGUMENT 1
#define KLOG_INFO 1
#define KLOG_WARN 2
#define KLOG_ERROR 3
'''+f'#define KLOG_MSG_MAX {limit}\n'+r'''
static char saved[64][KLOG_MSG_MAX];static int levels[64],entries;
static void klog(int severity,const char *tag,const char *fmt,...){
    assert(!strcmp(tag,"nvrm")&&entries<64);
    va_list ap;va_start(ap,fmt);int n=vsnprintf(saved[entries],KLOG_MSG_MAX,fmt,ap);va_end(ap);
    assert(n>=0&&n<KLOG_MSG_MAX);levels[entries++]=severity;
}
'''+function(src, 'nvrm_log_text')+'\n'
    # out_string is intentionally a one-line wrapper in production.
    c += re.search(r'^void out_string\([^\n]+', src, re.M)[0]+'\n'
    c += function(src, 'nvrm_display_entry_trace')+'\n'
    c += function(src, 'nv_printf')+'\n'+function(src, 'nv_log_error')+r'''
static NV_STATUS xid(NvU32 code,const char *fmt,...) {
    va_list ap;va_start(ap,fmt);NV_STATUS result=nv_log_error(NULL,code,fmt,ap);va_end(ap);
    return result;
}
static void reset(void){memset(saved,0,sizeof saved);entries=0;}
static void equal_payload(const char *text,int severity){
    unsigned at=0;
    for(int i=0;i<entries;i++) {
        assert(levels[i]==severity);
        const char *p=saved[i];if(i&&p[0]=='+'&&p[1]==' ')p+=2;
        while(*p){while(text[at]=='\n')at++;assert(*p++==text[at++]);}
    }
    while(text[at]=='\n')at++;assert(!text[at]);
}
int main(void){
    char text[2048];
    for(unsigned n=0;n<sizeof text;n++) {
        for(unsigned i=0;i<n;i++)text[i]=(char)('a'+i%26);text[n]=0;
        reset();out_string(text);equal_payload(text,KLOG_INFO);
        if(n<512)for(unsigned level=0;level<7;level++) {
            reset();assert(nv_printf(level,"%s",text)==(int)n);
            const int expected[]={KLOG_INFO,KLOG_INFO,KLOG_ERROR,KLOG_WARN,KLOG_ERROR,KLOG_ERROR,KLOG_ERROR};
            equal_payload(text,expected[level]);
        }
    }
    const char *assertion="NVRM: GPU0 nvAssertOkFailedNoLog: Assertion failed: Call timed out [NV_ERR_TIMEOUT] (0x00000065) returned from ceutilsMemset(pScrubber->pCeUtils, &memsetParams) @ mem_scrub.c:1234\n";
    reset();out_string(assertion);assert(entries==2);equal_payload(assertion,KLOG_INFO);
    assert(strstr(saved[1],":1234")); // complete text reconstructed above
    reset();out_string("\nalpha\n\nbeta\n");assert(entries==2);equal_payload("alphabeta",KLOG_INFO);
    reset();out_string(NULL);assert(!entries);
    const char *trace="NVRM: GPU0 dispapiControl_IMPL: class: 0x73 cmd 0x73138f\n";
    reset();nv_printf(0xffffffffu,"%s",trace);equal_payload(trace,KLOG_INFO);
    reset();nv_printf(6,"%s",trace);equal_payload(trace,KLOG_INFO);
    reset();nv_printf(4,"%s",trace);equal_payload(trace,KLOG_ERROR);
    reset();nv_printf(0xffffffffu,"%s",assertion);equal_payload(assertion,KLOG_ERROR);
    reset();nv_printf(6,"%s",assertion);equal_payload(assertion,KLOG_ERROR);
    const char *bad[]={"NVRM: GPU dispapiControl_IMPL: class: 0x73 cmd 0x73138f",
      "NVRM: GPU0 dispapiControl_IMPL: class: 0x cmd 0x73138f",
      "NVRM: GPU0 dispapiControl_IMPL: class: 0x73 cmd 0x",
      "NVRM: GPU0 dispapiControl_IMPL: class: 0x73 cmd 0x73138f failed",
      "NVRM: GPU0 dispapiControl_IMPL: class: 0x73 cmd 0x73138f\nAssertion failed",
      "NVRM: GPU0 dispapiControl_IMPL: class: 0x123456789 cmd 0x73138f"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {
        reset();nv_printf(0xffffffffu,"%s",bad[i]);equal_payload(bad[i],KLOG_ERROR);
        reset();nv_printf(6,"%s",bad[i]);equal_payload(bad[i],KLOG_ERROR);
    }
    memset(text,'z',sizeof text-1);text[sizeof text-1]=0;
    reset();assert(nv_printf(0,"%s",text)==2047);assert(entries==5);
    assert(strstr(saved[4],"remainder unavailable"));
    char expected[600];
    for(unsigned n=0;n<512;n++) {
        for(unsigned i=0;i<n;i++)text[i]=(char)('a'+i%26);text[n]=0;
        snprintf(expected,sizeof expected,"Xid 65: %s",text);
        reset();assert(xid(65,"%s",text)==NV_OK);equal_payload(expected,KLOG_ERROR);
    }
    reset();assert(xid(0xffffffffu,"engine=%#x\nmethod=%#x trailing-fault-detail",0x1c,0x300)==NV_OK);
    equal_payload("Xid 4294967295: engine=0x1c\nmethod=0x300 trailing-fault-detail",KLOG_ERROR);
    reset();assert(xid(65,NULL)==NV_OK);equal_payload("Xid 65: [error detail unavailable]",KLOG_ERROR);
    memset(text,'z',sizeof text-1);text[sizeof text-1]=0;
    reset();assert(xid(65,"%s",text)==NV_OK);
    assert(strstr(saved[entries-1],"remainder unavailable"));
    assert(strstr(saved[0],"Xid 65:"));
    for(int i=0;i<entries;i++)assert(levels[i]==KLOG_ERROR);
    puts("PASS Xid forwarding: 512 lengths, multiline detail, full unsigned code, NULL format, explicit truncation; no silent klog-tail loss");
    puts("PASS NVIDIA logging: 2048 source lengths, 3584 formatted severity cases, exact klog bound, complete assertion suffix, newline handling and explicit formatting truncation");
}
'''
    run_test(c, 'nvrm_logging')


if __name__ == '__main__':
    main()
