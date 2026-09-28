#!/usr/bin/env python3
"""Execute real VM admission/packing with modeled owned allocations and fences."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function,run_test
ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/'kernel/nv_chan.c').read_text()

if __name__=='__main__':
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef uint64_t u64;
typedef struct {bool open,submit_failed;} nv_channel_t;
typedef struct {u64 bytes,va,fb;u32 memory,state;bool copy_mapping_attempted;} nv_surface_t;
typedef struct {u32 qmd_off,shader_off,constant_off,bytes;} nv_dispatch_layout_t;
#define CH_GFX 0
#define CH_COPY 1
#define NS_READY 1
#define UPLOAD_STAGE_BYTES 0x10000u
#define VA_UPLOAD_STAGE 0x90000000000ull
static nv_channel_t channels[2]={{true,false},{true,false}};
static nv_surface_t surfaces[3]={{0x1000000,0x30000000000ull,0x5000000,21,0,false},
    {0x10000,0x30010000000ull,0x6000000,22,0,false},{0x10000,0x30020000000ull,0x7000000,23,0,false}};
static nv_surface_t *nv_surface_lookup(u64 owner,u64 h){return owner==9&&h>=101&&h<=103?&surfaces[h-101]:NULL;}
static unsigned ensures,uploads,launches;static int failure;
static unsigned char stage[0x10000],status[0x10000];
static u64 g_compute_vram_fb=0x1000000;
static bool ensure_compute_vram(nv_channel_t *ch){assert(ch==channels);ensures++;return failure!=1;}
static bool batch_mode,g_render_transaction=true;
static bool nv_surface_transfer_ce_ready(void){return batch_mode;}
static unsigned batches,mapping_checks;
static unsigned char upload_stage[UPLOAD_STAGE_BYTES];
static volatile unsigned char *g_upload_stage=upload_stage;
static bool nv_compute_copy_mapping(nv_channel_t *ch){assert(ch==channels);mapping_checks++;return failure!=7;}
static void cache_flush(const volatile void *p,u32 n){assert(p==g_upload_stage&&n<=UPLOAD_STAGE_BYTES);}
typedef struct {u64 src,dst;u32 src_pitch,dst_pitch,line,lines;bool fill;} nv_ce_prepare_t;
'''
    for path in ['include/kestrel/shader_vm.h','tools/shader_shader_vm.h']:
        code+='\n#include "'+(ROOT/path).as_posix()+'"\n'
    for name in ['VA_COMPUTE','H_COMPUTE_VRAM']:
        code+=re.search(r'^#define\s+'+name+r'\s+[^\n]*',SRC,re.M)[0]+'\n'
    code+=r'''
static const ksh_dispatch_t *expected;
static bool ce_prepare_batch(nv_channel_t *ch,const nv_ce_prepare_t *ops,u32 count){
    assert(ch==channels+CH_COPY);batches++;
    assert(count==(expected->texture_count?3u:2u));
    assert(ops[0].fill && ops[0].dst==surfaces[1].va && ops[0].line==expected->lanes*2u);
    assert(ops[0].src_pitch==expected->lanes*8u && ops[0].dst_pitch==ops[0].src_pitch);
    for(u32 i=0;i<count;i++)assert(ops[i].lines==1u);
    if(failure==8){ch->submit_failed=true;return false;}
    memset(status,0,expected->lanes*8u);
    for(u32 i=1;i<count;i++) {
        const nv_ce_prepare_t *p=&ops[i];
        assert(!p->fill && p->src>=VA_UPLOAD_STAGE && p->src-VA_UPLOAD_STAGE+p->line<=UPLOAD_STAGE_BYTES);
        assert(p->src_pitch==p->line && p->dst_pitch==p->line);
        assert(p->dst==VA_COMPUTE+(i==1?0u:0xe000u));
        assert(p->line==(i==1?expected->code_count*16u:expected->texture_count*32u));
        memcpy(stage+(p->dst-VA_COMPUTE),upload_stage+(p->src-VA_UPLOAD_STAGE),p->line);
    }
    return true;
}
static bool nv_vram_object_write(nv_channel_t *ch,u32 obj,u64 fb,u64 off,const void *p,u64 n){
    assert(ch==channels);uploads++;
    if(obj==surfaces[1].memory){
        assert(fb==surfaces[1].fb && !off && n==expected->lanes*8 && n<=sizeof status);
        const unsigned char *v=p;for(u64 i=0;i<n;i++)assert(v[i]==0);
        if(failure==2)return false;memcpy(status,p,(size_t)n);return true;
    }
    assert(obj==H_COMPUTE_VRAM && fb==g_compute_vram_fb && off+n<=sizeof stage);
    if(!off){assert(n==expected->code_count*16 && !memcmp(p,expected->code,(size_t)n));if(failure==3)return false;}
    else {assert(off==0xe000 && n==expected->texture_count*32 && !memcmp(p,expected->textures,(size_t)n));if(failure==4)return false;}
    memcpy(stage+off,p,(size_t)n);return true;
}
static bool nv_compute_upload(nv_channel_t *ch,u64 off,const void *p,u64 n) {
    return nv_vram_object_write(ch,H_COMPUTE_VRAM,g_compute_vram_fb,off,p,n);
}
static bool nv_surface_transfer(u64 owner,u64 handle,u64 off,void *p,u32 n,bool read) {
    nv_surface_t *s=nv_surface_lookup(owner,handle);assert(s&&!read);
    return nv_vram_object_write(channels,s->memory,s->fb,off,p,n);
}
static u64 arg64(const u32 *a,unsigned i){return ((u64)a[i+1]<<32)|a[i];}
static bool nv_compute_dispatch_one(nv_channel_t *ch,const unsigned char *sass,u32 bytes,
                                    u32 regs,u32 off,const u32 *a,u32 n,
                                    const u32 *grid,const u32 *block,u32 tag,const char *label){
    assert(ch==channels && sass==shader_vm_sass && bytes==sizeof shader_vm_sass);
    assert(regs==shader_vm_sass_reg_count && off==0x4000 && n==24 && tag==0x53560000);
    assert(!strcmp(label,"surface/shader-vm"));launches++;
    assert(arg64(a,0)==surfaces[0].va && arg64(a,2)==VA_COMPUTE && arg64(a,4)==surfaces[1].va);
    assert(!arg64(a,6)||arg64(a,6)==surfaces[2].va);
    assert(arg64(a,8)==VA_COMPUTE+0xe000);
    assert(arg64(a,10)==surfaces[0].bytes/4 && arg64(a,12)==expected->code_count*16);
    assert(arg64(a,14)==expected->lanes*8 && arg64(a,16)==(arg64(a,6)?surfaces[2].bytes:0));
    assert(arg64(a,18)==expected->texture_count*32);
    assert(a[20]==expected->lanes&&a[21]==expected->code_count&&a[22]==expected->budget&&a[23]==expected->texture_count);
    assert(grid[0]==(a[20]+63)/64 && grid[1]==1&&grid[2]==1);
    assert(block[0]==64&&block[1]==1&&block[2]==1);
    assert(!memcmp(stage,expected->code,expected->code_count*16));
    assert(!memcmp(stage+0xe000,expected->textures,expected->texture_count*32));
    for(unsigned i=0;i<expected->lanes*8;i++)assert(status[i]==0);
    if(failure==5)return false;
    if(failure==6){ch->submit_failed=true;return false;}
    return true;
}
'''
    for name in ['nv_compute_resident_layout','nv_surface_shader_valid','nv_shader_prepare','nv_surface_shader']:
        code+='\n'+function(SRC,name)
    code+=r'''
static bool launch(const ksh_dispatch_t *job,u64 texture){expected=job;return nv_surface_shader(9,101,102,texture,job);}
int main(void){
    static ksh_dispatch_t job,bad;
    job.lanes=67;job.code_count=2;job.budget=64;job.texture_count=1;
    job.textures[0]=(ksh_texture_t){.width=3,.height=2,.pitch=4,.offset=16};
    job.code[0]=(sh_instruction_t){.op=SH_MUL,.mask=15,.dst=193,.src={128,64,0},.swizzle={228,228,228}};
    job.code[1]=(sh_instruction_t){.op=SH_END};
    memset(status,0xff,sizeof status);assert(launch(&job,103));assert(ensures==1&&uploads==3&&launches==1);
    unsigned before=ensures;
    assert(!nv_surface_shader(8,101,102,103,&job));
    assert(!nv_surface_shader(9,101,101,103,&job));
    assert(!nv_surface_shader(9,101,102,101,&job));
    assert(!nv_surface_shader(9,101,102,102,&job));
    assert(!nv_surface_shader(9,101,102,104,&job));
    assert(!launch(NULL,103));assert(!launch(&job,0));
    for(unsigned test=0;test<26;test++){
        bad=job;
        switch(test){
        case 0:bad.lanes=0;break;case 1:bad.lanes=4097;break;
        case 2:bad.code_count=0;break;case 3:bad.code_count=1025;break;
        case 4:bad.budget=0;break;case 5:bad.budget=4097;break;
        case 6:bad.texture_count=5;break;case 7:bad.reserved[3]=1;break;
        case 8:bad.code[0].op=255;break;case 9:bad.code[0].dst=200;break;
        case 10:bad.code[0].src[0]=200;break;case 11:bad.code[0].src[1]=200;break;
        case 12:bad.code[0].src[2]=200;break;case 13:bad.code[0].mask=16;break;
        case 14:bad.code[0].op=SH_MATMUL;bad.code[0].src[0]=197;break;
        case 15:bad.code[0].op=SH_JMP;bad.code[0].target=-1;break;
        case 16:bad.code[0].op=SH_JMPZ;bad.code[0].target=3;break;
        case 17:bad.textures[0].width=0;break;case 18:bad.textures[0].height=32769;break;
        case 19:bad.textures[0].pitch=2;break;case 20:bad.textures[0].wrap_s=2;break;
        case 21:bad.textures[0].wrap_t=2;break;case 22:bad.textures[0].filter=2;break;
        case 23:bad.textures[0].offset=1;break;case 24:bad.textures[0].offset=UINT64_MAX-3;break;
        case 25:bad.textures[0].pitch=UINT32_MAX;break;
        }
        assert(!launch(&bad,103));assert(ensures==before);
    }
    u64 bytes=surfaces[0].bytes;surfaces[0].bytes=67*3200-1;assert(!launch(&job,103));surfaces[0].bytes=bytes;
    bytes=surfaces[1].bytes;surfaces[1].bytes=67*8-1;assert(!launch(&job,103));surfaces[1].bytes=bytes;
    assert(ensures==before);
    bad=job;bad.texture_count=0;assert(launch(&bad,0));
    bad=job;bad.textures[0]=(ksh_texture_t){0};assert(launch(&bad,0));
    bad=job;bad.lanes=4096;bad.budget=4096;bad.code_count=1024;
    assert(launch(&bad,103)); // maximum bytecode ends immediately before QMD
    bad.code[1023].op=255;before=uploads;assert(!launch(&bad,103)&&uploads==before);
    for(failure=1;failure<=6;failure++){
        unsigned l=launches;assert(!launch(&job,103));if(failure<=4)assert(launches==l);
    }
    before=uploads;assert(channels[0].submit_failed&&!launch(&job,103)&&uploads==before);
    channels[0].submit_failed=false;failure=0;batch_mode=true;
    surfaces[1].state=NS_READY;surfaces[1].copy_mapping_attempted=true;
    before=uploads;
    for(unsigned textures=0;textures<=4;textures++) {
        bad=job;bad.texture_count=textures;
        for(unsigned i=0;i<textures;i++)bad.textures[i]=job.textures[0];
        bad.code_count=1024;bad.lanes=4096;
        memset(status,0xff,sizeof status);
        unsigned b=batches;assert(launch(&bad,103));assert(batches==b+1&&uploads==before);
    }
    unsigned b=batches,l=launches,m=mapping_checks;
    surfaces[1].copy_mapping_attempted=false;assert(!launch(&job,103));
    surfaces[1].copy_mapping_attempted=true;surfaces[1].state=0;assert(!launch(&job,103));
    surfaces[1].state=NS_READY;g_render_transaction=false;assert(!launch(&job,103));
    g_render_transaction=true;assert(batches==b&&launches==l&&mapping_checks==m);
    failure=7;assert(!launch(&job,103));assert(batches==b&&uploads==before&&launches==l);
    failure=8;assert(!launch(&job,103));assert(batches==b+1&&uploads==before&&launches==l);
    puts("PASS VM preparation: one fenced COPY batch for status/code/0..4 descriptors, maximum spans, mapping refusal, no fallback after DMA failure");
    puts("PASS actual VM admission/dispatch: 26 malformed packets, owners/aliases/capacities, 24-word ABI, 1024-instruction upload, pending statuses, ordered failures and timeout quarantine (modeled GPU)");
}
'''
    run_test(code,'surface_shader')
