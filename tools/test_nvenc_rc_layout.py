#!/usr/bin/env python3
"""Host-only RC size/offset arithmetic; no submission, firmware model or codec."""
from pathlib import Path
import ctypes as C
import _ctypes
import hashlib
import json
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLANG = r'C:\Program Files\LLVM\bin\clang.exe'


def main():
    code = '#include "'+(ROOT/'kernel/nvenc_h264_buffers.h').as_posix()+'"\n'+r'''
void __main(void){}
__declspec(dllexport) int sizes(unsigned w,unsigned h,unsigned *v){
    nvenc_h264_buffer_requirements_s r;
    unsigned char *p=(unsigned char *)&r;
    for(unsigned i=0;i<sizeof r;i++)p[i]=0xff;
    int ok=nvenc_h264_buffer_requirements(&r,w,h);
    v[0]=r.rc_stat_bytes;v[1]=r.rc_act_offset;
    v[2]=r.height_mbs;v[3]=0;
    if(!ok)for(unsigned i=0;i<sizeof r;i++)v[3]|=p[i];
    return ok;
}
__declspec(dllexport) int cqp(unsigned qp,unsigned fps,unsigned char *v){
    nvenc_h264_rc_s r;
    _Static_assert(sizeof r==88,"RC wire size");
    int ok=nvenc_cfb7_h264_cqp_idr_defaults(&r,qp,fps);
    for(unsigned i=0;i<sizeof r;i++)v[i]=((unsigned char *)&r)[i];
    return ok;
}
'''
    with tempfile.TemporaryDirectory(prefix='nvenc-rc-layout-') as tmp:
        c,obj,dll=[Path(tmp)/('layout.'+s) for s in ('c','obj','dll')]
        c.write_text(code)
        subprocess.run([CLANG,'--target=x86_64-w64-windows-gnu','-mno-ms-bitfields',
                        '-ffreestanding','-O2','-Wall','-Wextra','-Werror','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([CLANG,'-shared',str(obj),'-o',str(dll)],check=True)
        lib=C.CDLL(str(dll))
        try:
            lib.sizes.argtypes=[C.c_uint,C.c_uint,C.POINTER(C.c_uint)]
            values=(C.c_uint*4)()
            cases=0
            for height in range(64,4097,16):
                for width in (48,256,1920,4096):
                    assert lib.sizes(width,height,values)==1
                    rows=height//16
                    # Independent integer ceiling formula, not helper's masks.
                    activity=256+(((rows+7)//8*8*16+255)//256)*256
                    assert tuple(values)==(activity+rows*256,activity,rows,0)
                    assert activity>=256+rows*16
                    cases+=1
            for w,h in ((0,0),(32,256),(256,48),(4097,256),(256,4097),
                        (255,256),(256,255),(0xffffffff,0xffffffff)):
                assert lib.sizes(w,h,values)==0 and not any(values)
            assert lib.sizes(256,256,values)==1 and tuple(values)==(4608,512,16,0)
            lib.cqp.argtypes=[C.c_uint,C.c_uint,C.POINTER(C.c_ubyte)]
            result=(C.c_ubyte*88)()
            for qp in range(52):
                for fps in (1,30,60,240,1000,0x7fffff):
                    assert lib.cqp(qp,fps,result)==1
                    raw=bytes(result)
                    assert raw[1:4]==bytes([qp])*3 and raw[4:7]==bytes(3)
                    assert raw[7:12]==bytes([51,51,51,6,3])
                    assert struct.unpack_from('<3i',raw,12)==(256,256,256)
                    assert struct.unpack_from('<i',raw,24)[0]==fps*256
                    assert struct.unpack_from('<I',raw,48)[0]==1
                    assert raw[74]==1 and raw[77:79]==bytes([1,1])
                    # CQP must not accidentally enable HRD/bitrate/AQ or hints.
                    allowed=set(range(1,4))|set(range(7,28))|set(range(48,52))|{74,77,78}
                    assert not any(b for i,b in enumerate(raw) if i not in allowed)
            for qp,fps in ((52,30),(0xffffffff,30),(26,0),(26,0x800000),(26,0xffffffff)):
                assert lib.cqp(qp,fps,result)==0 and not any(result)
            capture=json.loads((ROOT/'tools/fixtures/nvenc_cfb7_windows_256.json').read_text())
            pic=bytes.fromhex(capture['picture_hex'])
            assert hashlib.sha256(pic).hexdigest()==capture['picture_sha256']
            assert lib.cqp(18,30,result)==1
            # Independently captured wire bytes, not the helper's own structs.
            for a,b in ((1,28),(48,52),(74,75),(77,79)):
                assert bytes(result)[a:b]==pic[0x70+a:0x70+b]
        finally:
            _ctypes.FreeLibrary(lib._handle)
    print(f'PASS {cases} RC layouts + 8 rejected dimensions; 256x256 requires 4608 bytes, activity offset 512; host arithmetic only')
    print('PASS 312 CQP configurations + 5 invalid inputs; neutral controls match retained Windows wire fields; no codec executed')


if __name__=='__main__':main()
