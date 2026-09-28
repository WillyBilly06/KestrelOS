#!/usr/bin/env python3
"""Execute production BE warm-reset ordering against stale/absent MMIO."""
import re
from test_gpu_stable_candidate import ROOT, run_test

src = (ROOT/'kernel/rtw89.c').read_text()
hdr = (ROOT/'kernel/rtw89.h').read_text()
def function(source, name):
    match=re.search(r'^(?:bool|u8|const char \*)\s*'+name+r'\([^;]*?\)\s*\{',source,re.M)
    assert match,name
    return source[match.start():source.index('\n}',match.end())+2]
names = set(re.findall(r'\b(?:R_BE|B_BE|RTW89_FWDL)_[A-Z0-9_]+',
    '\n'.join(function(src, n) for n in ('rtw89_fwdl_status',
        'rtw89_fwdl_status_name','rtw89_fwdl_path_ready','rtw89_fwdl_start_cpu'))))
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
#define kerr(...) ((void)0)
#define kwarn(...) ((void)0)
static bool expect_dirty_debug;
'''
for name in sorted(names):
    match = re.search(r'^#define '+name+r'\s+[^\n]+', hdr, re.M)
    assert match, name
    code += match[0]+'\n'
code += r'''
static u32 registers[0x10000/4];
static unsigned resets, releases, writes, waits;
static bool absent, answer=true, held;
static u32 rd32(volatile u8 *r,u32 o){(void)r;return absent?0xffffffffu:registers[o/4];}
static void wr32(volatile u8 *r,u32 o,u32 v){
    (void)r;writes++;
    u32 old=registers[o/4];
    registers[o/4]=v;
    if(o==R_BE_PLATFORM_ENABLE&&(v&B_BE_HOLD_AFTER_RESET)&&(v&B_BE_WCPU_EN))held=true;
    if(o==R_BE_WCPU_FW_CTRL&&v==B_BE_RUN_ENV_MASK){assert(held);resets++;}
    if(o==R_BE_PLATFORM_ENABLE&&!(old&B_BE_WCPU_EN)&&(v&B_BE_WCPU_EN)&&!(v&B_BE_HOLD_AFTER_RESET)){
        assert(held&&resets==1);
        assert(!(registers[R_BE_WCPU_FW_CTRL/4]&(B_BE_H2C_PATH_RDY|B_BE_DLFW_PATH_RDY)));
        releases++;
    }
}
static void set32(volatile u8*r,u32 o,u32 v){wr32(r,o,rd32(r,o)|v);}
static void clr32(volatile u8*r,u32 o,u32 v){wr32(r,o,rd32(r,o)&~v);}
static u16 rd16(volatile u8*r,u32 o){return (u16)(rd32(r,o&~3u)>>((o&2u)*8u));}
static void wr16(volatile u8*r,u32 o,u16 v){u32 shift=(o&2u)*8u;wr32(r,o&~3u,(rd32(r,o&~3u)&~(0xffffu<<shift))|((u32)v<<shift));}
static void timer_udelay(unsigned n){
    assert(n==1);waits++;
    if(answer&&releases)registers[R_BE_WCPU_FW_CTRL/4]|=B_BE_H2C_PATH_RDY|(2u<<B_BE_WCPU_FWDL_STATUS_SHIFT);
}
static void timer_mdelay(unsigned n){assert(n==1);waits++;}
'''
for name in ('rtw89_fwdl_status','rtw89_fwdl_status_name','rtw89_fwdl_path_ready','rtw89_fwdl_start_cpu','rtw89_fw_wait_running'):
    if name.endswith('_name'):
        start=src.index('const char *rtw89_fwdl_status_name(')
        code += src[start:src.index('\n}',start)+2]+'\n'
    else:
        code += function(src,name)+'\n'
code += r'''
static void reset(void){
    memset(registers,0,sizeof registers);resets=releases=writes=waits=0;held=absent=false;answer=true;
    registers[R_BE_WCPU_FW_CTRL/4]=B_BE_RUN_ENV_MASK|B_BE_H2C_PATH_RDY|B_BE_DLFW_PATH_RDY|(3u<<B_BE_WCPU_FWDL_STATUS_SHIFT);
    registers[R_BE_PLATFORM_ENABLE/4]=B_BE_WCPU_EN;
}
int main(void){
    volatile u8 *r=(volatile u8*)registers;
    for(unsigned bb=0;bb<2;bb++){
        reset();assert(rtw89_fwdl_start_cpu(r,5,bb,"8922"));
        assert(resets==1&&releases==1&&waits==1);
        assert((registers[R_BE_WCPU_FW_CTRL/4]&B_BE_RUN_ENV_MASK)==B_BE_RUN_ENV_MASK);
        assert(!!(registers[R_BE_WCPU_FW_CTRL/4]&B_BE_BBMCU0_FWDL_EN)==bb);
        assert((rd16(r,R_BE_BOOT_REASON)&B_BE_BOOT_REASON_MASK)==5);
        assert(registers[R_BE_DCPU_PLATFORM_ENABLE/4]&B_BE_DCPU_PLATFORM_EN);
    }
    reset();answer=false;assert(!rtw89_fwdl_start_cpu(r,0,true,"8922")&&waits==1000000);
    reset();absent=true;assert(!rtw89_fwdl_start_cpu(r,0,true,"8922")&&!writes);
    assert(!rtw89_fwdl_path_ready(r,true,"8922")&&!waits);
    assert(!rtw89_fwdl_path_ready(NULL,true,"8922"));
    reset();registers[R_BE_WCPU_FW_CTRL/4]=0;
    assert(!rtw89_fw_wait_running(r,7,"8922")&&waits==7); /* downloaded, not running */
    registers[R_BE_WCPU_FW_CTRL/4]=3u<<B_BE_WCPU_FWDL_STATUS_SHIFT;
    assert(rtw89_fw_wait_running(r,7,"8922"));
    registers[R_BE_WCPU_FW_CTRL/4]|=B_BE_BBMCU0_FWDL_EN;
    assert(!rtw89_fw_wait_running(r,7,"8922"));
    for(unsigned raw=4;raw<=7;raw++){
        registers[R_BE_WCPU_FW_CTRL/4]=raw<<B_BE_WCPU_FWDL_STATUS_SHIFT;
        unsigned before=waits;assert(!rtw89_fw_wait_running(r,7,"8922")&&waits==before);
    }
    absent=true;assert(!rtw89_fw_wait_running(r,7,"8922"));
    puts("PASS BE CPU restart: stale warm state cleared while held, RUN_ENV preserved, fresh handshake, absent MMIO and timeout rejected");
}
'''
run_test(code,'rtw89_cpu_restart')
