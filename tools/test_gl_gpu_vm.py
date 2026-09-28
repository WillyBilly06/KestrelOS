#!/usr/bin/env python3
"""Run actual userspace shader transport with a modeled GPU surface syscall.

Tests transfer/lifetime/status contracts, not shader arithmetic or native GPU.
"""
from pathlib import Path
import re
import sys
from test_gpu_stable_candidate import run_test
ROOT=Path(__file__).resolve().parents[1]
def strip(text):return re.sub(r'^#include[^\n]*\n','',text,flags=re.M)

if __name__=='__main__':
    c=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define EIO 5
#define EBUSY 16
#define EINVAL 22
#define ENOMEM 12
#define ENOSPC 28
#define SYS_GPU 99
#define GPUOP_SURFACE 8
'''
    for path in ['include/kestrel/shader_vm.h','include/kestrel/shader_raster.h','include/kestrel/shader_setup.h','include/kestrel/gpu2d.h']:
        c+='\n#include "'+(ROOT/path).as_posix()+'"\n'
    c+=strip((ROOT/'user/libgl/gpuvm.h').read_text())
    c+=r'''
typedef struct {uint64_t handle,bytes;unsigned char *data;} allocation;
static allocation pool[32];static uint64_t next_handle=100;
static gl_gpu_vm_t *active;
static const float *expected_input;static uint64_t expected_floats;
static const void *expected_texture;static uint64_t expected_texture_bytes;
static unsigned creates,destroys,uploads,status_reads,register_reads,launches,requests;
static bool sparse_mode,sparse_used[SR_REGISTERS];
static uint64_t uploaded_bytes;
static unsigned fail_create,fail_upload,fail_register_read;
static bool fail_cpu,fail_launch,fail_status_read,busy,destroy_busy;
static unsigned bad_result,bad_steps;
static unsigned last_lanes;
static unsigned geometry_launches,geometry_fault;static bool geometry_vertex;
static bool gate_mode;static unsigned gate_corrupt;
static uint64_t gate_registers,gate_status;
static void *vm_malloc(size_t n){return fail_cpu?NULL:malloc(n);}
static void vm_free(void *p){free(p);}
static void sleep_ms(uint64_t ms){assert(ms==1);}
static void log_write(int level,const char *part,const char *text){(void)level;(void)part;(void)text;}
static allocation *get(uint64_t h){for(unsigned i=0;i<32;i++)if(pool[i].handle==h)return &pool[i];return NULL;}
static void gate_model(allocation *regs,allocation *status,allocation *tex,const ksh_dispatch_t *job){
    float *r=(float*)regs->data;unsigned n=job->lanes;
    for(unsigned lane=0;lane<n;lane++){
        unsigned pc=0,executed=0,result=KSH_PENDING;
        while(pc<job->code_count && executed<job->budget){
            const sh_instruction_t *in=&job->code[pc];float a[4],b[4],c[4],out[4]={0};executed++;
            for(unsigned k=0;k<4;k++){
                a[k]=r[(in->src[0]*4+((in->swizzle[0]>>(2*k))&3))*n+lane];
                b[k]=r[(in->src[1]*4+((in->swizzle[1]>>(2*k))&3))*n+lane];
                c[k]=r[(in->src[2]*4+((in->swizzle[2]>>(2*k))&3))*n+lane];
            }
            if(in->op==SH_END){result=KSH_COMPLETE;break;}
            if(in->op==SH_DISCARD){result=KSH_DISCARDED;break;}
            if(in->op==SH_JMPZ){pc=a[0]==0?in->target:pc+1;continue;}
            if(in->op==SH_MATMUL){
                for(unsigned k=0;k<4;k++)for(unsigned col=0;col<4;col++)out[k]+=r[((in->src[0]+col)*4+k)*n+lane]*b[col];
            }else if(in->op==SH_MAD||in->op==SH_MUL){
                for(unsigned k=0;k<4;k++)out[k]=a[k]*b[k]+(in->op==SH_MAD?c[k]:0);
            }else {
                assert(in->op==SH_TEX&&tex);
                const ksh_texture_t *t=&job->textures[0];
                unsigned x=(unsigned)(a[0]*t->width),y=(unsigned)((1-a[1])*t->height);
                assert(x<t->width&&y<t->height);
                unsigned p=((unsigned*)tex->data)[t->offset/4+y*t->pitch+x];
                for(unsigned k=0;k<4;k++)out[k]=((p>>(k==3?24:16-8*k))&255)*(1.0f/255.0f);
            }
            for(unsigned k=0;k<4;k++)if(in->mask&(1<<k))r[(in->dst*4+k)*n+lane]=out[k];
            pc++;
        }
        ((ksh_status_t*)status->data)[lane]=(ksh_status_t){result,executed};
    }
    if(gate_corrupt==launches)r[(job->code[0].op==SH_MATMUL?SR_POSITION:SR_FRAGCOLOR)*4*n]+=0.125f;
}
static intptr_t syscall6(intptr_t nr,intptr_t op,intptr_t addr,intptr_t size,intptr_t a3,intptr_t a4,intptr_t a5){
    assert(nr==SYS_GPU&&op==GPUOP_SURFACE&&size==72&&!a3&&!a4&&!a5);
    kg2d_request_t *r=(void*)(intptr_t)addr;assert(r->version==1);requests++;
    if(busy)return -EBUSY;
    if(r->operation==KG2D_CREATE){
        creates++;if(fail_create && creates==fail_create)return -ENOMEM;
        assert(r->width==4096 && r->height && r->height<=16384);
        allocation *p=NULL;for(unsigned i=0;i<32;i++)if(!pool[i].handle){p=&pool[i];break;}assert(p);
        p->handle=++next_handle;p->bytes=((uint64_t)r->height*16384+65535)&~65535ull;
        p->data=calloc(1,(size_t)p->bytes);assert(p->data);
        r->handle=p->handle;r->bytes=p->bytes;r->pitch=16384;return 0;
    }
    if(r->operation==KG2D_DESTROY){
        if(destroy_busy)return -EBUSY;
        allocation *p=get(r->handle);assert(p);free(p->data);*p=(allocation){0};destroys++;return 0;
    }
    if(r->operation==KG2D_UPLOAD||r->operation==KG2D_DOWNLOAD){
        allocation *p=get(r->handle);assert(p && r->bytes && r->bytes<=4*1024*1024);
        assert(!((r->bytes|r->offset)&3) && r->offset+r->bytes<=p->bytes);
        if(r->operation==KG2D_UPLOAD){
            uploads++;if(fail_upload && uploads==fail_upload)return -EIO;uploaded_bytes+=r->bytes;
            memcpy(p->data+r->offset,(void*)(uintptr_t)r->data,(size_t)r->bytes);
        } else {
            assert(launches && !fail_launch);
            if(r->handle==(gate_mode?gate_status:active->status.handle)){status_reads++;if(fail_status_read)return -EIO;}
            else {assert(r->handle==(gate_mode?gate_registers:active->registers.handle));register_reads++;
                  if(fail_register_read && register_reads==fail_register_read)return -EIO;}
            memcpy((void*)(uintptr_t)r->data,p->data+r->offset,(size_t)r->bytes);
        }
        return 0;
    }
    if(r->operation==KG2D_SHADER_GEOMETRY){
        const kshs_submission_t *s=(const void*)(uintptr_t)r->data;
        assert(r->bytes==sizeof(*s)&&r->handle==0xddd&&r->offset==0xeee);
        assert(s==active->geometry_packet&&!s->fragment.command_count);
        assert(s->registers==active->registers.handle&&s->vertex_status==active->status.handle);
        assert(s->workspace==active->geometry_workspace.handle);
        assert(s->raster_scratch==active->raster_scratch.handle&&s->raster_status==active->geometry_status.handle);
        uint64_t handles[]={s->registers,s->vertex_status,s->workspace,s->raster_scratch,s->raster_status};
        for(unsigned i=0;i<5;i++){assert(get(handles[i]));for(unsigned k=0;k<i;k++)assert(handles[i]!=handles[k]);}
        kshs_storage_layout_t layout;assert(kshs_storage_layout(s->setup.triangle_count,&layout));
        assert(get(s->workspace)->bytes>=layout.bytes);
        assert(get(s->raster_scratch)->bytes>=KSHR_JOB_ALLOCATION_BYTES(&s->fragment));
        assert(get(s->raster_status)->bytes>=(uint64_t)kshr_job_lanes(&s->fragment)*sizeof(ksh_status_t));
        assert(s->fragment.clip_x==(unsigned)s->setup.clip_x&&s->fragment.clip_w==(unsigned)s->setup.clip_w);
        geometry_launches++;
        if(geometry_fault==1)return -EIO; // submitted failure, never replay
        if(geometry_fault==2)return -EINVAL; // completed draw, failed result copyout
        r->count=geometry_fault==3?s->setup.triangle_count*KSHS_OUTPUT_TRIANGLES+1:2;
        r->bytes=geometry_fault==4?KSHS_MAX_RASTER_WORK+1ull:123;
        if(geometry_fault==5)r->count=r->bytes=0; // completely clipped/cull-rejected draw
        return 0;
    }
    if(r->operation==KG2D_SHADER_RASTER){
        assert(r->bytes==sizeof(kshr_submission_t)&&r->handle==0xddd&&r->offset==0xeee);
        const kshr_submission_t *s=(const void*)(uintptr_t)r->data;
        assert(s==active->raster_packet&&s->status==active->status.handle);
        assert(s->scratch==active->raster_scratch.handle||s->scratch==active->registers.handle);
        unsigned lanes=kshr_job_lanes(&s->job);
        assert(get(s->scratch)->bytes>=KSHR_JOB_ALLOCATION_BYTES(&s->job));
        allocation *status=get(s->status);assert(status&&status->bytes>=lanes*8);
        assert(active->texture_valid&&r->source==active->textures.handle);
        launches++;if(fail_launch)return -EIO;
        for(unsigned i=0;i<lanes;i++)((ksh_status_t*)status->data)[i]=(ksh_status_t){KSH_COMPLETE,i%2?2:0};
        if(bad_result)((ksh_status_t*)status->data)[lanes-1].result=bad_result==99?0:bad_result;
        if(bad_steps)((ksh_status_t*)status->data)[lanes-1].executed=bad_steps;
        return 0;
    }
    assert(r->operation==KG2D_SHADER_VM && r->bytes==sizeof(ksh_dispatch_t));
    const ksh_dispatch_t *job=(const void*)(uintptr_t)r->data;
    allocation *regs=get(r->handle),*status=get(r->offset),*tex=get(r->source);
    if(gate_mode){
        assert(regs&&status);gate_registers=r->handle;gate_status=r->offset;
        launches++;if(fail_launch)return -EIO;
        gate_model(regs,status,tex,job);return 0;
    }
    assert(regs&&status && r->handle==active->registers.handle && r->offset==active->status.handle);
    assert(expected_floats==(uint64_t)job->lanes*800 && regs->bytes>=expected_floats*4);
    if(geometry_vertex){
        bool used[SR_REGISTERS];gl_gpu_vertex_registers(job,used);
        for(unsigned reg=0;reg<SR_REGISTERS;reg++)if(used[reg])
            assert(!memcmp(regs->data+(size_t)reg*job->lanes*16,
                (const unsigned char*)expected_input+(size_t)reg*job->lanes*16,(size_t)job->lanes*16));
    }else if(sparse_mode){
        size_t plane=(size_t)job->lanes*16u;
        for(unsigned reg=0;reg<SR_REGISTERS;reg++){
            if(sparse_used[reg])assert(!memcmp(regs->data+reg*plane,(const unsigned char*)expected_input+reg*plane,plane));
            else for(size_t k=0;k<plane;k++)assert(regs->data[reg*plane+k]==0xa5);
        }
    }else assert(!memcmp(regs->data,expected_input,(size_t)expected_floats*4));
    if(expected_texture_bytes){assert(tex&&tex->bytes>=expected_texture_bytes);
        assert(!memcmp(tex->data,expected_texture,(size_t)expected_texture_bytes));}
    else assert(!r->source);
    launches++;if(fail_launch)return -EIO;
    last_lanes=job->lanes;
    // Modeled GPU produces unique per-lane results; not an arithmetic implementation.
    for(unsigned lane=0;lane<job->lanes;lane++){
        ((ksh_status_t*)status->data)[lane]=(ksh_status_t){geometry_vertex||lane%3?KSH_COMPLETE:KSH_DISCARDED,2};
        for(unsigned k=0;k<4;k++)((float*)regs->data)[(SR_POSITION*4+k)*job->lanes+lane]=42+k+lane;
    }
    if(bad_result)((ksh_status_t*)status->data)[job->lanes-1].result=bad_result==99?0:bad_result;
    if(bad_steps)((ksh_status_t*)status->data)[job->lanes-1].executed=bad_steps==99?0:bad_steps;
    return 0;
}
#define malloc vm_malloc
#define free vm_free
'''
    c+=strip((ROOT/'user/libgl/gpuvm.c').read_text())
    c+=strip((ROOT/'user/libgl/gpuvm_validate.c').read_text())
    c+=r'''
#undef malloc
#undef free
static float *inputs;
static ksh_dispatch_t job;
static unsigned texture_data[4]={0xff123456,0xffabcdef,0xff010203,0xff050607};
static void setup(unsigned lanes){
    job=(ksh_dispatch_t){.lanes=lanes,.code_count=2,.budget=8,.texture_count=1};
    job.textures[0]=(ksh_texture_t){.width=2,.height=2,.pitch=2};
    expected_input=inputs;expected_floats=(uint64_t)lanes*800;
    expected_texture=texture_data;expected_texture_bytes=16;
}
static bool execute(gl_gpu_vm_t *vm){active=vm;return gl_gpu_vm_execute(vm,&job,inputs,expected_floats,expected_texture,expected_texture_bytes);}
static void model_reset(void){
    // Test-only reset of separate modeled machines; production never frees unfenced buffers.
    for(unsigned i=0;i<32;i++){free(pool[i].data);pool[i]=(allocation){0};}
    creates=destroys=uploads=status_reads=register_reads=launches=requests=0;
    fail_create=fail_upload=fail_register_read=bad_result=bad_steps=0;
    fail_cpu=fail_launch=fail_status_read=busy=destroy_busy=false;
    gate_mode=false;gate_corrupt=0;gate_registers=gate_status=0;sparse_mode=false;uploaded_bytes=0;
    geometry_launches=geometry_fault=0;geometry_vertex=false;
}
int main(void){
    inputs=malloc(4096*800*4);assert(inputs);for(unsigned i=0;i<4096*800;i++)inputs[i]=(i%113)*0.0625f;
    gl_gpu_vm_t vm={0};setup(67);assert(execute(&vm));assert(vm.ready&&vm.lanes==67&&status_reads==1);
    float out[67*4];assert(gl_gpu_vm_read(&vm,SR_POSITION,1,out,67*4));
    for(unsigned k=0;k<4;k++)for(unsigned lane=0;lane<67;lane++)assert(out[k*67+lane]==42+k+lane);
    assert(vm.results[0].result==KSH_DISCARDED && vm.results[1].result==KSH_COMPLETE);
    unsigned saved_creates=creates,saved_destroys=destroys;
    for(unsigned i=0;i<100;i++)assert(execute(&vm));
    assert(creates==saved_creates&&destroys==saved_destroys); // no per-batch allocation churn
    job.texture_count=0;expected_texture=NULL;expected_texture_bytes=0;
    assert(execute(&vm));assert(!vm.textures.handle&&destroys==saved_destroys+1);
    ksh_status_t *old_results=vm.results;unsigned old_requests=requests;
    setup(4096);fail_cpu=true;assert(!execute(&vm)&&vm.last_error==-ENOMEM);
    assert(vm.results==old_results&&vm.result_capacity==67&&requests==old_requests);fail_cpu=false;
    unsigned before=uploads;assert(execute(&vm));assert(uploads-before==5); // four register chunks + texture
    assert(vm.registers.bytes>=4096*3200ull && vm.result_capacity==4096);
    saved_creates=creates;setup(67);assert(execute(&vm));assert(creates==saved_creates);
    unsigned no_reads=register_reads;
    assert(!gl_gpu_vm_read(&vm,199,2,out,67*8)&&!vm.ready&&register_reads==no_reads);
    assert(!gl_gpu_vm_read(&vm,0,1,out,67*4)&&register_reads==no_reads);
    unsigned before_requests=requests;
    job.lanes=4097;assert(!execute(&vm)&&requests==before_requests);setup(67);
    expected_floats--;assert(!execute(&vm)&&requests==before_requests);expected_floats++;
    expected_texture_bytes=3;assert(!execute(&vm)&&requests==before_requests);expected_texture_bytes=16;
    for(unsigned fault=KSH_BAD_PROGRAM;fault<=KSH_BAD_RESOURCE;fault++){
        bad_result=fault;assert(!execute(&vm)&&!vm.ready&&!vm.quarantined&&vm.bad_lane==66&&vm.lane_error==fault);
        assert(!gl_gpu_vm_read(&vm,SR_POSITION,1,out,67*4));
    }
    bad_result=99;assert(!execute(&vm)&&!vm.ready&&!vm.quarantined);bad_result=0;
    bad_steps=9;assert(!execute(&vm)&&!vm.ready&&!vm.quarantined);
    bad_steps=99;assert(!execute(&vm)&&!vm.ready&&!vm.quarantined);bad_steps=0;
    assert(execute(&vm));
    busy=true;before_requests=requests;assert(!execute(&vm)&&!vm.ready&&!vm.quarantined&&vm.last_error==-EBUSY);
    assert(requests==before_requests+32);busy=false;assert(execute(&vm));
    destroy_busy=true;assert(!gl_gpu_vm_release(&vm)&&!vm.ready&&!vm.quarantined&&vm.registers.handle);
    destroy_busy=false;assert(gl_gpu_vm_release(&vm));assert(!vm.registers.handle&&!vm.status.handle&&!vm.textures.handle&&!vm.results);
    for(unsigned scenario=0;scenario<7;scenario++){
        model_reset();vm=(gl_gpu_vm_t){0};setup(67);
        if(scenario==0)fail_cpu=true;
        if(scenario==1)fail_create=2;
        if(scenario==2)fail_upload=1;
        if(scenario==3)fail_launch=true;
        if(scenario==4)fail_status_read=true;
        if(scenario==5)bad_result=99;
        if(scenario==6)bad_steps=9;
        assert(!execute(&vm)&&!vm.ready);
        if(scenario<4)assert(status_reads==0);
        if(scenario==0)assert(requests==0);
        unsigned count=requests;
        if(scenario>=2&&scenario<=4){
            assert(vm.quarantined);assert(!execute(&vm)&&requests==count);
            assert(!gl_gpu_vm_release(&vm)&&requests==count&&vm.registers.handle);
        }else assert(gl_gpu_vm_release(&vm));
    }
    model_reset();vm=(gl_gpu_vm_t){0};setup(4096);assert(execute(&vm));
    float *large=malloc(4096*800*4);assert(large);fail_register_read=2;
    assert(!gl_gpu_vm_read(&vm,0,200,large,4096*800)&&vm.quarantined&&!vm.ready);
    before_requests=requests;assert(!gl_gpu_vm_release(&vm)&&requests==before_requests);
    free(large);model_reset();
    vm=(gl_gpu_vm_t){0};active=&vm;
    static kshr_job_t raster;
    raster.clip_w=67;raster.clip_h=1;raster.command_count=1;raster.code_count=2;raster.budget=8;
    assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.last_error==-EINVAL&&!requests);
    assert(gl_gpu_vm_set_textures(&vm,texture_data,16));
    assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.ready&&vm.raster_output&&vm.lanes==67);
    unsigned texture_uploads=uploads,raster_creates=creates;
    for(unsigned i=0;i<100;i++)assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));
    assert(uploads==texture_uploads&&creates==raster_creates&&!register_reads); // no per-tile texture or pixel transfer
    assert(!gl_gpu_vm_read(&vm,SR_POSITION,1,out,67*4)&&vm.last_error==-EINVAL&&!register_reads);
    setup(67);assert(gl_gpu_vm_execute_resident(&vm,&job,inputs,expected_floats));
    assert(uploads==texture_uploads+1&&!vm.raster_output); // only vertex registers upload
    assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));
    bad_result=KSH_DISCARDED;assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&!vm.quarantined&&!vm.ready);
    bad_result=99;assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));bad_result=0;
    bad_steps=9;assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));bad_steps=0;
    assert(gl_gpu_vm_set_textures(&vm,NULL,0)&&!vm.textures.handle&&vm.texture_valid);
    assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));
    assert(gl_gpu_vm_release(&vm)&&!vm.raster_packet&&!vm.texture_valid);
    vm=(gl_gpu_vm_t){0};active=&vm;assert(gl_gpu_vm_set_textures(&vm,NULL,0));
    raster.clip_w=4097;raster.clip_h=1;raster.budget=8;
    raster.code[0].op=SH_SIN; // generic GPU VM: preserve its 4096-worker boundary
    assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.lanes==4096);
    assert(!vm.raster_scratch.handle&&vm.registers.bytes<14u*1024*1024&&vm.status.bytes==65536); // modeled 64KiB allocation granularity
    // Only lane zero owns two pixels. Last lane still has an eight-step limit;
    // accepting a uniform ceil(pixels/lanes) budget would miss this corruption.
    bad_steps=9;assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.bad_lane==4095&&!vm.quarantined);
    bad_steps=0;raster.clip_w=4096;raster.clip_h=4096;raster.budget=1;raster.code_count=1;raster.code[0].op=SH_END;
    // Model uses two steps on odd lanes, within the aggregate 4096-step bound.
    assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));
    assert(vm.lanes==KSHR_FAST_MAX_LANES&&vm.status.bytes==KSHR_FAST_MAX_LANES*8);
    assert(kshr_job_scratch_bytes(&raster)==0);
    before_requests=requests;raster.clip_w++;
    assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&requests==before_requests);
    assert(gl_gpu_vm_release(&vm));
    raster.clip_w=67;raster.clip_h=1;raster.budget=8;raster.code_count=2;
    vm=(gl_gpu_vm_t){0};active=&vm;assert(gl_gpu_vm_set_textures(&vm,NULL,0));
    fail_cpu=true;before_requests=requests;assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.last_error==-ENOMEM);
    assert(requests==before_requests);fail_cpu=false;
    assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));
    busy=true;before_requests=requests;assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.last_error==-EBUSY);
    assert(requests==before_requests+32&&!vm.quarantined);busy=false;
    fail_launch=true;before_requests=status_reads;
    assert(!gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster)&&vm.quarantined&&status_reads==before_requests);
    before_requests=requests;assert(!gl_gpu_vm_release(&vm)&&requests==before_requests&&vm.registers.handle);
    model_reset();
    unsigned sparse_spans=0,sparse_regs=0;
    const unsigned touched[]={0,14,63,64,65,66,94,100,127,128,129,130,131,132,133,134,135,136,192,195,198};
    for(unsigned i=0;i<sizeof(touched)/sizeof(*touched);i++)sparse_used[touched[i]]=true;
    for(unsigned i=0;i<SR_REGISTERS;i++)if(sparse_used[i]){sparse_regs++;if(!i||!sparse_used[i-1])sparse_spans++;}
    for(unsigned fault=0;fault<=sparse_spans;fault++){
        vm=(gl_gpu_vm_t){0};active=&vm;setup(67);assert(execute(&vm));
        memset(get(vm.registers.handle)->data,0xa5,(size_t)vm.registers.bytes);
        job.code_count=3;
        job.code[0]=(sh_instruction_t){.op=SH_MATMUL,.dst=127,.src={63,195,198}};
        job.code[1]=(sh_instruction_t){.op=SH_MOV,.dst=14,.src={94,100,136}};
        job.code[2]=(sh_instruction_t){.op=SH_END};
        sparse_mode=true;unsigned before_upload=uploads,before_launch=launches,before_status=status_reads;
        uint64_t before_bytes=uploaded_bytes;
        if(fault)fail_upload=uploads+fault;
        bool ok=gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats);
        if(fault){
            assert(!ok&&vm.quarantined&&!vm.ready&&launches==before_launch&&status_reads==before_status);
            unsigned before=requests;assert(!gl_gpu_vm_release(&vm)&&requests==before);
        }else{
            assert(ok&&vm.vertex_output&&uploads-before_upload==sparse_spans);
            assert(uploaded_bytes-before_bytes==(uint64_t)sparse_regs*67u*16u);
            /* Cache allocation waits for a repeated SoA layout, so alternating
             * mesh/ground lane counts do not churn large shadow allocations. */
            assert(!vm.vertex_shadow);
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            /* Only destinations/vertex outputs need retransferring. The
             * current shader writes 0,14,127; outputs are always reinitialized. */
            uint64_t cached_before=uploaded_bytes;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            assert(uploaded_bytes-cached_before==12u*67u*16u);
            /* Same pointer, changed bytes: a cached source must be uploaded. */
            float old=inputs[94u*67u*4u];inputs[94u*67u*4u]=old+1;
            cached_before=uploaded_bytes;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            assert(uploaded_bytes-cached_before==13u*67u*16u);
            inputs[94u*67u*4u]=old;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            /* A bytecode change makes an old source writable. Model a device
             * store, then remove that write: the old shadow must not survive. */
            job.code[1].dst=94;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            memset(get(vm.registers.handle)->data+94u*67u*16u,0xa5,67u*16u);
            job.code[1].dst=14;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            /* Fragment scratch never aliases cached vertex input. */
            uint64_t vertex_handle=vm.registers.handle;
            assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));
            assert(vm.raster_scratch.handle&&vm.raster_scratch.handle!=vertex_handle);
            cached_before=uploaded_bytes;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            assert(uploaded_bytes-cached_before==12u*67u*16u);
            /* Cache allocation is optional, even after a prior cached draw. */
            free(vm.vertex_shadow);vm.vertex_shadow=NULL;vm.vertex_shadow_capacity=0;
            fail_cpu=true;cached_before=uploaded_bytes;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            assert(uploaded_bytes-cached_before==(uint64_t)sparse_regs*67u*16u);
            fail_cpu=false;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            /* Resource pressure falls back before raster submission, never
             * retaining vertex cache validity across shared scratch writes. */
            assert(vm_drop(&vm,&vm.raster_scratch));fail_create=creates+1;
            assert(gl_gpu_vm_raster(&vm,0xddd,0xeee,&raster));fail_create=0;
            assert(vm.raster_packet->scratch==vm.registers.handle);
            memset(get(vm.registers.handle)->data,0xa5,(size_t)vm.registers.bytes);
            cached_before=uploaded_bytes;
            assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
            assert(uploaded_bytes-cached_before==(uint64_t)sparse_regs*67u*16u);
            assert(gl_gpu_vm_read(&vm,SR_POSITION,1,out,67*4));
            unsigned before=register_reads;assert(!gl_gpu_vm_read(&vm,0,1,out,67*4)&&register_reads==before);
            sparse_mode=false;assert(execute(&vm)&&!vm.vertex_output);
            assert(gl_gpu_vm_read(&vm,0,1,out,67*4));assert(gl_gpu_vm_release(&vm));
        }
        model_reset();
    }
    printf("PASS sparse vertex transport: %u/200 initialized registers in %u spans, poisoned unused planes, matrix boundary, restricted readback, every partial upload failure quarantined before launch\n",sparse_regs,sparse_spans);
    model_reset();
    // Model the op9 transaction/result contract, NOT setup/raster arithmetic.
    static kshr_job_t fragment;
    fragment=(kshr_job_t){.width=16,.height=16,.pitch=16,.depth_pitch=16,
        .clip_w=16,.clip_h=16,.code_count=1,.budget=1};
    kshs_config_t geometry={.version=KSHS_ABI,.lanes=6,.triangle_count=2,
        .framebuffer_width=16,.framebuffer_height=16,.viewport_w=16,.viewport_h=16,
        .clip_w=16,.clip_h=16,.depth_far=1};
    for(unsigned fault=0;fault<=5;fault++){
        vm=(gl_gpu_vm_t){0};active=&vm;setup(6);job.texture_count=0;geometry_vertex=true;
        expected_texture=NULL;expected_texture_bytes=0;
        assert(gl_gpu_vm_set_textures(&vm,NULL,0));
        assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
        geometry_fault=fault;unsigned before_reads=register_reads,before_status=status_reads;
        unsigned before_launches=launches;
        bool ok=gl_gpu_vm_geometry(&vm,0xddd,0xeee,&geometry,&fragment);
        assert(geometry_launches==1&&launches==before_launches);
        assert(register_reads==before_reads&&status_reads==before_status); // no CPU vertex/status reread
        if(!fault||fault==5){
            assert(ok&&vm.ready&&!vm.quarantined&&vm.raster_output&&!vm.vertex_output);
            assert(vm.geometry_triangles==(fault?0:2)&&vm.geometry_instructions==(fault?0:123));
            unsigned count=creates;
            assert(gl_gpu_vm_release(&vm)&&destroys==count&&!vm.geometry_packet);
            assert(!vm.geometry_workspace.handle&&!vm.geometry_status.handle&&!vm.raster_scratch.handle);
        }else{
            assert(!ok&&!vm.ready&&!vm.geometry_triangles&&!vm.geometry_instructions);
            assert(vm.quarantined==(fault!=2));
            unsigned calls=requests;
            assert(!gl_gpu_vm_geometry(&vm,0xddd,0xeee,&geometry,&fragment));
            assert(requests==calls&&geometry_launches==1); // consumed vertex job cannot be retried
            uint64_t workspace=vm.geometry_workspace.handle,status=vm.geometry_status.handle;
            if(vm.quarantined){
                unsigned old_destroys=destroys;
                assert(!gl_gpu_vm_release(&vm)&&destroys==old_destroys&&!vm.geometry_packet);
                assert(vm.geometry_workspace.handle==workspace&&vm.geometry_status.handle==status);
            }else assert(gl_gpu_vm_release(&vm)&&destroys==creates);
        }
        model_reset();
    }
    // Every lazy device allocation and the packet allocation may fail before
    // op9; no geometry submission, no lost fenced allocation, no CPU replay.
    for(unsigned allocation=0;allocation<4;allocation++){
        vm=(gl_gpu_vm_t){0};active=&vm;setup(6);job.texture_count=0;geometry_vertex=true;
        expected_texture=NULL;expected_texture_bytes=0;
        assert(gl_gpu_vm_set_textures(&vm,NULL,0));
        assert(gl_gpu_vm_execute_vertex(&vm,&job,inputs,expected_floats));
        if(!allocation)fail_cpu=true;else fail_create=creates+allocation;
        assert(!gl_gpu_vm_geometry(&vm,0xddd,0xeee,&geometry,&fragment));
        assert(vm.last_error==-ENOMEM&&!vm.quarantined&&!geometry_launches);
        fail_cpu=false;fail_create=0;unsigned live=0;
        for(unsigned i=0;i<32;i++)live+=pool[i].handle!=0;
        unsigned old_destroys=destroys;assert(gl_gpu_vm_release(&vm)&&destroys-old_destroys==live);
        model_reset();
    }
    puts("PASS modeled op9: exact ABI, distinct resident buffers, emitted/instruction result, zero-output success, no vertex readback/replay, malformed result quarantine and all lazy/release failures");
    free(inputs);model_reset();
    gate_mode=true;assert(gl_gpu_vm_validate());assert(launches==3&&destroys==3);
    for(unsigned phase=1;phase<=3;phase++){
        model_reset();gate_mode=true;gate_corrupt=phase;assert(!gl_gpu_vm_validate());assert(launches==phase);
    }
    model_reset();gate_mode=true;fail_launch=true;
    assert(!gl_gpu_vm_validate()&&launches==1&&status_reads==0&&destroys==0);
    model_reset();
    puts("PASS actual GPU VM transport: 100 reuse batches, dynamic growth/failed growth, 4MiB chunks, unused texture release, all-lane status checks, busy return, partial/unfenced rejection; real gate rejects corrupt vertex/fragment/texture results (modeled syscalls)");
}
'''
    run_test(c,'gl_gpu_vm',compile_only='--compile-only' in sys.argv[1:])
