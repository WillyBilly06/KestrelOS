#!/usr/bin/env python3
"""Exercise production user-request validation and snapshot boundary."""
from pathlib import Path
import re
import sys
from test_gpu_stable_candidate import run_test, function

ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/'kernel/syscall.c').read_text()
start=SRC.index('static s64 gpu_surface_request(')
end=SRC.index('\n}',start)+2

def main():
    mm = (ROOT/'kernel/mm.h').read_text()
    c = re.search(r'^#define VMM_USER_COPY_MAX\s+[^\n]+', mm, re.M)[0]+'\n'
    c+=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;typedef uint32_t u32;typedef uint8_t u8;typedef int64_t s64;
#define E_INVAL 22
#define E_NOSYS 38
#define E_BUSY 16
#define E_NOMEM 12
#define E_IO 5
#define E_PERM 1
typedef struct proc {u64 gpu_owner_id;bool resource_closing;int pid;struct proc *leader;} proc_t;
typedef struct {u32 width,height,pitch;u64 bytes;} nv_surface_info_t;
static proc_t *proc_shared(proc_t *p){return p->leader?p->leader:p;}
static bool ready=true,locked,refuse_lock,fail_malloc,driver_result=true;
static unsigned locks,unlocks,allocations,frees,draws,transfers,presents,reapers;
static bool irq_enabled=true,revoke_on_alloc,revoke_on_transfer,revoke_on_create,data_revoked;
static bool revoke_on_geometry;
static int fb_owner_pid=7;
static bool nvkms_kapi_runtime_selected(void){return ready;}
static bool nv_render_try_begin(void){if(refuse_lock)return false;assert(!locked);locked=true;locks++;return true;}
static void nv_render_end(void){assert(locked);locked=false;unlocks++;}
static void *kmalloc(size_t bytes){assert(locked&&irq_enabled);if(fail_malloc)return NULL;allocations++;if(revoke_on_alloc)data_revoked=true;return malloc(bytes);}
static void kfree(void *p){assert(locked);frees++;free(p);}
static void gpu_surface_reaper(void *p){(void)p;}
static int kthread_create(const char *name,void(*fn)(void*),void *p){assert(locked&&fn==gpu_surface_reaper&&!p);(void)name;reapers++;return 20;}
static void nv_surface_reap_exited(void){assert(locked);}
static uintptr_t user_request,user_data;static size_t user_bytes;
static bool allow_output=true;
static bool user_range_ok(u64 a,size_t n,bool write){
    assert(locked&&irq_enabled); // VM locking now belongs to the copy/range helpers.
    if(a==user_request && n==72)return !write||allow_output;
    return !data_revoked&&a>=user_data&&a-user_data<=user_bytes&&n<=user_bytes-(a-user_data);
}
/* Stub the VM-locked copy boundary, not the production chunking/snapshot logic.
 * Mapping revocation injection above remains authoritative on every copy. */
static bool user_copy(void *buffer,u64 address,size_t bytes,bool write){
    assert(buffer&&bytes<=VMM_USER_COPY_MAX);
    if(!user_range_ok(address,bytes,write))return false;
    if(write)memcpy((void*)address,buffer,bytes);
    else memcpy(buffer,(const void*)address,bytes);
    return true;
}
static u64 expected_owner=111;
static void owner_ok(u64 owner){assert(locked && irq_enabled && owner==expected_owner);}
static bool pool_exhausted;
static u64 nv_surface_create_with_pressure(u64 owner,u32 w,u32 h,bool *pressure){
    owner_ok(owner);assert(w==37&&h==29);*pressure=pool_exhausted;
    if(revoke_on_create)allow_output=false;
    return driver_result&&!pool_exhausted?0x901:0;
}
static unsigned destroys;
static bool nv_surface_destroy(u64 owner,u64 h){owner_ok(owner);assert(h==0x901);destroys++;return driver_result;}
static bool nv_surface_info(u64 owner,u64 h,nv_surface_info_t *out){owner_ok(owner);assert(h==0x901);*out=(nv_surface_info_t){37,29,256,65536};return true;}
static bool nv_surface_transfer(u64 owner,u64 h,u64 off,void *data,u32 n,bool read){
    owner_ok(owner);assert(h==0x901 && off==4 && n==16 && (uintptr_t)data!=user_data);
    transfers++;
    if(revoke_on_transfer)data_revoked=true;
    if(read)memset(data,0x5a,n);else {assert(!memcmp(data,(void*)user_data,n));memset((void*)user_data,0,n);assert(((unsigned char*)data)[0]==0xa5);}
    return driver_result;
}
'''
    c+='\n#include "'+(ROOT/'include/kestrel/gpu2d.h').as_posix()+'"\n'
    c+='\n#include "'+(ROOT/'include/kestrel/gpu3d.h').as_posix()+'"\n'
    c+='\n#include "'+(ROOT/'include/kestrel/shader_vm.h').as_posix()+'"\n'
    c+='\n#include "'+(ROOT/'include/kestrel/shader_raster.h').as_posix()+'"\n'
    c+='\n#include "'+(ROOT/'include/kestrel/shader_setup.h').as_posix()+'"\n'
    c+=r'''
static bool nv_surface_draw(u64 owner,u64 dst,u64 src,const kg2d_command_t *cmd,u32 n,u32 x,u32 y,u32 w,u32 h){
    owner_ok(owner);assert(dst==0x901 && src==0x902 && n==2 && x==1&&y==2&&w==3&&h==4);
    assert((uintptr_t)cmd!=user_data && cmd[0].colour0==0xabcdef);
    memset((void*)user_data,0,user_bytes);assert(cmd[0].colour0==0xabcdef);draws++;return driver_result;
}
static bool nv_surface_draw3d(u64 owner,u64 dst,u64 depth,u64 tex,const kg3d_command_t *cmd,u32 n,u32 x,u32 y,u32 w,u32 h){
    owner_ok(owner);assert(dst==0x901 && depth==0x903 && tex==0x902 && n==2 && x==1&&y==2&&w==3&&h==4);
    assert((uintptr_t)cmd!=user_data && cmd[0].flags==KG3D_DEPTH_TEST);
    memset((void*)user_data,0,user_bytes);assert(cmd[0].flags==KG3D_DEPTH_TEST);draws++;return driver_result;
}
static bool nv_surface_shader(u64 owner,u64 regs,u64 status,u64 tex,const ksh_dispatch_t *job){
    owner_ok(owner);assert(regs==0x901 && status==0x903 && tex==0x902);
    assert((uintptr_t)job!=user_data && job->code_count==2 && job->code[0].op==SH_MUL);
    memset((void*)user_data,0,user_bytes);
    assert(job->code_count==2 && job->code[0].op==SH_MUL);
    draws++;return driver_result;
}
static bool nv_surface_shader_raster(u64 owner,u64 dst,u64 depth,u64 tex,const kshr_submission_t *s){
    owner_ok(owner);assert(dst==0x901&&depth==0x903&&tex==0x902);
    assert((uintptr_t)s!=user_data&&s->scratch==0x904&&s->status==0x905&&s->job.code_count==2);
    memset((void*)user_data,0,user_bytes);
    assert(s->scratch==0x904&&s->status==0x905&&s->job.code_count==2);
    draws++;return driver_result;
}
static bool nv_surface_shader_geometry(u64 owner,u64 dst,u64 depth,u64 tex,const kshs_submission_t *s,u32 *emitted,u64 *executed){
    owner_ok(owner);assert(dst==0x901&&depth==0x903&&tex==0x902);
    assert((uintptr_t)s!=user_data&&s->registers==0x904&&s->vertex_status==0x905);
    assert(s->workspace==0x906&&s->raster_scratch==0x907&&s->raster_status==0x908);
    assert(s->setup.triangle_count==2&&s->fragment.code_count==2);
    memset((void*)user_data,0,user_bytes);
    assert(s->workspace==0x906&&s->setup.triangle_count==2); // immutable snapshot
    *emitted=3;*executed=12345;
    if(revoke_on_geometry)allow_output=false;
    draws++;return driver_result;
}
static bool nvkms_kapi_runtime_present_surface(u64 owner,u64 h,u32 x,u32 y,u32 w,u32 ht){
    owner_ok(owner);assert(h==0x901&&x==1&&y==2&&w==3&&ht==4);presents++;return driver_result;
}
'''
    c+=function(SRC,'gpu_user_access')+'\n'+SRC[start:end]
    c+=r'''
int main(void){
    proc_t lead={.gpu_owner_id=111,.pid=7},thread={.gpu_owner_id=222,.pid=8,.leader=&lead};
    kg2d_request_t r={.version=KG2D_ABI,.operation=KG2D_CREATE,.width=37,.height=29};
    user_request=(uintptr_t)&r;
    assert(gpu_surface_request(&thread,user_request,71)==-E_INVAL&&!locks);
    ready=false;assert(gpu_surface_request(&thread,user_request,72)==-E_NOSYS&&!locks);ready=true;
    refuse_lock=true;assert(gpu_surface_request(&thread,user_request,72)==-E_BUSY&&!locks);refuse_lock=false;
    assert(gpu_surface_request(&thread,user_request+1,72)==-E_INVAL&&locks==unlocks);
    allow_output=false;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL&&!reapers);allow_output=true;
    r.version=99;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.version=KG2D_ABI;
    lead.resource_closing=true;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);lead.resource_closing=false;
    assert(gpu_surface_request(&thread,user_request,72)==0&&r.handle==0x901&&r.pitch==256&&r.bytes==65536&&reapers==1);
    pool_exhausted=true;r.handle=0xfeed;r.pitch=123;r.bytes=456;
    assert(gpu_surface_request(&thread,user_request,72)==-E_NOMEM);
    assert(r.handle==0xfeed&&r.pitch==123&&r.bytes==456); // no partial output
    pool_exhausted=false;driver_result=false;
    assert(gpu_surface_request(&thread,user_request,72)==-E_IO&&r.handle==0xfeed);
    driver_result=true;
    kg2d_command_t cmd[2]={{.colour0=0xabcdef},{0}};
    user_data=(uintptr_t)cmd;user_bytes=sizeof cmd;
    r=(kg2d_request_t){.version=KG2D_ABI,.operation=KG2D_DRAW,.handle=0x901,.source=0x902,
        .data=user_data,.count=2,.x=1,.y=2,.width=3,.height=4};
    assert(gpu_surface_request(&thread,user_request,72)==0&&draws==1);
    r.count=257;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL&&draws==1);
    r.count=0;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);
    r.count=2;fail_malloc=true;assert(gpu_surface_request(&thread,user_request,72)==-E_NOMEM);fail_malloc=false;
    r.data++;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.data--;
    r.operation=KG2D_PRESENT;
    fb_owner_pid=8;assert(gpu_surface_request(&thread,user_request,72)==-E_PERM&&!presents);
    fb_owner_pid=7;assert(gpu_surface_request(&thread,user_request,72)==0&&presents==1);
    r.operation=KG2D_UPLOAD;r.offset=4;r.bytes=16;memset(cmd,0xa5,16);
    assert(gpu_surface_request(&thread,user_request,72)==0&&transfers==1);
    r.bytes=KG2D_TRANSFER_MAX+4;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);
    r.bytes=15;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);
    r.bytes=16;r.offset=1;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.offset=4;
    r.operation=KG2D_DOWNLOAD;assert(gpu_surface_request(&thread,user_request,72)==0);
    assert(((unsigned char*)cmd)[0]==0x5a);
    driver_result=false;memset(cmd,0x33,16);assert(gpu_surface_request(&thread,user_request,72)==-E_IO);
    assert(((unsigned char*)cmd)[0]==0x33); /* no failed/partial readback published */
    driver_result=true;r.operation=KG2D_DESTROY;assert(gpu_surface_request(&thread,user_request,72)==0);
    r.operation=100;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);
    kg3d_command_t triangles[2]={{.flags=KG3D_DEPTH_TEST},{0}};
    user_data=(uintptr_t)triangles;user_bytes=sizeof triangles;
    r=(kg2d_request_t){.version=KG2D_ABI,.operation=KG2D_DRAW3D,.handle=0x901,.source=0x902,
        .offset=0x903,.data=user_data,.count=2,.x=1,.y=2,.width=3,.height=4};
    assert(gpu_surface_request(&thread,user_request,72)==0&&draws==2);
    r.count=65;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL&&draws==2);
    r.count=0;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);
    r.count=2;fail_malloc=true;assert(gpu_surface_request(&thread,user_request,72)==-E_NOMEM);fail_malloc=false;
    user_bytes--;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);user_bytes++;
    triangles[0].flags=KG3D_DEPTH_TEST;driver_result=false;
    assert(gpu_surface_request(&thread,user_request,72)==-E_IO&&draws==3);driver_result=true;
    static ksh_dispatch_t job;
    job.code_count=2;job.code[0].op=SH_MUL;
    user_data=(uintptr_t)&job;user_bytes=sizeof job;
    r=(kg2d_request_t){.version=KG2D_ABI,.operation=KG2D_SHADER_VM,.handle=0x901,
        .offset=0x903,.source=0x902,.data=user_data,.bytes=sizeof job};
    assert(gpu_surface_request(&thread,user_request,72)==0 && draws==4);
    r.bytes--;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.bytes+=2;
    assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.bytes--;
    user_bytes--;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);user_bytes++;
    r.data++;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.data--;
    fail_malloc=true;assert(gpu_surface_request(&thread,user_request,72)==-E_NOMEM);fail_malloc=false;
    job.code_count=2;job.code[0].op=SH_MUL;driver_result=false;
    assert(gpu_surface_request(&thread,user_request,72)==-E_IO && draws==5);
    driver_result=true;static kshr_submission_t raster;
    raster.scratch=0x904;raster.status=0x905;raster.job.code_count=2;
    user_data=(uintptr_t)&raster;user_bytes=sizeof raster;
    r.operation=KG2D_SHADER_RASTER;r.data=user_data;r.bytes=sizeof raster;
    assert(gpu_surface_request(&thread,user_request,72)==0&&draws==6);
    r.bytes--;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.bytes+=2;
    assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);r.bytes--;
    user_bytes--;assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);user_bytes++;
    fail_malloc=true;assert(gpu_surface_request(&thread,user_request,72)==-E_NOMEM);fail_malloc=false;
    raster.scratch=0x904;raster.status=0x905;raster.job.code_count=2;driver_result=false;
    assert(gpu_surface_request(&thread,user_request,72)==-E_IO&&draws==7);
    assert(!locked&&locks==unlocks&&allocations==frees);
    // A sibling revokes the input after preflight, during allocation. No
    // submission may observe that pointer; the kernel snapshot is cleaned up.
    driver_result=true;revoke_on_alloc=true;raster.job.code_count=2;
    unsigned before=draws;
    assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL&&draws==before);
    revoke_on_alloc=data_revoked=false;
    r=(kg2d_request_t){.version=KG2D_ABI,.operation=KG2D_DOWNLOAD,.handle=0x901,
        .data=user_data,.offset=4,.bytes=16};
    memset(&raster,0x33,16);revoke_on_transfer=true;
    assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL);
    assert(((u8*)&raster)[0]==0x33); // revoked destination was not touched
    revoke_on_transfer=data_revoked=false;
    r=(kg2d_request_t){.version=KG2D_ABI,.operation=KG2D_CREATE,.width=37,.height=29};
    revoke_on_create=true;before=destroys;
    assert(gpu_surface_request(&thread,user_request,72)==-E_INVAL&&destroys==before+1);
    assert(r.handle==0); // unreturnable new handle is retired, never leaked
    assert(irq_enabled&&!locked&&locks==unlocks&&allocations==frees);
    allow_output=true;revoke_on_create=false;
    static kshs_submission_t geometry;
    _Static_assert(sizeof(geometry)==56984,"op9 exact packet ABI");
    for(unsigned fault=0;fault<7;fault++){
        geometry=(kshs_submission_t){.registers=0x904,.vertex_status=0x905,
            .workspace=0x906,.raster_scratch=0x907,.raster_status=0x908,
            .setup={.triangle_count=2},.fragment={.code_count=2}};
        user_data=(uintptr_t)&geometry;user_bytes=sizeof geometry;
        r=(kg2d_request_t){.version=KG2D_ABI,.operation=KG2D_SHADER_GEOMETRY,
            .handle=0x901,.offset=0x903,.source=0x902,.data=user_data,.bytes=sizeof geometry,.count=0xfeed};
        before=draws;driver_result=fault!=3;revoke_on_geometry=fault==4;
        if(fault==1)r.bytes--;
        if(fault==2)r.bytes++;
        if(fault==5)allow_output=false;
        if(fault==6)revoke_on_alloc=true;
        s64 result=gpu_surface_request(&thread,user_request,72);
        if(!fault)assert(!result&&r.count==3&&r.bytes==12345&&draws==before+1);
        else{
            assert(result==(fault==3?-E_IO:-E_INVAL));
            assert(r.count==0xfeed&&r.bytes==sizeof geometry+(fault==1?-1:fault==2?1:0));
            assert(draws==before+(fault==3||fault==4)); // copyout failure does not internally replay
        }
        allow_output=true;revoke_on_geometry=revoke_on_alloc=data_revoked=false;
        assert(irq_enabled&&!locked&&locks==unlocks&&allocations==frees);
    }
    puts("PASS actual GPU surface syscall: ABI, address-space owner, framebuffer ownership, snapshots, transfers, failed readback, bounds, allocation failure and balanced transaction release");
    return 0;
}
'''
    run_test(c,'gpu_surface_syscall',compile_only='--compile-only' in sys.argv[1:])

if __name__=='__main__':main()
