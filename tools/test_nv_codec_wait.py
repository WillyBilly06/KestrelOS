#!/usr/bin/env python3
"""Exercise actual submission deadlines/cookies with a deterministic GPU edge.

No physical GPU is emulated: this proves bounded host control flow only.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main(compile_only=False):
    src = (ROOT / 'kernel/nv_chan.c').read_text()
    official = (ROOT / 'out/nvidia-open-595.99.02/src/nvidia/arch/nvalloc/unix/src/os.c').read_text()
    assert re.search(r'case NV_GPU_MODE_GRAPHICS_MODE:\s*\*pTimeoutUs = 4 \* 1000000;', official)
    notifier_source = (ROOT / 'out/nvidia-open-595.99.02/src/nvidia/src/kernel/gpu/rc/kernel_rc_notification.c').read_text()
    assert '0xffff /* notifierStatus */' in notifier_source
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#include "nv_error_notifier.h"
#define CH_NVENC 3
#define CH_NVDEC 4
#define GPFIFO_ENTRIES 64u
#define VA_PUSHBUF 0x200000000ull
#define VA_GPFIFO 0x100000000ull
#define RM_SUBDEVICE 1
#define NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL 1
#define NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN 2
#define RAMFC_GP_BASE 0
#define RAMFC_GP_BASE_HI 4
#define NV_USERMODE_DOORBELL 0xbb0090u
static void mock_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define kinfo(...) mock_log(__VA_ARGS__)
#define kwarn(...) mock_log(__VA_ARGS__)
#define kerr(...) mock_log(__VA_ARGS__)
typedef struct {bool host_api;volatile u8 *usermode;} rm_t;
typedef struct {
    bool submit_failed,userd_sysmem,work_submit_token_valid,userd_bar1,runlist_known;
    u32 pb_at,gp_put,work_submit_token,h_channel,chid,runlist_id;
    u64 ramfc_fb;
    volatile u8 *pushbuf;
    volatile u32 *gpfifo,*sem,*submit_bar1_flush;
    rm_t *rm;void *card;const char *name;
    volatile nv_error_notification_t *error_notifier;
} nv_channel_t;
typedef struct {u32 work_submit_token;} work_submit_token_params_t;
static nv_channel_t channels[5];static rm_t rm;
static volatile u64 g_uptime_ms;
static u64 now_us,signal_at,consume_at;
static unsigned publications,faults,doorbells;
static u32 payload,semaphore,ring[128],usermode[64];static u8 push[256];
static nv_channel_t *active;
static nv_error_notification_t notification;
static u64 error_at;
static void cache_flush(const volatile void *p,u32 size){
    (void)p;(void)size;
    if(now_us>=signal_at)semaphore=payload;
    if(now_us>=error_at){notification.info32=65;notification.info16=0x1c;notification.status=0xffff;}
}
static void timer_udelay(u32 us){
    bool native_fast=active->rm->host_api && active!=&channels[CH_NVENC] && active!=&channels[CH_NVDEC];
    assert(us==(native_fast?2u:50u));now_us+=us;g_uptime_ms=now_us/1000;
}
static u64 timer_now_us(void){return now_us;}
typedef struct {nv_channel_t *channel;u32 cookie;} nv_completion_wait_t;
/* This fixture isolates polling/deadline behavior, not scheduler switching. */
static bool nv_completion_wait_allowed(nv_channel_t *ch){(void)ch;return false;}
/* PRODUCTION_COMPLETION_PROBE */
static int sched_wait_device(bool (*probe)(void *),void *context,u64 deadline){
    (void)probe;(void)context;(void)deadline;assert(!"polling fixture must not sleep");return -1;
}
static void userd_write_gpput(nv_channel_t *ch,u32 put){
    assert(ch==active && ch->submit_failed && put==1);publications++;
    assert(ring[0]==((u32)VA_PUSHBUF+16));
    assert(ring[1]==(((u32)(VA_PUSHBUF>>32)&255u)|(8u<<10)));
}
static u32 userd_read_gpput(nv_channel_t *ch){return ch->gp_put;}
static u32 userd_read_gpget(nv_channel_t *ch){return now_us>=consume_at?ch->gp_put:UINT32_MAX;}
static u32 nv_fb_rd32(void *card,u64 off){(void)card;(void)off;return 0;}
static u32 nv_rd32(void *card,u32 off){(void)card;(void)off;return 0;}
static void nv_wr32(void *card,u32 off,u32 value){
    (void)card;(void)value;assert(off==NV_USERMODE_DOORBELL);doorbells++;
}
static bool nv_rm_control(void *card,rm_t *r,u32 h,u32 cmd,void *data,u32 size,void *out,u32 osize,u32 *got){
    (void)card;(void)r;(void)h;(void)cmd;(void)data;(void)size;(void)out;(void)osize;(void)got;
    assert(!"cached token/sysmem USERD must not need RM control");return false;
}
static void nv_channel_capture_fault(nv_channel_t *ch,u32 start,u32 bytes,u32 wanted){
    (void)wanted;
    assert(ch==active && ch->submit_failed && start==16 && bytes==32);faults++;
}
'''
    c = c.replace('#include "nv_error_notifier.h"',
                  (ROOT / 'kernel/nv_error_notifier.h').read_text())
    c = c.replace('/* PRODUCTION_COMPLETION_PROBE */', function(src, 'nv_completion_probe'))
    c += function(src, 'submit_and_wait')
    c += r'''
static void setup(unsigned channel,bool host,u64 signal,u32 value,u64 consumed){
    memset(channels,0,sizeof channels);memset(ring,0,sizeof ring);memset(usermode,0,sizeof usermode);
    active=&channels[channel];rm=(rm_t){host,(u8 *)usermode};
    *active=(nv_channel_t){.userd_sysmem=true,.work_submit_token_valid=true,.runlist_known=true,
        .pb_at=48,.work_submit_token=0x10203040,.chid=7,.runlist_id=6,
        .pushbuf=push,.gpfifo=ring,.sem=&semaphore,.rm=&rm,.name="test"};
    now_us=g_uptime_ms=0;signal_at=signal;payload=value;consume_at=consumed;
    publications=faults=doorbells=semaphore=0;
    memset(&notification,0,sizeof notification);error_at=UINT64_MAX;
    if(host && (channel==CH_NVENC || channel==CH_NVDEC))active->error_notifier=&notification;
}
static void check(unsigned channel,u64 at,u32 value,bool success,u64 elapsed){
    setup(channel,true,at,value,UINT64_MAX);
    assert(submit_and_wait(active,16,0x454e0002)==success);
    assert(now_us==elapsed && active->submit_failed==!success && publications==1);
    assert(faults==(success?0u:1u) && usermode[0x90/4]==active->work_submit_token);
    if(!success){
        u64 before=now_us;unsigned published=publications;
        signal_at=0;payload=0x454e0002;
        assert(!submit_and_wait(active,16,0x454e0002));
        assert(now_us==before && publications==published && faults==1);
    }
}
int main(void){
    const u64 completion[]={50,250000,250050,1000000,3999950,4000000};
    for(unsigned channel=CH_NVENC;channel<=CH_NVDEC;channel++){
        for(unsigned i=0;i<sizeof completion/sizeof completion[0];i++)
            check(channel,completion[i],0x454e0002,true,completion[i]);
        check(channel,4000050,0x454e0002,false,4000000);
        check(channel,UINT64_MAX,0,false,4000000);
        check(channel,50,0x454e0001,false,4000000); /* previous sequence cookie */
        check(channel,3999950,0x454f0002,false,4000000); /* wrong tag */
        /* Even an unexpected progress observation must not shorten or extend
         * this one absolute host-codec deadline. */
        setup(channel,true,2000000,0x454e0002,100000);
        assert(submit_and_wait(active,16,0x454e0002) && now_us==2000000);
        setup(channel,true,UINT64_MAX,0,3999000);
        assert(!submit_and_wait(active,16,0x454e0002) && now_us==4000000 && faults==1);
        const u64 error_times[]={50,1000000,4000000};
        for(unsigned i=0;i<sizeof error_times/sizeof error_times[0];i++){
            /* Both absent and simultaneous matching cookies must quarantine. */
            for(unsigned simultaneous=0;simultaneous<2;simultaneous++){
                setup(channel,true,simultaneous?error_times[i]:UINT64_MAX,0x454e0002,UINT64_MAX);
                error_at=error_times[i];
                assert(!submit_and_wait(active,16,0x454e0002));
                assert(now_us==error_times[i] && active->submit_failed && faults==1 && publications==1);
                assert(!submit_and_wait(active,16,0x454e0002) && publications==1);
            }
        }
        setup(channel,true,50,0x454e0002,UINT64_MAX);
        notification.status=0xffff;notification.info32=65;
        assert(!submit_and_wait(active,16,0x454e0002));
        assert(!publications && now_us==0 && active->submit_failed && faults==1);
        setup(channel,true,50,0x454e0002,UINT64_MAX);
        notification.status=1; /* Not the official terminal error status. */
        assert(submit_and_wait(active,16,0x454e0002));
        setup(channel,true,UINT64_MAX,0,UINT64_MAX);
        nv_completion_wait_t w={active,0x454e0002};
        assert(!nv_completion_probe(&w));
        notification.status=0xffff;notification.info32=65;
        assert(nv_completion_probe(&w)); /* Wake; never clear submit_failed here. */
    }
    for(unsigned channel=0;channel<3;channel++){
        check(channel,1,0x454e0002,true,2);
        check(channel,3,0x454e0002,true,4);
        check(channel,49,0x454e0002,true,50);
        check(channel,50,0x454e0002,true,50);
        check(channel,250000,0x454e0002,true,250000); /* final boundary read */
        check(channel,250050,0x454e0002,false,250000);
        check(channel,UINT64_MAX,0,false,250000);
    }
    /* Legacy path retains its one-second fetched-ring extension and stops
     * candidate sweeps once consumption was observed. */
    setup(CH_NVENC,false,500000,0x454e0002,100000);
    assert(submit_and_wait(active,16,0x454e0002) && now_us==500000 && doorbells==1);
    setup(CH_NVENC,false,UINT64_MAX,0,100000);
    assert(!submit_and_wait(active,16,0x454e0002) && now_us==1100000 && doorbells==1 && faults==0);
    setup(0,false,UINT64_MAX,0,100000);
    assert(submit_and_wait(active,16,0) && now_us==100000 && doorbells==1);
    puts("PASS actual submit_and_wait: codec-only 4s host deadline, delayed/boundary completion, stale cookies, finite timeout/quarantine, noncodec 250ms and legacy GP_GET grace unchanged (modeled time/GPU edge)");
}
'''
    if compile_only:
        clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
        with tempfile.TemporaryDirectory(prefix='kestrel-codec-wait-') as tmp:
            source = Path(tmp) / 'wait.c'
            source.write_text(c)
            subprocess.run([clang, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-c', str(source), '-o', str(Path(tmp) / 'wait.obj')], check=True)
        print('COMPILE ONLY: production wait/probe scaffold compiled; assertions NOT executed; no GPU workload')
    else:
        run_test(c, 'nv_codec_wait')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-only', action='store_true')
    main(parser.parse_args().compile_only)
