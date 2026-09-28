#!/usr/bin/env python3
"""Actual checked IDR job layout against independent sizing/alignment rules."""
from pathlib import Path
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    mesa = (ROOT / 'out/mesa-ref/src/nouveau/vulkan/rust/video/decode/h264.rs').read_text()
    for expression in ('sps.max_num_ref_frames + 1',
                       'align_u32(pic_height_in_map_units, 2) * pic_width_in_mbs * 64 - 63',
                       'align_u32(pic_width_in_mbs * 104, 0x100)',
                       'align_u32(pic_width_in_mbs * 0x300, 0x200)'):
        assert expression in mesa
    c = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
'''
    c += '#include "' + (ROOT / 'kernel/nvdec_h264_job.h').as_posix() + '"\n'
    c += r'''
static uint64_t up(uint64_t value,uint64_t unit){return ((value+unit-1)/unit)*unit;}
static kh264_decode_desc description(unsigned w,unsigned h,unsigned refs,unsigned bytes){
    kh264_decode_desc d={0};d.profile_idc=66;d.coded_width=d.display_width=w;
    d.coded_height=d.display_height=h;d.width_mbs=w/16;d.height_mbs=h/16;
    d.max_num_ref_frames=refs;d.slice_start=0;d.slice_end=bytes;return d;
}
static void check_plan(const kh264_decode_desc *d,unsigned bytes){
    nvdec_h264_job_layout p;memset(&p,0xa5,sizeof p);
    assert(nvdec_h264_job_plan(&p,d,bytes)==KH264_OK);
    uint64_t mbs_per_col=(d->height_mbs+1)/2*2;
    uint64_t coloc_per_pic=up(mbs_per_col*d->width_mbs*64-63,256);
    uint64_t size[9]={bytes+16ull,sizeof(nvdec_h264_pic_s),8,
        coloc_per_pic*(d->max_num_ref_frames+1),up(d->width_mbs*768,512),
        up(d->width_mbs*104,256),sizeof(nvdec_frame_status_t),
        up(d->coded_width,64)*up(d->coded_height,16),
        up(d->coded_width,64)*up(d->coded_height/2,16)};
    unsigned offset[9]={p.stream_offset,p.picture_offset,p.slice_offset,p.coloc_offset,
        p.history_offset,p.mbhist_offset,p.status_offset,p.luma_offset,p.chroma_offset};
    unsigned actual[9]={p.stream_bytes,p.picture_bytes,p.slice_bytes,p.coloc_bytes,
        p.history_bytes,p.mbhist_bytes,p.status_bytes,p.luma_bytes,p.chroma_bytes};
    uint64_t cursor=4096;
    for(unsigned i=0;i<9;i++){
        cursor=up(cursor,i>=7?1024:256);
        assert(offset[i]==cursor && actual[i]==size[i] && actual[i]);
        assert(!(offset[i]&255) && (uint64_t)offset[i]+actual[i]<=p.total_bytes);
        for(unsigned j=0;j<i;j++)assert((uint64_t)offset[j]+actual[j]<=offset[i]);
        cursor+=size[i];
    }
    assert(p.total_bytes==up(cursor,65536) && !(p.total_bytes&65535));
    assert(p.pitch==up(d->coded_width,64) && p.luma_height==up(d->coded_height,16));
    assert(p.chroma_height==up(d->coded_height/2,16));
    /* Last padded block is allocated, not only the logical NV12 sample area. */
    assert(!(p.luma_offset&1023) && !(p.chroma_offset&1023));
    assert(!(p.luma_bytes&1023) && !(p.chroma_bytes&1023));
    assert(p.luma_bytes>=(uint64_t)p.pitch*d->coded_height);
    assert(p.chroma_bytes>=(uint64_t)p.pitch*(d->coded_height/2));
}
static void rejected(const kh264_decode_desc *d,unsigned bytes,int expected){
    nvdec_h264_job_layout p,zero={0};memset(&p,0xa5,sizeof p);
    assert(nvdec_h264_job_plan(&p,d,bytes)==expected);
    assert(!memcmp(&p,&zero,sizeof p));
}
int main(void){
    unsigned tested=0;
    for(unsigned mw=3;mw<=256;mw++)for(unsigned mh=4;mh<=256;mh++)for(unsigned refs=0;refs<=16;refs++){
        unsigned n=(tested&1)?KH264_MAX_BYTES:1+tested%65536;
        kh264_decode_desc d=description(mw*16,mh*16,refs,n);check_plan(&d,n);tested++;
    }
    const unsigned edge_bytes[]={1,239,240,241,255,256,257,65535,65536,KH264_MAX_BYTES-1,KH264_MAX_BYTES};
    for(unsigned i=0;i<sizeof edge_bytes/sizeof edge_bytes[0];i++){
        kh264_decode_desc d=description(48,80,0,edge_bytes[i]);check_plan(&d,edge_bytes[i]);
        d=description(4096,4096,16,edge_bytes[i]);check_plan(&d,edge_bytes[i]);
    }
    kh264_decode_desc good=description(256,256,1,100),d;
    assert(nvdec_h264_job_plan(NULL,&good,100)==KH264_INVALID);
    rejected(NULL,100,KH264_INVALID);rejected(&good,0,KH264_INVALID);
    rejected(&good,KH264_MAX_BYTES+1u,KH264_INVALID);rejected(&good,UINT32_MAX,KH264_INVALID);
    for(unsigned i=0;i<19;i++){
        d=good;int expected=KH264_INVALID;
        switch(i){
        case 0:d.coded_width=0;break;case 1:d.coded_height=0;break;
        case 2:d.width_mbs=0;break;case 3:d.height_mbs=0;break;
        case 4:d.coded_width=32;d.width_mbs=2;expected=KH264_UNSUPPORTED;break;
        case 5:d.coded_height=48;d.height_mbs=3;expected=KH264_UNSUPPORTED;break;
        case 6:d.coded_width=4112;expected=KH264_UNSUPPORTED;break;
        case 7:d.coded_height=4112;expected=KH264_UNSUPPORTED;break;
        case 8:d.coded_width=UINT32_MAX;expected=KH264_UNSUPPORTED;break;
        case 9:d.coded_height=UINT32_MAX;expected=KH264_UNSUPPORTED;break;
        case 10:d.width_mbs=UINT32_MAX;break;case 11:d.height_mbs=UINT32_MAX;break;
        case 12:d.max_num_ref_frames=17;break;case 13:d.max_num_ref_frames=UINT32_MAX;break;
        case 14:d.coded_width=255;break;case 15:d.coded_height=255;break;
        case 16:d.slice_start=d.slice_end;break;case 17:d.slice_end=101;break;
        case 18:d.slice_start=UINT32_MAX;break;
        }
        rejected(&d,100,expected);
    }
    /* Overflow helper must reject before publishing any partial region. */
    const uint64_t cursors[]={UINT64_MAX,UINT32_MAX,UINT32_MAX-15};
    for(unsigned i=0;i<3;i++){
        unsigned long long cursor=cursors[i];unsigned off=123,size=456;
        assert(nvdec_h264_job_region(&cursor,256,256,&off,&size)==KH264_INVALID);
        assert(cursor==cursors[i] && off==123 && size==456);
    }
    unsigned long long cursor=4096;unsigned off=123,size=456;
    assert(nvdec_h264_job_region(&cursor,UINT64_MAX,256,&off,&size)==KH264_INVALID);
    assert(nvdec_h264_job_region(&cursor,0,256,&off,&size)==KH264_INVALID);
    assert(nvdec_h264_job_region(&cursor,256,0,&off,&size)==KH264_INVALID);
    assert(nvdec_h264_job_region(&cursor,256,255,&off,&size)==KH264_INVALID);
    assert(cursor==4096 && off==123 && size==456);
    printf("PASS dynamic IDR VRAM layout: %u dimension/reference combinations, EOS/alignment boundaries, independent Mesa scratch formulas, GOB_2 padded planes, all-region nonoverlap, malformed descriptors and overflow rejection\n",tested);
}
'''
    run_test(c, 'nvdec_h264_job')


if __name__ == '__main__':
    main()
