#!/usr/bin/env python3
"""Execute production class lookup and identification bounds without a GPU.

These are CPU-only regression checks, not simulated GPU/codec workloads and
not evidence that any additional hardware generation is accelerated.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^[^\n;{}]+\b" + name + r"\([^;{}]*\)\s*\{", source, re.M)
    assert match, name
    return source[match.start():source.index("\n}", match.end()) + 2]


def main():
    blackwell = (ROOT / "kernel/nv_blackwell.c").read_text()
    gpu = (ROOT / "kernel/gpu.c").read_text()
    nvidia = (ROOT / "kernel/nvidia.c").read_text()
    header = (ROOT / "kernel/nv.h").read_text()
    classes = re.search(r"typedef struct \{\s*u32\s+family;[^}]+\} nv_classes_t;", header)[0]
    table_start = blackwell.index("static const nv_classes_t class_table[]")
    table_end = blackwell.index("\n};", table_start) + 3
    probe = function(gpu, "probe")
    assert "size_t want = nvidia_probe_window_size(have);" in probe
    assert "(g->bar_mmio && want)" in probe
    assert "nvidia_identify(g, regs, regs ? want : 0);" in probe
    source = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_LEN(a) (sizeof(a)/sizeof((a)[0]))
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
'''
    source += classes + "\n" + blackwell[table_start:table_end] + "\n"
    source += function(blackwell, "nv_classes_for") + "\n"
    source += function(gpu, "nvidia_probe_window_size") + "\n"
    source += function(nvidia, "rd32") + "\n"
    source += r'''
int main(void) {
    /* Independent expected values preserve every existing mapped family,
     * including both Blackwell variants; this is not a support matrix. */
    static const u32 expected[][7] = {
        {0xc0,0x9097,0x90c0,0x90b5,0x906f,0,0},
        {0xd0,0x9097,0x90c0,0x90b5,0x906f,0,0},
        {0xe0,0xa097,0xa0c0,0xa0b5,0xa06f,0,0},
        {0xf0,0xa197,0xa1c0,0xa0b5,0xa06f,0,0},
        {0x100,0xa297,0xa1c0,0xa0b5,0xa06f,0,0},
        {0x110,0xb097,0xb0c0,0xb0b5,0xb06f,0,0},
        {0x120,0xb197,0xb1c0,0xb0b5,0xb06f,0,0},
        {0x130,0xc097,0xc0c0,0xc0b5,0xc06f,0,0},
        {0x140,0xc397,0xc3c0,0xc3b5,0xc36f,0xc37d,0xc37e},
        {0x160,0xc597,0xc5c0,0xc5b5,0xc46f,0xc57d,0xc57e},
        {0x170,0xc697,0xc6c0,0xc6b5,0xc56f,0xc67d,0xc67e},
        {0x180,0xcb97,0xcbc0,0xc8b5,0xc86f,0,0},
        {0x190,0xc997,0xc9c0,0xc7b5,0xc56f,0xc77d,0xc77e},
        {0x1a0,0xcd97,0xcdc0,0xc9b5,0xc96f,0,0},
        {0x1b0,0xce97,0xcec0,0xcab5,0xca6f,0xca7d,0xca7e}
    };
    for (u32 chip=0;chip<0x10000;chip++) {
        const u32 *want=NULL;
        for (size_t i=0;i<ARRAY_LEN(expected);i++)
            if ((chip & ~15u)==expected[i][0]) want=expected[i];
        const nv_classes_t *actual=nv_classes_for(chip);
        if (!want) { CHECK(!actual); continue; }
        CHECK(actual && actual->family==want[0]);
        CHECK(actual->three_d==want[1] && actual->compute==want[2]);
        CHECK(actual->copy==want[3] && actual->gpfifo==want[4]);
        CHECK(actual->disp_core==want[5] && actual->disp_window==want[6]);
    }
    CHECK(!nv_classes_for(0xffffffffu));
    CHECK(!nv_classes_for(0x800001b3u));
    static const u64 sizes[]={0,1,3,4,4096,8192,0x100000,0x100ce0,
        0x100ce4,0x102000,0x1000000,0x2000000,0x100000000ull,UINT64_MAX};
    for(size_t i=0;i<ARRAY_LEN(sizes);i++) {
        size_t mapped=nvidia_probe_window_size(sizes[i]);
        CHECK(mapped<=sizes[i] && mapped<=0x1000000);
        CHECK(mapped==(sizes[i]<0x1000000?sizes[i]:0x1000000));
    }
    volatile u32 registers[]={0x12345678,0xabcdef01};
    volatile u8 *bytes=(volatile u8 *)registers;
    CHECK(rd32(bytes,4,0)==0x12345678);
    CHECK(rd32(bytes,8,4)==0xabcdef01);
    CHECK(rd32(bytes,4,4)==0xffffffffu);
    CHECK(rd32(bytes,3,0)==0xffffffffu);
    CHECK(rd32(bytes,8,0x100ce0)==0xffffffffu);
    CHECK(rd32(bytes,8,0xffffffffu)==0xffffffffu);
    CHECK(rd32(NULL,0x1000000,0)==0xffffffffu);
    puts("PASS: 65538 chip IDs, 14 aperture sizes, 7 MMIO boundary cases");
    return 0;
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-gpu-variants-") as directory:
        c = Path(directory) / "variants.c"
        executable = Path(directory) / "variants.exe"
        c.write_text(source)
        subprocess.run([clang, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        str(c), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
