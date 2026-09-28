#!/usr/bin/env python3
"""Generate a Windows NVENC hardware reference, never a native-OS success gate.

Raw input/bitstream are preserved even if the actual Kestrel parser rejects the
stream. Constant-QP baseline is lossy: input.nv12 is NOT an exact decoded oracle.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import ctypes as C
import _ctypes
from contextlib import ExitStack
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from nvshader import find_cl, tool

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'tools/third_party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h'
HEADER_SHA = '4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'out/nvenc-reference-256' / datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    ap.add_argument('--capture-picture-records', action='store_true', help='Capture only exact CFB7 picture records from the helper process itself')
    ap.add_argument('--capture-method-streams', action='store_true', help='Recognize bounded candidate H264 launch sequences in the helper process; not execution proof')
    ap.add_argument('--capture-live-methods', action='store_true', help='Instrument only this helper DLL mapping at the SHA-pinned typed-command EXECUTE boundary')
    ap.add_argument('--neutral-cqp26', action='store_true', help='Capture normal H264 at QP26 with all NV12 samples 128, matching native boot input without forced IPCM')
    ap.add_argument('--mb-count-slice', action='store_true', help='Request SDK macroblock-count mode (0, 256 MBs) instead of slice-count mode (3, 1 slice); neither implies a firmware slice_mode value')
    args = ap.parse_args(); output = args.out.resolve()
    assert output.is_relative_to(ROOT), 'output must stay inside the workspace'
    assert not output.exists(), 'refusing to overwrite any previous reference output'
    assert hashlib.sha256(HEADER.read_bytes()).hexdigest() == HEADER_SHA
    if args.capture_live_methods:
        installed = Path(os.environ['SystemRoot']) / 'System32/nvcuvid.dll'
        assert hashlib.sha256(installed.read_bytes()).hexdigest() == '6014e9cbd0980fd55140056af52574142463431c9b1e232f14e7c929f7313686', 'Unrecognized installed nvcuvid.dll; refusing breakpoint'
    output.mkdir(parents=True)
    env = dict(os.environ); cl = find_cl()
    if cl: env['PATH'] = cl + os.pathsep + env.get('PATH', '')
    parser_header = ROOT / 'include/kestrel/h264_decode.h'
    desc = parser_header.read_text().split('typedef struct {', 1)[1].split('} kh264_decode_desc;', 1)[0]
    fields = []
    for kind, names in re.findall(r'\b(unsigned int|int)\s+([^;]+);', desc):
        for item in names.split(','):
            name, count = re.fullmatch(r'\s*(\w+)(?:\[(\d+)\])?\s*', item).groups()
            ty = C.c_uint if kind == 'unsigned int' else C.c_int
            fields.append((name, ty * int(count) if count else ty))
    class Desc(C.Structure): _fields_ = fields
    with tempfile.TemporaryDirectory(prefix='kestrel-nvenc-ref-') as tmp, ExitStack() as cleanup:
        exe = Path(tmp) / 'encode.exe'
        subprocess.run([tool('nvcc'), '-arch=sm_120', '-O2', '-o', str(exe),
                        str(ROOT / 'tools/generate_nvenc_h264_fixture.cu'), '-lcuda'],
                       check=True, env=env, cwd=tmp, timeout=120)
        result = subprocess.run([str(exe), str(output)] + (['--capture-picture-records'] if args.capture_picture_records else []) +
                                (['--capture-method-streams'] if args.capture_method_streams else []) +
                                (['--capture-live-methods'] if args.capture_live_methods else [])+
                                (['--neutral-cqp26'] if args.neutral_cqp26 else [])+
                                (['--mb-count-slice'] if args.mb_count_slice else []), capture_output=True, text=True, timeout=60)
        (output / 'encoder.stdout.txt').write_text(result.stdout)
        (output / 'encoder.stderr.txt').write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(f'Windows hardware encoder failed ({result.returncode}); raw evidence retained at {output}: {result.stderr}')
        hardware = json.loads(result.stdout.strip())
        data = (output / 'frame.h264').read_bytes(); pixels = (output / 'input.nv12').read_bytes()
        expected = bytearray([128] * (256 * 256 * 3 // 2))
        if not args.neutral_cqp26:
            for y in range(256):
                for x in range(256): expected[y * 256 + x] = 16 + (x // 2 + y // 3 + ((x // 32 + y // 32) & 1) * 48) % 220
            for y in range(128):
                for x in range(256):
                    expected[65536 + y * 256 + x] = 48 + ((255 - x // 2 + y * 3) if x & 1 else (x // 2 + y * 2)) % 160
        assert pixels == expected, 'CUDA pattern differs from independent CPU reference'
        c, dll = Path(tmp) / 'parse.c', Path(tmp) / 'parse.dll'
        c.write_text('#include "' + parser_header.as_posix() + '"\n' +
                     '__declspec(dllexport) int parse(const unsigned char *p,unsigned n,kh264_decode_desc *d){return kh264_parse_annexb(p,n,d);}\n')
        clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
        subprocess.run([clang, '-shared', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-trap=all', str(c), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll)); cleanup.callback(_ctypes.FreeLibrary, lib._handle)
        lib.parse.argtypes = [C.c_void_p, C.c_uint, C.POINTER(Desc)]
        buffer = C.create_string_buffer(data); d = Desc(); rc = lib.parse(buffer, len(data), C.byref(d))
        parsed = {name: list(getattr(d, name)) if isinstance(getattr(d, name), C.Array) else getattr(d, name) for name, _ in fields}
        manifest = {'kind': 'Windows NVIDIA NVENC reference; NOT native Kestrel encode/decode proof',
                    'created_utc': datetime.now(timezone.utc).isoformat(), 'hardware': hardware,
                    'config': {'width': 256, 'height': 256, 'format': 'NV12', 'profile': 'baseline',
                                'entropy': 'CAVLC', 'qp': 26 if args.neutral_cqp26 else 18,
                                'input_pattern': 'neutral-128' if args.neutral_cqp26 else 'ramp-checker-uv',
                                'sdk_slice_mode': 0 if args.mb_count_slice else 3,
                                'sdk_slice_mode_data': 256 if args.mb_count_slice else 1,
                                'slices': 1, 'preset': 'P1', 'tuning': 'low latency'},
                    'raw_output_unmodified': True, 'input_generated_by': 'CUDA sm_120 pattern kernel',
                    'warning': 'Lossy stream: input.nv12 is not an exact decoded pixel oracle.',
                    'header_sha256': HEADER_SHA,
                    'picture_capture': {
                        'enabled': args.capture_picture_records,
                        'scope': 'Matching CFB7 records from this helper process only; auxiliary files require a validated upload slice',
                        'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob('cfb7-picture-*.bin'))}},
                    'method_capture': {
                        'enabled': args.capture_method_streams,
                        'scope': 'Candidate decoded launch methods from this helper only; no GPU BAR/executable pages; not execution proof',
                        'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob('encoder-methods-*.json'))}},
                    'live_method_capture': {
                        'enabled': args.capture_live_methods,
                        'scope': 'Self-process typed command list before EXECUTE; not raw GPFIFO or native proof',
                        'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in
                                    list(output.glob('live-encoder-tokens.json'))+list(output.glob('completed-rc-*-prefix.bin'))+
                                    list(output.glob('rc-initializer-*.bin'))}},
                    'parser_status': rc, 'parsed': parsed,
                    'sha256': {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                               for name in ('frame.h264', 'input.nv12')},
                    'source_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in (ROOT / 'tools/generate_nvenc_h264_fixture.cu', ROOT / 'tools/nvenc_method_capture.h', ROOT / 'tools/nvenc_live_capture.h', parser_header)}}
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(json.dumps({'output': str(output), 'hardware': hardware, 'parser_status': rc, 'parsed': parsed}, indent=2))
        assert rc == 0, 'Raw NVENC stream rejected by current Kestrel parser; retained unchanged for investigation'
        assert (d.coded_width, d.coded_height, d.profile_idc) == (256, 256, 66)


if __name__ == '__main__': main()
