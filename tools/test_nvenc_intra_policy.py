#!/usr/bin/env python3
"""Pack the actual native MD/quant initialization in CPU memory only.

No encoder call, command submission, GPU model, or codec execution.
"""
from pathlib import Path
import ctypes as C
import _ctypes
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLANG = r'C:\Program Files\LLVM\bin\clang.exe'


def main():
    src = (ROOT / 'kernel/nv_chan.c').read_text()
    body = src.split('    md->intra_luma16x16_mode_enable =', 1)[1]
    body = '    md->intra_luma16x16_mode_enable =' + body.split(
        '    /* Match the ordinary reference resource lifecycle,', 1)[0]
    # RC work-state setup now follows MD/quant setup and has a separate
    # byte-level check. This harness executes only MD/quant construction.
    assert 'nvenc_h264_cqp_idr_work_init' not in body
    # The input source must not switch normal encoding into a boot-only mode.
    # Strip comments before checking so prose does not count as C control flow.
    import re
    executable = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)
    assert not re.search(r'\binput\b', executable)
    code = '#include "'+(ROOT/'kernel/nvenc_h264_policy.h').as_posix()+'"\n'
    code += r'''
__declspec(dllexport) void pack(unsigned char *out) {
    nvenc_h264_md_control_s md_value={0}, *md=&md_value;
    nvenc_h264_quant_control_s q_value={0}, *q=&q_value;
'''+body+r'''
    for(unsigned i=0;i<sizeof md_value;i++)out[i]=((unsigned char *)md)[i];
    for(unsigned i=0;i<sizeof q_value;i++)out[sizeof md_value+i]=((unsigned char *)q)[i];
}
'''
    with tempfile.TemporaryDirectory(prefix='nvenc-intra-policy-') as tmp:
        c,obj,dll=[Path(tmp)/('policy.'+s) for s in ('c','obj','dll')]
        c.write_text(code)
        subprocess.run([CLANG,'--target=x86_64-w64-windows-gnu','-mno-ms-bitfields',
                        '-ffreestanding','-O2','-Wall','-Wextra','-Werror','-c',str(c),'-o',str(obj)],check=True)
        subprocess.run([CLANG,'-shared',str(obj),'-o',str(dll)],check=True)
        lib=C.CDLL(str(dll))
        try:
            lib.pack.argtypes=[C.c_void_p]
            out=(C.c_ubyte*320)(*[0xa5]*320)
            lib.pack(out)
            raw=bytes(out)
            expected=bytearray(128)
            struct.pack_into('<I',expected,4,0x03fc0000)
            struct.pack_into('<I',expected,52,0x10180000)
            assert raw[:128]==expected
            # Explicit masks for normal encode, full 16x16 search, and no
            # early termination. These assertions do not model firmware.
            flags=struct.unpack_from('<I',raw,52)[0]
            assert not flags & (1<<30) and (flags>>26)&7==4
            assert (flags>>19)&3==3 and not flags & (1<<31)
            assert raw[128:130]==bytes.fromhex('6b05')
            assert raw[136:139]==bytes([15,15,15])
            print('PASS actual boot/application intra policy: identical normal QP26 MD setup, forced IPCM off, initialized quant record (CPU packing only)')
        finally:
            _ctypes.FreeLibrary(lib._handle)


if __name__ == '__main__':
    main()
