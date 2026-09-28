#!/usr/bin/env python3
"""Execute native surface lifecycle and actual VMM code with modeled RM/GPU.

This is ownership/ABI/rollback coverage, not a physical Kestrel launch proof.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
CHAN = (ROOT / "kernel/nv_chan.c").read_text()
VMM = (ROOT / "kernel/nv_vmm.c").read_text()


def main():
    # Bind modeled RM values to NVIDIA's published ABI, not to the production
    # allocator's assumptions (the previous kind-zero-only mock hid this bug).
    sdk = ROOT / 'out/nvidia-open-595.99.02/src/common'
    mmu = (sdk / 'inc/swref/published/blackwell/gb202/dev_mmu.h').read_text()
    ctrl = (sdk / 'sdk/nvidia/inc/ctrl/ctrl0041.h').read_text()
    for name, value in [('PITCH', 0), ('GENERIC_MEMORY', 6),
                        ('GENERIC_MEMORY_COMPRESSIBLE', 8)]:
        match = re.search(r'^#define NV_MMU_PTE_KIND_' + name + r'\s+(0x[0-9a-fA-F]+)\b', mmu, re.M)
        assert match and int(match[1], 16) == value
    assert re.search(r'#define NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR_APERTURE_VIDMEM\s+\(0x00000000\)', ctrl)
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
typedef int64_t s64;
#define kmalloc malloc
#define krealloc realloc
#define kfree free
#define PAGE_SIZE 4096u
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
#define kwarn(...) ((void)0)
typedef struct {u8 *pool; u64 pool_gpu; u32 pages, used; u64 root_gpu;
                u32 mapped; bool tables_in_vram, ready;} nv_vmm_t;
typedef struct {u64 phys_address, size; u32 aperture; u8 page_shift, _pad[3];} nv_vmm_level_t;
'''
    c += re.sub(r'^#include[^\n]*\n', '', VMM[:VMM.index('/* ------------------------------------------------------------------- test */')], flags=re.M)
    c += '\n#include "' + (ROOT / 'include/kestrel/gpu2d.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'include/kestrel/gpu3d.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'include/kestrel/shader_vm.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'include/kestrel/shader_raster.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'include/kestrel/shader_setup.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_shader_setup.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'kernel/shader_setup_compact_sass.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'kernel/shader_setup_compact_small_sass.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_shader_raster.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_shader_raster_fast.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_shader_vm.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_gl_raster.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_gui_raster.h').as_posix() + '"\n'
    c += '\n#include "' + (ROOT / 'tools/shader_gui_present.h').as_posix() + '"\n'
    # Exercise the actual wire-layout structs rather than reordered mock types.
    for typename in ('mem_alloc_params_t', 'phys_attr_params_t', 'nv_ce_prepare_t'):
        match = re.search(r'typedef struct \{[^{}]*\}\s*' + typename + r';', CHAN)
        assert match, typename
        c += match[0] + '\n'
    for name in ('NVOS32_TYPE_IMAGE', 'NVOS32_ATTR_VIDMEM_CONTIGUOUS',
                 'NVOS32_ATTR2_GPU_CACHEABLE_NO', 'NVOS32_ALLOC_FLAGS_NO_SCANOUT',
                 'NVOS32_ALLOC_FLAGS_ALIGN_FORCE', 'NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR',
                  'NV0041_APERTURE_VIDMEM', 'UPLOAD_STAGE_BYTES', 'VA_UPLOAD_STAGE'):
        match = re.search(r'^#define ' + name + r'\s+[^\n]+', CHAN, re.M)
        assert match, name
        c += match[0] + '\n'
    c += r'''
_Static_assert(sizeof(mem_alloc_params_t)==128, "RM allocation ABI");
_Static_assert(offsetof(mem_alloc_params_t, attr)==24, "RM attr offset");
_Static_assert(offsetof(mem_alloc_params_t, size)==64, "RM size offset");
_Static_assert(sizeof(phys_attr_params_t)==48, "RM phys-attr ABI");
_Static_assert(offsetof(phys_attr_params_t, mem_format)==8, "RM kind offset");
_Static_assert(offsetof(phys_attr_params_t, mem_aperture)==20, "RM aperture offset");
_Static_assert(offsetof(phys_attr_params_t, contig_segment_size)==40, "RM contiguous span offset");
typedef struct { u32 width,height,pitch; u64 bytes; } nv_surface_info_t;
typedef struct { bool host_api; } rm_fixture_t;
typedef struct { bool open, submit_failed; void *card; rm_fixture_t *rm; nv_vmm_t vmm; } nv_channel_t;
static rm_fixture_t rm_fixture;
static nv_channel_t channels[2];
#define CH_GFX 0
#define CH_COPY 1
#define RM_DEVICE 1
#define NV01_MEMORY_LOCAL_USER 2
_Static_assert(NV0041_APERTURE_VIDMEM==0, "official VIDMEM aperture");
_Static_assert(NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR==0x410103, "official phys query");
#define VA_COMPUTE 0x500000000ull
#define H_COMPUTE_VRAM 0x490000u
static const u64 g_compute_vram_fb = 0x800000;
static u32 g_runtime_scanout_width=100,g_runtime_scanout_height=50,g_runtime_scanout_pitch=448;
static u64 g_runtime_scanout_va=0x10000020000ull,g_runtime_scanout_bytes=448*50;
static bool g_scanout_bound=true,g_compute_scanout_bound=true;
/* This allocator/lifetime fixture has no runtime DMA stage or scheduled owner.
 * It intentionally retains the coherent boot transfer branch. */
static bool g_upload_stage_ready;
static volatile u8 *g_upload_stage;
static void *g_render_pin;
static u32 g_render_transaction;
/* Runtime packet encoding is covered by test_nv_raster_prepare. This fixture
 * exercises coherent boot transfers only: accidentally reaching DMA must fail. */
static bool nv_compute_copy_mapping(nv_channel_t *ch) {
    (void)ch;assert(!"unexpected runtime copy mapping");return false;
}
static bool ce_prepare_batch(nv_channel_t *ch,const nv_ce_prepare_t *ops,u32 count) {
    (void)ch;(void)ops;(void)count;assert(!"unexpected runtime prepare batch");return false;
}
static void *proc_current(void) { return NULL; }
static bool sched_device_wait_allowed(void) { return false; }
static void cache_flush(const volatile void *p,u32 bytes) { (void)p;(void)bytes; }
static bool ce_copy(nv_channel_t *ch,u64 src,u64 dst,u32 sp,u32 dp,u32 bytes,u32 lines,u32 signal) {
    (void)ch;(void)src;(void)dst;(void)sp;(void)dp;(void)bytes;(void)lines;(void)signal;
    assert(!"no runtime DMA route in this allocator fixture");return false;
}
static u8 ptpool[2][512*4096];
static u8 *storage[32];
static u64 sizes[32];
static bool mapped[32],copy_mapped[32];
static unsigned allocs, frees, dispatches, commits, writes, ce_fills;
static int failure;
static u32 returned_kind=6; /* GB202 GENERIC_MEMORY, as returned by the physical boot. */
static kg2d_command_t staged[256];
static u32 staged_count;
static bool proc_gpu_owner_alive(u64 owner) { return owner != 0; }
static u32 h_vaspace(int idx) { assert(idx == 0 || idx == 1); return 55+idx; }
static bool ensure_compute_vram(nv_channel_t *ch) { (void)ch; return true; }
static void nv_fault_shadow_path(nv_channel_t *ch,u64 va) {(void)ch;(void)va;}
static bool nv_rm_alloc(void *card,void *rm,u32 parent,u32 handle,u32 cls,void *params,u32 bytes) {
    (void)card;(void)rm; assert(parent==RM_DEVICE && cls==NV01_MEMORY_LOCAL_USER && bytes==sizeof(mem_alloc_params_t));
    mem_alloc_params_t *mp=params; u32 slot=handle-0x500000u;
    assert(slot<32 && !storage[slot]);
    assert(mp->size && !(mp->size&65535) && mp->size<=0x10000000u && mp->alignment==65536);
    assert(mp->owner==0x4b455354u && mp->type==NVOS32_TYPE_IMAGE);
    assert(mp->attr==NVOS32_ATTR_VIDMEM_CONTIGUOUS);
    assert(mp->attr2==(NVOS32_ATTR2_GPU_CACHEABLE_NO<<2));
    assert(mp->flags==(NVOS32_ALLOC_FLAGS_NO_SCANOUT|NVOS32_ALLOC_FLAGS_ALIGN_FORCE));
    if(failure==1) return false;
    storage[slot]=malloc((size_t)mp->size); assert(storage[slot]); sizes[slot]=mp->size;
    memset(storage[slot],0xa5,(size_t)mp->size); allocs++; return true;
}
static bool nv_rm_control(void *card,void *rm,u32 handle,u32 cmd,void *in,u32 isz,void *out,u32 osz,u32 *got) {
    (void)card;(void)rm; assert(cmd==NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR);
    assert(in==out && isz==48 && osz==48);
    phys_attr_params_t *pa=out;
    assert(pa->mem_offset==0 && pa->mmu_context==0);
    pa->mem_aperture=failure==4?1:NV0041_APERTURE_VIDMEM;
    pa->mem_offset=0x10000000ull+(handle-0x500000u)*0x10000000ull+(failure==5?1:0);
    pa->mem_format=failure==16?8:returned_kind;
    pa->contig_segment_size=failure==15?4096:sizes[handle-0x500000u];
    *got=failure==3?0:osz; return failure!=2;
}
static bool nv_vmm_commit(void *card,void *rm,nv_vmm_t *vmm,u32 vas,const char *label) {
    (void)card;(void)rm; assert(vas==55 || vas==56); commits++;
    assert(vmm==&channels[vas-55].vmm);
    if((failure==6 && !strcmp(label,"surface map")) ||
       (failure==12 && !strcmp(label,"surface unmap")) ||
       (failure==13 && !strcmp(label,"surface copy map")) ||
       (failure==14 && !strcmp(label,"surface copy unmap"))) return false;
    for(u32 i=0;i<32;i++) {
        nv_vmm_walk_t w; nv_vmm_walk(vmm,0x30000000000ull+i*0x10000000ull,&w);
        (vas==55?mapped:copy_mapped)[i]=w.present;
    }
    return true;
}
static bool nv_rm_free(void *card,void *rm,u32 parent,u32 handle) {
    (void)card;(void)rm; assert(parent==RM_DEVICE);
    u32 slot=handle-0x500000u;
    assert(slot<32 && storage[slot] && !mapped[slot] && !copy_mapped[slot] &&
           !channels[0].submit_failed && !channels[1].submit_failed);
    if(failure==11) return false;
    free(storage[slot]); storage[slot]=NULL; frees++; return true;
}
static bool nv_vram_object_write(nv_channel_t *ch,u32 handle,u64 fb,u64 offset,const void *data,u64 bytes) {
    (void)ch;(void)fb; writes++;
    if(handle==H_COMPUTE_VRAM) {
        assert(offset==0 && bytes<=sizeof staged && bytes%sizeof(staged[0])==0);
        memcpy(staged,data,(size_t)bytes); staged_count=(u32)bytes/sizeof(staged[0]);
        return failure!=7;
    }
    u32 slot=handle-0x500000u; assert(slot<32 && offset+bytes<=sizes[slot]);
    memcpy(storage[slot]+offset,data,(size_t)bytes); return true;
}
static bool nv_vram_object_read(nv_channel_t *ch,u32 handle,u64 fb,u64 offset,void *data,u64 bytes) {
    (void)ch;(void)fb; u32 slot=handle-0x500000u;
    assert(slot<32 && offset+bytes<=sizes[slot]);
    memcpy(data,storage[slot]+offset,(size_t)bytes); return true;
}
static bool nv_compute_upload(nv_channel_t *ch,u64 off,const void *p,u64 n) {
    return nv_vram_object_write(ch,H_COMPUTE_VRAM,g_compute_vram_fb,off,p,n);
}
static bool nv_compute_dispatch_one(nv_channel_t *ch,const u8 *sass,u32 len,u32 regs,u32 off,
                                    const u32 *args,u32 n,const u32 *grid,const u32 *block,u32 signal,const char *label) {
    (void)signal;(void)label; dispatches++;
    if(sass==gui_present_sass) {
        assert(len==sizeof gui_present_sass && regs==gui_present_sass_reg_count && off==0xc000 && n==19);
        assert(args[18]==0 || args[18]==90 || args[18]==180 || args[18]==270);
        assert(args[0]==(u32)g_runtime_scanout_va && args[1]==(u32)(g_runtime_scanout_va>>32));
        assert(args[7]==100 && args[8]==50 && args[9]==112);
        assert(grid[0]==(args[16]+15)/16 && grid[1]==(args[17]+15)/16);
        assert(block[0]==16 && block[1]==16 && block[2]==1 && grid[2]==1);
        return true;
    }
    /* Rounded-shape raster now needs the larger staging slot at 0x8000;
     * the separate layout test checks real shader size and non-overlap. */
    assert(sass==gui_raster_sass && len==sizeof gui_raster_sass && regs==gui_raster_sass_reg_count && off==0x8000 && n==16);
    assert(block[0]==16 && block[1]==16 && block[2]==1);
    assert(grid[0]==(args[14]+15)/16 && grid[1]==(args[15]+15)/16 && grid[2]==1);
    assert(args[4]==(u32)VA_COMPUTE && args[5]==(u32)(VA_COMPUTE>>32) && args[11]==staged_count);
    u64 va=(u64)args[0]|((u64)args[1]<<32), sva=(u64)args[2]|((u64)args[3]<<32);
    u32 slot=(u32)((va-0x30000000000ull)/0x10000000u);
    assert(slot<32 && mapped[slot] && storage[slot]);
    if(sva) { u32 src=(u32)((sva-0x30000000000ull)/0x10000000u);
        assert(src<32 && src!=slot && mapped[src]); assert(((u64)args[6]|((u64)args[7]<<32))==sizes[src]); }
    assert((u64)args[10]*args[9]*4<=sizes[slot]);
    if(failure==8) return false;
    if(failure==9) { ch->submit_failed=true; return false; }
    for(u32 y=args[13];y<args[13]+args[15];y++) for(u32 x=args[12];x<args[12]+args[14];x++) {
        for(u32 i=0;i<staged_count;i++) {kg2d_command_t *c=&staged[i];
            int64_t rx=(int64_t)x-c->x, ry=(int64_t)y-c->y;
            if(rx<0 || ry<0 || rx>=c->width || ry>=c->height) continue;
            if(c->op==KG2D_SOLID && c->opacity==255)
                ((u32 *)storage[slot])[(u64)y*args[10]+x]=c->colour0;
        }
    }
    return true;
}
/* This surface-allocation/2D fixture includes the geometry entrypoint but does
 * not exercise it. Never fake a successful geometry dispatch here: the actual
 * two-grid packet has separate coverage in test_nv_surface_dispatch.py. */
static bool nv_compute_geometry_pair(nv_channel_t *ch,const u32 *setup,
                                     const u32 *compact,u32 triangles) {
    (void)ch;(void)setup;(void)compact;(void)triangles;
    assert(!"geometry dispatch outside this fixture's scope");return false;
}
static bool ce_fill32(nv_channel_t *ch,u64 dst,u32 pitch,u32 width,u32 height,u32 colour,u32 signal) {
    assert(ch==&channels[CH_COPY] && signal==0x53430000u);
    u32 slot=(u32)((dst-0x30000000000ull)/0x10000000u);
    assert(slot<32 && mapped[slot] && copy_mapped[slot]);
    assert(width==4096 && pitch==16384 && (u64)height*pitch==sizes[slot] && !colour);
    /* Walk EVERY page in both address spaces, including the 2-MiB and 512-MiB
     * directory crossings of the scratch and subsequent desktop allocations. */
    for(u32 channel=0;channel<2;channel++)for(u64 off=0;off<sizes[slot];off+=4096) {
        nv_vmm_walk_t w;nv_vmm_walk(&channels[channel].vmm,dst+off,&w);
        assert(w.present && w.physical==0x10000000ull+slot*0x10000000ull+off);
        /* Independently decode the raw leaf kind in BOTH VASpaces. */
        nv_vmm_t *v=&channels[channel].vmm;
        u64 table=v->root_gpu, address=dst+off;
        unsigned shifts[]={56,47,38,29,21}, bits[]={1,9,9,9,8};
        for(unsigned level=0;level<5;level++) {
            u64 *entries=(u64 *)(v->pool+table-v->pool_gpu);
            u32 index=(u32)((address>>shifts[level])&((1u<<bits[level])-1));
            table=entries[level==4?index*2+1:index]&0x000ffffffffff000ull;
        }
        u64 leaf=((u64 *)(v->pool+table-v->pool_gpu))[(address>>12)&511];
        assert(((leaf>>8)&15)==returned_kind);
    }
    if(failure==8)return false;
    if(failure==9){ch->submit_failed=true;return false;}
    memset(storage[slot],0,(size_t)sizes[slot]); /* modeled CE, never production CPU clearing */
    ce_fills++;return true;
}
'''
    c += '\ntypedef struct {u32 qmd_off,shader_off,constant_off,bytes;} nv_dispatch_layout_t;\n'
    c += function(CHAN, 'nv_compute_resident_layout') + '\n'
    c += CHAN[CHAN.index('#define NV_SURFACE_SLOTS'):CHAN.index('/* Prove the compute engine executes')]
    c += r'''
static void reset(void) {
    for(u32 i=0;i<32;i++) {free(storage[i]);storage[i]=NULL;}
    memset(g_surfaces,0,sizeof g_surfaces); memset(mapped,0,sizeof mapped);
    memset(copy_mapped,0,sizeof copy_mapped);
    for(u32 i=0;i<2;i++) {
        channels[i]=(nv_channel_t){.open=true,.rm=&rm_fixture};
        assert(nv_vmm_init(&channels[i].vmm,ptpool[i],0x100000+i*0x200000,512,true));
    }
    allocs=frees=dispatches=commits=writes=ce_fills=0; failure=0;
}
int main(void) {
    reset(); nv_surface_info_t info={0};
    assert(!nv_surface_dimensions(0,10,&info)); assert(!nv_surface_dimensions(UINT32_MAX,10,&info));
    assert(!nv_surface_dimensions(32768,32768,&info));
    assert(nv_surface_dimensions(7680,4320,&info) && info.pitch==30720 && info.bytes>=132710400);
    for(u32 w=1;w<=8192;w+=113) for(u32 h=1;h<=4320;h+=131) {
        assert(nv_surface_dimensions(w,h,&info));
        assert(info.pitch>=w*4 && !(info.pitch&255) && info.bytes>=(u64)info.pitch*h && !(info.bytes&65535));
    }
    assert(!nv_surface_create(0,10,10));
    bool pressure=true;
    assert(!nv_surface_create_with_pressure(0,10,10,&pressure)&&!pressure);
    pressure=true;
    assert(!nv_surface_create_with_pressure(7,0,10,&pressure)&&!pressure);
    u64 a=nv_surface_create(7,143,75), b=nv_surface_create(7,16,16);
    assert(a && b && a!=b && nv_surface_info(7,a,&info));
    for(u32 i=0;i<info.bytes;i++) assert(storage[0][i]==0); /* includes padding */
    assert(!nv_surface_info(8,a,&info));
    u32 colour=0xff123456, got=0;
    assert(nv_surface_transfer(7,b,0,&colour,4,false));
    assert(nv_surface_transfer(7,b,0,&got,4,true) && got==colour);
    unsigned before_writes=writes;
    assert(nv_surface_present(7,a,0,0,143,75,0,0,100,50,0));
    assert(writes==before_writes); /* only the native dispatch stages shader data */
    for(u32 rotation=90;rotation<=270;rotation+=90)
        assert(nv_surface_present(7,a,0,0,143,75,0,0,100,50,rotation));
    unsigned before_dispatch=dispatches;
    assert(!nv_surface_present(7,a,0,0,143,75,0,0,100,50,45));
    assert(dispatches==before_dispatch);
    assert(!nv_surface_present(8,a,0,0,143,75,0,0,100,50,0));
    assert(!nv_surface_present(7,a,0,0,144,75,0,0,100,50,0));
    assert(!nv_surface_present(7,a,0,0,143,75,99,0,2,50,0));
    assert(!nv_surface_present(7,a,UINT32_MAX,0,1,1,0,0,1,1,0));
    g_compute_scanout_bound=false;
    assert(!nv_surface_present(7,a,0,0,143,75,0,0,100,50,0));
    g_compute_scanout_bound=true; g_runtime_scanout_bytes=1;
    assert(!nv_surface_present(7,a,0,0,143,75,0,0,100,50,0));
    g_runtime_scanout_bytes=448*50;
    assert(!nv_surface_transfer(7,b,UINT64_MAX-3,&got,4,true));
    assert(!nv_surface_transfer(7,b,1,&got,4,true));
    kg2d_command_t cmd={.x=-2,.y=-3,.width=12,.height=13,.op=KG2D_SOLID,.colour0=colour,.opacity=255};
    assert(nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    assert(((u32 *)storage[0])[0]==colour && ((u32 *)storage[0])[10]==0);
    assert(!nv_surface_draw(7,a,a,&cmd,1,0,0,143,75));
    assert(!nv_surface_draw(8,a,b,&cmd,1,0,0,143,75));
    assert(!nv_surface_draw(7,a,0,&cmd,257,0,0,143,75));
    assert(!nv_surface_draw(7,a,0,&cmd,1,UINT32_MAX,0,1,1));
    assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,UINT32_MAX,1));
    cmd.reserved[2]=1; assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75)); cmd.reserved[2]=0;
    cmd.op=KG2D_SHADOW;cmd.source_x=14;cmd.source_y=8;cmd.width=143;cmd.height=75;
    assert(nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_x=0;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_x=4097;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_x=14;cmd.source_y=4097;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_y=0;cmd.width=28;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.op=KG2D_ROUNDED;cmd.width=143;cmd.height=75;cmd.source_x=28;
    for(unsigned style=0;style<6;style++){
        cmd.source_y=style;assert(nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    }
    unsigned before_rounded=dispatches;
    cmd.source_y=6;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_y=0;cmd.source_x=38;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_y=2;cmd.source_x=72;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_x=4097;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_x=28;cmd.source_stride=1;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_stride=0;cmd.source_offset=1;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_offset=0;cmd.reserved[0]=1;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.reserved[0]=0;assert(dispatches==before_rounded);
    cmd.source_y=2;cmd.source_x=71;assert(nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    cmd.source_y=0;cmd.source_x=0;assert(nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    for(unsigned style=4;style<6;style++){
        cmd.source_y=style;cmd.source_x=38;assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
        cmd.source_x=0;assert(nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    }
    cmd.source_y=0;
    puts("PASS rounded-shape admission: six styles, zero/clamped radii, hard-shape half-height limits and malformed requests rejected before dispatch");
    cmd.op=KG2D_LINE;
    for(unsigned direction=0;direction<4;direction++){
        cmd.colour1=direction;assert(nv_surface_commands_valid(&cmd,1,NULL));
    }
    cmd.colour1=4;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.colour1=0;
    cmd.source_x=1;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.source_x=0;
    cmd.source_y=1;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.source_y=0;
    cmd.source_stride=4;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.source_stride=0;
    cmd.source_offset=4;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.source_offset=0;
    cmd.reserved[0]=1;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.reserved[0]=0;
    cmd.op=KG2D_GLYPH+1;assert(!nv_surface_commands_valid(&cmd,1,NULL));
    puts("PASS line admission: four endpoint directions, source-free records, malformed fields and unknown op rejected");
    cmd.op=KG2D_LINE_AA;
    for(unsigned flags=0;flags<4;flags++) {
        cmd.colour1=flags;cmd.width=flags&1?5:9;cmd.height=flags&1?9:5;
        assert(nv_surface_commands_valid(&cmd,1,NULL));
    }
    cmd.width=2;assert(!nv_surface_commands_valid(&cmd,1,NULL));
    cmd.width=20;assert(!nv_surface_commands_valid(&cmd,1,NULL));
    cmd.width=5;cmd.colour1=4;assert(!nv_surface_commands_valid(&cmd,1,NULL));
    cmd.colour1=3;cmd.source_stride=4;assert(!nv_surface_commands_valid(&cmd,1,NULL));cmd.source_stride=0;
    cmd.colour1=0;
    puts("PASS AA line admission: normalized axis/slope flags, positive major/minor extents, invalid slope geometry and source fields rejected");
    nv_surface_t glyph_source={.bytes=19};
    cmd.op=KG2D_GLYPH;cmd.source_x=9;cmd.source_y=2;cmd.source_stride=10;
    assert(!nv_surface_commands_valid(&cmd,1,NULL));
    assert(nv_surface_commands_valid(&cmd,1,&glyph_source)); // exact final byte
    glyph_source.bytes--;assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));glyph_source.bytes++;
    cmd.colour1=1;cmd.source_stride=2;glyph_source.bytes=4;
    assert(nv_surface_commands_valid(&cmd,1,&glyph_source)); // nine mono pixels need two bytes
    cmd.source_stride=1;assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));cmd.source_stride=2;
    cmd.colour1=2;assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));cmd.colour1=1;
    cmd.source_x=0;assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));cmd.source_x=9;
    cmd.source_y=0;assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));cmd.source_y=2;
    cmd.source_offset=1;assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));cmd.source_offset=0;
    cmd.source_y=UINT32_MAX;cmd.source_stride=UINT32_MAX;
    assert(!nv_surface_commands_valid(&cmd,1,&glyph_source));
    cmd.colour1=0;cmd.source_y=0;
    puts("PASS glyph admission: A8/bitpacked row bounds, exact capacity, missing source, malformed flags/stride/geometry and wide overflow rejected");
    cmd.source_x=0;
    cmd.op=KG2D_IMAGE;cmd.width=2;cmd.height=1;cmd.source_stride=8;
    assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,143,75));
    assert(nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.source_offset=65532; assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.source_offset=0;cmd.source_y=UINT32_MAX;cmd.height=2;
    assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.source_y=0;cmd.op=KG2D_IMAGE_SCALED;cmd.width=143;cmd.height=75;
    cmd.reserved[0]=2;cmd.reserved[1]=1;
    assert(nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.op=KG2D_ARGB_BILINEAR;cmd.colour1=0;
    assert(nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.colour1=1;assert(nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.colour1=2;assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));cmd.colour1=0;
    cmd.reserved[0]=0;assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.reserved[0]=3;assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    cmd.reserved[0]=2;cmd.reserved[1]=UINT32_MAX;
    assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,143,75));
    assert(nv_surface_destroy(7,a)); assert(!nv_surface_destroy(7,a));
    u64 newer=nv_surface_create(7,32,32); assert(newer && newer!=a);
    assert(!nv_surface_info(7,a,&info)); assert(nv_surface_release_owner(7)); assert(allocs==frees);
    u32 used=channels[0].vmm.used;
    for(u32 i=0;i<200;i++) {u64 h=nv_surface_create(11,32,32); assert(h);assert(nv_surface_destroy(11,h));}
    assert(channels[0].vmm.used==used && channels[0].vmm.mapped==0);
    for(int f=1;f<=9;f++) {
        if(f==7)continue; /* command upload belongs to DRAW, not CREATE */
        reset(); failure=f; pressure=true;
        assert(!nv_surface_create_with_pressure(7,32,32,&pressure)&&!pressure);
        if(f==9) { assert(channels[1].submit_failed && !frees && g_surfaces[0].state==NS_QUARANTINED);
            assert(!nv_surface_release_owner(7) && !frees); }
        else assert(allocs==frees && channels[0].vmm.mapped==0);
    }
    reset(); a=nv_surface_create(7,32,32); b=nv_surface_create(7,32,32); assert(a&&b);
    failure=9; cmd=(kg2d_command_t){.op=KG2D_IMAGE,.width=1,.height=1,.opacity=255,.source_stride=4};
    assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,32,32));
    assert(!nv_surface_release_owner(7) && !frees && mapped[0] && mapped[1]);
    for(int f=11;f<=14;f++) {
        if(f==13)continue;
        reset(); a=nv_surface_create(7,32,32); assert(a); failure=f;
        assert(!nv_surface_destroy(7,a) && !frees && g_surfaces[0].state==NS_QUARANTINED);
        failure=0; assert(nv_surface_release_owner(7) && frees==1);
    }
    reset(); channels[0].vmm.pages=6; /* partial map then PT exhaustion */
    assert(!nv_surface_create(7,1920,1080));
    assert(allocs==frees && channels[0].vmm.mapped==0);
    reset(); for(u32 i=0;i<32;i++) assert(nv_surface_create(7,1,1));
    assert(!nv_surface_create_with_pressure(7,1,1,&pressure)&&pressure);
    // Free one owned slot: retry must succeed and clear the previous pressure.
    assert(nv_surface_destroy(7,(g_surfaces[0].generation<<8)|1u));
    assert(nv_surface_create_with_pressure(7,1,1,&pressure)&&!pressure);
    assert(nv_surface_release_owner(7));
    assert(allocs==frees && channels[0].vmm.mapped==0);
    /* Repeated remap/unmap and holes must not inflate or underflow accounting. */
    nv_vmm_t *v=&channels[0].vmm; u64 va=VA_SURFACE_BASE;
    assert(nv_vmm_map(v,va,0x10000000,4096,true,false,false));
    assert(nv_vmm_map(v,va,0x20000000,4096,true,false,false) && v->mapped==1);
    assert(nv_vmm_unmap(v,va,65536) && v->mapped==0);
    assert(nv_vmm_unmap(v,va,65536) && v->mapped==0);
    reset(); g_surfaces[0].generation=0x00ffffffffffffffull;
    a=nv_surface_create(7,1,1); assert((a&255)==2); /* never wrap a generation */
    assert(nv_surface_release_owner(7));
    reset(); a=nv_surface_create(7,1,1); b=nv_surface_create(8,1,1); assert(a&&b);
    assert(!nv_surface_draw(7,a,b,&cmd,1,0,0,1,1));
    assert(nv_surface_release_owner(7) && !nv_surface_info(7,a,&info));
    assert(nv_surface_info(8,b,&info)); assert(nv_surface_release_owner(8));
    for(int f=13;f<=16;f++) {
        if(f==14)continue;
        reset();failure=f;assert(!nv_surface_create(7,1024,1024));
        assert(allocs==frees && !ce_fills && !dispatches);
        assert(channels[0].vmm.mapped==0 && channels[1].vmm.mapped==0);
    }
    reset(); /* Exact failed-boot allocation order, followed by desktop backbuffer. */
    assert(nv_surface_create(7,17,13));assert(nv_surface_create(7,23,19));
    assert(nv_surface_create(7,1024,1024));assert(nv_surface_create(7,7680,1440));
    assert(ce_fills==4 && dispatches==0 && writes==0);
    for(u32 s=0;s<4;s++)for(u64 i=0;i<sizes[s];i++)assert(!storage[s][i]);
    assert(nv_surface_release_owner(7) && allocs==frees);
    assert(!channels[0].vmm.mapped && !channels[1].vmm.mapped);
    for(u32 kind=0;kind<16;kind++) {
        reset();returned_kind=kind;
        u64 handle=nv_surface_create(7,17,13);
        assert(!!handle==(kind==0 || kind==6));
        assert(nv_surface_release_owner(7) && allocs==frees);
    }
    returned_kind=6;
    reset();channels[CH_COPY].open=false;pressure=true;
    assert(!nv_surface_create_with_pressure(7,10,10,&pressure)&&!pressure&&!allocs);
    reset();channels[CH_COPY].submit_failed=true;pressure=true;
    assert(!nv_surface_create_with_pressure(7,10,10,&pressure)&&!pressure&&!allocs);
    reset();a=nv_surface_create(7,32,32);assert(a);failure=7;
    cmd=(kg2d_command_t){.op=KG2D_SOLID,.width=1,.height=1,.opacity=255};
    assert(!nv_surface_draw(7,a,0,&cmd,1,0,0,1,1));failure=0;assert(nv_surface_release_owner(7));
    reset(); puts("PASS native surfaces + actual VMM: dual-channel full-page mappings, fenced CAB5 clears through desktop size, ownership, shader ABI, 200 reuse cycles, rollback and either-channel timeout quarantine (modeled RM/GPU)");
    return 0;
}
'''
    run_test(c, 'nv_surfaces')


if __name__ == '__main__':
    main()
