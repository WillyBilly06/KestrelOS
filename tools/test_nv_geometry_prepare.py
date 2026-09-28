#!/usr/bin/env python3
"""Compile-only scaffold for the production geometry preparation packet.

Extracts production control flow and CE encoding. Mock retirement is not GPU
execution. This command never runs the executable or claims assertions passed.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / 'kernel/nv_chan.c').read_text()


def definition(name):
    # The two bounded descriptor layouts are taken from production verbatim.
    return re.search(r'typedef struct \{[^{}]*\}\s*' + name + r';', SRC)[0]


def source_code():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64;
typedef struct { bool open,submit_failed; u32 pb_at,sem[4]; } nv_channel_t;
typedef struct { u64 bytes,va,owner; unsigned state; bool copy_mapping_attempted; } nv_surface_t;
enum { CH_COPY,CH_GFX,NS_READY=7 };
static nv_channel_t channels[2]={{.open=true},{.open=true}};
static unsigned g_render_transaction=1;
static unsigned char stage[4*1024*1024];
static void *g_upload_stage=stage;
static bool ready=true,reserve_ok=true,retire=true;
static u32 methods[256],words[256],word_count,method,remaining;
static unsigned reserves,submits,flushes;
static u32 reserved_bytes;
static bool nv_surface_transfer_ce_ready(void){return ready;}
static void cache_flush(const void *p,u32 n){assert(p==stage && n<=sizeof stage);flushes++;}
static bool pb_reserve(nv_channel_t *ch,u32 n){assert(ch==channels);reserves++;reserved_bytes=n;return reserve_ok;}
static u32 next_completion_signal(nv_channel_t *ch,u32 tag){(void)ch;return tag|123u;}
static u32 nv_completion_awaken(nv_channel_t *ch,u32 v){(void)ch;return v;}
static void pb_method(nv_channel_t *ch,u32 sub,u32 m,u32 count){
    assert(ch==channels && sub==0 && remaining==0);method=m;remaining=count;ch->pb_at+=4;
}
static void pb_data(nv_channel_t *ch,u32 value){
    assert(remaining && word_count<256);methods[word_count]=method;words[word_count++]=value;
    remaining--;method+=4;ch->pb_at+=4;
}
static bool submit_and_wait(nv_channel_t *ch,u32 start,u32 cookie){
    assert(!remaining && ch->sem[0]==0 && ch->pb_at-start<=reserved_bytes);
    submits++;ch->submit_failed=!retire;if(retire)ch->sem[0]=cookie;return retire;
}
'''
    for name in ('UPLOAD_STAGE_BYTES', 'VA_UPLOAD_STAGE', 'VA_SEM', 'SUBCH_COPY',
                 'BLACKWELL_DMA_COPY_B', 'CE_OFFSET_IN_UPPER', 'CE_OFFSET_OUT_UPPER',
                 'CE_PITCH_IN', 'CE_LINE_LENGTH_IN', 'CE_SET_SEMAPHORE_A',
                 'CE_SET_REMAP_CONST_B', 'CE_REMAP_FILL32', 'CE_REMAP_ENABLE',
                 'CE_LAUNCH_DMA', 'CE_COMPLETION_AWAKEN'):
        source += re.search(r'^#define ' + name + r'\s+[^\n]+', SRC, re.M)[0] + '\n'
    source += re.search(r'^#define CE_LAUNCH\s+[^\n]+\\\n[^\n]+', SRC, re.M)[0] + '\n'
    source += definition('nv_ce_prepare_t') + '\n'
    source += function(SRC, 'ce_prepare_batch') + '\n'
    source += definition('nv_surface_prepare_t') + '\n'
    source += function(SRC, 'nv_surface_prepare_batch') + '\n'
    source += r'''
static void reset(void){
    ready=reserve_ok=retire=true;channels[0].submit_failed=channels[1].submit_failed=false;
    channels[0].open=true;channels[0].pb_at=0;g_render_transaction=1;
    word_count=remaining=reserves=submits=flushes=0;
}
static void packet_check(u32 n,u32 fill_mask){
    unsigned launches=0,releases=0;
    for(u32 i=0;i<word_count;i++){
        if(methods[i]==CE_SET_SEMAPHORE_A)releases++;
        if(methods[i]!=CE_LAUNCH_DMA)continue;
        bool last=launches+1==n;
        u32 expected=(CE_LAUNCH&~(3u<<3));
        if(fill_mask&(1u<<launches))expected|=CE_REMAP_ENABLE;
        if(last)expected|=(1u<<3)|CE_COMPLETION_AWAKEN;
        assert(words[i]==expected);launches++;
    }
    assert(launches==n && releases==1 && reserves==1 && submits==1 && flushes==1);
}
int main(void){
    unsigned char payload[304];memset(payload,0x57,sizeof payload);
    nv_surface_t workspace={.bytes=20000000,.va=0x30000000000ull,.owner=19,.state=NS_READY,.copy_mapping_attempted=true};
    nv_surface_t scratch=workspace,status=workspace;scratch.va+=0x2000000;status.va+=0x4000000;
    nv_surface_prepare_t setup[3]={
        {.dst=&workspace,.offset=0,.upload=payload,.bytes=sizeof payload},
        {.dst=&workspace,.offset=12580352,.bytes=136*1365,.record_bytes=136,.records=1365},
        {.dst=&workspace,.offset=19230976,.bytes=16*1365,.record_bytes=16,.records=1365}};
    reset();assert(nv_surface_prepare_batch(setup,3));packet_check(3,6);
    assert(!memcmp(stage,payload,sizeof payload));
    nv_surface_prepare_t raster[3]={
        {.dst=&status,.bytes=131072,.record_bytes=8,.records=16384},
        {.dst=&scratch,.offset=1024,.upload=payload,.bytes=sizeof payload},
        {.dst=&scratch,.offset=1024+sizeof payload,.src=&workspace,.source_offset=12800000,.bytes=36864}};
    reset();assert(nv_surface_prepare_batch(raster,3));packet_check(3,1);
    // Check whole-packet validation: invalid last role cannot clear first role.
    reset();workspace.owner=20;assert(!nv_surface_prepare_batch(raster,3));
    assert(!reserves && !submits && !flushes);workspace.owner=19;
    reset();raster[2].source_offset=workspace.bytes-4;assert(!nv_surface_prepare_batch(raster,3));
    assert(!reserves && !submits && !flushes);raster[2].source_offset=12800000;
    reset();raster[2].src=&scratch;assert(!nv_surface_prepare_batch(raster,3));
    assert(!reserves && !submits && !flushes);raster[2].src=&workspace;
    reset();setup[2].bytes--;assert(!nv_surface_prepare_batch(setup,3));assert(!submits && !flushes);setup[2].bytes++;
    reset();setup[2].records--;assert(!nv_surface_prepare_batch(setup,3));assert(!submits && !flushes);setup[2].records++;
    reset();ready=false;assert(!nv_surface_prepare_batch(setup,3));assert(!submits && !flushes);
    reset();g_render_transaction=0;assert(!nv_surface_prepare_batch(setup,3));assert(!submits && !flushes);
    reset();channels[0].submit_failed=true;stage[0]=0x33;
    assert(!nv_surface_prepare_batch(setup,3) && !submits && !flushes && stage[0]==0x33);
    reset();channels[1].submit_failed=true;assert(!nv_surface_prepare_batch(setup,3));assert(!submits && !flushes);
    reset();workspace.copy_mapping_attempted=false;assert(!nv_surface_prepare_batch(setup,3));assert(!submits && !flushes);
    workspace.copy_mapping_attempted=true;
    reset();reserve_ok=false;assert(!nv_surface_prepare_batch(setup,3));assert(reserves==1 && !submits && !word_count);
    reset();retire=false;assert(!nv_surface_prepare_batch(setup,3));assert(submits==1 && channels[0].submit_failed);
    stage[0]=0x33;assert(!nv_surface_prepare_batch(setup,3));assert(submits==1 && stage[0]==0x33);
    reset();assert(!nv_surface_prepare_batch(setup,0));assert(!nv_surface_prepare_batch(setup,4));assert(!submits && !flushes);
    nv_ce_prepare_t overflow={.src=0x1000,.dst=UINT64_MAX-3,.src_pitch=8,.dst_pitch=8,.line=8,.lines=1};
    reset();assert(!ce_prepare_batch(channels,&overflow,1));assert(!reserves && !submits);
    return 0;
}
'''
    return source


def main():
    run_test(source_code(), 'nv_geometry_prepare', compile_only=True)


if __name__ == '__main__':
    main()
