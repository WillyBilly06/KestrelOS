#!/usr/bin/env python3
"""Actual runtime DLE/HFC initialization against traced, fault-injected MMIO."""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT/'kernel/rtw89.c').read_text()
hdr = (ROOT/'kernel/rtw89.h').read_text().replace('\\\n', ' ')
model = (ROOT/'kernel/rtw89_model.c').read_text()
body = '\n'.join(function(src,n) for n in (
    'rtw89_memory_split_adds_up','rtw89_dle_init','rtw89_flow_control_init'))
# Check transcribed quotas against the pinned primary source, not just the
# matching numbers in our device model. WDE adds the explicit unused Q2.
reference = (ROOT/'out/rtw89-linux-reference/mac-review.c').read_text()
def numbers(pattern, text):
    found = re.search(pattern, text)
    assert found, pattern
    return [int(n) for n in re.findall(r'\d+', found[1])]
wde = numbers(r'\.wde_qt0_v1\s*=\s*\{([^}]+)', reference)
assert numbers(r'wde_quota\[5\]\s*=\s*\{([^}]+)', body) == wde[:2]+[0]+wde[2:]
for local, upstream in (('ple_min','ple_qt0'),('ple_max','ple_qt1')):
    assert numbers(local+r'\[13\]\s*=\s*\{([^}]+)',body) == numbers(
        r'\.'+upstream+r'\s*=\s*\{([^}]+)',reference)
definitions = dict(re.findall(r'^#define\s+(\w+)\s+([^\n]+)',hdr,re.M))
needed = set(re.findall(r'\b[A-Z][A-Z0-9_]+\b',body)) & definitions.keys()
while True:
    more = set(re.findall(r'\b[A-Z][A-Z0-9_]+\b',' '.join(definitions[n] for n in needed))) & definitions.keys()
    if more <= needed: break
    needed |= more
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
'''
code += '\n'.join('#define '+n+' '+definitions[n] for n in sorted(needed))+'\n'
code += r'''
static u32 mem[0xc000/4], offsets[128], values[128], writes, elapsed, window=sizeof mem;
static u32 absent_at, stalled_at, drop_at;static bool drop_enable;
static bool reachable(u32 at){return window>=4 && at<=window-4;}
static u32 rd(u32 at){assert(reachable(at));return mem[at/4];}
static void wr(u32 at,u32 v){assert(reachable(at));mem[at/4]=v;}
'''
code += function(model,'packet_memory_sync')+'\n'
code += r'''
static u32 rd32(volatile u8 *r,u32 at){
    assert(r==(volatile u8*)mem && reachable(at));packet_memory_sync();
    if(absent_at==at)return 0xffffffffu;
    if(stalled_at==at)return 0;return mem[at/4];
}
static void wr32(volatile u8 *r,u32 at,u32 v){
    assert(r==(volatile u8*)mem && reachable(at));assert(writes<128);
    offsets[writes]=at;values[writes++]=v;
    if((at>=0x8c40&&at<=0x8c50) || (at>=0x9040&&at<=0x9070))
        assert(!(mem[0x8400/4]&((1u<<26)|(1u<<23))));
    if(at>=0xb718&&at<0xb748)assert(!(mem[0xb700/4]&9));
    if(at==drop_at)return;
    if(drop_enable&&at==0xb700)v&=~9u;
    mem[at/4]=v;
}
static void set32(volatile u8*r,u32 a,u32 v){wr32(r,a,rd32(r,a)|v);}
static void clr32(volatile u8*r,u32 a,u32 v){wr32(r,a,rd32(r,a)&~v);}
static void timer_udelay(unsigned us){elapsed+=us;}
static void reset(void){
    memset(mem,0,sizeof mem);writes=elapsed=0;window=sizeof mem;
    absent_at=stalled_at=drop_at=0;drop_enable=false;
    mem[0x8400/4]=0x84800011;mem[0x8404/4]=0x10;
    mem[0x8c08/4]=0xa0008000;mem[0x9008/4]=0xa0008000;
}
'''
code += body+r'''
int main(void){
    volatile u8 *r=(volatile u8*)mem;
    reset();assert(rtw89_dle_init(r));
    assert(writes==23 && offsets[0]==0x8400 && offsets[1]==0x8404);
    assert(offsets[2]==0x8c08&&offsets[3]==0x9008&&offsets[22]==0x8400);
    assert(mem[0x8c08/4]==0xad008000 && mem[0x9008/4]==0xaa809a01);
    assert(mem[0x8d00/4]==3&&mem[0x9100/4]==3);
    assert(!elapsed && (mem[0x8400/4]&0x80000011)==0x80000011);
    const u16 wq[]={3302,6,0,0,20};
    const u16 pmin[]={320,320,32,16,13,13,292,292,64,18,1,4,0};
    const u16 pmax[]={320,320,32,16,1316,1316,1595,1595,1367,1321,1,1307,0};
    for(unsigned i=0;i<5;i++)assert(mem[(0x8c40+4*i)/4]==((u32)wq[i]|(u32)wq[i]<<16));
    for(unsigned i=0;i<13;i++)assert(mem[(0x9040+4*i)/4]==((u32)pmin[i]|(u32)pmax[i]<<16));
    for(unsigned engine=0;engine<2;engine++){
        reset();stalled_at=engine?0x9100:0x8d00;
        assert(!rtw89_dle_init(r)&&elapsed==2000);
        assert(!(mem[0x8400/4]&0x04800000));
        reset();absent_at=engine?0x9100:0x8d00;
        assert(!rtw89_dle_init(r)&&!elapsed);
        assert(!(mem[0x8400/4]&0x04800000));
    }
    reset();drop_at=0x9040;assert(!rtw89_dle_init(r)&&elapsed==2000);
    reset();absent_at=0x8400;assert(!rtw89_dle_init(r)&&!writes);
    reset();window=0x9000;assert(!rtw89_dle_init(r)&&!writes);

    reset();mem[0xb700/4]=0xa503ffff;
    assert(rtw89_flow_control_init(r)&&elapsed==10);
    assert(writes==20&&offsets[0]==0xb700 && !(values[0]&9));
    for(unsigned ch=0;ch<12;ch++){
        unsigned g=(ch>=4&&ch<8)||ch>=10;
        assert(mem[(0xb718+4*ch)/4]==(2u|1641u<<16|g<<31));
    }
    assert(mem[0xb790/4]==(1651u|1651u<<16));
    assert(mem[0xb704/4]==0x00200002 && mem[0xb794/4]==3302);
    assert(mem[0xb7a4/4]==0 && mem[0xb7a8/4]==0);
    assert(mem[0xb700/4]==0xa503f009);
    reset();drop_at=0xb718;assert(!rtw89_flow_control_init(r));assert(!(mem[0xb700/4]&9));
    reset();drop_enable=true;assert(!rtw89_flow_control_init(r));assert(!(mem[0xb700/4]&9));
    reset();absent_at=0xb750;assert(!rtw89_flow_control_init(r));assert(!(mem[0xb700/4]&9));
    reset();window=0xb700;assert(!rtw89_flow_control_init(r)&&!writes);
    puts("PASS runtime packet memory: complete SCC quotas, both ready gates, traced HFC channels/groups/enables and failure cleanup");
}
'''
run_test(code,'rtw89-packet-memory')
