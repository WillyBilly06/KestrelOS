"""Real native plan, slot allocator, config builder and prepare/cancel/reaper.

Hardware, memory allocation and scheduling are modeled; commit=true is forbidden.
Reuses the hotplug harness so mode identity/epochs are not an unrelated mock.
"""
from test_gpu_stable_candidate import ROOT, function, run_test
from test_nvkms_hotplug import source

def main():
    code=source()
    # The existing hotplug test and the new prepare test run in one process.
    code=code.replace('assert(!strcmp(name,"nvkms-hotplug")&&fn&&!arg);',
                      'assert((!strcmp(name,"nvkms-hotplug")||!strcmp(name,"nvkms-config"))&&fn&&!arg);')
    code=code.replace('static NvU64 timer_now_us(void){return 0;}',
                      'static NvU64 clock_us=1000; static NvU64 timer_now_us(void){return clock_us;}')
    client=(ROOT/'kernel/nvkms_kapi_client.c').read_text()
    extra=r'''
#include <stdlib.h>
#define PAGE_SIZE 4096u
#define E_NOMEM 12
static bool owner_alive=true;
static bool proc_gpu_owner_alive(u64 owner){return owner_alive&&(owner==77||owner==88);}
static bool guard_held;
static bool nv_render_try_begin(void){assert(!guard_held);guard_held=true;return true;}
static void nv_render_end(void){assert(guard_held);guard_held=false;}
static unsigned allocation_attempt,fail_allocation,mem_live,surface_live,ram_live,validation_calls;
static bool invalid_route,all_routes_fail,flip_busy,flip_failed,race_validate,race_allocate;
static bool exit_validate;
struct NvKmsKapiMemory {NvU64 bytes;unsigned surfaces;};
struct NvKmsKapiSurface {struct NvKmsKapiMemory *memory;};
static bool fail_alloc(void){return ++allocation_attempt==fail_allocation;}
static struct NvKmsKapiMemory *allocate_memory(struct NvKmsKapiDevice *d,struct NvKmsKapiAllocateMemoryParams *p){
    assert(d==test_device&&p->layout==NvKmsSurfaceMemoryLayoutPitch&&p->type==NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT);
    assert(p->noDisplayCaching&&p->useVideoMemory&&p->size&&!(p->size&4095)&&p->compressible);
    if(fail_alloc())return NULL;
    struct NvKmsKapiMemory *m=calloc(1,sizeof *m);assert(m);m->bytes=p->size;mem_live++;
    if(race_allocate){race_allocate=false;nvkms_watch_event(0x800);}
    return m;
}
static void free_memory(struct NvKmsKapiDevice *d,struct NvKmsKapiMemory *m){
    assert(d==test_device&&m&&!m->surfaces&&mem_live);mem_live--;free(m);
}
static struct NvKmsKapiSurface *create_surface(struct NvKmsKapiDevice *d,struct NvKmsKapiCreateSurfaceParams *p){
    assert(d==test_device&&p->planes[0].memory&&p->format==NvKmsSurfaceMemoryFormatX8R8G8B8);
    assert(p->planes[0].pitch>=p->width*4&&p->planes[0].pitch%resources.caps.pitchAlignment==0);
    if(fail_alloc())return NULL;
    struct NvKmsKapiSurface *s=calloc(1,sizeof *s);assert(s);s->memory=p->planes[0].memory;
    s->memory->surfaces++;surface_live++;return s;
}
static void destroy_surface(struct NvKmsKapiDevice *d,struct NvKmsKapiSurface *s){
    assert(d==test_device&&s&&s->memory->surfaces&&surface_live);s->memory->surfaces--;surface_live--;free(s);
}
static void *dma_alloc_pages(size_t pages,u64 *physical){
    assert(pages&&physical);if(fail_alloc())return NULL;
    // Only identity is used; the code must not write or submit this shadow yet.
    void *p=calloc(1,1);assert(p);*physical=(uintptr_t)p;ram_live++;return p;
}
static void dma_free_pages(void *p,size_t pages){assert(p&&pages&&ram_live);ram_live--;free(p);}
'''
    for name in ('align_up_u64','set_all_layer_flags','build_display_config','allocate_display_slot','free_display_slot'):
        extra+=function(client.replace('static NvU64 ','static u64 '),name)+'\n'
    extra+='#include "'+(ROOT/'kernel/nvkms_display_prepare.h').as_posix()+'"\n'
    extra+=r'''
static NvBool atomic_validate(struct NvKmsKapiDevice *d,const struct NvKmsKapiRequestedModeSetConfig *c,
                              struct NvKmsKapiModeSetReplyConfig *reply,NvBool commit){
    assert(d==test_device&&!commit);validation_calls++;
    unsigned expected_heads=7;
    for(unsigned i=0;i<prepared.plan.count;i++)expected_heads|=1u<<prepared.plan.displays[i].head;
    assert(c==&prepared.config&&c->headsMask==expected_heads);
    for(unsigned i=0;i<prepared.plan.count;i++){
        const test_display_t *o=&prepared.plan.displays[i];
        const struct NvKmsKapiHeadRequestedConfig *h=&c->headRequestedConfig[o->head];
        assert(h->modeSetConfig.bActive&&h->modeSetConfig.displays[0]==o->handle);
        assert(!memcmp(&h->modeSetConfig.mode,&o->mode,sizeof o->mode));
        assert(h->layerRequestedConfig[0].config.surface==o->surface[0]);
    }
    reply->flipResult=flip_busy?NV_KMS_FLIP_RESULT_IN_PROGRESS:
        flip_failed?NV_KMS_FLIP_RESULT_INVALID_PARAMS:NV_KMS_FLIP_RESULT_SUCCESS;
    if(race_validate){race_validate=false;nvkms_watch_event(0x800);}
    if(exit_validate)owner_alive=false;
    if(all_routes_fail || (invalid_route&&prepared.plan.displays[0].head==0))return NV_FALSE;
    return NV_TRUE;
}
static void check_prepares(void){
    path_count=3;paths[0]=0x200;paths[1]=0x800;paths[2]=0x2000;connected=0x2a00;
    watch_ready=false;watch_events=1;watch_dirty=0;nvkms_watch_boot_boundary();nvkms_watch_start();
    resources.numHeads=3;resources.caps.hasVideoMemory=NV_TRUE;resources.caps.pitchAlignment=256;
    kapi.allocateMemory=allocate_memory;kapi.freeMemory=free_memory;
    kapi.createSurface=create_surface;kapi.destroySurface=destroy_surface;kapi.applyModeSetConfig=atomic_validate;
    kdisplay_configuration_t q=configuration();
    test_display_t saved[4];memcpy(saved,active,sizeof saved);
    kdisplay_prepared_configuration_t out,untouched;memset(&untouched,0xa5,sizeof untouched);
    thread_fail=true;out=untouched;
    assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_NOMEM&&!prepared_reaper_started);
    thread_fail=false;assert(!memcmp(&out,&untouched,sizeof out)&&!allocation_attempt);
    // Every partial scanout allocation/create and the final RAM allocation fail.
    for(unsigned failed=1;failed<=13;failed++){
        allocation_attempt=0;fail_allocation=failed;out=untouched;
        assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_NOMEM);
        assert(!prepared.ready&&!mem_live&&!surface_live&&!ram_live&&!memcmp(&out,&untouched,sizeof out));
        assert(!memcmp(active,saved,sizeof saved)&&!validation_calls);
    }
    fail_allocation=0;invalid_route=true;
    assert(!nvkms_kapi_prepare_configuration(77,&q,&out)&&prepared.ready&&validation_calls>1);
    assert(prepared.plan.displays[0].head!=0&&mem_live==6&&surface_live==6&&ram_live==1);
    assert(!memcmp(active,saved,sizeof saved)&&out.configuration.width==7680&&out.expires_us==clock_us+30000000);
    u64 first=out.token;kdisplay_prepared_configuration_t kept=out;
    assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_BUSY&&!memcmp(&out,&kept,sizeof out));
    assert(nvkms_kapi_cancel_configuration(88,first)==-E_PERM&&prepared.ready);
    assert(nvkms_kapi_cancel_configuration(77,first+1)==-E_NOENT&&prepared.ready);
    assert(!nvkms_kapi_cancel_configuration(77,first)&&!prepared.ready&&!mem_live&&!surface_live&&!ram_live);
    assert(nvkms_kapi_cancel_configuration(77,first)==-E_NOENT);
    assert(!nvkms_kapi_prepare_configuration(77,&q,&out));u64 released=out.token;
    // Simulate a syscall-authorized framebuffer handoff while owner 77 lives.
    assert(!nvkms_kapi_prepare_configuration(88,&q,&out)&&prepared.owner==88&&out.token!=released);
    assert(nvkms_kapi_cancel_configuration(77,released)==-E_NOENT&&prepared.ready);
    assert(!nvkms_kapi_cancel_configuration(88,out.token)&&!mem_live&&!surface_live&&!ram_live);
    invalid_route=false;
    // Drop to one monitor: retained old head mask must detach omitted heads.
    q.count=1;q.primary_output_id=q.outputs[0].output_id;
    assert(!nvkms_kapi_prepare_configuration(77,&q,&out));
    assert(prepared.config.headsMask==7&&prepared.config.headRequestedConfig[1].flags.activeChanged);
    assert(!prepared.config.headRequestedConfig[1].modeSetConfig.bActive);
    assert(!prepared.config.headRequestedConfig[1].layerRequestedConfig[0].config.surface);
    assert(!nvkms_kapi_cancel_configuration(77,out.token));q=configuration();
    for(unsigned failure=0;failure<5;failure++){
        validation_calls=0;all_routes_fail=failure==0;flip_busy=failure==1;flip_failed=failure==2;
        race_validate=failure==3;race_allocate=failure==4;out=untouched;
        assert(nvkms_kapi_prepare_configuration(77,&q,&out)==(failure>=3||failure==1?-E_BUSY:-E_INVAL));
        assert(!prepared.ready&&!mem_live&&!surface_live&&!ram_live&&!memcmp(&out,&untouched,sizeof out));
        if(failure==0||failure==2)assert(validation_calls==6);
        if(failure==1||failure==3)assert(validation_calls==1);
        if(failure==4)assert(!validation_calls);
        all_routes_fail=flip_busy=flip_failed=false;assert(watch_rescan());q=configuration();
    }
    for(unsigned reason=0;reason<3;reason++){
        assert(!nvkms_kapi_prepare_configuration(77,&q,&out)&&out.token>first);
        if(reason==0){clock_us=out.expires_us-1;prepared_reap();assert(prepared.ready);clock_us++;}
        if(reason==1)owner_alive=false;
        if(reason==2)nvkms_watch_event(0x800);
        prepared_reap();assert(!prepared.ready&&!mem_live&&!surface_live&&!ram_live);
        owner_alive=true;assert(watch_rescan());q=configuration();
    }
    exit_validate=true;assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_BUSY);
    assert(!mem_live&&!surface_live&&!ram_live);exit_validate=false;owner_alive=true;
    // Newly detected fourth HDMI output and all 24 four-head assignments.
    resources.numHeads=4;paths[path_count++]=0x4000;connected|=0x4000;nvkms_watch_event(0x4000);
    assert(watch_rescan());q=configuration();q.count=4;
    q.outputs[3]=(kdisplay_selection_t){0x4000,0x4000,1,0,7680,0,{0}};
    for(unsigned failed=1;failed<=17;failed++){
        allocation_attempt=0;fail_allocation=failed;
        assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_NOMEM);
        assert(!mem_live&&!surface_live&&!ram_live&&!prepared.ready);
    }
    fail_allocation=0;
    assert(!nvkms_kapi_prepare_configuration(77,&q,&out)&&prepared.config.headsMask==15&&mem_live==8);
    assert(prepared.plan.displays[3].handle==0x4000&&!prepared.plan.displays[3].connector_is_dp);
    assert(!nvkms_kapi_cancel_configuration(77,out.token));
    all_routes_fail=true;validation_calls=0;
    assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_INVAL&&validation_calls==24);
    assert(!mem_live&&!surface_live&&!ram_live);all_routes_fail=false;
    assert(nvkms_kapi_prepare_configuration(0,&q,&out)==-E_PERM);
    assert(nvkms_kapi_prepare_configuration(77,NULL,&out)==-E_INVAL);
    prepared_token=~0ull;assert(nvkms_kapi_prepare_configuration(77,&q,&out)==-E_BUSY);
    assert(!memcmp(active,saved,sizeof saved));
    puts("PASS prepare lifecycle: actual slot allocator/config builder, all partial allocation failures, NVIDIA validation across head permutations, explicit detach, busy/raced/failed validation, owner/token checks, expiry/exit/hotplug cleanup; no commit, VMM binding or active-state mutation");
}
'''
    marker='#include "'+(ROOT/'kernel/nvkms_display_plan.h').as_posix()+'"\n'
    # Prototypes used by checks above are defined later in the reused harness.
    extra='static kdisplay_configuration_t configuration(void);\n'+extra
    code=code.replace(marker,marker+extra)
    code=code.replace('    puts("PASS deferred hotplug:', '    check_prepares();\n    puts("PASS deferred hotplug:')
    run_test(code,'nvkms-prepare',[ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc'])

if __name__=='__main__':main()
