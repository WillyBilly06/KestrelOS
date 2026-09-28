/* Blackwell NVENC H.264 firmware ABI.
 *
 * The published NV_NVENC_8_2 record is C9B7, NOT the complete Blackwell ABI.
 * Reuse its common prefix and auxiliary records, but declare the CFB7 tail
 * separately. Supplied 595.99.02 libnvcuvid evidence (ELF virtual addresses):
 *   0xd0d90: producer, picture base this+0x66fc; timer this+0x68fc.
 *   0xddd70: CFB7 path copies 0x300 bytes directly into the upload buffer.
 *   0x11e200: legacy converter copies [4,0x1dc) unchanged, moves the 32-byte
 *             surface 0x1e0 -> 0x1dc and timer 0x200 -> 0x1fc.
 * Offset 0x208 describes each plane's image layout; local VRAM does NOT mean
 * that these selectors are zero. Other unknown extension fields stay zero;
 * in particular the producer's 0x0f at offset 0x204 is gated on flag 0x80000
 * (D1B7 magic), NOT CFB7's 0x40000. See the instruction-backed ABI test.
 * this declaration does not claim support for all optional encoding features.
 */
#ifndef KESTREL_NVENC_DRV_H264_H
#define KESTREL_NVENC_DRV_H264_H
#include "nvenc_h264_output.h"

#define NV_NVENC_8_2 1
#include "../refs/open-gpu-doc/classes/video/nvenc_drv.h"

#define NVENC_CFB7_DRV_MAGIC 0xCFB70006u
/* The published member name error_status is misleading for this backend:
 * Linux 595 ELF 0xbbd30 reads status+4 bits[1:0] and consumes the completed
 * picture's total_bit_count only for value 2 (0xbc314, 0xbbf67, 0xbc530).
 * Windows 610.62 independently does the same at 0x1801036b0/0x180103722.
 * Require this state AFTER the completion fence, plus clean upper status bits
 * and bounded picture/bitstream validation. Zero is not completed output.
 * See docs/nvenc-completion-state-20260917.md. */
#define NVENC_CFB7_H264_STATUS_COMPLETE 2u
/* Firmware picture byte 0x1ae is a DIFFERENT selector from method 0x700.
 * The 595 H.264 producer zeroes this field (this+0x68aa); successful Windows
 * CFB7 hardware capture independently confirms zero for H.264. Method 0x700
 * still uses codec 3. Never copy its numeric selector into the picture. */
#define NVENC_CFB7_PICTURE_CODEC_H264 0u
/* The public SDK's single-pass CQP setting is not firmware value zero here.
 * Linux 595 ELF 0xccd2f initializes encoder+0xa78 to 1; the H264 producer
 * copies it to encoder+0x67b6, picture+0xba. The completed Windows 610.62 CQP
 * capture independently has 1. This first-pass path writes RC statistics;
 * bind the separately sized 0x70c buffer whenever selecting it. */
#define NVENC_CFB7_H264_FIRST_PASS 1u
/* 595 ELF 0xec770 fills unused DPB entries with -2; the H.264 producer at
 * 0xd13a0/0xd13c0 copies their low bytes into pic_control.l0/l1. Zero is a
 * reference to slot zero, not an absent reference. */
#define NVENC_CFB7_H264_NO_REF 0xfeu
/* 595 H.264 producer writes 2 to reference descriptor byte 28 (ELF 0xd0f96)
 * and copies that descriptor to reconstructed output (0xd10df..0xd1115).
 * Input block-linear byte 28 is 1 << allocation block-height exponent
 * (0xd1095..0xd10a2). Select matching two-GOB blocks for our 8-bit NV12 test.
 * The old published "must be tiled_16x16 for refpics" comment is not CFB7. */
#define NVENC_CFB7_TEST_BLOCK_HEIGHT 2u
/* 595 H.264 producer ELF 0xd0d90 copies surface getter vtable+0x98 into
 * picture+0x208: two identical reference/output Y/UV pairs followed by input
 * Y/UV, two bits each. Allocator
 * ELF 0x2c620 and imported-array wrapper 0x2aa70 set that getter's object+0x88
 * byte from bytes per array element: 1 => 1, 2 => 2, otherwise 0 (YUY2 => 0).
 * This is the Blackwell GOB byte-order selector, NOT the memory aperture.
 * Our kind-6 NV12 surfaces use separate R8 Y and R8G8 UV plane views, matching
 * nv_video_layout.h. The successful owned CFB7 capture has 0x999 at 0x208.
 * Do not apply these selectors to a legacy/linear or packed-YUY2 surface. */
#define NVENC_CFB7_GOB_R8 1u
#define NVENC_CFB7_GOB_RG8 2u
#define NVENC_CFB7_GOB_NV12_PAIR (NVENC_CFB7_GOB_R8 | (NVENC_CFB7_GOB_RG8 << 2))
#define NVENC_CFB7_GOB_NV12_LAYOUTS (NVENC_CFB7_GOB_NV12_PAIR | \
    (NVENC_CFB7_GOB_NV12_PAIR << 4) | (NVENC_CFB7_GOB_NV12_PAIR << 8))

typedef struct {
    unsigned int magic;
    nvenc_h264_surface_cfg_s refpic_cfg;
    nvenc_h264_surface_cfg_s input_cfg;
    nvenc_h264_surface_cfg_s outputpic_cfg;
    nvenc_h264_sps_data_s sps_data;
    nvenc_h264_pps_data_s pps_data;
    nvenc_h264_rc_s rate_control;
    nvenc_h264_pic_control_s pic_control;
    unsigned int extended_pic_control;          /* 0x1dc, zero for this test */
    nvenc_h264_surface_cfg_s half_scaled_outputpic_cfg; /* 0x1e0 */
    unsigned int gpTimer_timeout_val;           /* 0x200, bit 31 selects newer scale */
    unsigned int reserved_204;                 /* D1B7-only control, zero on CFB7 */
    unsigned int plane_layouts;                /* 0x208, six two-bit GOB selectors */
    unsigned char extension[0xf4];             /* 0x20c..0x2ff */
} nvenc_cfb7_h264_drv_pic_setup_s;

/* 595 FUN_001c9390 (ELF 0xc9390), newer-scale branch. Use wide intermediates
 * so larger future dimensions cannot wrap before applying the driver's clamp.
 * The producer sets bit 31 for CFB7 before inserting the lower 31-bit value. */
static inline unsigned int nvenc_cfb7_h264_timer(unsigned int width,
                                                unsigned int height) {
    unsigned long long ticks = (((unsigned long long)width * height) >> 10) * 225u;
    if (ticks < 0x31704u) ticks = 0x31704u;
    if (ticks > 0x165a0bu) ticks = 0x165a0bu;
    return 0x80000000u | (unsigned int)ticks;
}

/* Independent-IDR CQP policy, not a VBR/CBR initializer. Linux 595 sequence
 * producer ELF 0xdc8cf..0xdc90a initializes all three CQP ratios to 0x100
 * (1.0 in 23.8), not zero. Its constructor defaults max/base QP delta to 6/3.
 * iSizeRatioX/Y is explicitly 1/1 for this equal-QP, independent-IDR policy;
 * a zero denominator is not a disabled ratio. The owned Windows CQP capture
 * confirms these values. HRD, bitrate, AQ and external hints stay disabled.
 * This reproduces configuration, not a claim of firmware completion. */
static inline int nvenc_cfb7_h264_cqp_idr_defaults(nvenc_h264_rc_s *rc,
                                                  unsigned int qp,
                                                  unsigned int fps) {
    if (!rc) return 0;
    *rc = (nvenc_h264_rc_s){0};
    if (qp > 51u || !fps || fps > 0x7fffffu) return 0;
    for (unsigned int i=0;i<3;i++) {
        rc->QP[i] = (unsigned char)qp;
        rc->maxQP[i] = 51;
        rc->rhopbi[i] = 256;
    }
    rc->maxQPD = 6;
    rc->baseQPD = 3;
    rc->framerate = (int)(fps * 256u);
    rc->gop_length = 1;
    rc->two_pass_rc = NVENC_CFB7_H264_FIRST_PASS;
    rc->iSizeRatioX = rc->iSizeRatioY = 1;
    return 1;
}

_Static_assert(sizeof(nvenc_h264_surface_cfg_s) == 32,
               "NVENC H.264 surface descriptor ABI");
_Static_assert(sizeof(nvenc_h264_slice_control_s) == 128,
               "NVENC H.264 slice-control ABI");
_Static_assert(sizeof(nvenc_h264_me_control_s) == 192,
               "NVENC H.264 ME-control ABI");
_Static_assert(sizeof(nvenc_h264_md_control_s) == 128,
               "NVENC H.264 mode-decision ABI");
_Static_assert(sizeof(nvenc_h264_quant_control_s) == 192,
               "NVENC H.264 quant-control ABI");
_Static_assert(sizeof(nvenc_pic_stat_s) == 128,
               "NVENC H.264 status ABI");
_Static_assert(sizeof(nvenc_rc_pic_stat_s) == 256 && sizeof(nvenc_mbrow_stat_s) == 16,
               "NVENC RC picture and row statistics ABI");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  rate_control.two_pass_rc) == 0xba,
               "CFB7 firmware pass from encoder+0x67b6 minus picture+0x66fc");
_Static_assert(sizeof(nvenc_h264_drv_pic_setup_s) == 512,
                "published C9B7 picture ABI, not the CFB7 upload format");
_Static_assert(__builtin_offsetof(nvenc_h264_drv_pic_setup_s,
                                  gpTimer_timeout_val) == 508,
                "published C9B7 timer offset ABI");
_Static_assert(sizeof(nvenc_cfb7_h264_drv_pic_setup_s) == 0x300,
               "CFB7 picture upload size from 595 driver");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  pic_control.hist_buf_size) == 0x19c,
               "CFB7 history extent from 595 object+0x6898");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  pic_control.stripID) == 0x1c2,
               "CFB7 strip ID from 595 object+0x68be");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  pic_control.strips_in_frame) == 0x1c3,
               "CFB7 legacy strip-count byte remains zero in full-frame mode");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  pic_control.slice_encoding_row_start) == 0x1c4,
               "CFB7 strip row start from 595 object+0x68c0");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  pic_control.slice_encoding_row_num) == 0x1c6,
               "CFB7 strip row count from 595 object+0x68c2");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  extended_pic_control) == 0x1dc,
               "CFB7 common prefix boundary");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  half_scaled_outputpic_cfg) == 0x1e0,
               "CFB7 half-scaled surface offset");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  gpTimer_timeout_val) == 0x200,
               "CFB7 timer offset from 595 legacy converter");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  reserved_204) == 0x204,
               "CFB7 reserved/D1B7-only control offset");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  plane_layouts) == 0x208,
               "CFB7 Blackwell image plane layout selectors");
_Static_assert(NVENC_CFB7_GOB_NV12_LAYOUTS == 0x999u,
               "CFB7 independent R8/RG8 planes for all three surfaces");
_Static_assert(__builtin_offsetof(nvenc_cfb7_h264_drv_pic_setup_s,
                                  extension) == 0x20c,
               "CFB7 extended tail offset");

#endif
