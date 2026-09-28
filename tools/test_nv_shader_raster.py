#!/usr/bin/env python3
"""Production programmable raster admission/packing with modeled VRAM/fences."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test
ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/'kernel/nv_chan.c').read_text()
if __name__=='__main__':
    c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef uint64_t u64;
typedef struct {bool open,submit_failed;} nv_channel_t;
typedef struct {u32 width,height,pitch;u64 bytes,va,fb;u32 memory;} nv_surface_t;
typedef struct {u32 qmd_off,shader_off,constant_off,bytes;} nv_dispatch_layout_t;
#define CH_GFX 0
static nv_channel_t channels[1]={{true,false}};
static nv_surface_t surfaces[5];
static nv_surface_t *nv_surface_lookup(u64 owner,u64 h){return owner==9&&h>=101&&h<=105?&surfaces[h-101]:NULL;}
static unsigned ensures,uploads,launches;static unsigned fail;
static bool ensure_compute_vram(nv_channel_t *ch){assert(ch==channels);ensures++;return fail!=1;}
'''
    for p in ['include/kestrel/shader_raster.h','tools/shader_shader_raster.h','tools/shader_shader_raster_fast.h']:
        c+='\n#include "'+(ROOT/p).as_posix()+'"\n'
    c+=r'''
static const kshr_submission_t *expected;
static unsigned char status[KSHR_FAST_MAX_LANES*8];static kshr_job_t snapshot;
static unsigned char inactive_tail[sizeof(kshr_job_t)];
static u64 packet_upload_bytes;
static size_t packet_prefix(const kshr_job_t *job){
    /* Independent fixed ABI contract, not the production size expression. */
    assert(offsetof(kshr_job_t,commands)==19776 && sizeof(kshr_command_t)==576);
    assert(job->command_count>=1 && job->command_count<=64);
    return 19776u+576u*job->command_count;
}
static bool nv_vram_object_write(nv_channel_t *ch,u32 memory,u64 fb,u64 off,const void *p,u64 n){
    assert(ch==channels);uploads++;
    unsigned lanes=kshr_job_lanes(&expected->job);
    if(memory==surfaces[4].memory){
        assert(fb==surfaces[4].fb&&!off&&n==lanes*8);
        const unsigned char *b=p;for(unsigned i=0;i<n;i++)assert(!b[i]);
        if(fail==2)return false;memcpy(status,p,(size_t)n);
    }else{
        assert(memory==surfaces[3].memory&&fb==surfaces[3].fb&&off==KSHR_JOB_PACKET_OFFSET(&expected->job));
        assert(off>=kshr_job_scratch_bytes(&expected->job)&&off+n<=surfaces[3].bytes);
        assert(n==packet_prefix(&expected->job)&&!memcmp(p,&expected->job,(size_t)n));
        packet_upload_bytes+=n;
        memcpy(inactive_tail,(unsigned char *)&snapshot+n,sizeof snapshot-(size_t)n);
        if(fail==3){
            /* Failure may modify a prefix. No dispatcher may see this mixed
             * new header/old command packet; a retry must replace all active bytes. */
            memset(&snapshot,0xa5,(size_t)n);
            memcpy(&snapshot,p,(size_t)n/2);return false;
        }
        memcpy(&snapshot,p,(size_t)n);
        assert(!memcmp((unsigned char *)&snapshot+n,inactive_tail,sizeof snapshot-(size_t)n));
    }return true;
}
static bool nv_surface_transfer(u64 owner,u64 handle,u64 off,void *p,u32 n,bool read) {
    nv_surface_t *s=nv_surface_lookup(owner,handle);assert(s&&!read);
    return nv_vram_object_write(channels,s->memory,s->fb,off,p,n);
}
static u64 arg(const u32 *a,unsigned i){return (u64)a[i]|((u64)a[i+1]<<32);}
static bool nv_compute_dispatch_one(nv_channel_t *ch,const unsigned char *sass,u32 bytes,u32 regs,u32 off,
    const u32 *a,u32 n,const u32 *grid,const u32 *block,u32 signal,const char *label){
    bool fast=kshr_register_fast_eligible(&expected->job);
    assert(ch==channels&&sass==(fast?shader_raster_fast_sass:shader_raster_sass));
    assert(bytes==(fast?sizeof shader_raster_fast_sass:sizeof shader_raster_sass));
    assert(regs==(fast?shader_raster_fast_sass_reg_count:shader_raster_sass_reg_count));
    assert(off==0x400&&n==24&&signal==0x53520000&&!strcmp(label,fast?"surface/shader-raster-register":"surface/shader-raster"));launches++;
    unsigned lanes=kshr_job_lanes(&snapshot);
    assert(arg(a,0)==surfaces[0].va&&(!arg(a,2)||arg(a,2)==surfaces[1].va));
    assert(!arg(a,4)||arg(a,4)==surfaces[2].va);
    assert(arg(a,6)==surfaces[3].va&&arg(a,8)==surfaces[4].va);
    assert(arg(a,10)==surfaces[3].va+KSHR_JOB_PACKET_OFFSET(&expected->job));
    assert(arg(a,12)==surfaces[0].bytes&&arg(a,14)==(arg(a,2)?surfaces[1].bytes:0));
    assert(arg(a,16)==(arg(a,4)?surfaces[2].bytes:0)&&arg(a,18)==kshr_job_scratch_bytes(&expected->job));
    assert(arg(a,20)==lanes*8&&arg(a,22)==sizeof snapshot);
    assert(grid[0]==(lanes+63)/64&&grid[1]==1&&grid[2]==1);
    assert(block[0]==64&&block[1]==1&&block[2]==1);
    for(unsigned i=0;i<lanes*8;i++)assert(status[i]==0);
    size_t prefix=packet_prefix(&expected->job);
    assert(!memcmp(&snapshot,&expected->job,prefix));
    assert(!memcmp((unsigned char *)&snapshot+prefix,inactive_tail,sizeof snapshot-prefix));
    if(fail==4)return false;if(fail==5){ch->submit_failed=true;return false;}return true;
}
'''
    c += re.search(r'typedef struct \{[^{}]*\}\s*nv_surface_prepare_t;', SRC)[0]+'\n'
    c += r'''
/* Admission/packing fixture retains the coherent non-runtime transfer route.
 * The actual CE batching route is exercised by test_nv_raster_prepare.py. */
static bool nv_surface_transfer_ce_ready(void) { return false; }
static bool nv_surface_prepare_batch(const nv_surface_prepare_t *ops,u32 count) {
    (void)ops;(void)count;assert(!"unexpected runtime route");return false;
}
'''
    for fn in ['nv_compute_resident_layout','nv_float_bits','nv_surface_3d_commands_valid',
               'nv_surface_shader_raster_state_valid','nv_surface_shader_raster_valid',
               'nv_surface_shader_raster_dispatch_prepared','nv_surface_shader_raster_dispatch_cpu',
               'nv_surface_shader_raster']:
        c+='\n'+function(SRC,fn)+'\n'
    c+=r'''
static bool launch(const kshr_submission_t *s,u64 z,u64 tex){expected=s;return nv_surface_shader_raster(9,101,z,tex,s);}
int main(void){
    for(unsigned i=0;i<5;i++)surfaces[i]=(nv_surface_t){32,16,128,0x1000000,0x30000000000ull+i*0x10000000ull,0x10000000ull+i*0x1000000ull,20+i};
    static kshr_submission_t s,bad;s.scratch=104;s.status=105;
    s.job.width=32;s.job.height=16;s.job.pitch=32;s.job.depth_pitch=32;s.job.clip_w=32;s.job.clip_h=16;
    s.job.command_count=1;s.job.code_count=2;s.job.budget=32;s.job.varying_count=8;
    s.job.commands[0].raster=(kg3d_command_t){.width=32,.height=16,.flags=KG3D_DEPTH_TEST,.depth_func=KG3D_LESS};
    for(unsigned i=0;i<3;i++)s.job.commands[0].raster.v[i].inv_w=1;
    s.job.code[0]=(sh_instruction_t){.op=SH_MOV,.dst=SR_FRAGCOLOR,.src={SR_VARYING,0,0},.mask=15,.swizzle={228,228,228}};
    s.job.code[1].op=SH_END;s.job.texture_count=1;s.job.textures[0]=(ksh_texture_t){.width=3,.height=2,.pitch=4};
    memset(status,0xff,sizeof status);assert(launch(&s,102,103));assert(ensures==1&&uploads==2&&launches==1);
    bad=s;bad.job.code[0].dst=8;assert(!kshr_register_fast_eligible(&bad.job));assert(launch(&bad,102,103));
    bad=s;bad.job.code[0].op=SH_SIN;assert(!kshr_register_fast_eligible(&bad.job));assert(launch(&bad,102,103));
    /* Exercise every active count in both directions and alternating extremes.
     * Stale inactive records deliberately contain NaN/invalid flag words. They
     * must neither be uploaded/cleared nor admitted as active commands. */
    for(unsigned pass=0;pass<3;pass++)for(unsigned step=0;step<64;step++){
        unsigned count=pass==0?step+1:pass==1?64-step:(step&1?64-step/2:step/2+1);
        bad=s;bad.job.command_count=count;
        for(unsigned i=0;i<count;i++){
            bad.job.commands[i]=s.job.commands[0];
            bad.job.commands[i].raster.x=(int)(i%7);
            bad.job.commands[i].varying[0][0][0]=(float)(pass*64+step+i);
        }
        memset(bad.job.commands+count,0xa5,(64-count)*sizeof bad.job.commands[0]);
        memset(snapshot.commands+count,0xff,(64-count)*sizeof snapshot.commands[0]);
        u64 uploaded=packet_upload_bytes;unsigned dispatched=launches;
        assert(launch(&bad,102,103));
        assert(packet_upload_bytes-uploaded==19776u+576u*count && launches==dispatched+1);
        for(size_t i=packet_prefix(&bad.job);i<sizeof snapshot;i++)assert(((unsigned char *)&snapshot)[i]==0xff);
    }
    /* A 64-command transfer failure after touching VRAM is retried with one
     * command, then grown back to64. The failed attempt must not dispatch. */
    bad=s;bad.job.command_count=64;
    for(unsigned i=1;i<64;i++)bad.job.commands[i]=s.job.commands[0];
    unsigned failed_launches=launches;fail=3;
    assert(!launch(&bad,102,103));assert(launches==failed_launches);fail=0;
    assert(launch(&s,102,103));assert(launch(&bad,102,103));
    unsigned before=ensures;
    assert(!nv_surface_shader_raster(8,101,102,103,&s));assert(!launch(NULL,102,103));
    // All ten resource pairs, including texture/depth, are disjoint.
    for(unsigned a=0;a<5;a++)for(unsigned b=a+1;b<5;b++){
        u64 handles[5]={101,102,103,104,105};handles[b]=handles[a];bad=s;bad.scratch=handles[3];bad.status=handles[4];
        assert(!nv_surface_shader_raster(9,handles[0],handles[1],handles[2],&bad));
    }
    for(unsigned i=0;i<5;i++){
        u64 h[5]={101,102,103,104,105};h[i]=106;bad=s;bad.scratch=h[3];bad.status=h[4];
        assert(!nv_surface_shader_raster(9,h[0],h[1],h[2],&bad));
    }
    for(unsigned test=0;test<36;test++){
        bad=s;u32 nan=0x7fc00000;
        switch(test){
        case 0:bad.job.width++;break;case 1:bad.job.height++;break;case 2:bad.job.pitch++;break;
        case 3:bad.job.depth_pitch++;break;case 4:bad.job.clip_x=32;break;case 5:bad.job.clip_y=16;break;
        case 6:bad.job.clip_w=0;break;case 7:bad.job.clip_h=17;break;case 8:bad.job.command_count=0;break;
        case 9:bad.job.command_count=65;break;case 10:bad.job.code_count=0;break;case 11:bad.job.code_count=1025;break;
        case 12:bad.job.budget=0;break;case 13:bad.job.budget=4097;break;case 14:bad.job.varying_count=9;break;
        case 15:bad.job.origin_lower_left=2;break;case 16:bad.job.reserved[1]=1;break;case 17:bad.job.texture_count=5;break;
        case 18:bad.job.commands[0].raster.flags|=KG3D_CLEAR_COLOUR;break;
        case 19:bad.job.commands[0].raster.flags|=KG3D_TEXTURE;break;
        case 20:memcpy(&bad.job.commands[0].varying[2][7][3],&nan,4);break;
        case 21:memcpy(&bad.job.seed[SR_UNIFORM][0],&nan,4);break;
        case 22:bad.job.code[0].op=255;break;case 23:bad.job.code[0].dst=200;break;
        case 24:bad.job.code[0].src[1]=200;break;case 25:bad.job.code[0].mask=16;break;
        case 26:bad.job.code[0].op=SH_MATMUL;bad.job.code[0].src[0]=197;break;
        case 27:bad.job.code[0].op=SH_JMP;bad.job.code[0].target=-1;break;
        case 28:bad.job.code[0].op=SH_JMPNZ;bad.job.code[0].target=3;break;
        case 29:bad.job.textures[0].width=32769;break;case 30:bad.job.textures[0].pitch=2;break;
        case 31:bad.job.textures[0].offset=UINT64_MAX-3;break;case 32:bad.job.textures[0].filter=2;break;
        case 33:bad.job.textures[0].wrap_s=2;break;case 34:bad.job.textures[0].height=0;break;
        case 35:bad.job.budget=4096;bad.job.command_count=64;
            for(unsigned i=1;i<64;i++)bad.job.commands[i]=bad.job.commands[0];break;
        }
        assert(!launch(&bad,102,103));assert(ensures==before);
    }
    u64 saved=surfaces[3].bytes;surfaces[3].bytes=KSHR_JOB_ALLOCATION_BYTES(&s.job)-1;
    assert(!launch(&s,102,103));surfaces[3].bytes=saved;
    saved=surfaces[4].bytes;surfaces[4].bytes=512*8-1;assert(!launch(&s,102,103));surfaces[4].bytes=saved;
    assert(ensures==before);
    bad=s;bad.job.depth_pitch=0;bad.job.commands[0].raster.flags=0;bad.job.texture_count=0;assert(launch(&bad,0,0));
    bad=s;bad.job.code_count=1024;memset(bad.job.code,0,sizeof bad.job.code);assert(launch(&bad,102,103));
    // Maximum tile, command count and exact work boundary without overlaps.
    bad=s;bad.job.width=64;bad.job.height=64;bad.job.pitch=64;bad.job.depth_pitch=64;
    bad.job.clip_w=64;bad.job.clip_h=64;bad.job.command_count=64;bad.job.budget=64;
    bad.job.commands[0].raster.width=64;bad.job.commands[0].raster.height=64;
    for(unsigned i=1;i<64;i++)bad.job.commands[i]=bad.job.commands[0];
    for(unsigned i=0;i<2;i++){surfaces[i].width=64;surfaces[i].height=64;surfaces[i].pitch=256;}
    assert(launch(&bad,102,103));before=ensures;bad.job.budget=65;assert(!launch(&bad,102,103));assert(ensures==before);
    // A multi-pixel job reuses only 4096 scratch/status slices, even at the
    // maximum total-work boundary. Pixel dimensions never size the scratch.
    bad=s;bad.job.width=4096;bad.job.height=4096;bad.job.pitch=4096;bad.job.depth_pitch=4096;
    bad.job.clip_w=4096;bad.job.clip_h=4096;bad.job.budget=1;bad.job.code_count=1;bad.job.code[0].op=SH_END;
    bad.job.commands[0].raster.width=4096;bad.job.commands[0].raster.height=4096;
    for(unsigned i=0;i<2;i++){surfaces[i].width=4096;surfaces[i].height=4096;surfaces[i].pitch=16384;surfaces[i].bytes=64ull*1024*1024;}
    surfaces[3].bytes=KSHR_JOB_ALLOCATION_BYTES(&bad.job);surfaces[4].bytes=KSHR_FAST_MAX_LANES*8;
    assert(launch(&bad,102,103));before=ensures;
    surfaces[3].bytes--;assert(!launch(&bad,102,103));surfaces[3].bytes++;
    surfaces[4].bytes--;assert(!launch(&bad,102,103));surfaces[4].bytes++;
    bad.job.budget=2;assert(!launch(&bad,102,103));assert(ensures==before);
    bad.job.budget=1;bad.job.clip_w=UINT32_MAX;assert(!launch(&bad,102,103));
    for(unsigned i=0;i<5;i++)surfaces[i]=(nv_surface_t){32,16,128,0x1000000,0x30000000000ull+i*0x10000000ull,0x10000000ull+i*0x1000000ull,20+i};
    for(unsigned i=1;i<=5;i++){
        unsigned previous_launches=launches,previous_uploads=uploads;
        fail=i;assert(!launch(&s,102,103));
        if(i<=3)assert(launches==previous_launches);
        assert(uploads-previous_uploads==(i==1?0:i==2?1:2));
    }
    before=uploads;fail=0;assert(!launch(&s,102,103));assert(uploads==before&&channels[0].submit_failed);
    puts("PASS actual programmable raster admission: owner/10 aliases, 36 malformed jobs, capacities/work, 192 exact active-prefix uploads with retained poisoned tail, grow/shrink and partial-write retry, full packet capacity, 24 launch words, pending status, uploads/fence/quarantine (modeled GPU)");
}
'''
    run_test(c,'nv_shader_raster')
