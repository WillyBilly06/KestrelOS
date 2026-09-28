/* H.264 quantization defaults from the supplied NVIDIA 595.99.02 driver.
 * This is a typed policy, not an opaque captured firmware record or bitstream.
 * See docs/nvenc-quantization-20260913.md for the producer/data provenance.
 */
#ifndef KESTREL_NVENC_H264_POLICY_H
#define KESTREL_NVENC_H264_POLICY_H
#include "nvenc_drv_h264.h"

static inline void nvenc_h264_quant_defaults(nvenc_h264_quant_control_s *q) {
    /* ELF 0xcca20 initializes object+0x7f4; ELF 0xd0470 copies that 192-byte
     * record to the picture's quant-control buffer. CFB7 feature 0x40000 takes
     * the newer cost-15/QPP-off branch. Saturation and reserved fields are zero.
     * The run vector's words are unpacked from immediate 0x000fffaaaaff056b. */
    *q=(nvenc_h264_quant_control_s){0};
    q->qpp_run_vector_4x4=0x056b;
    q->qpp_run_vector_8x8[0]=0xaaff;
    q->qpp_run_vector_8x8[1]=0xffaa;
    q->qpp_run_vector_8x8[2]=0x000f;
    q->qpp_luma8x8_cost=q->qpp_luma16x16_cost=q->qpp_chroma_cost=15;
    q->qpp_mode=0;
    /* ELF read-only data 0x100a0e0 and 0x100a100. Driver stores these in
     * natural table order, not zig-zag order; do not reorder the coefficients. */
    static const unsigned short intra4[16]={
        1023,878,820,682, 878,820,682,512,
        820,682,512,410, 682,512,410,410
    };
    static const unsigned short inter4[16]={
        682,586,546,456, 586,546,456,342,
        546,456,342,292, 456,342,292,274
    };
    for (unsigned int i=0;i<16;i++) {
        q->dz_4x4_YI[i]=intra4[i];
        q->dz_4x4_YP[i]=inter4[i];
        /* ELF data 0x1009fe0 and 0x100a060 are uniform 16-entry arrays. */
        q->dz_8x8_YI[i]=682;
        q->dz_8x8_YP[i]=342;
    }
    q->dz_4x4_CI=682;
    q->dz_4x4_CP=342;
}

#endif
