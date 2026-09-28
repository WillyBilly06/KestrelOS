#!/usr/bin/env python3
"""Compile-only regression scaffold for native video engine routing.

Reads NVIDIA source identifiers; does not load drivers, execute a codec,
execute the scaffold, or claim hardware validation.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / 'kernel/nv_chan.c').read_text()
REF = ROOT / 'out/nvidia-open-595.99.02/src'
notification = (REF / 'common/sdk/nvidia/inc/class/cl2080_notification.h').read_text()
rm_types = (REF / 'nvidia/inc/kernel/gpu/gpu_engine_type.h').read_text()
assert re.search(r'#define\s+NV2080_ENGINE_TYPE_NVENC3\s+\(0x0000003f\)', notification)
assert re.search(r'RM_ENGINE_TYPE_NVENC3\s*=\s*\(0x00000028\)', rm_types)
print('PASS NVIDIA source identifiers: NV2080 NVENC3=0x3f, RM NVENC3=0x28')

defines = '\n'.join(line for line in SRC.splitlines()
                    if re.match(r'#define NV2080_ENGINE_TYPE_', line))
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef uint32_t u32;
#define kinfo(...) ((void)0)
static u32 g_engine_count, g_engine_list[84];
'''
code += defines + '\n'
for name in ('nv_engine_present', 'nv_pick_engine', 'nv2080_to_rm_engine_type'):
    code += function(SRC, name) + '\n'
code += r'''
int main(void) {
    const u32 enc[] = {27, 28, 29, 63};
    /* Every subset; unrelated engines must not become an encoder. */
    for (u32 mask = 0; mask < 16; mask++) {
        g_engine_count = 0;
        g_engine_list[g_engine_count++] = 19; /* NVDEC0 */
        g_engine_list[g_engine_count++] = 62; /* OFA1, immediately before NVENC3 */
        for (u32 i = 0; i < 4; i++) if (mask & (1u << i))
            g_engine_list[g_engine_count++] = enc[i];
        for (u32 i = 0; i < 4; i++) {
            u32 expected = enc[i];
            if (!(mask & (1u << i)))
                for (u32 j = 0; j < 4; j++) if (mask & (1u << j)) {
                    expected = enc[j]; break;
                }
            assert(nv_pick_engine(enc[i]) == expected);
        }
    }
    for (u32 i = 0; i < 4; i++)
        assert(nv2080_to_rm_engine_type(enc[i]) == 37 + i);
    for (u32 i = 0; i < 8; i++)
        assert(nv2080_to_rm_engine_type(19 + i) == 29 + i);
    for (u32 i = 1; i <= 18; i++)
        assert(nv2080_to_rm_engine_type(i) == i);
    assert(nv2080_to_rm_engine_type(62) == 62);
    /* Preserve the existing unknown-enumeration behavior. */
    g_engine_count = 0;
    for (u32 i = 0; i < 4; i++) assert(nv_pick_engine(enc[i]) == enc[i]);
    return 0;
}
'''
clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
with tempfile.TemporaryDirectory(prefix='kestrel-video-routing-') as tmp:
    source = Path(tmp) / 'routing.c'
    source.write_text(code)
    subprocess.run([clang, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-c', str(source), '-o', str(Path(tmp) / 'routing.obj')], check=True)
print('COMPILE ONLY: production routing scaffold compiled; runtime assertions NOT executed')
