#!/usr/bin/env python3
"""Independent SPS/PPS context and exact retired-IDR transport regression.

Executes the production header under undefined-behavior traps. Metadata tests
do not claim real NVENC output, entropy decoding, or hardware completion.
"""
from pathlib import Path
from contextlib import ExitStack
import ctypes as C
import _ctypes
import random
import re
import hashlib
import shutil
import subprocess
import tempfile
import gen_h264_iframe as fixture
from test_h264_decode import Bits, nal, stream
from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]
CAPACITY = 0x40000


def main():
    header = (ROOT / 'include/kestrel/h264_decode.h').read_text()
    body = header.split('typedef struct {', 1)[1].split('} kh264_decode_desc;', 1)[0]
    fields = []
    for kind, names in re.findall(r'\b(unsigned int|int)\s+([^;]+);', body):
        for item in names.split(','):
            name, count = re.fullmatch(r'\s*(\w+)(?:\[(\d+)\])?\s*', item).groups()
            ty = C.c_uint if kind == 'unsigned int' else C.c_int
            fields.append((name, ty * int(count) if count else ty))
    class Desc(C.Structure):
        _fields_ = fields
    c = '#include "' + (ROOT / 'kernel/nvenc_h264_stream.h').as_posix() + '"\n'
    c += r'''
#define assert(x) do { if (!(x)) __builtin_trap(); } while (0)
static nvenc_cfb7_h264_drv_pic_setup_s config(const int *p,int bad){
    nvenc_cfb7_h264_drv_pic_setup_s c={0};c.magic=NVENC_CFB7_DRV_MAGIC;
    c.input_cfg.frame_width_minus1=p[0]-1;c.input_cfg.frame_height_minus1=p[1]-1;
    c.sps_data.profile_idc=66;c.sps_data.chroma_format_idc=1;
    c.sps_data.level_idc=p[2];c.sps_data.frame_mbs_only=1;
    c.sps_data.log2_max_frame_num_minus4=p[3]-4;
    c.sps_data.pic_order_cnt_type=p[4];c.sps_data.log2_max_pic_order_cnt_lsb_minus4=p[5]-4;
    c.pps_data.pic_param_set_id=p[6];c.pps_data.pic_init_qp_minus26=p[7];
    c.pps_data.chroma_qp_index_offset=p[8];c.pps_data.second_chroma_qp_index_offset=p[9];
    c.pps_data.deblocking_filter_control_present_flag=p[10];
    c.pps_data.pic_order_present_flag=p[11];
    c.pic_control.idr_pic_id=p[12];c.pic_control.pic_order_cnt_lsb=p[13];
    c.pic_control.pic_type=3;c.pic_control.ref_pic_flag=1;
    switch(bad){
    case 1:c.magic=0;break;
    case 2:c.sps_data.profile_idc=100;break;
    case 3:c.sps_data.chroma_format_idc=0;break;
    case 4:c.sps_data.frame_mbs_only=0;break;
    case 5:c.sps_data.stereo_mvc_enable=1;break;
    case 6:c.sps_data.separate_colour_plane_flag=1;break;
    case 7:c.sps_data.lossless_qpprime_flag=1;break;
    case 8:c.pps_data.entropy_coding_mode_flag=1;break;
    case 9:c.pps_data.transform_8x8_mode_flag=1;break;
    case 10:c.pps_data.weighted_pred_flag=1;break;
    case 11:c.pps_data.weighted_bipred_idc=2;break;
    case 12:c.pic_control.pic_type=0;break;
    case 13:c.pic_control.ref_pic_flag=0;break;
    case 14:c.pic_control.frame_num=1;break;
    case 15:c.input_cfg.frame_width_minus1=4096;break;
    case 16:c.input_cfg.frame_height_minus1=62;break;
    case 17:c.pps_data.num_ref_idx_l0_active_minus1=1;break;
    case 18:c.pps_data.constrained_intra_pred_flag=1;break;
    case 19:c.pic_control.delta_pic_order_cnt_bottom=1;break;
    case 20:c.pic_control.pic_struct=1;break;
    case 21:c.pic_control.bit_depth_minus_8=2;break;
    }
    return c;
}
__declspec(dllexport) int pack(const int *p,int bad,unsigned char *out,unsigned cap,
    const unsigned char *raw,unsigned n,unsigned *written,kh264_decode_desc *desc){
    nvenc_cfb7_h264_drv_pic_setup_s cfg=config(p,bad);
    return nvenc_h264_make_stream(out,cap,&cfg,raw,n,written,desc);
}
__declspec(dllexport) int ue(unsigned value,unsigned char *out,unsigned cap){
    nvenc_h264_writer w={0};nvenc_h264_ue(&w,value);
    return (int)nvenc_h264_nal(out,cap,6,&w);
}
__declspec(dllexport) void writer_bounds(void){
    struct {unsigned before;nvenc_h264_writer w;unsigned after;} a={0};
    a.before=0x12345678;a.after=0xabcdef01;
    for(unsigned i=0;i<16;i++)nvenc_h264_put(&a.w,32,0xffffffff);
    assert(a.w.bits==512 && !a.w.error);
    nvenc_h264_put(&a.w,1,1);assert(a.w.error && a.w.bits==512);
    assert(a.before==0x12345678 && a.after==0xabcdef01);
    for(unsigned i=0;i<64;i++)assert(a.w.bytes[i]==255);
    nvenc_h264_writer b={0};nvenc_h264_put(&b,33,0);assert(b.error && !b.bits);
    b=(nvenc_h264_writer){0};nvenc_h264_ue(&b,0xffffffff);assert(b.error && !b.bits);
    b=(nvenc_h264_writer){0};nvenc_h264_se(&b,-52);assert(b.error && !b.bits);
    b=(nvenc_h264_writer){0};nvenc_h264_se(&b,52);assert(b.error && !b.bits);
}
'''
    c += r'''
#include <stdbool.h>
typedef unsigned char u8;typedef unsigned int u32;
static nvenc_h264_test_output_s g_nvenc_output;
static nvenc_cfb7_h264_drv_pic_setup_s g_nvenc_output_context;
'''
    c += function((ROOT / 'kernel/nv_chan.c').read_text(), 'nv_nvenc_test_stream')
    c += r'''
__declspec(dllexport) int cached(const int *p,int corrupt,unsigned char *out,unsigned cap,
    const unsigned char *raw,unsigned n,unsigned *written,kh264_decode_desc *desc){
    assert(n<=sizeof g_nvenc_output.data);
    for(unsigned i=0;i<n;i++)g_nvenc_output.data[i]=raw[i];
    g_nvenc_output.bytes=n;
    nvenc_h264_single_idr(raw,n,&g_nvenc_output.slice_start,&g_nvenc_output.slice_end);
    g_nvenc_output_context=config(p,0);
    switch(corrupt){
    case 1:g_nvenc_output.bytes=0;break; /* stale bytes remain inaccessible */
    case 2:g_nvenc_output.slice_start++;break;
    case 3:g_nvenc_output.slice_end--;break;
    case 4:g_nvenc_output_context.input_cfg.frame_width_minus1^=0x10;break;
    case 5:g_nvenc_output_context.magic=0;break;
    case 6:g_nvenc_output.bytes=NVENC_H264_TEST_OUTPUT_CAPACITY+1;break;
    }
    return nv_nvenc_test_stream(out,cap,written,desc);
}
'''
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    total = 0
    with tempfile.TemporaryDirectory(prefix='kestrel-nvenc-stream-') as tmp, ExitStack() as cleanup:
        source, obj, dll = (Path(tmp) / name for name in ('stream.c', 'stream.obj', 'stream.dll'))
        source.write_text(c)
        # Same GNU bitfield ABI as the freestanding kernel, native Windows DLL
        # calling convention for ctypes; retain all production ABI assertions.
        subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-ffreestanding',
                        '-std=c11', '-O2', '-mno-ms-bitfields', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-trap=all', '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([clang, '-shared', str(obj), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll)); cleanup.callback(_ctypes.FreeLibrary, lib._handle)
        lib.pack.argtypes = [C.POINTER(C.c_int), C.c_int, C.c_void_p, C.c_uint,
                             C.c_void_p, C.c_uint, C.POINTER(C.c_uint), C.POINTER(Desc)]
        lib.pack.restype = C.c_int
        lib.cached.argtypes=lib.pack.argtypes;lib.cached.restype=C.c_int
        lib.ue.argtypes = [C.c_uint, C.c_void_p, C.c_uint]; lib.ue.restype = C.c_int
        lib.writer_bounds()

        def params(p):
            p = dict(p)
            if p['poc'] == 2: p['pbits'] = 4
            keys = ('w', 'h', 'level', 'fbits', 'poc', 'pbits', 'pid', 'qp', 'chroma',
                    'second_chroma', 'deblock', 'bottom', 'idr', 'poc_lsb')
            if p['second_chroma'] is None: p['second_chroma'] = p['chroma']
            return (C.c_int * len(keys))(*(p[k] for k in keys))

        def pack(raw, p, success=True, cap=CAPACITY + 128, bad=0):
            nonlocal total
            total += 1
            src = C.create_string_buffer(raw)
            dst = C.create_string_buffer(cap + 32); C.memset(dst, 0xa5, len(dst))
            written = C.c_uint(0xdeadbeef); d = Desc(); C.memset(C.byref(d), 0xa5, C.sizeof(d))
            rc = lib.pack(params(p), bad, dst, cap, src, len(raw), C.byref(written), C.byref(d))
            assert bool(rc) == success, (total, rc, success, bad, p, raw[:40].hex())
            assert dst.raw[cap:] == b'\xa5' * 32, 'destination capacity overwritten'
            if not rc:
                assert written.value == 0 and bytes(d) == bytes(C.sizeof(d))
                return b'', d
            assert 0 < written.value <= cap
            out = dst.raw[:written.value]
            assert (d.coded_width, d.coded_height, d.level_idc, d.profile_idc) == (p['w'], p['h'], p['level'], 66)
            for name, key in [('pps_id','pid'), ('pic_order_cnt_type','poc'),
                              ('pic_init_qp_minus26','qp'), ('chroma_qp_index_offset','chroma'),
                              ('deblocking_filter_control_present_flag','deblock'),
                              ('pic_order_present_flag','bottom'), ('idr_pic_id','idr'),
                              ('pic_order_cnt_lsb','poc_lsb')]:
                assert getattr(d, name) == p[key], (name, getattr(d, name), p[key])
            assert d.log2_max_frame_num_minus4 == p['fbits'] - 4
            assert d.log2_max_pic_order_cnt_lsb_minus4 == (p['pbits'] - 4 if p['poc'] == 0 else 0)
            assert d.second_chroma_qp_index_offset == (p['chroma'] if p['second_chroma'] is None else p['second_chroma'])
            return out, d

        boot = (ROOT / 'tools/h264_iframe_test.h').read_text()
        body = boot.split('h264_iframe_test[]', 1)[1].split('{', 1)[1].split('}', 1)[0]
        boot = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
        _, base, _, _ = stream()
        fixtures = [(boot, dict(base, w=64, h=64, level=11, deblock=0))]
        saved = fixture.WIDTH_MBS, fixture.HEIGHT_MBS, fixture.WIDTH, fixture.HEIGHT
        try:
            fixture.WIDTH_MBS = fixture.HEIGHT_MBS = 16; fixture.WIDTH = fixture.HEIGHT = 256
            ipcm = fixture.nal(3, 7, fixture.sps()) + fixture.nal(3, 8, fixture.pps()) + fixture.nal(3, 5, fixture.idr_slice())
            fixtures.append((ipcm, dict(base, level=11, deblock=0)))
        finally: fixture.WIDTH_MBS, fixture.HEIGHT_MBS, fixture.WIDTH, fixture.HEIGHT = saved
        # Actual unmodified Windows RTX 5070 Ti NVENC output, archived locally
        # so preflight never depends on the temporary Windows capture directory.
        # Independent capture manifest: profile66/level31/POC2, log2 frame_num
        # minus4=4 (eight frame bits), initial QP26, slice delta-8 => QP18.
        hardware = (ROOT / 'tools/fixtures/h264_nvenc_256.h').read_text()
        body = hardware.split('h264_nvenc_256[]',1)[1].split('{',1)[1].split('}',1)[0]
        hardware = bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',body))
        assert len(hardware)==5014
        assert hashlib.sha256(hardware).hexdigest()=='efc9304e30db3db7923ec26a5c9ffd0487c565af4b23cab3e32f11e01dae46da'
        hardware_params=dict(base,level=31,poc=2,fbits=8,deblock=1)
        fixtures.append((hardware,hardware_params))
        for raw, p in fixtures:
            out, d = pack(raw, p); assert out == raw
            if raw==hardware:
                assert (d.slice_start,d.slice_end,d.slice_header_bits)==(32,5014,28)
                assert (d.slice_qp_delta,d.slice_qp,d.disable_deblocking_filter_idc)==(-8,18,1)
                assert (d.num_units_in_tick,d.time_scale,d.fixed_frame_rate)==(1000,60000,1)
            idr = raw[d.slice_start:d.slice_end]
            out, d = pack(idr, p); assert out[d.slice_start:d.slice_end] == idr
            if raw==hardware:
                assert (d.slice_qp_delta,d.slice_qp)==(-8,18)
                assert not d.num_units_in_tick and not d.time_scale # New metadata has no VUI.
            exact = len(out); assert pack(idr, p, cap=exact)[0] == out
            pack(idr, p, False, cap=exact-1); pack(raw, p, False, cap=len(raw)-1)
            # Execute the actual channel getter with consistent and deliberately
            # stale/mismatched cached raw bounds plus its captured configuration.
            for source,expected in ((raw,raw),(idr,out)):
                src=C.create_string_buffer(source);dst=C.create_string_buffer(CAPACITY+128)
                for corrupt in range(7):
                    # A raw IDR carries no dimensions. Its context identity is
                    # guaranteed by the retirement snapshot, not recoverable
                    # from these bytes; only complete metadata can contradict it.
                    if corrupt==4 and source==idr:continue
                    written=C.c_uint(123);d=Desc();C.memset(C.byref(d),0xa5,C.sizeof(d))
                    rc=lib.cached(params(p),corrupt,dst,len(dst),src,len(source),C.byref(written),C.byref(d))
                    assert bool(rc)==(corrupt==0),(corrupt,len(source),rc)
                    if rc: assert dst.raw[:written.value]==expected
                    else: assert not written.value and bytes(d)==bytes(C.sizeof(d))
                written=C.c_uint(123);d=Desc()
                assert not lib.cached(params(p),0,dst,len(expected)-1,src,len(source),C.byref(written),C.byref(d))
                assert not written.value

        rng = random.Random(0xc0dec)
        for i in range(240):
            poc = i % 2 * 2; pbits = rng.randrange(4, 17)
            raw, p, _, (sps, pps, idr) = stream(
                w=rng.choice((48,64,256,1920,4096)), h=rng.choice((64,256,1088,4096)),
                pid=rng.randrange(256), fbits=rng.randrange(4,17), poc=poc,
                pbits=pbits, poc_lsb=rng.randrange(1 << pbits) if poc==0 else 0,
                idr=rng.randrange(65536), qp=rng.randrange(-26,26), chroma=rng.randrange(-12,13),
                second_chroma=rng.randrange(-12,13), bottom=i%2, deblock=i%3!=0,
                disable=1, level=rng.choice((31,40,51,52)))
            out, d = pack(raw,p); assert out == raw
            out, d = pack(idr,p); assert out[d.slice_start:d.slice_end] == idr
            assert not any((d.crop_left,d.crop_right,d.crop_top,d.crop_bottom))
            assert d.max_num_ref_frames == 1
            pack(sps+idr,p,False);pack(pps+idr,p,False)
            pack(raw,dict(p,level=(p['level']+1)%256),False)
            pack(raw,dict(p,qp=p['qp']-1 if p['qp']> -26 else 1),False)
            pack(raw,dict(p,w=64 if p['w']!=64 else 256),False)

        raw, p, _, (sps, pps, idr) = stream(bottom=1)
        # Existing metadata and optional NALs must survive exactly, never be replaced.
        enriched = nal(9,b'\x10')+raw+nal(12,b'\xff\x80')+nal(10,b'\x80')
        assert pack(enriched,p)[0] == enriched
        short_idr=idr[1:] # Three-byte start code is copied without normalization.
        out,d=pack(short_idr,p);assert out[d.slice_start:d.slice_end] == short_idr
        # Context is independently supplied, not inferred back from raw bytes.
        for key,value in [('w',64),('h',64),('level',40),('pid',1),('fbits',5),
                          ('poc',2),('pbits',5),('qp',1),('chroma',1),
                          ('second_chroma',1),('deblock',0),('bottom',0),
                          ('idr',1),('poc_lsb',1)]:
            pack(raw,dict(p,**{key:value}),False)
        pack(stream(bottom=1,delta_bottom=1)[0],p,False)
        pack(stream(bottom=1,redundant=1)[0],p,False)
        pack(stream(bottom=1,crop=(0,1,0,0))[0],p,False)
        for cap in (0,1,4,5,16): pack(idr,p,False,cap=cap)
        for bad in range(1,22): pack(raw,p,False,bad=bad)
        for candidate in (b'',b'garbage',sps+pps,sps+pps+idr+idr,b'bad'+raw,
                          raw+b'\0\0\1',sps+pps+nal(1,b'\x80',ref=3),
                          sps+pps+nal(5,b'\x80',ref=0),b'\0\0\1\xe5\x80'):
            pack(candidate,p,False)
        for end in range(len(sps)+len(pps)+5): pack(raw[:end],p,False)
        buf=C.create_string_buffer(raw+b'\0'*256); dst=C.create_string_buffer(512)
        for delta,cap in ((0,len(raw)),(1,len(raw)),(-1,len(raw)),(len(raw)-1,1)):
            address=C.addressof(buf)+max(0,delta); source=C.addressof(buf)+(1 if delta<0 else 0)
            written=C.c_uint(123);d=Desc()
            assert not lib.pack(params(p),0,address,cap,source,len(raw),C.byref(written),C.byref(d))
            assert not written.value
        written=C.c_uint(123)
        assert not lib.pack(params(p),0,dst,len(dst),buf,len(raw),C.byref(written),None)
        assert not written.value, 'valid written pointer must be cleared when desc is null'
        assert not lib.pack(params(p),0,dst,len(dst),buf,len(raw),None,C.byref(Desc()))
        for outptr,inptr,n in ((None,buf,len(raw)),(dst,None,len(raw)),(dst,C.c_void_p(1),CAPACITY+1)):
            written=C.c_uint(123);d=Desc()
            assert not lib.pack(params(p),0,outptr,len(dst),inptr,n,C.byref(written),C.byref(d))
            assert not written.value
        # Integer overflow in either advertised range must fail before access.
        highest=(1 << (C.sizeof(C.c_void_p)*8))-1
        for outptr,inptr in ((C.c_void_p(highest-7),buf),(dst,C.c_void_p(highest-7))):
            written=C.c_uint(123);d=Desc()
            assert not lib.pack(params(p),0,outptr,len(dst),inptr,len(raw),C.byref(written),C.byref(d))
            assert not written.value

        for value in [0,1,2,3,0x7fffffff,0xfffffffe]+[rng.randrange(0xffffffff) for _ in range(2000)]:
            bits=Bits();bits.ue(value);expected=nal(6,bits.finish())
            dst=C.create_string_buffer(128);n=lib.ue(value,dst,len(dst))
            assert n==len(expected) and dst.raw[:n]==expected
            assert lib.ue(value,dst,n-1)==0
        assert lib.ue(0xffffffff,dst,len(dst))==0
        print(f'PASS {total} actual NVENC stream packaging cases + actual cached-stream getter + 2006 independent UE encodings; exact retired IDR bytes, context matching, capacity/alias/failure guards')


if __name__ == '__main__':
    main()
