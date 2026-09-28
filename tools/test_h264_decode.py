#!/usr/bin/env python3
"""Execute the bounded production Annex-B header parser on native host code.

Checks syntax/context, NOT NVDEC execution or entropy decoding. Positive streams
include the real boot fixture and independent generated I_PCM macroblocks.
"""
from pathlib import Path
import ctypes as C
import _ctypes
from contextlib import ExitStack
import random
import re
import shutil
import subprocess
import tempfile
import gen_h264_iframe as fixture

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'include/kestrel/h264_decode.h'


class Bits:
    """Test-side syntax writer, separate from the C production reader."""
    def __init__(self): self.b = []
    def u(self, n, v): self.b.extend((v >> i) & 1 for i in range(n - 1, -1, -1))
    def ue(self, v):
        n = (v + 1).bit_length()
        self.u(n - 1, 0); self.u(n, v + 1)
    def se(self, v): self.ue(2 * v - 1 if v > 0 else -2 * v)
    def finish(self):
        self.u(1, 1)
        while len(self.b) % 8: self.u(1, 0)
        return bytes(sum(self.b[i + k] << (7 - k) for k in range(8))
                     for i in range(0, len(self.b), 8))


def nal(t, rbsp, ref=None, prefix=4):
    if ref is None: ref = 3 if t in (5, 7, 8) else 0
    out = bytearray(b'\0' * (prefix - 1) + b'\1' + bytes([ref * 32 + t]))
    zeros = 0
    for b in rbsp:
        if zeros == 2 and b <= 3: out.append(3); zeros = 0
        out.append(b); zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def stream(**p):
    p = dict(dict(w=256, h=256, sid=0, pid=0, fbits=4, poc=0, pbits=4,
                  refs=1, crop=(0, 0, 0, 0), qp=0, chroma=0, deblock=1,
                  bottom=0, delta_bottom=0, redundant=0, poc_lsb=0, idr=0,
                  delta_qp=0, disable=0, alpha=0, beta=0, direct=0, vui=False,
                  cabac=0, profile=66, frame_only=1, first=0, slice_type=7,
                  frame_num=0, groups=0, level=51, hrd=0, second_chroma=None), **p)
    b = Bits(); b.u(8, p['profile']); b.u(8, 0xc0); b.u(8, p['level']); b.ue(p['sid'])
    b.ue(p['fbits'] - 4); b.ue(p['poc'])
    if p['poc'] == 0: b.ue(p['pbits'] - 4)
    b.ue(p['refs']); b.u(1, 0); b.ue(p['w'] // 16 - 1); b.ue(p['h'] // 16 - 1)
    b.u(1, p['frame_only']); b.u(1, p['direct']); b.u(1, any(p['crop']))
    if any(p['crop']):
        for v in p['crop']: b.ue(v)
    b.u(1, p['vui'])
    if p['vui']:
        b.u(1, 1); b.u(8, 255); b.u(16, 1); b.u(16, 1)  # extended SAR
        b.u(1, 0); b.u(1, 1); b.u(3, 5); b.u(1, 1); b.u(1, 1)
        b.u(8, 1); b.u(8, 1); b.u(8, 1)
        b.u(1, 1); b.ue(0); b.ue(0)
        b.u(1, 1); b.u(32, 1001); b.u(32, 60000); b.u(1, 1)
        for flag in (1, 2):
            b.u(1, bool(p['hrd'] & flag))
            if p['hrd'] & flag:
                b.ue(1); b.u(4, 3); b.u(4, 4)
                for value in (0, 0xfffffffe): b.ue(value); b.ue(value); b.u(1, 1)
                for value in (23, 23, 23, 24): b.u(5, value)
        if p['hrd']: b.u(1, 0)
        b.u(1, 0); b.u(1, 1)
        b.u(1, 1)
        for v in (2, 1, 16, 16, 0, p['refs']): b.ue(v)
    sps = nal(7, b.finish())
    b = Bits(); b.ue(p['pid']); b.ue(p['sid']); b.u(1, p['cabac']); b.u(1, p['bottom'])
    b.ue(p['groups']); b.ue(0); b.ue(0); b.u(1, 0); b.u(2, 0)
    b.se(p['qp']); b.se(0); b.se(p['chroma'])
    b.u(1, p['deblock']); b.u(1, 0); b.u(1, p['redundant'])
    if p['second_chroma'] is not None:
        b.u(1, 0); b.u(1, 0); b.se(p['second_chroma'])
    pps = nal(8, b.finish())
    b = Bits(); b.ue(p['first']); b.ue(p['slice_type']); b.ue(p['pid'])
    b.u(p['fbits'], p['frame_num']); b.ue(p['idr'])
    if p['poc'] == 0:
        b.u(p['pbits'], p['poc_lsb'])
        if p['bottom']: b.se(p['delta_bottom'])
    if p['redundant']: b.ue(0)
    b.u(1, 0); b.u(1, 0); b.se(p['delta_qp'])
    if p['deblock']:
        b.ue(p['disable'])
        if p['disable'] != 1: b.se(p['alpha']); b.se(p['beta'])
    header_bits = len(b.b)
    # The parser deliberately leaves actual macroblock syntax to NVDEC. This
    # synthetic payload probes HEADER context only, not successful decoding.
    b.u(16, 0x1234)
    idr = nal(5, b.finish())
    return sps + pps + idr, p, header_bits, (sps, pps, idr)


def main():
    text = HEADER.read_text()
    desc = text.split('typedef struct {', 1)[1].split('} kh264_decode_desc;', 1)[0]
    fields = []
    for kind, names in re.findall(r'\b(unsigned int|int)\s+([^;]+);', desc):
        for item in names.split(','):
            name, count = re.fullmatch(r'\s*(\w+)(?:\[(\d+)\])?\s*', item).groups()
            ty = C.c_uint if kind == 'unsigned int' else C.c_int
            fields.append((name, ty * int(count) if count else ty))
    class Desc(C.Structure): _fields_ = fields
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    code = '#include "' + HEADER.as_posix() + '"\n' + r'''
__declspec(dllexport) int parse(const unsigned char *p,unsigned n,kh264_decode_desc *d){return kh264_parse_annexb(p,n,d);}
__declspec(dllexport) unsigned desc_size(void){return sizeof(kh264_decode_desc);}
__declspec(dllexport) int golomb(const unsigned char *p,unsigned n,unsigned *v){kh264_bits r={.data=p,.bytes=n};*v=kh264_ue(&r);return r.error;}
'''
    count = 0
    with tempfile.TemporaryDirectory(prefix='kestrel-h264-parser-') as tmp, ExitStack() as cleanup:
        c, dll = Path(tmp) / 'test.c', Path(tmp) / 'test.dll'
        c.write_text(code)
        subprocess.run([clang, '-shared', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-trap=all',
                        str(c), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll)); lib.parse.argtypes = [C.c_void_p, C.c_uint, C.POINTER(Desc)]
        cleanup.callback(_ctypes.FreeLibrary, lib._handle)
        lib.parse.restype = C.c_int
        assert lib.desc_size() == C.sizeof(Desc)

        def parse(data, expected=None):
            nonlocal count
            count += 1
            buf = C.create_string_buffer(data); out = Desc(); C.memset(C.byref(out), 0xa5, C.sizeof(out))
            rc = lib.parse(buf, len(data), C.byref(out))
            if expected is not None: assert rc == expected, (rc, expected, data[:40].hex())
            if rc: assert bytes(out) == bytes(C.sizeof(out)), 'stale context leaked after failure'
            return rc, out

        # Real production fixture, not just the parser's own synthetic writer.
        boot = (ROOT / 'tools/h264_iframe_test.h').read_text()
        body = boot.split('h264_iframe_test[]', 1)[1].split('{', 1)[1].split('}', 1)[0]
        boot = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
        _, d = parse(boot, 0)
        assert (d.coded_width, d.coded_height, d.slice_qp, d.max_num_ref_frames) == (64, 64, 26, 1)
        assert d.slice_start == boot.index(b'\0\0\0\1\x65')
        saved = fixture.WIDTH_MBS, fixture.HEIGHT_MBS, fixture.WIDTH, fixture.HEIGHT
        try:
            fixture.WIDTH_MBS = fixture.HEIGHT_MBS = 16; fixture.WIDTH = fixture.HEIGHT = 256
            actual = fixture.nal(3, 7, fixture.sps()) + fixture.nal(3, 8, fixture.pps()) + fixture.nal(3, 5, fixture.idr_slice())
            assert fixture.parse_validate(actual) == [7, 8, 5]
            _, d = parse(actual, 0)
            assert (d.coded_width, d.coded_height, d.slice_end) == (256, 256, len(actual))
        finally: fixture.WIDTH_MBS, fixture.HEIGHT_MBS, fixture.WIDTH, fixture.HEIGHT = saved

        rng = random.Random(0x264)
        positives = []
        for i in range(600):
            w, h = rng.choice((48, 64, 256, 1920, 4096)), rng.choice((64, 256, 1088, 4096))
            crop = (rng.randrange(w // 8), rng.randrange(w // 8), rng.randrange(h // 8), rng.randrange(h // 8))
            qp = rng.randrange(-26, 26); delta = rng.randrange(-26 - qp, 26 - qp)
            poc = rng.choice((0, 2)); pbits = rng.randrange(4, 17)
            data, p, hb, units = stream(w=w, h=h, crop=crop, sid=rng.randrange(32), pid=rng.randrange(256),
                fbits=rng.randrange(4, 17), pbits=pbits, poc=poc, poc_lsb=rng.randrange(1 << pbits) if poc == 0 else 0,
                qp=qp, delta_qp=delta, chroma=rng.randrange(-12, 13), bottom=1, delta_bottom=rng.randrange(-64, 65) if poc == 0 else 0,
                deblock=i % 2, redundant=i % 2, idr=rng.randrange(65536), disable=i % 3,
                alpha=rng.randrange(-6, 7), beta=rng.randrange(-6, 7), direct=i % 2, vui=i % 3 == 0,
                hrd=i % 4, second_chroma=rng.randrange(-12, 13) if i % 5 else None)
            _, d = parse(data, 0); positives.append(data)
            assert (d.coded_width, d.coded_height) == (w, h)
            assert (d.crop_left, d.crop_right, d.crop_top, d.crop_bottom) == tuple(x * 2 for x in crop)
            assert (d.display_width, d.display_height) == (w - 2 * (crop[0] + crop[1]), h - 2 * (crop[2] + crop[3]))
            for field, key in [('sps_id', 'sid'), ('pps_id', 'pid'), ('max_num_ref_frames', 'refs'),
                               ('pic_order_cnt_type', 'poc'), ('pic_init_qp_minus26', 'qp'), ('chroma_qp_index_offset', 'chroma'),
                               ('idr_pic_id', 'idr'), ('pic_order_cnt_lsb', 'poc_lsb'), ('slice_qp_delta', 'delta_qp')]:
                assert getattr(d, field) == p[key], (field, getattr(d, field), p[key])
            assert d.slice_qp == 26 + qp + delta and d.slice_header_bits == hb
            assert d.second_chroma_qp_index_offset == (p['chroma'] if p['second_chroma'] is None else p['second_chroma'])
            assert list(d.field_order_cnt) == [p['poc_lsb'], p['poc_lsb'] + p['delta_bottom']]
            if p['vui']: assert (d.num_units_in_tick, d.time_scale, d.full_range) == (1001, 60000, 1)

        for change in [dict(cabac=1), dict(profile=100), dict(frame_only=0), dict(poc=1),
                       dict(groups=1), dict(first=1), dict(slice_type=0), dict(w=4112)]:
            parse(stream(**change)[0], 3)
        for change in [dict(frame_num=1), dict(refs=17), dict(sid=32), dict(pid=256), dict(fbits=17),
                       dict(pbits=17), dict(idr=65536), dict(qp=26), dict(chroma=13), dict(disable=3),
                       dict(alpha=7), dict(beta=-7), dict(delta_qp=52), dict(crop=(128, 0, 0, 0)), dict(second_chroma=13)]:
            assert parse(stream(**change)[0])[0] != 0, change
        data, _, _, (sps, pps, idr) = stream()
        parse(sps + pps + idr + idr, 3); parse(sps + sps + pps + idr, 3)
        parse(pps + idr, 4); parse(sps + idr, 4); parse(sps + pps, 4)
        parse(sps + pps + nal(1, b'\x80', ref=3), 3)
        parse(data + b'\0\0\1', 2)
        # AUD, SEI, filler and EOS preserve the descriptor and exact slice span.
        b = Bits(); b.u(3, 0); aud = nal(9, b.finish())
        extras = aud + nal(6, b'\x05\x04\x00\x00\x00\x03\x80')
        extended = extras + data + nal(12, b'\xff\xff\x80') + nal(10, b'\x80')
        _, d = parse(extended, 0)
        assert d.slice_start == len(extras) + len(sps) + len(pps) and d.slice_end == len(extras) + len(data)
        parse(data + nal(6, b'\x05\x04\x11\x80'), 2)
        parse(data + b'\0\0\1\x06\x05\x01\x00\x00\x03\x04\x80', 1)
        parse(data + b'\0\0\1\x06\x05\x01\x00\x00\x03', 2)
        parse(b'bad' + data, 1)
        parse(b'\0\0' + data.replace(b'\0\0\0\1', b'\0\0\1') + b'\0\0', 0)
        # All truncations BEFORE complete slice-header/data availability fail.
        for end in range(len(sps) + len(pps) + 5): assert parse(data[:end])[0] != 0
        assert lib.parse(None, 1, C.byref(Desc())) != 0
        assert lib.parse(C.c_void_p(1), 16 * 1024 * 1024 + 1, C.byref(Desc())) == 3
        assert lib.parse(None, 0, None) != 0
        # Independent unsigned Exp-Golomb differential, through maximum bounded
        # codeNum 0xfffffffe. Escape insertion exercises real EBSP reading.
        lib.golomb.argtypes = [C.c_void_p, C.c_uint, C.POINTER(C.c_uint)]
        for v in [0, 1, 2, 3, 0x7fffffff, 0xfffffffe] + [rng.randrange(0xffffffff) for _ in range(5000)]:
            b = Bits(); b.ue(v); payload = nal(6, b.finish())[5:]; out = C.c_uint()
            assert lib.golomb(C.create_string_buffer(payload), len(payload), C.byref(out)) == 0 and out.value == v
        # Bounded malformed/mutation corpus: never crash, never expose partial
        # descriptions on failure. Successful header parses still require real
        # hardware entropy/status checks; mutations in slice data may succeed.
        for i in range(12000):
            if i % 2:
                candidate = bytearray(rng.choice(positives)); candidate[rng.randrange(len(candidate))] ^= 1 << rng.randrange(8)
            else: candidate = bytearray(rng.randbytes(rng.randrange(160)))
            rc, d = parse(bytes(candidate)); assert 0 <= rc <= 4
            if rc == 0:
                assert 0 < d.display_width <= d.coded_width <= 4096
                assert 0 < d.display_height <= d.coded_height <= 4096
                assert 0 <= d.slice_start < d.slice_end <= len(candidate)
                assert 0 <= d.slice_qp <= 51 and d.frame_num == 0
        # Place the final input byte immediately before an inaccessible page.
        # Unlike a padded Python string this faults on even one-byte over-read.
        win = C.WinDLL('kernel32', use_last_error=True)
        win.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_uint]
        win.VirtualAlloc.restype = C.c_void_p
        win.VirtualProtect.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.POINTER(C.c_uint)]
        win.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint]
        base = win.VirtualAlloc(None, 12288, 0x3000, 4)
        assert base
        try:
            old = C.c_uint()
            assert win.VirtualProtect(base + 8192, 4096, 1, C.byref(old))
            guarded = [boot] + [data[:i] for i in range(len(data) + 1)] + positives[:100]
            for candidate in guarded:
                assert len(candidate) <= 8192
                address = base + 8192 - len(candidate)
                C.memmove(address, candidate, len(candidate))
                out = Desc(); rc = lib.parse(address, len(candidate), C.byref(out))
                assert rc == parse(candidate)[0]
        finally: assert win.VirtualFree(base, 0, 0x8000)
        print(f'PASS {count} production H.264 header parser cases + 5006 independent Exp-Golomb comparisons; real 64x64/generated 256x256 IPCM, crop/VUI/POC/QP fields, unsupported formats and bounded fuzz (not NVDEC proof)')


if __name__ == '__main__': main()
