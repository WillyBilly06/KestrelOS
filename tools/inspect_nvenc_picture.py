#!/usr/bin/env python3
"""Decode captured, owned CFB7 picture/control records using the published ABI.

Read-only source evidence. Does not submit GPU work or imply native success.
"""
from pathlib import Path
import argparse
import ctypes as C
import _ctypes
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('picture', type=Path)
    ap.add_argument('--nonzero', action='store_true')
    args = ap.parse_args()
    if not args.picture.is_file():
        ap.error(f'picture capture does not exist: {args.picture}')
    clang = r'C:\Program Files\LLVM\bin\clang.exe'
    include = '#include "' + (ROOT / 'kernel/nvenc_drv_h264.h').as_posix() + '"\n'
    # Expand conditional ABI members first; parsing both sides of #if would
    # wrongly enumerate arrays from a different hardware-generation layout.
    source = subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-mno-ms-bitfields', '-E', '-P', '-x', 'c', '-'], input=include, capture_output=True, text=True, check=True).stdout
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    structs = dict((name, body) for body, name in re.findall(r'typedef\s+struct\s*\{([^{}]*)\}\s*(\w+)\s*;', source))

    def members(ty, prefix=''):
        result = []
        for decl in structs[ty].split(';'):
            decl = re.sub(r'^\s*#.*$', '', decl, flags=re.M).strip()
            if not decl:
                continue
            m = re.fullmatch(r'([\w\s]+?)\s+(\w+)\s*((?:\[\w+\]\s*)*)(?:\s*:\s*\d+)?', decl)
            if not m:
                raise ValueError(decl)
            kind, name, arrays = m.groups()
            dims = [int(x, 0) for x in re.findall(r'\[(0x[0-9a-fA-F]+|\d+)\]', arrays)]
            if arrays and not dims:
                raise ValueError(decl)
            paths = [prefix + name]
            for n in dims:
                paths = [p + f'[{i}]' for p in paths for i in range(n)]
            for path in paths:
                if kind in structs:
                    result.extend(members(kind, path + '.'))
                else:
                    result.append(path)
        return result

    kinds = {'': 'nvenc_cfb7_h264_drv_pic_setup_s', '-slice': 'nvenc_h264_slice_control_s',
             '-me': 'nvenc_h264_me_control_s', '-md': 'nvenc_h264_md_control_s',
             '-quant': 'nvenc_h264_quant_control_s', '-weights': 'nvenc_pred_weight_table_s'}
    text = include
    paths = {}
    for index, (suffix, ty) in enumerate(kinds.items()):
        paths[suffix] = members(ty)
        text += f'__declspec(dllexport) unsigned size{index}(void){{return sizeof({ty});}}\n'
        text += f'__declspec(dllexport) void parse{index}(const {ty} *p,long long *out){{\n'
        text += ''.join(f'out[{i}]=(long long)p->{p};\n' for i, p in enumerate(paths[suffix])) + '}\n'
    output = {}
    with tempfile.TemporaryDirectory(prefix='kestrel-picture-inspect-') as tmp:
        c, obj, dll = [Path(tmp) / ('parse.' + ext) for ext in ('c', 'obj', 'dll')]
        c.write_text(text)
        subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-mno-ms-bitfields', '-ffreestanding', '-std=c11', '-O2', '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([clang, '-shared', str(obj), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll))
        try:
            for index, suffix in enumerate(kinds):
                file = args.picture.with_name(args.picture.stem + suffix + '.bin')
                if not file.exists():
                    continue
                raw = file.read_bytes()
                assert len(raw) == getattr(lib, f'size{index}')(), f'wrong ABI size: {file}'
                data = C.create_string_buffer(raw)
                out = (C.c_longlong * len(paths[suffix]))()
                f = getattr(lib, f'parse{index}')
                f.argtypes = [C.c_void_p, C.c_void_p]
                f(data, out)
                output[suffix or 'picture'] = {k: v for k, v in zip(paths[suffix], out) if v or not args.nonzero}
        finally:
            _ctypes.FreeLibrary(lib._handle)
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
