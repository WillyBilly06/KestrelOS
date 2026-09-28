#!/usr/bin/env python3
"""Build/run actual Blackwell shader checks using the host CUDA driver."""
from pathlib import Path
import os
import subprocess
import tempfile
from nvshader import find_cl, tool

ROOT = Path(__file__).resolve().parents[1]

if __name__ == "__main__":
    env = dict(os.environ)
    cl = find_cl()
    if cl:
        env["PATH"] = cl + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="kestrel-gpu2d-") as tmp:
        exe = str(Path(tmp) / "gpu2d.exe")
        subprocess.run([tool("nvcc"), "-arch=sm_120", "-O2", "-o", exe,
                        str(ROOT / "tools/test_gpu2d_cuda.cu")],
                       check=True, env=env, cwd=tmp, timeout=120)
        subprocess.run([exe], check=True, timeout=60)
