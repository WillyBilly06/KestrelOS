#!/usr/bin/env python3
"""CPU-only history sizing and native region placement; no codec or VM."""
from pathlib import Path
import ctypes as C
import _ctypes
import subprocess
import tempfile
from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]
CLANG = r'C:\Program Files\LLVM\bin\clang.exe'


def main():
    source = (ROOT / 'kernel/nv_chan.c').read_text()
    encoder = function(source, 'nvenc_encode_frame_hw')
    start = encoder.index('enum {')
    layout = encoder[start:encoder.index('};', start) + 2]
    assert 'requirements.history_allocation_bytes>HISTORY_CAPACITY' in encoder
    assert 'cfg->pic_control.hist_buf_size = requirements.history_bytes;' in encoder
    code = '#include "' + (ROOT / 'kernel/nvenc_h264_buffers.h').as_posix() + '"\n'
    code += r'''
void __main(void){}
__declspec(dllexport) int sizes(unsigned w,unsigned h,unsigned *out) {
    nvenc_h264_buffer_requirements_s r;
    int ok=nvenc_h264_buffer_requirements(&r,w,h);
    out[0]=r.history_bytes; out[1]=r.history_allocation_bytes;
    return ok;
}
__declspec(dllexport) void placement(unsigned *out) {
'''
    code += layout + r'''
    unsigned values[]={HISTORY,HISTORY_CAPACITY,COLOC_IN,COLOC_OUT,
        IN_LUMA,IN_CHROMA,OUT_LUMA,OUT_CHROMA,RC_STATS,RC_STATS_CAPACITY,
        BITSTREAM,BITSTREAM_CAPACITY,RC_PROCESS,RC_PROCESS_CAPACITY};
    for(unsigned i=0;i<14;i++)out[i]=values[i];
}
'''
    with tempfile.TemporaryDirectory(prefix='nvenc-history-layout-') as tmp:
        c,obj,dll=[Path(tmp)/('layout.'+x) for x in ('c','obj','dll')]
        c.write_text(code)
        subprocess.run([CLANG,'--target=x86_64-w64-windows-gnu','-mno-ms-bitfields',
                        '-ffreestanding','-O2','-Wall','-Wextra','-Werror',
                        '-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([CLANG,'-shared',str(obj),'-o',str(dll)],check=True)
        lib=C.CDLL(str(dll))
        try:
            lib.sizes.argtypes=[C.c_uint,C.c_uint,C.POINTER(C.c_uint)]
            result=(C.c_uint*2)()
            cases=0
            for h in range(64,4097,16):
                for w in (48,256,1920,4096):
                    assert lib.sizes(w,h,result)==1
                    slot=((w//16*384+255)//256)*256
                    assert tuple(result)==(slot,slot*16)
                    cases+=1
            for w,h in ((0,0),(32,256),(256,48),(4097,256),(256,4097),
                        (255,256),(256,255),(0xffffffff,0xffffffff)):
                assert lib.sizes(w,h,result)==0 and not any(result)
            assert lib.sizes(256,256,result)==1
            assert tuple(result)==(6144,98304)
            placement=(C.c_uint*14)()
            lib.placement.argtypes=[C.POINTER(C.c_uint)]
            lib.placement(placement)
            assert tuple(placement)==(0x4f000,0x18000,0x67000,0x6f000,
                0x7f000,0x8f000,0x9f000,0xaf000,0xb7000,0x2000,0xb000,0x40000,
                0xb9000,0x3000)
            offsets=list(placement)
            regions=[(offsets[10],offsets[11]),(offsets[0],offsets[1]),
                     (offsets[2],0x8000),(offsets[3],0x10000),
                     (offsets[4],0x10000),(offsets[5],0x8000),
                     (offsets[6],0x10000),(offsets[7],0x8000),
                     (offsets[8],offsets[9]),(offsets[12],offsets[13])]
            for i,(base,size) in enumerate(regions):
                assert base%256==0 and base+size<=0xc0000
                for other,extent in regions[i+1:]:
                    assert base+size<=other or other+extent<=base
            # The previous 64 KiB slot covers a picture slot, but not the
            # reference driver's conservative pool. Never conflate those.
            assert 6144<=65536<result[1]<=placement[1]
        finally:
            _ctypes.FreeLibrary(lib._handle)
    print(f'PASS {cases} history layouts + 8 rejected dimensions; logical 6144, backing 98304; all native regions nonoverlapping and inside 0xc0000')
    print('Host arithmetic only; no firmware execution or native NVENC success claim')


if __name__=='__main__':
    main()
