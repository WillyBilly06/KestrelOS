#!/usr/bin/env python3
"""Decode one real 256x256 IDR using installed Windows NVDEC, never a CPU codec.
Preserves the compressed source and writes a fresh exact, pitch-linear NV12
oracle plus API/configuration provenance. Not proof of native Kestrel decoding.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import ctypes as C
import _ctypes
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from nvshader import find_cl, tool

ROOT = Path(__file__).resolve().parents[1]
PINNED = {
    'dynlink_cuviddec.h': '1244165ea5521d4be865995eb1c49b8910f755d0ca35004e1c4010ef7f612143',
    'dynlink_nvcuvid.h': '2141dade75f3178520092dce2c02df9b47c4b19036b56bb9316bd83a07bf16f1',
}


def compare_kestrel(tmp, source, captured):
    """Compile the actual native context builder using its Linux bitfield ABI."""
    names = ['PicWidthInMbs', 'FrameHeightInMbs', 'log2_max_frame_num_minus4',
             'pic_order_cnt_type', 'log2_max_pic_order_cnt_lsb_minus4', 'delta_pic_order_always_zero_flag',
             'frame_mbs_only_flag', 'direct_8x8_inference_flag', 'entropy_coding_mode_flag',
             'pic_order_present_flag', 'num_ref_idx_l0_active_minus1', 'num_ref_idx_l1_active_minus1',
             'weighted_pred_flag', 'weighted_bipred_idc', 'pic_init_qp_minus26',
             'deblocking_filter_control_present_flag', 'redundant_pic_cnt_present_flag',
             'transform_8x8_mode_flag', 'MbaffFrameFlag', 'constrained_intra_pred_flag',
             'chroma_qp_index_offset', 'second_chroma_qp_index_offset', 'ref_pic_flag', 'frame_num',
             'field_pic_flag', 'bottom_field_flag', 'second_field', 'qpprime_y_zero_transform_bypass_flag']
    desc_names = ['coded_width', 'coded_height', 'display_width', 'display_height',
                  'max_num_ref_frames', 'slice_start', 'slice_end', 'slice_header_bits']
    code = ''.join('#include "' + (ROOT / ('kernel/' + name)).as_posix() + '"\n'
                   for name in ('nvdec_h264_context.h', 'nvdec_h264_job.h'))
    code += '__declspec(dllexport) int context(const unsigned char *s,unsigned n,int *v){\n'
    code += 'kh264_decode_desc d;nvdec_h264_pic_s p;nvdec_h264_job_layout j;\n'
    code += 'int rc=kh264_parse_annexb(s,n,&d);if(rc)return rc;rc=nvdec_h264_job_plan(&j,&d,n);if(rc)return rc;\n'
    code += 'rc=nvdec_h264_parse_pic(&p,&d,s,n,j.pitch,j.pitch,j.history_bytes,j.mbhist_bytes);if(rc)return rc;\n'
    code += ''.join(f'*v++=(int)p.{name};\n' for name in names)
    code += ''.join(f'*v++=(int)d.{name};\n' for name in desc_names)
    code += '*v++=p.CurrFieldOrderCnt[0];*v++=p.CurrFieldOrderCnt[1];\n'
    code += 'for(unsigned i=0;i<16;i++){*v++=p.dpb[i].state;*v++=p.dpb[i].top_field_marking;*v++=p.dpb[i].bottom_field_marking;}\n'
    code += 'for(unsigned i=0;i<96;i++)*v++=p.WeightScale[i/16][i%16/4][i%4];\n'
    code += 'for(unsigned i=0;i<128;i++)*v++=p.WeightScale8x8[i/64][i%64/8][i%8];return 0;}\n'
    c, obj, dll = (Path(tmp) / ('context.' + ext) for ext in ('c', 'obj', 'dll')); c.write_text(code)
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-mno-ms-bitfields', '-ffreestanding',
                    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fsanitize-trap=all',
                    '-c', str(c), '-o', str(obj)], check=True)
    subprocess.run([clang, '-shared', str(obj), '-o', str(dll)], check=True)
    lib = C.CDLL(str(dll))
    try:
        lib.context.argtypes = [C.c_void_p, C.c_uint, C.POINTER(C.c_int)]
        values = (C.c_int * (len(names) + len(desc_names) + 2 + 48 + 96 + 128))()
        data = C.create_string_buffer(source); assert lib.context(data, len(source), values) == 0
    finally: _ctypes.FreeLibrary(lib._handle)
    at = len(names); fields = dict(zip(names, values[:at])); desc = dict(zip(desc_names, values[at:at + len(desc_names)])); at += len(desc_names)
    fields['CurrFieldOrderCnt'] = list(values[at:at + 2]); at += 2
    dpb = [dict(zip(('state', 'top_field_marking', 'bottom_field_marking'), values[at + i * 3:at + i * 3 + 3])) for i in range(16)]; at += 48
    fields['WeightScale4x4'] = list(values[at:at + 96]); at += 96
    fields['WeightScale8x8'] = list(values[at:at + 128])
    mismatches = {name: {'kestrel': value, 'cuvid': captured[name]} for name, value in fields.items() if value != captured[name]}
    if desc['max_num_ref_frames'] != captured['num_ref_frames']:
        mismatches['reference_capacity'] = {'kestrel': desc['max_num_ref_frames'], 'cuvid': captured['num_ref_frames']}
    return {'fields': fields, 'descriptor': desc, 'dpb': dpb, 'mismatches': mismatches,
            'cuvid_used_dpb_entries': sum(p['used_for_reference'] != 0 for p in captured['dpb']),
            'note': 'Unused CUVID DPB PicIdx values and native DPB bitfields are different APIs; raw entries retained, not assumed byte-identical.'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, default=ROOT / 'out/nvenc-reference-256/20260909-090552/frame.h264')
    ap.add_argument('--out', type=Path, default=ROOT / 'out/nvdec-reference-256' / datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S'))
    args = ap.parse_args(); source, out = args.input.resolve(), args.out.resolve()
    assert source.is_file() and source.is_relative_to(ROOT) and out.is_relative_to(ROOT)
    assert not out.exists(), 'refusing to overwrite a previous decode oracle'
    for name, expected in PINNED.items():
        assert hashlib.sha256((ROOT / 'tools/third_party/nv-codec-headers/include/ffnvcodec' / name).read_bytes()).hexdigest() == expected
    original = source.read_bytes(); assert 0 < len(original) <= 16 * 1024 * 1024
    out.mkdir(parents=True); env = dict(os.environ); cl = find_cl()
    if cl: env['PATH'] = cl + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='kestrel-nvdec-ref-') as tmp:
        exe = Path(tmp) / 'decode.exe'
        subprocess.run([tool('nvcc'), '-arch=sm_120', '-O2', '-o', str(exe),
                        str(ROOT / 'tools/decode_nvdec_reference.cu'), '-lcuda'],
                       check=True, env=env, cwd=tmp, timeout=120)
        result = subprocess.run([str(exe), str(source), str(out / 'decoded.nv12'), str(out / 'parser-bitstream.h264')], capture_output=True, text=True, timeout=60)
        (out / 'decoder.stdout.txt').write_text(result.stdout); (out / 'decoder.stderr.txt').write_text(result.stderr)
        assert source.read_bytes() == original, 'compressed source changed during reference decode'
        if result.returncode: raise RuntimeError(f'Windows NVDEC failed ({result.returncode}): {result.stderr}; evidence retained in {out}')
        hardware = json.loads(result.stdout); decoded = (out / 'decoded.nv12').read_bytes()
        assert len(decoded) == 98304 and hardware['decode_status'] == 2 and hardware['pictures'] == hardware['displays'] == 1
        assert all(call['status'] == 0 for call in hardware['api_calls'])
        assert len(set(decoded[:65536])) > 32 and len(set(decoded[65536:])) > 32, 'unexpected uniform oracle'
        comparison = compare_kestrel(tmp, original, hardware['cuvid_picture'])
        parser_stream = (out / 'parser-bitstream.h264').read_bytes()
        descriptor = comparison['descriptor']; native_slice = original[descriptor['slice_start']:descriptor['slice_end']]
        parser_transport = {'bytes': len(parser_stream), 'sha256': hashlib.sha256(parser_stream).hexdigest(),
                            'slice_offsets': hardware['cuvid_picture']['slice_offsets'],
                            'equals_kestrel_slice_bytes': parser_stream == native_slice,
                            'kestrel_slice_bytes': len(native_slice),
                            'first_32_bytes': parser_stream[:32].hex(), 'last_32_bytes': parser_stream[-32:].hex()}
        manifest = {'kind': 'Exact Windows NVDEC decoded oracle; NOT native Kestrel proof',
                    'created_utc': datetime.now(timezone.utc).isoformat(), 'source': str(source),
                    'source_sha256': hashlib.sha256(original).hexdigest(),
                    'decoded_sha256': hashlib.sha256(decoded).hexdigest(),
                    'plane_sha256': {'Y': hashlib.sha256(decoded[:65536]).hexdigest(), 'UV': hashlib.sha256(decoded[65536:]).hexdigest()},
                    'hardware': hardware, 'configuration': {'codec': 'H264', 'format': 'NV12', 'bit_depth': 8,
                        'creation_flags': 'cudaVideoCreate_PreferCUVID=4', 'deinterlace': 'Weave (progressive)',
                        'crop': [0, 0, 256, 256], 'target': [0, 0, 256, 256], 'scaling': False,
                        'intra_only': True, 'output_surfaces': 1, 'parser_error_threshold': 0},
                    'header_sha256': PINNED,
                    'kestrel_context_comparison': comparison, 'parser_transport': parser_transport,
                    'tool_sha256': {name: hashlib.sha256((ROOT / 'tools' / name).read_bytes()).hexdigest()
                                    for name in ('decode_nvdec_reference.py', 'decode_nvdec_reference.cu')},
                    'runtime_sha256': hashlib.sha256(Path('C:/Windows/System32/nvcuvid.dll').read_bytes()).hexdigest()}
        source_pixels = source.with_name('input.nv12')
        if source_pixels.is_file():
            pixels = source_pixels.read_bytes()
            if len(pixels) == len(decoded):
                differences = [abs(a - b) for a, b in zip(pixels, decoded)]
                manifest['lossy_source_comparison'] = {'different_bytes': sum(x != 0 for x in differences),
                    'max_absolute_difference': max(differences), 'mean_absolute_difference': sum(differences) / len(differences)}
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(json.dumps({'output': str(out), 'source_sha256': manifest['source_sha256'], 'decoded_sha256': manifest['decoded_sha256'],
                          'decode_status': hardware['decode_status'], 'parser_transport': parser_transport,
                          'kestrel_context_mismatches': comparison['mismatches'],
                          'lossy_source_comparison': manifest.get('lossy_source_comparison')}, indent=2))


if __name__ == '__main__': main()
