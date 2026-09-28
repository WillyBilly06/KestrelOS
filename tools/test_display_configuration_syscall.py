"""Actual configuration-check libc/dispatcher/helper with modeled native plan.

The native planner/cache/layout are exercised by test_nvkms_hotplug.py.
This verifies the user-memory and transaction boundary, not a physical modeset.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

def source():
    abi=(ROOT/'include/kestrel/syscall.h').read_text()
    number=int(re.search(r'^#define FB_CHECK_CONFIGURATION\s+(\d+)',abi,re.M)[1])
    assert number==12
    code=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t u32;typedef uint64_t u64;typedef int64_t s64;
#define E_PERM 1
#define E_BUSY 16
#define E_INVAL 22
#define E_NOSYS 38
'''
    code+='#include "'+(ROOT/'include/kestrel/display_info.h').as_posix()+'"\n'
    code+=f'#define FB_CHECK_CONFIGURATION {number}\n'
    code+=r'''
static u32 fb_owner_pid=7;
static bool native=true,busy,held,bad_read,bad_write,mutate;
static unsigned begins,ends,checks,calls;static int verdict;
static kdisplay_configuration_t request,expected;
static kdisplay_configuration_check_t answer,sentinel;
static void *out_address;
static bool nvkms_kapi_runtime_selected(void){return native;}
static bool nv_render_try_begin(void){begins++;if(busy)return false;assert(!held);held=true;return true;}
static void nv_render_end(void){assert(held);held=false;ends++;}
static bool user_range_ok(u64 address,size_t size,bool write){
    assert(held);checks++;
    if(write){
        if(mutate)memset(&request,0xcc,sizeof request);
        return address==(uintptr_t)out_address&&size==sizeof answer&&!bad_write;
    }
    return address==(uintptr_t)&request&&size==sizeof request&&!bad_read;
}
static int nvkms_kapi_check_configuration(const kdisplay_configuration_t *q,kdisplay_configuration_check_t *out){
    assert(held&&q!=&request&&!memcmp(q,&expected,sizeof *q));calls++;
    if(verdict)return verdict;
    memset(out,0,sizeof *out);out->version=1;out->generation=q->generation;out->width=7680;out->height=1440;
    out->gpu_pitch=30720;out->gpu_bytes=44236800;out->output_count=q->count;return 0;
}
'''
    src=(ROOT/'kernel/syscall.c').read_text()
    code+=function(src.replace('static s64 ','static long '),'sys_fb_check_configuration')+'\n'
    start=src.index('        if (a1 == FB_CHECK_CONFIGURATION)',src.index('case SYS_FRAMEBUFFER:'))
    end=src.index('\n        }',start)+10
    code+='static long dispatch(u64 a1,u64 a2,u64 a3){\n'+src[start:end].replace('proc_shared(p)->pid','7')+'\nreturn -E_NOSYS;\n}\n'
    code+=r'''
#define SYS_FRAMEBUFFER 99
static long syscall6(long nr,long a0,long op,long a2,long a3,long a4,long a5){
    assert(nr==SYS_FRAMEBUFFER&&!a0&&!a4&&!a5);return dispatch(op,a2,a3);
}
static int errno;
static long ret(long r){if(r<0){errno=(int)-r;return -1;}return r;}
'''
    code+=function((ROOT/'user/libc/syscalls.c').read_text(),'fb_check_configuration')+'\n'
    code+=r'''
static void reset(void){
    assert(!held);fb_owner_pid=7;native=true;busy=bad_read=bad_write=mutate=false;
    begins=ends=checks=calls=verdict=0;
    memset(&request,0,sizeof request);request.version=1;request.generation=32;request.count=3;
    expected=request;memset(&sentinel,0xa5,sizeof sentinel);answer=sentinel;out_address=&answer;
}
int main(void){
    reset();assert(!fb_check_configuration(&request,&answer)&&calls==1&&begins==1&&ends==1&&!held);
    assert(answer.width==7680&&answer.generation==32);
    reset();mutate=true;assert(!fb_check_configuration(&request,&answer)&&answer.generation==32);
    reset();out_address=&request;mutate=true;
    assert(!fb_check_configuration(&request,(void*)&request)&&request.version==1&&request.generation==32);
    reset();fb_owner_pid=8;assert(fb_check_configuration(&request,&answer)==-1&&errno==E_PERM&&!begins&&!checks);
    reset();native=false;assert(fb_check_configuration(&request,&answer)==-1&&errno==E_NOSYS&&!begins);
    reset();busy=true;assert(fb_check_configuration(&request,&answer)==-1&&errno==E_BUSY&&!ends&&!checks);
    for(unsigned which=0;which<4;which++){
        reset();bad_read=which==0;bad_write=which==1;verdict=which==2?-E_BUSY:which==3?-E_INVAL:0;
        assert(fb_check_configuration(&request,&answer)==-1&&errno==(which==2?E_BUSY:E_INVAL));
        assert(begins==1&&ends==1&&!held&&!memcmp(&answer,&sentinel,sizeof answer));
        assert(calls==(which>=2));
    }
    puts("PASS configuration syscall: real libc/dispatcher operation 12, owner/guard, read/write bounds, immutable request, aliased output, failed preflight preserves output and balances guard");
}
'''
    return code

def main():
    code=re.sub(r'\blong\b','intptr_t',source()) # Kestrel LP64, Windows host LLP64
    run_test(code,'display-configuration-syscall')

if __name__=='__main__':main()
