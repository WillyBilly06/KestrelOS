/* Checked VRAM plan for one parsed progressive baseline H.264 IDR.
 * This owns no resources and retains no decoded-picture buffer. Parser success
 * is required before planning; geometry is rechecked here before arithmetic.
 * Scratch formulas follow Mesa nouveau/vulkan/rust/video/decode/h264.rs.
 * Coloc reserves the SPS reference capacity plus this picture, not a claim of
 * interframe support. NV12 output uses two-GOB blocks (64 bytes x16 rows).
 */
#ifndef KESTREL_NVDEC_H264_JOB_H
#define KESTREL_NVDEC_H264_JOB_H
#include "nvdec_drv_h264.h"
#include "../include/kestrel/h264_decode.h"

typedef struct {
    unsigned int stream_offset, stream_bytes;
    unsigned int picture_offset, picture_bytes;
    unsigned int slice_offset, slice_bytes;
    unsigned int coloc_offset, coloc_bytes;
    unsigned int history_offset, history_bytes;
    unsigned int mbhist_offset, mbhist_bytes;
    unsigned int status_offset, status_bytes;
    unsigned int luma_offset, luma_bytes;
    unsigned int chroma_offset, chroma_bytes;
    unsigned int pitch, luma_height, chroma_height, total_bytes;
} nvdec_h264_job_layout;

static inline int nvdec_h264_job_region(unsigned long long *cursor,
    unsigned long long bytes, unsigned int alignment,
    unsigned int *offset, unsigned int *size)
{
    if (!cursor || !offset || !size || !bytes || !alignment ||
        (alignment & (alignment-1u)) || *cursor > 0xffffffffull ||
        bytes > 0xffffffffull) return KH264_INVALID;
    unsigned long long start = (*cursor + alignment-1u) & ~((unsigned long long)alignment-1u);
    if (start > 0xffffffffull || bytes > 0xffffffffull-start) return KH264_INVALID;
    *offset = (unsigned int)start;
    *size = (unsigned int)bytes;
    *cursor = start+bytes;
    return KH264_OK;
}

static inline int nvdec_h264_job_plan(nvdec_h264_job_layout *out,
    const kh264_decode_desc *desc, unsigned int stream_bytes)
{
    if (!out) return KH264_INVALID;
    *out = (nvdec_h264_job_layout){0};
    if (!desc || !stream_bytes || stream_bytes > KH264_MAX_BYTES ||
        !desc->coded_width || !desc->coded_height ||
        !desc->width_mbs || !desc->height_mbs || desc->max_num_ref_frames > 16u ||
        desc->slice_start >= desc->slice_end || desc->slice_end > stream_bytes)
        return KH264_INVALID;
    if (desc->coded_width < 48u || desc->coded_height < 64u ||
        desc->coded_width > KH264_MAX_DIMENSION || desc->coded_height > KH264_MAX_DIMENSION)
        return KH264_UNSUPPORTED;
    if ((desc->coded_width & 15u) || (desc->coded_height & 15u) ||
        desc->width_mbs != desc->coded_width/16u ||
        desc->height_mbs != desc->coded_height/16u) return KH264_INVALID;
    nvdec_h264_job_layout p = {0};
    p.pitch = (desc->coded_width+63u)&~63u;
    p.luma_height = (desc->coded_height+15u)&~15u;
    p.chroma_height = (desc->coded_height/2u+15u)&~15u;
    unsigned long long mbw = desc->width_mbs, mbh = desc->height_mbs;
    unsigned long long coloc = ((((mbh+1u)&~1ull)*mbw*64u-63u+255u)&~255ull) *
                               (desc->max_num_ref_frames+1ull);
    unsigned long long history = (mbw*0x300u+0x1ffu)&~0x1ffull;
    unsigned long long mbhist = (mbw*104u+255u)&~255ull;
    unsigned long long cursor = 0x1000u; /* preserve a leading guard page */
    if (nvdec_h264_job_region(&cursor,(unsigned long long)stream_bytes+16u,256,&p.stream_offset,&p.stream_bytes) ||
        nvdec_h264_job_region(&cursor,sizeof(nvdec_h264_pic_s),256,&p.picture_offset,&p.picture_bytes) ||
        nvdec_h264_job_region(&cursor,2u*sizeof(unsigned int),256,&p.slice_offset,&p.slice_bytes) ||
        nvdec_h264_job_region(&cursor,coloc,256,&p.coloc_offset,&p.coloc_bytes) ||
        nvdec_h264_job_region(&cursor,history,256,&p.history_offset,&p.history_bytes) ||
        nvdec_h264_job_region(&cursor,mbhist,256,&p.mbhist_offset,&p.mbhist_bytes) ||
        nvdec_h264_job_region(&cursor,sizeof(nvdec_frame_status_t),256,&p.status_offset,&p.status_bytes) ||
        nvdec_h264_job_region(&cursor,(unsigned long long)p.pitch*p.luma_height,1024,&p.luma_offset,&p.luma_bytes) ||
        nvdec_h264_job_region(&cursor,(unsigned long long)p.pitch*p.chroma_height,1024,&p.chroma_offset,&p.chroma_bytes))
        return KH264_INVALID;
    cursor = (cursor+0xffffu)&~0xffffull;
    if (cursor > 0xffffffffull) return KH264_INVALID;
    p.total_bytes = (unsigned int)cursor;
    *out = p;
    return KH264_OK;
}
#endif
