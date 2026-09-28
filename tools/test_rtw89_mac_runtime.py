#!/usr/bin/env python3
"""Run actual bounded MAC helpers against traced fake MMIO, not hardware.

Reference: Linux v6.17 mac_be.c sta_sch_init_be/mpdu_proc_init_be/
preload_init_be. This is not proof that the incomplete runtime MAC path works.
"""
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import test_rtw89_firmware as fw


def main():
    functions = '\n'.join(fw.function(n) for n in (
        'rtw89_sched_init', 'rtw89_mpdu_init', 'rtw89_preload_init'))
    names = sorted(set(re.findall(r'\b(?:R_BE_|B_BE_|TRXCFG_|PRELD_)[A-Z0-9_]+', functions)))
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
'''
    code += '\n'.join(fw.macro(n) for n in names) + r'''
static u8 regs[0x10000];
static unsigned writes, width[16], at[16], delay_us, reads, ready_after;
static bool absent;
static u32 load(u32 offset) {u32 v; memcpy(&v, regs + offset, 4); return v;}
static void store(u32 offset,u32 v) {memcpy(regs + offset,&v,4);}
static void trace(unsigned offset,unsigned bytes) {
    assert(writes < 16); at[writes]=offset; width[writes++]=bytes;
}
static u32 rd32(volatile u8 *r,u32 offset) {
    assert(r == regs);
    if(offset == R_BE_SS_CTRL) {
        reads++;
        if(absent) return 0xffffffffu;
        if(ready_after && reads>=ready_after) store(offset,load(offset)|B_BE_SS_INIT_DONE);
    }
    return load(offset);
}
static void wr32(volatile u8 *r,u32 offset,u32 value) {
    assert(r == regs); trace(offset,4); store(offset,value);
}
static void set32(volatile u8 *r,u32 offset,u32 mask) {wr32(r,offset,rd32(r,offset)|mask);}
static void clr32(volatile u8 *r,u32 offset,u32 mask) {wr32(r,offset,rd32(r,offset)&~mask);}
static void set8(volatile u8 *r,u32 offset,u8 mask) {
    assert(r == regs); trace(offset,1); regs[offset]|=mask;
}
static void timer_udelay(unsigned us) {delay_us+=us;}
static void reset(void) {
    memset(regs,0,sizeof regs); writes=delay_us=reads=ready_after=0; absent=false;
}
'''
    code += functions + r'''
int main(void) {
    assert(!rtw89_sched_init(NULL)); assert(!rtw89_mpdu_init(NULL));
    assert(!rtw89_preload_init(NULL)); assert(!writes);
    reset(); store(R_BE_SS_CTRL,B_BE_BAND_TRIG_EN|B_BE_BAND1_TRIG_EN|0x40);
    ready_after=3;
    assert(rtw89_sched_init(regs));
    assert(writes==3 && width[0]==1 && width[1]==4 && width[2]==4);
    assert(delay_us==2);
    assert(load(R_BE_SS_CTRL)==(B_BE_SS_INIT_DONE|B_BE_WARM_INIT|B_BE_SS_EN|0x40));
    reset(); assert(!rtw89_sched_init(regs));
    assert(writes==1 && delay_us==TRXCFG_WAIT_CNT);
    reset(); absent=true; assert(!rtw89_sched_init(regs));
    assert(writes==1 && delay_us==0);

    reset(); store(R_BE_HDR_SHCUT_SETTING,0x8010);
    store(R_BE_MPDU_PROC,0x40000000); store(R_BE_DISP_FWD_WLAN_0,0x876543ff);
    assert(rtw89_mpdu_init(regs));
    assert(writes==5);
    const unsigned expected[]={R_BE_MPDU_PROC,R_BE_CUT_AMSDU_CTRL,
        R_BE_HDR_SHCUT_SETTING,R_BE_RX_HDRTRNS,R_BE_DISP_FWD_WLAN_0};
    for(unsigned i=0;i<5;i++) assert(at[i]==expected[i] && width[i]==4);
    assert(load(R_BE_MPDU_PROC)==0x40000001);
    assert(load(R_BE_CUT_AMSDU_CTRL)==0x010e05f0);
    assert(load(R_BE_HDR_SHCUT_SETTING)==0x8017); /* Linux write32_set preserves MLD. */
    assert(load(R_BE_RX_HDRTRNS)==0);
    assert(load(R_BE_DISP_FWD_WLAN_0)==0x87654355);

    reset(); store(R_BE_TXPKTCTL_B0_PRELD_CFG0,0x441255aa);
    store(R_BE_TXPKTCTL_B0_PRELD_CFG1,0x1234affe);
    assert(rtw89_preload_init(regs));
    assert(writes==2 && at[0]==R_BE_TXPKTCTL_B0_PRELD_CFG0 && at[1]==R_BE_TXPKTCTL_B0_PRELD_CFG1);
    assert(width[0]==4 && width[1]==4);
    assert(load(R_BE_TXPKTCTL_B0_PRELD_CFG0)==0xc60855aa);
    assert(load(R_BE_TXPKTCTL_B0_PRELD_CFG1)==0x1234a134);
    puts("rtw89 bounded MAC runtime helpers: PASS (host MMIO model only)");
}
'''
    compiler = shutil.which('clang') or shutil.which('gcc') or r'C:\Program Files\LLVM\bin\clang.exe'
    if not Path(compiler).is_file():
        raise SystemExit('Host clang or gcc required')
    with tempfile.TemporaryDirectory(prefix='rtw89-mac-runtime-') as temp:
        src = Path(temp) / 'test.c'
        exe = Path(temp) / 'test.exe'
        src.write_text(code)
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
