/* Included by the native KAPI client after its boot inventory implementation.
 * Event callbacks never query NVKMS or acquire this lock. A separate worker
 * publishes complete inventory generations; scanout ownership is untouched.
 * Live commits must use the same generation, then revalidate before commit. */
#ifndef KESTREL_NVKMS_DISPLAY_WATCH_H
#define KESTREL_NVKMS_DISPLAY_WATCH_H
typedef struct {
    NvU32 generation,count;
    kdisplay_output_t output[KDISPLAY_MAX_OUTPUTS];
    kdisplay_mode_t mode[KDISPLAY_MAX_OUTPUTS][KDISPLAY_MAX_MODES];
    struct NvKmsKapiDisplayMode native_mode[KDISPLAY_MAX_OUTPUTS][KDISPLAY_MAX_MODES];
    struct NvKmsKapiStaticDisplayInfo static_info[KDISPLAY_MAX_OUTPUTS];
} nvkms_watch_snapshot_t;
static nvkms_watch_snapshot_t watch_live,watch_next;
static volatile NvU32 watch_events=1,watch_dirty,watch_held;
static NvBool watch_ready;
static NvU32 watch_boot_generation=1;

/* Called once when final boot geometry is recorded, before pattern/engine
 * tests. Framebuffer publication must not reset this epoch: cable changes
 * during tests, framebuffer setup and the later GPU gate remain pending. */
static void nvkms_watch_boot_boundary(void){
    watch_boot_generation=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE);
    __atomic_exchange_n(&watch_dirty,0,__ATOMIC_ACQ_REL);
}

static void watch_lock(void){
    while(__atomic_exchange_n(&watch_held,1,__ATOMIC_ACQUIRE))sched_yield();
}
static void watch_unlock(void){__atomic_store_n(&watch_held,0,__ATOMIC_RELEASE);}
static void nvkms_watch_event(NvU32 display){
    /* NVKMS display ids are masks. A zero/unspecified id invalidates all paths. */
    __atomic_fetch_or(&watch_dirty,display?display:~0u,__ATOMIC_RELEASE);
    NvU32 old=__atomic_load_n(&watch_events,__ATOMIC_RELAXED),next;
    do { next=old+1;if(!next)next=1; }
    while(!__atomic_compare_exchange_n(&watch_events,&old,next,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED));
}
static int nvkms_watch_output(NvU32 index,kdisplay_output_t *out){
    if(!__atomic_load_n(&watch_ready,__ATOMIC_ACQUIRE))return -1;
    if(!out)return 0;
    watch_lock();
    if(index>=watch_live.count){watch_unlock();return 0;}
    *out=watch_live.output[index];out->reserved=watch_live.generation;
    watch_unlock();
    if(out->reserved!=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE))out->flags|=KDISPLAY_STALE;
    return 1;
}
static int nvkms_watch_mode(NvU32 index,NvU32 mode,kdisplay_mode_t *out){
    if(!__atomic_load_n(&watch_ready,__ATOMIC_ACQUIRE))return -1;
    if(!out)return 0;
    NvU32 generation=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE);
    watch_lock();
    if(watch_live.generation!=generation || index>=watch_live.count ||
       mode>=watch_live.output[index].mode_count){watch_unlock();return 0;}
    kdisplay_mode_t copy=watch_live.mode[index][mode];watch_unlock();
    if(generation!=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE))return 0;
    *out=copy;return 1;
}
static int watch_find(const nvkms_watch_snapshot_t *s,NvU32 handle){
    for(NvU32 i=0;i<s->count;i++)if(s->output[i].output_id==handle)return (int)i;
    return -1;
}
static int watch_boot_slot(NvU32 handle){
    for(NvU32 i=0;i<active_count;i++)if(active[i].handle==handle)return (int)i;
    return -1;
}
static NvBool watch_detect(NvU32 handle,NvU32 slot){
    struct NvKmsKapiStaticDisplayInfo stat;
    struct NvKmsKapiConnectorInfo connector;
    /* Worker-private scratch, not kernel-stack storage; the full EDID parser
     * record and KAPI EDID array together exceed the small kernel stack budget.
     * This routine has exactly one caller: the serialized hotplug worker. */
    static struct NvKmsKapiDynamicDisplayParams dyn;
    static edid_info_t identity;
    memset(&stat,0,sizeof stat);memset(&connector,0,sizeof connector);memset(&dyn,0,sizeof dyn);
    if(!kapi.getStaticDisplayInfo(test_device,handle,&stat) ||
       !kapi.getConnectorInfo(test_device,stat.connectorHandle,&connector))return NV_FALSE;
    if(connector.signalFormat==NVKMS_CONNECTOR_SIGNAL_FORMAT_DP &&
       !connector.dynamicDpyIdListValid)return NV_FALSE;
    dyn.handle=handle;
    if(!kapi.getDynamicDisplayInfo(test_device,&dyn))return NV_FALSE;
    if(dyn.edid.bufferSize>sizeof dyn.edid.buffer)return NV_FALSE;
    kdisplay_output_t *o=&watch_next.output[slot];
    watch_next.static_info[slot]=stat;
    memset(o,0,sizeof *o);
    o->version=KDISPLAY_INFO_VERSION;o->output_id=handle;o->connector_id=stat.connectorHandle;
    o->reserved=watch_next.generation;
    strlcpy(o->connector,connector.signalFormat==NVKMS_CONNECTOR_SIGNAL_FORMAT_DP?"DisplayPort":
        connector.type==NVKMS_CONNECTOR_TYPE_HDMI?"HDMI":"Other",sizeof o->connector);
    if(!dyn.connected)return NV_TRUE;
    o->flags=KDISPLAY_CONNECTED|KDISPLAY_DETECTED_ONLY;
    memset(&identity,0,sizeof identity);
    if(dyn.edid.bufferSize>=128 && edid_parse(dyn.edid.buffer,dyn.edid.bufferSize,&identity)){
        strlcpy(o->manufacturer,identity.manufacturer,sizeof o->manufacturer);
        strlcpy(o->model,identity.model,sizeof o->model);
    }
    /* A changed/reconnected sink is NOT asserted to have the boot scanout.
     * No override, modeset, CE work or buffer release occurs in this worker.
     * User-provisioned zero-identity recovery belongs to the Apply transaction. */
    for(NvU32 i=0;i<KDISPLAY_MAX_MODES;i++){
        struct NvKmsKapiDisplayMode mode;
        NvBool valid=NV_FALSE,preferred=NV_FALSE;
        memset(&mode,0,sizeof mode);
        int more=kapi.getDisplayMode(test_device,handle,i,&mode,&valid,&preferred);
        if(more<0)return NV_FALSE;
        if(!more)break; /* NVKMS's end marker has no mode payload. */
        if(valid && mode.timings.hVisible && mode.timings.vVisible && mode.timings.refreshRate &&
           kapi.validateDisplayMode(test_device,handle,&mode)){
            kdisplay_mode_t m={mode.timings.hVisible,mode.timings.vVisible,mode.timings.refreshRate,
                (preferred?KDISPLAY_MODE_PREFERRED:0)|(mode.timings.flags.interlaced?KDISPLAY_MODE_INTERLACED:0)};
            NvBool duplicate=NV_FALSE;
            for(NvU32 j=0;j<o->mode_count;j++){
                kdisplay_mode_t *old=&watch_next.mode[slot][j];
                if(old->width==m.width && old->height==m.height && old->refresh_millihz==m.refresh_millihz &&
                   !((old->flags^m.flags)&KDISPLAY_MODE_INTERLACED)){
                    old->flags|=m.flags;duplicate=NV_TRUE;break;
                }
            }
            if(!duplicate){
                watch_next.mode[slot][o->mode_count]=m;
                watch_next.native_mode[slot][o->mode_count++]=mode;
            }
        }
        if(i+1==KDISPLAY_MAX_MODES)o->flags|=KDISPLAY_MODES_TRUNCATED;
    }
    return NV_TRUE;
}
/* One bounded scan attempt, separate from delay/backoff. Events racing a scan
 * invalidate its complete result; never publish half-old/half-new lists. */
static NvBool watch_rescan(void){
    NvU32 generation=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE);
    NvU32 dirty=__atomic_exchange_n(&watch_dirty,0,__ATOMIC_ACQ_REL);
    if(!dirty && generation==watch_live.generation)return NV_TRUE;
    if(!dirty)dirty=~0u;
    NvKmsKapiDisplay handles[TEST_MAX_ENUM_DISPLAYS];NvU32 count=TEST_MAX_ENUM_DISPLAYS;
    memset(handles,0,sizeof handles);memset(&watch_next,0,sizeof watch_next);
    watch_next.generation=generation;
    if(!kapi.getDisplays(test_device,&count,handles) || count>TEST_MAX_ENUM_DISPLAYS)goto failed;
    for(NvU32 i=0;i<count;i++){
        if(!handles[i])goto failed;
        for(NvU32 j=0;j<i;j++)if(handles[j]==handles[i])goto failed;
        int old=watch_find(&watch_live,handles[i]);
        /* Do not probe unchanged active sinks. In particular, an unsolicited
         * no-override detect must not replace the Acer's provisioned mode pool. */
        if(old<0 && !(handles[i]&dirty))continue;
        NvU32 slot=watch_next.count;
        if(slot>=KDISPLAY_MAX_OUTPUTS)goto failed;
        if(old>=0 && !(handles[i]&dirty)){
            watch_next.output[slot]=watch_live.output[old];
            memcpy(watch_next.mode[slot],watch_live.mode[old],sizeof watch_next.mode[slot]);
            memcpy(watch_next.native_mode[slot],watch_live.native_mode[old],sizeof watch_next.native_mode[slot]);
            watch_next.static_info[slot]=watch_live.static_info[old];
            watch_next.output[slot].reserved=generation;
        }else if(!watch_detect(handles[i],slot))goto failed;
        if((watch_next.output[slot].flags&KDISPLAY_CONNECTED) || watch_boot_slot(handles[i])>=0)
            watch_next.count++;
    }
    /* Keep disconnected boot outputs inspectable, even if NVKMS removed the
     * dynamic id from enumeration; this also represents an all-unplugged desk. */
    for(NvU32 i=0;i<active_count;i++)if(watch_find(&watch_next,active[i].handle)<0){
        if(watch_next.count==KDISPLAY_MAX_OUTPUTS)goto failed;
        kdisplay_output_t *o=&watch_next.output[watch_next.count++];
        memset(o,0,sizeof *o);o->version=KDISPLAY_INFO_VERSION;
        o->output_id=active[i].handle;o->connector_id=active[i].connector_handle;o->reserved=generation;
    }
    if(generation!=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE))goto failed;
    watch_lock();
    /* A live timing commit can publish a new epoch while this worker waits
     * for the snapshot lock. Never overwrite that commit with an older scan. */
    if(generation!=__atomic_load_n(&watch_events,__ATOMIC_ACQUIRE)){watch_unlock();goto failed;}
    memcpy(&watch_live,&watch_next,sizeof watch_live);watch_unlock();
    kinfo("nvkms-live","monitor inventory generation %u published: %u output(s); scanout configuration unchanged",
          generation,watch_next.count);
    return NV_TRUE;
failed:
    __atomic_fetch_or(&watch_dirty,dirty,__ATOMIC_RELEASE);
    return NV_FALSE;
}
static void nvkms_watch_thread(void *unused){
    (void)unused;
    NvU64 retry=0,last_error=0;
    for(;;){
        sched_sleep_ms(100);
        NvU64 now=timer_now_us();
        if(now<retry)continue;
        if(!watch_rescan()){
            retry=timer_now_us()+1000000ull;
            if(now-last_error>=5000000ull){
                kwarn("nvkms-live","monitor scan incomplete or changed while reading; cached modes remain stale");last_error=now;
            }
        }
    }
}
static void nvkms_watch_start(void){
    if(__atomic_load_n(&watch_ready,__ATOMIC_ACQUIRE))return;
    memset(&watch_live,0,sizeof watch_live);
    watch_live.generation=watch_boot_generation;
    for(NvU32 i=0;i<active_count && i<KDISPLAY_MAX_OUTPUTS;i++){
        if(!nvkms_kapi_display_output(i,&watch_live.output[i]))return;
        watch_live.static_info[i]=active[i].static_info;
        for(NvU32 j=0;j<watch_live.output[i].mode_count;j++){
            if(!nvkms_kapi_display_mode(i,j,&watch_live.mode[i][j]) ||
               !resolve_display_mode(i,active[i].handle,active[i].connector_handle,j,
                                     &watch_live.native_mode[i][j]))return;
        }
        watch_live.output[i].reserved=watch_live.generation;watch_live.count++;
    }
    if(kthread_create("nvkms-hotplug",nvkms_watch_thread,NULL)<0){
        kwarn("nvkms-live","could not start monitor worker; retaining boot-only inventory");return;
    }
    __atomic_store_n(&watch_ready,NV_TRUE,__ATOMIC_RELEASE);
}
#endif
