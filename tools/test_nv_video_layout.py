#!/usr/bin/env python3
"""Check production Blackwell NV12 layout transport against NIL sector lists.
No GPU/USB access. CPU conversion correctness is not codec execution proof.
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

ROOT = Path(__file__).resolve().parents[1]


class Layout(C.Structure):
    _fields_ = [(x, C.c_uint) for x in ('format', 'width', 'height', 'y_pitch', 'uv_pitch', 'y_rows', 'uv_rows')] + [
        (x, C.c_ulonglong) for x in ('y_bytes', 'uv_bytes', 'uv_offset', 'bytes')]


def main():
    # Independent address-to-coordinate tables, using the actual reference's
    # GOB line list. This is intentionally not the production coordinate formula.
    copy = (ROOT / 'out/mesa-ref/src/nouveau/nil/copy.rs').read_text()
    ref16 = copy.split('impl<C: CopyBytes> CopyGOBLines for CopyGOBBlackwell2D2BPP<C>', 1)[1].split('struct CopyGOBBlackwell2D1BPP', 1)[0]
    lines = re.findall(r'f\(i \* 0x100 \+ (0x[0-9a-f]+), i \* 32 \+ (\d+), (\d+), 0\);', ref16)
    assert len(lines) == 16
    ref8 = copy.split('impl<C: CopyBytes> CopyGOBLines for CopyGOBBlackwell2D1BPP<C>', 1)[1].split('fn aligned_range', 1)[0]
    assert 'f(x * 0x40 + y * 0x8, x * 0x8, y, 0);' in ref8
    maps = [dict(), dict()]
    for x in range(8):
        for y in range(8):
            for b in range(8): maps[0][x * 64 + y * 8 + b] = (x * 8 + b, y)
    for column in range(2):
        for off, x, y in lines:
            for b in range(16): maps[1][column * 256 + int(off, 16) + b] = (column * 32 + int(x) + b, int(y))
    for table in maps:
        assert set(table) == set(range(512))
        assert set(table.values()) == {(x, y) for y in range(8) for x in range(64)}
    assert maps[0] != maps[1]
    inverse = [{xy: off for off, xy in table.items()} for table in maps]

    def enumerate_offsets(l, plane):
        pitch, rows, start = (l.uv_pitch, l.uv_rows, l.uv_offset) if plane else (l.y_pitch, l.y_rows, 0)
        at = start
        for row in range(0, rows, 16):
            for col in range(0, pitch, 64):
                for gob in range(2):
                    for byte in range(512):
                        x, y = maps[plane][byte]
                        yield at + byte, col + x, row + gob * 8 + y
                    at += 512
        assert at == start + pitch * rows

    header = ROOT / 'kernel/nv_video_layout.h'
    code = '#include "' + header.as_posix() + '"\n' + r'''
__declspec(dllexport) int init(nv_video_nv12_layout_s *l,unsigned f,unsigned w,unsigned h,unsigned yp,unsigned up,unsigned yr,unsigned ur){return nv_video_nv12_layout_init(l,f,w,h,yp,up,yr,ur);}
__declspec(dllexport) int offset(const nv_video_nv12_layout_s *l,unsigned p,unsigned x,unsigned y,nv_video_size *o){return nv_video_nv12_offset(l,p,x,y,o);}
__declspec(dllexport) int convert(void *d,nv_video_size dn,const nv_video_nv12_layout_s *dl,const void *s,nv_video_size sn,const nv_video_nv12_layout_s *sl){return nv_video_nv12_convert(d,dn,dl,s,sn,sl);}
__declspec(dllexport) unsigned layout_size(void){return sizeof(nv_video_nv12_layout_s);}
'''
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    calls, compared = 0, 0
    with tempfile.TemporaryDirectory(prefix='kestrel-video-layout-') as tmp, ExitStack() as cleanup:
        c, dll = Path(tmp) / 'test.c', Path(tmp) / 'test.dll'; c.write_text(code)
        subprocess.run([clang, '-shared', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-trap=all', str(c), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll)); cleanup.callback(_ctypes.FreeLibrary, lib._handle)
        lib.init.argtypes = [C.POINTER(Layout)] + [C.c_uint] * 7
        lib.offset.argtypes = [C.POINTER(Layout)] + [C.c_uint] * 3 + [C.POINTER(C.c_ulonglong)]
        lib.convert.argtypes = [C.c_void_p, C.c_ulonglong, C.POINTER(Layout), C.c_void_p, C.c_ulonglong, C.POINTER(Layout)]
        assert lib.layout_size() == C.sizeof(Layout)

        def layout(f, w, h, yp=0, up=0, yr=0, ur=0):
            nonlocal calls
            calls += 1; l = Layout(); assert lib.init(C.byref(l), f, w, h, yp, up, yr, ur)
            return l

        def convert(dst, dl, src, sl, dn=None, sn=None, ok=1):
            nonlocal calls
            calls += 1
            rc = lib.convert(dst, dl.bytes if dn is None else dn, C.byref(dl), src, sl.bytes if sn is None else sn, C.byref(sl))
            assert rc == ok

        rng = random.Random(0xb1ac8)
        cases = [(w, h) for w in (2, 16, 48, 62, 64, 66, 126, 128, 130, 256)
                 for h in (2, 14, 16, 18, 30, 32, 34, 64)] + [(256, 258), (1920, 1088)]
        for index, (w, h) in enumerate(cases):
            sl = layout(0, w, h, w + index % 7, w + index % 5, h + index % 3, h // 2 + index % 4)
            pitch = (w + 63) & ~63
            bl = layout(1, w, h, pitch + 64 * (index % 2), pitch + 64 * (index % 3),
                        ((h + 15) & ~15) + 16 * (index % 2), ((h // 2 + 15) & ~15) + 16 * (index % 3))
            source = bytearray([0xa7] * sl.bytes)
            for plane in range(2):
                pitch_s, base = (sl.uv_pitch, sl.uv_offset) if plane else (sl.y_pitch, 0)
                for y in range(h // 2 if plane else h):
                    for x in range(w): source[base + y * pitch_s + x] = (x * 17 + y * 71 + (x // 7) * 23 + plane * 113) & 255
            expected = bytearray(bl.bytes)
            for plane in range(2):
                ps, sb = (sl.uv_pitch, sl.uv_offset) if plane else (sl.y_pitch, 0)
                offsets = []
                for offset, x, y in enumerate_offsets(bl, plane):
                    offsets.append(offset)
                    if x < w and y < (h // 2 if plane else h): expected[offset] = source[sb + y * ps + x]
                    if index < 10:
                        got = C.c_ulonglong(0xffffffffffffffff)
                        assert lib.offset(C.byref(bl), plane, x, y, C.byref(got)) and got.value == offset
                assert len(set(offsets)) == len(offsets)  # bijection over every padded plane
            sbuf = C.create_string_buffer(bytes(source)); before = bytes(sbuf)
            guard = 64; dbuf = C.create_string_buffer(bytes([0xd3]) * (bl.bytes + guard * 2))
            convert(C.addressof(dbuf) + guard, bl, sbuf, sl)
            actual = bytes(dbuf)
            assert actual[guard:guard + bl.bytes] == expected and bytes(sbuf) == before
            assert actual[:guard] == bytes([0xd3]) * guard and actual[guard + bl.bytes:] == bytes([0xd3]) * guard + b'\0'
            compared += bl.bytes
            # Different linear strides on the reverse transfer; all padding is
            # zero, not leaked from either input padding or previous contents.
            outl = layout(0, w, h, w + 3, w + 9, h + 2, h // 2 + 1)
            linear = C.create_string_buffer(bytes([0xee]) * outl.bytes)
            convert(linear, outl, C.addressof(dbuf) + guard, bl)
            reference = bytearray(outl.bytes)
            for plane in range(2):
                ps, ss = (sl.uv_pitch, sl.uv_offset) if plane else (sl.y_pitch, 0)
                pd, dd = (outl.uv_pitch, outl.uv_offset) if plane else (outl.y_pitch, 0)
                for y in range(h // 2 if plane else h): reference[dd + y * pd:dd + y * pd + w] = source[ss + y * ps:ss + y * ps + w]
            assert bytes(linear)[:-1] == reference; compared += outl.bytes

        # Invalid geometry and 64-bit combined-plane overflow fail cleanly.
        for args in [(2, 64, 64, 0, 0, 0, 0), (1, 0, 64, 0, 0, 0, 0), (1, 63, 64, 0, 0, 0, 0),
                     (1, 64, 63, 0, 0, 0, 0), (1, 64, 64, 65, 64, 64, 32), (1, 64, 64, 64, 64, 65, 32),
                     (1, 66, 64, 64, 128, 64, 32), (1, 64, 64, 64, 64, 48, 32),
                     (1, 0xfffffffe, 64, 0, 0, 0, 0), (1, 64, 0xfffffffe, 0, 0, 0, 0),
                     (0, 0xfffffffe, 0xfffffffe, 0, 0, 0, 0)]:
            l = Layout(); C.memset(C.byref(l), 0xa5, C.sizeof(l)); assert not lib.init(C.byref(l), *args)
            assert not any(getattr(l, name) for name, _ in l._fields_)
        # Model size arithmetic with unbounded Python integers, especially near
        # u32 alignment and u64 sum limits; no huge allocations are attempted.
        for i in range(3000):
            f = i % 2; w = rng.randrange(1, 1 << 31) * 2; h = rng.randrange(1, 1 << 31) * 2
            xa, ya = (64, 16) if f else (1, 1)
            pitch = (w + xa - 1) // xa * xa; yr = (h + ya - 1) // ya * ya; ur = (h // 2 + ya - 1) // ya * ya
            total = pitch * (yr + ur); valid = max(pitch, yr, ur) <= 0xffffffff and total <= 0xffffffffffffffff
            l = Layout(); assert bool(lib.init(C.byref(l), f, w, h, 0, 0, 0, 0)) == valid
            if valid:
                assert l.bytes == total and l.uv_offset == pitch * yr
                for plane, rows in [(0, yr), (1, ur)]:
                    x, y = pitch - 1, rows - 1
                    base = l.uv_offset if plane else 0
                    expected = base + (y * pitch + x if f == 0 else
                        ((y // 16) * (pitch // 64) + x // 64) * 1024 + (y // 8 % 2) * 512 + inverse[plane][x % 64, y % 8])
                    off = C.c_ulonglong(); assert lib.offset(C.byref(l), plane, x, y, C.byref(off)) and off.value == expected

        sl, bl = layout(0, 64, 32), layout(1, 64, 32)
        src = C.create_string_buffer(bytes([0x57]) * sl.bytes)
        dst = C.create_string_buffer(bytes([0xa5]) * bl.bytes); original = bytes(dst)
        for dn, sn in [(bl.bytes - 1, sl.bytes), (bl.bytes, sl.bytes - 1), (0, 0)]:
            convert(dst, bl, src, sl, dn, sn, 0); assert bytes(dst) == original
        for field, _ in bl._fields_:
            bad = Layout.from_buffer_copy(bl); setattr(bad, field, getattr(bad, field) ^ 1)
            convert(dst, bad, src, sl, ok=0); assert bytes(dst) == original
        for field in ('y_bytes', 'uv_bytes', 'uv_offset', 'bytes', 'width', 'height'):
            bad = Layout.from_buffer_copy(sl); setattr(bad, field, getattr(bad, field) ^ 1)
            convert(dst, bl, src, bad, ok=0); assert bytes(dst) == original
        shared = C.create_string_buffer(bytes([0x81]) * (sl.bytes * 4)); shared_before = bytes(shared)
        for delta in (0, 1, 16, sl.bytes - 1):
            convert(C.addressof(shared) + delta, bl, shared, sl, ok=0)
            convert(shared, bl, C.addressof(shared) + delta, sl, ok=0)
            assert bytes(shared) == shared_before
        # Exactly touching ranges do not alias.
        convert(C.addressof(shared) + sl.bytes, bl, shared, sl)
        convert(None, bl, src, sl, ok=0); convert(dst, bl, None, sl, ok=0)
        convert(C.c_void_p(0xfffffffffffffff0), bl, src, sl, ok=0)
        convert(dst, bl, C.c_void_p(0xfffffffffffffff0), sl, ok=0)
        assert bytes(dst) == original
        for plane, x, y in [(2, 0, 0), (0, bl.y_pitch, 0), (1, 0, bl.uv_rows)]:
            out = C.c_ulonglong(123); assert not lib.offset(C.byref(bl), plane, x, y, C.byref(out)) and out.value == 0

        # Real inaccessible pages after both allocations catch read/write
        # overruns that an ordinary sentinel buffer alone cannot detect.
        win = C.WinDLL('kernel32', use_last_error=True)
        win.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.c_uint]; win.VirtualAlloc.restype = C.c_void_p
        win.VirtualProtect.argtypes = [C.c_void_p, C.c_size_t, C.c_uint, C.POINTER(C.c_uint)]
        win.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint]
        guarded_sl, guarded_bl = layout(0, 66, 18), layout(1, 66, 18)
        allocations = []
        try:
            pointers = []
            for lay in (guarded_sl, guarded_bl):
                rounded = (lay.bytes + 4095) // 4096 * 4096
                base = win.VirtualAlloc(None, rounded + 4096, 0x3000, 4); assert base; allocations.append(base)
                old = C.c_uint(); assert win.VirtualProtect(base + rounded, 4096, 1, C.byref(old))
                pointers.append(base + rounded - lay.bytes)
            payload = rng.randbytes(guarded_sl.bytes); C.memmove(pointers[0], payload, len(payload))
            convert(pointers[1], guarded_bl, pointers[0], guarded_sl)
            C.memset(pointers[0], 0xa5, guarded_sl.bytes)
            convert(pointers[0], guarded_sl, pointers[1], guarded_bl)
            assert C.string_at(pointers[0], guarded_sl.bytes) == payload
        finally:
            for base in allocations: assert win.VirtualFree(base, 0, 0x8000)

        print(f'PASS Blackwell NV12 Y/UV distinct GOB mappings, {len(cases)} padded nonuniform bidirectional layouts, {compared} compared bytes, 3000 wide size cases, alias/invalid/guard checks; CPU data transport, not codec proof')


if __name__ == '__main__': main()
