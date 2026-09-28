"""Production deferred monitor inventory, real KAPI types, modeled events/AUX.

No physical link training, live modeset, allocation or rendering is performed.
"""
from test_gpu_stable_candidate import ROOT, run_test, function

def source():
    client=(ROOT/'kernel/nvkms_kapi_client.c').read_text()
    # The test's boot seed must describe the inventory collected before the
    # long RGBW/engine phases, not a new epoch at framebuffer publication.
    # A helper-only regression cannot detect accidentally moving this caller.
    publish=function(client,'nvkms_kapi_publish_runtime_framebuffer')
    assert 'nvkms_watch_boot_boundary' not in publish
    boundary=client.index('    nvkms_watch_boot_boundary();')
    assert client.count('    nvkms_watch_boot_boundary();')==1
    assert client.index('r->display[i].pixel_clock_hz = d->mode.timings.pixelClockHz;') < boundary
    assert boundary < client.index('if (!present_phase(7, count))')
    code=r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
'''
    code+='#include "'+(ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc/nvkms-kapi.h').as_posix()+'"\n'
    code+='#include "'+(ROOT/'include/kestrel/display_info.h').as_posix()+'"\n'
    a=client.index('typedef struct {\n    NvKmsKapiDisplay handle;')
    code+=client[a:client.index('} test_display_t;',a)+len('} test_display_t;')]+'\n'
    code+='#include "'+(ROOT/'kernel/display_layout.c').as_posix()+'"\n'
    code+=r'''
#define TEST_MAX_ENUM_DISPLAYS 32
#define KESTREL_NVKMS_MAX_TEST_DISPLAYS 4
#define E_PERM 1
#define E_NOENT 2
#define E_BUSY 16
#define E_NODEV 19
#define E_INVAL 22
#define E_NOSYS 38
typedef uint32_t u32;typedef uint64_t u64;typedef int64_t s64;
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
typedef struct {char manufacturer[4],model[32];} edid_info_t;
static test_display_t active[4];static NvU32 active_count=3;
static struct NvKmsKapiDeviceResourcesInfo resources;
static bool runtime_ready=true;
static struct NvKmsKapiDevice *test_device=(void*)(uintptr_t)1;
static struct NvKmsKapiFunctionsTable kapi;
static NvU32 flip_sequence[NVKMS_KAPI_MAX_HEADS];
static unsigned yields,threads,queries[32],mode_queries;
static bool thread_fail,enumeration_fail,duplicate,query_fail,mode_reject,mode_fail,race_event;
static bool bad_edid_length,invalid_dp_list;
static NvU32 paths[32]={0x200,0x800,0x2000};static unsigned path_count=3,connected=0x2a00;
static void sched_yield(void){yields++;assert(!"unexpected contended test lock");}
static void sched_sleep_ms(unsigned ms){(void)ms;assert(!"worker loop not invoked by a modeled scheduler");}
static NvU64 timer_now_us(void){return 0;}
static int kthread_create(const char *name,void(*fn)(void*),void*arg){assert(!strcmp(name,"nvkms-hotplug")&&fn&&!arg);threads++;return thread_fail?-1:1;}
static size_t strlcpy(char*d,const char*s,size_t n){size_t len=strlen(s);if(n){size_t take=len<n-1?len:n-1;memcpy(d,s,take);d[take]=0;}return len;}
static bool edid_parse(const unsigned char *p,unsigned n,edid_info_t*out){
    if(n!=128||p[0]!=1)return false;
    strlcpy(out->manufacturer,"ACR",4);strlcpy(out->model,"Live panel",32);return true;
}
static NvU32 bit_index(NvU32 h){unsigned n=0;assert(h&&!(h&(h-1)));while((1u<<n)!=h)n++;return n;}
static void nvkms_watch_event(NvU32);
static NvBool get_displays(struct NvKmsKapiDevice*d,NvU32*n,NvKmsKapiDisplay*out){
    assert(d==test_device&&*n==32);if(enumeration_fail)return NV_FALSE;
    *n=path_count;memcpy(out,paths,path_count*sizeof *out);if(duplicate)out[1]=out[0];return NV_TRUE;
}
static NvBool get_static(struct NvKmsKapiDevice*d,NvKmsKapiDisplay h,struct NvKmsKapiStaticDisplayInfo*out){
    assert(d==test_device);out->handle=h;out->connectorHandle=h;out->headMask=15;return NV_TRUE;
}
static NvBool get_connector(struct NvKmsKapiDevice*d,NvKmsKapiConnector h,struct NvKmsKapiConnectorInfo*out){
    assert(d==test_device);out->handle=h;out->signalFormat=NVKMS_CONNECTOR_SIGNAL_FORMAT_DP;
    if(h==0x4000){out->signalFormat=NVKMS_CONNECTOR_SIGNAL_FORMAT_TMDS;out->type=NVKMS_CONNECTOR_TYPE_HDMI;}
    out->dynamicDpyIdListValid=!invalid_dp_list;return NV_TRUE;
}
static NvBool get_dynamic(struct NvKmsKapiDevice*d,struct NvKmsKapiDynamicDisplayParams*out){
    assert(d==test_device&&!out->overrideEdid&&!out->forceConnected&&!out->forceDisconnected&&!out->edid.bufferSize);
    queries[bit_index(out->handle)]++;
    if(query_fail)return NV_FALSE;
    out->connected=!!(out->handle&connected);
    if(out->connected){out->edid.bufferSize=128;out->edid.buffer[0]=1;}
    if(bad_edid_length)out->edid.bufferSize=sizeof out->edid.buffer+1;
    if(race_event){race_event=false;nvkms_watch_event(0x800);}
    return NV_TRUE;
}
static int get_mode(struct NvKmsKapiDevice*d,NvKmsKapiDisplay h,NvU32 i,struct NvKmsKapiDisplayMode*m,NvBool*v,NvBool*p){
    assert(d==test_device&&(h&connected));mode_queries++;
    if(mode_fail&&i==1)return -1;
    if(i>=3){*v=NV_TRUE;m->timings.hVisible=9999;return 0;} // ignore end-marker payload
    memset(m,0,sizeof *m);m->timings.hVisible=2560;m->timings.vVisible=1440;
    m->timings.refreshRate=i?239970:60000;*v=NV_TRUE;*p=i==2;
    m->timings.pixelClockHz=987654321+i;m->timings.hSyncStart=2608;m->timings.hSyncEnd=2640;
    m->timings.hTotal=2720;m->timings.vTotal=1481;m->timings.flags.hSyncPos=1;
    strcpy(m->name,i?"high exact timing":"low exact timing");
    return 1;
}
static NvBool validate(struct NvKmsKapiDevice*d,NvKmsKapiDisplay h,const struct NvKmsKapiDisplayMode*m){
    assert(d==test_device&&(h&connected)&&m->timings.hVisible==2560);return !mode_reject;
}
static bool nvkms_kapi_display_output(uint32_t,kdisplay_output_t*);
static bool nvkms_kapi_display_mode(uint32_t,uint32_t,kdisplay_mode_t*);
static NvBool resolve_display_mode(NvU32 i,NvU32 d,NvU32 c,NvU32 m,struct NvKmsKapiDisplayMode*out){
    if(i>=3||active[i].handle!=d||active[i].connector_handle!=c||m)return NV_FALSE;
    memset(out,0,sizeof *out);out->timings.hVisible=2560;out->timings.vVisible=1440;
    out->timings.refreshRate=240000;out->timings.pixelClockHz=900001234;
    out->timings.hTotal=2701;strcpy(out->name,"boot exact timing");return NV_TRUE;
}
typedef struct {u32 width,height,pitch;u64 bytes;} nv_surface_info_t;
#define NV_SURFACE_MAX_BYTES 0x10000000ull
'''
    chan=(ROOT/'kernel/nv_chan.c').read_text()
    code+=function(chan,'nv_surface_dimensions')+'\n'
    code+=r'''
static bool plan_epoch_race;
bool nv_surface_query_dimensions(u32 w,u32 h,nv_surface_info_t*out){
    if(plan_epoch_race)nvkms_watch_event(0x800);
    return nv_surface_dimensions(w,h,out);
}
'''
    code+='#include "'+(ROOT/'kernel/nvkms_display_watch.h').as_posix()+'"\n'
    code+='#include "'+(ROOT/'kernel/nvkms_display_plan.h').as_posix()+'"\n'
    code+=function(client,'nvkms_test_event_cb')+'\n'
    code+=r'''
static bool nvkms_kapi_display_output(uint32_t i,kdisplay_output_t*out){
    int r=nvkms_watch_output(i,out);if(r>=0)return r;
    if(i>=active_count)return false;
    memset(out,0,sizeof *out);out->version=KDISPLAY_INFO_VERSION;out->output_id=active[i].handle;
    out->connector_id=active[i].connector_handle;out->flags=KDISPLAY_CONNECTED|KDISPLAY_ENABLED;
    out->width=2560;out->height=1440;out->refresh_millihz=240000;out->mode_count=1;
    return true;
}
static bool nvkms_kapi_display_mode(uint32_t i,uint32_t m,kdisplay_mode_t*out){
    int r=nvkms_watch_mode(i,m,out);if(r>=0)return r;
    if(i>=active_count||m)return false;
    *out=(kdisplay_mode_t){2560,1440,240000,KDISPLAY_MODE_CURRENT};return true;
}
static bool ui_race_modes,ui_race_outputs,ui_empty;
static int enum_display_output(uint32_t i,kdisplay_output_t*out){
    if(ui_empty)return -1;
    if(ui_race_outputs&&i==1){ui_race_outputs=false;nvkms_watch_event(0x800);assert(watch_rescan());}
    return nvkms_kapi_display_output(i,out)?0:-1;
}
static int enum_display_output_mode(uint32_t i,uint32_t m,kdisplay_mode_t*out){
    if(ui_race_modes&&m==1){ui_race_modes=false;nvkms_watch_event(0x200);}
    return nvkms_kapi_display_mode(i,m,out)?0:-1;
}
typedef struct {int x,y,w,h;}rect_t;
'''
    ui=(ROOT/'user/desktop/display_settings.h').read_text()
    a=ui.index('typedef struct {')
    code+=ui[a:ui.index('} display_settings_t;',a)+len('} display_settings_t;')]+'\n'
    for name in ('ds_rates','ds_select_output','ds_load','ds_refresh','ds_only_selection_safe'):
        code+=function(ui,name)+'\n'
    code+=r'''
static void changed(NvU32 h){
    struct NvKmsKapiEvent e={0};e.device=test_device;e.type=NVKMS_EVENT_TYPE_DPY_CHANGED;e.u.displayChanged.display=h;
    nvkms_test_event_cb(&e);
}
static kdisplay_configuration_t configuration(void){
    kdisplay_configuration_t q={0};q.version=KDISPLAY_CONFIGURATION_VERSION;
    q.generation=watch_events;q.count=3;q.primary_output_id=0x800;q.group_mode=DL_EXTEND;
    for(unsigned i=0;i<3;i++)q.outputs[i]=(kdisplay_selection_t){active[i].handle,active[i].connector_handle,0,0,(int)i*2560,0,{0}};
    return q;
}
static void check_plans(void){
    kdisplay_configuration_t q=configuration();nvkms_display_plan_t p,untouched;
    memset(&untouched,0xa5,sizeof untouched);p=untouched;
    assert(!plan_configuration(&q,&p)&&p.layout.fb_width==7680&&p.layout.fb_height==1440&&p.primary==1);
    assert(p.displays[0].mode.timings.pixelClockHz==900001234&&p.displays[0].mode.timings.hTotal==2701);
    assert(!strcmp(p.displays[0].mode.name,"boot exact timing")&&!p.displays[0].memory[0]&&!p.displays[0].surface[0]);
    kdisplay_configuration_check_t answer;
    assert(!nvkms_kapi_check_configuration(&q,&answer)&&answer.width==7680&&answer.gpu_pitch==30720);
    assert(answer.gpu_bytes>=7680ull*1440*4&&!(answer.gpu_bytes&65535));
    // IDs, not array order, select both the monitor and primary.
    kdisplay_selection_t swap=q.outputs[0];q.outputs[0]=q.outputs[2];q.outputs[2]=swap;
    assert(!plan_configuration(&q,&p)&&p.displays[0].handle==0x2000&&p.primary==1);
    q=configuration();q.outputs[0].x=-2560;q.outputs[1].x=0;q.outputs[2].x=2560;
    assert(!plan_configuration(&q,&p)&&p.layout.origin_x==-2560&&p.layout.out[1].src_x==2560);
    q.group_mode=DL_MIRROR;q.outputs[1].rotation_degrees=90;
    assert(!plan_configuration(&q,&p)&&p.layout.fb_width==1440&&p.layout.fb_height==2560);
    for(unsigned i=0;i<3;i++)assert(p.layout.out[i].src_w==1440&&p.layout.out[i].src_h==2560);
    q=configuration();q.count=1;q.primary_output_id=q.outputs[0].output_id;
    for(unsigned r=0;r<4;r++){
        q.outputs[0].rotation_degrees=r*90;assert(!plan_configuration(&q,&p));
        assert(p.layout.fb_width==(r&1?1440:2560)&&p.layout.fb_height==(r&1?2560:1440));
    }
    // Retaining a preferred head must backtrack, not strand a restrictive sink.
    q=configuration();watch_live.static_info[0].headMask=3;watch_live.static_info[1].headMask=1;
    assert(!plan_configuration(&q,&p)&&p.displays[0].head==1&&p.displays[1].head==0);
    watch_live.static_info[0].headMask=watch_live.static_info[1].headMask=15;
    for(unsigned invalid=0;invalid<18;invalid++){
        q=configuration();int want=-E_INVAL;
        switch(invalid){
        case 0:q.version++;break;case 1:q.count=0;break;case 2:q.count=5;break;
        case 3:q.generation++;want=-E_BUSY;break;
        case 4:q.primary_output_id=0x4000;break;
        case 5:q.outputs[1].output_id=q.outputs[0].output_id;break;
        case 6:q.outputs[1].connector_id=123;want=-E_NODEV;break;
        case 7:q.outputs[1].mode_index=1;break;
        case 8:q.outputs[1].rotation_degrees=360;break;
        case 9:q.outputs[1].x=100;break;
        case 10:q.outputs[1].x=INT_MAX;break;
        case 11:q.group_mode=3;break;
        case 12:q.outputs[0].reserved[0]=1;break;case 13:q.reserved[1]=1;break;
        case 14:q.outputs[0].output_id=0x4000;want=-E_NOENT;break;
        case 15:q.outputs[2].x=12000;q.outputs[2].y=6000;break; // GPU VMM slot capacity
        case 16:watch_live.static_info[0].headMask=0;break;
        case 17:resources.caps.maxWidthInPixels=1920;break;
        }
        p=untouched;assert(plan_configuration(&q,&p)==want&&!memcmp(&p,&untouched,sizeof p));
        watch_live.static_info[0].headMask=15;resources.caps.maxWidthInPixels=32768;
    }
    q=configuration();plan_epoch_race=true;p=untouched;
    assert(plan_configuration(&q,&p)==-E_BUSY&&!memcmp(&p,&untouched,sizeof p));plan_epoch_race=false;
    assert(watch_rescan());
    puts("PASS native configuration preflight: exact cached timings, identity/epoch, reorder/primary, rotations/mirror/negative positions, overlap/overflow/resource refusal, head backtracking; no allocations or hardware Apply");
}
int main(void){
    kapi.getDisplays=get_displays;kapi.getStaticDisplayInfo=get_static;kapi.getConnectorInfo=get_connector;
    kapi.getDynamicDisplayInfo=get_dynamic;kapi.getDisplayMode=get_mode;kapi.validateDisplayMode=validate;
    resources.numHeads=4;resources.caps.maxWidthInPixels=resources.caps.maxHeightInPixels=32768;
    for(unsigned i=0;i<4;i++)resources.numLayers[i]=1;
    for(unsigned i=0;i<3;i++){
        active[i].handle=paths[i];active[i].connector_handle=paths[i];active[i].head=i;
        active[i].static_info.handle=paths[i];active[i].static_info.connectorHandle=paths[i];active[i].static_info.headMask=15;
    }
    nvkms_watch_boot_boundary();
    thread_fail=true;nvkms_watch_start();assert(!watch_ready&&threads==1);
    thread_fail=false;nvkms_watch_start();assert(watch_ready&&threads==2&&watch_live.count==3);
    kdisplay_output_t o; kdisplay_mode_t m;
    assert(nvkms_watch_output(0,&o)==1&&o.reserved==1&&o.width==2560);
    assert(watch_rescan()&&!mode_queries); // no periodic reprobe of the unchanged Acer
    struct NvKmsKapiEvent e={0};e.device=test_device;e.type=NVKMS_EVENT_TYPE_FLIP_OCCURRED;
    e.u.flipOccurred.layer=NVKMS_KAPI_LAYER_PRIMARY_IDX;e.u.flipOccurred.head=1;
    nvkms_test_event_cb(&e);assert(flip_sequence[1]==1&&watch_events==1&&!watch_dirty);
    e.device=(void*)(uintptr_t)2;e.type=NVKMS_EVENT_TYPE_DPY_CHANGED;e.u.displayChanged.display=0x800;
    nvkms_test_event_cb(&e);assert(watch_events==1&&!watch_dirty);
    check_plans();
    // Restore seed counters; the planner race test deliberately detected 0x800.
    watch_ready=false;watch_events=1;watch_dirty=0;nvkms_watch_boot_boundary();nvkms_watch_start();
    memset(queries,0,sizeof queries);mode_queries=0;
    connected&=~0x800;changed(0x800);
    assert(nvkms_watch_output(0,&o)==1&&(o.flags&KDISPLAY_STALE));
    assert(!nvkms_watch_mode(0,0,&m));
    assert(watch_rescan()&&watch_live.count==3&&watch_live.generation==2);
    assert(queries[11]==1&&!queries[9]&&!queries[13]);
    assert(nvkms_watch_output(1,&o)==1&&!(o.flags&KDISPLAY_CONNECTED)&&!o.width&&!o.mode_count);
    connected|=0x800;changed(0x800);assert(watch_rescan());
    assert(nvkms_watch_output(1,&o)==1&&(o.flags&KDISPLAY_DETECTED_ONLY)&&!o.width&&o.mode_count==2);
    assert(nvkms_watch_mode(1,1,&m)==1&&m.refresh_millihz==239970&&(m.flags&KDISPLAY_MODE_PREFERRED));
    assert(!(m.flags&KDISPLAY_MODE_CURRENT));
    kdisplay_configuration_t rq=configuration();rq.outputs[1].mode_index=1;nvkms_display_plan_t rp;
    assert(!plan_configuration(&rq,&rp)&&rp.displays[1].mode.timings.pixelClockHz==987654322);
    assert(rp.displays[1].mode.timings.hSyncStart==2608&&!strcmp(rp.displays[1].mode.name,"high exact timing"));
    paths[path_count++]=0x4000;connected|=0x4000;
    e.device=test_device;e.type=NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED;e.u.dynamicDisplayConnected.display=0x4000;
    nvkms_test_event_cb(&e);assert(watch_rescan()&&watch_live.count==4&&queries[14]==1);
    assert(nvkms_watch_output(3,&o)&&!strcmp(o.connector,"HDMI"));
    assert(!queries[9]&&!queries[13]);
    // A new event while detecting invalidates the whole candidate and retries.
    changed(0x4000);NvU32 published=watch_live.generation;race_event=true;
    assert(!watch_rescan()&&watch_live.generation==published&&(watch_dirty&0x4800)==0x4800);
    assert(watch_rescan()&&watch_live.generation==watch_events);
    for(unsigned fail=0;fail<6;fail++){
        changed(0x800);published=watch_live.generation;
        enumeration_fail=fail==0;duplicate=fail==1;query_fail=fail==2;mode_fail=fail==3;
        bad_edid_length=fail==4;invalid_dp_list=fail==5;
        assert(!watch_rescan()&&watch_live.generation==published&&watch_dirty);
        enumeration_fail=duplicate=query_fail=mode_fail=bad_edid_length=invalid_dp_list=false;assert(watch_rescan());
    }
    mode_reject=true;changed(0x800);assert(watch_rescan());assert(nvkms_watch_output(1,&o)&&!o.mode_count);
    mode_reject=false;
    display_settings_t settings={0};ds_load(&settings);
    assert(settings.output_count==4&&!settings.inventory_pending&&settings.mode_count==1);
    unsigned reads=mode_queries;assert(!ds_refresh(&settings)&&mode_queries==reads);
    ui_race_modes=true;ds_select_output(&settings,0);
    assert(settings.inventory_pending&&!settings.mode_count&&!settings.rate_count&&!settings.resolution_count);
    assert(!ds_only_selection_safe(&settings));
    assert(watch_rescan()&&ds_refresh(&settings)&&!settings.inventory_pending&&settings.mode_count==2);
    assert(settings.modes[0].refresh_millihz==239970&&settings.mode_indices[0]==1&&settings.mode_indices[1]==0);
    assert(!ds_only_selection_safe(&settings)); // newly detected outputs have no stable startup ordinal yet
    ui_race_outputs=true;ds_load(&settings);
    assert(settings.inventory_pending&&!settings.mode_count&&!settings.rate_count&&!settings.resolution_count);
    assert(ds_refresh(&settings)&&!settings.inventory_pending);
    ui_empty=true;ds_load(&settings);assert(!settings.output_count&&!settings.mode_count&&!settings.rate_count);
    ui_empty=false;assert(ds_refresh(&settings)&&settings.output_count==4);
    // Every physical monitor removed, including a dynamic id disappearing.
    connected=0;path_count=0;changed(0);assert(watch_rescan()&&watch_live.count==3);
    for(unsigned i=0;i<3;i++){assert(nvkms_watch_output(i,&o)&&!o.flags&&!o.mode_count);}
    assert(!nvkms_watch_output(8,&o)&&!nvkms_watch_mode(8,0,&m));
    assert(!nvkms_watch_output(0,NULL)&&!nvkms_watch_mode(0,0,NULL));
    watch_events=UINT_MAX;changed(0);assert(watch_events==1); // zero reserved for legacy ABI
    // Changes after final boot inventory, during RGBW/engine tests and the
    // desktop gate, must all survive deferred watcher startup. Publication
    // cannot clear them (the production call-placement assertions above).
    watch_ready=false;nvkms_watch_boot_boundary();
    NvU32 boot_epoch=watch_boot_generation;
    changed(0x200);changed(0x800);changed(0x2000);
    assert(watch_dirty==0x2a00 && watch_boot_generation==boot_epoch);
    nvkms_watch_start();
    assert(nvkms_watch_output(1,&o)&&(o.flags&KDISPLAY_STALE));
    assert(watch_rescan()&&watch_live.generation==watch_events);
    puts("PASS deferred hotplug: actual callback, epoch/stale gating, no unchanged-sink probing, disconnect/reconnect/new output, raced/failed scan retry, paired/deduplicated modes, all unplugged; no modesets or buffer ownership changes");
}
'''
    return code

def main():
    run_test(source(),'nvkms-hotplug',[ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc'])

if __name__=='__main__':main()
