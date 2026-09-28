/* Convert supported Annex-B headers to the NVIDIA picture ABI. Header parsing
 * is CPU control work; compressed macroblocks are decoded only by NVDEC.
 * Mapping follows out/mesa-ref/src/nouveau/vulkan/rust/video/decode/h264.rs.
 * This builds no allocations/DPB and does not imply successful GPU execution. */
#ifndef KESTREL_NVDEC_H264_CONTEXT_H
#define KESTREL_NVDEC_H264_CONTEXT_H
#include "nvdec_drv_h264.h"
#include "../include/kestrel/h264_decode.h"

static inline int nvdec_h264_parse_pic(
    nvdec_h264_pic_s *out, kh264_decode_desc *desc,
    const unsigned char *annexb, unsigned int bytes,
    unsigned int luma_pitch, unsigned int chroma_pitch,
    unsigned int history_bytes, unsigned int mbhist_bytes)
{
    if (!out || !desc) return KH264_INVALID;
    *out = (nvdec_h264_pic_s){0};
    *desc = (kh264_decode_desc){0};
    kh264_decode_desc d;
    int rc = kh264_parse_annexb(annexb, bytes, &d);
    if (rc != KH264_OK) return rc;
    /* Current Blackwell backend: progressive 8-bit NV12, GOB_2, no DPB.
     * Pitches describe coded (not cropped display) dimensions. Allocation
     * bounds and scratch sizing remain the submitting backend's responsibility. */
    if (d.coded_width < 48 || d.coded_height < 64)
        return KH264_UNSUPPORTED;
    if (luma_pitch < d.coded_width || chroma_pitch < d.coded_width ||
        luma_pitch > KH264_MAX_DIMENSION || chroma_pitch > KH264_MAX_DIMENSION ||
        (luma_pitch & 63u) || (chroma_pitch & 63u) ||
        !history_bytes || (history_bytes & 255u) || !mbhist_bytes)
        return KH264_INVALID;
    nvdec_h264_pic_s p = {0};
    static const unsigned char eos[16] =
        {0,0,1,0xb,0,0,0,0,0,0,1,0xb,0,0,0,0};
    for (unsigned int i=0; i<16; i++) p.eos[i] = eos[i];
    p.explicitEOSPresentFlag = 1;
    p.stream_len = bytes + 16u; /* caller appends EOS outside slice_end */
    p.slice_count = 1;
    p.mbhist_buffer_size = mbhist_bytes;
    p.gptimer_timeout_value = 81000000u;
    p.log2_max_pic_order_cnt_lsb_minus4 = d.log2_max_pic_order_cnt_lsb_minus4;
    p.frame_mbs_only_flag = 1;
    p.PicWidthInMbs = d.width_mbs;
    p.FrameHeightInMbs = d.height_mbs;
    p.tileFormat = 1;
    p.gob_height = 0; /* GOB_2's y_log2 minus one */
    p.pic_order_present_flag = d.pic_order_present_flag;
    p.num_ref_idx_l0_active_minus1 = d.num_ref_idx_l0_active_minus1;
    p.num_ref_idx_l1_active_minus1 = d.num_ref_idx_l1_active_minus1;
    p.deblocking_filter_control_present_flag = d.deblocking_filter_control_present_flag;
    p.redundant_pic_cnt_present_flag = d.redundant_pic_cnt_present_flag;
    p.pitch_luma = luma_pitch;
    p.pitch_chroma = chroma_pitch;
    p.luma_bot_offset = d.coded_width;
    p.chroma_bot_offset = chroma_pitch / 2u;
    p.HistBufferSize = history_bytes >> 8;
    p.direct_8x8_inference_flag = d.direct_8x8_inference_flag;
    p.constrained_intra_pred_flag = d.constrained_intra_pred_flag;
    p.ref_pic_flag = 1; /* parser accepts reference IDRs only */
    p.log2_max_frame_num_minus4 = d.log2_max_frame_num_minus4;
    p.chroma_format_idc = 1;
    p.pic_order_cnt_type = d.pic_order_cnt_type;
    p.pic_init_qp_minus26 = d.pic_init_qp_minus26;
    p.chroma_qp_index_offset = d.chroma_qp_index_offset;
    p.second_chroma_qp_index_offset = d.second_chroma_qp_index_offset;
    p.frame_num = d.frame_num;
    p.CurrFieldOrderCnt[0] = d.field_order_cnt[0];
    p.CurrFieldOrderCnt[1] = d.field_order_cnt[1];
    for (unsigned int a=0; a<6; a++)
        for (unsigned int b=0; b<4; b++)
            for (unsigned int c=0; c<4; c++) p.WeightScale[a][b][c] = 16;
    for (unsigned int a=0; a<2; a++)
        for (unsigned int b=0; b<8; b++)
            for (unsigned int c=0; c<8; c++) p.WeightScale8x8[a][b][c] = 16;
    *out = p;
    *desc = d;
    return KH264_OK;
}
#endif
