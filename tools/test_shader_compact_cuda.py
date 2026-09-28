#!/usr/bin/env python3
"""Production compaction shader on Windows CUDA; not native OS verification."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
from nvshader import find_cl, tool

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitizer', choices=('memcheck', 'synccheck'))
    args = parser.parse_args()
    env = dict(os.environ)
    compiler = find_cl()
    if compiler:
        env['PATH'] = compiler+os.pathsep+env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='kestrel-compact-cuda-') as directory:
        executable = str(Path(directory)/'compact.exe')
        subprocess.run([tool('nvcc'), '-arch=sm_120', '-O2', '-o', executable,
                        str(ROOT/'tools/test_shader_compact_cuda.cu'),
                        str(ROOT/'tools/shader_setup_compact.cu'),
                        str(ROOT/'tools/shader_setup_compact_small.cu')],
                       check=True, env=env, cwd=directory, timeout=120)
        command = [executable]
        if args.sanitizer:
            sanitizer = Path(tool('nvcc')).parent.parent/'compute-sanitizer'/'compute-sanitizer.exe'
            if not sanitizer.is_file():
                raise SystemExit('CUDA compute-sanitizer is unavailable: '+str(sanitizer))
            command = [str(sanitizer), '--tool', args.sanitizer, '--error-exitcode', '86',
                       executable, '--no-timing']
        subprocess.run(command, check=True, timeout=300 if args.sanitizer else 120)


if __name__ == '__main__':
    main()
