#!/usr/bin/env python3
"""Check actual dispatch layouts against all current generated shader sizes."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/nv_chan.c").read_text()

if __name__ == "__main__":
    shader = (ROOT / "tools/shader_gui_raster.h").read_text()
    shader_bytes = int(re.search(r"gui_raster_sass\[(\d+)\]", shader)[1])
    present = (ROOT / "tools/shader_gui_present.h").read_text()
    present_bytes = int(re.search(r"gui_present_sass\[(\d+)\]", present)[1])
    gl = (ROOT / "tools/shader_gl_raster.h").read_text()
    gl_bytes = int(re.search(r"gl_raster_sass\[(\d+)\]", gl)[1])
    vm = (ROOT / "tools/shader_shader_vm.h").read_text()
    vm_bytes = int(re.search(r"shader_vm_sass\[(\d+)\]", vm)[1])
    raster = (ROOT / "tools/shader_shader_raster.h").read_text()
    raster_bytes = int(re.search(r"shader_raster_sass\[(\d+)\]", raster)[1])
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef struct {u32 qmd_off, shader_off, constant_off, bytes;} nv_dispatch_layout_t;
'''
    source += function(SRC, "nv_compute_dispatch_layout")
    source += '\n' + function(SRC, "nv_compute_resident_layout")
    source += '\n' + re.search(r'^#define NV_GUI_DISPATCH_OFF\s+[^\n]+',SRC,re.M)[0] + '\n'
    source += "\n#define GUI_SHADER_BYTES " + str(shader_bytes) + "u\n"
    source += "\n#define PRESENT_SHADER_BYTES " + str(present_bytes) + "u\n"
    source += "\n#define GL_SHADER_BYTES " + str(gl_bytes) + "u\n"
    source += "\n#define VM_SHADER_BYTES " + str(vm_bytes) + "u\n"
    source += "\n#define RASTER_SHADER_BYTES " + str(raster_bytes) + "u\n"
    source += r'''
int main(void) {
    nv_dispatch_layout_t l;
    for (u32 off = 0; off <= 0x10000; off += 0x100) {
        for (u32 len = 16; len <= 0x10000; len += 16) {
            l = (nv_dispatch_layout_t){1,2,3,4};
            bool ok = nv_compute_dispatch_layout(off,len,32,&l);
            if (!ok) { assert(l.qmd_off==1 && l.shader_off==2 && l.constant_off==3 && l.bytes==4); continue; }
            assert(l.qmd_off == off && !(l.qmd_off & 255));
            assert(l.shader_off == off+0x400 && !(l.shader_off & 15));
            assert(l.constant_off >= l.shader_off+len && !(l.constant_off & 63));
            assert(l.constant_off+0x400 == off+l.bytes && off+l.bytes <= 0x10000);
            if (len <= 0x400 && !(off & 0x3ff)) assert(l.constant_off==off+0x800 && l.bytes==0xc00);
        }
    }
    assert(!nv_compute_dispatch_layout(0xc000,GUI_SHADER_BYTES,16,&l));
    assert(nv_compute_dispatch_layout(NV_GUI_DISPATCH_OFF,GUI_SHADER_BYTES,16,&l));
    assert(256u*64u<=l.qmd_off && l.constant_off>=l.shader_off+GUI_SHADER_BYTES);
    assert(l.constant_off+0x400<=0x10000);
    assert(nv_compute_dispatch_layout(0xc000,PRESENT_SHADER_BYTES,19,&l));
    assert(l.constant_off >= 0xc400+PRESENT_SHADER_BYTES && l.constant_off+0x400<=0x10000);
    assert(nv_compute_dispatch_layout(0x8000,GL_SHADER_BYTES,22,&l));
    assert(l.constant_off >= 0x8400+GL_SHADER_BYTES && l.constant_off+0x400<=0x10000);
    assert(64u*192u<=l.qmd_off);
    assert(!nv_compute_dispatch_layout(0xc000,GL_SHADER_BYTES,22,&l));
    /* VM integration needs a lower offset than fixed-function raster: never
     * reuse 0x8000 for this larger program. 0x4000 leaves 16KiB before QMD. */
    assert(!nv_compute_dispatch_layout(0x8000,VM_SHADER_BYTES,24,&l));
    assert(nv_compute_dispatch_layout(0x4000,VM_SHADER_BYTES,24,&l));
    assert(l.constant_off >= 0x4400+VM_SHADER_BYTES && l.constant_off+0x400<=0x10000);
    assert(1024u*16u<=l.qmd_off);
    assert(!nv_compute_dispatch_layout(0x4000,RASTER_SHADER_BYTES,24,&l));
    assert(!nv_compute_dispatch_layout(0x1000,RASTER_SHADER_BYTES,24,&l));
    /* The generic resident fragment program has an independent 128KiB slot;
     * generic placement still rejects a program that would overlap bank0. */
    assert(RASTER_SHADER_BYTES<=0x20000);
    assert(!nv_compute_dispatch_layout(0x400,0x10000,24,&l));
    assert(nv_compute_resident_layout(0x400,24,&l));
    assert(l.qmd_off==0x400 && !l.shader_off && l.constant_off==0x800 && l.bytes==0x800);
    for(u32 off=0;off<=0x10100;off++)for(u32 args=0;args<=33;args++){
        l=(nv_dispatch_layout_t){1,2,3,4};
        bool valid=!(off&255) && off<=0xf800 && args<=32;
        assert(nv_compute_resident_layout(off,args,&l)==valid);
        if(valid){
            assert(l.qmd_off==off && l.constant_off==off+0x400 && l.bytes==0x800);
            assert(off+l.bytes<=0x10000 && !l.shader_off);
        }else assert(l.qmd_off==1 && l.shader_off==2 && l.constant_off==3 && l.bytes==4);
    }
    assert(!nv_compute_resident_layout(UINT32_MAX,0,&l));
    assert(!nv_compute_resident_layout(0,0,NULL));
    assert(!nv_compute_dispatch_layout(0,0,0,&l));
    assert(!nv_compute_dispatch_layout(1,16,0,&l));
    assert(!nv_compute_dispatch_layout(0,17,0,&l));
    assert(!nv_compute_dispatch_layout(UINT32_MAX,16,0,&l));
    assert(!nv_compute_dispatch_layout(0,UINT32_MAX,0,&l));
    assert(!nv_compute_dispatch_layout(0,16,33,&l));
    assert(!nv_compute_dispatch_layout(0,16,0,0));
    puts("PASS: >1M actual dispatch layouts, GUI/GL/VM shader sizes, legacy placement, overflow and argument limits");
    return 0;
}
'''
    run_test(source, "dispatch_layout")
