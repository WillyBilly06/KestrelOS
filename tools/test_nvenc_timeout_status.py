#!/usr/bin/env python3
"""Compile diagnostic-only timeout readback assertions; optional host execution.

No firmware, GPU commands or VM. Default remains compile-only under the user's
codec-testing restriction. Building a scaffold does not execute its assertions.
"""
import argparse
from test_gpu_stable_candidate import ROOT, run_test


def main(run=False):
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define NV_NVENC_8_2 1
#include "../refs/open-gpu-doc/classes/video/nvenc_drv.h"
_Static_assert(sizeof(nvenc_pic_stat_s)==128,"status wire size");
_Static_assert(__builtin_offsetof(nvenc_pic_stat_s,total_bit_count)==8,"status bit-count offset");
typedef struct {bool host_api;u32 client;} rm_t;
typedef struct {rm_t *rm;bool submit_failed;} nv_channel_t;
#define NVENC_VRAM_BYTES 0xc0000u
#define H_NVENC_VRAM 0x4c0000u
static unsigned reads,lines;
static bool fail;
static u32 nvrm_transfer_rm_memory(u32 client,u32 object,u64 offset,void *dst,u64 n,bool read){
    assert(client==7&&object==H_NVENC_VRAM&&offset==0x3000&&n==128&&read);
    reads++;if(fail)return 0x26;
    memset(dst,0xa5,(size_t)n);return 0;
}
static void log_message(const char *sub,const char *fmt,...){
    assert(!strcmp(sub,"nvenc-status"));char line[256];va_list args;
    va_start(args,fmt);int n=vsnprintf(line,sizeof line,fmt,args);va_end(args);
    assert(n>0&&n<168);lines++;
}
#define kerr(...) log_message(__VA_ARGS__)
/* Distinct instances cover all single-shot paths without mutating production
 * capture state or resetting it to allow a replay. */
#define nvenc_capture_unretired_status capture_poison
#include "nvenc_timeout_status.h"
#undef nvenc_capture_unretired_status
#undef KERNEL_NVENC_TIMEOUT_STATUS_H
#define nvenc_capture_unretired_status capture_failure
#include "nvenc_timeout_status.h"
#undef nvenc_capture_unretired_status
#undef KERNEL_NVENC_TIMEOUT_STATUS_H
#define nvenc_capture_unretired_status capture_invalid
#include "nvenc_timeout_status.h"
#undef nvenc_capture_unretired_status
int main(void){
    rm_t rm={.host_api=true,.client=7};nv_channel_t ch={.rm=&rm,.submit_failed=true};
    capture_poison(NULL,0x3000);assert(!reads&&!lines);
    ch.submit_failed=false;capture_poison(&ch,0x3000);assert(!reads&&!lines);
    ch.submit_failed=true;rm.host_api=false;capture_poison(&ch,0x3000);assert(!reads&&!lines);
    rm.host_api=true;capture_poison(&ch,0x3000);assert(reads==2&&lines==11&&ch.submit_failed);
    capture_poison(&ch,0x3000);assert(reads==2&&lines==11);
    fail=true;capture_failure(&ch,0x3000);assert(reads==4&&lines==12&&ch.submit_failed);
    capture_failure(&ch,0x3000);assert(reads==4&&lines==12);
    capture_invalid(&ch,NVENC_VRAM_BYTES-64);assert(reads==4&&lines==13);
    capture_invalid(&ch,0x3000);assert(reads==4&&lines==13);
    puts("PASS diagnostic-only status reads: host/quarantine gate, exact bounded read range, once-only attempts, failure retention, bounded raw logs");
}
'''
    run_test(code, 'nvenc_timeout_status', [ROOT / 'kernel'], compile_only=not run)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--run', action='store_true', help='explicitly execute host-only assertions')
    main(p.parse_args().run)
