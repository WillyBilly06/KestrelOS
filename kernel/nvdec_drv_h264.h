/* nvdec_drv_h264.h - NVDEC H.264 decode: the picture-setup struct and the
 * class method offsets, for a baseline I-frame decode on Blackwell (0xcfb0).
 *
 * The nvdec_h264_pic_s struct and its sub-structs are copied VERBATIM from
 * NVIDIA's own header as vendored by Mesa
 * (gpu-refs/mesa/src/nouveau/headers/nvidia/video/nvdec_drv.h) so the C
 * compiler computes byte-identical field offsets - we populate fields BY NAME,
 * never by hand-transcribed offset ([[kestrelos-verify-transcribed-constants]]).
 * The method offsets are class NVC5B0 (Ampere), byte-identical to Blackwell's
 * NVCFB0 (clcfb0.h is class-id-only; the pushbuffer ABI is stable across
 * generations - verified: every offset below matches classes/clc5b0.h).
 */
#ifndef KESTREL_NVDEC_DRV_H264_H
#define KESTREL_NVDEC_DRV_H264_H

/* ---- method offsets (verified vs Mesa classes/clc5b0.h) ---- */
#define NVCFB0_SET_APPLICATION_ID            0x00000200u
#define NVCFB0_SET_APPLICATION_ID_ID_H264    0x00000003u
#define NVCFB0_SEMAPHORE_A                   0x00000240u
#define NVCFB0_SEMAPHORE_D                   0x00000304u
#define NVCFB0_EXECUTE                       0x00000300u
#define NVCFB0_SET_CONTROL_PARAMS            0x00000400u
#define NVCFB0_SET_DRV_PIC_SETUP_OFFSET      0x00000404u
#define NVCFB0_SET_IN_BUF_BASE_OFFSET        0x00000408u
#define NVCFB0_SET_PICTURE_INDEX             0x0000040Cu
#define NVCFB0_SET_SLICE_OFFSETS_BUF_OFFSET  0x00000410u
#define NVCFB0_SET_COLOC_DATA_OFFSET         0x00000414u
#define NVCFB0_SET_HISTORY_OFFSET            0x00000418u
#define NVCFB0_SET_NVDEC_STATUS_OFFSET       0x00000424u
#define NVCFB0_SET_PICTURE_LUMA_OFFSET0      0x00000430u
#define NVCFB0_SET_PICTURE_CHROMA_OFFSET0    0x00000474u
#define NVCFB0_H264_SET_MBHIST_BUF_OFFSET    0x00000500u

/* SET_CONTROL_PARAMS bitfields (clc5b0.h SET_CONTROL_PARAMS_*, and h264.rs:717):
 * CODEC_TYPE[3:0]=H264(3), GPTIMER_ON[4], ERR_CONCEAL_ON[6], ERROR_FRM_IDX[12:7],
 * MBTIMER_ON[13]. For an I-frame ERROR_FRM_IDX=0. */
#define NVCFB0_H264_CONTROL_PARAMS \
    (3u | (1u<<4) | (1u<<6) | (1u<<13))

/* EXECUTE fields (clc5b0.h): NOTIFY_ON[1]=END(1) leaves default; we push the
 * documented "run to end, no notify/awaken" value 0 (h264.rs:769 Disable/End). */
#define NVCFB0_EXECUTE_VALUE                 0x00000000u

/* ---- picture-setup structs, VERBATIM from nvdec_drv.h ---- */
/* nvdec_drv.h::_nvdec_status_s. The HEVC/VP9 union occupies nine DWORDs;
 * H.264 does not consume those codec-specific statistics. Word zero is a
 * decoded macroblock COUNT, not an error code. */
typedef struct {
    unsigned int mbs_correctly_decoded, mbs_in_error, cycle_count, error_status;
    unsigned int codec_statistics[9];
    unsigned int slice_header_error_code;
} nvdec_frame_status_t;
_Static_assert(sizeof(nvdec_frame_status_t) == 56, "NVDEC status ABI");
_Static_assert(__builtin_offsetof(nvdec_frame_status_t, error_status) == 12, "NVDEC error offset");
_Static_assert(__builtin_offsetof(nvdec_frame_status_t, slice_header_error_code) == 52, "NVDEC slice error offset");

typedef struct _nvdec_pass2_otf_s {
    unsigned int   wrapped_session_key[4];
    unsigned int   wrapped_content_key[4];
    unsigned int   initialization_vector[4];
    unsigned int   enable_encryption : 1;
    unsigned int   key_increment     : 6;
    unsigned int   encryption_mode   : 4;
    unsigned int   key_slot_index    : 4;
    unsigned int   ssm_en            : 1;
    unsigned int   reserved1         :16;
} nvdec_pass2_otf_s; /* 0x10 bytes */

typedef struct _nvdec_pass2_otf_ext_s {
    unsigned int ssm_entry_num      :16;
    unsigned int ssm_iv_num         :16;
    unsigned int real_stream_length;
    unsigned int non_slice_data     :16;
    unsigned int drm_mode           : 7;
    unsigned int reserved           : 9;
} nvdec_pass2_otf_ext_s; /* 12 bytes */

typedef struct _nvdec_display_param_s {
    unsigned int enableTFOutput    : 1;
    unsigned int VC1MapYFlag       : 1;
    unsigned int MapYValue         : 3;
    unsigned int VC1MapUVFlag      : 1;
    unsigned int MapUVValue        : 3;
    unsigned int OutStride         : 8;
    unsigned int TilingFormat      : 3;
    unsigned int OutputStructure   : 1;
    unsigned int reserved0         :11;
    int OutputTop[2];
    int OutputBottom[2];
    unsigned int enableHistogram   : 1;
    unsigned int HistogramStartX   :12;
    unsigned int HistogramStartY   :12;
    unsigned int reserved1         : 7;
    unsigned int HistogramEndX     :12;
    unsigned int HistogramEndY     :12;
    unsigned int reserved2         : 8;
} nvdec_display_param_s;  /* 0x1c bytes */

typedef struct _nvdec_dpb_entry_s {  /* 16 bytes */
    unsigned int index          : 7;
    unsigned int col_idx        : 5;
    unsigned int state          : 2;
    unsigned int is_long_term   : 1;
    unsigned int not_existing   : 1;
    unsigned int is_field       : 1;
    unsigned int top_field_marking : 4;
    unsigned int bottom_field_marking : 4;
    unsigned int output_memory_layout : 1;
    unsigned int reserved       : 6;
    unsigned int FieldOrderCnt[2];
    int FrameIdx;
} nvdec_dpb_entry_s;

typedef struct _nvdec_h264_pic_s {
    nvdec_pass2_otf_s encryption_params;
    unsigned char eos[16];
    unsigned char explicitEOSPresentFlag;
    unsigned char hint_dump_en;
    unsigned char reserved0[2];
    unsigned int stream_len;
    unsigned int slice_count;
    unsigned int mbhist_buffer_size;
    unsigned int gptimer_timeout_value;
    int log2_max_pic_order_cnt_lsb_minus4;
    int delta_pic_order_always_zero_flag;
    int frame_mbs_only_flag;
    int PicWidthInMbs;
    int FrameHeightInMbs;
    unsigned int tileFormat                 : 2 ;
    unsigned int gob_height                 : 3 ;
    unsigned int reserverd_surface_format   : 27;
    int entropy_coding_mode_flag;
    int pic_order_present_flag;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
    int deblocking_filter_control_present_flag;
    int redundant_pic_cnt_present_flag;
    int transform_8x8_mode_flag;
    unsigned int pitch_luma;
    unsigned int pitch_chroma;
    unsigned int luma_top_offset;
    unsigned int luma_bot_offset;
    unsigned int luma_frame_offset;
    unsigned int chroma_top_offset;
    unsigned int chroma_bot_offset;
    unsigned int chroma_frame_offset;
    unsigned int HistBufferSize;
    unsigned int MbaffFrameFlag           : 1;
    unsigned int direct_8x8_inference_flag: 1;
    unsigned int weighted_pred_flag       : 1;
    unsigned int constrained_intra_pred_flag:1;
    unsigned int ref_pic_flag             : 1;
    unsigned int field_pic_flag           : 1;
    unsigned int bottom_field_flag        : 1;
    unsigned int second_field             : 1;
    unsigned int log2_max_frame_num_minus4: 4;
    unsigned int chroma_format_idc        : 2;
    unsigned int pic_order_cnt_type       : 2;
    int pic_init_qp_minus26               : 6;
    int chroma_qp_index_offset            : 5;
    int second_chroma_qp_index_offset     : 5;
    unsigned int weighted_bipred_idc      : 2;
    unsigned int CurrPicIdx               : 7;
    unsigned int CurrColIdx               : 5;
    unsigned int frame_num                : 16;
    unsigned int frame_surfaces           : 1;
    unsigned int output_memory_layout     : 1;
    int CurrFieldOrderCnt[2];
    nvdec_dpb_entry_s dpb[16];
    unsigned char WeightScale[6][4][4];
    unsigned char WeightScale8x8[2][8][8];
    unsigned char num_inter_view_refs_lX[2];
    char reserved1[14];
    signed char inter_view_refidx_lX[2][16];
    unsigned int lossless_ipred8x8_filter_enable        : 1;
    unsigned int qpprime_y_zero_transform_bypass_flag   : 1;
    unsigned int reserved2                              : 30;
    nvdec_display_param_s displayPara;
    nvdec_pass2_otf_ext_s ssm;
} nvdec_h264_pic_s;

/* Compile-enforce the layout so an accidental edit to the verbatim copy is
 * caught by the build.  NOTE: nvdec_pass2_otf_s is 52 bytes (3x u32[4] arrays +
 * a bitfield word); its Mesa "// 0x10 bytes" comment is wrong, but the field
 * declaration - which Mesa populates by name and the hardware reads - is what
 * the compiler lays out, so 52 is the real size.  eos therefore @52,
 * stream_len @72 (52 + eos 16 + flags/reserved 4). */
_Static_assert(sizeof(nvdec_pass2_otf_s) == 52, "pass2_otf_s 52 bytes (Mesa comment lies)");
_Static_assert(__builtin_offsetof(nvdec_h264_pic_s, eos) == 52, "eos @ 52");
_Static_assert(__builtin_offsetof(nvdec_h264_pic_s, stream_len) == 72, "stream_len @ 72");
_Static_assert(sizeof(nvdec_display_param_s) == 0x1c, "display_param_s 28 bytes");
_Static_assert(sizeof(nvdec_dpb_entry_s) == 16, "dpb entry 16 bytes");

#endif /* KESTREL_NVDEC_DRV_H264_H */
