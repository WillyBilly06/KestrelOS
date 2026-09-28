/* Frozen pre-parser 64x64 context, retained ONLY as a wire regression oracle.
 * Production generates picture context from the actual stream headers. */
/* Fill nvdec_h264_pic_s for our 64x64 baseline I_PCM IDR.  Values are the
 * SPS/PPS-derived fields the hardware needs (cuviddec.h/h264.rs semantics):
 * WeightScale flat 0x10, gptimer nonzero, no DPB refs, no encryption. */
static void nvdec_fill_pic(nvdec_h264_pic_s *pic, u32 stream_len,
                           u32 luma_pitch, u32 chroma_pitch,
                           u32 hist_size, u32 mbhist_size) {
    memset(pic, 0, sizeof *pic);
    static const unsigned char EOS[16] =
        {0,0,1,0xb, 0,0,0,0, 0,0,1,0xb, 0,0,0,0};   /* h264.rs EOS_ARRAY */
    for (int i = 0; i < 16; i++) pic->eos[i] = EOS[i];
    pic->explicitEOSPresentFlag = 1;
    pic->stream_len            = stream_len;         /* incl. the 16-byte EOS */
    pic->slice_count           = 1;
    pic->mbhist_buffer_size    = mbhist_size;
    pic->gptimer_timeout_value = 81000000u;          /* current Mesa/NVK value */
    pic->log2_max_pic_order_cnt_lsb_minus4 = 0;
    pic->frame_mbs_only_flag   = 1;
    pic->PicWidthInMbs         = H264_TEST_WIDTH_MBS;
    pic->FrameHeightInMbs      = H264_TEST_HEIGHT_MBS;
    /* Current Mesa/NVK forces NVDEC video images to GOB_2 block-linear on
     * every size.  Blackwell does not accept the old 16x16 pitch-linear test. */
    pic->tileFormat            = 1;
    pic->gob_height            = 0;
    pic->entropy_coding_mode_flag = 0;               /* CAVLC */
    pic->pitch_luma            = luma_pitch;
    pic->pitch_chroma          = chroma_pitch;
    pic->luma_bot_offset       = H264_TEST_WIDTH_MBS * 16u;
    pic->chroma_bot_offset     = chroma_pitch / 2u;
    pic->HistBufferSize        = hist_size >> 8;
    pic->ref_pic_flag          = 1;                  /* nal_ref_idc != 0 (IDR) */
    pic->log2_max_frame_num_minus4 = 0;
    pic->chroma_format_idc     = 1;                  /* 4:2:0 */
    pic->pic_order_cnt_type    = 0;
    pic->CurrPicIdx            = 0;
    pic->CurrColIdx            = 0;
    pic->frame_num             = 0;
    pic->output_memory_layout  = 0;                  /* NV12 */
    for (int a = 0; a < 6; a++) for (int b = 0; b < 4; b++) for (int d = 0; d < 4; d++)
        pic->WeightScale[a][b][d] = 0x10;            /* flat - never leave zero */
    for (int a = 0; a < 2; a++) for (int b = 0; b < 8; b++) for (int d = 0; d < 8; d++)
        pic->WeightScale8x8[a][b][d] = 0x10;
}
