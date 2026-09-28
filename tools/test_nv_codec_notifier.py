#!/usr/bin/env python3
"""Execute real codec notifier setup against allocation/mapping failure models."""
from pathlib import Path
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'kernel/nv_chan.c').read_text()
    assert source.index('chp.h_object_error = nv_codec_notifier_setup(ch, idx)') < source.index(
        'if (!nv_rm_alloc(c, rm, RM_DEVICE, h_channel(idx), BLACKWELL_CHANNEL_GPFIFO_B,')
    common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#include "nv_error_notifier.h"
enum {CH_COPY,CH_GFX,CH_COMPUTE,CH_NVDEC,CH_NVENC,CH_GOLDEN,CH_COUNT};
#define RM_DEVICE 1
#define RM_SUBDEVICE 2
#define NVOS32_ALLOC_FLAGS_ALIGN_FORCE 0x100u
#define kwarn(...) ((void)0)
#define kinfo(...) ((void)0)
typedef struct {bool host_api;u32 client;} rm_t;
typedef struct {void *card;rm_t *rm;const char *name;volatile nv_error_notification_t *error_notifier;} nv_channel_t;
typedef struct {u32 owner,type,attr,flags;u64 size,alignment;} mem_alloc_params_t;
static unsigned allocs,maps,frees,mode;static u32 nv_last_alloc_status;
static u64 memory[512];
static bool nv_rm_alloc(void *c,rm_t *r,u32 parent,u32 h,u32 cls,void *p,u32 size) {
    (void)c;allocs++;mem_alloc_params_t *m=p;
    assert(r->client==99 && parent==RM_DEVICE && cls==0x3e);
    assert(h==0x430003 || h==0x430004);assert(size==sizeof *m);
    assert(m->owner==99 && m->type==13 && m->attr==((1u<<23)|(1u<<25)|(2u<<27)));
    assert(m->size==4096 && m->alignment==4096 && m->flags==0x100);
    return mode!=1;
}
static u32 nvrm_host_map_memory(u32 client,u32 dev,u32 h,u64 off,u64 size,void **p,u32 flags) {
    maps++;assert(client==99 && dev==RM_SUBDEVICE && (h==0x430003 || h==0x430004));
    assert(!off && size==4096 && !flags);*p=(mode==2 || mode==3)?NULL:memory;
    return (mode==2 || mode==4)?59:0;
}
static bool nv_rm_free(void *c,rm_t *r,u32 p,u32 h) {
    (void)c;(void)r;assert(p==RM_DEVICE && (h==0x430003 || h==0x430004));frees++;return true;
}
'''
    common = common.replace('#include "nv_error_notifier.h"',
                            (ROOT / 'kernel/nv_error_notifier.h').read_text())
    common += (ROOT / 'kernel/nv_codec_notifier_setup.h').read_text()
    for mode in range(5):
        test = common + f'\n#define TEST_MODE {mode}\n' + r'''
int main(void) {
    (void)nv_last_alloc_status;
    mode=TEST_MODE;rm_t rm={false,99};nv_channel_t ch={.rm=&rm,.name="codec"};
    assert(nv_codec_notifier_setup(&ch,CH_NVENC)==0 && allocs==0);
    rm.host_api=true;
    for(int i=0;i<CH_COUNT;i++)if(i!=CH_NVENC && i!=CH_NVDEC)
        assert(nv_codec_notifier_setup(&ch,i)==0);
    assert(!allocs);memset(memory,0xa5,sizeof memory);
    u32 h=nv_codec_notifier_setup(&ch,CH_NVENC);
    assert(allocs==1 && maps==(mode==1?0u:1u));
    if(!mode) {
        assert(h==0x430004 && ch.error_notifier==(void *)memory);
        for(unsigned i=0;i<512;i++)assert(memory[i]==0);
        ch.error_notifier->info32=32;ch.error_notifier->info16=0x1c;
        ch.error_notifier->status=0xffff;
        assert(nv_codec_notifier_setup(&ch,CH_NVENC)==h && allocs==1 && maps==1);
        nv_error_notification_t n={0};
        assert(nv_error_notification_snapshot(ch.error_notifier,&n));
        assert(n.status==0xffff && n.info32==32 && n.info16==0x1c);
        assert(!nv_error_notification_snapshot(NULL,&n));
        assert(!nv_error_notification_snapshot(ch.error_notifier,NULL));
        rm.client=100;ch.error_notifier=NULL;
        assert(nv_codec_notifier_setup(&ch,CH_NVENC)==0 && !ch.error_notifier);
        assert(!frees);
    } else {
        assert(h==0 && !ch.error_notifier);
        assert(frees==((mode==2 || mode==3)?1u:0u));
        assert(!nv_codec_notifier_setup(&ch,CH_NVENC) && allocs==1);
        assert(memory[0]==0xa5a5a5a5a5a5a5a5ull);
    }
    puts("PASS codec notifier allocation/map/lifetime case");
}
'''
        run_test(test, f'codec_notifier_{mode}')


if __name__ == '__main__':
    main()
