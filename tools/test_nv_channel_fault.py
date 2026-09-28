#!/usr/bin/env python3
"""Execute error capture with synthetic RM replies; no hardware claims."""
from pathlib import Path
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#include "nv_error_notifier.h"
#define CH_COUNT 5
#define PAGE_SIZE 4096
#define PUSHBUF_BYTES 4096
static char logs[64000];static unsigned log_at,queries,mode;
static void capture(const char *sub,const char *fmt,...) {
    (void)sub;va_list ap;va_start(ap,fmt);
    int n=vsnprintf(logs+log_at,sizeof logs-log_at,fmt,ap);va_end(ap);
    assert(n>=0 && log_at+(unsigned)n+1<sizeof logs);log_at+=(unsigned)n;logs[log_at++]='\n';logs[log_at]=0;
}
#define kerr capture
typedef struct {u8 *pool;u64 pool_gpu,root_gpu;u32 used,pages;} nv_vmm_t;
typedef struct {
    void *card,*rm;nv_vmm_t vmm;u32 h_channel,obj_class,chid,runlist_id;
    u32 work_submit_token,gp_put;volatile u32 *sem;volatile u8 *pushbuf;const char *name;
    volatile nv_error_notification_t *error_notifier;
} nv_channel_t;
static nv_channel_t channels[CH_COUNT];static u32 nv_last_control_status;
static u64 table_words[6*512];static u32 push[1024],sem;
static u8 *table=(u8 *)table_words;
static bool nv_rm_control(void *card,void *rm,u32 handle,u32 cmd,void *data,u32 size,
                          void *out,u32 osize,u32 *got) {
    (void)card;(void)rm;assert(handle>=70 && handle<75 && data==out && size==osize);
    unsigned q=queries++%3;assert(cmd==(q==0?0xb06f010fu:q==1?0x906f0105u:0x906f0106u));
    assert(size==(q==0?4:q==1?1:104));
    nv_last_control_status=mode==1?0x56:0;*got=mode==2?0:size;
    memset(data,0xff,size); /* Poison/unterminated data must not be decoded on failure. */
    if(mode==0 && q==0)*(u32 *)data=0x38;
    if(mode==0 && q==2) {
        ((u32 *)data)[0]=0x300;((u32 *)data)[1]=0x20000000;((u32 *)data)[2]=2;
    }
    return mode!=1;
}
'''
    c = c.replace('#include "nv_error_notifier.h"',
                  (ROOT / 'kernel/nv_error_notifier.h').read_text())
    c += (ROOT / 'kernel/nv_channel_fault.h').read_text()
    c += r'''
int main(void) {
    for(unsigned i=0;i<CH_COUNT;i++)channels[i]=(nv_channel_t){
        .vmm={table,0x100000,0x100000,1,1},.h_channel=70+i,
        .obj_class=0xcab5,.sem=&sem,.pushbuf=(u8 *)push,.name="test"};
    mode=0;nv_channel_capture_fault(&channels[0],0,80,123);
    assert(queries==3 && strstr(logs,"engine-fault=1 pbdma-fault=1 acquire-fail=1"));
    assert(strstr(logs,"RM-error-notifier unavailable"));
    assert(strstr(logs,"saved-MMU address=0x30020000000 type=0x2"));
    assert(strstr(logs,"debugger-deferred-RC="));
    unsigned before=log_at;nv_channel_capture_fault(&channels[0],0,80,123);
    assert(queries==3 && log_at==before); /* No repeat of consuming query. */
    for(mode=1;mode<=2;mode++) {
        logs[0]=0;log_at=0;nv_channel_capture_fault(&channels[mode],0,80,123);
        assert(strstr(logs,"valid=0") && !strstr(logs,"saved-MMU address="));
        assert(!strstr(logs,"HW-state=") && !strstr(logs,"debugger-deferred-RC="));
    }
    nv_error_notification_t notifier={.time_hi=0x11223344,.time_lo=0x55667788,.info32=32,.info16=0x1c,.status=0xffff};
    assert(((u32 *)&notifier)[0]==0x11223344 && ((u32 *)&notifier)[1]==0x55667788);
    channels[3].error_notifier=&notifier;
    mode=0;logs[0]=0;log_at=0;nv_channel_capture_fault(&channels[3],4000,200,123);
    assert(strstr(logs,"status=0xffff exception=32 engine=0x1c"));
    assert(strstr(logs,"timestamp=0x1122334455667788"));
    assert(strstr(logs,"matching-reads=1"));
    assert(!strstr(logs,"push+")); /* Out-of-bounds packet rejected. */
    logs[0]=0;log_at=0;nv_channel_capture_fault(&channels[4],0,400,123);
    assert(strstr(logs,"push truncated: 64 of 100"));
    assert(queries==15);
    /* Walk all six Blackwell levels, including the dual-PDE small-page half. */
    u64 va=(1ull<<56)|(17ull<<47)|(33ull<<38)|(55ull<<29)|(77ull<<21)|(99ull<<12);
    const unsigned indexes[6]={1,17,33,55,77*2+1,99};
    for(unsigned i=0;i<5;i++)table_words[i*512+indexes[i]]=(0x101000ull+i*4096)|0x12;
    table_words[5*512+99]=0xdead000ull|0x681;
    channels[0].vmm.used=channels[0].vmm.pages=6;
    logs[0]=0;log_at=0;nv_fault_shadow_path(&channels[0],va);
    assert(strstr(logs,"level=4 table=0x104000 index=155"));
    assert(strstr(logs,"level=5 table=0x105000 index=99 entry=0xdead681"));
    table_words[4*512+155]=0x106012; /* Outside used shadow: never dereference. */
    logs[0]=0;log_at=0;nv_fault_shadow_path(&channels[0],va);
    assert(strstr(logs,"outside local table shadow"));
    assert(!strstr(logs,"level=5 table=0x105000"));
    puts("PASS actual channel fault capture: official-sized queries, raw errors, missing/truncated replies, string bounds, once-per-channel capture and bounded packets");
}
'''
    run_test(c, 'channel_fault')


if __name__ == '__main__':
    main()
