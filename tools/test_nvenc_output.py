#!/usr/bin/env python3
"""Production Annex-B framing helper vs an independent regex reference.

This validates bounded single-IDR framing only, not H.264 syntax or decode.
The actual firmware output still requires native NVDEC round-trip validation.
"""
from pathlib import Path
import ctypes
import random
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CAPACITY = 0x40000


def reference(data):
    if not 5 <= len(data) <= CAPACITY:
        return None
    starts = list(re.finditer(b"\x00\x00(?:\x00)?\x01", data))
    if not starts or any(data[:starts[0].start()]):
        return None
    result = None
    for index, match in enumerate(starts):
        end = starts[index+1].start() if index+1 < len(starts) else len(data)
        if end - match.end() < 2:
            return None
        header = data[match.end()]
        kind = header & 31
        if header & 128 or kind not in (5, 6, 7, 8, 9, 10, 11, 12):
            return None
        if kind == 5:
            if result is not None or not header & 96:
                return None
            result = (match.start(), end)
    return result


def main():
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-nvenc-output-") as tmp:
        tmp = Path(tmp)
        source = '#include "' + (ROOT / 'kernel/nvenc_h264_output.h').as_posix() + '"\n'
        source += '''__declspec(dllexport) int check_idr(const unsigned char *p,
            unsigned n, unsigned *start, unsigned *end) {
            return nvenc_h264_single_idr(p,n,start,end);
        }\n'''
        (tmp / 'check.c').write_text(source)
        subprocess.run([clang, '-shared', '-std=c11', '-O1', '-Wall', '-Wextra',
                        str(tmp / 'check.c'), '-o', str(tmp / 'check.dll')], check=True)
        lib = ctypes.CDLL(str(tmp / 'check.dll'))
        check = lib.check_idr
        check.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                          ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        check.restype = ctypes.c_int
        tested = 0

        def compare(data):
            nonlocal tested
            storage = ctypes.create_string_buffer(data)
            start, end = ctypes.c_uint(0xdead), ctypes.c_uint(0xbeef)
            result = check(storage, len(data), ctypes.byref(start), ctypes.byref(end))
            actual = (start.value, end.value) if result else None
            assert actual == reference(data), (data[:100], actual, reference(data))
            if not result:
                assert start.value == end.value == 0
            tested += 1

        nal = b'\0\0\0\1\x65\x88\x80'
        for n in range(len(nal)+1):
            compare(nal[:n])
        for prefix in (b'\0\0\1', b'\0\0\0\1'):
            for header in range(256):
                compare(prefix + bytes([header]) + b'\x80')
            for zeros in range(8):
                compare(b'\0' * zeros + prefix + b'\x65\x88\x80' + b'\0' * zeros)
        for tail in (b'', b'\0\0\1', b'\0\0\1\x67', b'\0\0\1\x67\x80',
                     b'\0\0\1\x61\x80', nal, b'\0\0\3\1\x80'):
            compare(nal + tail)
        compare(b'garbage' + nal)
        compare(b'\0\0\1\x06' + b'\x55' * 4096 + nal)
        compare(nal + b'\x55' * (CAPACITY-len(nal)))
        compare(nal + b'\x55' * (CAPACITY-len(nal)+1))
        for length in (0, 5, CAPACITY, 0xffffffff):
            start, end = ctypes.c_uint(1), ctypes.c_uint(1)
            assert not check(None, length, ctypes.byref(start), ctypes.byref(end))
            assert not start.value and not end.value
        value = ctypes.c_uint(7)
        assert not check(nal, len(nal), None, ctypes.byref(value))
        assert not check(nal, len(nal), ctypes.byref(value), None)

        rng = random.Random(0xCFB7)
        for _ in range(10000):
            stream = bytearray(b'\0' * rng.randrange(5))
            for _ in range(rng.randrange(1, 6)):
                stream += rng.choice((b'\0\0\1', b'\0\0\0\1'))
                stream.append(rng.choice((5, 0x65, 6, 7, 8, 9, 10, 12, 0xe5, 0x61, 0x74)))
                stream += bytes(rng.choice((0, 0, 1, 2, 3, 0x80, 0x55, 0xff)) for _ in range(rng.randrange(24)))
            if rng.randrange(3) == 0:
                del stream[rng.randrange(len(stream)):]
            compare(bytes(stream))
        print(f'PASS {tested} production single-IDR framing comparisons, exact capacity, prefixes, escapes, truncation, extra VCL and null inputs (not decode proof)')
        # Windows cannot delete a DLL while ctypes still holds its handle.
        import _ctypes
        _ctypes.FreeLibrary(lib._handle)


if __name__ == '__main__':
    main()
