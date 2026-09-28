#!/usr/bin/env python3
"""Differential host tests for BE gain decoding and TX-power data preparation."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import test_rtw89_radio_tables as tables

ROOT = Path(__file__).resolve().parents[1]


def gain_reference(payload, rfe, cv):
    result = bytearray(1592)
    loaded = ignored = 0
    for address, data in tables.linux_rows(payload, rfe, cv):
        t, pb, band, config = address.to_bytes(4, 'little')
        path, bw = pb & 15, pb >> 4
        if config == 2 or (config == 4 and rfe < 50):
            ignored += 1
            continue
        index = (band*2+bw)*2+path
        if config in (0, 3):
            if t < 2:
                start, n = (0, 4) if t == 0 else (4, 3)
                off = (432 if config else 0) + index*7+start
                bit = t + (3 if config else 0)
            else:
                start, n = (4 if t == 3 else 0), (4 if config else 2)
                off = (768 + index*8 if config else 336+index*2) + start
                bit = t+3 if config else 2
            result[1512+index] |= 1 << bit
        elif config == 1:
            sub, half = t >> 4, t & 15
            off = (1152, 1176, 1224, 1320)[sub] + (band*2+path)*(1 << sub)
            off += half*4 if sub == 3 else 0
            n = min(4, 1 << sub)
            result[1560+band*2+path] |= 1 << (sub+(half if sub == 3 else 0))
        else:
            raise AssertionError((address, data))
        result[off:off+n] = data.to_bytes(4, 'little')[:n]
        loaded += 1
    struct.pack_into('<II', result, 1584, loaded, ignored)
    return result


def power_reference(eid, b):
    # Canonical sequence: id, band,bw,nss,ofdma,rs,shift,len,ntx,bf,reg,p6,ch,ru,
    # four signed rate bytes, and signed limit/unsigned shaping value.
    v = [0]*19
    v[0] = eid
    if eid == 9:
        v[1], v[3], v[5], v[6], v[7] = b[:5]
        v[2], v[4] = b[9:11]
        for i in range(v[7]):
            v[14+i] = b[5+i]
    elif eid <= 12:
        v[1] = eid-10
        v[2], v[8], v[5], v[9], v[10] = b[:5]
        if eid == 12:
            v[11], v[12] = b[5:7]
        else:
            v[12] = b[5]
        v[18] = struct.unpack('b', b[-1:])[0] & 0xffffffff
    elif eid <= 15:
        v[1] = eid-13
        v[13], v[8], v[10] = b[:3]
        if eid == 15:
            v[11], v[12] = b[3:5]
        else:
            v[12] = b[3]
        v[18] = struct.unpack('b', b[-1:])[0] & 0xffffffff
    else:
        v[1] = b[0]
        if eid == 16:
            v[5], v[10], v[18] = b[1:4]
        else:
            v[10], v[18] = b[1:3]
    return v


def hash_words(values):
    h = 2166136261
    for v in values:
        h = ((h ^ v)*16777619) & 0xffffffff
    return h


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
typedef uint8_t u8; typedef uint32_t u32;
#include "rtw89_radio_gain.h"
#include "rtw89_radio_power.h"
static unsigned count; static u32 hash;
static bool power(void *ctx,const rtw89_radio_power_entry_t *e) {
    (void)ctx;
    u32 v[]={e->id,e->band,e->bw,e->nss,e->ofdma,e->rate_section,e->shift,e->length,
        e->ntx,e->beamforming,e->regulation,e->power_6ghz,e->channel_index,e->ru,
        (u8)e->rates[0],(u8)e->rates[1],(u8)e->rates[2],(u8)e->rates[3],(u32)e->value};
    for(unsigned i=0;i<19;i++) hash=(hash^v[i])*16777619u;
    count++; return true;
}
static void run(const char *path,const u8 *gain_expected,const u32 expected[9][2]) {
    FILE *fp=fopen(path,"rb"); assert(fp); fseek(fp,0,SEEK_END);
    size_t size=ftell(fp); rewind(fp);u8 *b=malloc(size);assert(b);
    assert(fread(b,1,size,fp)==size);fclose(fp);
    rtw89_radio_resources_t r; rtw89_radio_gain_t gain;
    assert(sizeof gain==1592);
    for(unsigned rfe=1;rfe<=2;rfe++) for(unsigned cv=0;cv<3;cv++) {
        assert(rtw89_radio_resources_load(b,size,cv,rfe,&r));
        assert(rtw89_radio_gain_decode(&r.gain,rfe,cv,&gain));
        assert(!memcmp(&gain,gain_expected,sizeof gain));
        assert(rtw89_radio_gain_complete(&gain,0,0,0));
        for(unsigned i=0;i<9;i++) {
            count=0;hash=2166136261u;
            assert(rtw89_radio_power_walk(&r.power[i],power,NULL));
            assert(count==expected[i][0] && hash==expected[i][1]);
        }
    }
    /* Unsupported band/path/type cannot scribble into adjacent calibration. */
    u8 synthetic[16]={0,0,0,0,1,2,3,4,0,0,12,0,1,2,3,4};
    rtw89_radio_element_t e={.id=3,.data=synthetic,.size=sizeof synthetic};
    memset(&gain,0xff,sizeof gain);
    assert(!rtw89_radio_gain_decode(&e,1,0,&gain) && gain.loaded_rows==0);
    for(unsigned i=0;i<sizeof gain;i++) assert(((u8 *)&gain)[i]==0);
    rtw89_radio_power_entry_t entry;
    u8 record[12]={0,0,0,0,4,0xff,0xfe,1,2,0,0,0};
    assert(rtw89_radio_power_entry(9,record,sizeof record,&entry));
    assert(entry.rates[0]==-1 && entry.rates[1]==-2);
    record[11]=1;
    assert(!rtw89_radio_power_entry(9,record,sizeof record,&entry));
    record[11]=0;record[4]=5;
    assert(!rtw89_radio_power_entry(9,record,sizeof record,&entry));
    for(unsigned id=10;id<=17;id++) {
        memset(record,0,sizeof record);record[0]=255;
        assert(!rtw89_radio_power_entry(id,record,sizeof record,&entry));
        assert(!rtw89_radio_power_entry(id,record,1,&entry));
    }
    for(unsigned i=0;i<9;i++) {
        rtw89_radio_element_t *table=&r.power[i];
        unsigned width=table->header[27];
        u32 n=rtw89_radio_le32(table->header+28);
        u8 *last=(u8 *)table->data+(n-1)*width;
        u8 saved=last[0];last[0]=255;count=0;
        assert(!rtw89_radio_power_walk(table,power,NULL) && !count);
        last[0]=saved;
    }
    free(b);
}
int main(void) {
'''
    entries = 0
    for index, name in enumerate(('rtw8922a_fw.bin', 'rtw8922a_fw-1.bin')):
        path = ROOT/'firmware/rtw89'/name
        b = path.read_bytes()
        elms = tables.elements(b)
        gain = gain_reference(next(p for _, eid, _, p in elms if eid == 3), 1, 0)
        assert gain == gain_reference(next(p for _, eid, _, p in elms if eid == 3), 2, 1)
        code += 'static const u8 gain%d[]={%s};\n' % (index, ','.join(map(str, gain)))
        expected = []
        for off, eid, _, payload in elms:
            if not 9 <= eid <= 17:
                continue
            width, n = b[off+27], tables.u32(b, off+28)
            values = []
            for i in range(n):
                values.extend(power_reference(eid, payload[i*width:(i+1)*width]))
            expected.append((n, hash_words(values)))
            entries += n
        code += 'static const u32 expected%d[9][2]={%s};\n' % (index, ','.join('{%du,%du}' % e for e in expected))
        code += 'run("%s",gain%d,expected%d);\n' % (path.as_posix(), index, index)
    code += 'puts("PASS: BE gain data and all nine TX-power record types match firmware; malformed bounds rejected");return 0;}\n'
    with tempfile.TemporaryDirectory(prefix='kestrel-rtw89-gain-') as temp:
        src, exe = Path(temp)/'test.c', Path(temp)/'test.exe'
        src.write_text(code)
        cc = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
        subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Werror', '-I', str(ROOT/'kernel'), str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print(f'{entries} power records across two firmware packages; six RFE/cut variants each; no MMIO')


if __name__ == '__main__':
    main()
