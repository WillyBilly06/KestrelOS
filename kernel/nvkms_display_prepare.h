/* Owner-bound replacement scanouts. Every entry point and reaper holds the
 * render guard. These resources have NEVER been submitted for scanout or bound
 * into the copy/compute VMM; only NVIDIA's commit=false validation sees them.
 * A future commit must transfer ownership, not run this unsubmitted cleanup. */
#ifndef KESTREL_NVKMS_DISPLAY_PREPARE_H
#define KESTREL_NVKMS_DISPLAY_PREPARE_H
#define NVKMS_PREPARE_LEASE_US 30000000ull
typedef struct {
    NvBool ready;
    u64 owner,token,expires,fb_phys,fb_bytes;
    NvU32 fb_pitch,previous_heads;
    void *fb_pixels;
    nvkms_display_plan_t plan;
    struct NvKmsKapiRequestedModeSetConfig config;
    struct NvKmsKapiModeSetReplyConfig reply;
} nvkms_prepared_t;
static nvkms_prepared_t prepared;
static NvU64 prepared_token;
static NvBool prepared_reaper_started;

static void prepared_discard(void){
    for(NvU32 i=0;i<prepared.plan.count;i++)
        for(NvU32 slot=0;slot<2;slot++)free_display_slot(&prepared.plan.displays[i],slot);
    if(prepared.fb_pixels)dma_free_pages(prepared.fb_pixels,(size_t)(prepared.fb_bytes/PAGE_SIZE));
    memset(&prepared,0,sizeof prepared);
}
static void prepared_reap(void){
    if(prepared.ready && (timer_now_us()>=prepared.expires ||
       !proc_gpu_owner_alive(prepared.owner) ||
       __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=prepared.plan.generation))prepared_discard();
}
static void prepared_reaper(void *unused){
    (void)unused;
    for(;;){
        sched_sleep_ms(100);
        if(nv_render_try_begin()){prepared_reap();nv_render_end();}
    }
}

/* Head masks only establish possible wiring. Ask the complete NVIDIA atomic
 * validator at every leaf, because SOR/IMP/bandwidth/DSC can reject one valid
 * mask assignment and accept another. Never touch active[] or boot requested. */
static int prepared_assign(NvU32 at,NvU32 used){
    if(__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=prepared.plan.generation)return -E_BUSY;
    if(at==prepared.plan.count){
        if(!build_display_config(prepared.plan.displays,prepared.plan.count,&prepared.plan.layout,
                                 prepared.previous_heads,&prepared.config))return -E_INVAL;
        memset(&prepared.reply,0,sizeof prepared.reply);
        NvBool ok=kapi.applyModeSetConfig(test_device,&prepared.config,&prepared.reply,NV_FALSE);
        if(__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=prepared.plan.generation)return -E_BUSY;
        if(prepared.reply.flipResult==NV_KMS_FLIP_RESULT_IN_PROGRESS)return -E_BUSY;
        return ok && prepared.reply.flipResult==NV_KMS_FLIP_RESULT_SUCCESS?0:-E_INVAL;
    }
    test_display_t *d=&prepared.plan.displays[at];
    NvU32 allowed=d->static_info.headMask & ((1u<<resources.numHeads)-1u) & ~used;
    int old=watch_boot_slot(d->handle);
    NvU32 preferred=old>=0?active[old].head:resources.numHeads;
    for(NvU32 pass=0;pass<=resources.numHeads;pass++){
        NvU32 head=pass?pass-1:preferred;
        if(head>=resources.numHeads || (pass && head==preferred) ||
           !(allowed&(1u<<head)) || !resources.numLayers[head])continue;
        d->head=head;
        int status=prepared_assign(at+1,used|(1u<<head));
        if(status!=-E_INVAL)return status;
    }
    return -E_INVAL;
}

int nvkms_kapi_prepare_configuration(u64 owner,const kdisplay_configuration_t *request,
                                    kdisplay_prepared_configuration_t *out){
    if(!owner || !proc_gpu_owner_alive(owner))return -E_PERM;
    if(!out || !request)return -E_INVAL;
    prepared_reap();
    /* The syscall has verified that owner currently controls the framebuffer.
     * A previous owner may still be alive after FB_RELEASE; its unsubmitted
     * lease must not block the new owner for the remainder of 30 seconds. */
    if(prepared.ready && prepared.owner!=owner)prepared_discard();
    if(prepared.ready || prepared_token==~0ull)return -E_BUSY;
    int status=plan_configuration(request,&prepared.plan);
    if(status)return status;
    if(!prepared_reaper_started){
        if(kthread_create("nvkms-config",prepared_reaper,NULL)<0){status=-E_NOMEM;goto failed;}
        prepared_reaper_started=NV_TRUE;
    }
    if(!resources.caps.pitchAlignment){status=-E_INVAL;goto failed;}
    for(NvU32 i=0;i<active_count;i++){
        if(active[i].head>=resources.numHeads){status=-E_INVAL;goto failed;}
        prepared.previous_heads|=1u<<active[i].head;
    }
    for(NvU32 i=0;i<prepared.plan.count;i++){
        test_display_t *d=&prepared.plan.displays[i];
        NvU64 pitch=align_up_u64((NvU64)d->mode.timings.hVisible*4u,resources.caps.pitchAlignment);
        NvU64 bytes=align_up_u64(pitch*d->mode.timings.vVisible,PAGE_SIZE);
        if(!pitch || pitch>0xffffffffu || !bytes || bytes>0xffffffffu){status=-E_INVAL;goto failed;}
        d->pitch=(NvU32)pitch;d->bytes=bytes;
        for(NvU32 slot=0;slot<2;slot++){
            if(!allocate_display_slot(d,slot)){status=-E_NOMEM;goto failed;}
            if(__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=prepared.plan.generation){status=-E_BUSY;goto failed;}
        }
    }
    /* New system shadow stays private; no console publication, user mapping,
     * mouse-bound change or replacement of runtime_fb_pixels happens here. */
    prepared.fb_pitch=(NvU32)align_up_u64((NvU64)prepared.plan.layout.fb_width*4u,64u);
    prepared.fb_bytes=align_up_u64((NvU64)prepared.fb_pitch*prepared.plan.layout.fb_height,PAGE_SIZE);
    prepared.fb_pixels=dma_alloc_pages((size_t)(prepared.fb_bytes/PAGE_SIZE),&prepared.fb_phys);
    if(!prepared.fb_pixels){status=-E_NOMEM;goto failed;}
    status=prepared_assign(0,0);
    if(status)goto failed;
    if(!proc_gpu_owner_alive(owner) ||
       __atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)!=prepared.plan.generation){status=-E_BUSY;goto failed;}
    NvU64 now=timer_now_us();
    if(now>~0ull-NVKMS_PREPARE_LEASE_US){status=-E_BUSY;goto failed;}
    prepared.owner=owner;prepared.token=++prepared_token;prepared.expires=now+NVKMS_PREPARE_LEASE_US;
    prepared.ready=NV_TRUE;
    kdisplay_prepared_configuration_t answer;
    memset(&answer,0,sizeof answer);
    answer.configuration.version=KDISPLAY_CONFIGURATION_VERSION;
    answer.configuration.generation=prepared.plan.generation;
    answer.configuration.width=prepared.plan.compositor.width;
    answer.configuration.height=prepared.plan.compositor.height;
    answer.configuration.gpu_pitch=prepared.plan.compositor.pitch;
    answer.configuration.gpu_bytes=prepared.plan.compositor.bytes;
    answer.configuration.output_count=prepared.plan.count;
    answer.configuration.origin_x=prepared.plan.layout.origin_x;
    answer.configuration.origin_y=prepared.plan.layout.origin_y;
    answer.token=prepared.token;answer.expires_us=prepared.expires;
    *out=answer;return 0;
failed:
    prepared_discard();return status;
}

int nvkms_kapi_cancel_configuration(u64 owner,u64 token){
    if(!owner || !token)return -E_INVAL;
    prepared_reap();
    if(!prepared.ready || prepared.token!=token)return -E_NOENT;
    if(prepared.owner!=owner)return -E_PERM;
    prepared_discard();return 0;
}
#endif
