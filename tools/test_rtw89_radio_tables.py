#!/usr/bin/env python3
"""Execute production element/table parser against packaged RTL8922 firmware.

No radio I/O. Independent Python translation of Linux v6.17 table selection
provides expected row counts/hashes for the production C interpreter.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def u32(b, p):
    return struct.unpack_from('<I', b, p)[0]


def elements(b):
    end = max(u32(b, 20+i*16) + u32(b, 24+i*16) for i in range(b[1]))
    off = (end+15) & ~15
    result = []
    while off < len(b):
        size = u32(b, off+4)
        result.append((off, u32(b, off), b[off+24], b[off+32:off+32+size]))
        off = (off+32+size+15) & ~15
    return result


def linux_rows(payload, rfe, cv):
    regs = list(struct.iter_unpack('<II', payload))
    n = next((i for i, (a, _) in enumerate(regs) if a >> 28 != 15), len(regs))
    chosen = 0
    if n:
        hs = [a & 0xfffffff for a, _ in regs[:n]]
        for target in (rfe << 16 | cv, rfe << 16 | 255):
            if target in hs:
                chosen = hs.index(target)
                break
        else:
            for rr in (rfe, 255):
                candidates = [(a & 255, i) for i, a in enumerate(hs) if (a >> 16 & 255) == rr]
                if candidates:
                    chosen = max(candidates)[1]
                    break
            else:
                return None
    cfg = regs[chosen][0] & 0xfffffff
    matched, found, target = True, False, 0
    out = []
    for addr, data in regs[n:]:
        cond = addr >> 28
        if cond in (8, 9):
            target = addr & 0xfffffff
        elif cond == 10:
            if not found:
                return None
            matched = False
        elif cond == 11:
            matched, found = True, False
        elif cond == 4:
            if found:
                matched = False
            else:
                matched = found = target == cfg
        elif matched:
            out.append((addr, data))
    return out


def digest(rows):
    value = 2166136261
    for a, d in rows:
        for word in (a, d):
            value = ((value ^ word) * 16777619) & 0xffffffff
    return value


def main():
    code = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "rtw89_radio_tables.h"
static unsigned count;
static u32 hash;
static u32 expected_rows[4096][2];
static unsigned applied, pages, page_words, expected_path;
static u8 work[6000];
static bool row(void *ctx, u32 a, u32 d) {
    (void)ctx; assert(count<4096); expected_rows[count][0]=a; expected_rows[count][1]=d;
    count++; hash = (hash ^ a) * 16777619u;
    hash = (hash ^ d) * 16777619u; return true;
}
static bool bbwrite(void *ctx, u32 a, u32 d) {
    (void)ctx; assert(applied<count);
    assert(a==expected_rows[applied][0] && d==expected_rows[applied][1]);
    applied++; return true;
}
static void delay(void *ctx, u32 us) {(void)ctx; (void)us; assert(0);}
static bool rfwrite(void *ctx, u8 path, u32 a, u32 d) {
    assert(path==expected_path); return bbwrite(ctx,a,d);
}
static bool rfpage(void *ctx, u8 path, u8 page, const u8 *bytes, size_t len) {
    (void)ctx; assert(path==expected_path && page==pages++);
    assert(len && len<=2000 && !(len%4));
    unsigned offset=page_words;
    for(unsigned i=0,n=0;i<count;i++) if(expected_rows[i][0]>=0x100) {
        if(n>=offset && n<offset+len/4) {
            assert(rtw89_radio_le32(bytes+(n-offset)*4)==
                   (expected_rows[i][0]<<20 | expected_rows[i][1]));
        }
        n++;
    }
    page_words+=(unsigned)len/4; return true;
}
static void put(u8 *p, u32 v) {for(unsigned i=0;i<4;i++) p[i]=(u8)(v>>(8*i));}
static void run(const char *path, const unsigned expected[][5], size_t tests) {
    FILE *fp=fopen(path,"rb"); assert(fp); fseek(fp,0,SEEK_END);
    size_t size=ftell(fp); rewind(fp); u8 *b=malloc(size); assert(b);
    assert(fread(b,1,size,fp)==size); fclose(fp);
    rtw89_radio_resources_t resources;
    for(unsigned rfe=1;rfe<=2;rfe++) for(unsigned cv=0;cv<3;cv++) {
        assert(rtw89_radio_resources_load(b,size,cv,rfe,&resources));
        assert(resources.rf[0].id==5 && resources.rf[1].id==4);
        assert((resources.recognized & 0x7ff35u)==0x7ff35u);
        assert(resources.power[0].header[26]==0);
    }
    assert(!rtw89_radio_resources_load(b,size,1,0xff,&resources));
    assert(!resources.bbmcu && !resources.bb.data);
    for(size_t i=0;i<tests;i++) {
        const u8 *p=NULL; size_t len=0;
        unsigned id=expected[i][0], rfe=expected[i][1], cv=expected[i][2];
        assert(rtw89_fw_element_select(b,size,cv,id,&p,&len));
        count=0; hash=2166136261u;
        bool ok=rtw89_radio_table_apply(p,len,rfe,cv,row,NULL);
        assert(ok == (expected[i][3] != 0xffffffffu));
        if(ok) {assert(count==expected[i][3]); assert(hash==expected[i][4]);}
        else assert(count==0);
        if(ok && id!=3) {
            rtw89_radio_element_t element={.id=id,.data=p,.size=len};
            rtw89_radio_io_t io={.bb_write=bbwrite,.delay_us=delay,
                .rf_write=rfwrite,.rf_page=rfpage,.bb_window=0x10000};
            applied=pages=page_words=0;
            if(id==4 || id==5) {
                expected_path=id-4;
                assert(rtw89_radio_apply_rf(&element,rfe,cv,&io,work,sizeof work));
                unsigned words=0;
                for(unsigned j=0;j<count;j++) if(expected_rows[j][0]>=0x100) words++;
                assert(page_words==words && pages==(words+499)/500);
            } else {
                assert(rtw89_radio_apply_bb(&element,rfe,cv,&io));
            }
            assert(applied==count);
        }
    }
    /* BB MCU nearest-cut selection is independent of element order. */
    for(unsigned cv=0;cv<4;cv++) {
        const u8 *p=NULL; size_t len=0;
        assert(rtw89_fw_element_select(b,size,cv,0,&p,&len));
        assert(p[-8] == (cv ? 1 : 0));
        assert(len>=32 && (p[15] & 0xff)==1); /* standalone v1 image */
        assert(len==33560 && (rtw89_radio_le32(p+20)>>16)==80);
        assert((rtw89_radio_le32(p+24)>>8 & 255)==2);
        assert(rtw89_radio_le32(p+48)==0x18638000u);
        assert(rtw89_radio_le32(p+52)==0x0a008000u); /* BB payload: type10, 32768 */
        assert(rtw89_radio_le32(p+64)==0x20302a00u);
        assert(rtw89_radio_le32(p+68)==0x090002c8u); /* security: type9, 712 */
        assert(rtw89_radio_le32(p+72)==0); /* zero MSSC; no selectable signature pool */
    }
    size_t cursor; assert(rtw89_radio_elements_begin(b,size,&cursor));
    rtw89_radio_element_t e;
    while(rtw89_radio_element_next(b,size,&cursor,&e)>0) {
        size_t off=(size_t)(e.header-b);
        for(size_t cut=off+1;cut<off+32;cut++) {
            size_t at=off;
            assert(rtw89_radio_element_next(b,cut,&at,&e)==-1);
        }
        size_t at=off; u32 old=u32_dummy;
    }
    free(b);
}
int main(void) {
'''.replace('        size_t at=off; u32 old=u32_dummy;', r'''
        size_t at=off; u32 old=rtw89_radio_le32(b+off+4);
        put(b+off+4,0xffffffffu);
        assert(rtw89_radio_element_next(b,size,&at,&e)==-1);
        const u8 *p=(const u8 *)1; size_t n=123;
        assert(!rtw89_fw_element_select(b,size,1,0,&p,&n));
        assert(!p && !n);
        put(b+off+4,old);
        if(e.id==4) {
            u8 idx=b[off+24]; b[off+24]=2;
            assert(!rtw89_radio_resources_load(b,size,1,1,&resources));
            assert(!resources.bbmcu);
            b[off+24]=idx;
        }
        if(e.id==9) {
            u32 entries=rtw89_radio_le32(b+off+28);
            put(b+off+28,0xffffffffu);
            assert(!rtw89_radio_resources_load(b,size,1,1,&resources));
            put(b+off+28,entries);
        }
        if(e.id==18) {
            u32 bitmap=rtw89_radio_le32(b+off+24);
            put(b+off+24,0xfffe);
            assert(!rtw89_radio_resources_load(b,size,1,1,&resources));
            put(b+off+24,bitmap);
        }
''')
    total = 0
    for index, filename in enumerate(('rtw8922a_fw.bin', 'rtw8922a_fw-1.bin')):
        path = ROOT / 'firmware/rtw89' / filename
        expected = []
        for off, eid, priv, payload in elements(path.read_bytes()):
            if eid not in (2, 3, 4, 5, 8):
                continue
            for rfe in (1, 2, 0, 255):
                for cv in (0, 1, 2):
                    rows = linux_rows(payload, rfe, cv)
                    expected.append((eid, rfe, cv, len(rows) if rows is not None else 0xffffffff,
                                     digest(rows) if rows is not None else 0))
        code += 'static const unsigned e%d[][5]={\n' % index
        code += ',\n'.join('{%s}' % ','.join(str(v)+'u' for v in e) for e in expected)
        code += '};\nrun("%s",e%d,sizeof e%d/sizeof e%d[0]);\n' % (path.as_posix(), index, index, index)
        total += len(expected)
    code += r'''
    u8 t[48]={0};
    put(t,0xf0010000); put(t+8,0x80010000); put(t+16,0x40000000);
    put(t+24,0x100); put(t+28,0x1234); put(t+32,0xb0000000);
    count=0; hash=2166136261u;
    assert(rtw89_radio_table_apply(t,40,1,0,row,NULL) && count==1);
    /* Late corruption must not cause even the earlier valid MMIO write. */
    put(t+40,0x80010000); count=0;
    assert(!rtw89_radio_table_apply(t,48,1,0,row,NULL) && !count);
    for(unsigned len=1;len<48;len++) if(len%8) {
        assert(!rtw89_radio_table_apply(t,len,1,0,row,NULL) && !count);
    }
    /* Tiny/overflowing MFW tables are refused without touching output. */
    u8 b[64]={0xff,1}; size_t cursor;
    for(unsigned len=0;len<32;len++) assert(!rtw89_radio_elements_begin(b,len,&cursor));
    put(b+20,0xfffffff0); put(b+24,0xffffffff);
    assert(!rtw89_radio_elements_begin(b,sizeof b,&cursor));
    puts("PASS: actual firmware radio resources, differential table interpreter, malformed bounds, no partial writes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='kestrel-rtw89-radio-') as temp:
        src, exe = Path(temp)/'test.c', Path(temp)/'test.exe'
        src.write_text(code)
        cc = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
        subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Werror', '-I', str(ROOT/'kernel'), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print(f'{total} packaged table/RFE/cut comparisons; no hardware or USB writes')


if __name__ == '__main__':
    main()
