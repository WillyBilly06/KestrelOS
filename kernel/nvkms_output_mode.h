/* Live output timings with retained scanout storage. Every entry and watchdog
 * holds nv_render's guard. No allocation, VMM alias, active[] geometry or user
 * mapping changes here: src remains the original scanout, dst is the new mode.
 * This is output scaling, not a logical-desktop resize or topology transaction. */
#ifndef KESTREL_NVKMS_OUTPUT_MODE_H
#define KESTREL_NVKMS_OUTPUT_MODE_H
#define OUTPUT_MODE_CONFIRM_US 20000000ull
static struct {
    NvBool seeded, worker, pending, failed;
    NvU32 slot, generation;
    u64 owner, token, next_token, expires, retry;
    struct NvKmsKapiDisplayMode current[KESTREL_NVKMS_MAX_TEST_DISPLAYS], previous;
    struct NvKmsKapiRequestedModeSetConfig request;
    struct NvKmsKapiModeSetReplyConfig reply;
} output_mode;

static NvBool output_mode_build(NvU32 slot,const struct NvKmsKapiDisplayMode *mode) {
    if(slot>=active_count || !runtime_layout.out[slot].active ||
       !build_display_config(active,active_count,NULL,0,&output_mode.request))return NV_FALSE;
    /* Touch precisely one existing head, retaining the known front surface and
     * source extent. NVIDIA's validator checks scaler/DSC/link feasibility. */
    output_mode.request.headsMask=1u<<active[slot].head;
    struct NvKmsKapiHeadRequestedConfig *h=&output_mode.request.headRequestedConfig[active[slot].head];
    h->modeSetConfig.mode=*mode;
    h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX].config.dstWidth=mode->timings.hVisible;
    h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX].config.dstHeight=mode->timings.vVisible;
    return NV_TRUE;
}
static NvBool output_mode_submit(NvBool commit) {
    memset(&output_mode.reply,0,sizeof output_mode.reply);
    return kapi.applyModeSetConfig(test_device,&output_mode.request,&output_mode.reply,commit) &&
           output_mode.reply.flipResult==NV_KMS_FLIP_RESULT_SUCCESS;
}
static NvBool output_mode_commit(void) {
    NvU32 before[NVKMS_KAPI_MAX_HEADS];snapshot_flip_sequences(before);
    return output_mode_submit(NV_TRUE) &&
           wait_for_flip_mask(output_mode.request.headsMask,before,"live output timing");
}
/* Publish one coherent inventory epoch only if no hotplug invalidated this
 * transaction. Do not call the detection worker or fabricate a new mode list. */
static NvBool output_mode_publish(NvU32 slot,NvU32 generation) {
    NvBool ok=NV_FALSE;
    watch_lock();
    int index=watch_find(&watch_live,active[slot].handle);
    if(index>=0 && watch_live.generation==generation &&
       watch_live.output[index].connector_id==active[slot].connector_handle){
        NvU32 next=generation+1;if(!next)next=1;
        if(__atomic_compare_exchange_n(&watch_events,&generation,next,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            const struct NvKmsKapiDisplayMode *m=&output_mode.current[slot];
            kdisplay_output_t *o=&watch_live.output[index];
            o->width=m->timings.hVisible;o->height=m->timings.vVisible;
            o->refresh_millihz=m->timings.refreshRate;
            o->flags&=~KDISPLAY_OUTPUT_SCALED;
            if(o->width!=active[slot].mode.timings.hVisible || o->height!=active[slot].mode.timings.vVisible)
                o->flags|=KDISPLAY_OUTPUT_SCALED;
            for(NvU32 j=0;j<o->mode_count;j++){
                kdisplay_mode_t *label=&watch_live.mode[index][j];
                label->flags&=~KDISPLAY_MODE_CURRENT;
                if(label->width==o->width && label->height==o->height && label->refresh_millihz==o->refresh_millihz &&
                   !!(label->flags&KDISPLAY_MODE_INTERLACED)==!!m->timings.flags.interlaced)
                    label->flags|=KDISPLAY_MODE_CURRENT;
            }
            watch_live.generation=next;
            for(NvU32 i=0;i<watch_live.count;i++)watch_live.output[i].reserved=next;
            output_mode.generation=next;ok=NV_TRUE;
        }
    }
    watch_unlock();return ok;
}
static int output_mode_restore(void) {
    if(!output_mode.pending)return -E_NOENT;
    /* Validate the old mode again: a cable may have been removed or a different
     * monitor attached. Never force an EDID override or replay an unchecked
     * mode into the new sink. Retain all resources even on failed rollback. */
    if(!output_mode_build(output_mode.slot,&output_mode.previous) ||
       !output_mode_submit(NV_FALSE) || !output_mode_commit()){
        if(!output_mode.failed)nvkms_watch_event(active[output_mode.slot].handle);
        output_mode.failed=NV_TRUE;output_mode.retry=timer_now_us()+1000000ull;
        kwarn("nvkms-live","output timing rollback pending; original surfaces retained");
        return -E_IO;
    }
    output_mode.current[output_mode.slot]=output_mode.previous;
    if(!output_mode_publish(output_mode.slot,output_mode.generation))
        nvkms_watch_event(active[output_mode.slot].handle);
    output_mode.pending=NV_FALSE;output_mode.failed=NV_FALSE;
    kinfo("nvkms-live","restored output %#x to %ux%u at %u mHz",
          active[output_mode.slot].handle,output_mode.previous.timings.hVisible,
          output_mode.previous.timings.vVisible,output_mode.previous.timings.refreshRate);
    return 0;
}
static void output_mode_reap(void) {
    if(output_mode.pending && timer_now_us()>=output_mode.retry &&
       (output_mode.failed || timer_now_us()>=output_mode.expires ||
        !proc_gpu_owner_alive(output_mode.owner) ||
        __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=output_mode.generation))
        (void)output_mode_restore();
}
static void output_mode_worker(void *unused) {
    (void)unused;
    for(;;){sched_sleep_ms(100);if(nv_render_try_begin()){output_mode_reap();nv_render_end();}}
}
s64 nvkms_kapi_apply_output_mode(u64 owner,const kdisplay_output_mode_request_t *q) {
    if(!owner || !proc_gpu_owner_alive(owner))return -E_PERM;
    if(!q || !runtime_ready || !test_device || !watch_ready)return -E_NODEV;
    output_mode_reap();
    if(output_mode.pending || prepared.ready)return -E_BUSY;
    if(output_mode.next_token>=0x7fffffffffffffffull)return -E_BUSY;
    int slot=watch_boot_slot(q->output_id);
    if(slot<0 || active[slot].connector_handle!=q->connector_id || !runtime_layout.out[slot].active)
        return -E_INVAL;
    struct NvKmsKapiDisplayMode mode;
    watch_lock();
    int index=watch_find(&watch_live,q->output_id);
    NvBool valid=index>=0 && watch_live.generation==q->generation &&
        __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)==q->generation &&
        watch_live.output[index].connector_id==q->connector_id &&
        (watch_live.output[index].flags&KDISPLAY_ENABLED) &&
        (watch_live.output[index].flags&KDISPLAY_CONNECTED) &&
        !(watch_live.output[index].flags&(KDISPLAY_STALE|KDISPLAY_DETECTED_ONLY)) &&
        q->mode_index<watch_live.output[index].mode_count;
    if(valid)mode=watch_live.native_mode[index][q->mode_index];
    watch_unlock();
    if(!valid)return -E_BUSY;
    if(!output_mode.seeded){
        for(NvU32 i=0;i<active_count;i++)output_mode.current[i]=active[i].mode;
        output_mode.seeded=NV_TRUE;
    }
    if(!output_mode.worker){
        if(kthread_create("nvkms-output",output_mode_worker,NULL)<0)return -E_NOMEM;
        output_mode.worker=NV_TRUE;
    }
    if(!output_mode_build((NvU32)slot,&mode) || !output_mode_submit(NV_FALSE))return -E_INVAL;
    if(!proc_gpu_owner_alive(owner) || __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=q->generation)
        return -E_BUSY;
    output_mode.slot=(NvU32)slot;output_mode.previous=output_mode.current[slot];
    output_mode.owner=owner;output_mode.token=++output_mode.next_token;
    output_mode.generation=q->generation;output_mode.failed=NV_FALSE;output_mode.retry=0;
    /* Mark submitted BEFORE the call. A failed/timeout commit does not prove
     * that hardware never consumed any part of the request. */
    output_mode.pending=NV_TRUE;output_mode.expires=timer_now_us()+OUTPUT_MODE_CONFIRM_US;
    if(!output_mode_commit()){
        (void)output_mode_restore();return -E_IO;
    }
    output_mode.current[slot]=mode;
    if(!output_mode_publish((NvU32)slot,q->generation)){
        (void)output_mode_restore();return -E_BUSY;
    }
    output_mode.expires=timer_now_us()+OUTPUT_MODE_CONFIRM_US;
    kinfo("nvkms-live","output %#x applied %ux%u at %u mHz; desktop canvas retained; awaiting confirmation",
          q->output_id,mode.timings.hVisible,mode.timings.vVisible,mode.timings.refreshRate);
    return (s64)output_mode.token;
}
int nvkms_kapi_finish_output_mode(u64 owner,u64 token,bool confirm) {
    if(!owner || !token)return -E_INVAL;
    if(!output_mode.pending || output_mode.token!=token)return -E_NOENT;
    if(output_mode.owner!=owner)return -E_PERM;
    if(!confirm)return output_mode_restore();
    if(output_mode.failed || timer_now_us()>=output_mode.expires ||
       __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=output_mode.generation || !proc_gpu_owner_alive(owner)){
        (void)output_mode_restore();return -E_BUSY;
    }
    output_mode.pending=NV_FALSE;return 0;
}
#endif
