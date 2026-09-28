#!/usr/bin/env python3
"""Exercise the actual native launcher, not a stub of the launch boundary.

Models persistent instruction-cache contents and decodes the emitted methods.
Checks QMD/constant/shader staging, dimensional arguments and fence ordering.
This is command-contract coverage, not native GPU execution proof.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / 'kernel/nv_chan.c').read_text()


def main():
    official = (ROOT / 'out/mesa-ref/src/nouveau/headers/nvidia/classes/clcec0.h').read_text()
    assert re.search(r'NVCEC0_INVALIDATE_SHADER_CACHES\s+0x021c\b', official)
    for field, bit in (('INSTRUCTION', 0), ('DATA', 4), ('CONSTANT', 12)):
        assert re.search(r'NVCEC0_INVALIDATE_SHADER_CACHES_' + field +
                          r'\s+' + str(bit) + ':' + str(bit) + r'\b', official)
    assert re.search(r'NVCEC0_INVALIDATE_SHADER_CACHES_FLUSH_DATA\s+2:2\b',official)
    assert re.search(r'#define CEC0_INVALIDATE_SHADER_CACHES\s+0x021cu\b', SRC)
    assert re.search(r'#define CEC0_INVALIDATE_SHADER_CODE_DATA_CONST\s+0x1011u\b', SRC)
    for name, value in (('LINE_LENGTH_IN','0180'), ('LINE_COUNT','0184'),
                        ('OFFSET_OUT_UPPER','0188'), ('OFFSET_OUT','018c'),
                        ('LAUNCH_DMA','01b0'), ('LOAD_INLINE_DATA','01b4')):
        assert re.search(r'NVCEC0_' + name + r'\s+0x' + value + r'\b', official)
    assert re.search(r'NVCEC0_LAUNCH_DMA_COMPLETION_TYPE_FLUSH_ONLY\s+0x00000001\b',official)
    assert re.search(r'NVCEC0_LAUNCH_DMA_SYSMEMBAR_DISABLE\s+6:6\b',official)
    fifo=(ROOT/'out/mesa-ref/src/nouveau/headers/nvidia/classes/cl906f.h').read_text()
    assert re.search(r'NV906F_DMA_ONEINCR_OPCODE_VALUE\s+\(0x00000005\)',fifo)
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define NV_QMD_HOST_TEST
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
#define kwarn(...) ((void)0)
typedef struct { int unused; } nv_card_t;
typedef struct {
    bool open, submit_failed;
    volatile u8 *pushbuf;
    volatile u32 *sem;
    u32 pb_at, completion_seq;
    nv_card_t *card;
} nv_channel_t;
typedef struct {u32 qmd_off, shader_off, constant_off, bytes;} nv_dispatch_layout_t;
static u8 vram[0x90000], instructions[0x90000], staging[0x80000];
static u32 semaphore[4], uploads, submits, invalidates;
static u32 program_uploads[9], dynamic_uploads;
static u64 uploaded_bytes;
static u64 g_compute_vram_fb=0x800000;
static bool cache_valid, fail_upload, fail_fence, fail_reserve;
static bool runtime_inline;
static u32 inline_uploads, reserved_end;
static u32 fail_upload_call;
static const u8 *expected_shader;
static u32 expected_size, expected_regs, expected_args[32], expected_words;
static u32 expected_grid[3], expected_block[3];
static bool expect_surface;
static u32 g_render_transaction;
static bool pair_mode, awaken_enabled;
static bool force_wrap, fail_after_first;
static u32 pair_triangles, pair_setup_args[24], pair_compact_args[21];
'''
    c += SRC[SRC.index('typedef struct {\n    u32 start, end, recorded, signal;'):SRC.index('} nv_compute_pair_t;')+len('} nv_compute_pair_t;')] + '\n'
    c += '#include "' + (ROOT/'include/kestrel/shader_setup.h').as_posix() + '"\n'
    for path in ('kernel/nv_qmd.c', 'tools/shader_gui_raster.h',
                 'tools/shader_gui_present.h', 'tools/shader_gpunop.h',
                 'tools/shader_gl_raster.h', 'tools/shader_shader_vm.h', 'tools/shader_shader_raster.h', 'tools/shader_shader_raster_fast.h',
                 'tools/shader_shader_setup.h', 'kernel/shader_setup_compact_sass.h',
                 'kernel/shader_setup_compact_small_sass.h'):
        c += '\n#include "' + (ROOT / path).as_posix() + '"\n'
    for name in ('VA_COMPUTE', 'H_COMPUTE_VRAM', 'VA_SEM', 'BLACKWELL_COMPUTE_B',
                 'SUBCH_COMPUTE', 'NV_GUI_RESIDENT_OFF', 'NV_PRESENT_RESIDENT_OFF',
                 'NV_GL_RESIDENT_OFF', 'NV_VM_RESIDENT_OFF', 'NV_RASTER_RESIDENT_OFF', 'NV_RASTER_FAST_RESIDENT_OFF',
                 'NV_SETUP_RESIDENT_OFF', 'NV_COMPACT_RESIDENT_OFF',
                 'NV_COMPACT_SMALL_RESIDENT_OFF', 'NV_RESIDENT_PROGRAMS',
                 'COMPUTE_COMPLETION_AWAKEN'):
        c += re.search(r'^#define ' + name + r'\s+[^\n]+', SRC, re.M)[0] + '\n'
    c += '\n'.join(re.findall(r'^#define CEC0_[^\n]+', SRC, re.M)) + '\n'
    c += function(SRC, 'pb_method') + '\n' + function(SRC, 'pb_data') + '\n'
    c += r'''
/* No scheduler/IRQ route in this command model. */
static u32 nv_completion_awaken(nv_channel_t *ch,u32 flag) { (void)ch;return awaken_enabled?flag:0; }
static unsigned program_slot(const u8 *shader) {
    return shader==gui_raster_sass?0:shader==gui_present_sass?1:
        shader==gl_raster_sass?2:shader==shader_vm_sass?3:shader==shader_raster_sass?4:shader==shader_raster_fast_sass?5:
        shader==shader_setup_sass?6:shader==shader_setup_compact_sass?7:
        shader==shader_setup_compact_small_sass?8:NV_RESIDENT_PROGRAMS;
}
static const u32 program_offsets[NV_RESIDENT_PROGRAMS]={0x10000,0x18000,0x20000,0x30000,0x40000,0x60000,0x70000,0x80000,0x88000};
static bool ensure_compute_vram(nv_channel_t *ch) { (void)ch; return true; }
static bool nv_surface_transfer_ce_ready(void) { return runtime_inline; }
static bool pb_reserve(nv_channel_t *ch,u32 bytes) {
    if(force_wrap && !fail_reserve){ch->pb_at=0x10000;force_wrap=false;}
    assert(bytes >= 128 && ch->pb_at+bytes <= sizeof staging);
    reserved_end=ch->pb_at+bytes;return !fail_reserve;
}
static u32 next_completion_signal(nv_channel_t *ch,u32 tag) {
    return tag | ++ch->completion_seq;
}
static u32 nv_rd32(nv_card_t *card,u32 offset) { (void)card;(void)offset;return 0; }
static bool nv_vram_object_write(nv_channel_t *ch,u32 handle,u64 fb,u64 off,const void *p,u64 n) {
    (void)ch; assert(handle==H_COMPUTE_VRAM && fb==g_compute_vram_fb);
    assert(off+n<=sizeof vram); uploads++;uploaded_bytes+=n;
    if(fail_upload || uploads==fail_upload_call) {
        /* A coherent transfer may fail after touching destination memory.
         * Leave a valid prefix and corrupt tail: a retry must replace the
         * whole record/program, not merely trust the attempted upload. */
        memset(vram+off,0xa5,(size_t)n);
        memcpy(vram+off,p,(size_t)n/2);
        return false;
    }
    if(off>=0x10000) {
        const u8 *asset=pair_mode?p:expected_shader;
        unsigned slot=program_slot(asset);
        assert(slot<NV_RESIDENT_PROGRAMS && off==program_offsets[slot]);
        assert(p==asset); /* never stage resident code over method-ring bytes */
        u32 size=pair_mode?(slot==6?sizeof shader_setup_sass:
            slot==7?sizeof shader_setup_compact_sass:sizeof shader_setup_compact_small_sass):expected_size;
        if(pair_mode)assert(slot>=6 && slot<=8);
        assert(n==size);program_uploads[slot]++;
    } else if(program_slot(expected_shader)<NV_RESIDENT_PROGRAMS) {
        assert(n==0x800);dynamic_uploads++;
    }
    memcpy(vram+off,p,(size_t)n);return true;
}
/* Transport is modeled here; this fixture checks QMD/code publication order. */
static bool nv_compute_upload(nv_channel_t *ch,u64 off,const void *p,u64 n) {
    return nv_vram_object_write(ch,H_COMPUTE_VRAM,g_compute_vram_fb,off,p,n);
}
static bool submit_and_wait(nv_channel_t *ch,u32 start,u32 signal) {
    submits++; assert(!ch->sem[0]);
    assert(ch->pb_at<=reserved_end);
    bool launched=false,idle=false,reported=false,invalidated=false;
    u32 qmd_off=0,sem_lo=0,sem_hi=0,payload=0;
    u32 dma_bytes=0,dma_count=0,dma_hi=0,dma_lo=0,dma_written=0;
    bool dma_started=false;
    bool producer_flushed=false;
    u32 completed_grids=0, prior_cookie=0;
    for(u32 off=start;off<ch->pb_at;) {
        u32 header=*(u32 *)(staging+off);off+=4;
        u32 opcode=header>>29;
        assert((opcode==1 || opcode==5) && ((header>>13)&7)==SUBCH_COMPUTE);
        u32 count=(header>>16)&0x1fff, method=(header&0x1fff)*4;
        assert(off+count*4<=ch->pb_at);
        if(opcode==5)assert(method==CEC0_LAUNCH_DMA && count==513);
        for(u32 i=0;i<count;i++,off+=4) {
            if(i && (opcode==1 || i==1))method+=4;
            u32 value=*(u32 *)(staging+off);
            if(method==CEC0_SET_OBJECT && completed_grids) {
                assert(pair_mode && completed_grids==1 && launched && idle && reported && producer_flushed);
                launched=idle=reported=invalidated=dma_started=false;
                dma_written=dma_bytes=dma_count=dma_hi=dma_lo=0;
            }
            if(method==CEC0_LINE_LENGTH_IN) {assert(!launched);dma_bytes=value;}
            else if(method==CEC0_LINE_LENGTH_IN+4)dma_count=value;
            else if(method==CEC0_LINE_LENGTH_IN+8)dma_hi=value;
            else if(method==CEC0_LINE_LENGTH_IN+12)dma_lo=value;
            else if(method==CEC0_LAUNCH_DMA) {
                assert(!launched && !dma_started && dma_bytes==0x800 && dma_count==1);
                assert(value==0x11 && !(value&(1u<<6))); // PITCH, FLUSH_ONLY, SYSMEMBAR enabled
                u64 address=((u64)dma_hi<<32)|dma_lo;
                assert(address>=VA_COMPUTE && address+dma_bytes<=VA_COMPUTE+0x10000);
                dma_started=true;inline_uploads++;
            } else if(method==CEC0_LAUNCH_DMA+4) {
                assert(dma_started && !launched && dma_written+4<=dma_bytes);
                u64 address=(((u64)dma_hi<<32)|dma_lo)-VA_COMPUTE;
                memcpy(vram+address+dma_written,&value,4);dma_written+=4;
            } else if(method==0x021c) {
                /* NVIDIA CEC0 instruction bit 0, data bit 4, constant bit 12. */
                if(pair_mode && launched) {
                    assert(!completed_grids && idle && value==0x14 && !reported);
                    producer_flushed=true;
                    continue;
                }
                assert(!launched && (value&0x1011)==0x1011);
                cache_valid=false; invalidated=true; invalidates++;
            } else if(method==CEC0_SEND_PCAS_A) {
                u64 address=(u64)value<<8;assert(address>=VA_COMPUTE);
                qmd_off=(u32)(address-VA_COMPUTE);
            } else if(method==CEC0_SEND_PCAS2_B) {
                if(pair_mode) {
                    bool compact=completed_grids!=0,small=pair_triangles<=32;
                    expected_shader=!compact?shader_setup_sass:small?shader_setup_compact_small_sass:shader_setup_compact_sass;
                    expected_size=!compact?sizeof shader_setup_sass:small?sizeof shader_setup_compact_small_sass:sizeof shader_setup_compact_sass;
                    expected_regs=!compact?shader_setup_sass_reg_count:small?shader_setup_compact_small_sass_reg_count:shader_setup_compact_sass_reg_count;
                    expected_words=compact?21:24;expect_surface=true;
                    memcpy(expected_args,compact?pair_compact_args:pair_setup_args,expected_words*4);
                    expected_grid[0]=(pair_triangles+63)/64;expected_grid[1]=expected_grid[2]=1;
                    expected_block[0]=64;expected_block[1]=expected_block[2]=1;
                    assert(qmd_off==(compact?0xc00:0x400)); // disjoint immutable QMD/CB slots
                }
                assert(!launched && value==CEC0_PCAS2_ACTION_ICS);
                if(runtime_inline && program_slot(expected_shader)<NV_RESIDENT_PROGRAMS)
                    assert(dma_started && dma_written==0x800);
                else assert(!dma_started && !dma_written);
                u32 *q=(u32 *)(vram+qmd_off);
                u64 prog=((u64)nv_qmd_get(q,NV_QMD_F_PROGRAM_ADDRESS_UPPER)<<32)|
                                   nv_qmd_get(q,NV_QMD_F_PROGRAM_ADDRESS_LOWER);
                prog=(prog<<4)-VA_COMPUTE;
                assert(prog+expected_size<=sizeof vram);
                assert(!memcmp(vram+prog,expected_shader,expected_size));
                if(!cache_valid){memcpy(instructions,vram,sizeof vram);cache_valid=true;}
                /* A missing invalidation executes the previous shader here. */
                assert(!memcmp(instructions+prog,expected_shader,expected_size));
                if(!expect_surface)assert(!invalidated);
                u64 cb=((u64)nv_qmd_get(q,NV_QMD_F_CB_ADDR_UPPER(0))<<32)|
                                  nv_qmd_get(q,NV_QMD_F_CB_ADDR_LOWER(0));
                cb=(cb<<6)-VA_COMPUTE;
                if(program_slot(expected_shader)<NV_RESIDENT_PROGRAMS) {
                    assert(prog==program_offsets[program_slot(expected_shader)]);
                    assert(cb==qmd_off+0x400 && cb+0x400<=0x10000);
                } else assert(cb>=prog+expected_size && cb+0x400<=0x10000);
                assert(nv_qmd_get(q,NV_QMD_F_REGISTER_COUNT)==expected_regs);
                assert(nv_qmd_get(q,NV_QMD_F_GRID_WIDTH)==expected_grid[0]);
                assert(nv_qmd_get(q,NV_QMD_F_GRID_HEIGHT)==expected_grid[1]);
                assert(nv_qmd_get(q,NV_QMD_F_GRID_DEPTH)==expected_grid[2]);
                assert(nv_qmd_get(q,NV_QMD_F_CTA_THREAD_DIMENSION0)==expected_block[0]);
                assert(nv_qmd_get(q,NV_QMD_F_CTA_THREAD_DIMENSION1)==expected_block[1]);
                assert(nv_qmd_get(q,NV_QMD_F_CTA_THREAD_DIMENSION2)==expected_block[2]);
                if(expected_shader==gl_raster_sass || expected_shader==shader_raster_sass || expected_shader==shader_raster_fast_sass || expected_shader==gui_raster_sass || expected_shader==shader_setup_sass) {
                    const u8 *bank=expected_shader==shader_setup_sass?shader_setup_sass_const2:expected_shader==gui_raster_sass?gui_raster_sass_const2:expected_shader==gl_raster_sass?gl_raster_sass_const2:expected_shader==shader_raster_fast_sass?shader_raster_fast_sass_const2:shader_raster_sass_const2;
                    u32 size=expected_shader==shader_setup_sass?sizeof shader_setup_sass_const2:expected_shader==gui_raster_sass?sizeof gui_raster_sass_const2:expected_shader==gl_raster_sass?sizeof gl_raster_sass_const2:expected_shader==shader_raster_fast_sass?sizeof shader_raster_fast_sass_const2:sizeof shader_raster_sass_const2;
                    assert(nv_qmd_get(q,NV_QMD_F_CB_VALID(2)));
                    u64 table=((u64)nv_qmd_get(q,NV_QMD_F_CB_ADDR_UPPER(2))<<32) |
                                    nv_qmd_get(q,NV_QMD_F_CB_ADDR_LOWER(2));
                    table=(table<<6)-VA_COMPUTE;
                    assert(table==qmd_off+0x200 && table+size<=prog);
                    assert(nv_qmd_get(q,NV_QMD_F_CB_SIZE(2))*16>=size);
                    assert(!memcmp(vram+table,bank,size));
                } else assert(!nv_qmd_get(q,NV_QMD_F_CB_VALID(2)));
                assert(!memcmp(vram+cb+0x380,expected_args,expected_words*4));
                assert(!memcmp(vram+cb+0x360,expected_block,sizeof expected_block));
                /* Check reserved bytes too: the failed upload poisons the
                 * entire CB0, not only the argument subset checked above. */
                u32 expected_cb[0x400/4]={0};
                expected_cb[0x2f4/4]=1;
                expected_cb[0x2f8/4]=0xff000000u;
                expected_cb[0x37c/4]=0x00fffdc0u;
                memcpy((u8 *)expected_cb+0x360,expected_block,sizeof expected_block);
                memcpy((u8 *)expected_cb+0x380,expected_args,expected_words*4);
                assert(!memcmp(vram+cb,expected_cb,sizeof expected_cb));
                launched=true;
            } else if(method==CEC0_WAIT_FOR_IDLE) {assert(launched);idle=true;}
            else if(method==CEC0_REPORT_SEM_ADDR_LO)sem_lo=value;
            else if(method==CEC0_REPORT_SEM_ADDR_LO+4)sem_hi=value;
            else if(method==CEC0_REPORT_SEM_PAY_LO)payload=value;
            else if(method==CEC0_REPORT_SEM_EXECUTE) {
                bool final=!pair_mode || completed_grids==1;
                u32 awake=awaken_enabled&&final?COMPUTE_COMPLETION_AWAKEN:0;
                assert(launched && idle && value==(CEC0_REPORT_SEM_RELEASE_1W|awake));
                assert(payload && payload!=prior_cookie);
                if(final)assert(payload==signal);else assert(payload!=signal);
                assert((((u64)sem_hi<<32)|sem_lo)==VA_SEM);reported=true;
                prior_cookie=payload;completed_grids++;
                if(pair_mode && fail_after_first && completed_grids==1) {
                    ch->sem[0]=payload;ch->submit_failed=true;return false;
                }
            }
        }
    }
    assert(launched && idle && reported);
    assert(completed_grids==(pair_mode?2u:1u));
    if(fail_fence){ch->submit_failed=true;return false;}
    ch->sem[0]=signal;return true;
}
'''
    c += function(SRC, 'nv_compute_dispatch_layout') + '\n'
    c += function(SRC, 'nv_compute_resident_layout') + '\n'
    c += function(SRC, 'nv_compute_inline_upload') + '\n'
    c += re.search(r'^#define NV_GUI_DISPATCH_OFF\s+[^\n]+',SRC,re.M)[0] + '\n'
    c += function(SRC, 'nv_compute_dispatch_record') + '\n'
    c += function(SRC, 'nv_compute_dispatch_one') + '\n'
    c += function(SRC, 'nv_compute_geometry_pair') + '\n'
    c += r'''
static bool launch(nv_channel_t *ch,const u8 *shader,u32 bytes,u32 regs,u32 height,bool surface) {
    expected_shader=shader;expected_size=bytes;expected_regs=regs;expect_surface=surface;
    expected_words=(shader==shader_vm_sass||shader==shader_raster_sass||shader==shader_raster_fast_sass)?24:shader==gl_raster_sass?22:shader==gui_present_sass?19:16;
    for(u32 i=0;i<expected_words;i++)expected_args[i]=0x13570000+i+ch->completion_seq*0x100;
    expected_args[0]=0x20000000;expected_args[1]=0x300;
    expected_grid[0]=256;expected_grid[1]=(height+15)/16;expected_grid[2]=1;
    expected_block[0]=16;expected_block[1]=16;expected_block[2]=1;
    if(shader==shader_vm_sass||shader==shader_raster_sass||shader==shader_raster_fast_sass){expected_grid[0]=3;expected_grid[1]=1;expected_block[0]=64;expected_block[1]=1;}
    u32 placement=shader==gui_raster_sass?NV_GUI_DISPATCH_OFF:(shader==shader_raster_sass||shader==shader_raster_fast_sass)?0x400:shader==shader_vm_sass?0x4000:surface&&shader!=gl_raster_sass?0xc000:0x8000;
    return nv_compute_dispatch_one(ch,shader,bytes,regs,placement,
        expected_args,expected_words,expected_grid,expected_block,0x47320000,
        surface?"surface/test":"legacy/test");
}
int main(void) {
    nv_channel_t ch={.open=true,.pushbuf=staging,.sem=semaphore,.pb_at=0x10000};
    /* Resident privilege is identity+exact length, never "large code" alone.
     * Reject invalid dynamic bounds before touching VRAM or the method ring. */
    static const u8 untrusted_large_code[0x10000]={0};
    u32 no_args[33]={0};u32 at=ch.pb_at;
    assert(!nv_compute_dispatch_one(&ch,untrusted_large_code,sizeof untrusted_large_code,
        32,0x400,no_args,24,NULL,NULL,0,"nonresident-large"));
    assert(!nv_compute_dispatch_one(&ch,shader_raster_sass,0x10010,
        32,0x400,no_args,24,NULL,NULL,0,"wrong-resident-size"));
    assert(!nv_compute_dispatch_one(&ch,shader_raster_sass,sizeof shader_raster_sass,
        32,0xf900,no_args,24,NULL,NULL,0,"resident-staging-overlap"));
    assert(!nv_compute_dispatch_one(&ch,shader_raster_sass,sizeof shader_raster_sass,
        32,0x401,no_args,24,NULL,NULL,0,"resident-alignment"));
    assert(!nv_compute_dispatch_one(&ch,shader_raster_sass,sizeof shader_raster_sass,
        32,0x400,no_args,33,NULL,NULL,0,"resident-argument-overrun"));
    assert(!uploads && !submits && ch.pb_at==at);
    assert(!nv_compute_inline_upload(&ch,0x401,staging,0x800));
    assert(!nv_compute_inline_upload(&ch,0xf900,staging,0x800));
    assert(!nv_compute_inline_upload(&ch,0x400,staging,0x7fc));
    assert(!nv_compute_inline_upload(&ch,0x400,staging,0x804));
    assert(!nv_compute_inline_upload(&ch,0x400,staging+1,0x800));
    assert(!nv_compute_inline_upload(&ch,0x400,NULL,0x800));
    assert(!nv_compute_inline_upload(NULL,0x400,staging,0x800));
    assert(ch.pb_at==at);
    assert(launch(&ch,gpunop_sass,sizeof gpunop_sass,gpunop_sass_reg_count,16,false));
    assert(!invalidates); /* original boot path before any surface shader */
    assert(launch(&ch,shader_vm_sass,sizeof shader_vm_sass,shader_vm_sass_reg_count,1,true));
    assert(invalidates==1); /* VM must invalidate even before any GUI shader */
    u32 before=submits;
    u32 before_uploads=uploads, before_cursor=ch.pb_at;
    fail_reserve=true;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,4,true));
    assert(submits==before&&uploads==before_uploads&&ch.pb_at==before_cursor);
    assert(!program_uploads[0]);fail_reserve=false;
    fail_upload=true;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,4,true));
    assert(submits==before&&!program_uploads[0]&&ch.pb_at==before_cursor);
    assert(!memcmp(vram+0x10000,gui_raster_sass,sizeof gui_raster_sass/2));
    assert(vram[0x10000+sizeof gui_raster_sass-1]==0xa5);fail_upload=false;
    /* Program upload succeeds but changing QMD upload fails. Retain only the
     * successful immutable program, retry dynamic data, and never submit stale QMD. */
    fail_upload_call=uploads+2;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,4,true));
    assert(submits==before&&program_uploads[0]==1&&ch.pb_at==before_cursor);
    assert(!memcmp(vram+0x10000,gui_raster_sass,sizeof gui_raster_sass));
    assert(vram[NV_GUI_DISPATCH_OFF+0x7ff]==0xa5);fail_upload_call=0;
    /* Same allocation sequence as the failed boot, then shader swaps at C400. */
    for(u32 i=0;i<3;i++)assert(launch(&ch,gui_raster_sass,sizeof gui_raster_sass,
                                    gui_raster_sass_reg_count,i==2?256:4,true));
    for(u32 i=0;i<100;i++) {
        assert(launch(&ch,gui_present_sass,sizeof gui_present_sass,gui_present_sass_reg_count,1440,true));
        assert(launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,1440,true));
    }
    assert(invalidates==204 && submits==205 && uploads==210);
    assert(program_uploads[0]==1&&program_uploads[1]==1&&program_uploads[3]==1&&dynamic_uploads==204);
    /* A partial first 3D program transfer must not establish residency. */
    before=submits;before_cursor=ch.pb_at;fail_upload=true;
    assert(!launch(&ch,gl_raster_sass,sizeof gl_raster_sass,gl_raster_sass_reg_count,49,true));
    assert(submits==before && ch.pb_at==before_cursor && !program_uploads[2]);
    assert(!memcmp(vram+0x20000,gl_raster_sass,sizeof gl_raster_sass/2));
    assert(vram[0x20000+sizeof gl_raster_sass-1]==0xa5);fail_upload=false;
    for(u32 i=0;i<50;i++) {
        assert(launch(&ch,gl_raster_sass,sizeof gl_raster_sass,gl_raster_sass_reg_count,49,true));
        assert(launch(&ch,gui_present_sass,sizeof gui_present_sass,gui_present_sass_reg_count,1440,true));
    }
    assert(launch(&ch,gpunop_sass,sizeof gpunop_sass,gpunop_sass_reg_count,16,true));
    assert(invalidates==305); /* returning to legacy after surface code must invalidate */
    for(u32 i=0;i<50;i++){
        assert(launch(&ch,shader_vm_sass,sizeof shader_vm_sass,shader_vm_sass_reg_count,1,true));
        assert(launch(&ch,gl_raster_sass,sizeof gl_raster_sass,gl_raster_sass_reg_count,49,true));
    }
    assert(invalidates==405); /* larger VM plus constant-bank-2 raster restoration */
    /* Largest shader: failed partial high-slot transfer, then a failed QMD
     * transfer after successful residency. Neither attempt may emit methods. */
    before=submits;before_cursor=ch.pb_at;fail_upload=true;
    assert(!launch(&ch,shader_raster_sass,sizeof shader_raster_sass,shader_raster_sass_reg_count,1,true));
    assert(submits==before && ch.pb_at==before_cursor && !program_uploads[4]);
    assert(!memcmp(vram+0x40000,shader_raster_sass,sizeof shader_raster_sass/2));
    assert(vram[0x40000+sizeof shader_raster_sass-1]==0xa5);fail_upload=false;
    fail_upload_call=uploads+2;
    assert(!launch(&ch,shader_raster_sass,sizeof shader_raster_sass,shader_raster_sass_reg_count,1,true));
    assert(submits==before && ch.pb_at==before_cursor && program_uploads[4]==1);
    assert(vram[0xbff]==0xa5);fail_upload_call=0;
    for(u32 i=0;i<50;i++){
        assert(launch(&ch,shader_raster_sass,sizeof shader_raster_sass,shader_raster_sass_reg_count,1,true));
        assert(launch(&ch,shader_vm_sass,sizeof shader_vm_sass,shader_vm_sass_reg_count,1,true));
        assert(launch(&ch,gl_raster_sass,sizeof gl_raster_sass,gl_raster_sass_reg_count,49,true));
    }
    assert(invalidates==555);
    before=submits;before_cursor=ch.pb_at;fail_upload=true;
    assert(!launch(&ch,shader_raster_fast_sass,sizeof shader_raster_fast_sass,shader_raster_fast_sass_reg_count,1,true));
    assert(submits==before && ch.pb_at==before_cursor && !program_uploads[5]);fail_upload=false;
    for(unsigned i=0;i<50;i++){
        assert(launch(&ch,shader_raster_fast_sass,sizeof shader_raster_fast_sass,shader_raster_fast_sass_reg_count,1,true));
        assert(launch(&ch,shader_raster_sass,sizeof shader_raster_sass,shader_raster_sass_reg_count,1,true));
    }
    assert(invalidates==655);
    assert(!memcmp(vram+0x60000,shader_raster_fast_sass,sizeof shader_raster_fast_sass));
    /* Cover all current geometry programs, including high-slot boundaries,
     * failed publication, and immutable residency across repeat dispatches. */
    const u8 *extra[]={shader_setup_sass,shader_setup_compact_sass,shader_setup_compact_small_sass};
    const u32 sizes[]={sizeof shader_setup_sass,sizeof shader_setup_compact_sass,sizeof shader_setup_compact_small_sass};
    const u32 regs[]={shader_setup_sass_reg_count,shader_setup_compact_sass_reg_count,shader_setup_compact_small_sass_reg_count};
    for(unsigned i=0;i<3;i++) {
        before=submits;before_cursor=ch.pb_at;fail_upload=true;
        assert(!launch(&ch,extra[i],sizes[i],regs[i],1,true));
        assert(submits==before && ch.pb_at==before_cursor && !program_uploads[i+6]);
        fail_upload=false;
        assert(launch(&ch,extra[i],sizes[i],regs[i],1,true));
        assert(launch(&ch,extra[i],sizes[i],regs[i],1,true));
        assert(program_uploads[i+6]==1);
        assert(!memcmp(vram+program_offsets[i+6],extra[i],sizes[i]));
    }
    assert(invalidates==661);
    /* Resident runtime launches upload dynamic bytes as inline DMA on GR,
     * before PCAS, with one final compute retirement and no COPY upload. */
    runtime_inline=true;
    const u8 *runtime_shaders[]={gui_raster_sass,gui_present_sass,gl_raster_sass,
        shader_vm_sass,shader_raster_sass,shader_raster_fast_sass,
        shader_setup_sass,shader_setup_compact_sass,shader_setup_compact_small_sass};
    const u32 runtime_sizes[]={sizeof gui_raster_sass,sizeof gui_present_sass,sizeof gl_raster_sass,
        sizeof shader_vm_sass,sizeof shader_raster_sass,sizeof shader_raster_fast_sass,
        sizeof shader_setup_sass,sizeof shader_setup_compact_sass,sizeof shader_setup_compact_small_sass};
    const u32 runtime_regs[]={gui_raster_sass_reg_count,gui_present_sass_reg_count,gl_raster_sass_reg_count,
        shader_vm_sass_reg_count,shader_raster_sass_reg_count,shader_raster_fast_sass_reg_count,
        shader_setup_sass_reg_count,shader_setup_compact_sass_reg_count,shader_setup_compact_small_sass_reg_count};
    before_uploads=uploads;before_cursor=ch.pb_at;before=submits;fail_reserve=true;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,16,true));
    assert(ch.pb_at==before_cursor && submits==before && uploads==before_uploads && !inline_uploads);
    fail_reserve=false;
    for(unsigned repeat=0;repeat<5;repeat++)for(unsigned slot=0;slot<9;slot++) {
        memset(vram,0xa5,0x10000); // stale QMD/CB cannot pass on prior contents
        assert(launch(&ch,runtime_shaders[slot],runtime_sizes[slot],runtime_regs[slot],16,true));
    }
    assert(inline_uploads==45 && submits==before+45 && uploads==before_uploads);
    assert(launch(&ch,gpunop_sass,sizeof gpunop_sass,gpunop_sass_reg_count,16,true));
    assert(inline_uploads==45 && uploads==before_uploads+1); // nonresident path unchanged
    runtime_inline=false;
    before=submits;fail_upload=true;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,256,true));
    assert(submits==before);fail_upload=false;fail_fence=true;runtime_inline=true;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,256,true));
    assert(ch.submit_failed);before=uploads;
    u32 stopped_cursor=ch.pb_at,stopped_inline=inline_uploads,stopped_submits=submits;
    assert(!launch(&ch,gui_raster_sass,sizeof gui_raster_sass,gui_raster_sass_reg_count,256,true));
    assert(uploads==before && ch.pb_at==stopped_cursor && inline_uploads==stopped_inline && submits==stopped_submits);
    for(unsigned slot=0;slot<NV_RESIDENT_PROGRAMS;slot++)assert(program_uploads[slot]==1);
    assert(!memcmp(vram+0x10000,gui_raster_sass,sizeof gui_raster_sass));
    assert(!memcmp(vram+0x18000,gui_present_sass,sizeof gui_present_sass));
    assert(!memcmp(vram+0x20000,gl_raster_sass,sizeof gl_raster_sass));
    assert(!memcmp(vram+0x30000,shader_vm_sass,sizeof shader_vm_sass));
    assert(!memcmp(vram+0x40000,shader_raster_sass,sizeof shader_raster_sass));
    printf("PASS native launch packet model: nine programs, 45 warm inline-DMA launches without separate upload, reserved-byte bounds, fresh constants, boot/legacy transfers, QMD/ABI, shader switches, fence order and quarantine; %llu separate-upload bytes including injected failures; NOT native execution/performance proof\n",(unsigned long long)uploaded_bytes);
}
'''
    run_test(c, 'surface_dispatch')
    # Fresh process/static state: cover first-use residency under inline mode,
    # rather than relying only on programs already uploaded by the boot cases.
    cold = c[:c.index('int main(void) {')] + r'''
int main(void) {
    nv_channel_t ch={.open=true,.pushbuf=staging,.sem=semaphore,.pb_at=0x10000};
    runtime_inline=true;
    const u8 *programs[]={gui_raster_sass,gui_present_sass,gl_raster_sass,
        shader_vm_sass,shader_raster_sass,shader_raster_fast_sass,
        shader_setup_sass,shader_setup_compact_sass,shader_setup_compact_small_sass};
    const u32 sizes[]={sizeof gui_raster_sass,sizeof gui_present_sass,sizeof gl_raster_sass,
        sizeof shader_vm_sass,sizeof shader_raster_sass,sizeof shader_raster_fast_sass,
        sizeof shader_setup_sass,sizeof shader_setup_compact_sass,sizeof shader_setup_compact_small_sass};
    const u32 regs[]={gui_raster_sass_reg_count,gui_present_sass_reg_count,gl_raster_sass_reg_count,
        shader_vm_sass_reg_count,shader_raster_sass_reg_count,shader_raster_fast_sass_reg_count,
        shader_setup_sass_reg_count,shader_setup_compact_sass_reg_count,shader_setup_compact_small_sass_reg_count};
    for(unsigned i=0;i<9;i++) {
        u32 before=ch.pb_at,old_submits=submits,old_inline=inline_uploads;
        fail_upload=true;
        assert(!launch(&ch,programs[i],sizes[i],regs[i],16,true));
        assert(ch.pb_at==before && submits==old_submits && inline_uploads==old_inline && !program_uploads[i]);
        fail_upload=false;
        assert(launch(&ch,programs[i],sizes[i],regs[i],16,true));
        assert(program_uploads[i]==1 && inline_uploads==old_inline+1);
        u32 old_uploads=uploads;
        assert(launch(&ch,programs[i],sizes[i],regs[i],16,true));
        assert(uploads==old_uploads && program_uploads[i]==1 && inline_uploads==old_inline+2);
    }
    assert(!dynamic_uploads && inline_uploads==18);
    puts("PASS cold inline-DMA packet model: nine first-use program failures/replacements and 18 ordered launches, no separate dynamic upload; NOT native hardware proof");
}
'''
    run_test(cold,'surface_dispatch_inline_cold')
    paired = c[:c.index('int main(void) {')] + r'''
int main(void) {
    nv_channel_t ch={.open=true,.pushbuf=staging,.sem=semaphore,.pb_at=0x10000};
    runtime_inline=pair_mode=awaken_enabled=true;
    pair_triangles=12;
    for(unsigned i=0;i<24;i++)pair_setup_args[i]=0x12340000+i;
    for(unsigned i=0;i<21;i++)pair_compact_args[i]=0x56780000+i;
    u32 original=ch.pb_at;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,12)); // transaction required
    g_render_transaction=1;
    assert(!nv_compute_geometry_pair(NULL,pair_setup_args,pair_compact_args,12));
    assert(!nv_compute_geometry_pair(&ch,NULL,pair_compact_args,12));
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,NULL,12));
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,0));
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,KSHS_MAX_TRIANGLES+1));
    ch.open=false;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,12));ch.open=true;
    assert(!submits && !uploads && ch.pb_at==original);
    nv_compute_pair_t invalid={original,original+2208,0,0};
    u32 one[3]={1,1,1};
    assert(!nv_compute_dispatch_record(&ch,shader_setup_sass,sizeof shader_setup_sass,
        shader_setup_sass_reg_count,0xc00,pair_setup_args,24,one,one,1,"bad-slot",&invalid));
    invalid.end=original+2207;
    assert(!nv_compute_dispatch_record(&ch,shader_setup_sass,sizeof shader_setup_sass,
        shader_setup_sass_reg_count,0x400,pair_setup_args,24,one,one,1,"short-reserve",&invalid));
    invalid.end=original+4416;invalid.recorded=2;
    assert(!nv_compute_dispatch_record(&ch,shader_setup_sass,sizeof shader_setup_sass,
        shader_setup_sass_reg_count,0x400,pair_setup_args,24,one,one,1,"third-grid",&invalid));
    assert(!submits && !uploads && ch.pb_at==original);
    fail_reserve=true;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,12));fail_reserve=false;
    assert(!submits && !uploads && ch.pb_at==original);
    fail_upload=true;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,12));fail_upload=false;
    assert(!submits && !inline_uploads && ch.pb_at==original && !program_uploads[6]);
    // First stage records, second resident upload fails. Nothing was published.
    fail_upload_call=uploads+2;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,12));fail_upload_call=0;
    assert(!submits && !inline_uploads && ch.pb_at==original);
    assert(program_uploads[6]==1 && !program_uploads[8]);
    assert(vram[0x400]==0 && vram[0xc00]==0);
    assert(nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,12));
    assert(submits==1 && inline_uploads==2 && program_uploads[6]==1 && program_uploads[8]==1);
    const u32 cases[]={1,12,32,33,64,KSHS_MAX_TRIANGLES};
    for(unsigned repeat=0;repeat<4;repeat++)for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        pair_triangles=cases[i];pair_setup_args[2]++;pair_compact_args[2]++;
        memset(vram,0xa5,0x10000); // cannot use stale constants from either stage
        if(i==3) {ch.pb_at=0x7f000;force_wrap=true;}
        u32 before=submits, transfers=inline_uploads;
        assert(nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,pair_triangles));
        assert(submits==before+1 && inline_uploads==transfers+2);
    }
    assert(program_uploads[6]==1 && program_uploads[7]==1 && program_uploads[8]==1);
    assert(!dynamic_uploads);
    // Intermediate release is NOT success. Keep the published packet and quarantine.
    u32 before=ch.pb_at;fail_after_first=true;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,pair_triangles));
    assert(ch.submit_failed && ch.pb_at>before && ch.sem[0]);
    u32 stopped=submits,stopped_uploads=uploads,stopped_cursor=ch.pb_at;
    assert(!nv_compute_geometry_pair(&ch,pair_setup_args,pair_compact_args,pair_triangles));
    assert(submits==stopped && uploads==stopped_uploads && ch.pb_at==stopped_cursor);
    puts("PASS geometry pair: cold/partial uploads, prepublication rollback, 25 two-grid/one-submit packets, distinct QMDs/constants, WFI+flush between grids, final-only awaken, wrap reservation, exact final fence and quarantine; NOT native speedup proof");
}
'''
    run_test(paired,'surface_dispatch_geometry_pair')


if __name__ == '__main__':
    main()
