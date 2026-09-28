/* Checked CFB7 H.264 work-buffer requirements, independent of GPU allocation.
 * Computes progressive 8-bit frame sizes and conservative history backing.
 * This does not
 * advertise support for a dimension or construct a GPU address. See
 * docs/nvenc-buffer-sizing-20260913.md for the 595 binary allocation trace.
 */
#ifndef KESTREL_NVENC_H264_BUFFERS_H
#define KESTREL_NVENC_H264_BUFFERS_H
#include "nvenc_drv_h264.h"

#define NVENC_CFB7_FULL_FRAME_WORK_SLOTS 16u
/* Linux ELF 0xe2489..0xe2499: capacity*512 + 3072, then 0xca55c
 * allocates two alternating objects. An independent-IDR submission uses one
 * initialized object; two state records at offsets 0 and 256 are distinct
 * from those two alternating objects. */
#define NVENC_CFB7_RC_PROCESS_BYTES (NVENC_CFB7_FULL_FRAME_WORK_SLOTS*512u+3072u)

typedef struct {
    unsigned int width, height, width_mbs, height_mbs, macroblocks;
    unsigned int pitch, luma_rows, chroma_rows, luma_bytes, chroma_bytes;
    unsigned int history_bytes, coloc_bytes;
    unsigned int history_allocation_bytes;
    unsigned int slice_stat_offset, mpec_stat_offset, fifo_stat_offset, aq_stat_offset;
    unsigned int status_bytes;
    unsigned int rc_stat_bytes, rc_act_offset;
} nvenc_h264_buffer_requirements_s;

/* No caller-supplied cached sizes are trusted. Failure zeros the whole result.
 * Arithmetic uses wide intermediates; dimensions are checked before computing
 * products. Coded dimensions are macroblock aligned; cropping is separate work.
 * No formulas from legacy history or unpadded macroblock coloc arrays are used.
 */
static inline int nvenc_h264_buffer_requirements(
    nvenc_h264_buffer_requirements_s *out,unsigned int width,unsigned int height) {
    if (!out) return 0;
    *out=(nvenc_h264_buffer_requirements_s){0};
    if (width<48u || height<64u || width>4096u || height>4096u ||
        ((width|height)&15u)) return 0;
    unsigned long long mbw=width/16u,mbh=height/16u,mbs=mbw*mbh;
    unsigned long long pitch=((unsigned long long)width+63u)&~63ull;
    unsigned long long yrows=height,uvrows=((unsigned long long)height/2u+15u)&~15ull;
    unsigned long long ybytes=pitch*yrows,uvbytes=pitch*uvrows;
    /* ELF 0xe21a0 builds allocation descriptor+0x1c with this padded row
     * count, 128-byte pair stride and 512-byte alignment. ELF 0xca260 saves
     * it as object+0x418; 0xdeba0 uses it as the per-frame coloc slot stride. */
    unsigned long long coloc_rows=(((mbh+1u)>>1)+1u)|1u;
    unsigned long long coloc=(coloc_rows*mbw*128u+0x1feu)&~0x1ffull;
    /* CFB7 picture producer ELF 0xd0d90 writes the logical slot size.
     * The normal full-frame allocation path is separate: ELF 0xd5f50 sets
     * session+0xa7d8 to 16; 0xe21a0 multiplies the slot by that capacity;
     * 0xca260 allocates descriptor+0x14 as the history object. Preserve that
     * conservative pool even for one submitted frame. This is allocation
     * capacity, NOT an engine count or the value of pic_control.hist_buf_size.
     * It does not establish how many slots firmware touches on this GPU. */
    unsigned long long history=(mbw*384u+255u)&~255ull;
    unsigned long long history_allocation=history*NVENC_CFB7_FULL_FRAME_WORK_SLOTS;
    /* Status subarrays use the public record sizes. Reserve one slice entry
     * per MB even though current submission has only one slice. Each distinct
     * array starts on a 256-byte boundary, and cannot alias its predecessor. */
    unsigned long long slice=(sizeof(nvenc_pic_stat_s)+255u)&~255ull;
    unsigned long long mpec=(slice+mbs*sizeof(nvenc_slice_stat_s)+255u)&~255ull;
    unsigned long long fifo=(mpec+mbs*sizeof(nvenc_mpec_stat_s)+255u)&~255ull;
    unsigned long long aq=(fifo+mbs*sizeof(nvenc_stats_fifo_s)+255u)&~255ull;
    unsigned long long status=(aq+mbs*sizeof(nvenc_aq_stat_s)+255u)&~255ull;
    /* Linux ELF 0xcbcf0: RC picture header, eight-row-padded 16-byte row
     * records, then a 256-byte-per-row activity/QP region. The 0x70c binding
     * is separate from 0x718 status; first-pass firmware writes its stats. */
    unsigned long long rc_rows=((((mbh+7u)&~7ull)*16u)+255u)&~255ull;
    unsigned long long rc_act=256u+rc_rows,rc_bytes=rc_act+mbh*256u;
    if (ybytes>0xffffffffull || uvbytes>0xffffffffull || history>0xffffffffull ||
        history_allocation>0xffffffffull || coloc>0xffffffffull ||
        status>0xffffffffull || rc_bytes>0xffffffffull) return 0;
    *out=(nvenc_h264_buffer_requirements_s){
        .width=width,.height=height,.width_mbs=(unsigned int)mbw,
        .height_mbs=(unsigned int)mbh,.macroblocks=(unsigned int)mbs,
        .pitch=(unsigned int)pitch,.luma_rows=(unsigned int)yrows,
        .chroma_rows=(unsigned int)uvrows,.luma_bytes=(unsigned int)ybytes,
        .chroma_bytes=(unsigned int)uvbytes,.history_bytes=(unsigned int)history,
        .history_allocation_bytes=(unsigned int)history_allocation,
        .coloc_bytes=(unsigned int)coloc,.slice_stat_offset=(unsigned int)slice,
        .mpec_stat_offset=(unsigned int)mpec,.fifo_stat_offset=(unsigned int)fifo,
        .aq_stat_offset=(unsigned int)aq,.status_bytes=(unsigned int)status,
        .rc_stat_bytes=(unsigned int)rc_bytes,.rc_act_offset=(unsigned int)rc_act
    };
    return 1;
}

/* Linux ELF 0xc8de0 initializes the public 256-byte persistent-state record.
 * CQP independent-IDR policy has no CPB fullness, previous P/B pictures or
 * bitrate budget. Those fields stay zero, not a captured Windows session's
 * fullness. The complexity defaults and prevQS/prevQP are unconditional
 * initializer constants, even when the requested CQP differs from 26.
 * This supplies the ordinary reference driver's 0x724 resource; it does not
 * imply firmware needs picture-level RC when rc_mode is zero. */
static inline int nvenc_h264_cqp_idr_work_init(void *buffer,unsigned int bytes,
                                             const nvenc_h264_rc_s *rc) {
    if (!buffer || bytes<NVENC_CFB7_RC_PROCESS_BYTES || !rc) return 0;
    /* Do not silently reuse this limited initializer for inter-frame/VBR/CBR.
     * Validate before touching caller memory; the caller must abort on zero. */
    if (rc->hrd_type || rc->nal_cpb_size || rc->vcl_cpb_size ||
        rc->nal_bitrate || rc->vcl_bitrate || rc->gop_length!=1 || rc->Np ||
        rc->buffersize || rc->Bmin || rc->Ravg || rc->R)
        return 0;
    unsigned char *dst=(unsigned char *)buffer;
    for (unsigned int i=0;i<NVENC_CFB7_RC_PROCESS_BYTES;i++) dst[i]=0;
    nvenc_persistent_state_s state={0};
    _Static_assert(sizeof(state)==256,"NVENC persistent state ABI");
    const int complexity[3]={24,12,48};
    for (unsigned int i=0;i<3;i++) {
        state.Wpbi[i]=complexity[i];
        for (unsigned int j=0;j<3;j++) state.Whist[i][j]=complexity[i];
    }
    state.prevQS=256;
    state.prevQP=26;
    const unsigned char *src=(const unsigned char *)&state;
    for (unsigned int i=0;i<sizeof state;i++) {
        dst[i]=src[i];
        dst[256u+i]=src[i];
    }
    return 1;
}

#endif
