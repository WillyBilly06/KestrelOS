#!/usr/bin/env python3
"""Compile production RF I/O; assert physical offsets, bounds and absence handling."""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/rtw89.c').read_text()
hdr = (ROOT / 'kernel/rtw89.h').read_text().replace('\\\n', ' ')
macros = '\n'.join(m[0] for m in re.finditer(
    r'^#define (?:RTW89_RF_|RTW89_PHY_CR_BASE|R_HWSI_|B_HWSI_)[^\n]*(?:\\\n[^\n]*)*', hdr, re.M))
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32;
#define kwarn(...) ((void)0)
static u32 mem[0x40000/4], window=sizeof mem, writes, delays, last_write;
static bool absent, busy, done=true;
static bool reachable(u32 at){return window>=4 && at<=window-4;}
static u32 rd32(volatile u8 *regs,u32 at){
    assert(regs==(volatile u8*)mem && reachable(at));
    if(absent)return 0xffffffffu;
    if(at==0x22c24||at==0x22d24)
        return (busy?1u<<29:0) | (done?1u<<31:0) | 0xabcde;
    return mem[at/4];
}
static void wr32(volatile u8 *regs,u32 at,u32 value){
    assert(regs==(volatile u8*)mem && reachable(at));
    assert(at>=0x20000); mem[at/4]=value;last_write=at;++writes;
}
static void timer_udelay(u32 us){delays+=us;}
'''
code += macros + '\n'
for name in ('poll_bit','wr32_masked','rf_read_direct','rf_write_direct',
             'rtw89_rf_read','rtw89_rf_write'):
    code += function(src,name) + '\n'
code += r'''
int main(void){
    volatile u8 *regs=(volatile u8*)mem;
    assert(R_HWSI_ADD(0)==0x22adc && R_HWSI_ADD(1)==0x22bdc);
    assert(R_HWSI_DATA(0)==0x22ae0 && R_HWSI_DATA(1)==0x22be0);
    assert(R_HWSI_VAL(0)==0x22c24 && R_HWSI_VAL(1)==0x22d24);
    for(int path=0;path<2;++path){
        assert(rtw89_rf_read(regs,path,0x18)==0xabcde);
        assert((mem[R_HWSI_ADD(path)/4]&B_HWSI_ADD_MASK)==0x180);
        assert(!(mem[R_HWSI_ADD(path)/4]&B_HWSI_ADD_POLL_MASK));
        assert(rtw89_rf_write(regs,path,0x18,0x54321));
        assert(last_write==(path?0x22be0u:0x22ae0u));
        assert(mem[last_write/4]==0x05432118);
        u32 direct=(path?0x2f000u:0x2e000u)+0x18*4;
        mem[direct/4]=0x123abcde;
        assert(rtw89_rf_read(regs,path,0x10018)==0xabcde);
        assert(rtw89_rf_write(regs,path,0x10018,0x55555));
        assert(mem[direct/4]==0x12355555);
    }
    window=0x10000;u32 before=writes;
    assert(rtw89_rf_read(regs,0,0x18)==RTW89_RF_INVALID);
    assert(!rtw89_rf_write(regs,1,0x18,1));
    assert(!rtw89_rf_write(regs,0,0x10018,1));assert(writes==before);
    window=sizeof mem;absent=true;
    assert(rtw89_rf_read(regs,0,0x18)==RTW89_RF_INVALID);
    assert(rtw89_rf_read(regs,0,0x10018)==RTW89_RF_INVALID);
    assert(!rtw89_rf_write(regs,0,0x18,1));
    assert(!rtw89_rf_write(regs,0,0x10018,1));
    absent=false;done=false;delays=0;
    assert(rtw89_rf_read(regs,0,0x18)==RTW89_RF_INVALID);
    assert(delays==3802);
    assert(!(mem[R_HWSI_ADD(0)/4]&B_HWSI_ADD_POLL_MASK));
    busy=true;delays=0;before=writes;
    assert(!rtw89_rf_write(regs,0,0x18,1));
    assert(delays==3800 && writes==before);
    assert(rtw89_rf_read(regs,2,0x18)==RTW89_RF_INVALID);
    assert(!rtw89_rf_write(0,0,0x18,1));
    puts("PASS RF serial/direct: exact BE PHY offsets, separate paths, bounds, absent device, timeout and cleanup");
}
'''
run_test(code,'rtw89-rf-io')
