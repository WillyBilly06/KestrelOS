#!/usr/bin/env python3
"""Execute raster preparation and CE command encoding in host memory only.

Compiles the actual CPU preparation helper and existing production CE packet
builder. Submission is a call spy; this does not execute or emulate a GPU shader.
"""
from pathlib import Path
from test_gpu_variant_safety import function
from test_gpu_stable_candidate import run_test
from test_nv_geometry_prepare import source_code

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT/'kernel/nv_chan.c').read_text()
    code = source_code()
    code = code[:code.index('int main(void)')]
    code += '#include <stdio.h>\n#include "'+(ROOT/'include/kestrel/shader_raster.h').as_posix()+'"\n'
    code += r'''
typedef struct { u32 qmd_off,shader_off,constant_off,bytes; } nv_dispatch_layout_t;
static bool layout_ok=true,compute_memory_ok=true,dispatch_ok=true;
static unsigned ensures,uploads,dispatches;
static unsigned transfer_fail;
static nv_surface_t scratch,status,dst;
static kshr_submission_t request;
static size_t prefix_bytes(void) {
    assert(offsetof(kshr_job_t,commands)==19776 && sizeof(kshr_command_t)==576);
    return 19776u+576u*request.job.command_count;
}
static bool nv_compute_resident_layout(u32 off,u32 args,nv_dispatch_layout_t *layout) {
    (void)layout;assert(off==0x400u&&args==24u);return layout_ok;
}
static bool ensure_compute_vram(nv_channel_t *ch) {
    assert(ch==&channels[CH_GFX]);ensures++;return compute_memory_ok;
}
static bool nv_surface_transfer(u64 owner,u64 handle,u64 offset,void *data,u32 bytes,bool read) {
    assert(owner==19&&!read);uploads++;
    if(handle==request.status) {
        assert(!offset&&bytes==kshr_job_lanes(&request.job)*sizeof(ksh_status_t));
        for(u32 i=0;i<bytes;i++)assert(!((unsigned char*)data)[i]);
    } else {
        assert(handle==request.scratch&&offset==KSHR_JOB_PACKET_OFFSET(&request.job));
        assert(bytes==prefix_bytes()&&!memcmp(data,&request.job,bytes));
    }
    return uploads!=transfer_fail;
}
static bool nv_surface_shader_raster_dispatch_prepared(nv_surface_t *d,nv_surface_t *z,
    nv_surface_t *t,nv_surface_t *sc,nv_surface_t *st,const kshr_job_t *job) {
    assert(d==&dst&&!z&&!t&&sc==&scratch&&st==&status&&job==&request.job);
    assert(ready?(submits==1&&channels[CH_COPY].sem[0]!=0):(uploads==2));
    dispatches++;return dispatch_ok;
}
'''
    code += function(src,'nv_surface_shader_raster_dispatch_cpu')+'\n'
    code += r'''
static void reset_draw_size(u32 count,bool fast,u32 width,u32 height) {
    reset();layout_ok=compute_memory_ok=dispatch_ok=true;
    ensures=uploads=dispatches=transfer_fail=0;
    request=(kshr_submission_t){.scratch=101,.status=102};
    request.job.width=width;request.job.height=height;request.job.pitch=width;
    request.job.clip_w=width;request.job.clip_h=height;request.job.command_count=count;
    request.job.code_count=2;request.job.budget=32;request.job.varying_count=8;
    request.job.code[0]=(sh_instruction_t){.op=SH_MOV,.dst=SR_FRAGCOLOR,
        .src={SR_VARYING,0,0},.mask=15,.swizzle={228,228,228}};
    request.job.code[1].op=SH_END;
    if(!fast)request.job.code[0].op=SH_SIN;
    // Distinct command bytes check full active-prefix transfer, not zero data.
    for(u32 i=0;i<count;i++)request.job.commands[i].varying[0][0][0]=(float)(i+1);
    memset(request.job.commands+count,0xa5,(64-count)*sizeof(kshr_command_t));
    scratch=(nv_surface_t){.bytes=KSHR_JOB_ALLOCATION_BYTES(&request.job),
        .va=0x30000000000ull,.owner=19,.state=NS_READY,.copy_mapping_attempted=true};
    status=scratch;status.va+=0x10000000ull;
    status.bytes=(u64)kshr_job_lanes(&request.job)*sizeof(ksh_status_t);
    memset(stage,0xcc,sizeof stage);
}
static void reset_draw(u32 count,bool fast) { reset_draw_size(count,fast,512,64); }
static bool draw(void) {
    return nv_surface_shader_raster_dispatch_cpu(19,&dst,NULL,NULL,&scratch,&status,&request);
}
int main(void) {
    const u32 sizes[][2]={{1,1},{63,1},{64,1},{65,1},{127,129},{128,128},{129,128},{512,64}};
    unsigned cases=0;
    for(unsigned size=0;size<sizeof sizes/sizeof sizes[0];size++)
    for(unsigned fast=0;fast<2;fast++)for(u32 count=1;count<=64;count++) {
        reset_draw_size(count,fast,sizes[size][0],sizes[size][1]);
        assert(draw());packet_check(2,1);cases++;
        assert(dispatches==1&&uploads==0&&ensures==1);
        assert(!memcmp(stage,&request.job,prefix_bytes())&&stage[prefix_bytes()]==0xcc);
        unsigned lines=0,outputs=0,inputs=0;
        for(u32 i=0;i<word_count;i++) {
            if(methods[i]==CE_LINE_LENGTH_IN) {
                assert(words[i]==(lines==0?status.bytes/4u:prefix_bytes()));
                assert(words[i+1]==1u);lines++;
            }
            if(methods[i]==CE_OFFSET_OUT_UPPER) {
                u64 va=((u64)words[i]<<32)|words[i+1];
                assert(va==(outputs==0?status.va:scratch.va+KSHR_JOB_PACKET_OFFSET(&request.job)));outputs++;
            }
            if(methods[i]==CE_OFFSET_IN_UPPER) {
                assert((((u64)words[i]<<32)|words[i+1])==VA_UPLOAD_STAGE);inputs++;
            }
        }
        assert(lines==2&&outputs==2&&inputs==1); // GPU clear has no CPU zero-array upload.
    }
    for(unsigned fault=0;fault<16;fault++) {
        reset_draw(64,true);
        if(fault==0)scratch.bytes--;
        if(fault==1)status.bytes--;
        if(fault==2)layout_ok=false;
        if(fault==3)compute_memory_ok=false;
        if(fault==4)status.owner++;
        if(fault==5)status.copy_mapping_attempted=false;
        if(fault==6)reserve_ok=false;
        if(fault==7)retire=false;
        if(fault==8)dispatch_ok=false;
        if(fault==9)g_render_transaction=0;
        if(fault==10)channels[CH_COPY].open=false;
        if(fault==11)channels[CH_COPY].submit_failed=true;
        if(fault==12)channels[CH_GFX].submit_failed=true;
        if(fault==13)scratch.copy_mapping_attempted=false;
        if(fault==14)scratch.state=0;
        if(fault==15)status.state=0;
        assert(!draw()&&!uploads); // Never retry batched work through old transfer path.
        assert(dispatches==(fault==8));
        assert(submits==(fault==7||fault==8));
    }
    reset_draw(1,true);ready=false;
    assert(draw()&&uploads==2&&!submits&&dispatches==1);
    for(unsigned fail=1;fail<=2;fail++) {
        reset_draw(1,true);ready=false;transfer_fail=fail;
        assert(!draw()&&uploads==fail&&!submits&&!dispatches);
    }
    assert(cases==1024);
    puts("PASS: 1024 raster preparation packets, lane boundaries, one final CE fence, active-prefix uploads, 16 faults without replay, retained boot transfer path");
    return 0;
}
'''
    run_test(code,'nv-raster-prepare')


if __name__ == '__main__':
    main()
