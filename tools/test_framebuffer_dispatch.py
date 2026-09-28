"""Run production framebuffer dispatch helpers and libc calls, mocked hardware.

Checks operation-number uniqueness, immutable presentation metadata, exact
source bounds, failed-native-copy exclusion, and display-mode reachability.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

def main():
    abi=(ROOT/'include/kestrel/syscall.h').read_text()
    names=['FB_MAP','FB_RELEASE','FB_REACQUIRE','FB_UPDATE','FB_SETMODE','FB_CURSOR',
           'FB_CURSORAT','FB_FILL','FB_COPY','FB_ACCEL','FB_PRESENT','FB_DISPLAYMODE','FB_CHECK_CONFIGURATION',
           'FB_PREPARE_CONFIGURATION','FB_CANCEL_CONFIGURATION']
    values={n:int(re.search(r'^#define '+n+r'\s+(\d+)',abi,re.M)[1]) for n in names}
    assert len(set(values.values()))==len(values),values
    assert values['FB_PRESENT']==10 and values['FB_DISPLAYMODE']==11
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
typedef uint64_t u64;typedef uint32_t u32;typedef int64_t s64;
#define E_PERM 1
#define E_NOENT 2
#define E_BUSY 16
#define E_IO 5
#define E_NODEV 19
#define E_INVAL 22
#define E_NOSYS 38
#define KDISPLAY_MAX_OUTPUTS 8u
#define KDISPLAY_CONNECTED 1u
#define KDISPLAY_STALE 16u
#define KDISPLAY_DETECTED_ONLY 32u
typedef struct {unsigned flags,reserved;} kdisplay_output_t;
static unsigned output_flags=KDISPLAY_CONNECTED;
static bool output_epoch_race;
typedef struct {u64 back;u32 stride;int x,y,w,h;} kpresent_t;
typedef struct {const u32*back;u32*front;u32 back_stride,front_stride;int x,y,w,h;}present_job_t;
static int fb_owner_pid=7;
static u32 pixels[160],front[160];
static u32 fw=16,fh=10,pitch=64,outputs=3;
static bool native=true,success=true,attached=true,extend=false,have_fb=true;
static unsigned gpu_calls,bands,checks;static int selected=-1,workers=19;
static u64 allowed_bytes=sizeof pixels,last_need;static bool valid_request=true,valid_data=true;
static kpresent_t request;static bool mutate;
static bool user_range_ok(u64 p,size_t n,bool write){
    assert(!write);checks++;
    if(p==(uintptr_t)&request)return n==sizeof request&&valid_request;
    last_need=n;
    if(mutate){memset(&request,0xff,sizeof request);}
    u64 base=(uintptr_t)pixels;
    return p>=base&&p-base<=allowed_bytes&&n<=allowed_bytes-(p-base)&&valid_data;
}
static bool user_copy(void *buffer,u64 address,size_t bytes,bool write){
    assert(!write&&buffer&&address==(uintptr_t)&request&&bytes==sizeof request);
    if(!user_range_ok(address,bytes,write))return false;
    memcpy(buffer,(const void*)address,bytes);return true;
}
static u64 console_framebuffer(u32*w,u32*h,u32*p){*w=fw;*h=fh;*p=pitch;return have_fb?(uintptr_t)front:0;}
static bool nvkms_kapi_runtime_selected(void){return native;}
static bool nvkms_kapi_display_output(u32 i,kdisplay_output_t*out){out->flags=output_flags;out->reserved=output_epoch_race?i:1;return i<outputs;}
static bool nvkms_kapi_runtime_present(const u32*src,u32 w,u32 h,u32 stride,int x,int y){
    assert(src==pixels+y*stride+x&&w==3&&h==2&&stride==16&&x==2&&y==3);gpu_calls++;return success;
}
static int smp_worker_count(void){return workers;}
static u64 read_cr3(void){return 123;}
static void smp_run_in(void (*fn)(void*,int,int),void *arg,int n,u64 cr3){assert(cr3==123&&n>=1&&n<=2);bands++;for(int i=0;i<n;i++)fn(arg,i,n);}
static bool svga3d_second_attached(void){return attached;}
static bool svga3d_extend_active(void){return extend;}
static void svga3d_set_second_mode(int m){selected=m;}
'''
    code+='\n'.join(f'#define {n} {v}' for n,v in values.items())+'\n'
    src=(ROOT/'kernel/syscall.c').read_text()
    for name in ('present_band','sys_fb_present','sys_fb_displaymode'):
        code+=function(src.replace('static s64 ','static long '),name)+'\n'
    # Take the actual dispatcher arms: the historical collision made the
    # display arm unreachable despite both helpers looking individually valid.
    code+='static long dispatch(u64 a1,u64 a2){\n'
    for op in ('FB_PRESENT','FB_DISPLAYMODE'):
        start=src.index('        if (a1 == '+op+') {',src.index('case SYS_FRAMEBUFFER:'))
        end=src.index('\n        }',start)+10
        code+=src[start:end].replace('proc_shared(p)->pid','7')+'\n'
    code+='return -E_NOSYS;\n}\n'
    code+=r'''
#define SYS_FRAMEBUFFER 77
static long syscall6(long nr,long a0,long op,long a2,long a3,long a4,long a5){
    assert(nr==SYS_FRAMEBUFFER&&!a0&&!a3&&!a4&&!a5);return dispatch(op,(u64)a2);
}
static int errno;
static long ret(long r){if(r<0){errno=(int)-r;return -1;}return r;}
'''
    libc=(ROOT/'user/libc/syscalls.c').read_text()
    code+=function(libc,'fb_set_display_mode')+'\n'+function(libc,'fb_present')+'\n'
    code+=r'''
static void reset(void){
    request=(kpresent_t){(uintptr_t)pixels,16,2,3,3,2};
    fw=16;fh=10;pitch=64;outputs=3;fb_owner_pid=7;
    native=success=attached=have_fb=valid_request=valid_data=true;extend=mutate=false;
    selected=-1;workers=19;checks=gpu_calls=bands=0;allowed_bytes=sizeof pixels;last_need=0;
    for(unsigned i=0;i<160;i++){pixels[i]=i+1;front[i]=0;}
}
int main(void){
    reset();assert(!fb_present(&request)&&gpu_calls==1&&!bands);
    assert(last_need==(16+3)*4); // excludes preceding rows and unused final-row padding
    reset();allowed_bytes=(4*16+5)*4;assert(!fb_present(&request));
    reset();allowed_bytes=(4*16+5)*4-1;assert(fb_present(&request)==-1&&errno==E_INVAL&&!gpu_calls&&!bands);
    reset();request.back=UINT64_MAX-199;assert(fb_present(&request)==-1&&errno==E_INVAL&&checks==1);
    reset();mutate=true;assert(!fb_present(&request)&&gpu_calls==1);
    reset();native=false;mutate=true;assert(!fb_present(&request)&&bands==1);
    for(int y=0;y<10;y++)for(int x=0;x<16;x++)assert(front[y*16+x]==(x>=2&&x<5&&y>=3&&y<5?pixels[y*16+x]:0));
    reset();success=false;assert(fb_present(&request)==-1&&errno==E_IO&&gpu_calls==1&&!bands);
    reset();fb_owner_pid=8;assert(fb_present(&request)==-1&&errno==E_PERM&&!checks);
    reset();valid_request=false;assert(fb_present(&request)==-1&&errno==E_INVAL&&!gpu_calls);
    reset();have_fb=false;assert(fb_present(&request)==-1&&errno==E_NODEV);
    reset();request.w=0;assert(!fb_present(&request)&&!gpu_calls);
    reset();fh=INT_MAX;request.y=INT_MAX-1;request.h=1;request.stride=UINT_MAX;
    assert(fb_present(&request)==-1&&errno==E_INVAL&&checks==1); // byte-count overflow refused before data lookup
    for(int which=0;which<10;which++){
        reset();switch(which){
        case 0:request.x=-1;break;case 1:request.y=-1;break;
        case 2:request.w=-1;break;case 3:request.h=-1;break;
        case 4:request.x=INT_MAX;request.w=INT_MAX;break;
        case 5:request.y=INT_MAX;request.h=INT_MAX;break;
        case 6:request.stride=4;break;case 7:pitch=63;break;
        case 8:valid_data=false;break;
        case 9:fw=fh=UINT_MAX;request.x=INT_MAX;request.w=INT_MAX;break;
        }
        assert(fb_present(&request)==-1&&errno==E_INVAL&&!gpu_calls&&!bands);
    }
    reset();assert(fb_set_display_mode(2)==1&&!checks&&!gpu_calls&&selected==-1);
    assert(fb_set_display_mode(18)==1&&fb_set_display_mode(19)==-1&&errno==E_NOENT);
    output_flags|=KDISPLAY_STALE;assert(fb_set_display_mode(18)==-1&&errno==E_BUSY);
    output_flags=KDISPLAY_CONNECTED|KDISPLAY_DETECTED_ONLY;assert(fb_set_display_mode(18)==-1&&errno==E_BUSY);
    output_flags=0;assert(fb_set_display_mode(18)==-1&&errno==E_BUSY);
    output_flags=KDISPLAY_CONNECTED;output_epoch_race=true;assert(fb_set_display_mode(18)==-1&&errno==E_BUSY);
    output_epoch_race=false;
    assert(fb_set_display_mode(4)==-1&&errno==E_INVAL);
    assert(dispatch(FB_DISPLAYMODE,UINT64_C(0x100000002))==-E_INVAL);
    outputs=1;assert(fb_set_display_mode(3)==-1&&errno==E_NOENT);
    fb_owner_pid=8;assert(fb_set_display_mode(2)==-1&&errno==E_PERM);
    reset();native=false;assert(!fb_set_display_mode(2)&&selected==2);
    assert(!fb_set_display_mode(17)&&selected==3);
    assert(fb_set_display_mode(18)==-1&&errno==E_NOENT);
    selected=-1;extend=true;assert(fb_set_display_mode(0)==1&&selected==-1);
    extend=false;attached=false;assert(fb_set_display_mode(0)==-1&&errno==E_NOSYS);
    puts("PASS framebuffer ABI: distinct libc/dispatcher routes, immutable metadata, exact source extent, overflow/owner/failure checks, no native-to-CPU demotion, real mode status");
}
'''
    # Kestrel is LP64; the Windows host is LLP64. Keep pointer-sized syscall
    # words while executing the otherwise unchanged production libc wrappers.
    code=re.sub(r'\blong\b','intptr_t',code)
    run_test(code,'framebuffer-dispatch')

if __name__=='__main__':main()
