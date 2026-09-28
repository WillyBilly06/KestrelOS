/* Bounded Annex-B -> baseline H.264 IDR decode description.
 * No allocation, GPU addresses, entropy decoding, or implicit DPB state.
 * Accepted subset: one SPS, one PPS, one progressive 8-bit 4:2:0 I/IDR slice;
 * POC types 0/2, CAVLC, no FMO/scaling matrices. Other profiles, interframes,
 * fields, extra slices/parameter sets return UNSUPPORTED, not a guessed setup.
 * SPS/PPS and slice HEADER syntax are checked; slice DATA remains the hardware
 * decoder's responsibility. OK is not proof that pixels decode successfully.
 * Syntax: ITU-T H.264 7.3.2.1/7.3.2.2/7.3.3, 7.4 and Annex B/E; named output
 * fields correspond to NVIDIA nvdec_drv.h::nvdec_h264_pic_s.
 */
#ifndef KESTREL_H264_DECODE_H
#define KESTREL_H264_DECODE_H

#define KH264_MAX_BYTES (16u * 1024u * 1024u)
#define KH264_MAX_DIMENSION 4096u
enum { KH264_OK=0, KH264_INVALID=1, KH264_TRUNCATED=2,
       KH264_UNSUPPORTED=3, KH264_MISSING_PARAMETERS=4 };

typedef struct {
    unsigned int profile_idc, level_idc, constraint_flags, sps_id, pps_id;
    unsigned int coded_width, coded_height, display_width, display_height;
    unsigned int crop_left, crop_top, crop_right, crop_bottom; /* pixel units */
    unsigned int width_mbs, height_mbs, max_num_ref_frames;
    unsigned int gaps_in_frame_num_allowed, direct_8x8_inference_flag;
    unsigned int log2_max_frame_num_minus4, pic_order_cnt_type;
    unsigned int log2_max_pic_order_cnt_lsb_minus4;
    unsigned int pic_order_present_flag, num_ref_idx_l0_active_minus1;
    unsigned int num_ref_idx_l1_active_minus1, deblocking_filter_control_present_flag;
    unsigned int constrained_intra_pred_flag, redundant_pic_cnt_present_flag;
    int pic_init_qp_minus26, chroma_qp_index_offset, second_chroma_qp_index_offset;
    unsigned int frame_num, idr_pic_id, pic_order_cnt_lsb;
    int delta_pic_order_cnt_bottom, field_order_cnt[2];
    unsigned int no_output_of_prior_pics_flag, long_term_reference_flag;
    int slice_qp_delta, slice_qp, slice_alpha_c0_offset_div2, slice_beta_offset_div2;
    unsigned int disable_deblocking_filter_idc;
    unsigned int slice_start, slice_end; /* start-code inclusive, end exclusive */
    unsigned int slice_header_bits; /* unescaped RBSP bits, excludes NAL header */
    unsigned int full_range, colour_description_present;
    unsigned int colour_primaries, transfer_characteristics, matrix_coefficients;
    unsigned int num_units_in_tick, time_scale, fixed_frame_rate;
} kh264_decode_desc;

typedef struct {
    const unsigned char *data;
    unsigned int bytes, pos, byte, left, zeros, bits;
    int error;
} kh264_bits;

static inline unsigned int kh264_u(kh264_bits *r, unsigned int n) {
    unsigned int v=0;
    if(n>32){r->error=KH264_INVALID;return 0;}
    while(n-- && !r->error){
        if(!r->left){
            if(r->pos==r->bytes){r->error=KH264_TRUNCATED;break;}
            unsigned int b=r->data[r->pos++];
            if(r->zeros==2 && b==3){
                if(r->pos==r->bytes){r->error=KH264_TRUNCATED;break;}
                b=r->data[r->pos++];r->zeros=0;
                if(b>3){r->error=KH264_INVALID;break;}
            }else if(r->zeros==2 && b<3){r->error=KH264_INVALID;break;}
            r->zeros=b==0?r->zeros+1:0;r->byte=b;r->left=8;
        }
        v=(v<<1)|((r->byte>>--r->left)&1u);r->bits++;
    }
    return v;
}
static inline unsigned int kh264_ue(kh264_bits *r) {
    unsigned int z=0;
    while(!r->error && !kh264_u(r,1)){
        if(++z>31){r->error=KH264_INVALID;return 0;}
    }
    if(r->error)return 0;
    return ((1u<<z)-1u)+kh264_u(r,z);
}
static inline int kh264_se(kh264_bits *r) {
    unsigned int v=kh264_ue(r);
    return v&1u?(int)(v/2u+1u):-(int)(v/2u);
}
static inline int kh264_trailing(kh264_bits *r) {
    if(kh264_u(r,1)!=1 && !r->error)r->error=KH264_INVALID;
    while(r->left && !r->error)if(kh264_u(r,1))r->error=KH264_INVALID;
    if(!r->error && r->pos!=r->bytes)r->error=KH264_INVALID;
    return r->error;
}
/* Validate the entire EBSP, including unparsed slice data, and find the final
 * rbsp_stop_one_bit. This does not validate macroblocks or entropy syntax. */
static inline int kh264_ebsp(const unsigned char *p,unsigned int n,unsigned int *data_bits){
    kh264_bits r={.data=p,.bytes=n};unsigned int last=0,count=0;
    if(!n)return KH264_TRUNCATED;
    while(r.pos<n && !r.error){last=kh264_u(&r,8);count++;}
    if(r.error)return r.error;
    if(!last)return KH264_INVALID;
    unsigned int zeros=0;while(!(last&1)){last>>=1;zeros++;}
    *data_bits=count*8u-zeros-1u;return KH264_OK;
}
static inline int kh264_hrd(kh264_bits *r){
    unsigned int count=kh264_ue(r);
    if(r->error)return r->error;
    if(count>31)return KH264_INVALID;
    kh264_u(r,4);kh264_u(r,4);
    for(unsigned int i=0;i<=count && !r->error;i++){
        kh264_ue(r);kh264_ue(r);kh264_u(r,1);
    }
    kh264_u(r,5);kh264_u(r,5);kh264_u(r,5);kh264_u(r,5);
    return r->error;
}
static inline int kh264_vui(kh264_bits *r,kh264_decode_desc *d){
    if(kh264_u(r,1)){
        unsigned int aspect=kh264_u(r,8);
        if(aspect==255){unsigned int w=kh264_u(r,16),h=kh264_u(r,16);if(!w||!h)return r->error?r->error:KH264_INVALID;}
        else if(aspect>16)return KH264_UNSUPPORTED;
    }
    if(kh264_u(r,1))kh264_u(r,1);
    if(kh264_u(r,1)){
        unsigned int format=kh264_u(r,3);if(format>5)return KH264_INVALID;
        d->full_range=kh264_u(r,1);d->colour_description_present=kh264_u(r,1);
        if(d->colour_description_present){d->colour_primaries=kh264_u(r,8);d->transfer_characteristics=kh264_u(r,8);d->matrix_coefficients=kh264_u(r,8);}
    }
    if(kh264_u(r,1)){unsigned int a=kh264_ue(r),b=kh264_ue(r);if(a>5||b>5)return KH264_INVALID;}
    if(kh264_u(r,1)){
        d->num_units_in_tick=kh264_u(r,32);d->time_scale=kh264_u(r,32);d->fixed_frame_rate=kh264_u(r,1);
        if(!d->num_units_in_tick||!d->time_scale)return r->error?r->error:KH264_INVALID;
    }
    unsigned int nal_hrd=kh264_u(r,1);int rc;
    if(nal_hrd && (rc=kh264_hrd(r)))return rc;
    unsigned int vcl_hrd=kh264_u(r,1);
    if(vcl_hrd && (rc=kh264_hrd(r)))return rc;
    if(nal_hrd||vcl_hrd)kh264_u(r,1);
    kh264_u(r,1);
    if(kh264_u(r,1)){
        kh264_u(r,1);
        for(unsigned int i=0;i<4;i++)if(kh264_ue(r)>16)return KH264_INVALID;
        unsigned int reorder=kh264_ue(r),buffer=kh264_ue(r);
        if(reorder>buffer||buffer>16||buffer<d->max_num_ref_frames)return KH264_INVALID;
    }
    return r->error;
}
static inline int kh264_sps(kh264_bits *r,kh264_decode_desc *d){
    d->profile_idc=kh264_u(r,8);d->constraint_flags=kh264_u(r,8);d->level_idc=kh264_u(r,8);
    if(r->error)return r->error;
    if(d->constraint_flags&3)return KH264_INVALID;
    if(d->profile_idc!=66)return KH264_UNSUPPORTED;
    switch(d->level_idc){
    case 10:case 11:case 12:case 13:case 20:case 21:case 22:case 30:case 31:case 32:
    case 40:case 41:case 42:case 50:case 51:case 52:case 60:case 61:case 62:break;
    default:return KH264_UNSUPPORTED;
    }
    d->sps_id=kh264_ue(r);d->log2_max_frame_num_minus4=kh264_ue(r);d->pic_order_cnt_type=kh264_ue(r);
    if(r->error)return r->error;
    if(d->sps_id>31||d->log2_max_frame_num_minus4>12||d->pic_order_cnt_type>2)return KH264_INVALID;
    if(d->pic_order_cnt_type==1)return KH264_UNSUPPORTED;
    if(d->pic_order_cnt_type==0){d->log2_max_pic_order_cnt_lsb_minus4=kh264_ue(r);if(d->log2_max_pic_order_cnt_lsb_minus4>12)return KH264_INVALID;}
    d->max_num_ref_frames=kh264_ue(r);d->gaps_in_frame_num_allowed=kh264_u(r,1);
    unsigned int w=kh264_ue(r),h=kh264_ue(r);
    if(r->error)return r->error;
    if(d->max_num_ref_frames>16)return KH264_INVALID;
    if(w>=KH264_MAX_DIMENSION/16u||h>=KH264_MAX_DIMENSION/16u)return KH264_UNSUPPORTED;
    d->width_mbs=w+1;d->height_mbs=h+1;d->coded_width=(w+1)*16;d->coded_height=(h+1)*16;
    if(!kh264_u(r,1))return r->error?r->error:KH264_UNSUPPORTED;
    d->direct_8x8_inference_flag=kh264_u(r,1);
    if(kh264_u(r,1)){
        unsigned int l=kh264_ue(r),rr=kh264_ue(r),t=kh264_ue(r),b=kh264_ue(r);
        if(r->error)return r->error;
        if(l>=d->coded_width/2||rr>=d->coded_width/2-l||t>=d->coded_height/2||b>=d->coded_height/2-t)return KH264_INVALID;
        d->crop_left=l*2;d->crop_right=rr*2;d->crop_top=t*2;d->crop_bottom=b*2;
    }
    d->display_width=d->coded_width-d->crop_left-d->crop_right;
    d->display_height=d->coded_height-d->crop_top-d->crop_bottom;
    if(kh264_u(r,1)){int rc=kh264_vui(r,d);if(rc)return rc;}
    return kh264_trailing(r);
}
static inline int kh264_pps(kh264_bits *r,kh264_decode_desc *d){
    d->pps_id=kh264_ue(r);unsigned int sid=kh264_ue(r),cabac=kh264_u(r,1);
    if(r->error)return r->error;
    if(d->pps_id>255||sid>31)return KH264_INVALID;
    if(sid!=d->sps_id)return KH264_MISSING_PARAMETERS;
    if(cabac)return KH264_UNSUPPORTED;
    d->pic_order_present_flag=kh264_u(r,1);
    if(kh264_ue(r))return r->error?r->error:KH264_UNSUPPORTED;
    d->num_ref_idx_l0_active_minus1=kh264_ue(r);d->num_ref_idx_l1_active_minus1=kh264_ue(r);
    unsigned int weighted=kh264_u(r,1),bipred=kh264_u(r,2);
    d->pic_init_qp_minus26=kh264_se(r);int qs=kh264_se(r);d->chroma_qp_index_offset=kh264_se(r);
    if(r->error)return r->error;
    if(d->num_ref_idx_l0_active_minus1>31||d->num_ref_idx_l1_active_minus1>31||bipred>2||
       d->pic_init_qp_minus26 < -26||d->pic_init_qp_minus26>25||qs < -26||qs>25||
       d->chroma_qp_index_offset < -12||d->chroma_qp_index_offset>12)return KH264_INVALID;
    if(weighted||bipred)return KH264_UNSUPPORTED;
    d->deblocking_filter_control_present_flag=kh264_u(r,1);d->constrained_intra_pred_flag=kh264_u(r,1);d->redundant_pic_cnt_present_flag=kh264_u(r,1);
    d->second_chroma_qp_index_offset=d->chroma_qp_index_offset;
    kh264_bits tail=*r;
    if(!kh264_trailing(&tail))return KH264_OK;
    unsigned int transform=kh264_u(r,1),scaling=kh264_u(r,1);
    if(r->error)return r->error;
    if(transform||scaling)return KH264_UNSUPPORTED;
    d->second_chroma_qp_index_offset=kh264_se(r);
    if(d->second_chroma_qp_index_offset < -12||d->second_chroma_qp_index_offset>12)return KH264_INVALID;
    return kh264_trailing(r);
}
static inline int kh264_idr(kh264_bits *r,kh264_decode_desc *d,unsigned int data_bits){
    unsigned int first=kh264_ue(r),type=kh264_ue(r),pid=kh264_ue(r);
    if(r->error)return r->error;
    if(type>9||pid>255)return KH264_INVALID;
    if(first||type%5!=2)return KH264_UNSUPPORTED;
    if(pid!=d->pps_id)return KH264_MISSING_PARAMETERS;
    d->frame_num=kh264_u(r,d->log2_max_frame_num_minus4+4);d->idr_pic_id=kh264_ue(r);
    if(d->pic_order_cnt_type==0){
        d->pic_order_cnt_lsb=kh264_u(r,d->log2_max_pic_order_cnt_lsb_minus4+4);
        if(d->pic_order_present_flag)d->delta_pic_order_cnt_bottom=kh264_se(r);
    }
    if(d->redundant_pic_cnt_present_flag && kh264_ue(r))return r->error?r->error:KH264_UNSUPPORTED;
    d->no_output_of_prior_pics_flag=kh264_u(r,1);d->long_term_reference_flag=kh264_u(r,1);
    d->slice_qp_delta=kh264_se(r);
    if(r->error)return r->error;
    if(d->frame_num||d->idr_pic_id>65535||d->slice_qp_delta < -51||d->slice_qp_delta>51)return KH264_INVALID;
    d->slice_qp=26+d->pic_init_qp_minus26+d->slice_qp_delta;
    if(d->slice_qp<0||d->slice_qp>51)return KH264_INVALID;
    if(d->deblocking_filter_control_present_flag){
        d->disable_deblocking_filter_idc=kh264_ue(r);
        if(d->disable_deblocking_filter_idc>2)return KH264_INVALID;
        if(d->disable_deblocking_filter_idc!=1){d->slice_alpha_c0_offset_div2=kh264_se(r);d->slice_beta_offset_div2=kh264_se(r);}
        if(d->slice_alpha_c0_offset_div2 < -6||d->slice_alpha_c0_offset_div2>6||d->slice_beta_offset_div2 < -6||d->slice_beta_offset_div2>6)return KH264_INVALID;
    }
    if(r->error)return r->error;
    if(r->bits>=data_bits)return KH264_TRUNCATED; /* no slice_data before stop bit */
    d->slice_header_bits=r->bits;
    d->field_order_cnt[0]=(int)d->pic_order_cnt_lsb;
    /* Guard signed addition before deriving bottom-field order count. */
    if(d->delta_pic_order_cnt_bottom>0x7fffffff-d->field_order_cnt[0])return KH264_INVALID;
    d->field_order_cnt[1]=d->field_order_cnt[0]+d->delta_pic_order_cnt_bottom;
    return KH264_OK;
}
static inline unsigned int kh264_start(const unsigned char *p,unsigned int n,unsigned int from,unsigned int *prefix){
    for(unsigned int i=from;i+2<n;i++){
        if(p[i]||p[i+1])continue;
        if(p[i+2]==1){*prefix=3;return i;}
        if(i+3<n&&!p[i+2]&&p[i+3]==1){*prefix=4;return i;}
    }
    *prefix=0;return n;
}
static inline int kh264_parse_annexb(const unsigned char *data,unsigned int bytes,kh264_decode_desc *out){
    if(!out)return KH264_INVALID;
    *out=(kh264_decode_desc){0};
    if(!data||!bytes)return KH264_TRUNCATED;
    if(bytes>KH264_MAX_BYTES)return KH264_UNSUPPORTED;
    kh264_decode_desc d={0};unsigned int prefix=0,start=kh264_start(data,bytes,0,&prefix),seen=0,units=0;
    if(!prefix)return KH264_INVALID;
    for(unsigned int i=0;i<start;i++)if(data[i])return KH264_INVALID;
    while(start<bytes){
        if(++units>64)return KH264_UNSUPPORTED;
        unsigned int header=start+prefix,next_prefix=0;
        if(header>=bytes)return KH264_TRUNCATED;
        unsigned int next=kh264_start(data,bytes,header+1,&next_prefix),end=next;
        while(end>header+1&&!data[end-1])end--; /* Annex-B trailing_zero_8bits */
        unsigned int type=data[header]&31,ref=(data[header]>>5)&3,bits=0;
        if(data[header]&0x80)return KH264_INVALID;
        if(!type||type>12||type<5)return KH264_UNSUPPORTED;
        if((type==5||type==7||type==8)?!ref:ref!=0)return KH264_INVALID;
        int rc=kh264_ebsp(data+header+1,end-header-1,&bits);if(rc)return rc;
        kh264_bits r={.data=data+header+1,.bytes=end-header-1};
        if(type==7){if(seen)return KH264_UNSUPPORTED;rc=kh264_sps(&r,&d);seen|=1;}
        else if(type==8){if(!(seen&1))return KH264_MISSING_PARAMETERS;if(seen&6)return KH264_UNSUPPORTED;rc=kh264_pps(&r,&d);seen|=2;}
        else if(type==5){if((seen&3)!=3)return KH264_MISSING_PARAMETERS;if(seen&4)return KH264_UNSUPPORTED;rc=kh264_idr(&r,&d,bits);d.slice_start=start;d.slice_end=end;seen|=4;}
        else if(type==9){kh264_u(&r,3);rc=kh264_trailing(&r);}
        else if(type==10||type==11){rc=kh264_trailing(&r);}
        else if(type==12){while(r.bits<bits&&!r.error)if(kh264_u(&r,8)!=255)return KH264_INVALID;rc=kh264_trailing(&r);}
        else { /* SEI payload framing is byte-oriented; semantics are not decoder state. */
            while(r.bits<bits&&!r.error){
                unsigned int b=255,size=0;
                while(b==255&&!r.error)b=kh264_u(&r,8); /* payloadType */
                b=255;
                while(b==255&&!r.error){b=kh264_u(&r,8);if(size>KH264_MAX_BYTES-b)return KH264_INVALID;size+=b;}
                if(r.error)return r.error;
                if(r.bits>bits||size>(bits-r.bits)/8)return KH264_TRUNCATED;
                while(size--&&!r.error)kh264_u(&r,8);
            }
            rc=kh264_trailing(&r);
        }
        if(rc)return rc;
        start=next;prefix=next_prefix;
    }
    if(seen!=7)return KH264_MISSING_PARAMETERS;
    *out=d;return KH264_OK;
}
#endif
