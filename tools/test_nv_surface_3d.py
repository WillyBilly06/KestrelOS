#!/usr/bin/env python3
"""Execute actual 3D admission/dispatch code with modeled surface allocations."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/nv_chan.c").read_text()


def main():
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef uint64_t u64;
typedef struct {bool open,submit_failed;} nv_channel_t;
typedef struct {u32 width,height,pitch;u64 bytes,va;} nv_surface_t;
#define CH_GFX 0
static nv_channel_t channels[1]={{true,false}};
static nv_surface_t surfaces[3]={{71,49,336,65536,0x30000000000ull},
                               {71,49,320,65536,0x30010000000ull},
                               {7,5,64,65536,0x30020000000ull}};
static nv_surface_t *nv_surface_lookup(u64 owner,u64 h) {
    return owner==9 && h>=101 && h<=103 ? &surfaces[h-101] : NULL;
}
static unsigned ensures,uploads,launches;static int failure;
static unsigned char staged[64*192];
static u64 g_compute_vram_fb=0x1000000;
static bool ensure_compute_vram(nv_channel_t *ch) {
    assert(ch==channels);ensures++;return failure!=1;
}
static bool nv_vram_object_write(nv_channel_t *ch,u32 obj,u64 fb,u64 off,const void *p,u64 n) {
    assert(ch==channels && obj && fb==g_compute_vram_fb && !off && n<=sizeof staged);
    uploads++;if(failure==2)return false;memcpy(staged,p,(size_t)n);return true;
}
'''
    for path in ["include/kestrel/gpu3d.h", "tools/shader_gl_raster.h"]:
        code += '\n#include "' + (ROOT / path).as_posix() + '"\n'
    for name in ["VA_COMPUTE", "H_COMPUTE_VRAM"]:
        code += re.search(r"^#define\s+" + name + r"\s+[^\n]*", SRC, re.M)[0] + "\n"
    code += r'''
static bool nv_compute_upload(nv_channel_t *ch,u64 off,const void *p,u64 n) {
    return nv_vram_object_write(ch,H_COMPUTE_VRAM,g_compute_vram_fb,off,p,n);
}
static bool nv_compute_dispatch_one(nv_channel_t *ch,const unsigned char *sass,u32 bytes,
                                    u32 regs,u32 off,const u32 *args,u32 n,
                                    const u32 *grid,const u32 *block,u32 tag,const char *label) {
    assert(ch==channels && sass==gl_raster_sass && bytes==sizeof gl_raster_sass);
    assert(regs==gl_raster_sass_reg_count && off==0x8000 && n==22 && tag==0x47330000);
    assert(!strcmp(label,"surface/3d"));launches++;
    assert((((u64)args[1]<<32)|args[0])==surfaces[0].va);
    u64 depth=((u64)args[3]<<32)|args[2],texture=((u64)args[5]<<32)|args[4];
    assert(!depth || depth==surfaces[1].va);assert(!texture || texture==surfaces[2].va);
    assert((((u64)args[7]<<32)|args[6])==VA_COMPUTE);
    assert((((u64)args[9]<<32)|args[8])==(texture?surfaces[2].bytes:0));
    assert(args[10]==71 && args[11]==49 && args[12]==84);
    assert(args[13]==(depth?80:0) && args[14]==(texture?7:0));
    assert(args[15]==(texture?5:0) && args[16]==(texture?16:0));
    assert(args[17]>=1 && args[17]<=64);
    assert(grid[0]==(args[20]+15)/16 && grid[1]==(args[21]+15)/16 && grid[2]==1);
    assert(block[0]==16 && block[1]==16 && block[2]==1);
    const kg3d_command_t *c=(const void *)staged;
    assert(c[0].width==71 && c[0].height==49);
    if(failure==3)return false;
    if(failure==4){ch->submit_failed=true;return false;}return true;
}
'''
    for name in ["nv_float_bits", "nv_surface_3d_commands_valid", "nv_surface_draw3d"]:
        code += "\n" + function(SRC, name)
    code += r'''
static void set_bits(float *p,u32 bits){memcpy(p,&bits,4);}
static bool draw(const kg3d_command_t *c,u32 n,u64 depth,u64 texture) {
    return nv_surface_draw3d(9,101,depth,texture,c,n,0,0,71,49);
}
int main(void) {
    kg3d_command_t c={.width=71,.height=49,.flags=19,.depth_func=1};
    for(unsigned j=0;j<3;j++)c.v[j].inv_w=1;
    assert(draw(&c,1,102,103));assert(ensures==1 && uploads==1 && launches==1);
    unsigned before=ensures;
    assert(!nv_surface_draw3d(8,101,102,103,&c,1,0,0,71,49)); /* owner */
    assert(!draw(&c,1,101,103));assert(!draw(&c,1,102,101)); /* destination alias */
    assert(!draw(&c,1,102,102));assert(!draw(&c,1,104,103)); /* source alias/bad handle */
    assert(!draw(&c,1,0,103));assert(!draw(&c,1,102,0)); /* required resources */
    assert(!draw(&c,0,102,103));assert(!draw(&c,65,102,103));assert(!draw(NULL,1,102,103));
    assert(!nv_surface_draw3d(9,101,102,103,&c,1,UINT32_MAX,0,1,1));
    assert(!nv_surface_draw3d(9,101,102,103,&c,1,0,0,UINT32_MAX,1));
    surfaces[1].height--;assert(!draw(&c,1,102,103));surfaces[1].height++;
    assert(ensures==before && launches==1);
    for(unsigned i=0;i<23;i++) {
        kg3d_command_t bad=c;
        switch(i) {
        case 0:bad.width=0;break;case 1:bad.height=-1;break;case 2:bad.flags=512;break;
        case 3:bad.depth_func=8;break;case 4:bad.alpha_func=8;break;case 5:bad.blend_src=6;break;
        case 6:bad.blend_dst=6;break;case 7:bad.filter=2;break;case 8:bad.wrap_s=2;break;
        case 9:bad.wrap_t=2;break;case 10:bad.tex_env=3;break;case 11:bad.reserved[0]=1;break;
        case 12:bad.v[1].x=1048577;break;case 13:bad.v[1].y=-1048577;break;
        case 14:bad.v[1].inv_w=-1;break;case 15:bad.v[1].inv_w=0;break;
        case 16:set_bits(&bad.v[1].z,0x7f800000);break;
        case 17:set_bits(&bad.v[1].rgba[3],0x7fc00001);break;
        case 18:set_bits(&bad.v[1].uv[1],0xff800000);break;
        case 19:set_bits(&bad.depth_bias,0x7fc00000);break;
        case 20:set_bits(&bad.alpha_ref,0xff800000);break;
        case 21:set_bits(&bad.v[1].inv_w,0x80000000);break;
        case 22:set_bits(&bad.v[1].inv_w,0x7f800000);break;
        }
        assert(!draw(&bad,1,102,103));assert(ensures==before);
    }
    kg3d_command_t batch[64];for(unsigned i=0;i<64;i++)batch[i]=c;
    assert(draw(batch,64,102,103)); /* maximum command upload */
    batch[63].reserved[2]=1;before=uploads;assert(!draw(batch,64,102,103));assert(uploads==before);
    /* Work budget must be checked without multiplication overflow. */
    surfaces[0].width=surfaces[0].height=32768;batch[63]=c;
    for(unsigned i=0;i<64;i++)batch[i].flags=0;
    assert(!nv_surface_draw3d(9,101,0,0,batch,64,0,0,32768,32768));
    surfaces[0].width=71;surfaces[0].height=49;
    c.flags=KG3D_LINE;assert(draw(&c,1,0,0));
    c.flags=KG3D_POINT;assert(draw(&c,1,0,0));
    unsigned invalid_flags[]={KG3D_LINE|KG3D_POINT,KG3D_LINE|KG3D_TEXTURE,
                              KG3D_POINT|KG3D_CLEAR_DEPTH,KG3D_LINE|KG3D_CLEAR_COLOUR};
    before=ensures;
    for(unsigned i=0;i<4;i++){c.flags=invalid_flags[i];assert(!draw(&c,1,102,103));}
    assert(ensures==before);
    /* Line records can evaluate six DDA samples per pixel. The same rectangle
     * and record count fit for triangles but MUST reject for lines. */
    surfaces[0].width=surfaces[0].height=1024;
    for(unsigned i=0;i<11;i++){batch[i]=c;batch[i].flags=KG3D_LINE;}
    assert(!nv_surface_draw3d(9,101,0,0,batch,11,0,0,1024,1024));
    assert(ensures==before);
    for(unsigned i=0;i<11;i++)batch[i].flags=0;
    failure=1;assert(!nv_surface_draw3d(9,101,0,0,batch,11,0,0,1024,1024));
    assert(ensures==before+1);failure=0;
    surfaces[0].width=71;surfaces[0].height=49;
    c.flags=0;assert(draw(&c,1,0,0));
    c.flags=KG3D_CLEAR_COLOUR;for(unsigned j=0;j<3;j++)c.v[j].inv_w=0;assert(draw(&c,1,0,0));
    c.flags=KG3D_CLEAR_DEPTH;assert(!draw(&c,1,0,0));assert(draw(&c,1,102,0));
    for(failure=1;failure<=4;failure++) {
        unsigned u=uploads,l=launches;assert(!draw(&c,1,102,0));
        if(failure==1)assert(uploads==u);
        if(failure<=2)assert(launches==l);
    }
    before=uploads;assert(channels[0].submit_failed && !draw(&c,1,102,0) && uploads==before);
    puts("PASS actual 3D surface admission/dispatch: owner/alias checks, finite records, work limits, 22-word ABI, coherent upload, failure ordering and timeout quarantine (modeled GPU)");
}
'''
    run_test(code, "surface_3d")


if __name__ == "__main__":
    main()
