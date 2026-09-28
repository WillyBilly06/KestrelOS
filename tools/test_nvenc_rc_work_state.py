#!/usr/bin/env python3
"""Check RC initializer bytes in CPU memory, not encoder/firmware execution."""
from pathlib import Path
import ctypes as C
import _ctypes
import hashlib
import json
import struct
import subprocess
import tempfile
from test_gpu_stable_candidate import function

ROOT=Path(__file__).resolve().parents[1]
CLANG=r'C:\Program Files\LLVM\bin\clang.exe'
BYTES=0x2c00


def main():
    encoder=function((ROOT/'kernel/nv_chan.c').read_text(),'nvenc_encode_frame_hw')
    call=encoder.index('if (!nvenc_h264_cqp_idr_work_init(')
    upload=encoder.index('if (!nv_vram_object_write(')
    bind=encoder.index('nvenc_address(ch, NVCFB7_SET_IO_RC_PROCESS, VA_NVENC + RC_PROCESS);')
    execute=encoder.index('pb_method(ch, SUBCH_NVENC, NVCFB7_EXECUTE, 1)')
    assert call<upload<bind<execute
    assert 'stage+RC_PROCESS,RC_PROCESS_CAPACITY' in encoder[call:upload]
    assert '&cfg->rate_control' in encoder[call:upload]
    assert 'return -1;' in encoder[call:upload]
    code='#include "'+(ROOT/'kernel/nvenc_h264_buffers.h').as_posix()+'"\n'+r'''
void __main(void){}
__declspec(dllexport) int init(unsigned char *out,unsigned bytes,unsigned qp,unsigned bad) {
    nvenc_h264_rc_s rc;
    if (!nvenc_cfb7_h264_cqp_idr_defaults(&rc,qp,30)) return -1;
    switch(bad) {
    case 1:rc.hrd_type=1;break;
    case 2:rc.nal_cpb_size=1;break;
    case 3:rc.vcl_cpb_size=1;break;
    case 4:rc.nal_bitrate=1;break;
    case 5:rc.vcl_bitrate=1;break;
    case 6:rc.gop_length=2;break;
    case 7:rc.Np=16;break;
    case 8:rc.buffersize=1;break;
    case 9:rc.Bmin=1;break;
    case 10:rc.Ravg=1;break;
    case 11:rc.R=1;break;
    }
    return nvenc_h264_cqp_idr_work_init(out,bytes,bad==12?0:&rc);
}
'''
    words=[0]*64
    words[2:14]=[24,12,48,24,24,24,12,12,12,48,48,48]
    words[20:22]=[256,26]
    expected=struct.pack('<64I',*words)
    # Two independent, retained Windows QP18/QP26 observations. These encode
    # different inputs but use the same unconditional RC initializer values.
    # Verify source record hashes from each capture's immutable manifest.
    for stamp in ('20260919-004637','20260919-031740'):
        directory=ROOT/'out/nvenc-reference-256'/stamp
        manifest=json.loads((directory/'manifest.json').read_text())
        for name in ('rc-initializer-0.bin','rc-initializer-1.bin'):
            raw=(directory/name).read_bytes()
            assert len(raw)==256
            # Match this exact artifact's manifest entry. Do not infer bounds
            # or sample data from a driver's live GPU VA.
            digest=hashlib.sha256(raw).hexdigest()
            assert digest==manifest['live_method_capture']['sha256'][name]
            assert raw[8:]==expected[8:]
            assert struct.unpack_from('<2I',raw)==(22400000,0)
    with tempfile.TemporaryDirectory(prefix='nvenc-rc-work-') as tmp:
        c,obj,dll=[Path(tmp)/('state.'+x) for x in ('c','obj','dll')]
        c.write_text(code)
        subprocess.run([CLANG,'--target=x86_64-w64-windows-gnu','-mno-ms-bitfields',
                        '-ffreestanding','-O2','-Wall','-Wextra','-Werror','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([CLANG,'-shared',str(obj),'-o',str(dll)],check=True)
        lib=C.CDLL(str(dll))
        try:
            lib.init.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.c_uint]
            for qp in range(52):
                guard=(C.c_ubyte*(BYTES+2))(*([0xa5]*(BYTES+2)))
                assert lib.init(C.byref(guard,1),BYTES,qp,0)==1
                assert guard[0]==guard[-1]==0xa5
                assert bytes(guard)[1:-1]==expected*2+bytes(BYTES-512)
            for bad in range(1,13):
                guard=(C.c_ubyte*(BYTES+2))(*([0xa5]*(BYTES+2)))
                assert lib.init(C.byref(guard,1),BYTES,26,bad)==0
                assert bytes(guard)==bytes([0xa5])*(BYTES+2)
            for length in (0,1,255,256,511,512,BYTES-1):
                assert lib.init(C.byref(guard,1),length,26,0)==0
                assert bytes(guard)==bytes([0xa5])*(BYTES+2)
            assert lib.init(None,BYTES,26,0)==0
        finally:
            _ctypes.FreeLibrary(lib._handle)
    print('PASS 52 QP cases; both 256-byte RC records, 11264-byte work extent, guards and invalid policies; four hashed Windows records agree except session CPB fullness')
    print('CPU-only initializer validation; no codec, GPU submission, firmware model or VM')


if __name__=='__main__':main()
