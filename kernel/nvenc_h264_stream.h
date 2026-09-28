/* Package retired baseline NVENC output with its actual encoder context.
 * SPS/PPS writing is control metadata, not CPU pixel/entropy encoding. No slice
 * bytes are manufactured. A successful result still needs an actual decode. */
#ifndef KESTREL_NVENC_H264_STREAM_H
#define KESTREL_NVENC_H264_STREAM_H
#include "nvenc_drv_h264.h"
#include "../include/kestrel/h264_decode.h"
#define NVENC_H264_STREAM_CAPACITY (NVENC_H264_TEST_OUTPUT_CAPACITY+128u)

typedef struct { unsigned char bytes[64]; unsigned int bits; int error; } nvenc_h264_writer;
static inline void nvenc_h264_put(nvenc_h264_writer *w,unsigned int n,unsigned int v) {
    if(n>32 || n>sizeof(w->bytes)*8u-w->bits){w->error=1;return;}
    for(unsigned int i=n;i;i--){
        unsigned int b=w->bits++;
        w->bytes[b/8u]|=((v>>(i-1u))&1u)<<(7u-b%8u);
    }
}
static inline void nvenc_h264_ue(nvenc_h264_writer *w,unsigned int v) {
    if(v==0xffffffffu){w->error=1;return;}
    unsigned int n=0,x=v+1;while(x){n++;x>>=1;}
    nvenc_h264_put(w,n-1u,0);nvenc_h264_put(w,n,v+1u);
}
static inline void nvenc_h264_se(nvenc_h264_writer *w,int v) {
    /* This metadata writer only needs bounded QP fields. */
    if(v < -51 || v>51){w->error=1;return;}
    nvenc_h264_ue(w,v>0?(unsigned int)v*2u-1u:(unsigned int)(-v)*2u);
}
static inline unsigned int nvenc_h264_nal(unsigned char *out,unsigned int cap,
    unsigned char header,nvenc_h264_writer *w) {
    nvenc_h264_put(w,1,1);
    while(!w->error && w->bits%8u)nvenc_h264_put(w,1,0);
    if(w->error || cap<5)return 0;
    out[0]=out[1]=out[2]=0;out[3]=1;out[4]=header;
    unsigned int at=5,zeros=0;
    for(unsigned int i=0;i<w->bits/8u;i++){
        unsigned int b=w->bytes[i];
        if(zeros==2 && b<=3){if(at==cap)return 0;out[at++]=3;zeros=0;}
        if(at==cap)return 0;out[at++]=(unsigned char)b;zeros=b==0?zeros+1u:0;
    }
    return at;
}

static inline int nvenc_h264_context_matches(const nvenc_cfb7_h264_drv_pic_setup_s *c,
                                            const kh264_decode_desc *d) {
    return d->profile_idc==c->sps_data.profile_idc && d->level_idc==c->sps_data.level_idc &&
        d->coded_width==(unsigned)c->input_cfg.frame_width_minus1+1u &&
        d->coded_height==(unsigned)c->input_cfg.frame_height_minus1+1u &&
        d->pps_id==c->pps_data.pic_param_set_id &&
        d->log2_max_frame_num_minus4==c->sps_data.log2_max_frame_num_minus4 &&
        d->pic_order_cnt_type==c->sps_data.pic_order_cnt_type &&
        d->log2_max_pic_order_cnt_lsb_minus4==c->sps_data.log2_max_pic_order_cnt_lsb_minus4 &&
        d->pic_order_present_flag==c->pps_data.pic_order_present_flag &&
        d->pic_init_qp_minus26==c->pps_data.pic_init_qp_minus26 &&
        d->chroma_qp_index_offset==c->pps_data.chroma_qp_index_offset &&
        d->second_chroma_qp_index_offset==c->pps_data.second_chroma_qp_index_offset &&
        d->deblocking_filter_control_present_flag==c->pps_data.deblocking_filter_control_present_flag &&
        d->constrained_intra_pred_flag==c->pps_data.constrained_intra_pred_flag &&
        d->num_ref_idx_l0_active_minus1==c->pps_data.num_ref_idx_l0_active_minus1 &&
        d->num_ref_idx_l1_active_minus1==c->pps_data.num_ref_idx_l1_active_minus1 &&
        d->frame_num==c->pic_control.frame_num && d->idr_pic_id==c->pic_control.idr_pic_id &&
        d->pic_order_cnt_lsb==c->pic_control.pic_order_cnt_lsb &&
        !d->delta_pic_order_cnt_bottom && !c->pic_control.delta_pic_order_cnt_bottom &&
        !d->redundant_pic_cnt_present_flag &&
        !d->crop_left && !d->crop_top && !d->crop_right && !d->crop_bottom;
}

/* Destination is private staging: on failure its contents are unspecified,
 * but *written is always zero. Source/destination overlap is rejected. When
 * firmware already supplies SPS/PPS, validate and preserve the WHOLE stream;
 * never replace mismatching metadata with headers that conceal a discrepancy.
 * Otherwise create SPS/PPS and copy the actual single IDR NAL byte for byte.
 * This independent-IDR path declares one reference slot, no crop or VUI. */
static inline int nvenc_h264_make_stream(unsigned char *out,unsigned int cap,
    const nvenc_cfb7_h264_drv_pic_setup_s *c,const unsigned char *raw,unsigned int bytes,
    unsigned int *written,kh264_decode_desc *desc) {
    if(written)*written=0;
    if(desc)*desc=(kh264_decode_desc){0};
    if(!written || !desc)return 0;
    if(!out || !c || !raw || !bytes || bytes>NVENC_H264_TEST_OUTPUT_CAPACITY)return 0;
    __UINTPTR_TYPE__ a=(__UINTPTR_TYPE__)out,b=(__UINTPTR_TYPE__)raw;
    if(cap>~(__UINTPTR_TYPE__)0-a || bytes>~(__UINTPTR_TYPE__)0-b ||
       (a<b+bytes && b<a+cap))return 0;
    unsigned int w=(unsigned)c->input_cfg.frame_width_minus1+1u;
    unsigned int h=(unsigned)c->input_cfg.frame_height_minus1+1u;
    if(c->magic!=NVENC_CFB7_DRV_MAGIC || c->sps_data.profile_idc!=66 ||
       c->sps_data.chroma_format_idc!=1 || !c->sps_data.frame_mbs_only ||
       c->sps_data.stereo_mvc_enable || c->sps_data.separate_colour_plane_flag ||
       c->sps_data.lossless_qpprime_flag || c->pps_data.entropy_coding_mode_flag ||
       c->pps_data.transform_8x8_mode_flag || c->pps_data.weighted_pred_flag ||
       c->pps_data.weighted_bipred_idc || c->pic_control.pic_type!=3 ||
        !c->pic_control.ref_pic_flag || c->pic_control.frame_num ||
        c->pic_control.delta_pic_order_cnt_bottom || c->pic_control.pic_struct ||
        c->pic_control.bit_depth_minus_8 ||
       w>4096 || h>4096 || ((w|h)&15u))return 0;
    unsigned int first=0,end=0;
    if(!nvenc_h264_single_idr(raw,bytes,&first,&end))return 0;
    int has_parameters=0;
    for(unsigned int i=0;i+3u<bytes;i++){
        if(raw[i] || raw[i+1])continue;
        unsigned int p=raw[i+2]==1?3u:(!raw[i+2] && i+4u<bytes && raw[i+3]==1?4u:0u);
        if(p && i+p<bytes && ((raw[i+p]&31u)==7 || (raw[i+p]&31u)==8))has_parameters=1;
    }
    kh264_decode_desc d;
    if(has_parameters){
        if(cap<bytes || kh264_parse_annexb(raw,bytes,&d)!=KH264_OK || !nvenc_h264_context_matches(c,&d))return 0;
        for(unsigned int i=0;i<bytes;i++)out[i]=raw[i];
        *written=bytes;*desc=d;return 1;
    }
    unsigned char headers[128];nvenc_h264_writer s={0},p={0};
    nvenc_h264_put(&s,8,c->sps_data.profile_idc);nvenc_h264_put(&s,8,0);
    nvenc_h264_put(&s,8,c->sps_data.level_idc);nvenc_h264_ue(&s,0); /* SPS id */
    nvenc_h264_ue(&s,c->sps_data.log2_max_frame_num_minus4);
    nvenc_h264_ue(&s,c->sps_data.pic_order_cnt_type);
    if(c->sps_data.pic_order_cnt_type==0)nvenc_h264_ue(&s,c->sps_data.log2_max_pic_order_cnt_lsb_minus4);
    else if(c->sps_data.pic_order_cnt_type!=2)return 0;
    nvenc_h264_ue(&s,1);nvenc_h264_put(&s,1,0); /* reference capacity, no gaps */
    nvenc_h264_ue(&s,w/16u-1u);nvenc_h264_ue(&s,h/16u-1u);
    nvenc_h264_put(&s,1,1);nvenc_h264_put(&s,1,1); /* frame-only, direct8x8 */
    nvenc_h264_put(&s,1,0);nvenc_h264_put(&s,1,0); /* no cropping or VUI */
    unsigned int n=nvenc_h264_nal(headers,sizeof headers,0x67,&s);if(!n)return 0;
    nvenc_h264_ue(&p,c->pps_data.pic_param_set_id);nvenc_h264_ue(&p,0);
    nvenc_h264_put(&p,1,0);nvenc_h264_put(&p,1,c->pps_data.pic_order_present_flag);
    nvenc_h264_ue(&p,0); /* no slice groups */
    nvenc_h264_ue(&p,c->pps_data.num_ref_idx_l0_active_minus1);
    nvenc_h264_ue(&p,c->pps_data.num_ref_idx_l1_active_minus1);
    nvenc_h264_put(&p,1,0);nvenc_h264_put(&p,2,0); /* no weighting */
    nvenc_h264_se(&p,c->pps_data.pic_init_qp_minus26);nvenc_h264_se(&p,0);
    nvenc_h264_se(&p,c->pps_data.chroma_qp_index_offset);
    nvenc_h264_put(&p,1,c->pps_data.deblocking_filter_control_present_flag);
    nvenc_h264_put(&p,1,c->pps_data.constrained_intra_pred_flag);nvenc_h264_put(&p,1,0);
    nvenc_h264_put(&p,1,0);nvenc_h264_put(&p,1,0); /* no transform8/scaling lists */
    nvenc_h264_se(&p,c->pps_data.second_chroma_qp_index_offset);
    unsigned int m=nvenc_h264_nal(headers+n,sizeof headers-n,0x68,&p);if(!m)return 0;n+=m;
    if(n>cap || end-first>cap-n)return 0;
    for(unsigned int i=0;i<n;i++)out[i]=headers[i];
    for(unsigned int i=first;i<end;i++)out[n+i-first]=raw[i];
    unsigned int total=n+end-first;
    if(kh264_parse_annexb(out,total,&d)!=KH264_OK || !nvenc_h264_context_matches(c,&d))return 0;
    *written=total;*desc=d;return 1;
}
#endif
