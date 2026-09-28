#!/usr/bin/env python3
"""Exercise production board-resource admission before any firmware DMA."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT/'kernel/rtw.c').read_text()
bringup = function(src, 'bring_up')
assert bringup.index('be_prepare_radio(') < bringup.index('rtw89_fwdl_preinit(')
assert 'be_setup_mailbox(' not in src
assert 'rtw89_rfk_calibrate(' not in bringup
assert 'rtw89_data_attach(' not in bringup
assert 'c->fw = image;' in bringup and 'memset(&image, 0, sizeof image);' in bringup
assert 'dev->radio_up = true' not in bringup
code = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32;
typedef struct {u8 *data;size_t size;} firmware_t;
#include "rtw89_radio_gain.h"
#include "rtw89_radio_power.h"
'''
code += function(src,'be_prepare_radio') + r'''
static void check(const char *path){
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    long bytes=ftell(f);assert(bytes>0);rewind(f);
    firmware_t fw={.data=malloc((size_t)bytes),.size=(size_t)bytes};assert(fw.data);
    assert(fread(fw.data,1,fw.size,f)==fw.size);fclose(f);
    rtw89_radio_resources_t r;rtw89_radio_gain_t gain;
    for(u8 cv=0;cv<2;++cv)for(u8 rfe=1;rfe<3;++rfe){
        assert(be_prepare_radio(&fw,cv,rfe,&r,&gain));
        assert(gain.loaded_rows && r.bb.data && r.nctl.data && r.bbmcu);
        for(unsigned p=0;p<9;++p){
            u8 *header=fw.data+(r.power[p].header-fw.data);
            u8 keep=header[27];header[27]=0;
            rtw89_radio_resources_t bad;
            assert(!be_prepare_radio(&fw,cv,rfe,&bad,&gain));
            header[27]=keep;
        }
        assert(be_prepare_radio(&fw,cv,rfe,&r,&gain));
        u8 *last=fw.data+(r.gain.data-fw.data)+r.gain.size-8;
        u8 keep=last[3];last[3]=0xe0;
        assert(!be_prepare_radio(&fw,cv,rfe,&r,&gain));last[3]=keep;
    }
    assert(!be_prepare_radio(&fw,0,255,&r,&gain));
    size_t saved=fw.size;
    for(fw.size=0;fw.size<32;fw.size++)assert(!be_prepare_radio(&fw,0,1,&r,&gain));
    fw.size=saved;
    assert(be_prepare_radio(&fw,1,1,&r,&gain));free(fw.data);
}
int main(void){
'''
for name in ('rtw8922a_fw.bin','rtw8922a_fw-1.bin'):
    code += 'check("'+(ROOT/'firmware/rtw89'/name).as_posix()+'");\n'
code += 'puts("PASS production radio resource admission: both packages, cuts/RFEs, corrupt gain/power refusal and retained ownership guards");}\n'
run_test(code,'rtw89-radio-admission',include_dirs=(ROOT/'kernel',))
