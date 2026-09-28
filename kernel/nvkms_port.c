/*
 * KestrelOS host boundary for NVIDIA's complete NVKMS 595.99.02 core.
 *
 * The supplied nv-modeset-kernel.o_binary is not a Linux module: it is the
 * relocatable, OS-neutral display state machine that NVIDIA's thin Linux
 * wrapper hosts.  It imports this small nvkms_* surface and sends every RM
 * operation through nvkms_call_rm().  Keeping that state machine intact is
 * essential for DP link training, DSC, IMP validation, multi-head ownership,
 * cursor/overlay channels and modeset rollback to happen in NVIDIA's tested
 * order rather than in Kestrel's former hand-written approximation.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "proc.h"
#include "time.h"
#include "nv.h"
#include "nvkms_port.h"

#undef ARRAY_LEN
#include "nvidia-modeset-os-interface.h"
#include "nv-kernel-rmapi-ops.h"

#define NVKMS_ERR_NOT_SUPPORTED 0x00000056u
#define NVKMS_ERR_NOT_READY     0x0000001eu

#define RPC_MAP_MEMORY_DMA      14u
#define RPC_UNMAP_MEMORY_DMA    15u
#define RPC_DUP_OBJECT          21u
#define RPC_ALLOC_MEMORY         4u

/* Current GSP wire structures from 595.99.02 g_sdk-structures.h.  Do not send
 * the public NVOS structs directly: NVOS46 gained flags2 in the host API while
 * the v2C_05 RPC ABI did not, so their trailing offsets intentionally differ. */
typedef struct {
    u32 hClient, hDevice, hDma, hMemory;
    u64 offset, length;
    u32 flags, kindOverride;
    u64 dmaOffset;
    u32 status;
} nvkms_rpc_map_dma_params_t;
typedef struct { nvkms_rpc_map_dma_params_t params; } nvkms_rpc_map_dma_t;

typedef struct {
    u32 hClient, hDevice, hDma, hMemory;
    u32 flags;
    u64 dmaOffset, size;
    u32 status;
} nvkms_rpc_unmap_dma_params_t;
typedef struct { nvkms_rpc_unmap_dma_params_t params; } nvkms_rpc_unmap_dma_t;

typedef struct {
    u32 hClient, hParent, hObject, hClientSrc, hObjectSrc, flags, status;
} nvkms_rpc_dup_params_t;
typedef struct { nvkms_rpc_dup_params_t params; } nvkms_rpc_dup_t;

_Static_assert(sizeof(nvkms_rpc_map_dma_params_t) == 56,
               "595.99.02 NVOS46 v2C_05 wire size");
_Static_assert(sizeof(nvkms_rpc_unmap_dma_params_t) == 48,
               "595.99.02 NVOS47 v2C_05 wire size");
_Static_assert(sizeof(nvkms_rpc_dup_params_t) == 28,
               "595.99.02 NVOS55 v03_00 wire size");

/* rpc_alloc_memory_v13_01 has a flexible PTE descriptor.  Kestrel's DMA
 * allocator returns one physically-contiguous run, so GSP-RM wants exactly
 * one PTE (the first physical page number), not one entry per 4 KiB page. */
typedef struct {
    u32 hClient, hDevice, hMemory, hClass, flags;
    u32 pteAdjust, format;
    u64 length;
    u32 pageCount;
    u32 pteDescHeader;           /* idr:2, reserved:14, length:16 */
    u32 pteAlign;
    u64 pte;
} nvkms_rpc_alloc_memory_t;
_Static_assert(sizeof(nvkms_rpc_alloc_memory_t) == 64,
               "595.99.02 alloc-memory body with one PTE");

static nv_card_t *host_card;
static nv_rm_t *host_rm;
static NvBool host_module_loaded;
/* Linux's NVKMS wrapper serializes nvKmsOpen/Close/Ioctl and every timer
 * callback with one global semaphore.  The binary core relies on that
 * contract; per-object locks inside the core are not a replacement. */
static volatile NvU32 nvkms_core_lock;
static void nvkms_core_lock_acquire(void)
{ while (__atomic_exchange_n(&nvkms_core_lock, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
static void nvkms_core_lock_release(void)
{ __atomic_store_n(&nvkms_core_lock, 0, __ATOMIC_RELEASE); }
extern NvBool nvrm_rmapi_op(void *raw);
extern NvBool nvrm_is_ready(void);
extern NvU32 nvrm_gpu_id(void);

#define NVKMS_RM_CLIENT 0xC1D000E0u
#define NVKMS_CLASS_MEMORY_SYSTEM 0x0000003eu

#define NVKMS_HOST_ALLOCS 128
typedef struct {
    NvBool used;
    u32 client, parent, handle, klass;
    void *cpu;
    u64 phys, bytes;
    size_t pages;
} nvkms_host_alloc_t;
static nvkms_host_alloc_t host_allocs[NVKMS_HOST_ALLOCS];

static nvkms_host_alloc_t *host_alloc_find(u32 client, u32 handle)
{
    for (u32 i = 0; i < NVKMS_HOST_ALLOCS; i++)
        if (host_allocs[i].used && host_allocs[i].client == client &&
            host_allocs[i].handle == handle) return &host_allocs[i];
    return NULL;
}

static nvkms_host_alloc_t *host_alloc_slot(void)
{
    for (u32 i = 0; i < NVKMS_HOST_ALLOCS; i++)
        if (!host_allocs[i].used) return &host_allocs[i];
    return NULL;
}

static u32 nvkms_alloc_param_size(u32 klass, const void *params)
{
    if (!params) return 0;
    switch (klass) {
    case 0x0080: return 56;   /* NV0080_ALLOC_PARAMETERS */
    case 0x2080: return 4;    /* NV2080_ALLOC_PARAMETERS */
    case 0x0002: return 32;   /* NV_CONTEXT_DMA_ALLOCATION_PARAMS */
    case 0x003e:              /* NV01_MEMORY_SYSTEM */
    case 0x0040: return 128;  /* NV_MEMORY_ALLOCATION_PARAMS */
    case 0x0070: return 24;   /* NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS */
    case 0x003f: return 40;   /* NV_OS_DESC_MEMORY_ALLOCATION_PARAMS */
    default:
        /* NVDisplay DMA channels share the published 40-byte
         * NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS layout across classes. */
        if ((klass & 0xffu) == 0x7du || (klass & 0xffu) == 0x7eu) return 40;
        /* Cursor immediate PIO channels use the 16-byte PIO layout. */
        if ((klass & 0xffu) == 0x7au) return 16;
        return 0;
    }
}

/* ----------------------------- exact Linux module-parameter defaults */
NvBool nvkms_test_fail_alloc_core_channel(enum FailAllocCoreChannelMethod method)
{ (void)method; return NV_FALSE; }
NvBool nvkms_conceal_vrr_caps(void)              { return NV_FALSE; }
NvBool nvkms_output_rounding_fix(void)            { return NV_TRUE; }
NvBool nvkms_disable_hdmi_frl(void)               { return NV_FALSE; }
NvBool nvkms_disable_vrr_memclk_switch(void)      { return NV_FALSE; }
NvBool nvkms_hdmi_deepcolor(void)                 { return NV_TRUE; }
NvBool nvkms_opportunistic_display_sync(void)     { return NV_TRUE; }
enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void)
{ return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE; }
NvBool nvkms_enable_overlay_layers(void)          { return NV_TRUE; }
NvBool nvkms_debug_logging(void)                  { return NV_FALSE; }
NvBool nvkms_kernel_supports_syncpts(void)        { return NV_FALSE; }

/* ------------------------------------------------ libc / allocation */
void *nvkms_alloc(size_t size, NvBool zero)
{ return zero ? kzalloc(size) : kmalloc(size); }
void nvkms_free(void *ptr, size_t size) { (void)size; kfree(ptr); }
void *nvkms_memset(void *p, NvU8 c, size_t n) { return memset(p, c, n); }
void *nvkms_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *nvkms_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
int nvkms_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
size_t nvkms_strlen(const char *s) { return strlen(s); }
int nvkms_strcmp(const char *a, const char *b) { return strcmp(a, b); }

NvU64 nvkms_get_usec(void) { return timer_now_us(); }

/* NVKMS' KAPI path passes kernel addresses; no user address is accepted by
 * Kestrel's initial integration. */
int nvkms_copyin(void *kptr, NvU64 address, size_t n)
{
    if (!kptr || !address) return -1;
    memcpy(kptr, (const void *)(NvUPtr)address, n);
    return 0;
}
int nvkms_copyout(NvU64 address, const void *kptr, size_t n)
{
    if (!kptr || !address) return -1;
    memcpy((void *)(NvUPtr)address, kptr, n);
    return 0;
}

int nvkms_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{ return vsnprintf(s, n, fmt, ap); }
int nvkms_snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return ret;
}
void nvkms_log(const int level, const char *gpu, const char *msg)
{
    const char *prefix = gpu ? gpu : "";
    if (level >= NVKMS_LOG_LEVEL_ERROR) kerr("nvkms", "%s%s", prefix, msg);
    else if (level == NVKMS_LOG_LEVEL_WARN) kwarn("nvkms", "%s%s", prefix, msg);
    else kinfo("nvkms", "%s%s", prefix, msg);
}

/* ------------------------------------------------ reference pointers */
struct nvkms_ref_ptr {
    volatile u32 refs;
    void *ptr;
};

struct nvkms_ref_ptr *nvkms_alloc_ref_ptr(void *ptr)
{
    struct nvkms_ref_ptr *r = kzalloc(sizeof *r);
    if (r) { r->refs = 1; r->ptr = ptr; }
    return r;
}
void nvkms_inc_ref(struct nvkms_ref_ptr *r)
{ if (r) __atomic_add_fetch(&r->refs, 1, __ATOMIC_SEQ_CST); }
void *nvkms_dec_ref(struct nvkms_ref_ptr *r)
{
    if (!r) return NULL;
    void *ptr = r->ptr;
    if (__atomic_sub_fetch(&r->refs, 1, __ATOMIC_SEQ_CST) == 0) kfree(r);
    return ptr;
}
void nvkms_free_ref_ptr(struct nvkms_ref_ptr *r)
{
    if (!r) return;
    r->ptr = NULL;
    (void)nvkms_dec_ref(r);
}

/* ------------------------------------------------ deferred timers
 * Linux runs these callbacks in a process-context workqueue while holding the
 * global NVKMS semaphore.  A dedicated Kestrel thread preserves both parts of
 * that contract and, unlike a synchronous pump from nvkms_usleep(), cannot
 * re-enter the core halfway through an ioctl. */
struct nvkms_timer_t {
    nvkms_timer_proc_t *proc;
    void *data;
    NvU32 value;
    NvU64 due_us;
    NvBool cancelled, complete, ref_ptr, auto_free;
    struct nvkms_timer_t *next;
};
static struct nvkms_timer_t *timers;

static nvkms_timer_handle_t *timer_add(nvkms_timer_proc_t *proc, void *data,
                                       NvU32 value, NvU64 usec,
                                       NvBool ref_ptr, NvBool auto_free)
{
    struct nvkms_timer_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->proc = proc; t->data = data; t->value = value;
    NvU64 now = timer_now_us();
    t->due_us = usec > ~0ull - now ? ~0ull : now + usec;
    t->ref_ptr = ref_ptr; t->auto_free = auto_free;
    bool irq = irq_save();
    /* Zero-delay callbacks become FIFO work in NVIDIA's Linux wrapper. Keep
     * equal deadlines in publication order instead of the old LIFO stack,
     * where a callback queuing another event could overtake older DP work. */
    struct nvkms_timer_t **link = &timers;
    while (*link && (*link)->due_us <= t->due_us) link = &(*link)->next;
    t->next = *link;
    *link = t;
    irq_restore(irq);
    return t;
}

nvkms_timer_handle_t *nvkms_alloc_timer(nvkms_timer_proc_t *proc,
        void *data, NvU32 value, NvU64 usec)
{ return timer_add(proc, data, value, usec, NV_FALSE, NV_FALSE); }

NvBool nvkms_alloc_timer_with_ref_ptr(nvkms_timer_proc_t *proc,
        struct nvkms_ref_ptr *ref, NvU32 value, NvU64 usec)
{
    nvkms_inc_ref(ref);
    if (!timer_add(proc, ref, value, usec, NV_TRUE, NV_TRUE)) {
        (void)nvkms_dec_ref(ref);
        return NV_FALSE;
    }
    return NV_TRUE;
}

void nvkms_free_timer(nvkms_timer_handle_t *handle)
{
    struct nvkms_timer_t *t = handle;
    if (!t) return;
    t->cancelled = NV_TRUE;
    if (t->complete) kfree(t);
}

void nvkms_host_run_timers(void)
{
    for (;;) {
        struct nvkms_timer_t *due = NULL, **link;
        NvU64 now = timer_now_us();
        bool irq = irq_save();
        for (link = &timers; *link; link = &(*link)->next) {
            if ((*link)->cancelled || (*link)->due_us <= now) {
                due = *link;
                *link = due->next;
                break;
            }
        }
        irq_restore(irq);
        if (!due) break;

        nvkms_core_lock_acquire();
        if (!due->cancelled && due->proc) {
            void *arg = due->data;
            if (due->ref_ptr) {
                arg = nvkms_dec_ref(due->data);
                if (!arg) due->cancelled = NV_TRUE;
            }
            /* NULL data is valid for an ordinary timer. Only a reference
             * pointer whose target was destroyed suppresses its callback. */
            if (!due->cancelled) due->proc(arg, due->value);
        } else if (due->ref_ptr) {
            (void)nvkms_dec_ref(due->data);
        }
        /* Match NVIDIA's nvkms_kthread_q_callback: completion and final free
         * belong INSIDE the core lock. nvkms_free_timer() uses that same lock
         * and can otherwise free a completed handle while this worker still
         * inspects it. DP callbacks may also cancel their own handle. */
        due->complete = NV_TRUE;
        if (due->auto_free || due->cancelled) kfree(due);
        nvkms_core_lock_release();
    }
}

static void nvkms_timer_thread(void *unused)
{
    (void)unused;
    for (;;) {
        nvkms_host_run_timers();
        sched_sleep_ms(1);
    }
}

void nvkms_usleep(NvU64 usec)
{
    NvU64 until = timer_now_us() + usec;
    do {
        if (usec >= 1000) sched_yield();
        else timer_udelay((u32)usec);
    } while (timer_now_us() < until);
}
void nvkms_yield(void) { sched_yield(); }

/* ------------------------------------------------ mutex abstraction */
struct nvkms_sema_t { volatile u32 held; };
nvkms_sema_handle_t *nvkms_sema_alloc(void)
{ return kzalloc(sizeof(struct nvkms_sema_t)); }
void nvkms_sema_free(nvkms_sema_handle_t *s) { kfree(s); }
void nvkms_sema_down(nvkms_sema_handle_t *s)
{
    while (__atomic_exchange_n(&s->held, 1, __ATOMIC_ACQUIRE)) sched_yield();
}
void nvkms_sema_up(nvkms_sema_handle_t *s)
{ __atomic_store_n(&s->held, 0, __ATOMIC_RELEASE); }

/* -------------------------------------------- unsupported Tegra/user hooks */
NvBool nvkms_syncpt_op(enum NvKmsSyncPtOp op, NvKmsSyncPtOpParams *p)
{ (void)op; (void)p; return NV_FALSE; }
NvBool nvkms_fd_is_nvidia_chardev(int fd) { (void)fd; return NV_FALSE; }
void *nvkms_get_per_open_data(int fd) { (void)fd; return NULL; }
struct nvkms_per_open {
    void *data;
    struct NvKmsKapiDevice *device;
};

static struct nvkms_per_open *nvkms_event_pending;
static volatile NvU32 nvkms_event_running;
static volatile NvU32 nvkms_workers_started;

static void nvkms_event_thread(void *unused)
{
    (void)unused;
    for (;;) {
        __atomic_store_n(&nvkms_event_running, 1, __ATOMIC_RELEASE);
        struct nvkms_per_open *p = (struct nvkms_per_open *)
            __atomic_exchange_n(&nvkms_event_pending, NULL, __ATOMIC_ACQ_REL);
        if (!p) {
            __atomic_store_n(&nvkms_event_running, 0, __ATOMIC_RELEASE);
            sched_sleep_ms(1);
            continue;
        }
        /* HandleEventQueueChange calls back through ioctl_from_kapi(), which
         * takes the global core lock.  Never call it inline from NVKMS/RM BH:
         * Linux deliberately queues this exact transition for the same reason. */
        struct NvKmsKapiDevice *device =
            __atomic_load_n(&p->device, __ATOMIC_ACQUIRE);
        if (device) nvKmsKapiHandleEventQueueChange(device);
        __atomic_store_n(&nvkms_event_running, 0, __ATOMIC_RELEASE);
    }
}

void nvkms_event_queue_changed(nvkms_per_open_handle_t *handle, NvBool available)
{
    struct nvkms_per_open *p = (struct nvkms_per_open *)handle;
    if (available && p && p->device)
        __atomic_store_n(&nvkms_event_pending, p, __ATOMIC_RELEASE);
}
struct nvkms_backlight_device *nvkms_register_backlight(NvU32 gpu, NvU32 display,
        void *priv, NvU32 brightness)
{ (void)gpu; (void)display; (void)priv; (void)brightness; return NULL; }
void nvkms_unregister_backlight(struct nvkms_backlight_device *d) { (void)d; }

NvBool nvkms_open_gpu(NvU32 gpu, NvBool reset_aware)
{ (void)reset_aware; return host_card && nvrm_is_ready() && gpu == nvrm_gpu_id(); }
void nvkms_close_gpu(NvU32 gpu, NvBool reset_aware)
{ (void)gpu; (void)reset_aware; }
NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *info)
{
    if (!host_card || !nvrm_is_ready() || !info) return 0;
    memset(info, 0, sizeof *info);
    info->gpu_id = nvrm_gpu_id();
    info->pci_info.domain = 0;
    info->pci_info.bus = host_card->pci_bus;
    info->pci_info.slot = host_card->pci_slot;
    info->pci_info.function = host_card->pci_func;
    info->os_device_ptr = host_card;
    return 1;
}
NvBool nvkms_allow_write_combining(void) { return NV_TRUE; }

struct nvkms_per_open *nvkms_open_from_kapi(struct NvKmsKapiDevice *device)
{
    /* Refuse activation while the RM serializer gate is incomplete. */
    if (!host_card || !nvrm_is_ready()) return NULL;
    struct nvkms_per_open *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    p->device = device;
    nvkms_core_lock_acquire();
    p->data = nvKmsOpen(0, NVKMS_CLIENT_KERNEL_SPACE,
                        (nvkms_per_open_handle_t *)p);
    nvkms_core_lock_release();
    if (!p->data) { kfree(p); return NULL; }
    return p;
}
void nvkms_close_from_kapi(struct nvkms_per_open *p)
{
    if (!p) return;
    nvkms_core_lock_acquire();
    if (p->data) nvKmsClose(p->data);
    p->data = NULL;
    nvkms_core_lock_release();
    __atomic_store_n(&p->device, NULL, __ATOMIC_RELEASE);
    /* Flush the one-item coalescing event queue before freeing the per-open.
     * Repeat because the worker can move p from pending to running between
     * either individual observation.  Clearing device first makes a worker
     * that already owns p harmless; waiting for running==0 makes it finished. */
    for (;;) {
        struct nvkms_per_open *expected = p;
        (void)__atomic_compare_exchange_n(&nvkms_event_pending, &expected, NULL,
                                          NV_FALSE, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE);
        while (__atomic_load_n(&nvkms_event_running, __ATOMIC_ACQUIRE))
            sched_yield();
        if (__atomic_load_n(&nvkms_event_pending, __ATOMIC_ACQUIRE) != p)
            break;
    }
    kfree(p);
}
NvBool nvkms_ioctl_from_kapi(struct nvkms_per_open *p, NvU32 cmd,
                             void *params, const size_t size)
{
    if (!p || !p->data || !params) return NV_FALSE;
    nvkms_core_lock_acquire();
    NvBool ok = p->data ?
        nvKmsIoctl(p->data, cmd, (NvU64)(NvUPtr)params, size) : NV_FALSE;
    nvkms_core_lock_release();
    return ok;
}
NvBool nvkms_ioctl_from_kapi_try_pmlock(struct nvkms_per_open *p, NvU32 cmd,
                                        void *params, const size_t size)
{ return nvkms_ioctl_from_kapi(p, cmd, params, size); }

/* ---------------------------------------------------------- RM bridge */
void nvkms_call_rm(void *raw)
{
    nvidia_kernel_rmapi_ops_t *op = raw;
    if (!op) return;

    /* The matching NVIDIA host RM owns object descriptors, BAR/USERD maps,
     * local video memory and the GSP split.  Its native dispatcher implements
     * all ten NVKMS operations; the historical serializer below is retained
     * only as a pre-init error path and is never used after adapter bring-up. */
    if (nvrm_rmapi_op(raw)) return;

    if (!host_card || !host_rm) {
        switch (op->op) {
        case NV01_FREE: op->params.free.status = NVKMS_ERR_NOT_READY; break;
        case NV01_ALLOC_MEMORY: op->params.allocMemory64.status = NVKMS_ERR_NOT_READY; break;
        case NV04_ALLOC: op->params.alloc.status = NVKMS_ERR_NOT_READY; break;
        case NV04_VID_HEAP_CONTROL:
            if (op->params.pVidHeapControl) op->params.pVidHeapControl->status = NVKMS_ERR_NOT_READY;
            break;
        case NV04_MAP_MEMORY: op->params.mapMemory.status = NVKMS_ERR_NOT_READY; break;
        case NV04_UNMAP_MEMORY: op->params.unmapMemory.status = NVKMS_ERR_NOT_READY; break;
        case NV04_MAP_MEMORY_DMA: op->params.mapMemoryDma.status = NVKMS_ERR_NOT_READY; break;
        case NV04_UNMAP_MEMORY_DMA: op->params.unmapMemoryDma.status = NVKMS_ERR_NOT_READY; break;
        case NV04_DUP_OBJECT: op->params.dupObject.status = NVKMS_ERR_NOT_READY; break;
        }
        return;
    }

    switch (op->op) {
    case NV01_FREE: {
        NVOS00_PARAMETERS *p = &op->params.free;
        nvkms_host_alloc_t *a = host_alloc_find(p->hRoot, p->hObjectOld);
        p->status = nv_rm_free(host_card, host_rm, p->hObjectParent, p->hObjectOld)
                  ? 0 : nv_last_control_status;
        if (p->status == 0 && a) {
            dma_free_pages(a->cpu, a->pages);
            memset(a, 0, sizeof *a);
        }
        break;
    }
    case NV01_ALLOC_MEMORY: {
        NVOS02_PARAMETERS *p = &op->params.allocMemory64;

        /* The only NV01_ALLOC_MEMORY class reachable from NVKMS 595.99.02 is
         * NV01_MEMORY_SYSTEM: push buffers, progress semaphores and notifiers.
         * Local/virtual memory uses NV04_ALLOC and remains owned by RM. */
        if (p->hClass != NVKMS_CLASS_MEMORY_SYSTEM || p->limit == ~(NvU64)0) {
            p->status = NVKMS_ERR_NOT_SUPPORTED;
            break;
        }
        u64 bytes = p->limit + 1;
        if (!bytes || bytes > (64ull << 20) ||
            host_alloc_find(p->hRoot, p->hObjectNew)) {
            p->status = 0x0000001fu; /* NV_ERR_INVALID_ARGUMENT */
            break;
        }
        size_t pages = (size_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        u64 phys = 0;
        void *cpu = dma_alloc_pages(pages, &phys);
        nvkms_host_alloc_t *slot = host_alloc_slot();
        if (!cpu || !slot) {
            if (cpu) dma_free_pages(cpu, pages);
            p->status = 0x00000051u; /* NV_ERR_NO_MEMORY */
            break;
        }

        nvkms_rpc_alloc_memory_t q;
        memset(&q, 0, sizeof q);
        q.hClient = p->hRoot;
        q.hDevice = p->hObjectParent;
        q.hMemory = p->hObjectNew;
        q.hClass = p->hClass;
        q.flags = p->flags;
        q.length = (u64)pages * PAGE_SIZE;
        q.pageCount = 1;             /* contiguous memdesc PTE array size */
        q.pteDescHeader = 1u << 16;  /* idr NONE, one PTE */
        q.pte = phys >> 12;

        nvkms_rpc_alloc_memory_t answer;
        u32 got = 0;
        if (!nv_rm_rpc_raw(host_card, host_rm, RPC_ALLOC_MEMORY,
                           &q, sizeof q, &answer, sizeof answer, &got)) {
            dma_free_pages(cpu, pages);
            p->status = NVKMS_ERR_NOT_READY;
            break;
        }
        /* ALLOC_MEMORY has no status in its body; NVIDIA checks rpc_result in
         * the common 32-byte header.  nv_gsp_rpc_receive preserves it. */
        if (nv_gsp_last_rpc_result != 0) {
            dma_free_pages(cpu, pages);
            p->status = nv_gsp_last_rpc_result;
            break;
        }

        memset(slot, 0, sizeof *slot);
        slot->used = NV_TRUE;
        slot->client = p->hRoot;
        slot->parent = p->hObjectParent;
        slot->handle = p->hObjectNew;
        slot->klass = p->hClass;
        slot->cpu = cpu;
        slot->phys = phys;
        slot->bytes = (u64)pages * PAGE_SIZE;
        slot->pages = pages;
        p->pMemory = cpu;
        p->limit = slot->bytes - 1;
        p->status = 0;
        break;
    }
    case NV04_MAP_MEMORY: {
        NVOS33_PARAMETERS *p = &op->params.mapMemory;
        nvkms_host_alloc_t *a = host_alloc_find(p->hClient, p->hMemory);
        if (!a || p->offset > a->bytes || p->length > a->bytes - p->offset) {
            /* MMIO channel and VRAM mappings require NVIDIA host RM's local
             * memory descriptors; never manufacture an address for them. */
            p->status = a ? 0x0000001fu : NVKMS_ERR_NOT_SUPPORTED;
        } else {
            p->pLinearAddress = (u8 *)a->cpu + p->offset;
            p->status = 0;
        }
        break;
    }
    case NV04_UNMAP_MEMORY: {
        NVOS34_PARAMETERS *p = &op->params.unmapMemory;
        nvkms_host_alloc_t *a = host_alloc_find(p->hClient, p->hMemory);
        u64 first = a ? (u64)(NvUPtr)a->cpu : 0;
        u64 address = (u64)(NvUPtr)p->pLinearAddress;
        p->status = (a && address >= first && address < first + a->bytes)
                  ? 0 : NVKMS_ERR_NOT_SUPPORTED;
        break;
    }
    case NV04_CONTROL: {
        NVOS54_PARAMETERS *p = &op->params.control;
        void *params = (void *)(NvUPtr)p->params;
        u32 got = 0;
        p->status = nv_rm_control(host_card, host_rm, p->hObject, p->cmd,
                                  params, p->paramsSize, params, p->paramsSize,
                                  &got) ? 0 : nv_last_control_status;
        break;
    }
    case NV04_ALLOC: {
        NVOS64_PARAMETERS *p = &op->params.alloc;

        /* NVIDIA's NVKMS module-load call asks RM to generate its root handle:
         * hObjectNew is zero and pAllocParms points to one NvHandle output,
         * not an NV0000 allocation struct.  GSP's wire ABI wants a fixed root
         * and the full 108-byte kernel-client structure, so translate it. */
        if (p->hClass == NV01_ROOT && p->hObjectNew == 0) {
            struct {
                u32 hClient, processId;
                char processName[100];
            } __attribute__((packed)) root;
            memset(&root, 0, sizeof root);
            root.hClient = NVKMS_RM_CLIENT;
            root.processId = 0xffffffffu;
            u32 saved = host_rm->client;
            host_rm->client = NVKMS_RM_CLIENT;
            bool ok = nv_rm_alloc(host_card, host_rm, 0, NVKMS_RM_CLIENT,
                                  NV01_ROOT, &root, sizeof root);
            host_rm->client = saved;
            if (ok) {
                p->hRoot = p->hObjectNew = NVKMS_RM_CLIENT;
                if (p->pAllocParms)
                    *(u32 *)(NvUPtr)p->pAllocParms = NVKMS_RM_CLIENT;
                p->status = 0;
            } else p->status = nv_last_alloc_status;
            break;
        }

        u32 bytes = p->paramsSize;
        void *params = (void *)(NvUPtr)p->pAllocParms;
        if (!bytes) bytes = nvkms_alloc_param_size(p->hClass, params);
        if (params && !bytes) {
            kwarn("nvkms", "holding unknown RM alloc class %#x: no exact parameter size",
                  p->hClass);
            p->status = NVKMS_ERR_NOT_SUPPORTED;
            break;
        }
        u32 saved = host_rm->client;
        host_rm->client = p->hRoot;
        bool ok = nv_rm_alloc(host_card, host_rm, p->hObjectParent,
                              p->hObjectNew, p->hClass, params, bytes);
        host_rm->client = saved;
        p->status = ok ? 0 : nv_last_alloc_status;
        break;
    }
    case NV04_MAP_MEMORY_DMA: {
        NVOS46_PARAMETERS *p = &op->params.mapMemoryDma;
        nvkms_rpc_map_dma_t q = { .params = {
            .hClient = p->hClient, .hDevice = p->hDevice,
            .hDma = p->hDma, .hMemory = p->hMemory,
            .offset = p->offset, .length = p->length,
            .flags = p->flags, .kindOverride = p->kindOverride,
            .dmaOffset = p->dmaOffset,
        }};
        nvkms_rpc_map_dma_t answer;
        u32 got = 0;
        if (!nv_rm_rpc_raw(host_card, host_rm, RPC_MAP_MEMORY_DMA,
                           &q, sizeof q, &answer, sizeof answer, &got) ||
            got < sizeof answer) {
            p->status = NVKMS_ERR_NOT_READY;
        } else {
            p->dmaOffset = answer.params.dmaOffset;
            p->status = answer.params.status;
        }
        break;
    }
    case NV04_UNMAP_MEMORY_DMA: {
        NVOS47_PARAMETERS *p = &op->params.unmapMemoryDma;
        nvkms_rpc_unmap_dma_t q = { .params = {
            .hClient = p->hClient, .hDevice = p->hDevice,
            .hDma = p->hDma, .hMemory = p->hMemory,
            .flags = p->flags, .dmaOffset = p->dmaOffset, .size = p->size,
        }};
        nvkms_rpc_unmap_dma_t answer;
        u32 got = 0;
        if (!nv_rm_rpc_raw(host_card, host_rm, RPC_UNMAP_MEMORY_DMA,
                           &q, sizeof q, &answer, sizeof answer, &got) ||
            got < sizeof answer) p->status = NVKMS_ERR_NOT_READY;
        else p->status = answer.params.status;
        break;
    }
    case NV04_DUP_OBJECT: {
        NVOS55_PARAMETERS *p = &op->params.dupObject;
        nvkms_rpc_dup_t q = { .params = {
            .hClient = p->hClient, .hParent = p->hParent,
            .hObject = p->hObject, .hClientSrc = p->hClientSrc,
            .hObjectSrc = p->hObjectSrc, .flags = p->flags,
        }};
        nvkms_rpc_dup_t answer;
        u32 got = 0;
        if (!nv_rm_rpc_raw(host_card, host_rm, RPC_DUP_OBJECT,
                           &q, sizeof q, &answer, sizeof answer, &got) ||
            got < sizeof answer) p->status = NVKMS_ERR_NOT_READY;
        else {
            p->hObject = answer.params.hObject;
            p->status = answer.params.status;
        }
        break;
    }
    default:
        /* Anything left here remains an explicit release-gate failure. */
        switch (op->op) {
        case NV04_VID_HEAP_CONTROL:
            if (op->params.pVidHeapControl) op->params.pVidHeapControl->status = NVKMS_ERR_NOT_SUPPORTED;
            break;
        }
        break;
    }
}

void nvkms_host_attach(nv_card_t *card, nv_rm_t *rm)
{
    host_card = card;
    host_rm = rm;
    kinfo("nvkms", "NVIDIA 595.99.02 NVKMS core host attached; activation held "
                    "until all ten RM operations pass the offline bridge gate");
}

bool nvkms_host_attach_full(nv_card_t *card)
{
    if (!card || !nvrm_is_ready()) return false;
    host_module_loaded = NV_FALSE;
    host_card = card;
    host_rm = NULL;
    if (!__atomic_exchange_n(&nvkms_workers_started, 1, __ATOMIC_ACQ_REL)) {
        if (kthread_create("nvkms-timer", nvkms_timer_thread, NULL) < 0 ||
            kthread_create("nvkms-event", nvkms_event_thread, NULL) < 0) {
            kerr("nvkms", "could not start NVKMS timer/event process-context workers");
            host_card = NULL;
            return false;
        }
    }
    nvkms_core_lock_acquire();
    if (!nvKmsModuleLoad()) {
        nvkms_core_lock_release();
        host_card = NULL;
        kerr("nvkms", "NVIDIA NVKMS module load failed after host RM init");
        return false;
    }
    nvkms_core_lock_release();
    host_module_loaded = NV_TRUE;
    kinfo("nvkms", "NVIDIA 595.99.02 NVKMS loaded over native host RM dispatcher");
    return true;
}

bool nvkms_host_link_ready(void)
{
    return host_module_loaded && host_card && nvrm_is_ready();
}

nv_card_t *nvkms_host_card(void)
{
    return nvkms_host_link_ready() ? host_card : NULL;
}
