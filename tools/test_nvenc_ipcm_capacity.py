#!/usr/bin/env python3
"""Independent encoded-size proof using the existing bit-level I_PCM generator.

No fixtures are written or changed. This validates capacity, not NVENC execution.
"""
from pathlib import Path
import re
import gen_h264_iframe as h264
from test_gpu_stable_candidate import function

def main():
    root=Path(__file__).resolve().parents[1]
    src=(root/'kernel/nv_chan.c').read_text()
    # Boot and applications now share the encoder body. Inspect that actual
    # implementation rather than the thin boot wrapper located after it.
    assert 'return nvenc_encode_frame_hw(NULL,0,NULL);' in function(src,'nv_nvenc_selftest_hw')
    test=function(src,'nvenc_encode_frame_hw')
    dimensions=(root/'kernel/nvenc_h264_output.h').read_text()
    # The encoder and round-trip decoder now share these dimension constants.
    # Resolve their actual definition, not obsolete numeric assignments in C.
    assert re.search(r'ENC_WIDTH\s*=\s*NVENC_H264_TEST_WIDTH\b',test)
    assert re.search(r'ENC_HEIGHT\s*=\s*NVENC_H264_TEST_HEIGHT\b',test)
    width=int(re.search(r'#define\s+NVENC_H264_TEST_WIDTH\s+(\d+)u?\b',dimensions)[1])
    height=int(re.search(r'#define\s+NVENC_H264_TEST_HEIGHT\s+(\d+)u?\b',dimensions)[1])
    capacity=int(re.search(r'BITSTREAM_CAPACITY=(0x[0-9a-f]+)',test)[1],16)
    assert width%16==height%16==0
    saved=(h264.WIDTH_MBS,h264.HEIGHT_MBS,h264.WIDTH,h264.HEIGHT)
    try:
        h264.WIDTH_MBS,h264.HEIGHT_MBS=width//16,height//16
        h264.WIDTH,h264.HEIGHT=width,height
        stream=h264.nal(3,7,h264.sps())+h264.nal(3,8,h264.pps())+h264.nal(3,5,h264.idr_slice())
        assert h264.parse_validate(stream)==[7,8,5]
        samples=width*height*3//2
        assert samples>65536 and len(stream)>samples
        assert len(stream)<=capacity
        # Also bound worst-case escaping of the fixed test's sample payload,
        # with deliberately generous per-MB syntax and header allowance.
        rbsp_allowance=(width//16)*(height//16)*(384+16)+4096
        assert len(h264.ebsp(bytes(rbsp_allowance)))<=capacity
        print(f'PASS {width}x{height} parsed I_PCM fixture: {len(stream)} bytes; '
              f'old 65536-byte buffer cannot fit it; new {capacity}-byte capacity '
              'also covers conservative escaping allowance (not native encode proof)')
    finally:
        h264.WIDTH_MBS,h264.HEIGHT_MBS,h264.WIDTH,h264.HEIGHT=saved

if __name__=='__main__':main()
