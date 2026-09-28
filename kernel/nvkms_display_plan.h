/* Live configuration preparation. Included after the generation-bound cache.
 * No active scanout, user mapping, mode pool or allocation is modified here.
 * Atomic NVIDIA validation and retirement belong to the subsequent transaction. */
#ifndef KESTREL_NVKMS_DISPLAY_PLAN_H
#define KESTREL_NVKMS_DISPLAY_PLAN_H
typedef struct {
    NvU32 generation,count,primary,group_mode;
    test_display_t displays[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
    dl_layout_t layout;
    nv_surface_info_t compositor;
} nvkms_display_plan_t;

static NvBool plan_assign_heads(nvkms_display_plan_t *plan,NvU32 at,NvU32 used){
    if(at==plan->count)return NV_TRUE;
    test_display_t *d=&plan->displays[at];
    NvU32 allowed=d->static_info.headMask & ((1u<<resources.numHeads)-1u) & ~used;
    int old=watch_boot_slot(d->handle);
    NvU32 preferred=old>=0?active[old].head:resources.numHeads;
    /* Prefer retaining an output's current head, but backtrack when another
     * output has a more restrictive mask. A greedy choice can reject a valid
     * arrangement. Hardware routing validation must still examine assignments. */
    for(NvU32 pass=0;pass<=resources.numHeads;pass++){
        NvU32 head=pass?pass-1:preferred;
        if(head>=resources.numHeads || (pass && head==preferred) ||
           !(allowed&(1u<<head)) || !resources.numLayers[head])continue;
        d->head=head;
        if(plan_assign_heads(plan,at+1,used|(1u<<head)))return NV_TRUE;
    }
    return NV_FALSE;
}

static int plan_configuration(const kdisplay_configuration_t *request,
                              nvkms_display_plan_t *out){
    if(!request || !out || request->version!=KDISPLAY_CONFIGURATION_VERSION ||
       !request->generation || !request->count || request->count>KDISPLAY_MAX_OUTPUTS ||
       request->count>KESTREL_NVKMS_MAX_TEST_DISPLAYS ||
       (request->group_mode!=DL_EXTEND && request->group_mode!=DL_MIRROR) ||
       request->reserved[0] || request->reserved[1] || request->reserved[2])return -E_INVAL;
    if(!runtime_ready || !test_device || !__atomic_load_n(&watch_ready,__ATOMIC_ACQUIRE))return -E_NODEV;
    if(!resources.numHeads || resources.numHeads>NVKMS_KAPI_MAX_HEADS ||
       resources.numHeads>=32 || request->count>resources.numHeads)return -E_INVAL;
    nvkms_display_plan_t next;
    memset(&next,0,sizeof next);
    next.generation=request->generation;next.count=request->count;next.group_mode=request->group_mode;
    dl_placement_t placements[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
    memset(placements,0,sizeof placements);
    int primary=-1,result=-E_INVAL;
    watch_lock();
    if(watch_live.generation!=request->generation ||
       __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=request->generation){result=-E_BUSY;goto unlock;}
    for(NvU32 i=0;i<request->count;i++){
        const kdisplay_selection_t *q=&request->outputs[i];
        if(!q->output_id || !q->connector_id || q->reserved[0] || q->reserved[1])goto unlock;
        for(NvU32 j=0;j<i;j++)if(request->outputs[j].output_id==q->output_id)goto unlock;
        int slot=watch_find(&watch_live,q->output_id);
        if(slot<0){result=-E_NOENT;goto unlock;}
        const kdisplay_output_t *o=&watch_live.output[slot];
        if(o->connector_id!=q->connector_id || !(o->flags&KDISPLAY_CONNECTED)){
            result=-E_NODEV;goto unlock;
        }
        if(q->mode_index>=o->mode_count)goto unlock;
        test_display_t *d=&next.displays[i];
        d->handle=q->output_id;d->connector_handle=q->connector_id;
        d->static_info=watch_live.static_info[slot];
        d->mode=watch_live.native_mode[slot][q->mode_index];
        if(d->static_info.handle!=d->handle || d->static_info.connectorHandle!=d->connector_handle ||
           !d->mode.timings.refreshRate || d->mode.timings.hVisible<resources.caps.minWidthInPixels ||
           d->mode.timings.hVisible>resources.caps.maxWidthInPixels ||
           d->mode.timings.vVisible<resources.caps.minHeightInPixels ||
           d->mode.timings.vVisible>resources.caps.maxHeightInPixels)goto unlock;
        strlcpy(d->manufacturer,o->manufacturer,sizeof d->manufacturer);
        strlcpy(d->model,o->model,sizeof d->model);
        d->connector_is_dp=!strcmp(o->connector,"DisplayPort");
        if(q->output_id==request->primary_output_id)primary=(int)i;
        placements[i]=(dl_placement_t){q->x,q->y,(int)d->mode.timings.hVisible,
            (int)d->mode.timings.vVisible,(int)q->rotation_degrees,1};
    }
    result=0;
unlock:
    watch_unlock();
    if(result)return result;
    if(primary<0 || !display_layout_arrange(placements,(int)next.count,primary,
        (dl_mode_t)next.group_mode,32768,32768,&next.layout))return -E_INVAL;
    next.primary=(NvU32)primary;
    /* Use the actual compositor allocation constraints, including pitch and
     * page rounding. A legal aggregate rectangle may still exceed its VMM slot. */
    if(!nv_surface_query_dimensions((NvU32)next.layout.fb_width,(NvU32)next.layout.fb_height,
                                    &next.compositor) || !plan_assign_heads(&next,0,0))return -E_INVAL;
    if(__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=request->generation)return -E_BUSY;
    *out=next;return 0;
}

int nvkms_kapi_check_configuration(const kdisplay_configuration_t *request,
                                  kdisplay_configuration_check_t *out){
    if(!out)return -E_INVAL;
    nvkms_display_plan_t plan;
    int result=plan_configuration(request,&plan);
    if(result)return result;
    kdisplay_configuration_check_t answer;
    memset(&answer,0,sizeof answer);
    answer.version=KDISPLAY_CONFIGURATION_VERSION;answer.generation=plan.generation;
    answer.width=plan.compositor.width;answer.height=plan.compositor.height;
    answer.gpu_pitch=plan.compositor.pitch;answer.gpu_bytes=plan.compositor.bytes;
    answer.output_count=plan.count;answer.origin_x=plan.layout.origin_x;answer.origin_y=plan.layout.origin_y;
    *out=answer;return 0;
}
#endif
