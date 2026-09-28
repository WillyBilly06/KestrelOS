"""Real output-timing ABI dispatcher, wrappers and immutable user copy."""
import re
from test_gpu_stable_candidate import ROOT,function,run_test

def main():
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
#define SYS_FRAMEBUFFER 99
'''
    code+='#include "'+(ROOT/'include/kestrel/display_info.h').as_posix()+'"\n'
    abi=(ROOT/'include/kestrel/syscall.h').read_text()
    for name,value in [('FB_APPLY_OUTPUT_MODE',15),('FB_CONFIRM_OUTPUT_MODE',16),('FB_REVERT_OUTPUT_MODE',17)]:
        assert int(re.search(r'^#define '+name+r'\s+(\d+)',abi,re.M)[1])==value
        code+=f'#define {name} {value}\n'
    code+=r'''
typedef struct {u32 pid;u64 gpu_owner_id;bool resource_closing;}proc_t;
static proc_t process;
static proc_t *proc_shared(proc_t *p){return p;}
static u32 fb_owner_pid;
static bool native,busy,held,bad_read,mutate;
static unsigned begins,ends,copies,calls;
static s64 verdict;
static kdisplay_output_mode_request_t request,expected;
static u64 expected_token=UINT64_C(0x123456789);
static bool last_confirm;
static bool nvkms_kapi_runtime_selected(void){return native;}
static bool nv_render_try_begin(void){begins++;if(busy)return false;assert(!held);held=true;return true;}
static void nv_render_end(void){assert(held);held=false;ends++;}
static bool user_copy(void *dst,u64 src,size_t size,bool write){
    assert(held&&!write&&src==(uintptr_t)&request&&size==sizeof request);copies++;
    if(bad_read)return false;
    memcpy(dst,&request,size);if(mutate)memset(&request,0xcc,sizeof request);return true;
}
static s64 nvkms_kapi_apply_output_mode(u64 owner,const kdisplay_output_mode_request_t *q){
    assert(held&&owner==process.gpu_owner_id&&owner>UINT32_MAX&&q!=&request&&!memcmp(q,&expected,sizeof *q));
    calls++;return verdict?verdict:(s64)expected_token;
}
static int nvkms_kapi_finish_output_mode(u64 owner,u64 token,bool confirm){
    assert(held&&owner==process.gpu_owner_id&&token==expected_token);calls++;last_confirm=confirm;return (int)verdict;
}
'''
    src=(ROOT/'kernel/syscall.c').read_text()
    code+=function(src.replace('static s64 ','static long '),'sys_fb_output_mode')+'\n'
    start=src.index('        if (a1 == FB_APPLY_OUTPUT_MODE',src.index('case SYS_FRAMEBUFFER:'))
    end=src.index('\n        }',start)+10
    code+='static long dispatch(u64 a1,u64 a2){\n'+src[start:end].replace('sys_fb_output_mode(p,','sys_fb_output_mode(&process,')+'\nreturn -E_NOSYS;\n}\n'
    code+=r'''
static long syscall6(long nr,long a0,long op,long a2,long a3,long a4,long a5){
    assert(nr==SYS_FRAMEBUFFER&&!a0&&!a3&&!a4&&!a5);return dispatch(op,a2);
}
static int errno;
static long ret(long r){if(r<0){errno=(int)-r;return -1;}return r;}
'''
    libc=(ROOT/'user/libc/syscalls.c').read_text()
    code+=function(libc.replace('int64_t fb_apply_output_mode','long fb_apply_output_mode'),'fb_apply_output_mode')+'\n'+function(libc,'fb_finish_output_mode')+'\n'
    code+=r'''
static void reset(void){
    assert(!held);process=(proc_t){7,UINT64_C(0x177775555),false};fb_owner_pid=7;
    native=true;busy=bad_read=mutate=false;begins=ends=copies=calls=0;verdict=0;
    expected=request=(kdisplay_output_mode_request_t){42,0x800,4,3};
}
int main(void){
    reset();assert(fb_apply_output_mode(&request)==(s64)expected_token&&calls==1&&copies==1&&begins==ends);
    reset();mutate=true;assert(fb_apply_output_mode(&request)==(s64)expected_token&&calls==1);
    reset();assert(!fb_finish_output_mode(expected_token,true)&&last_confirm&&calls==1&&!copies&&begins==ends);
    reset();assert(!fb_finish_output_mode(expected_token,false)&&!last_confirm&&calls==1&&!copies&&begins==ends);
    for(unsigned i=0;i<3;i++){
        reset();if(i==0)process.pid=8;if(i==1)process.gpu_owner_id=0;if(i==2)process.resource_closing=true;
        assert(fb_apply_output_mode(&request)==-1&&errno==E_PERM&&!begins&&!calls);
    }
    reset();bad_read=true;assert(fb_apply_output_mode(&request)==-1&&errno==E_INVAL&&!calls&&begins==ends);
    reset();native=false;assert(fb_apply_output_mode(&request)==-1&&errno==E_NOSYS&&!begins);
    reset();busy=true;assert(fb_apply_output_mode(&request)==-1&&errno==E_BUSY&&!copies&&!ends);
    reset();verdict=-E_BUSY;assert(fb_apply_output_mode(&request)==-1&&errno==E_BUSY&&begins==ends);
    reset();verdict=-E_BUSY;assert(fb_finish_output_mode(expected_token,true)==-1&&errno==E_BUSY&&begins==ends);
    reset();assert(sys_fb_output_mode(&process,999,0)==-E_INVAL&&!calls&&!copies&&begins==ends);
    puts("PASS live timing syscall: real operations 15/16/17, immutable user copy, ownership, full-width tokens, native failure and balanced render guard");
}
'''
    code=re.sub(r'\blong\b','intptr_t',code)
    run_test(code,'output-mode-syscall')

if __name__=='__main__':main()
