"""Execute real live timing transaction with NVIDIA KAPI types, modeled hardware.

Checks retained-surface/scaler commands, commit/retirement, confirmation, rollback,
owner/epoch failures and unchanged logical desktop. Not a physical modeset test.
"""
from test_gpu_stable_candidate import ROOT, function, run_test
from test_nvkms_hotplug import source

def main():
    code = source()
    code = code.replace('static NvU64 timer_now_us(void){return 0;}',
                        'static NvU64 clock_us=1000;static NvU64 timer_now_us(void){return clock_us;}')
    code = code.replace('static void sched_sleep_ms(unsigned ms){(void)ms;assert(!"worker loop not invoked by a modeled scheduler");}',
                        'static void sched_sleep_ms(unsigned ms);')
    code = code.replace('assert(!strcmp(name,"nvkms-hotplug")&&fn&&!arg);',
                        'assert((!strcmp(name,"nvkms-hotplug")||!strcmp(name,"nvkms-output"))&&fn&&!arg);')
    client = (ROOT/'kernel/nvkms_kapi_client.c').read_text()
    # Desktop rendering retains the original scanout geometry and only writes
    # its pixels. It must not replay boot timing/flip configuration after Keep.
    for name in ('runtime_select','runtime_present_locked','runtime_fill_locked',
                 'runtime_copy_locked','nvkms_kapi_runtime_present_surface','runtime_draw_locked'):
        body=function(client,name)
        for forbidden in ('applyModeSetConfig','build_requested_config','build_flip_config',
                          'present_prepared_slot','present_phase'):
            assert forbidden not in body,(name,forbidden)
    channel=(ROOT/'kernel/nv_chan.c').read_text()
    for name in ('nv_chan_bind_scanout','nv_surface_present'):
        body=function(channel,name)
        assert 'applyModeSetConfig' not in body and 'nvkms_kapi_' not in body
    extra = r'''
#define E_IO 5
#define E_NOMEM 12
static bool owner_alive=true,guard_held;
static bool proc_gpu_owner_alive(u64 owner){return owner_alive&&(owner==77||owner==88);}
static bool nv_render_try_begin(void){assert(!guard_held);guard_held=true;return true;}
static void nv_render_end(void){assert(guard_held);guard_held=false;}
static void sched_sleep_ms(unsigned ms){clock_us+=(u64)ms*1000;}
static struct {bool ready;} prepared;
static dl_layout_t runtime_layout;
'''
    for name in ('set_all_layer_flags','build_display_config','snapshot_flip_sequences','wait_for_flip_mask'):
        extra += function(client, name)+'\n'
    extra += '#include "'+(ROOT/'kernel/nvkms_output_mode.h').as_posix()+'"\n'
    extra += r'''
static unsigned validate_count,commit_count;
static bool reject_validate,fail_commit,no_completion,race_validate,race_commit,exit_validate,fail_restore;
static struct NvKmsKapiDisplayMode hardware_mode;
static NvBool apply_timing(struct NvKmsKapiDevice *d,const struct NvKmsKapiRequestedModeSetConfig *q,
                          struct NvKmsKapiModeSetReplyConfig *r,NvBool commit){
    assert(d==test_device&&q->headsMask==2);
    const struct NvKmsKapiHeadRequestedConfig *h=&q->headRequestedConfig[1];
    assert(h->modeSetConfig.displays[0]==0x800&&h->modeSetConfig.bActive&&h->flags.modeChanged);
    assert(h->layerRequestedConfig[0].config.surface==active[1].surface[0]);
    assert(h->layerRequestedConfig[0].config.srcWidth==2560&&h->layerRequestedConfig[0].config.srcHeight==1440);
    assert(h->layerRequestedConfig[0].config.dstWidth==h->modeSetConfig.mode.timings.hVisible);
    assert(h->layerRequestedConfig[0].config.dstHeight==h->modeSetConfig.mode.timings.vVisible);
    assert(h->layerRequestedConfig[0].config.minPresentInterval==1&&!h->layerRequestedConfig[0].config.tearing);
    r->flipResult=NV_KMS_FLIP_RESULT_SUCCESS;
    if(!commit){
        validate_count++;
        if(race_validate){race_validate=false;nvkms_watch_event(0x800);}
        if(exit_validate)owner_alive=false;
        if(reject_validate || (fail_restore&&h->modeSetConfig.mode.timings.hVisible==2560))return NV_FALSE;
        return NV_TRUE;
    }
    commit_count++;hardware_mode=h->modeSetConfig.mode;
    if(race_commit){race_commit=false;nvkms_watch_event(0x800);}
    if(fail_commit){fail_commit=false;return NV_FALSE;} // may have changed hardware before failure
    if(!no_completion){
        struct NvKmsKapiEvent e={0};e.device=test_device;e.type=NVKMS_EVENT_TYPE_FLIP_OCCURRED;
        e.u.flipOccurred.layer=0;e.u.flipOccurred.head=1;nvkms_test_event_cb(&e);
    }
    return NV_TRUE;
}
static kdisplay_output_mode_request_t timing_request(void){
    return (kdisplay_output_mode_request_t){watch_events,0x800,0x800,1};
}
static void timing_reset(void){
    memset(&output_mode,0,sizeof output_mode);memset(&runtime_layout,0,sizeof runtime_layout);
    owner_alive=true;clock_us=1000;validate_count=commit_count=0;
    reject_validate=fail_commit=no_completion=race_validate=race_commit=exit_validate=fail_restore=false;
    thread_fail=false;prepared.ready=false;path_count=active_count=3;resources.numHeads=3;
    paths[0]=0x200;paths[1]=0x800;paths[2]=0x2000;connected=0x2a00;
    resources.caps.pitchAlignment=256;
    for(unsigned i=0;i<3;i++){
        active[i].handle=active[i].connector_handle=paths[i];active[i].head=i;active[i].static_info.headMask=7;
        active[i].mode.timings.hVisible=2560;active[i].mode.timings.vVisible=1440;
        active[i].mode.timings.refreshRate=240000;active[i].mode.timings.pixelClockHz=900001234;
        active[i].pitch=10240;active[i].bytes=10240ull*1440;active[i].front=0;
        active[i].surface[0]=(void*)(uintptr_t)(100+i);active[i].memory[0]=(void*)(uintptr_t)(200+i);
        runtime_layout.out[i].active=1;
    }
    hardware_mode=active[1].mode;
    watch_ready=false;watch_events=1;watch_dirty=0;nvkms_watch_boot_boundary();nvkms_watch_start();
    watch_live.output[1].mode_count=2;
    watch_live.mode[1][1]=(kdisplay_mode_t){1920,1080,144000,0};
    watch_live.native_mode[1][1]=active[1].mode;
    watch_live.native_mode[1][1].timings.hVisible=1920;watch_live.native_mode[1][1].timings.vVisible=1080;
    watch_live.native_mode[1][1].timings.refreshRate=144000;
    watch_live.native_mode[1][1].timings.pixelClockHz=333123456;
    kapi.applyModeSetConfig=apply_timing;
}
static void check_live_timings(void){
    timing_reset();test_display_t saved[4];memcpy(saved,active,sizeof saved);
    dl_layout_t canvas=runtime_layout;kdisplay_output_mode_request_t q=timing_request();
    s64 token=nvkms_kapi_apply_output_mode(77,&q);
    assert(token==1&&output_mode.pending&&validate_count==1&&commit_count==1);
    assert(hardware_mode.timings.hVisible==1920&&hardware_mode.timings.refreshRate==144000);
    assert(hardware_mode.timings.pixelClockHz==333123456); // exact cached native record, not label reconstruction
    assert(watch_live.output[1].width==1920&&watch_live.output[1].flags&KDISPLAY_OUTPUT_SCALED);
    assert(watch_live.mode[1][1].flags&KDISPLAY_MODE_CURRENT);
    assert(!(watch_live.mode[1][0].flags&KDISPLAY_MODE_CURRENT));
    assert(!memcmp(saved,active,sizeof saved)&&!memcmp(&canvas,&runtime_layout,sizeof canvas));
    assert(nvkms_kapi_finish_output_mode(88,token,true)==-E_PERM&&output_mode.pending);
    assert(nvkms_kapi_finish_output_mode(77,token+1,true)==-E_NOENT&&output_mode.pending);
    assert(!nvkms_kapi_finish_output_mode(77,token,true)&&!output_mode.pending);
    // A scan of another changed connector copies this confirmed timing,
    // rather than rebuilding it from active[].mode's retained canvas size.
    nvkms_watch_event(0x200);assert(watch_rescan());
    assert(watch_live.output[1].width==1920&&watch_live.output[1].height==1080&&
           watch_live.output[1].refresh_millihz==144000);
    assert(watch_live.mode[1][1].flags&KDISPLAY_MODE_CURRENT);
    q=timing_request();q.mode_index=0;token=nvkms_kapi_apply_output_mode(77,&q);
    assert(token==2&&hardware_mode.timings.hVisible==2560);
    assert(!nvkms_kapi_finish_output_mode(77,token,false)&&hardware_mode.timings.hVisible==1920);
    assert(!memcmp(saved,active,sizeof saved));
    // If THIS sink changes, do not assert either the prior live mode or boot
    // timing: reprobe publishes detected-only with unknown current signal.
    nvkms_watch_event(0x800);assert(watch_rescan());
    assert((watch_live.output[1].flags&KDISPLAY_DETECTED_ONLY)&&
           !watch_live.output[1].width&&!watch_live.output[1].refresh_millihz);
    for(unsigned problem=0;problem<9;problem++){
        timing_reset();q=timing_request();s64 expected=-E_BUSY;
        if(problem==0)q.generation++;
        if(problem==1)q.connector_id=0,expected=-E_INVAL;
        if(problem==2)q.output_id=0x4000,expected=-E_INVAL;
        if(problem==3)q.mode_index=100;
        if(problem==4)watch_live.output[1].flags|=KDISPLAY_DETECTED_ONLY;
        if(problem==5)watch_live.output[1].flags&=~KDISPLAY_CONNECTED;
        if(problem==6)prepared.ready=true;
        if(problem==7)thread_fail=true,expected=-E_NOMEM;
        if(problem==8)owner_alive=false,expected=-E_PERM;
        assert(nvkms_kapi_apply_output_mode(77,&q)==expected&&!commit_count&&!output_mode.pending);
    }
    timing_reset();q=timing_request();reject_validate=true;
    assert(nvkms_kapi_apply_output_mode(77,&q)==-E_INVAL&&!commit_count);
    timing_reset();q=timing_request();race_validate=true;
    assert(nvkms_kapi_apply_output_mode(77,&q)==-E_BUSY&&!commit_count);
    timing_reset();q=timing_request();exit_validate=true;
    assert(nvkms_kapi_apply_output_mode(77,&q)==-E_BUSY&&!commit_count);
    timing_reset();q=timing_request();race_commit=true;
    assert(nvkms_kapi_apply_output_mode(77,&q)==-E_BUSY&&commit_count==2&&!output_mode.pending);
    assert(hardware_mode.timings.hVisible==2560);
    timing_reset();q=timing_request();fail_commit=true;
    assert(nvkms_kapi_apply_output_mode(77,&q)==-E_IO&&commit_count==2&&!output_mode.pending);
    assert(hardware_mode.timings.hVisible==2560);
    timing_reset();q=timing_request();no_completion=true;
    assert(nvkms_kapi_apply_output_mode(77,&q)==-E_IO&&output_mode.pending&&output_mode.failed);
    no_completion=false;clock_us=output_mode.retry;output_mode_reap();assert(!output_mode.pending);
    for(unsigned cause=0;cause<3;cause++){
        timing_reset();q=timing_request();token=nvkms_kapi_apply_output_mode(77,&q);assert(token>0);
        if(cause==0){clock_us=output_mode.expires-1;output_mode_reap();assert(output_mode.pending);clock_us++;}
        if(cause==1)owner_alive=false;
        if(cause==2)nvkms_watch_event(0x800);
        output_mode_reap();assert(!output_mode.pending&&hardware_mode.timings.hVisible==2560);
    }
    timing_reset();q=timing_request();token=nvkms_kapi_apply_output_mode(77,&q);fail_restore=true;
    assert(nvkms_kapi_finish_output_mode(77,token,false)==-E_IO&&output_mode.pending&&output_mode.failed);
    unsigned commits=commit_count;output_mode_reap();assert(commit_count==commits); // bounded retry, no tight loop
    fail_restore=false;clock_us=output_mode.retry;output_mode_reap();assert(!output_mode.pending);
    // A foreign device's event must not satisfy our retirement barrier.
    struct NvKmsKapiEvent foreign={0};foreign.device=(void*)(uintptr_t)2;foreign.type=NVKMS_EVENT_TYPE_FLIP_OCCURRED;
    foreign.u.flipOccurred.layer=0;foreign.u.flipOccurred.head=1;unsigned sequence=flip_sequence[1];
    nvkms_test_event_cb(&foreign);assert(sequence==flip_sequence[1]);
    puts("PASS live output timing: real KAPI request/scaler, exact modes, retained resources, confirmation/expiry/owner/hotplug rollback, partial commit, missing completion and recovery retry; hardware modeled");
}
'''
    marker = '#include "'+(ROOT/'kernel/nvkms_display_plan.h').as_posix()+'"\n'
    extra = 'static void nvkms_test_event_cb(const struct NvKmsKapiEvent *);\n'+extra
    code = code.replace(marker, marker+extra)
    code = code.replace('    puts("PASS deferred hotplug:', '    check_live_timings();\n    puts("PASS deferred hotplug:')
    run_test(code, 'nvkms-output-mode', [ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc'])

if __name__ == '__main__':
    main()
