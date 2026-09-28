/* Native NVKMS KAPI display client for KestrelOS.
 *
 * This is intentionally the same public interface used by nvidia-drm.  Link
 * training, bandwidth validation, DSC, SOR routing and the Blackwell display
 * programming sequence remain inside NVIDIA's matching 595.99.02 NVKMS/RM
 * pair; Kestrel only supplies policy and scanout pixels. */
#include "kernel.h"
#include "gpu.h"
#include "klog.h"
#include "time.h"
#include "mm.h"
#include "proc.h"
#include "edid.h"
#include "vfs.h"
#include "nvkms_port.h"
#include "nvkms_kapi_client.h"
#include "nv_surface.h"
#include "../include/kestrel/display_layout.h"
#undef ARRAY_LEN
#include "nvkms-kapi.h"

extern NvBool nvKmsKapiGetFunctionsTableInternal(
    struct NvKmsKapiFunctionsTable *funcsTable);
extern NvBool nvrm_is_ready(void);
extern NvU32 nvrm_gpu_id(void);
extern NvU32 nvrm_transfer_rm_memory(NvU32 hClient, NvU32 hMemory,
                                     NvU64 offset, void *buffer,
                                     NvU64 size, NvBool read);
extern NvU32 nvrm_host_control_object(NvU32 client, NvU32 object, NvU32 cmd,
                                      void *params, NvU32 params_size);

/* The public KAPI intentionally leaves these objects opaque.  These prefixes
 * are from the exact 595.99.02 nvkms-kapi-internal.h linked into this image;
 * they expose only the two RM handles needed by the official MemUtils copy
 * path.  The offset assertion makes an ABI drift a build failure. */
typedef struct {
    NvU32 gpuId;
    void *pSema;
    NvU32 hRmClient;
    NvU32 hRmDevice;
    NvU32 hRmSubDevice;
} kapi_device_rm_prefix_t;
typedef struct { NvU32 hRmHandle; } kapi_memory_rm_prefix_t;
_Static_assert(__builtin_offsetof(kapi_device_rm_prefix_t, hRmClient) == 16,
               "595.99.02 NvKmsKapiDevice RM-handle ABI changed");
_Static_assert(__builtin_offsetof(kapi_device_rm_prefix_t, hRmSubDevice) == 24,
               "595.99.02 NvKmsKapiDevice subdevice-handle ABI changed");

#define NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR 0x00410103u
#define NV0041_APERTURE_VIDMEM 0u
typedef struct {
    NvU64 mem_offset;
    NvU32 mem_format;
    NvU32 compr_offset;
    NvU32 compr_format;
    NvU32 mem_aperture;
    NvU32 gpu_cache_attr;
    NvU32 gpu_p2p_cache_attr;
    NvU32 mmu_context;
    NvU64 contig_segment_size;
} kapi_phys_attr_params_t;
_Static_assert(sizeof(kapi_phys_attr_params_t) == 48,
               "NV0041 surface physical-attribute ABI changed");

#define TEST_MAX_ENUM_DISPLAYS 32

typedef struct {
    NvKmsKapiDisplay handle;
    struct NvKmsKapiStaticDisplayInfo static_info;
    NvKmsKapiConnector connector_handle;
    NvBool connector_is_dp;
    NvBool edid_pending;
    NvBool mode_changed_after_wake;
    char manufacturer[4];
    char model[16];
    struct NvKmsKapiDisplayMode mode;
    NvU32 head;
    NvU32 pitch;
    NvU64 bytes;
    struct NvKmsKapiMemory *memory[2];
    struct NvKmsKapiSurface *surface[2];
    NvU32 front;
    NvU32 *pixels; /* system-memory staging image; never a BAR1 pointer */
} test_display_t;

/* These objects must outlive the modeset: freeing them would tear down the
 * active scanout before the requested observation/reboot window finishes. */
static struct NvKmsKapiFunctionsTable kapi;
static struct NvKmsKapiDevice *test_device;
static nv_card_t *test_card;
static struct NvKmsKapiDeviceResourcesInfo resources;
static struct NvKmsKapiRequestedModeSetConfig requested;
static struct NvKmsKapiModeSetReplyConfig reply;
static test_display_t active[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
static edid_info_t parsed_edid;
static NvU32 enum_gpu_count;
static NvBool enum_gpu_matched;
static NvU32 wanted_gpu;
static volatile NvU32 flip_sequence[NVKMS_KAPI_MAX_HEADS];
static NvU32 active_count;
static dl_layout_t runtime_layout;
static dl_mode_t runtime_mode;
static NvBool runtime_ready;
static u64 runtime_fb_phys;
static NvU32 *runtime_fb_pixels;
static NvU32 runtime_fb_pitch;
static NvU32 runtime_primary_index;
static void nvkms_watch_event(NvU32 display);
static void nvkms_watch_boot_boundary(void);
static void nvkms_watch_start(void);

static struct {
    NvU32 handle, count;
    NvBool truncated;
    kdisplay_mode_t modes[KDISPLAY_MAX_MODES];
    /* Preserve the validated NVIDIA timing record, including porches, sync
     * polarity, clock and name. A width/height/Hz label cannot reconstruct it. */
    struct NvKmsKapiDisplayMode native_modes[KDISPLAY_MAX_MODES];
} display_modes[KESTREL_NVKMS_MAX_TEST_DISPLAYS];

static void cache_display_mode(NvU32 slot,
                               const struct NvKmsKapiDisplayMode *mode,
                               NvBool preferred)
{
    if (slot >= KESTREL_NVKMS_MAX_TEST_DISPLAYS) return;
    kdisplay_mode_t m = {
        .width = mode->timings.hVisible, .height = mode->timings.vVisible,
        .refresh_millihz = mode->timings.refreshRate,
        .flags = (preferred ? KDISPLAY_MODE_PREFERRED : 0) |
                 (mode->timings.flags.interlaced ? KDISPLAY_MODE_INTERLACED : 0)
    };
    if (!m.width || !m.height || !m.refresh_millihz) return;
    for (NvU32 i = 0; i < display_modes[slot].count; i++) {
        kdisplay_mode_t *old = &display_modes[slot].modes[i];
        if (old->width == m.width && old->height == m.height &&
            old->refresh_millihz == m.refresh_millihz &&
            !((old->flags ^ m.flags) & KDISPLAY_MODE_INTERLACED)) {
            old->flags |= m.flags;
            return;
        }
    }
    if (display_modes[slot].count == KDISPLAY_MAX_MODES) {
        display_modes[slot].truncated = NV_TRUE;
        return;
    }
    NvU32 at = display_modes[slot].count++;
    display_modes[slot].modes[at] = m;
    display_modes[slot].native_modes[at] = *mode;
}

/* Resolve only an identity-matched cached mode. The eventual transaction
 * must revalidate it against current connector state before committing. */
static NvBool resolve_display_mode(NvU32 slot, NvU32 display,
                                   NvU32 connector, NvU32 index,
                                   struct NvKmsKapiDisplayMode *out)
{
    if (!out || slot >= active_count || slot >= KESTREL_NVKMS_MAX_TEST_DISPLAYS ||
        active[slot].handle != display || active[slot].connector_handle != connector ||
        display_modes[slot].handle != display || index >= display_modes[slot].count)
        return NV_FALSE;
    *out = display_modes[slot].native_modes[index];
    return NV_TRUE;
}

/* Choose the requested desktop primary without reordering `active`.  Hardware
 * tests and mode ownership retain NVKMS's already-proven connector order; only
 * the final logical desktop layout places this output at framebuffer x=0. */
static void select_preferred_primary(NvU32 count)
{
    runtime_primary_index = 0;
    NvBool selected = NV_FALSE;
    for (NvU32 i = 0; i < count; i++) {
        if (strcmp(active[i].manufacturer, "AOC") == 0) {
            runtime_primary_index = i;
            selected = NV_TRUE;
            break;
        }
    }
    if (count) {
        test_display_t *primary = &active[runtime_primary_index];
        kinfo("nvkms-runtime", "primary display is %s %s (display %#x%s)",
              primary->manufacturer[0] ? primary->manufacturer : "unidentified",
              primary->model[0] ? primary->model : "monitor",
              primary->handle,
              selected ? "; selected by EDID policy" : "; enumeration fallback");
    }
}

static void nvkms_test_event_cb(const struct NvKmsKapiEvent *event)
{
    if(event && event->device==test_device){
        if(event->type==NVKMS_EVENT_TYPE_DPY_CHANGED)
            nvkms_watch_event(event->u.displayChanged.display);
        else if(event->type==NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED)
            nvkms_watch_event(event->u.dynamicDisplayConnected.display);
    }
    /* This is the completion barrier used by nvidia-drm's blocking atomic
     * path, not merely an event drain.  A successful KmsFlip means that NVKMS
     * accepted the transaction; only FLIP_OCCURRED says the hardware retired
     * it at vblank and the old front buffer may be reused. */
    if (event && event->device == test_device && event->type == NVKMS_EVENT_TYPE_FLIP_OCCURRED &&
        event->u.flipOccurred.layer == NVKMS_KAPI_LAYER_PRIMARY_IDX &&
        event->u.flipOccurred.head < NVKMS_KAPI_MAX_HEADS)
        __atomic_add_fetch(&flip_sequence[event->u.flipOccurred.head], 1,
                           __ATOMIC_RELEASE);
}

static void snapshot_flip_sequences(NvU32 before[NVKMS_KAPI_MAX_HEADS])
{
    for (NvU32 head = 0; head < NVKMS_KAPI_MAX_HEADS; head++)
        before[head] = __atomic_load_n(&flip_sequence[head], __ATOMIC_ACQUIRE);
}

static NvBool wait_for_flip_mask(NvU32 heads_mask,
                                 const NvU32 before[NVKMS_KAPI_MAX_HEADS],
                                 const char *operation)
{
    /* Linux uses a three-second wait_event_timeout for blocking atomic
     * commits.  Sleep rather than spin so the nvkms-event worker can drain the
     * queue and invoke nvkms_test_event_cb. */
    NvU64 deadline = timer_now_us() + 3000000ull;
    for (;;) {
        NvU32 complete = 0;
        for (NvU32 head = 0; head < NVKMS_KAPI_MAX_HEADS; head++) {
            if (!(heads_mask & (1u << head))) continue;
            if (__atomic_load_n(&flip_sequence[head], __ATOMIC_ACQUIRE) !=
                before[head])
                complete |= 1u << head;
        }
        if ((complete & heads_mask) == heads_mask)
            return NV_TRUE;
        if (timer_now_us() >= deadline) {
            kwarn("nvkms-test", "%s FLIP_OCCURRED timeout: wanted heads %#x completed %#x",
                  operation, heads_mask, complete);
            return NV_FALSE;
        }
        sched_sleep_ms(1);
    }
}

static void enum_gpu_cb(const struct NvKmsKapiGpuInfo *info) {
    enum_gpu_count++;
    if (info && info->gpuInfo.gpu_id == wanted_gpu &&
        info->migDevice == NO_MIG_DEVICE)
        enum_gpu_matched = NV_TRUE;
}

static NvU32 align_up_u32(NvU32 value, NvU32 alignment) {
    if (alignment <= 1) return value;
    return (NvU32)(((NvU64)value + alignment - 1) / alignment * alignment);
}
static NvU64 align_up_u64(NvU64 value, NvU64 alignment) {
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

/* Maximum capability policy requested by the user: maximize addressable
 * pixels first, then refresh rate among modes at that maximum resolution.
 * Interlaced modes never outrank a progressive mode with the same geometry. */
static NvBool select_max_mode(NvKmsKapiDisplay display,
                              struct NvKmsKapiDisplayMode *best) {
    NvBool have = NV_FALSE;
    NvU32 slot = KESTREL_NVKMS_MAX_TEST_DISPLAYS;
    for (NvU32 i = 0; i < KESTREL_NVKMS_MAX_TEST_DISPLAYS; i++)
        if (active[i].handle == display) { slot = i; break; }
    if (slot < KESTREL_NVKMS_MAX_TEST_DISPLAYS) {
        memset(&display_modes[slot], 0, sizeof display_modes[slot]);
        display_modes[slot].handle = display;
    }
    for (NvU32 i = 0; i < 1024; i++) {
        struct NvKmsKapiDisplayMode candidate;
        NvBool valid = NV_FALSE, preferred = NV_FALSE;
        memset(&candidate, 0, sizeof(candidate));
        int more = kapi.getDisplayMode(test_device, display, i, &candidate,
                                       &valid, &preferred);
        (void)preferred;
        if (more < 0) return NV_FALSE;
        if (valid && kapi.validateDisplayMode(test_device, display, &candidate)) {
            cache_display_mode(slot, &candidate, preferred);
            NvU64 area = (NvU64)candidate.timings.hVisible * candidate.timings.vVisible;
            NvU64 best_area = have ?
                (NvU64)best->timings.hVisible * best->timings.vVisible : 0;
            NvBool better = !have || area > best_area;
            if (area == best_area && have) {
                if (best->timings.flags.interlaced &&
                    !candidate.timings.flags.interlaced)
                    better = NV_TRUE;
                else if (best->timings.flags.interlaced ==
                         candidate.timings.flags.interlaced &&
                         candidate.timings.refreshRate > best->timings.refreshRate)
                    better = NV_TRUE;
            }
            if (better) {
                *best = candidate;
                have = NV_TRUE;
            }
        }
        if (more == 0) break;
    }
    return have;
}

/* A powered-down or input-switching DP sink can report connected before its
 * AUX EDID transaction has completed.  Linux revisits such connectors through
 * its hotplug helper.  This boot client has no DRM hotplug worker, so perform
 * that revisit here for a real (monotonic-clocked) 15-second wake window.
 *
 * Do not express this as an attempt count around sched_sleep_ms(): on the
 * single-CPU bring-up scheduler an unrelated wake can legally return that
 * sleep early.  That made the old advertised five-second window expire in
 * about 180 ms on the target, before the sleeping Acer sink supplied EDID.
 * NVIDIA's own GetConnectorInfo() uses timer-based elapsed time and allows ten
 * seconds for DP detection; use the same clock, then leave another bounded
 * interval for the actual EDID AUX transaction.  Periodically repeat the full
 * connector query too, matching Linux's connector hotplug lifecycle rather
 * than merely rereading cached display state.
 *
 * Each dynamic request starts with a completely zeroed IN structure, exactly
 * like __nv_drm_detect_encoder() in nvidia-drm-connector.c. */
#define EDID_INITIAL_TIMEOUT_USEC  1000000ull
#define EDID_WAKE_TIMEOUT_USEC    15000000ull
#define EDID_WAKE_POLL_MSEC         250u
#define DP_CONNECT_TIMEOUT_USEC   10000000ull

/* Load an explicitly provisioned, connector-scoped EDID only after the live
 * monitor has returned no valid identity.  This is the same mechanism as
 * Linux's drm.edid_firmware=DP-x:file and NVIDIA KAPI's documented
 * overrideEdid input.  A monitor that supplies a checksum-valid, parseable
 * live EDID always wins, so replacing the Acer with a normal sink remains
 * automatic.  A complete-sized but corrupt Acer EDID must not silently become
 * NVKMS's 640x480 fallback.
 *
 * A zero-EDID sink is, by definition, unable to identify itself.  Therefore
 * overrides are opt-in, explicitly mapped in nvkms-bindings.txt. The saved
 * filename identifies an asset, NOT a permanently attached monitor. A cable
 * move changes the binding; a zero-identity replacement cannot be inferred.
 * No vendor/model guess or broadly-applied timing is permitted. */
#include "nvkms_edid_binding.h"
static NvBool load_provisioned_edid(NvKmsKapiDisplay handle,
                                    NvU8 *buffer, NvU16 *buffer_size)
{
    char bindings[513];
    s64 binding_bytes=vfs_read_file("/lib/firmware/edid/nvkms-bindings.txt",
                                     bindings,sizeof bindings);
    unsigned profile=0;
    if(binding_bytes<=0 || binding_bytes>512 ||
       !nvkms_edid_binding(bindings,(unsigned)binding_bytes,handle,&profile))
        return NV_FALSE;
    char path[64];
    snprintf(path, sizeof(path),
             "/lib/firmware/edid/nvkms-%08x.bin", profile);
    s64 n = vfs_read_file(path, buffer, NVKMS_KAPI_EDID_BUFFER_SIZE);
    if (n <= 0) return NV_FALSE;
    if (n < 128 || n > NVKMS_KAPI_EDID_BUFFER_SIZE || (n % 128) != 0) {
        kerr("nvkms-test", "rejecting malformed EDID override %s (%lld bytes)",
             path, (long long)n);
        return NV_FALSE;
    }
    static const NvU8 magic[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(buffer, magic, sizeof(magic)) != 0 ||
        (NvU32)(buffer[126] + 1u) != (NvU32)n / 128u) {
        kerr("nvkms-test", "rejecting EDID override %s with invalid header/block count",
             path);
        return NV_FALSE;
    }
    for (s64 block = 0; block < n / 128; block++) {
        NvU8 sum = 0;
        for (NvU32 j = 0; j < 128; j++)
            sum = (NvU8)(sum + buffer[block * 128 + j]);
        if (sum != 0) {
            kerr("nvkms-test", "rejecting EDID override %s: block %lld checksum is nonzero",
                 path, (long long)block);
            return NV_FALSE;
        }
    }
    edid_info_t check;
    if (!edid_parse(buffer, (NvU32)n, &check)) {
        kerr("nvkms-test", "rejecting EDID override %s: local parser failed",
             path);
        return NV_FALSE;
    }
    *buffer_size = (NvU16)n;
    kinfo("nvkms-test", "validated explicitly bound EDID override %s for display %#x: %s %s, %lld bytes",
          path, handle, check.manufacturer,
          check.model[0] ? check.model : "(unnamed)", (long long)n);
    return NV_TRUE;
}

static NvBool apply_provisioned_edid(NvKmsKapiDisplay handle,
                                     struct NvKmsKapiDynamicDisplayParams *dyn,
                                     NvBool *identity_ok)
{
    if(!dyn->connected)return NV_FALSE;
    NvU8 identity[8];
    NvBool known_identity=nvkms_edid_has_identity(dyn->edid.buffer,dyn->edid.bufferSize);
    memcpy(identity,dyn->edid.buffer+8,sizeof identity);
    /* An absent/rejected override must not destroy the live AUX result that
     * callers still need for safe fallback and evidence. Publish only after
     * the complete override request succeeds and its identity parses. */
    struct NvKmsKapiDynamicDisplayParams *candidate = kzalloc(sizeof(*candidate));
    if (!candidate) return NV_FALSE;
    NvBool applied = NV_FALSE;
    candidate->handle = handle;
    if (!load_provisioned_edid(handle, candidate->edid.buffer,
                               &candidate->edid.bufferSize))
        goto done;
    if(known_identity && memcmp(identity,candidate->edid.buffer+8,sizeof identity)) {
        kwarn("nvkms-test", "display %#x base EDID identifies a different sink; refusing bound override",handle);
        goto done;
    }
    candidate->overrideEdid = NV_TRUE;
    if (!kapi.getDynamicDisplayInfo(test_device, candidate) || !candidate->connected)
        goto done;
    if (!edid_parse(candidate->edid.buffer, candidate->edid.bufferSize,
                    &parsed_edid)) goto done;
    *dyn = *candidate;
    *identity_ok = NV_TRUE;
    applied = NV_TRUE;
done:
    kfree(candidate);
    return applied;
}

static void capture_display_evidence(nvkms_display_evidence_t *out,
                                     NvKmsKapiDisplay handle,
                                     NvKmsKapiConnector connector, NvBool is_dp,
                                     const struct NvKmsKapiDynamicDisplayParams *dyn,
                                     NvBool valid, NvBool overridden, NvU32 waited)
{
    _Static_assert(KESTREL_NVKMS_EDID_BYTES == NVKMS_KAPI_EDID_BUFFER_SIZE,
                   "retained EDID must match the linked KAPI ABI");
    memset(out, 0, sizeof(*out));
    out->handle = handle;
    out->connector = connector;
    out->is_dp = is_dp;
    out->source = valid ? (overridden ? 2u : 1u) : 0u;
    out->waited_ms = waited;
    out->size = dyn->edid.bufferSize;
    if (out->size > sizeof out->bytes) out->size = sizeof out->bytes;
    memcpy(out->bytes, dyn->edid.buffer, out->size);
    if (valid) {
        strlcpy(out->manufacturer, parsed_edid.manufacturer, sizeof out->manufacturer);
        strlcpy(out->model, parsed_edid.model, sizeof out->model);
    }
}

static NvBool query_dynamic_display(NvKmsKapiDisplay handle,
                                    NvKmsKapiConnector connector_handle,
                                    NvBool connector_is_dp,
                                    struct NvKmsKapiDynamicDisplayParams *dyn,
                                    NvBool *edid_ok,
                                    NvU32 *waited_ms,
                                    NvU64 timeout_usec,
                                    NvU64 connection_deadline_usec)
{
    NvU64 began = timer_now_us();
    NvU64 deadline = began + timeout_usec;
    NvU32 attempt = 0;
    NvBool saw_connection = NV_FALSE;
    *edid_ok = NV_FALSE;
    *waited_ms = 0;
    for (;;) {
        memset(dyn, 0, sizeof(*dyn));
        dyn->handle = handle;
        if (!kapi.getDynamicDisplayInfo(test_device, dyn))
            return NV_FALSE;
        NvU64 now = timer_now_us();
        *waited_ms = (NvU32)((now - began) / 1000ull);
        if (!dyn->connected) {
            /* An initial negative DPLib snapshot is not a completed hotplug
             * lifecycle. Revisit late DP sinks before freezing the boot head
             * set; never force an absent connector connected or supply its
             * provisioned EDID before physical detection succeeds. All initial
             * connectors share one deadline, so unused ports do not each add
             * another ten seconds. A disconnect after connection stays final. */
            if (!connector_is_dp || saw_connection ||
                now >= connection_deadline_usec)
                return NV_TRUE;
        } else if (!saw_connection) {
            saw_connection = NV_TRUE;
            deadline = now + timeout_usec;
        }
        if (dyn->connected && dyn->edid.bufferSize >= 128) {
            *edid_ok = edid_parse(dyn->edid.buffer, dyn->edid.bufferSize,
                                  &parsed_edid) ? NV_TRUE : NV_FALSE;
            *waited_ms = (NvU32)((timer_now_us() - began) / 1000ull);
            if (*edid_ok) return NV_TRUE;
        }

        if (dyn->connected && now >= deadline) {
            *waited_ms = (NvU32)((now - began) / 1000ull);
            return NV_TRUE;
        }

        /* QUERY_CONNECTOR_DYNAMIC_DATA is the step NVIDIA's KAPI uses to
         * wait for DPLib detection.  Repeat it once per second while EDID is
         * absent so a late HPD/AUX completion is observed before mode query. */
        if (connector_is_dp && attempt != 0 && (attempt % 4u) == 0) {
            struct NvKmsKapiConnectorInfo refreshed;
            memset(&refreshed, 0, sizeof(refreshed));
            if (!kapi.getConnectorInfo(test_device, connector_handle,
                                       &refreshed))
                return NV_FALSE;
        }
        attempt++;
        timer_mdelay(EDID_WAKE_POLL_MSEC);
    }
}

static void set_all_layer_flags(struct NvKmsKapiLayerRequestedConfig *layer) {
    layer->flags.surfaceChanged = NV_TRUE;
    layer->flags.srcXYChanged = NV_TRUE;
    layer->flags.srcWHChanged = NV_TRUE;
    layer->flags.dstXYChanged = NV_TRUE;
    layer->flags.dstWHChanged = NV_TRUE;
    layer->flags.cscChanged = NV_TRUE;
    layer->flags.inputTfChanged = NV_TRUE;
    layer->flags.outputTfChanged = NV_TRUE;
    layer->flags.inputColorSpaceChanged = NV_TRUE;
    layer->flags.inputColorRangeChanged = NV_TRUE;
    layer->flags.hdrMetadataChanged = NV_TRUE;
    layer->flags.matrixOverridesChanged = NV_TRUE;
    layer->flags.ilutChanged = NV_TRUE;
    layer->flags.tmoChanged = NV_TRUE;
}

static NvBool build_display_config(const test_display_t *displays, NvU32 count,
                                    const dl_layout_t *layout, NvU32 previous_heads,
                                    struct NvKmsKapiRequestedModeSetConfig *config)
{
    /* Pure preparation: do not alter active[], requested, scanout resources
     * or the destination request until every candidate entry is checked.
     * Bandwidth/DSC/SOR compatibility still needs NVIDIA atomic validation. */
    if (!config || !displays || !count || count > KESTREL_NVKMS_MAX_TEST_DISPLAYS ||
        !resources.numHeads || resources.numHeads > NVKMS_KAPI_MAX_HEADS ||
        resources.numHeads >= 32 || (previous_heads >> resources.numHeads) ||
        (layout && layout->n != (int)count)) return NV_FALSE;
    NvU32 used_heads = 0;
    for (NvU32 i = 0; i < count; i++) {
        const test_display_t *d = &displays[i];
        if (layout && layout->out[i].active != 0 && layout->out[i].active != 1)
            return NV_FALSE;
        if (layout && !layout->out[i].active) continue;
        NvU32 w = d->mode.timings.hVisible, h = d->mode.timings.vVisible;
        if (!d->handle || d->head >= resources.numHeads ||
            !(d->static_info.headMask & (1u << d->head)) ||
            !resources.numLayers[d->head] || (used_heads & (1u << d->head)) ||
            d->front >= 2 || !d->surface[d->front] || !d->memory[d->front] ||
            !w || !h || (NvU64)w * 4 > d->pitch || (NvU64)d->pitch * h > d->bytes)
            return NV_FALSE;
        if (layout && (layout->out[i].out_w != (int)w ||
                       layout->out[i].out_h != (int)h)) return NV_FALSE;
        for (NvU32 j = 0; j < i; j++)
            if ((!layout || layout->out[j].active) && displays[j].handle == d->handle)
                return NV_FALSE;
        used_heads |= 1u << d->head;
    }
    if (!used_heads) return NV_FALSE;
    memset(config, 0, sizeof(*config));
    /* Heads removed or reassigned by a topology change need explicit inactive
     * state and a detached primary plane. Merely omitting them retains the
     * previous hardware state in NVIDIA's incremental request protocol. */
    config->headsMask = previous_heads;
    for (NvU32 head = 0; head < resources.numHeads; head++) {
        if (!(previous_heads & (1u << head)) || (used_heads & (1u << head))) continue;
        struct NvKmsKapiHeadRequestedConfig *h = &config->headRequestedConfig[head];
        h->flags.activeChanged = NV_TRUE;
        h->flags.displaysChanged = NV_TRUE;
        h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX].flags.surfaceChanged = NV_TRUE;
    }
    for (NvU32 i = 0; i < count; i++) {
        const test_display_t *d = &displays[i];
        if (layout && !layout->out[i].active) continue;
        NvU32 w = d->mode.timings.hVisible;
        NvU32 h = d->mode.timings.vVisible;
        struct NvKmsKapiHeadRequestedConfig *head =
            &config->headRequestedConfig[d->head];
        struct NvKmsKapiLayerRequestedConfig *layer =
            &head->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];

        config->headsMask |= 1u << d->head;
        head->modeSetConfig.bActive = NV_TRUE;
        head->modeSetConfig.numDisplays = 1;
        head->modeSetConfig.displays[0] = d->handle;
        head->modeSetConfig.mode = d->mode;
        /* nvidia-drm initializes every fresh CRTC state to this value.  Zero
         * is not the neutral FP normalization scale in the 595 KAPI. */
        head->modeSetConfig.olutFpNormScale =
            NVKMS_OLUT_FP_NORM_SCALE_DEFAULT;
        /* Match color_mgmt_config_set_luts() in NVIDIA's Linux DRM client.
         * KmsSetMode always marks the legacy input LUT specified, even when
         * no ramps are supplied.  depth=30,end=0 is the documented linear /
         * disabled encoding; leaving the zero-initialized depth at 0 can
         * commit a live head whose color pipeline emits only black. */
        head->modeSetConfig.lut.input.depth = 30;
        head->modeSetConfig.lut.input.start = 0;
        head->modeSetConfig.lut.input.end = 0;
        head->modeSetConfig.lut.input.pRamps = NULL;
        head->modeSetConfig.lut.output.enabled = NV_FALSE;
        head->modeSetConfig.lut.output.pRamps = NULL;
        head->flags.activeChanged = NV_TRUE;
        head->flags.displaysChanged = NV_TRUE;
        head->flags.modeChanged = NV_TRUE;
        /* Merely filling modeSetConfig.lut is not enough: NVKMS consumes
         * requested state incrementally and ignores fields whose matching
         * Changed bit is clear.  NVIDIA's color_mgmt_config_set_luts() sets
         * both of these bits even for the linear-input/disabled-output case.
         * Without them a newly activated Blackwell head can retain the reset
         * LUT state, accept every surface flip, and still emit only black. */
        head->flags.legacyIlutChanged = NV_TRUE;
        head->flags.legacyOlutChanged = NV_TRUE;
        layer->config.surface = d->surface[d->front];
        layer->config.compParams.compMode =
            NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
        layer->config.compParams.surfaceAlpha = 0xff;
        /* NVIDIA's non-async Linux plane path always presents at the next
         * vblank.  Zero is reserved for an async/tearing flip; the former
         * zero/false hybrid was not a configuration Linux ever submits. */
        layer->config.minPresentInterval = 1;
        layer->config.tearing = NV_FALSE;
        layer->config.rrParams.rotation = NVKMS_ROTATION_0;
        layer->config.inputTf = NVKMS_INPUT_TF_LINEAR;
        layer->config.outputTf = NVKMS_OUTPUT_TF_NONE;
        layer->config.inputColorSpace = NVKMS_INPUT_COLOR_SPACE_NONE;
        layer->config.inputColorRange = NVKMS_INPUT_COLOR_RANGE_DEFAULT;
        layer->config.csc = NVKMS_IDENTITY_CSC_MATRIX;
        layer->config.srcWidth = w;
        layer->config.srcHeight = h;
        layer->config.dstWidth = w;
        layer->config.dstHeight = h;
        set_all_layer_flags(layer);
    }
    return NV_TRUE;
}

static NvBool build_requested_config(NvU32 count)
{
    return build_display_config(active, count, NULL, 0, &requested);
}

/* Head masks alone are not a sufficient topology proof: SOR routing and IMP
 * resource allocation can reject one otherwise legal permutation while
 * accepting another.  Enumerate every unique compatible head assignment and
 * ask NVIDIA's own atomic validator at the leaf.  At four heads this is at
 * most 24 validation calls.  `requested` is intentionally left holding the
 * successful assignment for the subsequent real commit. */
static NvBool find_valid_head_assignment(NvU32 count, NvU32 at,
                                         NvU32 used_mask)
{
    if (at == count) {
        if (!build_requested_config(count)) return NV_FALSE;
        memset(&reply, 0, sizeof(reply));
        return kapi.applyModeSetConfig(test_device, &requested, &reply,
                                       NV_FALSE);
    }
    NvU32 allowed = active[at].static_info.headMask &
                    ((1u << resources.numHeads) - 1u) & ~used_mask;
    for (NvU32 head = 0; head < resources.numHeads; head++) {
        if (!(allowed & (1u << head)) || resources.numLayers[head] == 0)
            continue;
        active[at].head = head;
        if (find_valid_head_assignment(count, at + 1,
                                       used_mask | (1u << head)))
            return NV_TRUE;
    }
    return NV_FALSE;
}

static NvBool allocate_display_slot(test_display_t *d, NvU32 slot)
{
    NvU8 compressible = 0;
    struct NvKmsKapiAllocateMemoryParams mp;
    memset(&mp, 0, sizeof(mp));
    mp.layout = NvKmsSurfaceMemoryLayoutPitch;
    mp.type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT;
    mp.size = d->bytes;
    mp.noDisplayCaching = NV_TRUE;
    mp.useVideoMemory = resources.caps.hasVideoMemory ? NV_TRUE : NV_FALSE;
    mp.compressible = &compressible;
    d->memory[slot] = kapi.allocateMemory(test_device, &mp);
    if (!d->memory[slot]) return NV_FALSE;

    struct NvKmsKapiCreateSurfaceParams sp;
    memset(&sp, 0, sizeof(sp));
    sp.planes[0].memory = d->memory[slot];
    sp.planes[0].offset = 0;
    sp.planes[0].pitch = d->pitch;
    sp.width = d->mode.timings.hVisible;
    sp.height = d->mode.timings.vVisible;
    sp.format = NvKmsSurfaceMemoryFormatX8R8G8B8;
    sp.explicit_layout = NV_FALSE;
    sp.layout = NvKmsSurfaceMemoryLayoutPitch;
    d->surface[slot] = kapi.createSurface(test_device, &sp);
    if (!d->surface[slot]) {
        kapi.freeMemory(test_device, d->memory[slot]);
        d->memory[slot] = NULL;
        return NV_FALSE;
    }
    return NV_TRUE;
}

static void free_display_slot(test_display_t *d, NvU32 slot)
{
    if (d->surface[slot]) {
        kapi.destroySurface(test_device, d->surface[slot]);
        d->surface[slot] = NULL;
    }
    if (d->memory[slot]) {
        kapi.freeMemory(test_device, d->memory[slot]);
        d->memory[slot] = NULL;
    }
}

static void fill_display_red(test_display_t *d)
{
    NvU32 w = d->mode.timings.hVisible;
    NvU32 h = d->mode.timings.vVisible;
    NvU32 stride = d->pitch / 4u;
    for (NvU32 y = 0; y < h; y++)
        for (NvU32 x = 0; x < w; x++)
            d->pixels[(NvU64)y * stride + x] = 0x00ff0000u;
}

static NvBool upload_display(test_display_t *d, NvU32 slot)
{
    NvU32 hClient = ((const kapi_device_rm_prefix_t *)test_device)->hRmClient;
    NvU32 hMemory =
        ((const kapi_memory_rm_prefix_t *)d->memory[slot])->hRmHandle;
    NvU32 status = nvrm_transfer_rm_memory(hClient, hMemory, 0,
                                           d->pixels, d->bytes,
                                           NV_FALSE);
    if (status != 0)
        kerr("nvkms-test", "copy-engine VRAM upload failed: %#x", status);
    return status == 0;
}

static NvBool draw_phase(NvU32 phase, NvU32 count, NvU32 slot) {
    static const NvU32 solid[4] = {
        0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00ffffffu
    };
    static const NvU32 vertical[8] = {
        0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
        0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u
    };
    static const NvU32 horizontal[8] = {
        0x00000000u, 0x000000ffu, 0x00ff0000u, 0x00ff00ffu,
        0x0000ff00u, 0x0000ffffu, 0x00ffff00u, 0x00ffffffu
    };
    static const NvU32 mosaic[6] = {
        0x00ff3030u, 0x0030ff30u, 0x003030ffu,
        0x00ffff30u, 0x0030ffffu, 0x00ff30ffu
    };
    for (NvU32 d = 0; d < count; d++) {
        NvU32 w = active[d].mode.timings.hVisible;
        NvU32 h = active[d].mode.timings.vVisible;
        NvU32 stride = active[d].pitch / 4u;
        for (NvU32 y = 0; y < h; y++) {
            NvU32 *row = active[d].pixels + (NvU64)y * stride;
            NvU32 row_color = phase == 5 ? horizontal[(NvU64)y * 8u / h] : 0;
            for (NvU32 x = 0; x < w; x++) {
                NvU32 color;
                if (phase < 4) color = solid[phase];
                else if (phase == 4) color = vertical[(NvU64)x * 8u / w];
                else if (phase == 5) color = row_color;
                else if (phase == 6) color = mosaic[((x >> 6) + (y >> 6) + d) % 6u];
                else color = 0; /* neutral signal-acquisition frame */
                row[x] = color;
            }
        }
        if (!upload_display(&active[d], slot)) return NV_FALSE;
    }
    return NV_TRUE;
}

/* Linux DRM presents every new framebuffer through the NVKMS flip ioctl.
 * Rewriting a surface already owned by the display engine is neither the
 * normal contract nor guaranteed to invalidate the display-side cache.  Build
 * a flip-only request (no modeset flags) for an alternate, fully populated
 * VRAM surface on every head.  This also prevents the raster from reading a
 * buffer while the copy engine is updating it. */
static void build_flip_config(NvU32 count, NvU32 slot)
{
    memset(&requested, 0, sizeof(requested));
    for (NvU32 i = 0; i < count; i++) {
        test_display_t *d = &active[i];
        NvU32 w = d->mode.timings.hVisible;
        NvU32 h = d->mode.timings.vVisible;
        struct NvKmsKapiHeadRequestedConfig *head =
            &requested.headRequestedConfig[d->head];
        struct NvKmsKapiLayerRequestedConfig *layer =
            &head->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];

        requested.headsMask |= 1u << d->head;
        /* KmsFlip's head validator requires the currently-active topology,
         * even though the flip itself does not change it. */
        head->modeSetConfig.bActive = NV_TRUE;
        head->modeSetConfig.numDisplays = 1;
        head->modeSetConfig.displays[0] = d->handle;
        head->modeSetConfig.mode = d->mode;

        layer->config.surface = d->surface[slot];
        layer->config.compParams.compMode =
            NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
        layer->config.compParams.surfaceAlpha = 0xff;
        layer->config.minPresentInterval = 1;
        layer->config.tearing = NV_FALSE;
        layer->config.rrParams.rotation = NVKMS_ROTATION_0;
        layer->config.srcWidth = w;
        layer->config.srcHeight = h;
        layer->config.dstWidth = w;
        layer->config.dstHeight = h;
        layer->flags.surfaceChanged = NV_TRUE;
    }
}

static NvBool present_prepared_slot(NvU32 count, NvU32 slot,
                                    const char *operation)
{
    NvU32 before[NVKMS_KAPI_MAX_HEADS];
    build_flip_config(count, slot);

    /* A preceding modeset/flip can remain outstanding until the next vblank.
     * Match Linux's atomic check-before-commit behavior, but wait boundedly for
     * NVKMS to retire that operation instead of silently dropping this frame. */
    for (NvU32 attempt = 0; attempt < 100; attempt++) {
        memset(&reply, 0, sizeof(reply));
        if (!kapi.applyModeSetConfig(test_device, &requested, &reply,
                                     NV_FALSE))
            return NV_FALSE;
        if (reply.flipResult == NV_KMS_FLIP_RESULT_SUCCESS)
            break;
        if (reply.flipResult != NV_KMS_FLIP_RESULT_IN_PROGRESS ||
            attempt == 99) {
            kerr("nvkms-test", "%s flip validation result=%u",
                 operation, reply.flipResult);
            return NV_FALSE;
        }
        timer_mdelay(10);
    }

    snapshot_flip_sequences(before);
    memset(&reply, 0, sizeof(reply));
    /* Match nv_drm_atomic_apply_modeset_config(): publish any outstanding
     * write-combined scanout stores before the hardware consumes the flip. */
    if (kapi.systemInfo.bAllowWriteCombining)
        __asm__ volatile("sfence" ::: "memory");
    if (!kapi.applyModeSetConfig(test_device, &requested, &reply, NV_TRUE) ||
        reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS) {
        kerr("nvkms-test", "%s atomic flip result=%u",
             operation, reply.flipResult);
        return NV_FALSE;
    }
    /* Linux logs a three-second timeout but retires its client-side flip and
     * continues.  Do the same: the following five-second dwell is also a
     * conservative buffer-reuse barrier when this host has failed to deliver
     * the optional completion notification. */
    NvBool completion_seen =
        wait_for_flip_mask(requested.headsMask, before, operation);
    for (NvU32 i = 0; i < count; i++)
        active[i].front = slot;
    kinfo("nvkms-test", "%s atomically presented on heads %#x via VRAM slot %u%s",
          operation, requested.headsMask, slot,
          completion_seen ? "" : " (completion event unavailable)");
    return NV_TRUE;
}

static NvBool present_phase(NvU32 phase, NvU32 count)
{
    NvU32 slot = active[0].front ^ 1u;
    if (!draw_phase(phase, count, slot))
        return NV_FALSE;
    char operation[40];
    snprintf(operation, sizeof(operation), "atomic pattern phase %u flip", phase);
    return present_prepared_slot(count, slot, operation);
}

static bool fail(nvkms_kapi_test_result_t *r, const char *where) {
    r->failed_at = where;
    kerr("nvkms-test", "native display test stopped at %s", where);
    return false;
}

bool nvkms_kapi_run_display_test(nvkms_kapi_test_result_t *r) {
    NvKmsKapiDisplay handles[TEST_MAX_ENUM_DISPLAYS];
    NvU32 handle_count = TEST_MAX_ENUM_DISPLAYS;
    NvU32 count = 0;

    memset(r, 0, sizeof(*r));
    active_count = 0;
    r->ran = true;
    if (!nvrm_is_ready() || !nvkms_host_link_ready())
        return fail(r, "RM/NVKMS module readiness");

    memset(&kapi, 0, sizeof(kapi));
    kapi.versionString = "595.99.02";
    if (!nvKmsKapiGetFunctionsTableInternal(&kapi))
        return fail(r, "KAPI version/functions table");
    r->table_ok = true;

    wanted_gpu = nvrm_gpu_id();
    test_card = nvkms_host_card();
    if (!test_card) return fail(r, "KAPI GPU ownership");
    enum_gpu_count = 0;
    enum_gpu_matched = NV_FALSE;
    memset((void *)flip_sequence, 0, sizeof(flip_sequence));
    r->enumerated_gpus = kapi.enumerateGpus(enum_gpu_cb);
    if (!enum_gpu_matched || r->enumerated_gpus == 0)
        return fail(r, "KAPI GPU enumeration");

    struct NvKmsKapiAllocateDeviceParams alloc;
    memset(&alloc, 0, sizeof(alloc));
    alloc.gpuId = wanted_gpu;
    alloc.migDevice = NO_MIG_DEVICE;
    alloc.eventCallback = nvkms_test_event_cb;
    test_device = kapi.allocateDevice(&alloc);
    if (!test_device) return fail(r, "KAPI allocateDevice");
    r->device_ok = true;
    if (!kapi.grabOwnership(test_device))
        return fail(r, "KAPI grabOwnership");
    r->ownership_ok = true;

    memset(&resources, 0, sizeof(resources));
    if (!kapi.getDeviceResourcesInfo(test_device, &resources) ||
        resources.numHeads == 0 || resources.numHeads > NVKMS_KAPI_MAX_HEADS)
        return fail(r, "KAPI device resources");
    if (!(resources.supportedSurfaceMemoryFormats[NVKMS_KAPI_LAYER_PRIMARY_IDX] &
          (1ull << NvKmsSurfaceMemoryFormatX8R8G8B8)))
        return fail(r, "XRGB8888 primary scanout unsupported");
    r->resources_ok = true;

    if (kapi.declareEventInterest &&
        !kapi.declareEventInterest(
            test_device,
            (1u << NVKMS_EVENT_TYPE_DPY_CHANGED) |
            (1u << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED) |
            (1u << NVKMS_EVENT_TYPE_FLIP_OCCURRED)))
        return fail(r, "KAPI event interest registration");

    memset(handles, 0, sizeof(handles));
    if (!kapi.getDisplays(test_device, &handle_count, handles))
        return fail(r, "KAPI display enumeration");
    if (handle_count > TEST_MAX_ENUM_DISPLAYS)
        return fail(r, "display enumeration overflow");

    NvU64 connection_deadline = timer_now_us() + DP_CONNECT_TIMEOUT_USEC;
    NvU32 disconnected_mask = 0;
    memset(active, 0, sizeof(active));
    for (NvU32 i = 0; i < handle_count; i++) {
        struct NvKmsKapiDynamicDisplayParams dyn;
        struct NvKmsKapiStaticDisplayInfo stat;
        struct NvKmsKapiConnectorInfo connector;
        memset(&dyn, 0, sizeof(dyn));
        memset(&stat, 0, sizeof(stat));
        memset(&connector, 0, sizeof(connector));
        /* Match nvidia-drm's per-display connector lifecycle before asking
         * NVKMS for dynamic state or modes.  getConnectorInfo() is not just
         * metadata for DisplayPort: it polls QUERY_CONNECTOR_DYNAMIC_DATA
         * until DP detection is complete (or the official timeout expires).
         * Skipping this let the first connector expose only NVKMS's 1024x768
         * fallback while already-cached connectors exposed complete lists. */
        if (!kapi.getStaticDisplayInfo(test_device, handles[i], &stat))
            return fail(r, "KAPI static display information");
        if (!kapi.getConnectorInfo(test_device, stat.connectorHandle,
                                   &connector))
            return fail(r, "KAPI connector detection");
        if (connector.signalFormat == NVKMS_CONNECTOR_SIGNAL_FORMAT_DP &&
            !connector.dynamicDpyIdListValid)
            return fail(r, "DisplayPort detection completion");
        NvBool edid_ok = NV_FALSE;
        NvU32 edid_waited_ms = 0;
        if (!query_dynamic_display(handles[i], stat.connectorHandle,
                                   connector.signalFormat ==
                                       NVKMS_CONNECTOR_SIGNAL_FORMAT_DP,
                                   &dyn, &edid_ok, &edid_waited_ms,
                                   EDID_INITIAL_TIMEOUT_USEC,
                                   connection_deadline))
            return fail(r, "KAPI dynamic display information");
        if (!dyn.connected) {
            disconnected_mask |= handles[i];
            kinfo("nvkms-test", "display %#x connector %#x remained disconnected after %u ms; not forced active",
                  handles[i], stat.connectorHandle, edid_waited_ms);
            continue;
        }
        r->connected_displays++;
        if (count >= KESTREL_NVKMS_MAX_TEST_DISPLAYS)
            return fail(r, "more connected displays than supported heads");

        /* Linux's drm.edid_firmware override is supplied on the detect call
         * that creates the connector's mode pool, before get_modes() asks
         * NVKMS to enumerate it.  Do the same here after a bounded live-EDID
         * attempt.  Applying the override only after a 1024x768 wake modeset
         * is too late on a zero-EDID SST sink: the RM SET/GET round trip in
         * NVIDIA's stock NVKMS path can leave the fallback pool in place.
         *
         * This remains connector- and failure-scoped.  Any complete live
         * EDID wins, and an unprovisioned replacement monitor continues down
         * the generic safe-wake/hotplug recovery path below. */
        NvBool used_initial_override = NV_FALSE;
        if (!edid_ok &&
            apply_provisioned_edid(handles[i], &dyn, &edid_ok)) {
            used_initial_override = NV_TRUE;
        }
        capture_display_evidence(&r->evidence[count], handles[i],
                                  stat.connectorHandle,
                                  connector.signalFormat == NVKMS_CONNECTOR_SIGNAL_FORMAT_DP,
                                  &dyn, edid_ok, used_initial_override, edid_waited_ms);
        r->evidence_count = count + 1;

        active[count].handle = handles[i];
        active[count].static_info = stat;
        active[count].connector_handle = stat.connectorHandle;
        active[count].connector_is_dp =
            connector.signalFormat == NVKMS_CONNECTOR_SIGNAL_FORMAT_DP;
        active[count].edid_pending = !edid_ok;
        if (edid_ok) {
            strlcpy(active[count].manufacturer, parsed_edid.manufacturer,
                    sizeof active[count].manufacturer);
            strlcpy(active[count].model, parsed_edid.model,
                    sizeof active[count].model);
        }
        if (!select_max_mode(handles[i], &active[count].mode))
            return fail(r, "no validated display mode");
        if (edid_ok)
            kinfo("nvkms-test", "display %#x is %s %s; %s EDID %u B after %u ms, selected %ux%u @ %u.%03u Hz",
                  handles[i], parsed_edid.manufacturer,
                  parsed_edid.model[0] ? parsed_edid.model : "(unnamed)",
                  used_initial_override ? "connector-override" : "live",
                  dyn.edid.bufferSize, edid_waited_ms,
                  active[count].mode.timings.hVisible,
                  active[count].mode.timings.vVisible,
                  active[count].mode.timings.refreshRate / 1000u,
                  active[count].mode.timings.refreshRate % 1000u);
        else if (dyn.edid.bufferSize >= 128)
            kwarn("nvkms-test", "display %#x supplied %u EDID bytes after %u ms but the local identity parser rejected them; NVKMS still selected its validated %ux%u @ %u.%03u Hz mode",
                  handles[i], dyn.edid.bufferSize, edid_waited_ms,
                  active[count].mode.timings.hVisible,
                  active[count].mode.timings.vVisible,
                  active[count].mode.timings.refreshRate / 1000u,
                  active[count].mode.timings.refreshRate % 1000u);
        else
            kwarn("nvkms-test", "display %#x stayed connected with zero EDID bytes for %u ms; selected only NVKMS-validated fallback %ux%u @ %u.%03u Hz",
                  handles[i], edid_waited_ms,
                  active[count].mode.timings.hVisible,
                  active[count].mode.timings.vVisible,
                  active[count].mode.timings.refreshRate / 1000u,
                  active[count].mode.timings.refreshRate % 1000u);
        count++;
    }
    if (count == 0) return fail(r, "no connected displays");
    if (count > resources.numHeads)
        return fail(r, "connected display count exceeds hardware heads");
    for (NvU32 i = 0; i < count; i++) {
        test_display_t *d = &active[i];
        NvU32 w = d->mode.timings.hVisible;
        NvU32 h = d->mode.timings.vVisible;
        d->pitch = align_up_u32(w * 4u, resources.caps.pitchAlignment);
        d->bytes = align_up_u64((NvU64)d->pitch * h, 4096u);

        for (NvU32 slot = 0; slot < 2; slot++) {
            /* VRAM scanout.  HW-proven: sysmem scanout is rejected by
             * Blackwell NVKMS.  Use two independent surfaces so each pattern
             * can be submitted through the official page-flip path. */
            if (!allocate_display_slot(d, slot))
                return fail(r, "KAPI scanout memory allocation");
        }
        /* Do not CPU-map this VRAM allocation.  The card's BAR1 client-vidmem
         * aperture returns 0xBAD0AC00 even though BAR2, the raster and all DP
         * links are live.  Populate the allocation through NVIDIA RM's CE
         * transfer path instead. */
        d->pixels = kzalloc((size_t)d->bytes);
        if (!d->pixels) return fail(r, "scanout staging allocation");

        /* Start with red already resident.  The atomic commit can therefore
         * light every connector with valid pixels in the same transaction. */
        fill_display_red(d);
        d->front = 0;
        if (!upload_display(d, d->front))
            return fail(r, "initial copy-engine VRAM upload");

    }

    /* Linux does this only after event registration and connector/encoder
     * enumeration.  At this point every DP connector has finished detection,
     * its EDID-backed maximum mode is retained, and Kestrel's GOP view was
     * already permanently abandoned by the preceding GPU reset. */
    if (kapi.framebufferConsoleDisabled)
        kapi.framebufferConsoleDisabled(test_device);

    if (!find_valid_head_assignment(count, 0, 0))
        return fail(r, "no atomic-valid unique head assignment");
    r->validated = true;
    for (NvU32 i = 0; i < count; i++) {
        test_display_t *d = &active[i];
        r->display[i].handle = d->handle;
        r->display[i].head = d->head;
        r->display[i].width = d->mode.timings.hVisible;
        r->display[i].height = d->mode.timings.vVisible;
        r->display[i].refresh_millihz = d->mode.timings.refreshRate;
        r->display[i].pixel_clock_hz = d->mode.timings.pixelClockHz;
    }
    memset(&reply, 0, sizeof(reply));
    /* NVIDIA's Linux client performs this fence for every committing atomic
     * request when the KAPI reports that write combining is available. */
    if (kapi.systemInfo.bAllowWriteCombining)
        __asm__ volatile("sfence" ::: "memory");
    if (!kapi.applyModeSetConfig(test_device, &requested, &reply, NV_TRUE))
        return fail(r, "atomic modeset commit");
    r->committed = true;
    r->active_displays = count;
    r->active_heads_mask = requested.headsMask;
    kinfo("nvkms-test", "atomic modeset committed: %u display(s), heads %#x",
          count, requested.headsMask);
    kinfo("nvkms-test", "boot discovery: %u enumerated display paths, %u connected, disconnected path mask %#x",
          handle_count, count, disconnected_mask);
    /* Do not wait for FLIP_OCCURRED here.  This is the first transition from
     * an inactive CRTC/plane to an active one.  NVIDIA's Linux client
     * (__will_generate_flip_event) explicitly says hardware generates no flip
     * event in that state, and therefore never enqueues or waits for one. */

    /* Some DP sinks (the target Acer X27U W1 is one) keep AUX EDID asleep
     * until they have received a valid video stream.  Waiting longer before
     * modeset cannot solve that protocol ordering: the hardware log proved it
     * returned zero bytes for a real 15 seconds, yet accepted fallback scanout.
     * Treat the first atomic commit as a wake modeset for only those sinks.
     * Once every connector has a valid stream, repeat NVIDIA's dynamic query,
     * enumerate its newly-created validated modes, and promote all heads in a
     * second atomic transaction.  This is monitor-agnostic and is also the
     * ordering used by a normal hotplug-driven desktop after a sleeping sink
     * wakes. */
    NvBool have_pending_edid = NV_FALSE;
    for (NvU32 i = 0; i < count; i++)
        if (active[i].edid_pending) have_pending_edid = NV_TRUE;

    if (have_pending_edid) {
        kinfo("nvkms-test", "safe wake modeset is live; waiting 2000 ms before post-signal EDID discovery");
        timer_mdelay(2000);
        NvBool any_mode_changed = NV_FALSE;

        for (NvU32 i = 0; i < count; i++) {
            test_display_t *d = &active[i];
            if (!d->edid_pending) continue;

            struct NvKmsKapiDynamicDisplayParams dyn;
            NvBool edid_ok = NV_FALSE;
            NvU32 waited_ms = 0;
            if (!query_dynamic_display(d->handle, d->connector_handle,
                                       d->connector_is_dp, &dyn, &edid_ok,
                                       &waited_ms, EDID_INITIAL_TIMEOUT_USEC, 0))
                return fail(r, "post-signal dynamic display information");

            NvBool used_override = NV_FALSE;
            if (!edid_ok) {
                /* The sink remained anonymous or invalid after valid scanout. Only
                 * now may a connector-scoped provisioned EDID replace it.  If
                 * none was provisioned, preserve the full 15-second live-only
                 * recovery window before accepting the safe fallback. */
                if (!apply_provisioned_edid(d->handle, &dyn, &edid_ok)) {
                    NvU32 extra_waited_ms = 0;
                    if (!query_dynamic_display(d->handle, d->connector_handle,
                                               d->connector_is_dp, &dyn,
                                               &edid_ok, &extra_waited_ms,
                                               EDID_WAKE_TIMEOUT_USEC, 0))
                        return fail(r, "extended post-signal dynamic display information");
                    waited_ms += extra_waited_ms;
                    if (!edid_ok) {
                        kwarn("nvkms-test", "display %#x still supplied no valid EDID after %u ms of valid video signal and has no valid connector override; retaining %ux%u @ %u.%03u Hz fallback",
                              d->handle, waited_ms,
                              d->mode.timings.hVisible, d->mode.timings.vVisible,
                              d->mode.timings.refreshRate / 1000u,
                              d->mode.timings.refreshRate % 1000u);
                        continue;
                    }
                } else {
                    used_override = NV_TRUE;
                }
            }

            struct NvKmsKapiDisplayMode promoted;
            capture_display_evidence(&r->evidence[i], d->handle,
                                      d->connector_handle, d->connector_is_dp,
                                      &dyn, edid_ok, used_override, waited_ms);
            memset(&promoted, 0, sizeof(promoted));
            if (!select_max_mode(d->handle, &promoted))
                return fail(r, "post-signal maximum display mode");

            kinfo("nvkms-test", "display %#x %s EDID %u B after %u ms%s; promoting %ux%u @ %u.%03u Hz to maximum validated %ux%u @ %u.%03u Hz",
                  d->handle, used_override ? "connector-override" : "post-signal",
                  dyn.edid.bufferSize, waited_ms,
                  edid_ok ? " (identity parsed)" : " (NVKMS parsed)",
                  d->mode.timings.hVisible, d->mode.timings.vVisible,
                  d->mode.timings.refreshRate / 1000u,
                  d->mode.timings.refreshRate % 1000u,
                  promoted.timings.hVisible, promoted.timings.vVisible,
                  promoted.timings.refreshRate / 1000u,
                  promoted.timings.refreshRate % 1000u);
            d->edid_pending = NV_FALSE;
            if (edid_ok) {
                strlcpy(d->manufacturer, parsed_edid.manufacturer,
                        sizeof d->manufacturer);
                strlcpy(d->model, parsed_edid.model, sizeof d->model);
            }

            if (memcmp(&promoted, &d->mode, sizeof(promoted)) == 0)
                continue;

            /* Slot zero is still scanned out by the wake commit.  Slot one is
             * inactive, so replace it at the final dimensions, commit it, and
             * only then replace the old slot-zero allocation. */
            free_display_slot(d, 1);
            kfree(d->pixels);
            d->mode = promoted;
            d->pitch = align_up_u32(d->mode.timings.hVisible * 4u,
                                    resources.caps.pitchAlignment);
            d->bytes = align_up_u64((NvU64)d->pitch *
                                    d->mode.timings.vVisible, 4096u);
            d->pixels = kzalloc((size_t)d->bytes);
            if (!d->pixels)
                return fail(r, "post-signal scanout staging allocation");
            fill_display_red(d);
            if (!allocate_display_slot(d, 1) || !upload_display(d, 1))
                return fail(r, "post-signal maximum-mode VRAM surface");
            d->mode_changed_after_wake = NV_TRUE;
            any_mode_changed = NV_TRUE;
        }

        if (any_mode_changed) {
            /* Use slot one on every head, including heads whose mode did not
             * change, so slot-zero can be safely rebuilt where necessary. */
            for (NvU32 i = 0; i < count; i++) active[i].front = 1;
            if (!find_valid_head_assignment(count, 0, 0))
                return fail(r, "post-signal atomic head/mode validation");
            memset(&reply, 0, sizeof(reply));
            if (kapi.systemInfo.bAllowWriteCombining)
                __asm__ volatile("sfence" ::: "memory");
            if (!kapi.applyModeSetConfig(test_device, &requested, &reply,
                                        NV_TRUE) ||
                reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS)
                return fail(r, "post-signal maximum-mode atomic commit");

            for (NvU32 i = 0; i < count; i++) {
                test_display_t *d = &active[i];
                if (!d->mode_changed_after_wake) continue;
                free_display_slot(d, 0);
                if (!allocate_display_slot(d, 0) || !upload_display(d, 0))
                    return fail(r, "post-signal alternate VRAM surface");
            }
            kinfo("nvkms-test", "post-signal maximum-mode atomic commit complete on heads %#x",
                  requested.headsMask);
        }
    }

    /* Select only metadata here: never perturb the head/surface order used by
     * the already-validated rendering sequence. */
    select_preferred_primary(count);

    /* Publish the final post-hotplug geometry, not an initial wake fallback. */
    for (NvU32 i = 0; i < count; i++) {
        test_display_t *d = &active[i];
        r->display[i].handle = d->handle;
        r->display[i].head = d->head;
        r->display[i].width = d->mode.timings.hVisible;
        r->display[i].height = d->mode.timings.vVisible;
        r->display[i].refresh_millihz = d->mode.timings.refreshRate;
        r->display[i].pixel_clock_hz = d->mode.timings.pixelClockHz;
    }

    /* Snapshot the boot inventory epoch when its final geometry is recorded,
     * not after the long pattern/engine tests. Events after this point must
     * invalidate the desktop's seed inventory even before its worker starts. */
    nvkms_watch_boot_boundary();

    /* The initial request changes the legacy input LUT state.  Linux's
     * blocking atomic commit waits for each affected LUT notifier before it
     * treats the commit as complete; do the same instead of racing the first
     * scanout/CRC transaction. */
    if (kapi.checkLutNotifier) {
        for (NvU32 i = 0; i < count; i++)
            if (!kapi.checkLutNotifier(test_device, active[i].head, NV_TRUE))
                kwarn("nvkms-test", "LUT notifier timeout on head %u; continuing as NVIDIA's blocking Linux client does",
                      active[i].head);
    }

    /* A completed modeset proves the source is sending a signal, not that a
     * monitor's scaler has finished acquiring it. Keep a valid neutral frame
     * on EVERY output after the last timing change, then start red with one
     * all-head flip. Do not count the modeset's incidental red as a test phase.
     * DP does not report when a panel actually lights its pixels; this common
     * ten-second acquisition allowance is not a claim of panel-ready feedback
     * or physical genlock between monitors running independent refresh clocks. */
    if (!present_phase(7, count))
        return fail(r, "all-head neutral acquisition frame");
    kinfo("nvkms-test", "all-head signal acquisition: 10000 ms neutral frame before synchronized red");
    timer_mdelay(10000);
    if (!present_phase(0, count))
        return fail(r, "all-head synchronized red start");
    kinfo("nvkms-test", "RGBW shared start: all %u displays, five seconds per phase", count);

    /* Sample all three hardware stages now (frame A = red).
     * Compositor distinguishes a fetched surface
     * from a rejected promotion, while output is the SF/SOR value NVIDIA's
     * Linux DRM driver exposes as the authoritative scanout CRC. */
    timer_mdelay(500);
    for (NvU32 i = 0; i < count; i++) {
        struct NvKmsKapiCrcs crcs; memset(&crcs, 0, sizeof crcs);
        if (kapi.getCRC32 && kapi.getCRC32(test_device, active[i].head, &crcs)) {
            r->display[i].compositor_crc_supported = crcs.compositorCrc32.supported;
            r->display[i].compositor_crc_a = crcs.compositorCrc32.value;
            r->display[i].raster_crc_supported = crcs.rasterGeneratorCrc32.supported;
            r->display[i].raster_crc_a = crcs.rasterGeneratorCrc32.value;
            r->display[i].output_crc_supported = crcs.outputCrc32.supported;
            r->display[i].output_crc_a = crcs.outputCrc32.value;
        }
    }

    /* Continue with G/B/W, vertical bars, horizontal bars and a colour mix. */
    /* Keep every phase visible long enough to inspect several physical
     * monitors.  The restarted red phase has already been displayed for the
     * 500 ms CRC settling interval above, so wait another 4500 ms here; every
     * subsequent phase receives the full five-second dwell after its flip. */
    timer_mdelay(4500);
    for (NvU32 phase = 1; phase < 7; phase++) {
        if (!present_phase(phase, count))
            return fail(r, "atomic VRAM pattern flip");
        timer_mdelay(5000);
    }

    /* Frame B = the last (mosaic) pattern, visually very different from red.
     * Report every stage rather than inferring promotion from raster alone. */
    for (NvU32 i = 0; i < count; i++) {
        struct NvKmsKapiCrcs crcs; memset(&crcs, 0, sizeof crcs);
        if (kapi.getCRC32 && kapi.getCRC32(test_device, active[i].head, &crcs)) {
            r->display[i].compositor_crc_supported = crcs.compositorCrc32.supported;
            r->display[i].compositor_crc_b = crcs.compositorCrc32.value;
            r->display[i].raster_crc_supported = crcs.rasterGeneratorCrc32.supported;
            r->display[i].raster_crc_b = crcs.rasterGeneratorCrc32.value;
            r->display[i].output_crc_supported = crcs.outputCrc32.supported;
            r->display[i].output_crc_b = crcs.outputCrc32.value;
        }
        bool compositor_live = r->display[i].compositor_crc_supported &&
            r->display[i].compositor_crc_a != r->display[i].compositor_crc_b;
        bool raster_live = r->display[i].raster_crc_supported &&
            r->display[i].raster_crc_a != r->display[i].raster_crc_b;
        bool output_live = r->display[i].output_crc_supported &&
            r->display[i].output_crc_a != r->display[i].output_crc_b;
        if (r->display[i].output_crc_supported) r->crc_supported = true;
        if (compositor_live) r->surface_fetch_confirmed = true;
        if (output_live) r->scanout_confirmed = true;
        kinfo("nvkms-test",
              "head%u CRC comp=%#x->%#x(%s) raster=%#x->%#x(%s) output=%#x->%#x(%s)",
              active[i].head,
              r->display[i].compositor_crc_a, r->display[i].compositor_crc_b,
              compositor_live ? "live" : "static",
              r->display[i].raster_crc_a, r->display[i].raster_crc_b,
              raster_live ? "live" : "static",
              r->display[i].output_crc_a, r->display[i].output_crc_b,
              output_live ? "live" : "static");
    }
    if (!r->crc_supported)
        kinfo("nvkms-test", "output CRC unsupported: cannot confirm SF/SOR scanout");
    else
        kinfo("nvkms-test", "surface_fetch_confirmed=%d output_scanout_confirmed=%d",
              r->surface_fetch_confirmed, r->scanout_confirmed);

    /* Verify the same CE route in both directions.  This is stronger than the
     * old BAR1 sentinel: it checks the actual backing allocation while avoiding
     * the known-broken CPU aperture.  One top-left pixel is visually irrelevant. */
    for (NvU32 i = 0; i < count; i++) {
        u32 want = 0xdead0000u | (i + 1u);
        u32 got = 0;
        NvU32 hClient = ((const kapi_device_rm_prefix_t *)test_device)->hRmClient;
        NvU32 hMemory = ((const kapi_memory_rm_prefix_t *)
                         active[i].memory[active[i].front])->hRmHandle;
        NvU32 ws = nvrm_transfer_rm_memory(hClient, hMemory, 0, &want,
                                           sizeof want, NV_FALSE);
        NvU32 rs = nvrm_transfer_rm_memory(hClient, hMemory, 0, &got,
                                           sizeof got, NV_TRUE);
        bool ok = ws == 0 && rs == 0 && got == want;
        if (ok) r->vram_writeback_ok = true;
        kinfo("nvkms-test", "head%u CE VRAM round-trip wrote %#x read %#x (w=%#x r=%#x) -> %s",
              active[i].head, want, got, ws, rs,
              ok ? "reaches mapped VRAM" : "INCOHERENT map (writes lost)");
    }
    active_count = count;
    return true;
}

typedef struct {
    NvBool compositor_supported;
    NvBool output_supported;
    NvU32 compositor;
    NvU32 output;
} visible_crc_t;

static void read_visible_crc(test_display_t *d, visible_crc_t *out)
{
    memset(out, 0, sizeof(*out));
    struct NvKmsKapiCrcs crcs;
    memset(&crcs, 0, sizeof(crcs));
    if (!kapi.getCRC32 || !kapi.getCRC32(test_device, d->head, &crcs))
        return;
    out->compositor_supported = crcs.compositorCrc32.supported;
    out->output_supported = crcs.outputCrc32.supported;
    out->compositor = crcs.compositorCrc32.value;
    out->output = crcs.outputCrc32.value;
}

static NvBool display_slot_fb(test_display_t *d, NvU32 slot, NvU64 *fb)
{
    const kapi_device_rm_prefix_t *dev =
        (const kapi_device_rm_prefix_t *)test_device;
    NvU32 memory = ((const kapi_memory_rm_prefix_t *)d->memory[slot])->hRmHandle;
    kapi_phys_attr_params_t attr;
    memset(&attr, 0, sizeof(attr));
    NvU32 status = nvrm_host_control_object(dev->hRmClient, memory,
                                             NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,
                                             &attr, sizeof(attr));
    if (status != 0 || attr.mem_aperture != NV0041_APERTURE_VIDMEM ||
        attr.mem_offset == 0) {
        kerr("nvkms-accel", "head%u slot%u physical VRAM query failed: status=%#x aperture=%u offset=%#llx",
             d->head, slot, status, attr.mem_aperture,
             (unsigned long long)attr.mem_offset);
        return NV_FALSE;
    }
    *fb = attr.mem_offset;
    return NV_TRUE;
}

/* The 32x32 upload probe alone cannot prove the new source-free CE fills.
 * Sample corners, every coloured rectangle's last row, and both banners in
 * the actual NVKMS allocation, before its all-head publication. */
static NvBool verify_visible_2d_fill(test_display_t *d, NvU32 slot)
{
    const NvU32 w = d->mode.timings.hVisible, h = d->mode.timings.vVisible;
    if (w < 320u || h < 240u) return NV_FALSE;
    const NvU32 margin = w / 16u, gap = w / 64u;
    const NvU32 card_w = (w - 2u * margin - 5u * gap) / 6u;
    static const NvU32 colours[6] = {
        0xffff3b30u, 0xffff9500u, 0xffffcc00u,
        0xff34c759u, 0xff007affu, 0xffaf52deu,
    };
    struct { NvU32 x, y, want; } samples[12] = {
        {0, 0, 0xff101827u}, {w - 1u, 0, 0xff101827u},
        {0, h - 1u, 0xff101827u}, {w - 1u, h - 1u, 0xff101827u},
        {w / 8u + 1u, h / 16u + h / 8u - 1u, 0xff19d3ffu},
        {w / 8u + 1u, h * 13u / 16u + h / 8u - 1u, 0xff19d3ffu},
    };
    for (NvU32 i = 0; i < 6u; i++) {
        samples[6u + i].x = margin + i * (card_w + gap) + card_w / 2u;
        samples[6u + i].y = h * 5u / 16u + h / 4u - 1u;
        samples[6u + i].want = colours[i];
    }
    const kapi_device_rm_prefix_t *dev =
        (const kapi_device_rm_prefix_t *)test_device;
    NvU32 memory = ((const kapi_memory_rm_prefix_t *)d->memory[slot])->hRmHandle;
    for (NvU32 i = 0; i < 12u; i++) {
        NvU32 got = 0;
        NvU64 offset = (NvU64)samples[i].y * d->pitch + (NvU64)samples[i].x * 4u;
        NvU32 status = nvrm_transfer_rm_memory(dev->hRmClient, memory, offset,
                                               &got, sizeof(got), NV_TRUE);
        if (status || got != samples[i].want) {
            kerr("nvkms-accel", "CE constant-fill head%u sample%u (%u,%u)=%#x expected=%#x status=%#x",
                 d->head, i, samples[i].x, samples[i].y, got, samples[i].want, status);
            return NV_FALSE;
        }
    }
    kinfo("nvkms-accel", "CE constant-fill head%u PASS: corners, six colours, last rows, copy after remap",
          d->head);
    return NV_TRUE;
}

static NvBool prepare_visible_accel(nvkms_kapi_accel_result_t *r,
                                    NvBool three_d, NvU32 slot)
{
    NvU32 draw_mask = 0, pixel_mask = 0;
    NvU32 wanted_mask = 0;
    /* 2D verifies the unique pixel written by nv_chan_visible_2d_hw()'s exact
     * 32x32 desktop-damage probe, not the larger cyan banner below it. */
    const NvU32 expected = three_d ? 0xffff00ffu : 0xff31e6a8u;
    const char *name = three_d ? "visible 3D" : "visible 2D";

    for (NvU32 i = 0; i < active_count; i++) {
        test_display_t *d = &active[i];
        const NvU32 bit = 1u << d->head;
        wanted_mask |= bit;
        NvU64 fb = 0;
        if (!display_slot_fb(d, slot, &fb) ||
            !nv_chan_bind_scanout(fb, d->bytes, d->pitch,
                                  d->mode.timings.hVisible,
                                  d->mode.timings.vVisible)) {
            kerr("nvkms-accel", "%s could not bind head%u slot%u",
                 name, d->head, slot);
            continue;
        }
        NvU64 render_started = timer_now_us();
        int rendered = three_d ? nv_chan_visible_3d_hw()
                               : nv_chan_visible_2d_hw();
        NvU64 render_us = timer_now_us() - render_started;
        kinfo("nvkms-accel", "%s head%u GPU render/retire time: %llu us",
              name, d->head, (unsigned long long)render_us);
        if (rendered != 0) {
            kerr("nvkms-accel", "%s hardware render failed on head%u (%d)",
                 name, d->head, rendered);
            continue;
        }
        draw_mask |= bit;

        /* Verify a deterministic pixel in the exact NVKMS allocation, through
         * NVIDIA's official cross-client MemUtils transfer path. */
        NvU32 x = d->mode.timings.hVisible / 2u;
        NvU32 y = three_d ? d->mode.timings.vVisible / 2u
                          : d->mode.timings.vVisible / 8u;
        NvU64 offset = (NvU64)y * d->pitch + (NvU64)x * 4u;
        NvU32 got = 0;
        const kapi_device_rm_prefix_t *dev =
            (const kapi_device_rm_prefix_t *)test_device;
        NvU32 memory = ((const kapi_memory_rm_prefix_t *)d->memory[slot])->hRmHandle;
        NvU32 status = nvrm_transfer_rm_memory(dev->hRmClient, memory, offset,
                                               &got, sizeof(got), NV_TRUE);
        NvBool pixels_ok = status == 0 && got == expected;
        if (!three_d && pixels_ok)
            pixels_ok = verify_visible_2d_fill(d, slot);
        if (pixels_ok)
            pixel_mask |= bit;
        kinfo("nvkms-accel", "%s head%u pixel (%u,%u)=%#x expected=%#x transfer=%#x -> %s",
              name, d->head, x, y, got, expected, status,
              pixels_ok ? "PASS" : "FAIL");
    }

    if (three_d) {
        r->three_d_drawn_mask = draw_mask;
        r->three_d_pixel_mask = pixel_mask;
    } else {
        r->two_d_drawn_mask = draw_mask;
        r->two_d_pixel_mask = pixel_mask;
    }
    return (draw_mask & wanted_mask) == wanted_mask &&
           (pixel_mask & wanted_mask) == wanted_mask;
}

static NvBool show_visible_accel(nvkms_kapi_accel_result_t *r,
                                 NvBool three_d, NvU32 slot)
{
    visible_crc_t before[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
    NvU32 wanted_mask = r->active_heads_mask, crc_mask = 0;
    const char *name = three_d ? "visible 3D" : "visible 2D";
    /* Snapshot immediately BEFORE publication, not before preparing the back
     * surface: the preceding stage can change while that surface is prepared. */
    for (NvU32 i = 0; i < active_count; i++)
        read_visible_crc(&active[i], &before[i]);

    NvU64 flip_started = timer_now_us();
    if (!present_prepared_slot(active_count, slot, name))
        return NV_FALSE;
    kinfo("nvkms-accel", "%s prepared-scene switch: %llu us (no rendering or upload in switch)",
          name, (unsigned long long)(timer_now_us() - flip_started));

    /* Let at least 120 frames pass even at 240 Hz before sampling output. */
    timer_mdelay(500);
    for (NvU32 i = 0; i < active_count; i++) {
        test_display_t *d = &active[i];
        visible_crc_t after;
        read_visible_crc(d, &after);
        NvBool output_live = before[i].output_supported &&
                             after.output_supported &&
                             before[i].output != after.output;
        NvBool compositor_live = before[i].compositor_supported &&
                                 after.compositor_supported &&
                                 before[i].compositor != after.compositor;
        /* If the physical output CRC exists, it is the acceptance authority;
         * compositor is a fallback only on hardware without output CRC. */
        NvBool live = before[i].output_supported ? output_live
                                                 : compositor_live;
        if (live) crc_mask |= 1u << d->head;
        kinfo("nvkms-accel", "%s head%u CRC compositor %#x->%#x, output %#x->%#x -> %s",
              name, d->head, before[i].compositor, after.compositor,
              before[i].output, after.output, live ? "PASS" : "FAIL");
    }
    if (three_d)
        r->three_d_crc_mask = crc_mask;
    else
        r->two_d_crc_mask = crc_mask;

    return (crc_mask & wanted_mask) == wanted_mask;
}

static NvBool observe_visible_until(NvU64 deadline)
{
    /* Preserve the hardware-proven CE+SM keepalive while viewing a static
     * scene. A deadline includes time spent preparing the following scene,
     * instead of adding a second hidden pause before its flip. */
    while (timer_now_us() < deadline) {
        if (!nv_chan_render_keepalive()) {
            kerr("nvkms-accel", "render-channel keepalive failed during scene observation");
            return NV_FALSE;
        }
        NvU64 now = timer_now_us();
        if (now >= deadline) break;
        NvU64 remaining = deadline - now;
        if (remaining > 500000ull) remaining = 500000ull;
        /* Do not overshoot a scene deadline by one whole polling period. */
        if (remaining >= 1000ull) timer_mdelay((NvU32)(remaining / 1000ull));
        if (remaining % 1000ull) timer_udelay((NvU32)(remaining % 1000ull));
    }
    return NV_TRUE;
}

bool nvkms_kapi_run_visible_accel_test(nvkms_kapi_accel_result_t *r,
                                       bool post_codec)
{
    memset(r, 0, sizeof(*r));
    r->ran = true;
    r->post_codec = post_codec;
    r->active_displays = active_count;
    if (!test_device || active_count == 0) {
        r->failed_at = "no live NVKMS display state";
        return false;
    }
    for (NvU32 i = 0; i < active_count; i++)
        r->active_heads_mask |= 1u << active[i].head;

    NvU32 slot = active[0].front ^ 1u;
    kinfo("nvkms-accel", "%s visible all-head engine proof begins on heads %#x",
          post_codec ? "post-codec" : "pre-codec", r->active_heads_mask);
    r->two_d_pass = prepare_visible_accel(r, NV_FALSE, slot) &&
                    show_visible_accel(r, NV_FALSE, slot);
    if (!r->two_d_pass) {
        r->failed_at = "all-head visible 2D proof";
        return false;
    }

    /* The 2D image remains scanned out while ALL 3D back surfaces are rendered
     * and read back. Only the final all-head flip switches the visible image.
     * Five seconds of intentional 2D observation includes that preparation;
     * there is no blank frame or modeset between 2D and 3D. */
    NvU64 switch_deadline = timer_now_us() + 4500000ull;
    slot = active[0].front ^ 1u;
    if (!prepare_visible_accel(r, NV_TRUE, slot) ||
        !observe_visible_until(switch_deadline)) {
        r->failed_at = "3D back-surface preparation/2D observation";
        return false;
    }
    r->three_d_pass = show_visible_accel(r, NV_TRUE, slot);
    if (!r->three_d_pass) {
        r->failed_at = "all-head visible 3D proof";
        return false;
    }
    if (!observe_visible_until(timer_now_us() + 7500000ull)) {
        r->three_d_pass = false;
        r->failed_at = "3D observation keepalive";
        return false;
    }
    kinfo("nvkms-accel", "%s visible all-head 2D+3D proof PASS on heads %#x",
          post_codec ? "post-codec" : "pre-codec", r->active_heads_mask);
    return true;
}

/* ---------------------------------------------------------------- runtime
 *
 * NVKMS owns one native VRAM surface per head, while the existing desktop ABI
 * owns one logical CPU-addressable canvas.  This bridge is the missing piece
 * between those two contracts.  In extend mode it clips a damaged logical
 * rectangle into each monitor's tile; mirror sends the same logical picture to
 * every head; an only-output layout leaves all other heads black. */

static dl_mode_t boot_layout_mode(void)
{
    NvU32 mode = g_boot.display.display_mode;
    if (mode >= KB_DISPLAY_ONLY_BASE)
        return DL_ONLY_OUTPUT(mode - KB_DISPLAY_ONLY_BASE);
    if (mode == KB_DISPLAY_MIRROR) return DL_MIRROR;
    if (mode == KB_DISPLAY_ONLY_OTHER) return DL_ONLY_SECONDARY;
    return DL_EXTEND;
}

static NvBool runtime_select(test_display_t *d)
{
    NvU64 fb = 0;
    return display_slot_fb(d, d->front, &fb) &&
           nv_chan_bind_scanout(fb, d->bytes, d->pitch,
                                d->mode.timings.hVisible,
                                d->mode.timings.vVisible);
}

static NvBool clip_i32(s32 ax, s32 ay, s32 aw, s32 ah,
                       s32 bx, s32 by, s32 bw, s32 bh,
                       s32 *x, s32 *y, s32 *w, s32 *h)
{
    s32 x0 = ax > bx ? ax : bx;
    s32 y0 = ay > by ? ay : by;
    s32 x1 = ax + aw < bx + bw ? ax + aw : bx + bw;
    s32 y1 = ay + ah < by + bh ? ay + ah : by + bh;
    if (x1 <= x0 || y1 <= y0) return NV_FALSE;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return NV_TRUE;
}

bool nvkms_kapi_publish_runtime_framebuffer(void)
{
    if (runtime_ready) return true;
    if (!test_device || !active_count || active_count > DISPLAY_MAX_OUTPUTS)
        return false;

    nv_card_t *render_card = nv_chan_display_card();
    if (!render_card || render_card != test_card) {
        kerr("nvkms-runtime", "display and render channel belong to different GPUs");
        return false;
    }

    dl_output_size_t sizes[DISPLAY_MAX_OUTPUTS];
    NvU32 order[DISPLAY_MAX_OUTPUTS];
    memset(sizes, 0, sizeof(sizes));
    order[0] = runtime_primary_index < active_count ? runtime_primary_index : 0;
    NvU32 at = 1;
    for (NvU32 i = 0; i < active_count; i++)
        if (i != order[0]) order[at++] = i;
    for (NvU32 slot = 0; slot < active_count; slot++) {
        NvU32 i = order[slot];
        sizes[slot].width = (int)active[i].mode.timings.hVisible;
        sizes[slot].height = (int)active[i].mode.timings.vVisible;
    }
    runtime_mode = boot_layout_mode();
    dl_layout_t ordered_layout;
    memset(&ordered_layout, 0, sizeof ordered_layout);
    display_layout_compute(sizes, (int)active_count, runtime_mode,
                           &ordered_layout);
    memset(&runtime_layout, 0, sizeof runtime_layout);
    runtime_layout.n = ordered_layout.n;
    runtime_layout.fb_width = ordered_layout.fb_width;
    runtime_layout.fb_height = ordered_layout.fb_height;
    for (NvU32 slot = 0; slot < active_count; slot++)
        runtime_layout.out[order[slot]] = ordered_layout.out[slot];
    if (runtime_layout.fb_width <= 0 || runtime_layout.fb_height <= 0)
        return false;

    NvU64 pitch64 = ((NvU64)(NvU32)runtime_layout.fb_width * 4u + 63u) & ~63ull;
    NvU64 bytes = pitch64 * (NvU32)runtime_layout.fb_height;
    if (pitch64 > 0xffffffffu || !bytes || bytes > 0xffffffffu)
        return false;
    runtime_fb_pitch = (NvU32)pitch64;
    runtime_fb_pixels = dma_alloc_pages((size_t)((bytes + PAGE_SIZE - 1u) /
                                                  PAGE_SIZE),
                                        &runtime_fb_phys);
    if (!runtime_fb_pixels) {
        kerr("nvkms-runtime", "could not allocate %llu-byte logical compositor framebuffer",
             (unsigned long long)bytes);
        return false;
    }

    /* Keep physically connected but layout-disabled heads visually dark.  A
     * later live-topology implementation can release their links entirely;
     * this already gives every generated only-output mode deterministic
     * content without risking a new modeset after the engine proofs. */
    for (NvU32 i = 0; i < active_count; i++) {
        if (runtime_layout.out[i].active) continue;
        if (!runtime_select(&active[i]) ||
            !nv_chan_fill_scanout(0, 0,
                                  (s32)active[i].mode.timings.hVisible,
                                  (s32)active[i].mode.timings.vVisible,
                                  0xff000000u)) {
            kerr("nvkms-runtime", "could not black layout-disabled head%u",
                 active[i].head);
            return false;
        }
    }

    if (nv_chan_display_card() != render_card ||
        !gpu_accel_select(render_card->pci_bus,
                          render_card->pci_slot, render_card->pci_func)) {
        kerr("nvkms-runtime", "no registered accelerator for the scanout channel's GPU");
        return false;
    }
    console_publish_framebuffer(runtime_fb_phys,
                                (NvU32)runtime_layout.fb_width,
                                (NvU32)runtime_layout.fb_height,
                                runtime_fb_pitch);
    mouse_set_bounds(runtime_layout.fb_width, runtime_layout.fb_height);
    runtime_ready = NV_TRUE;
    kinfo("nvkms-runtime", "%s desktop published as %dx%d across %u NVKMS head(s)",
          display_layout_mode_name(runtime_mode), runtime_layout.fb_width,
          runtime_layout.fb_height, active_count);
    return true;
}

bool nvkms_kapi_runtime_ready(void)
{
    return runtime_ready == NV_TRUE;
}

bool nvkms_kapi_runtime_matches_gpu(u8 bus, u8 slot, u8 func)
{
    return runtime_ready && test_card && test_card->pci_bus == bus &&
           test_card->pci_slot == slot && test_card->pci_func == func;
}

bool nvkms_kapi_runtime_selected(void)
{
    return runtime_ready && test_card &&
           gpu_accel_is_selected(test_card->pci_bus, test_card->pci_slot, test_card->pci_func);
}

#include "nvkms_display_watch.h"
#include "nvkms_display_plan.h"
#include "nvkms_display_prepare.h"
#include "nvkms_output_mode.h"

bool nvkms_kapi_display_output(u32 index, kdisplay_output_t *out)
{
    int watched=nvkms_watch_output(index,out);
    if(watched>=0)return watched!=0;
    if (!out || !runtime_ready || index >= active_count ||
        index >= KESTREL_NVKMS_MAX_TEST_DISPLAYS) return false;
    const test_display_t *d = &active[index];
    const dl_scanout_t *s = &runtime_layout.out[index];
    memset(out, 0, sizeof *out);
    out->version = KDISPLAY_INFO_VERSION;
    out->output_id = d->handle;
    out->connector_id = d->connector_handle;
    out->flags = KDISPLAY_CONNECTED | (s->active ? KDISPLAY_ENABLED : 0) |
                 (index == runtime_primary_index ? KDISPLAY_PRIMARY : 0) |
                 (display_modes[index].truncated ? KDISPLAY_MODES_TRUNCATED : 0);
    out->width = d->mode.timings.hVisible;
    out->height = d->mode.timings.vVisible;
    out->refresh_millihz = d->mode.timings.refreshRate;
    if (display_modes[index].handle == d->handle)
        out->mode_count = display_modes[index].count;
    out->x = s->src_x; out->y = s->src_y;
    out->logical_width = s->src_w; out->logical_height = s->src_h;
    out->group_mode = runtime_mode;
    /* Rotation is applied by the GPU compositor; physical scanout timing and
     * the NVKMS plane stay unrotated. */
    out->rotation_degrees = s->rotation;
    strlcpy(out->manufacturer, d->manufacturer, sizeof out->manufacturer);
    strlcpy(out->model, d->model, sizeof out->model);
    strlcpy(out->connector, d->connector_is_dp ? "DisplayPort" : "Other",
            sizeof out->connector);
    return true;
}

bool nvkms_kapi_display_mode(u32 index, u32 mode, kdisplay_mode_t *out)
{
    int watched=nvkms_watch_mode(index,mode,out);
    if(watched>=0)return watched!=0;
    if (!out || !runtime_ready || index >= active_count ||
        index >= KESTREL_NVKMS_MAX_TEST_DISPLAYS ||
        display_modes[index].handle != active[index].handle ||
        mode >= display_modes[index].count) return false;
    struct NvKmsKapiDisplayMode native;
    if (!resolve_display_mode(index, active[index].handle,
                              active[index].connector_handle, mode, &native))
        return false;
    *out = display_modes[index].modes[mode];
    out->width = native.timings.hVisible;
    out->height = native.timings.vVisible;
    out->refresh_millihz = native.timings.refreshRate;
    const struct NvKmsKapiDisplayMode *current = &active[index].mode;
    if (out->width == current->timings.hVisible &&
        out->height == current->timings.vVisible &&
        out->refresh_millihz == current->timings.refreshRate &&
        !!(out->flags & KDISPLAY_MODE_INTERLACED) == !!current->timings.flags.interlaced)
        out->flags |= KDISPLAY_MODE_CURRENT;
    return true;
}

bool nvkms_kapi_runtime_accel_selftest(void)
{
    if (!runtime_ready) return false;

    /* The former boot gate stopped at the test-to-runtime handoff.  Userland
     * began roughly 4.2 seconds later, drew one triangle, and its very next
     * 32x32 CAB5 upload killed the copy channel.  Reproduce that sequence and
     * delay here, before calling the desktop hardware-ready. */
    timer_mdelay(4500);

    static const NvU32 tri[3u * 8u] = {
        0x00000000u,0x3f19999au,0x3f000000u,0x3f800000u,
        0x3f800000u,0,0,0x3f800000u,
        0x3f0ccccdu,0xbf000000u,0x3f000000u,0x3f800000u,
        0,0x3f800000u,0,0x3f800000u,
        0xbf0ccccdu,0xbf000000u,0x3f000000u,0x3f800000u,
        0,0,0x3f800000u,0x3f800000u,
    };
    if (nvkms_kapi_runtime_draw((const float *)tri, 1u) != 1) {
        kerr("nvkms-runtime", "desktop-sequence gate: one-triangle GR draw failed after idle");
        return false;
    }

    const dl_scanout_t *tile = NULL;
    for (NvU32 i = 0; i < active_count; i++) {
        if (runtime_layout.out[i].active) {
            tile = &runtime_layout.out[i];
            break;
        }
    }
    if (!tile || tile->src_w < 128 || tile->src_h < 128) return false;
    s32 cx = tile->src_x + tile->src_w / 2;
    s32 cy = tile->src_y + tile->src_h / 2;
    NvU32 got = 0;
    if (!nvkms_kapi_runtime_read_pixel(cx, cy, &got) || got != 0xff555555u) {
        kerr("nvkms-runtime", "desktop-sequence gate: GR centre pixel %#x (expected 0xff555555)", got);
        return false;
    }

    static NvU32 patch[32u * 32u];
    for (NvU32 i = 0; i < 32u * 32u; i++) patch[i] = 0xff19d3ffu;
    s32 px = tile->src_x + 32;
    s32 py = tile->src_y + 32;
    if (!nvkms_kapi_runtime_present(patch, 32u, 32u, 32u, px, py) ||
        !nvkms_kapi_runtime_read_pixel(px + 16, py + 16, &got) ||
        got != 0xff19d3ffu) {
        kerr("nvkms-runtime", "desktop-sequence gate: 32x32 CAB5 present/readback failed (%#x)", got);
        return false;
    }

    /* These are the two FB_ACCEL operations advertised to the compositor.
     * The copy is same-head and therefore exercises the VRAM scratch path. */
    if (!nvkms_kapi_runtime_fill(px, py + 40, 32, 24, 0xffd04a72u) ||
        !nvkms_kapi_runtime_copy(px, py + 40, px + 40, py + 40,
                                 32, 24) ||
        !nvkms_kapi_runtime_read_pixel(px + 56, py + 52, &got) ||
        got != 0xffd04a72u) {
        kerr("nvkms-runtime", "desktop-sequence gate: CAB5 fill/copy/readback failed (%#x)", got);
        return false;
    }

    kinfo("nvkms-runtime", "desktop-sequence gate PASS after 4.5 s idle: GR triangle + 32x32 CAB5 present + fill/copy reached live scanout");
    nvkms_watch_start();
    return true;
}

static bool runtime_read_pixel_locked(s32 x, s32 y, u32 *pixel)
{
    if (!runtime_ready || !pixel || x < 0 || y < 0 ||
        x >= runtime_layout.fb_width || y >= runtime_layout.fb_height)
        return false;

    /* Resolve a logical desktop coordinate back to the NVKMS allocation that
     * is actually being scanned out.  This deliberately uses RM's memory
     * transfer path instead of the CPU shadow: otherwise a user-space test can
     * "verify" pixels the GPU never produced, or reject pixels that exist only
     * in VRAM after an SM draw. */
    for (NvU32 i = 0; i < active_count; i++) {
        const dl_scanout_t *s = &runtime_layout.out[i];
        if (!s->active || x < s->src_x || y < s->src_y ||
            x >= s->src_x + s->src_w || y >= s->src_y + s->src_h)
            continue;

        test_display_t *d = &active[i];
        NvU32 local_x = (NvU32)(((NvU64)(x - s->src_x) * s->out_w) /
                                (NvU32)s->src_w);
        NvU32 local_y = (NvU32)(((NvU64)(y - s->src_y) * s->out_h) /
                                (NvU32)s->src_h);
        if (local_x >= d->mode.timings.hVisible)
            local_x = d->mode.timings.hVisible - 1u;
        if (local_y >= d->mode.timings.vVisible)
            local_y = d->mode.timings.vVisible - 1u;

        const kapi_device_rm_prefix_t *dev =
            (const kapi_device_rm_prefix_t *)test_device;
        NvU32 memory = ((const kapi_memory_rm_prefix_t *)
                        d->memory[d->front])->hRmHandle;
        NvU64 offset = (NvU64)local_y * d->pitch + (NvU64)local_x * 4u;
        return nvrm_transfer_rm_memory(dev->hRmClient, memory, offset,
                                      pixel, sizeof(*pixel), NV_TRUE) == 0;
    }
    return false;
}

static void shadow_present(const NvU32 *pixels, NvU32 width, NvU32 height,
                           NvU32 stride, s32 x, s32 y)
{
    NvU32 dst_stride = runtime_fb_pitch / 4u;
    for (NvU32 row = 0; row < height; row++)
        memcpy(runtime_fb_pixels + (NvU64)(y + (s32)row) * dst_stride + x,
               pixels + (NvU64)row * stride, (size_t)width * 4u);
}

static bool runtime_present_locked(const u32 *pixels, u32 width, u32 height,
                                u32 source_stride, s32 x, s32 y)
{
    if (!runtime_ready || !pixels || !width || !height) return false;
    if (!source_stride) source_stride = width;
    if (source_stride < width || x < 0 || y < 0 ||
        (NvU64)(NvU32)x + width > (NvU32)runtime_layout.fb_width ||
        (NvU64)(NvU32)y + height > (NvU32)runtime_layout.fb_height)
        return false;

    shadow_present(pixels, width, height, source_stride, x, y);
    NvBool ok = NV_TRUE;
    NvBool warned_scale = NV_FALSE;
    for (NvU32 i = 0; i < active_count; i++) {
        dl_scanout_t *s = &runtime_layout.out[i];
        if (!s->active) continue;
        test_display_t *d = &active[i];

        if (runtime_mode == DL_MIRROR &&
            (s->src_w != s->out_w || s->src_h != s->out_h)) {
            /* The surface itself is native-sized.  Prepare a nearest-neighbour
             * image only for mismatched mirror panels, then let CAB5 perform
             * the visible transfer.  Equal-sized panels and every extend/only
             * layout stay entirely on the direct damage path. */
            NvU32 sw = (NvU32)runtime_layout.fb_width;
            NvU32 sh = (NvU32)runtime_layout.fb_height;
            NvU32 dw = d->mode.timings.hVisible;
            NvU32 dh = d->mode.timings.vVisible;
            NvU32 src_stride = runtime_fb_pitch / 4u;
            NvU32 dst_stride = d->pitch / 4u;
            for (NvU32 oy = 0; oy < dh; oy++) {
                const NvU32 *src = runtime_fb_pixels +
                    (NvU64)((NvU64)oy * sh / dh) * src_stride;
                NvU32 *dst = d->pixels + (NvU64)oy * dst_stride;
                for (NvU32 ox = 0; ox < dw; ox++)
                    dst[ox] = src[(NvU64)ox * sw / dw];
            }
            if (!runtime_select(d) ||
                !nv_chan_present_image(d->pixels, dw, dh, dst_stride, 0, 0))
                ok = NV_FALSE;
            warned_scale = NV_TRUE;
            continue;
        }

        s32 ix, iy, iw, ih;
        if (!clip_i32(x, y, (s32)width, (s32)height,
                      s->src_x, s->src_y, s->src_w, s->src_h,
                      &ix, &iy, &iw, &ih))
            continue;
        const NvU32 *src = pixels + (NvU64)(iy - y) * source_stride +
                           (NvU32)(ix - x);
        if (!runtime_select(d) ||
            !nv_chan_present_image(src, (NvU32)iw, (NvU32)ih,
                                   source_stride, ix - s->src_x,
                                   iy - s->src_y))
            ok = NV_FALSE;
    }
    if (warned_scale) {
        static NvBool once;
        if (!once) {
            once = NV_TRUE;
            kwarn("nvkms-runtime", "mismatched mirror modes use CPU nearest-neighbour preparation plus GPU CAB5 presentation");
        }
    }
    return ok == NV_TRUE;
}

static bool runtime_fill_locked(s32 x, s32 y, s32 w, s32 h, u32 colour)
{
    if (!runtime_ready || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        (NvU64)(NvU32)x + (NvU32)w > (NvU32)runtime_layout.fb_width ||
        (NvU64)(NvU32)y + (NvU32)h > (NvU32)runtime_layout.fb_height)
        return false;
    NvU32 stride = runtime_fb_pitch / 4u;
    for (s32 row = 0; row < h; row++)
        for (s32 col = 0; col < w; col++)
            runtime_fb_pixels[(NvU64)(y + row) * stride + x + col] = colour;

    NvBool ok = NV_TRUE;
    for (NvU32 i = 0; i < active_count; i++) {
        dl_scanout_t *s = &runtime_layout.out[i];
        if (!s->active) continue;
        s32 ix, iy, iw, ih;
        if (!clip_i32(x, y, w, h, s->src_x, s->src_y, s->src_w, s->src_h,
                      &ix, &iy, &iw, &ih)) continue;
        if (!runtime_select(&active[i]) ||
            !nv_chan_fill_scanout(ix - s->src_x, iy - s->src_y,
                                  iw, ih, colour))
            ok = NV_FALSE;
    }
    return ok == NV_TRUE;
}

static bool runtime_copy_locked(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h)
{
    if (!runtime_ready || sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
        w <= 0 || h <= 0 ||
        (NvU64)(NvU32)sx + (NvU32)w > (NvU32)runtime_layout.fb_width ||
        (NvU64)(NvU32)dx + (NvU32)w > (NvU32)runtime_layout.fb_width ||
        (NvU64)(NvU32)sy + (NvU32)h > (NvU32)runtime_layout.fb_height ||
        (NvU64)(NvU32)dy + (NvU32)h > (NvU32)runtime_layout.fb_height)
        return false;

    NvU32 stride = runtime_fb_pitch / 4u;
    if (dy > sy) {
        for (s32 row = h - 1; row >= 0; row--)
            memmove(runtime_fb_pixels + (NvU64)(dy + row) * stride + dx,
                    runtime_fb_pixels + (NvU64)(sy + row) * stride + sx,
                    (size_t)w * 4u);
    } else {
        for (s32 row = 0; row < h; row++)
            memmove(runtime_fb_pixels + (NvU64)(dy + row) * stride + dx,
                    runtime_fb_pixels + (NvU64)(sy + row) * stride + sx,
                    (size_t)w * 4u);
    }

    NvBool ok = NV_TRUE;
    for (NvU32 i = 0; i < active_count; i++) {
        dl_scanout_t *s = &runtime_layout.out[i];
        if (!s->active) continue;
        s32 ix, iy, iw, ih;
        if (!clip_i32(dx, dy, w, h, s->src_x, s->src_y, s->src_w, s->src_h,
                      &ix, &iy, &iw, &ih)) continue;
        s32 source_x = sx + (ix - dx);
        s32 source_y = sy + (iy - dy);
        NvBool same_head = source_x >= s->src_x && source_y >= s->src_y &&
                           source_x + iw <= s->src_x + s->src_w &&
                           source_y + ih <= s->src_y + s->src_h;
        if (!runtime_select(&active[i])) { ok = NV_FALSE; continue; }
        if (same_head) {
            if (!nv_chan_copy_scanout(source_x - s->src_x,
                                      source_y - s->src_y,
                                      ix - s->src_x, iy - s->src_y,
                                      iw, ih))
                ok = NV_FALSE;
        } else {
            const NvU32 *src = runtime_fb_pixels + (NvU64)iy * stride + ix;
            if (!nv_chan_present_image(src, (NvU32)iw, (NvU32)ih, stride,
                                       ix - s->src_x, iy - s->src_y))
                ok = NV_FALSE;
        }
    }
    return ok == NV_TRUE;
}

/* Native compositor presentation. Unlike runtime_present(), this neither
 * uploads CPU pixels nor maintains a software shadow. Call only under the
 * shared render submission lock, after ownership/requests are validated. */
bool nvkms_kapi_runtime_present_surface(u64 owner, u64 surface,
                                        s32 x, s32 y, s32 w, s32 h)
{
    nv_surface_info_t info;
    if (!runtime_ready || !nv_surface_info(owner, surface, &info) ||
        info.width != (u32)runtime_layout.fb_width ||
        info.height != (u32)runtime_layout.fb_height ||
        x < 0 || y < 0 || w <= 0 || h <= 0 ||
        (u64)(u32)x + (u32)w > info.width ||
        (u64)(u32)y + (u32)h > info.height) return false;
    struct {int x, y, w, h, visible;} clips[KESTREL_NVKMS_MAX_TEST_DISPLAYS];
    if (!active_count || active_count > KESTREL_NVKMS_MAX_TEST_DISPLAYS) return false;
    /* Preflight all geometry before changing any monitor. */
    for (u32 i = 0; i < active_count; i++) {
        const dl_scanout_t *s = &runtime_layout.out[i];
        clips[i].visible = display_layout_damage(s, x, y, w, h,
            &clips[i].x, &clips[i].y, &clips[i].w, &clips[i].h);
        if (clips[i].visible < 0) return false;
        if (s->active && ((u64)(u32)s->src_x + (u32)s->src_w > info.width ||
                          (u64)(u32)s->src_y + (u32)s->src_h > info.height ||
                          s->out_w != (int)active[i].mode.timings.hVisible ||
                          s->out_h != (int)active[i].mode.timings.vVisible))
            return false;
    }
    for (u32 i = 0; i < active_count; i++) {
        if (!clips[i].visible) continue;
        const dl_scanout_t *s = &runtime_layout.out[i];
        if (!runtime_select(&active[i]) ||
            !nv_surface_present(owner, surface, (u32)s->src_x, (u32)s->src_y,
                                  (u32)s->src_w, (u32)s->src_h,
                                  (u32)clips[i].x, (u32)clips[i].y,
                                  (u32)clips[i].w, (u32)clips[i].h,
                                  (u32)s->rotation)) return false;
    }
    return true;
}

static int runtime_draw_locked(const float *vertices, u32 triangles)
{
    if (!runtime_ready || !vertices || !triangles) return -1;
    for (NvU32 i = 0; i < active_count; i++) {
        if (!runtime_layout.out[i].active) continue;
        if (!runtime_select(&active[i]) ||
            nv_chan_draw_triangles(vertices, triangles) != (int)triangles)
            return -1;
    }
    return (int)triangles;
}

/* Keep the entire head-selection/staging/submit/fence sequence indivisible
 * with respect to another process, without spinning while RM/GSP needs CPU
 * service. The off-screen syscall bridge must take this same guard around its
 * transaction before that new interface is exposed to userland. */
bool nvkms_kapi_runtime_read_pixel(s32 x, s32 y, u32 *pixel)
{
    if (!nv_render_try_begin()) return false;
    bool ok = runtime_read_pixel_locked(x, y, pixel);
    nv_render_end();
    return ok;
}

bool nvkms_kapi_runtime_present(const u32 *pixels, u32 width, u32 height,
                                u32 stride, s32 x, s32 y)
{
    if (!nv_render_try_begin()) return false;
    bool ok = runtime_present_locked(pixels, width, height, stride, x, y);
    nv_render_end();
    return ok;
}

bool nvkms_kapi_runtime_fill(s32 x, s32 y, s32 w, s32 h, u32 colour)
{
    if (!nv_render_try_begin()) return false;
    bool ok = runtime_fill_locked(x, y, w, h, colour);
    nv_render_end();
    return ok;
}

bool nvkms_kapi_runtime_copy(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h)
{
    if (!nv_render_try_begin()) return false;
    bool ok = runtime_copy_locked(sx, sy, dx, dy, w, h);
    nv_render_end();
    return ok;
}

int nvkms_kapi_runtime_draw(const float *vertices, u32 triangles)
{
    if (!nv_render_try_begin()) return -1;
    int result = runtime_draw_locked(vertices, triangles);
    nv_render_end();
    return result;
}
