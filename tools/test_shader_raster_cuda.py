#!/usr/bin/env python3
"""Actual GPU programmable pixels vs independent coverage + production CPU VM.

Windows CUDA evidence, not Kestrel dispatch/desktop/application proof.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from nvshader import find_cl, tool

ROOT=Path(__file__).resolve().parents[1]
if __name__=='__main__':
    src=(ROOT/'user/libgl/shadervm.c').read_text()
    start=src.index('#define VERY_LARGE')
    end=src.index('/* --------------------------------------------------------------- locations */',start)
    preamble='''#include <stdint.h>
#include <math.h>
#include "'''+(ROOT/'include/kestrel/shader_vm.h').as_posix()+'''"
typedef void (*sh_sampler_fn)(int,float,float,float*);
typedef struct {float reg[SR_REGISTERS][4];sh_sampler_fn sample;bool discarded;int executed;} sh_machine_t;
typedef struct {bool compiled;int count;sh_instruction_t code[SH_MAX_INSTRUCTIONS];} sh_shader_t;
'''
    env=dict(os.environ)
    cl=find_cl()
    if cl:env['PATH']=cl+os.pathsep+env.get('PATH','')
    with tempfile.TemporaryDirectory(prefix='kestrel-shader-raster-') as tmp:
        (Path(tmp)/'cpu_reference.h').write_text(preamble+src[start:end])
        exe=str(Path(tmp)/'shader_raster_test.exe')
        subprocess.run([tool('nvcc'),'-arch=sm_120','-O2','-I',tmp,'-o',exe,
                        str(ROOT/'tools/test_shader_raster_cuda.cu'),str(ROOT/'tools/shader_raster_fast.cu')],
                       check=True,env=env,cwd=tmp,timeout=120)
        subprocess.run([exe],check=True,timeout=120)
        subprocess.run([exe,'register'],check=True,timeout=120)
