#!/usr/bin/env python3
# gen_h264_iframe.py - emit a minimal, VALID H.264 Annex-B single IDR frame that
# uses an I_PCM macroblock, and a matching C header for the NVDEC decode test.
#
# Why I_PCM: without a real encoder on this host (no ffmpeg), an I_PCM frame is
# the one H.264 picture that can be built by hand and trusted, because I_PCM
# carries the pixel samples RAW - no transform, no quantisation, no CAVLC
# residual coding.  Only the SPS/PPS and the slice header + mb_type need
# bit-packing (Exp-Golomb), which this script does explicitly and then PARSES
# BACK to prove the structure round-trips.  The pixels are a solid mid-grey
# (Y=U=V=0x80), so a correct decode yields an all-0x80 NV12 surface - a clean
# sanity check.
#
# Blackwell's documented H.264 NVDEC minimum is 48x64.  Use 64x64 (4x4 luma
# macroblocks), which is safely above that minimum and exactly fills four
# 64-byte-wide GOB_2 blocks without partial-row padding.  Every macroblock is
# I_PCM and carries 16x16 Y plus 8x8 U/V.
# Provenance: hand-built per ITU-T H.264 (2021) 7.3.2.1/7.3.2.2/7.3.3/7.3.4.
# Structurally validated here; ACCEPTANCE BY NVDEC IS BOOT-GATED.

import sys

WIDTH_MBS = 4
HEIGHT_MBS = 4
WIDTH = WIDTH_MBS * 16
HEIGHT = HEIGHT_MBS * 16

class BW:
    def __init__(s): s.bits=[]
    def u(s,n,v):
        for i in range(n-1,-1,-1): s.bits.append((v>>i)&1)
    def ue(s,v):
        # Exp-Golomb unsigned
        v1=v+1; n=v1.bit_length()
        s.u(n-1,0); s.u(n,v1)
    def se(s,v):
        # signed Exp-Golomb: 0->0, 1->1, -1->2, 2->3, ...
        s.ue(0 if v==0 else (2*v-1 if v>0 else -2*v))
    def align(s, bit=0):
        while len(s.bits)%8: s.bits.append(bit)
    def bytes(s):
        s.align(0)
        out=bytearray()
        for i in range(0,len(s.bits),8):
            b=0
            for k in range(8): b=(b<<1)|s.bits[i+k]
            out.append(b)
        return bytes(out)

class BR:
    def __init__(s, data): s.data=data; s.bit=0
    def u(s,n):
        v=0
        for _ in range(n):
            assert s.bit < len(s.data)*8, "unexpected end of RBSP"
            v=(v<<1)|((s.data[s.bit//8]>>(7-(s.bit%8)))&1); s.bit+=1
        return v
    def ue(s):
        z=0
        while s.u(1)==0: z+=1
        return (1<<z)-1 + (s.u(z) if z else 0)
    def se(s):
        code=s.ue()
        return (code+1)//2 if code&1 else -(code//2)
    def align_zero(s):
        while s.bit%8: assert s.u(1)==0

def rbsp_from_ebsp(data):
    out=bytearray(); zeros=0; i=0
    while i<len(data):
        b=data[i]
        if zeros>=2 and b==3:
            assert i+1<len(data) and data[i+1]<=3
            zeros=0; i+=1; continue
        out.append(b)
        zeros=zeros+1 if b==0 else 0
        i+=1
    return bytes(out)

def ebsp(rbsp):
    # emulation-prevention: insert 0x03 after any 00 00 0x (x<=3)
    out=bytearray(); z=0
    for byte in rbsp:
        if z>=2 and byte<=3:
            out.append(3); z=0
        out.append(byte)
        z = z+1 if byte==0 else 0
    return bytes(out)

def nal(nal_ref_idc, nal_unit_type, rbsp):
    hdr = (0<<7)|(nal_ref_idc<<5)|nal_unit_type
    return b'\x00\x00\x00\x01' + bytes([hdr]) + ebsp(rbsp)

def sps():
    b=BW()
    b.u(8,66)      # profile_idc = 66 (Baseline)
    b.u(1,1); b.u(1,1); b.u(1,0)  # constraint_set0,1,2
    b.u(1,0); b.u(1,0); b.u(1,0)  # set3,4,5
    b.u(2,0)       # reserved_zero_2bits
    b.u(8,11)      # level_idc = 1.1 (enough for 48x64)
    b.ue(0)        # seq_parameter_set_id
    b.ue(0)        # log2_max_frame_num_minus4 -> 4 bits
    b.ue(0)        # pic_order_cnt_type = 0
    b.ue(0)        # log2_max_pic_order_cnt_lsb_minus4 -> 4 bits
    b.ue(1)        # max_num_ref_frames
    b.u(1,0)       # gaps_in_frame_num_value_allowed_flag
    b.ue(WIDTH_MBS - 1)
    b.ue(HEIGHT_MBS - 1)
    b.u(1,1)       # frame_mbs_only_flag
    b.u(1,0)       # direct_8x8_inference_flag
    b.u(1,0)       # frame_cropping_flag
    b.u(1,0)       # vui_parameters_present_flag
    b.u(1,1); b.align(0)  # rbsp_trailing_bits (stop bit + align)
    return b.bytes()

def pps():
    b=BW()
    b.ue(0)        # pic_parameter_set_id
    b.ue(0)        # seq_parameter_set_id
    b.u(1,0)       # entropy_coding_mode_flag = 0 (CAVLC)
    b.u(1,0)       # bottom_field_pic_order_in_frame_present_flag
    b.ue(0)        # num_slice_groups_minus1
    b.ue(0)        # num_ref_idx_l0_default_active_minus1
    b.ue(0)        # num_ref_idx_l1_default_active_minus1
    b.u(1,0)       # weighted_pred_flag
    b.u(2,0)       # weighted_bipred_idc
    b.se(0)        # pic_init_qp_minus26
    b.se(0)        # pic_init_qs_minus26
    b.se(0)        # chroma_qp_index_offset
    b.u(1,0)       # deblocking_filter_control_present_flag
    b.u(1,0)       # constrained_intra_pred_flag
    b.u(1,0)       # redundant_pic_cnt_present_flag
    b.u(1,1); b.align(0)  # rbsp_trailing_bits
    return b.bytes()

def idr_slice():
    b=BW()
    # slice_header
    b.ue(0)        # first_mb_in_slice
    b.ue(7)        # slice_type = 7 (I, all slices I)
    b.ue(0)        # pic_parameter_set_id
    b.u(4,0)       # frame_num (log2_max_frame_num = 4)
    b.ue(0)        # idr_pic_id (IDR)
    b.u(4,0)       # pic_order_cnt_lsb (log2_max_poc_lsb = 4)
    # dec_ref_pic_marking (IDR)
    b.u(1,0)       # no_output_of_prior_pics_flag
    b.u(1,0)       # long_term_reference_flag
    b.se(0)        # slice_qp_delta
    # slice_data: one I_PCM record for every macroblock in raster order.
    for _mb in range(WIDTH_MBS * HEIGHT_MBS):
        b.ue(25)       # mb_type = 25 = I_PCM (in an I slice)
        b.align(0)     # pcm_alignment_zero_bits -> byte align
        for _ in range(256): b.u(8,0x80)   # luma 16x16
        for _ in range(64):  b.u(8,0x80)   # Cb 8x8
        for _ in range(64):  b.u(8,0x80)   # Cr 8x8
    # after last mb: rbsp_slice_trailing_bits
    b.u(1,1); b.align(0)
    return b.bytes()

def parse_validate(annexb):
    # Independent bit-level round-trip.  This does more than recognize NAL
    # headers: it removes emulation-prevention bytes, parses every SPS/PPS/slice
    # field emitted above, then verifies all 16 I_PCM macroblocks and all 6144
    # raw samples.  A malformed Exp-Golomb code or byte-alignment error fails the
    # image build instead of being discovered by a reboot.
    starts=[]; i=0
    while i<=len(annexb)-4:
        if annexb[i:i+4]==b'\x00\x00\x00\x01': starts.append(i); i+=4
        else: i+=1
    starts.append(len(annexb))
    units=[annexb[starts[n]+4:starts[n+1]] for n in range(len(starts)-1)]
    types=[u[0]&0x1f for u in units]
    assert types==[7,8,5], f"NAL order {types} != [7,8,5]"

    r=BR(rbsp_from_ebsp(units[0][1:]))
    assert r.u(8)==66 and r.u(6)==0x30 and r.u(2)==0 and r.u(8)==11
    assert [r.ue() for _ in range(4)]==[0,0,0,0]
    assert r.ue()==1 and r.u(1)==0
    assert r.ue()==WIDTH_MBS-1 and r.ue()==HEIGHT_MBS-1
    assert r.u(1)==1 and r.u(1)==0 and r.u(1)==0 and r.u(1)==0
    assert r.u(1)==1; r.align_zero()

    r=BR(rbsp_from_ebsp(units[1][1:]))
    assert r.ue()==0 and r.ue()==0
    assert r.u(1)==0 and r.u(1)==0
    assert r.ue()==0 and r.ue()==0 and r.ue()==0
    assert r.u(1)==0 and r.u(2)==0
    assert r.se()==0 and r.se()==0 and r.se()==0
    assert r.u(1)==0 and r.u(1)==0 and r.u(1)==0
    assert r.u(1)==1; r.align_zero()

    r=BR(rbsp_from_ebsp(units[2][1:]))
    assert r.ue()==0 and r.ue()==7 and r.ue()==0
    assert r.u(4)==0 and r.ue()==0 and r.u(4)==0
    assert r.u(1)==0 and r.u(1)==0 and r.se()==0
    for mb in range(WIDTH_MBS*HEIGHT_MBS):
        assert r.ue()==25, f"macroblock {mb}: not I_PCM"
        r.align_zero()
        for sample in range(384):
            assert r.u(8)==0x80, f"macroblock {mb} sample {sample}: not 0x80"
    assert r.u(1)==1
    while r.bit<len(r.data)*8: assert r.u(1)==0
    return types

def main():
    sps_n = nal(3,7,sps())
    pps_n = nal(3,8,pps())
    idr_n = nal(3,5,idr_slice())
    stream = sps_n+pps_n+idr_n
    slice_off = len(sps_n)+len(pps_n)   # byte offset of the IDR slice NAL
    parse_validate(stream)
    # NVDEC wants the bitstream fed with a 16-byte EOS marker appended
    # (explicitEOSPresentFlag + eos[16]); the eos bytes live in the pic struct,
    # here we only note stream_len = len(stream)+16 for the caller.
    out = sys.argv[1] if len(sys.argv)>1 else "h264_iframe_test.h"
    with open(out,"w") as f:
        f.write("/* GENERATED by tools/gen_h264_iframe.py - do not edit.\n")
        f.write(f" * A hand-built, structurally-validated {WIDTH}x{HEIGHT} I_PCM IDR (solid 0x80).\n")
        f.write(" * Decode acceptance by NVDEC is BOOT-GATED. */\n")
        f.write("#ifndef H264_IFRAME_TEST_H\n#define H264_IFRAME_TEST_H\n")
        f.write(f"#define H264_TEST_WIDTH {WIDTH}\n#define H264_TEST_HEIGHT {HEIGHT}\n")
        f.write(f"#define H264_TEST_WIDTH_MBS {WIDTH_MBS}\n#define H264_TEST_HEIGHT_MBS {HEIGHT_MBS}\n")
        f.write(f"#define H264_TEST_LEN {len(stream)}\n")
        f.write(f"#define H264_TEST_SLICE_OFFSET {slice_off}\n")
        f.write("static const unsigned char h264_iframe_test[] = {\n")
        for i in range(0,len(stream),12):
            f.write("  "+",".join("0x%02x"%x for x in stream[i:i+12])+",\n")
        f.write("};\n#endif\n")
    print(f"OK: {len(stream)} bytes, NAL order [7,8,5] validated -> {out}")

if __name__=="__main__": main()
