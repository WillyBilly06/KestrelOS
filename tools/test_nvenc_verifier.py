#!/usr/bin/env python3
"""Negative tests for offline NVENC source checks; never invokes a codec."""
from pathlib import Path
import re
from verify_nv_nvenc import (NAMES, REFERENCE_NAMES, method_mismatches,
                             context_order_mismatches, without_comments)
from test_gpu_stable_candidate import function

ROOT=Path(__file__).resolve().parents[1]


def drop(text,prefix,name):
    actual=REFERENCE_NAMES.get(name,name) if prefix=='NVC9B7' else name
    changed,n=re.subn(r'^\s*#define\s+'+prefix+'_'+actual+r'\b[^\n]*\n','',text,flags=re.M)
    assert n==1,(prefix,name,n)
    return changed


def main():
    native=(ROOT/'kernel/nv_chan.c').read_text()
    reference=(ROOT/'refs/open-gpu-doc/classes/video/clc9b7.h').read_text()
    assert not method_mismatches(native,reference)
    assert not context_order_mismatches(native)
    cases=0
    for name in NAMES:
        a=drop(native,'NVCFB7',name)
        b=drop(reference,'NVC9B7',name)
        for left,right in ((a,reference),(native,b),(a,b)):
            assert method_mismatches(left,right),name
            cases+=1
        # A comment must not resurrect a deleted method.
        for comment in ('// #define NVCFB7_'+name+' 0x00000700u\n',
                        '/*\n#define NVCFB7_'+name+' 0x00000700u\n*/\n'):
            assert method_mismatches(a+comment,reference),name
            cases+=1
        # Ambiguous duplicate, expression, and changed literal all fail closed.
        for rhs in ('0x00000700u','0x00000700','(0x700u+4u)','0xdeadbeefu'):
            mutant=a+'\n#define NVCFB7_'+name+' '+rhs+'\n'
            if rhs in ('0x00000700u','0x00000700'):
                mutant=native+'\n#define NVCFB7_'+name+' '+rhs+'\n'
            assert method_mismatches(mutant,reference),name
            cases+=1
    channel=function(native,'open_engine_channel')
    host=function(native,'host_bind_channel_resources')
    for original,replacement in (
        ('if (!host_bind_channel_resources(', 'if (!missing_host_resources('),
        ('nvrm_host_get_video_falcon_context(', 'missing_context_query('),
        ('nv_vmm_map_kind(', 'missing_map('),
        ('nv_vmm_commit(', 'missing_commit('),
        ('NV2080_CTRL_CMD_GPU_PROMOTE_CTX', 'MISSING_PROMOTE'),
        ('nvrm_host_mark_context_bound(', 'missing_mark_bound(')):
        target=channel if original.startswith('if') else host
        assert original in target
        changed=native.replace(target,target.replace(original,replacement))
        assert context_order_mismatches(changed),original
        cases+=1
    # Strip the sole fail-closed return between host resource binding and
    # scheduling. Other returns elsewhere must not satisfy this check.
    clean=without_comments(native)
    c=function(clean,'open_engine_channel')
    begin=c.index('if (!host_bind_channel_resources(')
    end=c.index('NVA06F_CTRL_CMD_GPFIFO_SCHEDULE')
    segment=c[begin:end]
    assert segment.count('return -1;')==1
    mutant=clean.replace(c,c[:begin]+segment.replace('return -1;','')+c[end:])
    assert context_order_mismatches(mutant)
    cases+=1
    for signature in ('static int open_engine_channel(', 'static bool host_bind_channel_resources('):
        assert context_order_mismatches(native.replace(signature,signature.replace('(', '_missing(')))
        cases+=1
    print(f'PASS {cases} deliberately invalid source variants rejected; 19 current methods and active host context source order accepted')
    print('Checks verifier behavior, not GPU execution or native encoder correctness')


if __name__=='__main__':main()
