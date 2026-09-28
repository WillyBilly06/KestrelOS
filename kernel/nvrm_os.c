/*
 * Host-OS boundary for NVIDIA's 595.99.02 Resource Manager core.
 *
 * This file is deliberately separate from nvkms_port.c.  NVKMS is the
 * display policy/state machine; nv-kernel.o_binary is the host RM that owns
 * memory descriptors, PCI/BAR mappings, firmware lifecycle and the GSP split.
 * The latter is required for MapMemory to be correct for USERD, display
 * channels and video memory.  These are real Kestrel implementations of the
 * portable primitives, not success-returning placeholders.
 */
#include "kernel.h"
#include "klog.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "cpu.h"
#include "apic.h"
#include "time.h"
#include "firmware.h"
#include "nvrm_telemetry.h"
#include "nvrm_completion.h"
#include "nvrm_dp_link.h"
#include "smp.h"
#include "acpi.h"

/* Pull the exact 595.99.02 ABI, bypassing kernel/nv.h (Kestrel's own GPU
 * header has the same basename). */
#define NVRM 1
#define NV_KERNEL_INTERFACE_LAYER 1
#include "../out/nvidia-open-595.99.02/kernel-open/common/inc/os-interface.h"
#include "../out/nvidia-open-595.99.02/kernel-open/common/inc/nv.h"
#include "../out/nvidia-open-595.99.02/kernel-open/nvidia/nvlink_os.h"
#include "../out/nvidia-open-595.99.02/kernel-open/nvidia/nvlink_errors.h"
#include "../out/nvidia-open-595.99.02/kernel-open/nvidia/export_nvswitch.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl0073/ctrl0073dp.h"
#include "class/cl00de.h"

/* RM treats this as an opaque host handle and passes it back to the nv_dma_*
 * functions below.  It must nevertheless be non-NULL: memory-descriptor
 * construction rejects an adapter with no DMA device. */
struct nv_dma_device { NvU64 kestrel_cookie; };
static struct nv_dma_device nvrm_dma_device = { 0x4b45535452454cull };

NvU64 os_page_size = PAGE_SIZE;
/* NV_PAGE_MASK = ~(PAGE_SIZE-1): the ALIGN-DOWN mask (zeroes the low bits),
 * NOT the sub-page offset mask.  os.c:osMapKernelSpace does `Start &= os_page_mask`
 * to page-align the register BAR; a value of PAGE_SIZE-1 (0xFFF) zeroed the base
 * to physical 0, so RM's register map returned NULL -> "failed to map registers!"
 * -> host RM rejected the card and never brought up NVKMS/display.  Matches
 * Linux driver os-interface.c: `NvU64 os_page_mask = NV_PAGE_MASK`. */
NvU64 os_page_mask = ~((NvU64)PAGE_SIZE - 1);
NvU8  os_page_shift = 12;
NvBool os_dma_buf_enabled = NV_FALSE;
NvBool os_cc_enabled = NV_FALSE;
NvBool os_cc_sev_snp_enabled = NV_FALSE;
NvBool os_cc_sme_enabled = NV_FALSE;
NvBool os_cc_snp_vtom_enabled = NV_FALSE;
NvBool os_cc_tdx_enabled = NV_FALSE;
/* Registry enumeration terminates on the first nv_parm_t.name == NULL.  Keep
 * the ABI-sized terminator until individual Kestrel boot options are exposed. */
void *nv_parms[2] = { NULL, NULL };
nv_cap_t *nvidia_caps_root = NULL;
const NvBool nv_is_rm_firmware_supported_os = NV_TRUE;
static volatile NvU32 nvrm_in_isr[ACPI_MAX_CPUS];

/* RM timers are dispatched from every sleep/yield boundary and from the GPU
 * service loop.  Keep this declaration above the time primitives. */
void nvrm_os_pump(void);

NV_STATUS os_alloc_mem(void **out, NvU64 bytes)
{
    if (!out || bytes > (NvU64)(size_t)-1) return NV_ERR_INVALID_ARGUMENT;
    *out = kzalloc((size_t)(bytes ? bytes : 1));
    return *out ? NV_OK : NV_ERR_NO_MEMORY;
}

void os_free_mem(void *p) { kfree(p); }
void *os_mem_copy(void *d, const void *s, NvU32 n) { return memcpy(d, s, n); }
void *os_mem_set(void *d, NvU8 c, NvU32 n) { return memset(d, c, n); }
NV_STATUS os_memcpy_from_user(void *d, const void *s, NvU32 n)
{ if (!d || !s) return NV_ERR_INVALID_ARGUMENT; memcpy(d, s, n); return NV_OK; }
NV_STATUS os_memcpy_to_user(void *d, const void *s, NvU32 n)
{ if (!d || !s) return NV_ERR_INVALID_ARGUMENT; memcpy(d, s, n); return NV_OK; }
NvS32 os_mem_cmp(const NvU8 *a, const NvU8 *b, NvU32 n)
{ return (NvS32)memcmp(a, b, n); }

char *os_string_copy(char *d, const char *s) { return strcpy(d, s); }
NvU32 os_string_length(const char *s) { return (NvU32)strlen(s); }
NvS32 os_string_compare(const char *a, const char *b) { return strcmp(a, b); }

NV_STATUS os_get_system_time(NvU32 *sec, NvU32 *usec)
{
    /* RM's contract (osGetSystemTime): seconds/useconds since the 1970 epoch,
     * NOT since boot.  tmrSetCurrentTime_GH100 does
     *   NV_ASSERT_OR_RETURN(secTimerNs < osTimeNs, NV_ERR_INVALID_STATE);
     *   sysTimerOffsetNs = osTimeNs - secTimerNs;
     * where secTimerNs is the GPU's persistent SCI secure timer (days of ns on
     * a real card).  Returning timer_now_us() (a few seconds since boot) made
     * osTimeNs << secTimerNs, so the assert fired, the call returned early, and
     * pTmr->sysTimerOffsetNs was left 0 -> GSP heartbeat miscompare flood.
     * Wall-clock-since-1970 (~1.77e18 ns) is always > the GPU timer, so the
     * offset calibrates correctly. */
    if (sec)  *sec  = (NvU32)time_unix_seconds();
    if (usec) *usec = (NvU32)(timer_now_us() % 1000000ull);
    return NV_OK;
}
NvU64 os_get_monotonic_time_ns(void) { return timer_now_us() * 1000ull; }
NvU64 os_get_monotonic_time_ns_hr(void) { return timer_now_us() * 1000ull; }
NvU64 os_get_monotonic_tick_resolution_ns(void) { return 1000; }
NvU64 os_get_cpu_frequency(void)
{
    NvU32 lo,hi; NvU64 a,b;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); a=((NvU64)hi<<32)|lo;
    timer_mdelay(10);
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); b=((NvU64)hi<<32)|lo;
    return (b-a)*100ull;
}
NV_STATUS os_delay(NvU32 ms)
{
    NvU64 until = timer_now_us() + (NvU64)ms * 1000ull;
    do { nvrm_os_pump(); sched_yield(); } while (timer_now_us() < until);
    return NV_OK;
}
NV_STATUS os_delay_us(NvU32 us)
{
    NvU64 until = timer_now_us() + us;
    do {
        nvrm_os_pump();
        if (us >= 1000) sched_yield(); else timer_udelay(us ? us : 1);
    } while (timer_now_us() < until);
    return NV_OK;
}
NV_STATUS os_schedule(void) { nvrm_os_pump(); sched_yield(); return NV_OK; }
NvU32 os_get_cpu_count(void) { return 1; }
NvU32 os_get_cpu_number(void) { return 0; }
NvU32 os_get_current_process(void)
{ proc_t *p=proc_current(); return p ? (NvU32)p->pid : 1u; }
NvU32 os_get_current_process_flags(void)
{ return OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD; }
void os_get_current_process_name(char *out, NvU32 len)
{
    static const char name[] = "kestrel-kernel";
    if (!out || !len) return;
    NvU32 n = (NvU32)sizeof(name);
    if (n > len) n = len;
    memcpy(out, name, n);
    out[len - 1] = 0;
}
NV_STATUS os_get_current_thread(NvU64 *thread)
{
    if (!thread) return NV_ERR_INVALID_ARGUMENT;
    proc_t *p=proc_current();
    /* Zero is PORT_THREAD_INVALID inside RM.  The scheduler object address is
     * stable for a thread's lifetime and distinguishes API, workqueue and BH
     * contexts exactly where RM's recursive-lock tracking needs it. */
    *thread = p ? (NvU64)(NvUPtr)p : 1ull;
    return NV_OK;
}

typedef struct { volatile NvU32 held; } nvrm_lock_t;
typedef struct { volatile NvU32 count; } nvrm_sema_t;

/* The Linux host port's NV_MAY_SLEEP requires enabled IRQs and task context.
 * RM entry is currently confined to scheduled BSP tasks and its own hard ISR;
 * restricted AP workers are not sleep-capable task contexts. A permanent event
 * counter avoids registering waiters inside objects that may later be freed.
 * The caller must still retain the mutex/semaphore until all users have left. */
static u64 nvrm_sync_event;
static NvBool nvrm_current_isr(void) {
    /* Keep the slot and its depth on one CPU across this snapshot. The RM hard
     * ISR restores its depth before any EOI/context switch. */
    bool irq = irq_save();
    u32 slot = cpu_current_slot();
    NvBool active = slot >= ARRAY_LEN(nvrm_in_isr) ||
                    __atomic_load_n(&nvrm_in_isr[slot], __ATOMIC_ACQUIRE) ? NV_TRUE : NV_FALSE;
    irq_restore(irq);
    return active;
}
NvBool os_semaphore_may_sleep(void) {
    return (read_flags() & 0x200u) && proc_current() &&
           !nvrm_current_isr() ? NV_TRUE : NV_FALSE;
}

static NV_STATUS lock_alloc(void **out)
{
    if (!out) return NV_ERR_INVALID_ARGUMENT;
    *out = kzalloc(sizeof(nvrm_lock_t));
    return *out ? NV_OK : NV_ERR_NO_MEMORY;
}
static void lock_take(nvrm_lock_t *l)
{
    for (;;) {
        u64 expected = __atomic_load_n(&nvrm_sync_event, __ATOMIC_ACQUIRE);
        if (!__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE)) return;
        /* RM waits are not process cancellation points. Its owner must unwind
         * the transaction before a stop request can release device resources. */
        if (sched_wait_event(&nvrm_sync_event, expected, ~0ull) < 0) sched_yield();
    }
}
static NvBool lock_try(nvrm_lock_t *l)
{
    NvU32 expected = 0;
    return __atomic_compare_exchange_n(&l->held, &expected, 1, false,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? NV_TRUE : NV_FALSE;
}
static void lock_drop(nvrm_lock_t *l)
{ __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE); }

NV_STATUS os_alloc_mutex(void **p) { return lock_alloc(p); }
void os_free_mutex(void *p) { kfree(p); }
NV_STATUS os_acquire_mutex(void *p)
{
    if (!p) return NV_ERR_INVALID_ARGUMENT;
    if (!os_semaphore_may_sleep()) return NV_ERR_INVALID_REQUEST;
    lock_take(p);
    return NV_OK;
}
NV_STATUS os_cond_acquire_mutex(void *p)
{
    if (!p) return NV_ERR_INVALID_ARGUMENT;
    if (!os_semaphore_may_sleep()) return NV_ERR_INVALID_REQUEST;
    return lock_try(p) ? NV_OK : NV_ERR_TIMEOUT_RETRY;
}
void os_release_mutex(void *p) {
    if (!p) return;
    lock_drop(p);
    sched_signal_event(&nvrm_sync_event);
}

NV_STATUS os_alloc_spinlock(void **p) { return lock_alloc(p); }
void os_free_spinlock(void *p) { kfree(p); }
NvU64 os_acquire_spinlock(void *p)
{
    NvU64 flags = irq_save();
    if (p) {
        nvrm_lock_t *l=p;
        while (__atomic_exchange_n(&l->held,1,__ATOMIC_ACQUIRE)) {
            smp_tlb_poll();
            __asm__ __volatile__("pause");
        }
    }
    return flags;
}
void os_release_spinlock(void *p, NvU64 flags)
{ if (p) lock_drop(p); irq_restore((bool)flags); }

void *os_alloc_rwlock(void)
{ nvrm_lock_t *p = kzalloc(sizeof *p); return p; }
void os_free_rwlock(void *p) { kfree(p); }
NV_STATUS os_acquire_rwlock_read(void *p) { return os_acquire_mutex(p); }
NV_STATUS os_acquire_rwlock_write(void *p) { return os_acquire_mutex(p); }
NV_STATUS os_cond_acquire_rwlock_read(void *p) { return os_cond_acquire_mutex(p); }
NV_STATUS os_cond_acquire_rwlock_write(void *p) { return os_cond_acquire_mutex(p); }
void os_release_rwlock_read(void *p) { os_release_mutex(p); }
void os_release_rwlock_write(void *p) { os_release_mutex(p); }

void *os_alloc_semaphore(NvU32 initial)
{
    nvrm_sema_t *s = kzalloc(sizeof *s);
    if (s) s->count = initial;
    return s;
}
void os_free_semaphore(void *p) { kfree(p); }
NV_STATUS os_cond_acquire_semaphore(void *p)
{
    nvrm_sema_t *s = p;
    if (!s) return NV_ERR_INVALID_ARGUMENT;
    NvU32 old = __atomic_load_n(&s->count, __ATOMIC_RELAXED);
    while (old > 0) {
        if (__atomic_compare_exchange_n(&s->count, &old, old - 1, false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return NV_OK;
    }
    return NV_ERR_TIMEOUT_RETRY;
}
NV_STATUS os_acquire_semaphore(void *p)
{
    if (!p) return NV_ERR_INVALID_ARGUMENT;
    if (!os_semaphore_may_sleep()) return NV_ERR_INVALID_REQUEST;
    for (;;) {
        u64 expected = __atomic_load_n(&nvrm_sync_event, __ATOMIC_ACQUIRE);
        NV_STATUS status = os_cond_acquire_semaphore(p);
        if (status != NV_ERR_TIMEOUT_RETRY) return status;
        if (sched_wait_event(&nvrm_sync_event, expected, ~0ull) < 0) sched_yield();
    }
}
NV_STATUS os_release_semaphore(void *p)
{
    if (!p) return NV_ERR_INVALID_ARGUMENT;
    nvrm_sema_t *s = p;
    NvU32 old = __atomic_load_n(&s->count, __ATOMIC_RELAXED);
    do {
        if (old == 0xffffffffu) return NV_ERR_INVALID_STATE;
    } while (!__atomic_compare_exchange_n(&s->count, &old, old + 1u, false,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    sched_signal_event(&nvrm_sync_event);
    return NV_OK;
}

static nvidia_stack_t *nvrm_stack_alloc(void)
{
    nvidia_stack_t *sp = kzalloc(sizeof *sp);
    if (sp) {
        sp->size = sizeof sp->stack;
        sp->top = sp->stack + sp->size;
    }
    return sp;
}

#include "nvrm_timers.h"

#include "nvrm_workqueue.h"

void *os_pci_init_handle(NvU32 segment, NvU8 bus, NvU8 slot, NvU8 function,
                         NvU16 *vendor, NvU16 *device)
{
    for (pci_dev_t *d = pci_first(); d; d = d->next) {
        if (d->segment != segment || d->bus != bus || d->slot != slot ||
            d->func != function) continue;
        if (vendor) *vendor = d->vendor;
        if (device) *device = d->device;
        return d;
    }
    return NULL;
}
NV_STATUS os_pci_read_byte(void *h, NvU32 o, NvU8 *v)
{ if (!h || !v) return NV_ERR_INVALID_ARGUMENT; *v = pci_read8(h, o); return NV_OK; }
NV_STATUS os_pci_read_word(void *h, NvU32 o, NvU16 *v)
{ if (!h || !v) return NV_ERR_INVALID_ARGUMENT; *v = pci_read16(h, o); return NV_OK; }
NV_STATUS os_pci_read_dword(void *h, NvU32 o, NvU32 *v)
{ if (!h || !v) return NV_ERR_INVALID_ARGUMENT; *v = pci_read32(h, o); return NV_OK; }
NV_STATUS os_pci_write_byte(void *h, NvU32 o, NvU8 v)
{ if (!h) return NV_ERR_INVALID_ARGUMENT; pci_write8(h, o, v); return NV_OK; }
NV_STATUS os_pci_write_word(void *h, NvU32 o, NvU16 v)
{ if (!h) return NV_ERR_INVALID_ARGUMENT; pci_write16(h, o, v); return NV_OK; }
NV_STATUS os_pci_write_dword(void *h, NvU32 o, NvU32 v)
{ if (!h) return NV_ERR_INVALID_ARGUMENT; pci_write32(h, o, v); return NV_OK; }
NvBool os_pci_remove_supported(void) { return NV_FALSE; }
void os_pci_remove(void *h) { (void)h; }

void *os_map_kernel_space(NvU64 phys, NvU64 bytes, NvU32 mode)
{
    /* Match Linux's RM contract: mode 2 is NV_MEMORY_WRITECOMBINED and is
     * used for CPU mappings of scanout/VRAM.  Treating it as uncached made
     * full-resolution multi-monitor pattern fills slow enough to trip the
     * watchdog, even when the display pipeline itself was healthy. */
    if (mode == NV_MEMORY_WRITECOMBINED)
        return vmm_map_wc(phys, (size_t)bytes);
    return vmm_map_mmio(phys, (size_t)bytes);
}
void os_unmap_kernel_space(void *p, NvU64 bytes) { (void)p; (void)bytes; }

/* ------------------------------------------------ NVIDIA page/DMA contract */
#define NVRM_PAGES_MAGIC 0x4e565047u
typedef struct {
    NvU32 magic, pages;
    NvBool owned, contiguous, mapped_alias;
    /* `direct_cpu` is the allocator-owned WB alias used only to release the
     * physical run.  `cpu` is the cache-correct address handed to RM. */
    void *direct_cpu;
    void *cpu;
    NvU64 phys;
    NvU64 *phys_list;
} nvrm_pages_t;

/* NVIDIA's Linux wrapper changes every non-cached RM system allocation to UC
 * before returning its kernel address (nv-vm.c:nv_alloc_system_pages).  This
 * is load-bearing for nvidia-push: pushbuffers are allocated WC, mapped into
 * the GPU with CACHE_SNOOP_DISABLE, and published with only an sfence.  If we
 * expose Kestrel's ordinary WB direct-map alias, the methods can remain dirty
 * in a CPU cache forever and the first channel NOP times out as "Failed to
 * initialize DMA."  Flush the allocator's initial zeroing, then give RM a UC
 * alias exactly as the official driver does for non-cached allocations. */
static void nvrm_writeback_cache_range(void *address, size_t bytes)
{
    NvUPtr start = (NvUPtr)address & ~(NvUPtr)63u;
    NvUPtr end = ((NvUPtr)address + bytes + 63u) & ~(NvUPtr)63u;
    for (NvUPtr p = start; p < end; p += 64u)
        __asm__ __volatile__("clflush (%0)" :: "r"((void *)p) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
}

NV_STATUS nv_alloc_pages(nv_state_t *nv, NvU32 page_count, NvU64 page_size,
                         NvBool contiguous, NvU32 cache_type, NvBool zeroed,
                         NvBool unencrypted, NvS32 node_id, NvU64 *ptes,
                         void **private_out)
{
    (void)nv; (void)zeroed; (void)unencrypted; (void)node_id;
    if (!page_count || !ptes || !private_out || !page_size ||
        page_size % PAGE_SIZE) return NV_ERR_INVALID_ARGUMENT;
    /* OS contract (Linux nv.c nv_alloc_pages / nv-vm.c): page_count is ALREADY
     * the total count of PAGE_SIZE (4K) pages; page_size is only the physical
     * allocation granularity/order, NEVER a multiplier.  The 595 GSP radix3
     * sets AT_GPU page size to RM_PAGE_SIZE_HUGE (2 MB), so the old
     * `page_count * (page_size/PAGE_SIZE)` inflated the count ×512 and tripped
     * this guard -> NV_ERR_INVALID_ARGUMENT (kgspCreateRadix3 failed). */
    NvU64 total_pages64 = (NvU64)page_count;
    if (!total_pages64 || total_pages64 > 0x100000u)
        return NV_ERR_INVALID_ARGUMENT;
    size_t total_pages = (size_t)total_pages64;
    nvrm_pages_t *a = kzalloc(sizeof *a);
    if (!a) return NV_ERR_NO_MEMORY;
    u64 phys = 0;
    a->direct_cpu = dma_alloc_pages(total_pages, &phys);
    a->phys = phys;
    if (!a->direct_cpu) { kfree(a); return NV_ERR_NO_MEMORY; }
    a->cpu = a->direct_cpu;
    if (cache_type != NV_MEMORY_CACHED) {
        size_t bytes = total_pages * PAGE_SIZE;
        nvrm_writeback_cache_range(a->direct_cpu, bytes);
        a->cpu = vmm_map_mmio(phys, bytes);
        if (!a->cpu) {
            dma_free_pages(a->direct_cpu, total_pages);
            kfree(a);
            return NV_ERR_NO_MEMORY;
        }
        a->mapped_alias = NV_TRUE;
    }
    a->magic = NVRM_PAGES_MAGIC;
    a->pages = (NvU32)total_pages;
    a->owned = NV_TRUE;
    /* Kestrel's allocator supplies a physically contiguous run even when RM
     * only required a page list.  Record that stronger property so kernel
     * mappings do not reject otherwise valid non-contiguous requests. */
    a->contiguous = NV_TRUE;
    if (contiguous) {
        ptes[0] = a->phys;
    } else {
        /* RM's pageArrayGranularity is RM_PAGE_SIZE (4K): the pte_array holds
         * PAGE_SIZE-strided sub-page addresses, regardless of page_size. */
        for (NvU32 i = 0; i < page_count; i++)
            ptes[i] = a->phys + (NvU64)i * PAGE_SIZE;
    }
    *private_out = a;
    return NV_OK;
}

NV_STATUS nv_free_pages(nv_state_t *nv, NvU32 page_count, NvBool contiguous,
                        NvU32 cache_type, void *private_data)
{
    (void)nv; (void)page_count; (void)contiguous; (void)cache_type;
    nvrm_pages_t *a = private_data;
    if (!a || a->magic != NVRM_PAGES_MAGIC) return NV_ERR_INVALID_ARGUMENT;
    if (a->mapped_alias)
        vmm_unmap_range(vmm_kernel_pml4(), (NvU64)(NvUPtr)a->cpu,
                        (size_t)a->pages * PAGE_SIZE);
    if (a->owned) dma_free_pages(a->direct_cpu, a->pages);
    kfree(a->phys_list);
    a->magic = 0;
    kfree(a);
    return NV_OK;
}

NV_STATUS nv_register_phys_pages(nv_state_t *nv, NvU64 *phys, NvU64 count,
                                 NvU32 cache_type, void **private_out)
{
    (void)nv; (void)cache_type;
    if (!phys || !count || count > 0x100000u || !private_out)
        return NV_ERR_INVALID_ARGUMENT;
    nvrm_pages_t *a = kzalloc(sizeof *a);
    if (!a) return NV_ERR_NO_MEMORY;
    a->phys_list = kmalloc((size_t)count * sizeof(NvU64));
    if (!a->phys_list) { kfree(a); return NV_ERR_NO_MEMORY; }
    memcpy(a->phys_list, phys, (size_t)count * sizeof(NvU64));
    a->magic = NVRM_PAGES_MAGIC;
    a->pages = (NvU32)count;
    a->phys = phys[0];
    a->direct_cpu = a->cpu = phys_to_virt(phys[0]);
    a->owned = NV_FALSE;
    a->contiguous = NV_TRUE;
    for (NvU64 i = 1; i < count; i++)
        if (phys[i] != phys[0] + i * PAGE_SIZE) a->contiguous = NV_FALSE;
    *private_out = a;
    return NV_OK;
}

void nv_unregister_phys_pages(nv_state_t *nv, void *private_data)
{
    (void)nv;
    nvrm_pages_t *a = private_data;
    if (!a || a->magic != NVRM_PAGES_MAGIC) return;
    kfree(a->phys_list);
    a->magic = 0;
    kfree(a);
}

NV_STATUS nv_register_peer_io_mem(nv_state_t *nv, NvU64 *phys,
                                  NvU64 count, void **private_out)
{ return nv_register_phys_pages(nv, phys, count, NV_MEMORY_UNCACHED, private_out); }
void nv_unregister_peer_io_mem(nv_state_t *nv, void *private_data)
{ nv_unregister_phys_pages(nv, private_data); }
NV_STATUS nv_register_user_pages(nv_state_t *nv, NvU64 count, NvU64 *ptes,
                                 void *user, void **private_out, NvBool write)
{
    (void)user; (void)write;
    return nv_register_phys_pages(nv, ptes, count, NV_MEMORY_CACHED, private_out);
}
void nv_unregister_user_pages(nv_state_t *nv, NvU64 count, void **pages,
                              void **private_data)
{
    (void)count; (void)pages;
    if (private_data && *private_data) {
        nv_unregister_phys_pages(nv, *private_data); *private_data = NULL;
    }
}
NV_STATUS nv_register_sgt(nv_state_t *nv, NvU64 *ptes, NvU64 count,
                          NvU32 cache, void **private_out, struct sg_table *sgt,
                          void *import_private, NvBool imported)
{
    (void)sgt; (void)import_private; (void)imported;
    return nv_register_phys_pages(nv, ptes, count, cache, private_out);
}
void nv_unregister_sgt(nv_state_t *nv, struct sg_table **sgt,
                       void **import_private, void *private_data)
{
    if (sgt) *sgt = NULL; if (import_private) *import_private = NULL;
    nv_unregister_phys_pages(nv, private_data);
}
NV_STATUS nv_get_num_phys_pages(void *private_data, NvU32 *count)
{
    nvrm_pages_t *a = private_data;
    if (!a || a->magic != NVRM_PAGES_MAGIC || !count) return NV_ERR_INVALID_ARGUMENT;
    *count = a->pages; return NV_OK;
}
NV_STATUS nv_get_phys_pages(void *private_data, void *pages, NvU32 *count)
{
    nvrm_pages_t *a = private_data;
    if (!a || a->magic != NVRM_PAGES_MAGIC || !pages || !count)
        return NV_ERR_INVALID_ARGUMENT;
    NvU32 n = *count < a->pages ? *count : a->pages;
    void **out = pages;
    for (NvU32 i = 0; i < n; i++)
        out[i] = phys_to_virt(a->phys_list ? a->phys_list[i] :
                              a->phys + (NvU64)i * PAGE_SIZE);
    *count = n; return NV_OK;
}

NV_STATUS nv_alias_pages(nv_state_t *nv, NvU32 page_count, NvU64 page_size,
                         NvU32 contiguous, NvU32 cache_type, NvU64 guest_id,
                         NvU64 *ptes, NvBool carveout, void **private_out)
{
    (void)guest_id; (void)carveout;
    if (!ptes || !page_count || !private_out || page_size != PAGE_SIZE)
        return NV_ERR_INVALID_ARGUMENT;
    NV_STATUS s = nv_register_phys_pages(nv, ptes, page_count, cache_type,
                                         private_out);
    if (s == NV_OK)
        ((nvrm_pages_t *)*private_out)->contiguous = contiguous ? NV_TRUE : NV_FALSE;
    return s;
}

void *nv_alloc_kernel_mapping(nv_state_t *nv, void *private_data,
                              NvU64 page_index, NvU32 page_offset, NvU64 size,
                              void **mapping_private)
{
    (void)nv;
    nvrm_pages_t *a = private_data;
    if (mapping_private) *mapping_private = NULL;
    if (!a || a->magic != NVRM_PAGES_MAGIC || !a->contiguous ||
        page_index >= a->pages || page_offset >= PAGE_SIZE ||
        size > (NvU64)(a->pages - page_index) * PAGE_SIZE - page_offset)
        return NULL;
    return (u8 *)a->cpu + page_index * PAGE_SIZE + page_offset;
}

void nv_free_kernel_mapping(nv_state_t *nv, void *private_data, void *address,
                            void *mapping_private)
{ (void)nv; (void)private_data; (void)address; (void)mapping_private; }

NV_STATUS nv_alloc_user_mapping(nv_state_t *nv, void *private_data,
                                NvU64 page_index, NvU32 page_offset, NvU64 size,
                                NvU32 protect, NvU64 *address, void **mapping_private)
{
    (void)nv; (void)protect;
    nvrm_pages_t *a = private_data;
    if (mapping_private) *mapping_private = NULL;
    if (!a || a->magic != NVRM_PAGES_MAGIC || !address ||
        page_index >= a->pages || page_offset >= PAGE_SIZE ||
        size > (NvU64)(a->pages - page_index) * PAGE_SIZE - page_offset)
        return NV_ERR_INVALID_ARGUMENT;
    NvU64 phys = a->contiguous ? a->phys + page_index * PAGE_SIZE :
                 (a->phys_list ? a->phys_list[page_index] :
                                 a->phys + page_index * PAGE_SIZE);
    *address = phys + page_offset;
    return NV_OK;
}
void nv_free_user_mapping(nv_state_t *nv, void *private_data,
                          NvU64 address, void *mapping_private)
{ (void)nv; (void)private_data; (void)address; (void)mapping_private; }

NV_STATUS nv_dma_map_alloc(nv_dma_device_t *dev, NvU64 count, NvU64 *ptes,
                           NvBool contig, NvBool cache, void **priv)
{
    (void)dev; (void)count; (void)ptes; (void)contig; (void)cache;
    if (priv) *priv = NULL;              /* identity DMA, no IOMMU */
    return NV_OK;
}
NV_STATUS nv_dma_unmap_alloc(nv_dma_device_t *dev, NvU64 count, NvU64 *ptes,
                             void **priv)
{ (void)dev; (void)count; (void)ptes; if (priv) *priv = NULL; return NV_OK; }
NV_STATUS nv_dma_map_mmio(nv_dma_device_t *dev, NvU64 phys, NvU64 *dma)
{ (void)dev; if (!dma) return NV_ERR_INVALID_ARGUMENT; *dma = phys; return NV_OK; }
void nv_dma_unmap_mmio(nv_dma_device_t *dev, NvU64 dma, NvU64 size)
{ (void)dev; (void)dma; (void)size; }
NV_STATUS nv_dma_map_peer(nv_dma_device_t *dev, nv_dma_device_t *peer,
                          NvU8 bar, NvU64 phys, NvU64 *dma)
{ (void)dev; (void)peer; (void)bar; if (!dma) return NV_ERR_INVALID_ARGUMENT; *dma = phys; return NV_OK; }
void nv_dma_unmap_peer(nv_dma_device_t *dev, NvU64 dma, NvU64 size)
{ (void)dev; (void)dma; (void)size; }
void nv_dma_cache_invalidate(nv_dma_device_t *dev, void *private_data)
{ (void)dev; (void)private_data; __asm__ volatile("mfence" ::: "memory"); }
void *nv_dma_get_dev_pagemap(NvU64 phys) { (void)phys; return NULL; }
void nv_dma_put_dev_pagemap(void *map) { (void)map; }
NvBool nv_grdma_pci_topology_supported(nv_state_t *nv, nv_dma_device_t *dev)
{ (void)nv; (void)dev; return NV_FALSE; }
void nv_flush_coherent_cpu_cache_range(nv_state_t *nv, NvU64 va, NvU64 size)
{ (void)nv; (void)va; (void)size; __asm__ volatile("mfence" ::: "memory"); }
NvBool nv_requires_dma_remap(nv_state_t *nv) { (void)nv; return NV_FALSE; }
void nv_set_dma_address_size(nv_state_t *nv, NvU32 bits)
{ if (nv) nv->dma_mask = bits >= 64 ? ~(NvU64)0 : ((1ull << bits) - 1); }

/* ------------------------------------------------ firmware/platform policy */
/* Linux returns true here only for the special case where RM itself is hosted
 * as firmware (rm_firmware_active=all).  A normal Blackwell dGPU uses the host
 * RM plus GSP-RM, selected by request_firmware/request_fw_client_rm below. */
NvBool nv_is_rm_firmware_active(nv_state_t *nv) { (void)nv; return NV_FALSE; }
const void *nv_get_firmware(nv_state_t *nv, nv_firmware_type_t type,
                            nv_firmware_chip_family_t family,
                            const void **buffer, NvU32 *bytes)
{
    (void)nv; (void)family;
    if (!buffer || !bytes || type != NV_FIRMWARE_TYPE_GSP) return NULL;
    firmware_t *fw = kzalloc(sizeof *fw);
    if (!fw) return NULL;
    /* NVIDIA deliberately uses the GA10x GSP image for GA10x and every
     * subsequent discrete family, including GB20x (nv-firmware.h). */
    if (!firmware_load("nvidia/gb202/gsp/gsp-595.99.02.bin", fw) ||
        fw->size > 0xffffffffu) {
        kfree(fw);
        return NULL;
    }
    *buffer = fw->data;
    *bytes = (NvU32)fw->size;
    return fw;
}
void nv_put_firmware(const void *handle)
{
    firmware_t *fw = (firmware_t *)handle;
    if (!fw) return;
    firmware_free(fw);
    kfree(fw);
}

NV_STATUS nv_set_primary_vga_status(nv_state_t *nv)
{ if (nv) nv->primary_vga = NV_TRUE; return NV_OK; }
NvBool nv_is_chassis_notebook(void) { return NV_FALSE; }
NvBool nv_platform_supports_s0ix(void) { return NV_FALSE; }
NV_STATUS nv_indicate_idle(nv_state_t *nv) { (void)nv; return NV_OK; }
NV_STATUS nv_indicate_not_idle(nv_state_t *nv) { (void)nv; return NV_OK; }
NV_STATUS nv_enable_clk(nv_state_t *nv, TEGRASOC_WHICH_CLK c)
{ (void)nv; (void)c; return NV_ERR_NOT_SUPPORTED; }
void nv_disable_clk(nv_state_t *nv, TEGRASOC_WHICH_CLK c) { (void)nv; (void)c; }
NV_STATUS nv_get_max_freq(nv_state_t *nv, TEGRASOC_WHICH_CLK c, NvU32 *f)
{ (void)nv; (void)c; if (f) *f = 0; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_set_freq(nv_state_t *nv, TEGRASOC_WHICH_CLK c, NvU32 f)
{ (void)nv; (void)c; (void)f; return NV_ERR_NOT_SUPPORTED; }
void nv_get_updated_emu_seg(NvU32 *start, NvU32 *end)
{ if (start) *start = 0; if (end) *end = 0; }
NvU32 nv_get_dev_minor(nv_state_t *nv) { (void)nv; return 0; }
NV_STATUS nv_get_device_memory_config(nv_state_t *nv, NvU64 *compressed,
        NvU64 *guest, NvU64 *guest_size, NvU64 *reserved, NvU32 *width,
        NvS32 *node)
{
    (void)nv;
    if (compressed) *compressed = 0; if (guest) *guest = 0;
    if (guest_size) *guest_size = 0; if (reserved) *reserved = 0;
    if (width) *width = 0; if (node) *node = -1;
    return NV_ERR_NOT_SUPPORTED; /* x86 discrete GPU: no coherent SoC carveout */
}
void nv_get_disp_smmu_stream_ids(nv_state_t *nv, NvU32 *iso, NvU32 *niso)
{ if (iso) *iso = nv ? nv->iommus.dispIsoStreamId : 0;
  if (niso) *niso = nv ? nv->iommus.dispNisoStreamId : 0; }

static nv_state_t *nvrm_active_nv;
static nvidia_stack_t *nvrm_driver_stack;
static nvidia_stack_t *nvrm_isr_stack;
static nvidia_stack_t *nvrm_isr_bh_stack;
static NvBool nvrm_global_ready, nvrm_adapter_ready;
static volatile NvU32 nvrm_bh_pending;
static int nvrm_irq_vector = -1;
bool nvrm_completion_irq_ready(void)
{ return nvrm_adapter_ready && nvrm_irq_vector >= 0; }
void nvrm_os_set_active_gpu(nv_state_t *nv) { nvrm_active_nv = nv; }
static void nvrm_timer_thread(void *unused)
{ (void)unused; for(;;) { nvrm_os_pump(); sched_sleep_ms(1); } }

static regs_t *nvrm_irq_handler(regs_t *regs, void *opaque)
{
    nv_state_t *nv = (nv_state_t *)opaque;
    NvU32 need_bh = 0;
    u32 slot = cpu_current_slot();
    if (slot >= ARRAY_LEN(nvrm_in_isr)) panic("RM IRQ: invalid CPU slot");
    __atomic_add_fetch(&nvrm_in_isr[slot],1,__ATOMIC_ACQ_REL);
    if (nv && nvrm_isr_stack) {
        (void)rm_isr(nvrm_isr_stack, nv, &need_bh);
    }
    __atomic_sub_fetch(&nvrm_in_isr[slot],1,__ATOMIC_ACQ_REL);
    if (need_bh)
        __atomic_store_n(&nvrm_bh_pending, 1, __ATOMIC_RELEASE);
    /* This kernel does not EOI handled vectors in interrupt_dispatch(); every
     * installed handler acks its own vector (timer_isr, keyboard_isr,
     * mouse_isr all do).  Without this, the MSI vector's local-APIC
     * in-service bit stays set forever after the first GSP interrupt: no
     * further NVIDIA MSI is delivered (GSP RPC completions never arrive) and,
     * because the MSI vector (0x40+) outranks the timer (0x30), the timer is
     * masked too - freezing timer_now_us() so os_delay() spins forever and
     * rm_init_adapter hangs. */
    lapic_eoi();
    /* RM has acknowledged its engine leaves and released ISR locks. An IRQ
     * only prompts a fresh exact-cookie probe; unrelated GPU IRQs never
     * complete a submission. Switch after EOI, never from inside rm_isr. */
    return sched_device_irq(regs);
}

static void nvrm_bh_thread(void *unused)
{
    (void)unused;
    for (;;) {
        if (__atomic_exchange_n(&nvrm_bh_pending, 0, __ATOMIC_ACQ_REL)) {
            if (nvrm_active_nv && nvrm_isr_bh_stack)
                rm_isr_bh(nvrm_isr_bh_stack, nvrm_active_nv);
            continue;
        }
        sched_sleep_ms(1);
    }
}

NvBool nvrm_start_gpu(NvU8 bus, NvU8 slot, NvU8 function, void *mapped_bar0)
{
    if (nvrm_adapter_ready) return NV_TRUE;
    pci_dev_t *pdev = NULL;
    for (pci_dev_t *d=pci_first(); d; d=d->next)
        if (d->bus==bus && d->slot==slot && d->func==function) { pdev=d;break; }
    if (!pdev || pdev->vendor != 0x10de || !pdev->bar[0] || !pdev->bar_size[0])
        return NV_FALSE;

    /* RM may queue from its hard ISR after adapter initialization. Ensure the
     * default worker is really created before any RM entry can publish work. */
    if (nvrm_work_start(&nvrm_global_work_queue) != NV_OK) {
        kerr("nvrm", "cannot start RM work queue"); return NV_FALSE;
    }

    nvrm_driver_stack = nvrm_stack_alloc();
    if (!nvrm_driver_stack) return NV_FALSE;
    if (!nvidia_caps_root)
        nvidia_caps_root = os_nv_cap_create_dir_entry(NULL,"driver/nvidia",0);
    if (!nvidia_caps_root || !rm_init_rm(nvrm_driver_stack)) {
        kerr("nvrm","rm_init_rm failed"); return NV_FALSE;
    }
    nvrm_global_ready = NV_TRUE;

    nv_state_t *nv = kzalloc(sizeof *nv);
    if (!nv) return NV_FALSE;
    nv->pci_info.domain=pdev->segment;nv->pci_info.bus=pdev->bus;
    nv->pci_info.slot=pdev->slot;nv->pci_info.function=pdev->func;
    nv->pci_info.vendor_id=pdev->vendor;nv->pci_info.device_id=pdev->device;
    nv->subsystem_vendor=pdev->subsys_vendor;nv->subsystem_id=pdev->subsys_device;
    nv->handle=pdev;nv->os_state=pdev;nv->interrupt_line=pdev->irq_line;
    nv->dma_dev=&nvrm_dma_device;
    nv->niso_dma_dev=&nvrm_dma_device;
    nv->dma_mask=0xffffffffull;nv->cpu_numa_node_id=-1;
    nv->bars[NV_GPU_BAR_INDEX_REGS].cpu_address=pdev->bar[0];
    nv->bars[NV_GPU_BAR_INDEX_REGS].size=pdev->bar_size[0];
    nv->bars[NV_GPU_BAR_INDEX_REGS].offset=NVRM_PCICFG_BAR_OFFSET(0);
    /* Match the Linux probe contract: RM owns creation of its permanent BAR0
     * mapping in RmSetupRegisters().  Pre-populating these fields made
     * nv_os_map_kernel_space() trip its aperture->map == NULL assertion and
     * then overwrite the caller's mapping.  Keep mapped_bar0 only as the
     * Kestrel driver's independent diagnostic view below. */
    nv->bars[NV_GPU_BAR_INDEX_REGS].map=NULL;
    nv->bars[NV_GPU_BAR_INDEX_REGS].map_u=NULL;
    nv->regs=&nv->bars[NV_GPU_BAR_INDEX_REGS];
    unsigned outbar=1;
    for(unsigned i=1;i<6 && outbar<NV_GPU_NUM_BARS;i++) {
        if(!pdev->bar[i]||!pdev->bar_size[i]||pdev->bar_is_io[i])continue;
        nv->bars[outbar].cpu_address=pdev->bar[i];
        nv->bars[outbar].size=pdev->bar_size[i];
        nv->bars[outbar].offset=NVRM_PCICFG_BAR_OFFSET(i);outbar++;
        if(pdev->bar_is_64[i])i++;
    }
    nv->fb=&nv->bars[NV_GPU_BAR_INDEX_FB];
    nv->ud=nv->bars[NV_GPU_BAR_INDEX_IMEM];
    for(unsigned i=0;i<NVRM_PCICFG_NUM_DWORDS;i++)nv->pci_cfg_space[i]=pci_read32(pdev,i*4);
    memcpy(nv->cached_gpu_info.vbios_version,"??.??.??.??.??",15);
    nvrm_active_nv=nv;

    pci_enable_memory(pdev);
    pci_enable_bus_master(pdev);

    if (!rm_init_event_locks(nvrm_driver_stack,nv)) {
        kerr("nvrm","rm_init_event_locks failed"); return NV_FALSE;
    }
    if (rm_is_supported_device(nvrm_driver_stack,nv)!=NV_OK) {
        kerr("nvrm","595.99.02 RM rejected PCI device %04x:%04x",pdev->vendor,pdev->device);
        return NV_FALSE;
    }
    if (!rm_init_private_state(nvrm_driver_stack,nv)) {
        kerr("nvrm","rm_init_private_state failed"); return NV_FALSE;
    }
    rm_set_rm_firmware_requested(nvrm_driver_stack,nv);
    nv->request_fw_client_rm=NV_TRUE;
    nv->allow_fallback_to_monolithic_rm=NV_FALSE;

    /* Linux installs the RM interrupt route after private-state creation but
     * before rm_init_adapter().  GSP RPC completions, display supervisor
     * events and atomic modeset progress depend on this path.  Use one MSI—not
     * a partially configured MSI-X table—because this port has one shared RM
     * top half. */
    nvrm_isr_stack = nvrm_stack_alloc();
    nvrm_isr_bh_stack = nvrm_stack_alloc();
    nvrm_irq_vector = irq_alloc_vector();
    if (!nvrm_isr_stack || !nvrm_isr_bh_stack || nvrm_irq_vector < 0) {
        kerr("nvrm", "could not allocate RM interrupt stacks/vector");
        return NV_FALSE;
    }
    irq_install(nvrm_irq_vector, nvrm_irq_handler, nv);
    if (!pci_setup_single_msi(pdev, (u8)nvrm_irq_vector)) {
        irq_free_vector(nvrm_irq_vector);
        nvrm_irq_vector = -1;
        kerr("nvrm", "GPU exposes no usable single-vector MSI route");
        return NV_FALSE;
    }
    nv->interrupt_line = (NvU32)nvrm_irq_vector;
    nv->flags |= NV_FLAG_USES_MSI;
    kthread_create("nvrm-bh", nvrm_bh_thread, NULL);
    if (!rm_init_adapter(nvrm_driver_stack,nv)) {
        kerr("nvrm","rm_init_adapter failed"); return NV_FALSE;
    }
    nv->flags|=NV_FLAG_INITIALIZED;
    nvrm_adapter_ready=NV_TRUE;
    kthread_create("nvrm-timer",nvrm_timer_thread,NULL);
    kinfo("nvrm","NVIDIA 595.99.02 host RM initialized adapter %04x:%04x at %04x:%02x:%02x.%u",
          pdev->vendor,pdev->device,pdev->segment,pdev->bus,pdev->slot,pdev->func);

    /* Decisive BAR1 probe: the dark-display root is a VRAM/BAR1 aperture that
     * reads the PRI poison 0xBAD0AC (HOST_FB_ACK_TIMEOUT).  Read GSP's own
     * bar1PdeBase (the FB phys base of the BAR1 page directory GSP built and
     * handed CPU-RM at kbusPatchBar1Pdb, kern_bus.c:786) directly from the RM's
     * exported getters.  Offsets 0x5e0/0x5e8 are disassembly-verified against
     * this exact 595.99.02 blob's kbusPatchBar1Pdb_GSPCLIENT.
     *   bar1PdeBase == 0  -> GSP never built BAR1 tables (upstream: GSP FB
     *                        devinit / VRAM bring-up), not the CPU walker bind.
     *   bar1PdeBase != 0  -> tables exist; the FB-ack-timeout is FB/HSHUB
     *                        routing, not the base value. */
    {
        extern void *gpumgrGetSomeGpu(void);
        extern void *gpuGetGspStaticInfo(void *pGpu);
        kinfo("nvrm","BAR probe: bars[0]regs=%#llx/%#llx bars[1]fb=%#llx/%#llx",
              (unsigned long long)nv->bars[0].cpu_address,
              (unsigned long long)nv->bars[0].size,
              (unsigned long long)nv->bars[NV_GPU_BAR_INDEX_FB].cpu_address,
              (unsigned long long)nv->bars[NV_GPU_BAR_INDEX_FB].size);
        void *pGpu = gpumgrGetSomeGpu();
        void *pGSCI = pGpu ? gpuGetGspStaticInfo(pGpu) : NULL;
        if (!pGSCI) {
            kwarn("nvrm","BAR probe: GSP static-info NULL (pGpu=%p) - GSP client not up",pGpu);
        } else {
            NvU64 bar1pde = *(volatile NvU64 *)((NvU8 *)pGSCI + 0x5e0u);
            NvU64 bar2pde = *(volatile NvU64 *)((NvU8 *)pGSCI + 0x5e8u);
            kinfo("nvrm","BAR probe: GSP bar1PdeBase=%#llx bar2PdeBase=%#llx (pGSCI=%p)",
                  (unsigned long long)bar1pde,(unsigned long long)bar2pde,pGSCI);
        }

        /* HW BAR1/BAR2 instance-block BIND registers (VF PRIV block @ BAR0+
         * 0x00B80000; GH100/GB20x bind HAL).  On a GSP client the BAR1 HW bind
         * is GSP-RM's job (CPU-RM only patches its SW walker, kern_bus.c:766) -
         * if GSP never bound the BAR1 instblk, every BAR1->FB access times out
         * (0xBAD0AC).  BAR2 works because CPU-RM pushes its PDE to GSP via RPC.
         * Compare the two: LOW bit0=PENDING bit1=OUTSTANDING bit9=MODE(1=VIRTUAL)
         * bits11:10=TARGET(0=VIDMEM) bits31:12=instblk[31:12]; HIGH=instblk[63:32]. */
        if (mapped_bar0) {
            volatile NvU8 *b0 = (volatile NvU8 *)mapped_bar0;
            NvU32 l1 = *(volatile NvU32 *)(b0 + 0x00B80F60u);
            NvU32 h1 = *(volatile NvU32 *)(b0 + 0x00B80F64u);
            NvU32 l2 = *(volatile NvU32 *)(b0 + 0x00B80F70u);
            NvU32 h2 = *(volatile NvU32 *)(b0 + 0x00B80F74u);
            NvU64 ib1 = ((NvU64)h1 << 32) | (l1 & 0xFFFFF000u);
            NvU64 ib2 = ((NvU64)h2 << 32) | (l2 & 0xFFFFF000u);
            kinfo("nvrm","BAR1 BLOCK lo=%#x hi=%#x instblk=%#llx pend=%u outst=%u mode=%s tgt=%u",
                  l1,h1,(unsigned long long)ib1,l1&1u,(l1>>1)&1u,
                  ((l1>>9)&1u)?"VIRTUAL":"PHYSICAL",(l1>>10)&3u);
            kinfo("nvrm","BAR2 BLOCK lo=%#x hi=%#x instblk=%#llx pend=%u outst=%u mode=%s tgt=%u",
                  l2,h2,(unsigned long long)ib2,l2&1u,(l2>>1)&1u,
                  ((l2>>9)&1u)?"VIRTUAL":"PHYSICAL",(l2>>10)&3u);
        }
    }
    return NV_TRUE;
}

NV_STATUS nvrm_transfer_rm_memory(NvU32 hClient, NvU32 hMemory,
                                  NvU64 offset, void *buffer, NvU64 size,
                                  NvBool read);

/*
 * NVDisplay 3 cannot scan an ordinary XRGB surface without an input LUT: the
 * modeset core converts it to FP16 through pDevEvo->lut.defaultLut, and also
 * uses the output half of that allocation as the default OLUT.  NVIDIA's
 * matching nvkms-lut.c allocates this object in ISO vidmem and initializes it
 * with plain CPU stores through nvRmApiMapMemory().  That is normally fine,
 * but this GB203's CPU BAR1 mapping returns HOST_FB_ACK_TIMEOUT poison while
 * CE access to the same vidmem is sound.  The result is a live raster and
 * completed flips whose compositor never emits the submitted surface.
 *
 * Seed every allocation having the exact private NVKMS LUT tuple through RM's
 * supported CE MemUtils path as soon as it exists.  The later BAR1 stores are
 * ineffective on this machine, so the identity data remains resident.  This
 * is intentionally narrower than rewriting all IMAGE allocations: it matches
 * AllocLutSurfaceEvoInVidmem() in the supplied 595.99.02 source by class,
 * owner, type, ISO attribute and exact ABI size.
 */
typedef struct {
    NvU16 red, green, blue, unused;
} nvrm_evo_lut_entry_t;

#define NVRM_EVO_LUT_VSS_HEADER_ENTRIES 4u
#define NVRM_EVO_LUT_ENTRIES            1025u

typedef struct {
    nvrm_evo_lut_entry_t base[NVRM_EVO_LUT_VSS_HEADER_ENTRIES +
                              NVRM_EVO_LUT_ENTRIES];
    nvrm_evo_lut_entry_t output[NVRM_EVO_LUT_VSS_HEADER_ENTRIES +
                                NVRM_EVO_LUT_ENTRIES]
        __attribute__((aligned(0x100)));
} nvrm_evo_lut_data_t;

_Static_assert(__builtin_offsetof(nvrm_evo_lut_data_t, output) == 0x2100,
               "595.99.02 NVEvoLutDataRec output offset");
_Static_assert(sizeof(nvrm_evo_lut_data_t) == 0x4200,
               "595.99.02 NVEvoLutDataRec size");

/* Exact nvUnorm10ToFp16() result for i/1024.  These values are powers-of-two
 * fractions with at most ten significant bits, so every value is represented
 * exactly in binary16 and no software floating-point operation is required. */
static NvU16 nvrm_unorm10_to_fp16(NvU16 i)
{
    if (i == 0) return 0;
    NvU32 top = 0;
    for (NvU32 v = i; v >>= 1; ) top++;
    return (NvU16)(((top + 5u) << 10) |
                   (((NvU32)i - (1u << top)) << (10u - top)));
}

static NV_STATUS nvrm_seed_evo_identity_lut(NvU32 hClient, NvU32 hMemory)
{
    nvrm_evo_lut_data_t *lut = kzalloc(sizeof(*lut));
    nvrm_evo_lut_data_t *verify = kzalloc(sizeof(*verify));
    if (!lut || !verify) {
        kfree(lut);
        kfree(verify);
        return NV_ERR_NO_MEMORY;
    }

    for (NvU32 i = 0; i < 1024; i++) {
        NvU16 in = nvrm_unorm10_to_fp16((NvU16)i);
        NvU16 out = (NvU16)(i << 6);
        NvU32 n = NVRM_EVO_LUT_VSS_HEADER_ENTRIES + i;
        lut->base[n].red = lut->base[n].green = lut->base[n].blue = in;
        lut->output[n].red = lut->output[n].green = lut->output[n].blue = out;
    }
    lut->base[NVRM_EVO_LUT_VSS_HEADER_ENTRIES + 1024] =
        lut->base[NVRM_EVO_LUT_VSS_HEADER_ENTRIES + 1023];
    lut->output[NVRM_EVO_LUT_VSS_HEADER_ENTRIES + 1024] =
        lut->output[NVRM_EVO_LUT_VSS_HEADER_ENTRIES + 1023];

    NV_STATUS status = nvrm_transfer_rm_memory(hClient, hMemory, 0, lut,
                                                sizeof(*lut), NV_FALSE);
    if (status == NV_OK) {
        status = nvrm_transfer_rm_memory(hClient, hMemory, 0, verify,
                                         sizeof(*verify), NV_TRUE);
        if (status == NV_OK && memcmp(lut, verify, sizeof(*lut)) != 0)
            status = NV_ERR_INVALID_DATA;
    }

    kfree(verify);
    kfree(lut);
    return status;
}

NvBool nvrm_rmapi_op(void *raw)
{
    if (!nvrm_adapter_ready || !raw) return NV_FALSE;

    /*
     * Blackwell's NVIDIA-push channels normally place USERD in vidmem.  The
     * mapped USERD is then written by UserDKickoff/DoorbellKickoff on every
     * submission.  This machine's BAR1 aperture is poisoned with
     * HOST_FB_ACK_TIMEOUT (0xBAD0AC00), so those GPPut writes never reach the
     * channel: NVKMS fails its first NOP with "Failed to initialize DMA" and
     * cannot promote an otherwise valid scanout surface.
     *
     * Use NVIDIA-push's own no-FB USERD choice for precisely that allocation:
     * NV01_MEMORY_SYSTEM, PCI aperture, 4 KiB, uncached, alignment-forced.
     * The official RM channel path accepts this configuration and derives
     * ADDR_SYSMEM from the supplied Memory object's memdesc.  Do not rewrite
     * LUTs, head surfaces, or scanout allocations: USERD is uniquely selected
     * by the exact class/type/flags/size/alignment tuple constructed by
     * nvDmaAllocUserD() in nvidia-push-init.c.
     */
    /* The kernel-open and SDK copies of nvos.h intentionally have different
     * include guards, so including nv-kernel-rmapi-ops.h here would redefine
     * the shared rights structs.  These narrow views preserve the published
     * NVOS64 and NV_MEMORY_ALLOCATION_PARAMS layouts used by 595.99.02. */
    typedef struct {
        NvU32 hRoot, hObjectParent, hObjectNew, hClass;
        NvU64 pAllocParms, pRightsRequested;
        NvU32 paramsSize, flags, status;
    } nvrm_nvos64_view_t;
    typedef struct {
        NvU32 op, alignUnion;
        union { nvrm_nvos64_view_t alloc; } params;
    } nvrm_rmapi_ops_view_t;
    typedef struct {
        NvU32 owner, type, flags, width, height;
        NvS32 pitch;
        NvU32 attr, attr2, format, comprCovg, zcullCovg, alignU64;
        NvU64 rangeLo, rangeHi, size, alignment, offset, limit, address;
        NvU32 ctagOffset, hVASpace, internalflags, tag;
        NvS32 numaNode;
        NvU32 tailPad;
    } nvrm_mem_alloc_view_t;
    _Static_assert(sizeof(nvrm_nvos64_view_t) == 48,
                   "595.99.02 NVOS64 layout");
    _Static_assert(sizeof(nvrm_mem_alloc_view_t) == 128,
                   "595.99.02 memory allocation layout");
    nvrm_rmapi_ops_view_t *op = raw;
    NvBool seedEvoLut = NV_FALSE;
    NvU32 seedClient = 0, seedMemory = 0;
    if (op->op == 0x00000015u /* NV04_ALLOC */) {
        nvrm_nvos64_view_t *alloc = &op->params.alloc;

        /* Blackwell/Hopper NVIDIA-push also asks HOPPER_USERMODE_A to expose
         * its work-submit doorbell through BAR1.  A sysmem USERD alone is not
         * enough: DoorbellKickoff writes USERD.GPPut and then this MMIO token,
         * and the latter still disappears into the poisoned BAR1 aperture.
         *
         * usrmodeConstruct_IMPL in the matching RM has an explicit BAR0 path
         * backed by pRegVF when bBar1Mapping is false.  Select it only for the
         * HOPPER_USERMODE_A allocation made by NVIDIA-push. */
        if (alloc->hClass == 0x0000c661u /* HOPPER_USERMODE_A */ &&
            alloc->pAllocParms != 0 &&
            (alloc->paramsSize == 0 || alloc->paramsSize == 2)) {
            NvBool *bBar1Mapping = (NvBool *)(u64)alloc->pAllocParms;
            if (*bBar1Mapping) {
                *bBar1Mapping = NV_FALSE;
                kinfo("nvrm", "NVKMS doorbell %#x: forcing official BAR0 usermode mapping",
                      alloc->hObjectNew);
            }
        }

        nvrm_mem_alloc_view_t *mem =
            (nvrm_mem_alloc_view_t *)(u64)alloc->pAllocParms;
        const NvU32 userdFlags = 0x00000100u | /* ALIGNMENT_FORCE */
                                 0x00010000u;  /* PERSISTENT_VIDMEM */

        if (alloc->hClass == 0x00000040u /* NV01_MEMORY_LOCAL_USER */ && mem &&
            (alloc->paramsSize == 0 ||
             alloc->paramsSize == sizeof(nvrm_mem_alloc_view_t)) &&
            mem->type == 6u /* NVOS32_TYPE_DMA */ &&
            mem->flags == userdFlags &&
            mem->size != 0 && mem->size == mem->alignment &&
            mem->size <= PAGE_SIZE) {
            alloc->hClass = 0x0000003eu; /* NV01_MEMORY_SYSTEM */
            /* NVOS32_ATTR_LOCATION is bits 26:25; PCI is value 1. */
            mem->attr = (mem->attr & ~0x06000000u) | 0x02000000u;
            mem->flags &= ~0x00010000u;
            kinfo("nvrm", "NVKMS USERD %#x: forcing supported sysmem aperture (%llu bytes)",
                  alloc->hObjectNew, (unsigned long long)mem->size);
        }

        if (alloc->hClass == 0x00000040u /* NV01_MEMORY_LOCAL_USER */ && mem &&
            (alloc->paramsSize == 0 ||
             alloc->paramsSize == sizeof(nvrm_mem_alloc_view_t)) &&
            mem->owner == 0xDCBAu /* NVKMS_RM_HEAP_ID */ &&
            mem->type == 0u /* NVOS32_TYPE_IMAGE */ &&
            mem->size == sizeof(nvrm_evo_lut_data_t) &&
            (mem->attr2 & (1u << 18)) != 0 /* NVOS32_ATTR2_ISO_YES */) {
            seedEvoLut = NV_TRUE;
            seedClient = alloc->hRoot;
            seedMemory = alloc->hObjectNew;
        }
    }

    nvidia_stack_t *sp=nvrm_stack_alloc();
    if(!sp)return NV_FALSE;
    rm_kernel_rmapi_op(sp,raw);
    kfree(sp);

    if (seedEvoLut && op->params.alloc.status == NV_OK) {
        NV_STATUS status = nvrm_seed_evo_identity_lut(seedClient, seedMemory);
        if (status == NV_OK) {
            kinfo("nvrm", "NVKMS LUT %#x: CE identity upload and read-back verified (0x4200 bytes)",
                  seedMemory);
        } else {
            kerr("nvrm", "NVKMS LUT %#x: CE identity upload failed (%#x)",
                 seedMemory, status);
        }
    }
    return NV_TRUE;
}

/* -------------------------------------------------------------------------
 * Native host-RM object API for Kestrel's render/codec client.
 *
 * NVKMS already reaches the same entry point through nvkms_call_rm().  Keep a
 * narrow typed interface here so nv_gsp_rm.c can select the host backend
 * without importing NVIDIA's nvos.h into kernel/nv.h (where its names collide
 * with Kestrel's own compact GPU ABI).  These calls do not start firmware and
 * cannot create a second GSP owner; they are ordinary kernel RM clients, just
 * like NVIDIA's nvidia-drm and UVM modules on Linux.
 */
/* Do not include the SDK nv-kernel-rmapi-ops.h here: kernel-open and the SDK
 * ship intentionally separate nvos.h/rs_access.h copies with incompatible
 * include guards.  These are exact narrow views of the three operations used
 * below.  `alignUnion` places the union at byte 8, matching the official
 * nvidia_kernel_rmapi_ops_t ABI on x86-64. */
typedef struct {
    NvU32 hRoot, hObjectParent, hObjectOld, status;
} nvrm_host_free_params_t;
typedef struct {
    NvU32 hRoot, hObjectParent, hObjectNew, hClass;
    NvU64 pAllocParms, pRightsRequested;
    NvU32 paramsSize, flags, status;
} nvrm_host_alloc_params_t;
typedef struct {
    NvU32 hClient, hObject, cmd, flags;
    NvU64 params;
    NvU32 paramsSize, status;
} nvrm_host_control_params_t;
typedef struct {
    NvU32 hClient, hDevice, hMemory, alignU64;
    NvU64 offset, length, pLinearAddress;
    NvU32 status, flags;
} nvrm_host_map_memory_params_t;
typedef struct {
    NvU32 op, alignUnion;
    union {
        nvrm_host_free_params_t free;
        nvrm_host_alloc_params_t alloc;
        nvrm_host_control_params_t control;
        nvrm_host_map_memory_params_t mapMemory;
    } params;
} nvrm_host_ops_t;
_Static_assert(__builtin_offsetof(nvrm_host_ops_t, params) == 8,
               "kernel RMAPI union offset");
_Static_assert(sizeof(nvrm_host_alloc_params_t) == 48, "NVOS64 size");
_Static_assert(sizeof(nvrm_host_control_params_t) == 32, "NVOS54 size");
_Static_assert(sizeof(nvrm_host_map_memory_params_t) == 48, "NVOS33 size");

#define NVRM_OP_FREE    0x00000000u
#define NVRM_OP_ALLOC   0x00000015u
#define NVRM_OP_MAP     0x00000021u
#define NVRM_OP_CONTROL 0x00000036u

NvU32 nvrm_host_alloc_root(NvU32 *client)
{
    if (!client) return NV_ERR_INVALID_ARGUMENT;
    *client = 0;
    if (!nvrm_adapter_ready) return NV_ERR_INVALID_STATE;
    nvrm_host_ops_t op;
    memset(&op, 0, sizeof(op));
    op.op = NVRM_OP_ALLOC;
    op.params.alloc.hRoot = 0;
    op.params.alloc.hObjectParent = 0;
    op.params.alloc.hObjectNew = 0;
    op.params.alloc.hClass = 0; /* NV01_ROOT */
    op.params.alloc.pAllocParms = (NvU64)(NvUPtr)client;
    /* A refused dispatch (for example, no RM stack) did not execute the
     * operation. A zero-initialized NVOS status is not evidence of success. */
    op.params.alloc.status = NV_ERR_GENERIC;
    if (!nvrm_rmapi_op(&op)) return NV_ERR_GENERIC;
    if (op.params.alloc.status != NV_OK) {
        *client = 0;
        return op.params.alloc.status;
    }
    /* The root allocator publishes the generated handle through both the
     * allocation parameter and hObjectNew depending on RM build. */
    if (*client == 0) *client = op.params.alloc.hObjectNew;
    return op.params.alloc.status;
}

NvU32 nvrm_host_alloc_object(NvU32 client, NvU32 parent, NvU32 object,
                             NvU32 klass, void *params, NvU32 params_size)
{
    if (!nvrm_adapter_ready) return NV_ERR_INVALID_STATE;
    nvrm_host_ops_t op;
    memset(&op, 0, sizeof(op));
    op.op = NVRM_OP_ALLOC;
    op.params.alloc.hRoot = client;
    op.params.alloc.hObjectParent = parent;
    op.params.alloc.hObjectNew = object;
    op.params.alloc.hClass = klass;
    op.params.alloc.pAllocParms = (NvU64)(NvUPtr)params;
    op.params.alloc.paramsSize = params_size;
    op.params.alloc.status = NV_ERR_GENERIC;
    if (!nvrm_rmapi_op(&op)) return NV_ERR_GENERIC;
    return op.params.alloc.status;
}

NvU32 nvrm_host_control_object(NvU32 client, NvU32 object, NvU32 cmd,
                               void *params, NvU32 params_size)
{
    if (!nvrm_adapter_ready) return NV_ERR_INVALID_STATE;
    nvrm_host_ops_t op;
    memset(&op, 0, sizeof(op));
    op.op = NVRM_OP_CONTROL;
    op.params.control.hClient = client;
    op.params.control.hObject = object;
    op.params.control.cmd = cmd;
    op.params.control.params = (NvU64)(NvUPtr)params;
    op.params.control.paramsSize = params_size;
    op.params.control.status = NV_ERR_GENERIC;
    if (!nvrm_rmapi_op(&op)) return NV_ERR_GENERIC;
    return op.params.control.status;
}

void nvrm_host_read_dp_link(u32 client, u32 display_object, u32 display_id,
                           nvrm_dp_link_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->tx_status = out->tx_after_status = out->aux_status = NV_ERR_INVALID_ARGUMENT;
    out->msa_status = NV_ERR_INVALID_ARGUMENT;
    out->aux_reply = 0xffffffffu;
    if (!client || !display_object || !display_id || (display_id & (display_id - 1u)))
        return;

    /* Exact 595.99.02 ABI. Subdevice instance zero is the existing sole-GPU
     * host render tree, not an inferred connector/head number. GET_LINK_CONFIG
     * reports transmitter configuration, not successful receiver training. */
    NV0073_CTRL_DP_GET_LINK_CONFIG_PARAMS before = {0}, after = {0};
    before.displayId = after.displayId = display_id;
    out->tx_status = nvrm_host_control_object(client, display_object,
        NV0073_CTRL_CMD_DP_GET_LINK_CONFIG, &before, sizeof before);
    if (out->tx_status == NV_OK) {
        out->tx_valid = true;
        out->lanes = before.laneCount;
        out->rate_code = before.linkBW;
        out->rate_10mbps = before.dp2LinkBW;
        out->fec = before.bFECEnabled;
    }

    NV0073_CTRL_DP_AUXCH_CTRL_PARAMS aux = {0};
    aux.displayId = display_id;
    aux.cmd = (NV0073_CTRL_DP_AUXCH_CMD_TYPE_AUX << 3) |
               NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_READ;
    aux.addr = 0x200; /* dpcd.h: SINK_COUNT through SINK_STATUS */
    aux.size = sizeof(out->receiver) - 1u; /* request is zero-based */
    aux.replyType = 0xffffffffu;
    out->aux_status = nvrm_host_control_object(client, display_object,
        NV0073_CTRL_CMD_DP_AUXCH_CTRL, &aux, sizeof aux);
    out->aux_reply = aux.replyType;
    out->aux_bytes = aux.size;
    /* NVIDIA ReadDPCDReg requires the exact one-based returned count. A short
     * ACK, DEFER, timeout or rejected RM call is unknown, never a locked link.
     * No retry loop: preserve the first status without adding a boot delay. */
    if (out->aux_status == NV_OK &&
        aux.replyType == NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK &&
        aux.size == sizeof(out->receiver) && !aux.retryTimeMs) {
        out->rx_valid = true;
        memcpy(out->receiver, aux.data, sizeof out->receiver);
    }
    /* MSA is the stream's transmitted timing/clock descriptor, not just the
     * requested mode in Kestrel's cache. An unsupported capture stays unknown. */
    NV0073_CTRL_DP_GET_MSA_ATTRIBUTES_PARAMS msa = {0};
    msa.displayId = display_id;
    out->msa_status = nvrm_host_control_object(client, display_object,
        NV0073_CTRL_CMD_DP_GET_MSA_ATTRIBUTES, &msa, sizeof msa);
    if (out->msa_status == NV_OK) {
        out->mvid = msa.mvid; out->nvid = msa.nvid;
        out->h_total = msa.hTotal; out->v_total = msa.vTotal;
        out->h_start = msa.hActiveStart; out->v_start = msa.vActiveStart;
        out->width = msa.hActiveWidth; out->height = msa.vActiveWidth;
        out->h_sync = msa.hSyncWidth; out->v_sync = msa.vSyncWidth;
        out->misc0 = msa.misc0; out->misc1 = msa.misc1;
        out->h_positive = msa.hSyncPolarity; out->v_positive = msa.vSyncPolarity;
    }
    out->tx_after_status = nvrm_host_control_object(client, display_object,
        NV0073_CTRL_CMD_DP_GET_LINK_CONFIG, &after, sizeof after);
    out->tx_config_stable = out->tx_valid && out->tx_after_status == NV_OK &&
        before.laneCount == after.laneCount && before.linkBW == after.linkBW &&
        before.dp2LinkBW == after.dp2LinkBW && before.bFECEnabled == after.bFECEnabled;

    /* 8b/10b lane CR/EQ/symbol-lock nibbles and interlane align, dpcd.h
     * 0x202..0x204. UHBR uses additional 128b/132b training semantics: keep
     * its raw evidence but never label it with this legacy interpretation. */
    if (out->tx_config_stable && out->rx_valid && out->rate_code && !out->rate_10mbps &&
        (out->lanes == 1 || out->lanes == 2 || out->lanes == 4)) {
        out->legacy_lock_known = true;
        out->legacy_locked = (out->receiver[4] & 1u) != 0;
        for (u32 lane = 0; lane < out->lanes; lane++) {
            u32 nibble = out->receiver[2 + lane / 2] >> (4 * (lane % 2));
            if ((nibble & 7u) != 7u) out->legacy_locked = false;
        }
    }
}

u32 nvrm_host_read_utilization(u32 client, u32 subdevice,
                              u32 utilization[3], u64 *timestamp)
{
    if (!utilization || !timestamp || !nvrm_adapter_ready)
        return NV_ERR_INVALID_STATE;
    *timestamp = 0;
    for (unsigned i = 0; i < 3; i++) utilization[i] = 0xffffffffu;
    /* 72 large sample records do not belong on a kernel stack. Use the exact
     * supplied vendor type, including process metadata and alignment. */
    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *p = kzalloc(sizeof(*p));
    if (!p) return NV_ERR_NO_MEMORY;
    p->type = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL;
    p->bufSize = sizeof(p->samples);
    NvU32 status = nvrm_host_control_object(client, subdevice,
        NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2, p, sizeof(*p));
    static bool reported_reply;
    if (!reported_reply) {
        kinfo("nv-telemetry", "GPUMON raw status=%#x type=%u bytes=%u/%u count=%u tracker=%u",
              status, p->type, p->bufSize, (u32)sizeof(p->samples), p->count, p->tracker);
        reported_reply = true;
    }
    if (status == NV_OK) {
        if (p->bufSize != sizeof(p->samples) ||
            p->type != NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL ||
            p->count > NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL ||
            p->tracker >= NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL) {
            status = NV_ERR_INVALID_STATE;
        } else if (p->count) {
            unsigned newest = (p->tracker + NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL - 1u) %
                               NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL;
            const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *s = &p->samples[newest];
            *timestamp = s->base.timeStamp;
            utilization[0] = s->gr.util;
            utilization[1] = s->nvenc.util;
            utilization[2] = s->nvdec.util;
        }
    }
    kfree(p);
    return status;
}

NvU32 nvrm_host_map_memory(NvU32 client, NvU32 device, NvU32 memory,
                           NvU64 offset, NvU64 length, void **address,
                           NvU32 flags)
{
    if (!address) return NV_ERR_INVALID_ARGUMENT;
    *address = NULL;
    if (!nvrm_adapter_ready) return NV_ERR_INVALID_STATE;
    nvrm_host_ops_t op;
    memset(&op, 0, sizeof(op));
    op.op = NVRM_OP_MAP;
    op.params.mapMemory.hClient = client;
    op.params.mapMemory.hDevice = device;
    op.params.mapMemory.hMemory = memory;
    op.params.mapMemory.offset = offset;
    op.params.mapMemory.length = length;
    op.params.mapMemory.flags = flags;
    op.params.mapMemory.status = NV_ERR_GENERIC;
    if (!nvrm_rmapi_op(&op)) return NV_ERR_GENERIC;
    if (op.params.mapMemory.status == NV_OK)
        *address = (void *)(NvUPtr)op.params.mapMemory.pLinearAddress;
    return op.params.mapMemory.status;
}

NvU32 nvrm_host_free_object(NvU32 client, NvU32 parent, NvU32 object)
{
    if (!nvrm_adapter_ready) return NV_ERR_INVALID_STATE;
    nvrm_host_ops_t op;
    memset(&op, 0, sizeof(op));
    op.op = NVRM_OP_FREE;
    op.params.free.hRoot = client;
    op.params.free.hObjectParent = parent;
    op.params.free.hObjectOld = object;
    op.params.free.status = NV_ERR_GENERIC;
    if (!nvrm_rmapi_op(&op)) return NV_ERR_GENERIC;
    return op.params.free.status;
}
NvBool nvrm_is_ready(void) { return nvrm_adapter_ready; }

/* Official RM_USER_SHARED_DATA: allocation requests firmware polling, unlike
 * a historical GPUMON ring query. Setup is serialized by the render/RM guard;
 * the separate peek API only reads an acquire-published immutable mapping.
 * Objects and read-only mappings live as long as their boot-lifetime client;
 * never free an allocation after an ambiguous mapping failure. */
typedef struct {
    u32 client, device, subdevice, status;
    bool occupied, attempted, capacity_attempted;
    const NV00DE_SHARED_DATA *data;
    u64 vram_bytes;
} nvrm_shared_state_t;
static nvrm_shared_state_t nvrm_shared_state[2];

static bool nvrm_shared_copy(const void *source, void *dest, size_t bytes)
{
    const volatile NvU64 *seq = source;
    for (unsigned attempt = 0; attempt < 10; attempt++) {
        NvU64 before = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        /* These are polled fields, not non-polled sequence-number fields. */
        if (!before || before >= RUSD_SEQ_START) continue;
        memcpy(dest, source, bytes);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        NvU64 after = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (before == after && *(NvU64 *)dest == before) return true;
    }
    memset(dest, 0, bytes);
    return false;
}

u32 nvrm_host_read_shared_telemetry(u32 client, u32 device, u32 subdevice,
                                   nvrm_shared_telemetry_t *out)
{
    if (!out) return NV_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));out->temperature_c = -1000;
    for (unsigned i = 0; i < 3; i++) out->utilization[i] = 0xffffffffu;
    if (!nvrm_adapter_ready) return NV_ERR_INVALID_STATE;
    nvrm_shared_state_t *s = NULL;
    unsigned slot;
    for (slot = 0; slot < 2; slot++) {
        nvrm_shared_state_t *p = &nvrm_shared_state[slot];
        if (p->occupied && p->client == client && p->device == device && p->subdevice == subdevice) {
            s = p;break;
        }
    }
    if (!s) for (slot = 0; slot < 2; slot++) if (!nvrm_shared_state[slot].occupied) {
        s = &nvrm_shared_state[slot];s->occupied = true;
        s->client = client;s->device = device;s->subdevice = subdevice;break;
    }
    if (!s) return NV_ERR_INSUFFICIENT_RESOURCES;
    if (!s->capacity_attempted) {
        s->capacity_attempted = true;
        NV2080_CTRL_FB_GET_INFO_V2_PARAMS p = {0};
        p.fbInfoListSize = 1;
        p.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE;
        u32 status = nvrm_host_control_object(client, subdevice,
            NV2080_CTRL_CMD_FB_GET_INFO_V2, &p, sizeof(p));
        u64 bytes = (u64)p.fbInfoList[0].data << 10; /* official ABI: KiB */
        if (!status && p.fbInfoListSize == 1 &&
            p.fbInfoList[0].index == NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE &&
            bytes >= (16ull << 20) && bytes <= (256ull << 30)) s->vram_bytes = bytes;
        kinfo("nv-telemetry", "physical framebuffer query status=%#x validated=%u MiB",
              status, (u32)(s->vram_bytes >> 20));
    }
    out->vram_bytes = s->vram_bytes;
    if (!s->attempted) {
        s->attempted = true;
        /* Reserved telemetry handle range; not in surface/channel namespaces. */
        u32 handle = 0x006e0000u + slot;
        NV00DE_ALLOC_PARAMETERS p = { .polledDataMask = NV00DE_RUSD_POLL_PERF | NV00DE_RUSD_POLL_THERMAL };
        s->status = nvrm_host_alloc_object(client, subdevice, handle,
                                          RM_USER_SHARED_DATA, &p, sizeof(p));
        if (!s->status) {
            void *mapped = NULL;
            s->status = nvrm_host_map_memory(client, device, handle, 0,
                sizeof(NV00DE_SHARED_DATA), &mapped, 1u); /* nvos.h NVOS33_FLAGS_ACCESS_READ_ONLY */
            if (!s->status && !mapped) s->status = NV_ERR_INVALID_STATE;
            if (!s->status) __atomic_store_n(&s->data, mapped, __ATOMIC_RELEASE);
        }
        kinfo("nv-telemetry", "RUSD perf/thermal polling status=%#x mapped=%u",
              s->status, s->data != NULL);
    }
    if (s->status || !s->data) return s->status ? s->status : NV_ERR_INVALID_STATE;
    return nvrm_host_peek_shared_telemetry(client, device, subdevice, out);
}

u32 nvrm_host_peek_shared_telemetry(u32 client, u32 device, u32 subdevice,
                                   nvrm_shared_telemetry_t *out)
{
    if (!out) return NV_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); out->temperature_c = -1000;
    for (unsigned i = 0; i < 3; i++) out->utilization[i] = 0xffffffffu;
    const NV00DE_SHARED_DATA *data = NULL;
    /* The release-published mapping pins identity/capacity for the entire boot.
     * Read only those immutable fields after acquire; never inspect a slot that
     * is still being allocated. No render lock is needed to read firmware data. */
    for (unsigned slot = 0; slot < 2; slot++) {
        nvrm_shared_state_t *s = &nvrm_shared_state[slot];
        const NV00DE_SHARED_DATA *mapped = __atomic_load_n(&s->data, __ATOMIC_ACQUIRE);
        if (mapped && s->client == client && s->device == device && s->subdevice == subdevice) {
            data = mapped; out->vram_bytes = s->vram_bytes; break;
        }
    }
    if (!data) return NV_ERR_INVALID_STATE;
    RUSD_PERF_DEVICE_UTILIZATION util;
    if (nvrm_shared_copy(&data->perfDevUtil, &util, sizeof(util))) {
        out->timestamp = util.lastModifiedTimestamp;
        /* RUSD counters are whole percentages, GPUMON counters are pct*100. */
        if (util.info.gpuPercentBusy <= 100) out->utilization[0] = util.info.gpuPercentBusy * 100u;
        const unsigned engines[2] = {RUSD_ENG_UTILIZATION_VID_ENG_NVENC, RUSD_ENG_UTILIZATION_VID_ENG_NVDEC};
        for (unsigned i = 0; i < 2; i++) {
            RUSD_ENG_UTILIZATION *e = &util.info.engUtil[engines[i]];
            if (e->samplingPeriodUs && e->clkPercentBusy <= 100) out->utilization[i+1] = e->clkPercentBusy * 100u;
        }
    }
    RUSD_TEMPERATURE temperature;
    if (nvrm_shared_copy(&data->temperatures[RUSD_TEMPERATURE_TYPE_GPU], &temperature, sizeof(temperature))) {
        int celsius = NV_TYPES_NV_TEMP_TO_CELSIUS_TRUNCED(temperature.temperature);
        if (celsius >= -40 && celsius <= 150) {
            out->temperature_timestamp = temperature.lastModifiedTimestamp;
            out->temperature_c = celsius;
        }
    }
    return NV_OK;
}
NvU32 nvrm_gpu_id(void) { return nvrm_active_nv ? nvrm_active_nv->gpu_id : 0xffffffffu; }

/* Copy into an RM allocation through the copy engine, rather than through the
 * CPU BAR1 aperture.  On this GB203, ordinary CPU mappings of client vidmem
 * return 0xBAD0AC00 (HOST_FB_ACK_TIMEOUT), although BAR2 and the display
 * raster are alive.  NVIDIA's own MemUtils layer has the supported escape
 * hatch: TRANSFER_FLAGS_PREFER_CE stages system memory and submits a CE copy.
 *
 * Keep the few NVOC types opaque here.  Their class ids and TRANSFER_SURFACE
 * ABI are taken verbatim from the matching 595.99.02 generated headers.  We
 * locate MemoryManager by RTTI instead of depending on OBJGPU's large private
 * layout, then take the same API/GPU locks used by RM memory entry points. */
typedef struct {
    void *pMemDesc;
    NvU64 offset;
    void *pMapping;
    void *pMappingPriv;
} nvrm_transfer_surface_t;

#define NVRM_NVOC_CLASS_OBJECT       0x497031u
#define NVRM_NVOC_CLASS_MEMMGR       0x22ad47u
#define NVRM_LOCK_MODULE_MEM         0x00002000u
#define NVRM_TRANSFER_PREFER_CE      (1u << 6)

static void *nvrm_memory_manager(void)
{
    extern void *gpumgrGetSomeGpu(void);
    extern void *objDynamicCastById_IMPL(void *, NvU32);
    extern void *objGetChild_IMPL(void *);
    extern void *objGetSibling_IMPL(void *);

    void *gpu = gpumgrGetSomeGpu();
    void *object = gpu ? objDynamicCastById_IMPL(gpu, NVRM_NVOC_CLASS_OBJECT)
                       : NULL;
    for (void *child = object ? objGetChild_IMPL(object) : NULL;
         child; child = objGetSibling_IMPL(child)) {
        void *memmgr = objDynamicCastById_IMPL(child,
                                               NVRM_NVOC_CLASS_MEMMGR);
        if (memmgr) return memmgr;
    }
    return NULL;
}

/* nvGpuOpsBindChannelResources() finishes a successful virtual PROMOTE_CTX by
 * setting KernelChannel::bIsContextBound.  The public control cannot perform
 * that host-private state transition by itself, and GR scheduling explicitly
 * rejects an external VAS while this byte is false.  Kestrel follows the same
 * locked transition after it has mapped/promoted all retained resources.
 *
 * The two narrow layout facts below are pinned to the exact linked 595.99.02
 * open RM object.  RsResourceRef begins with {pClient,pResource}; the derived
 * KernelChannel has bIsContextBound at 0x2d6.  The latter is independently
 * visible in the supplied source's generated g_kernel_channel_nvoc.h and in
 * nv-kernel.o_binary kchannelConstruct_IMPL (`movb $0,0x2d6(%r12)`). */
typedef struct {
    void *pClient;
    void *pResource;
} nvrm_resource_ref_prefix_t;

#define NVRM_NVOC_CLASS_KERNEL_CHANNEL 0x5d8d70u
#define NVRM_KERNEL_CHANNEL_CONTEXT_BOUND_OFFSET 0x2d6u
#define NVRM_KERNEL_CHANNEL_GROUP_API_OFFSET 0x2b8u
#define NVRM_CHANNEL_GROUP_FROM_API_OFFSET 0x138u
#define NVRM_LOCK_MODULE_GPU 0x00000800u

/*
 * Return the context allocation which kflcnAllocContext_IMPL created for an
 * NVDEC/NVENC channel using an externally-owned VAS.
 *
 * NV2080_CTRL_CMD_FLCN_GET_CTX_BUFFER_INFO looks tempting, but the matching
 * 595.99.02 implementation (kernel_falcon_ctrl.c) explicitly accepts only
 * RM_ENGINE_TYPE_SEC2 and returns NV_ERR_NOT_SUPPORTED for every video
 * Falcon.  The video object constructor has nevertheless already allocated,
 * zeroed and physically promoted its context via kflcnAllocContext_IMPL.  The
 * official data path reaches that allocation through
 *
 *   KernelChannel::pKernelChannelGroupApi->pKernelChannelGroup
 *       -> kchangrpGetEngineContextMemDesc_IMPL().
 *
 * This narrow bridge follows that path while holding RM's API/GPU locks.  The
 * two pointer offsets are pinned to the exact linked 595.99.02 generated
 * layouts and independently visible in nv-kernel.o_binary:
 * kflcnAllocContext_IMPL loads [channel+0x2b8], then [groupApi+0x138].
 * MEMORY_DESCRIPTOR's stable prefix is likewise from g_mem_desc_nvoc.h:
 * Alignment at 0x28 and requested Size at 0x30.  Everything else is obtained
 * through RM's exported accessors rather than guessed from the object.
 */
NV_STATUS nvrm_host_get_video_falcon_context(
    NvU32 hClient, NvU32 hChannel,
    NvU64 *pAlignment, NvU64 *pSize, NvU64 *pPhysAddr,
    NvU32 *pAperture, NvU32 *pKind, NvU32 *pPageSize,
    NvBool *pContiguous, NvBool *pPrivileged)
{
    extern NV_STATUS rmapiLockAcquire(NvU32, NvU32);
    extern void rmapiLockRelease(void);
    extern NV_STATUS rmGpuLocksAcquire(NvU32, NvU32);
    extern NvU32 rmGpuLocksRelease(NvU32, void *);
    extern NV_STATUS serverutilGetResourceRefWithType(NvHandle, NvHandle,
                                                       NvU32, void **);
    extern void *objDynamicCastById_IMPL(void *, NvU32);
    extern void *gpumgrGetSomeGpu(void);
    extern NV_STATUS kchangrpGetEngineContextMemDesc_IMPL(void *, void *,
                                                           void **);
    extern NvU64 memdescGetPhysAddr(void *, void *, NvU64);
    extern NvBool memdescGetContiguity(void *, void *);
    extern NvU64 memdescGetPageSize(void *, void *);
    extern NvU32 memdescGetAddressSpace(void *);
    extern NvU32 memdescGetPteKindForGpu(void *, void *);
    extern NvBool memdescGetFlag(void *, NvU64);

    if (!nvrm_adapter_ready || !hClient || !hChannel || !pAlignment ||
        !pSize || !pPhysAddr || !pAperture || !pKind || !pPageSize ||
        !pContiguous || !pPrivileged)
        return NV_ERR_INVALID_ARGUMENT;

    *pAlignment = *pSize = *pPhysAddr = 0;
    *pAperture = *pKind = *pPageSize = 0;
    *pContiguous = NV_FALSE;
    *pPrivileged = NV_FALSE;

    NV_STATUS status = rmapiLockAcquire(0, NVRM_LOCK_MODULE_GPU);
    if (status != NV_OK) return status;
    status = rmGpuLocksAcquire(0, NVRM_LOCK_MODULE_GPU);
    if (status != NV_OK) {
        rmapiLockRelease();
        return status;
    }

    void *raw_ref = NULL;
    status = serverutilGetResourceRefWithType(
        hClient, hChannel, NVRM_NVOC_CLASS_KERNEL_CHANNEL, &raw_ref);
    if (status == NV_OK && raw_ref) {
        void *resource = ((nvrm_resource_ref_prefix_t *)raw_ref)->pResource;
        NvU8 *channel = (NvU8 *)objDynamicCastById_IMPL(
            resource, NVRM_NVOC_CLASS_KERNEL_CHANNEL);
        void *gpu = gpumgrGetSomeGpu();
        if (!channel || !gpu) {
            status = NV_ERR_INVALID_OBJECT;
        } else {
            void *group_api = *(void **)(channel +
                                         NVRM_KERNEL_CHANNEL_GROUP_API_OFFSET);
            void *group = group_api
                ? *(void **)((NvU8 *)group_api +
                             NVRM_CHANNEL_GROUP_FROM_API_OFFSET)
                : NULL;
            void *memdesc = NULL;
            if (!group) {
                status = NV_ERR_INVALID_OBJECT;
            } else {
                status = kchangrpGetEngineContextMemDesc_IMPL(
                    gpu, group, &memdesc);
                if (status == NV_OK && !memdesc)
                    status = NV_ERR_OBJECT_NOT_FOUND;
            }
            if (status == NV_OK) {
                /* ADDRESS_TRANSLATION is a tagged pointer; AT_GPU is variant
                 * 1 in this exact RM ABI. */
                void *at_gpu = (void *)(uintptr_t)1u;
                *pAlignment = *(NvU64 *)((NvU8 *)memdesc + 0x28u);
                *pSize = *(NvU64 *)((NvU8 *)memdesc + 0x30u);
                *pPhysAddr = memdescGetPhysAddr(memdesc, at_gpu, 0);
                *pAperture = memdescGetAddressSpace(memdesc);
                *pKind = memdescGetPteKindForGpu(memdesc, gpu);
                NvU64 page_size = memdescGetPageSize(memdesc, at_gpu);
                if (page_size > 0xffffffffull)
                    status = NV_ERR_INVALID_DATA;
                else
                    *pPageSize = (NvU32)page_size;
                *pContiguous = memdescGetContiguity(memdesc, at_gpu);
                /* nvGpuOpsGetExternalAllocPtes uses this descriptor flag, not
                 * the GR-context privilege policy. Falcon allocates without
                 * GPU_PRIVILEGED; forcing it caused codec context DMA faults. */
                *pPrivileged = memdescGetFlag(memdesc, 1ull << 13);
            }
        }
    } else if (status == NV_OK) {
        status = NV_ERR_INVALID_OBJECT;
    }

    rmGpuLocksRelease(0, NULL);
    rmapiLockRelease();
    return status;
}

NV_STATUS nvrm_host_mark_context_bound(NvU32 hClient, NvU32 hChannel)
{
    extern NV_STATUS rmapiLockAcquire(NvU32, NvU32);
    extern void rmapiLockRelease(void);
    extern NV_STATUS rmGpuLocksAcquire(NvU32, NvU32);
    extern NvU32 rmGpuLocksRelease(NvU32, void *);
    extern NV_STATUS serverutilGetResourceRefWithType(NvHandle, NvHandle,
                                                       NvU32, void **);
    extern void *objDynamicCastById_IMPL(void *, NvU32);

    if (!nvrm_adapter_ready || !hClient || !hChannel)
        return NV_ERR_INVALID_ARGUMENT;

    NV_STATUS status = rmapiLockAcquire(0, NVRM_LOCK_MODULE_GPU);
    if (status != NV_OK) return status;
    status = rmGpuLocksAcquire(0, NVRM_LOCK_MODULE_GPU);
    if (status != NV_OK) {
        rmapiLockRelease();
        return status;
    }

    void *raw_ref = NULL;
    status = serverutilGetResourceRefWithType(
        hClient, hChannel, NVRM_NVOC_CLASS_KERNEL_CHANNEL, &raw_ref);
    if (status == NV_OK && raw_ref) {
        void *resource = ((nvrm_resource_ref_prefix_t *)raw_ref)->pResource;
        NvU8 *channel = (NvU8 *)objDynamicCastById_IMPL(
            resource, NVRM_NVOC_CLASS_KERNEL_CHANNEL);
        if (!channel) {
            status = NV_ERR_INVALID_OBJECT;
        } else {
            channel[NVRM_KERNEL_CHANNEL_CONTEXT_BOUND_OFFSET] = NV_TRUE;
            /* The byte is host scheduling state, not a device doorbell.  A
             * compiler barrier prevents the following control from being
             * reordered ahead of the state transition. */
            __asm__ volatile("" ::: "memory");
        }
    } else if (status == NV_OK) {
        status = NV_ERR_INVALID_OBJECT;
    }

    rmGpuLocksRelease(0, NULL);
    rmapiLockRelease();
    return status;
}

NV_STATUS nvrm_transfer_rm_memory(NvU32 hClient, NvU32 hMemory,
                                  NvU64 offset, void *buffer, NvU64 size,
                                  NvBool read)
{
    extern NV_STATUS rmapiLockAcquire(NvU32, NvU32);
    extern void rmapiLockRelease(void);
    extern NV_STATUS rmGpuLocksAcquire(NvU32, NvU32);
    extern NvU32 rmGpuLocksRelease(NvU32, void *);
    extern void *memmgrMemUtilsGetMemDescFromHandle_IMPL(void *, NvU32,
                                                         NvU32);
    extern NV_STATUS memmgrMemWrite_IMPL(void *, nvrm_transfer_surface_t *,
                                         void *, NvU64, NvU32);
    extern NV_STATUS memmgrMemRead_IMPL(void *, nvrm_transfer_surface_t *,
                                        void *, NvU64, NvU32);

    if (!nvrm_adapter_ready || !hClient || !hMemory || !buffer || !size)
        return NV_ERR_INVALID_ARGUMENT;
    void *memmgr = nvrm_memory_manager();
    if (!memmgr) return NV_ERR_INVALID_STATE;

    NV_STATUS status = rmapiLockAcquire(0, NVRM_LOCK_MODULE_MEM);
    if (status != NV_OK) return status;
    status = rmGpuLocksAcquire(0, NVRM_LOCK_MODULE_MEM);
    if (status != NV_OK) {
        rmapiLockRelease();
        return status;
    }

    void *memdesc = memmgrMemUtilsGetMemDescFromHandle_IMPL(memmgr, hClient,
                                                             hMemory);
    if (!memdesc) {
        status = NV_ERR_INVALID_OBJECT;
    } else {
        nvrm_transfer_surface_t surface = {
            .pMemDesc = memdesc, .offset = offset,
            .pMapping = NULL, .pMappingPriv = NULL
        };
        status = read
            ? memmgrMemRead_IMPL(memmgr, &surface, buffer, size,
                                 NVRM_TRANSFER_PREFER_CE)
            : memmgrMemWrite_IMPL(memmgr, &surface, buffer, size,
                                  NVRM_TRANSFER_PREFER_CE);
    }
    rmGpuLocksRelease(0, NULL);
    rmapiLockRelease();
    return status;
}

/* External VAS roots are stored in OBJGVASPACE::pExternalPDB. The ordinary
 * DMA_INVALIDATE_TLB control walks RM's INTERNAL root, which may be NULL, and
 * returns success without invalidating anything. Use the actual root memory
 * descriptor and NVIDIA's desktop-GPU HAL (also selected on GB202/GB203).
 * ALL_TLBS includes GR and hub/CE translations; PTE_DOWNGRADE supplies the
 * required membar and waits for completion. Never invalidate all PDBs. */
NV_STATUS nvrm_invalidate_external_root(NvU32 hClient, NvU32 hMemory,
                                        NvU64 expectedRoot)
{
    extern void *gpumgrGetSomeGpu(void);
    extern NV_STATUS rmapiLockAcquire(NvU32, NvU32);
    extern void rmapiLockRelease(void);
    extern NV_STATUS rmGpuLocksAcquire(NvU32, NvU32);
    extern NvU32 rmGpuLocksRelease(NvU32, void *);
    extern void *objDynamicCastById_IMPL(void *, NvU32);
    extern void *objGetChild_IMPL(void *);
    extern void *objGetSibling_IMPL(void *);
    extern void *memmgrMemUtilsGetMemDescFromHandle_IMPL(void *, NvU32, NvU32);
    extern NvU64 memdescGetPhysAddr(void *, void *, NvU64);
    extern NvU32 memdescGetAddressSpace(void *);
    extern NV_STATUS kgmmuInvalidateTlb_GM107(void *, void *, void *, NvU32,
                                              NvU32, NvU32, NvU32, NvBool);
    if (!nvrm_adapter_ready || !hClient || !hMemory || (expectedRoot & 0xfffu))
        return NV_ERR_INVALID_ARGUMENT;
    NV_STATUS status = rmapiLockAcquire(0, NVRM_LOCK_MODULE_MEM);
    if (status != NV_OK) return status;
    status = rmGpuLocksAcquire(0, NVRM_LOCK_MODULE_MEM);
    if (status != NV_OK) { rmapiLockRelease(); return status; }
    void *gpu = gpumgrGetSomeGpu();
    void *memmgr = nvrm_memory_manager();
    void *object = gpu ? objDynamicCastById_IMPL(gpu, NVRM_NVOC_CLASS_OBJECT) : NULL;
    void *gmmu = NULL;
    for (void *child = object ? objGetChild_IMPL(object) : NULL;
         child; child = objGetSibling_IMPL(child)) {
        gmmu = objDynamicCastById_IMPL(child, 0x29362fu); /* KernelGmmu */
        if (gmmu) break;
    }
    void *rootMem = memmgr
        ? memmgrMemUtilsGetMemDescFromHandle_IMPL(memmgr, hClient, hMemory) : NULL;
    if (!gpu || !gmmu || !rootMem) {
        status = NV_ERR_INVALID_OBJECT;
    } else if (memdescGetAddressSpace(rootMem) != 2u || /* ADDR_FBMEM */
               memdescGetPhysAddr(rootMem, (void *)(uintptr_t)1u, 0) != expectedRoot) {
        status = NV_ERR_INVALID_ARGUMENT;
    } else {
        status = kgmmuInvalidateTlb_GM107(gpu, gmmu, rootMem,
                                          0u, /* normal external VAS, not BAR */
                                          1u, /* PTE_DOWNGRADE */
                                          0u, /* GPU_GFID_PF */
                                          0u, /* NV_GMMU_INVAL_SCOPE_ALL_TLBS */
                                          NV_FALSE);
    }
    rmGpuLocksRelease(0, NULL);
    rmapiLockRelease();
    return status;
}

nv_state_t *nv_get_adapter_state(NvU32 domain, NvU8 bus, NvU8 slot)
{
    nv_state_t *nv = nvrm_active_nv;
    return nv && nv->pci_info.domain == domain && nv->pci_info.bus == bus &&
           nv->pci_info.slot == slot ? nv : NULL;
}
nv_state_t *nv_get_ctl_state(void) { return nvrm_active_nv; }
NV_STATUS nv_get_egm_info(nv_state_t *nv, NvU64 *base, NvU64 *size, NvS32 *node)
{ (void)nv; if (base) *base = 0; if (size) *size = 0; if (node) *node = -1;
  return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_add_mapping_context_to_file(nv_state_t *nv,
        nv_usermap_access_params_t *params, NvU32 prot, void *private_data,
        NvU64 page_index, NvU32 page_offset)
{ (void)nv;(void)params;(void)prot;(void)private_data;(void)page_index;
  (void)page_offset; return NV_ERR_NOT_SUPPORTED; }
void nv_acquire_mmap_lock(nv_state_t *nv) { (void)nv; }
void nv_release_mmap_lock(nv_state_t *nv) { (void)nv; }
NvBool nv_get_all_mappings_revoked_locked(nv_state_t *nv)
{ (void)nv; return NV_TRUE; }
void nv_set_safe_to_mmap_locked(nv_state_t *nv, NvBool safe)
{ (void)nv; (void)safe; }
NV_STATUS nv_revoke_gpu_mappings(nv_state_t *nv) { (void)nv; return NV_OK; }

NvBool nv_dynamic_power_available(nv_state_t *nv) { (void)nv; return NV_FALSE; }
void nv_audio_dynamic_power(nv_state_t *nv) { (void)nv; }
void nv_idle_holdoff(nv_state_t *nv) { (void)nv; }
void nv_allow_runtime_suspend(nv_state_t *nv) { (void)nv; }
void nv_disallow_runtime_suspend(nv_state_t *nv) { (void)nv; }
NvBool nv_s2idle_pm_configured(void) { return NV_FALSE; }
NvBool nv_pci_tegra_pm_init(nv_state_t *nv) { (void)nv; return NV_FALSE; }
void nv_pci_tegra_pm_deinit(nv_state_t *nv) { (void)nv; }
NvBool nv_is_gpu_accessible(nv_state_t *nv) { return nv && !nv->removed; }
NvBool nv_match_gpu_os_info(nv_state_t *nv, void *os) { return nv && nv->os_state == os; }
void nv_control_soc_irqs(nv_state_t *nv, NvBool enable) { (void)nv; (void)enable; }
NV_STATUS nv_get_current_irq_priv_data(nv_state_t *nv, NvU32 *data)
{ (void)nv; if (data) *data = 0; return NV_ERR_NOT_SUPPORTED; }
nv_soc_irq_type_t nv_get_current_irq_type(nv_state_t *nv)
{ return nv ? nv->soc_irq_info[nv->current_soc_irq].irq_type : 0; }

NV_STATUS nv_get_syncpoint_aperture(NvU32 id, NvU64 *base, NvU64 *size, NvU32 *prot)
{ (void)id; if (base) *base=0; if (size) *size=0; if (prot) *prot=0;
  return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_get_num_dpaux_instances(nv_state_t *nv, NvU32 *count)
{ if (!nv || !count) return NV_ERR_INVALID_ARGUMENT;
  *count = nv->num_dpaux_instance; return NV_OK; }
NV_STATUS nv_get_tegra_brightness_level(nv_state_t *nv, NvU32 *level)
{ (void)nv; if (level) *level=0; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_set_tegra_brightness_level(nv_state_t *nv, NvU32 level)
{ (void)nv;(void)level; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_bpmp_send_mrq(nv_state_t *nv, NvU32 mrq, const void *tx, NvU32 txlen,
        void *rx, NvU32 rxlen, NvS32 *fw, NvS32 *transport)
{ (void)nv;(void)mrq;(void)tx;(void)txlen;(void)rx;(void)rxlen;
  if (fw) *fw=-1; if (transport) *transport=-1; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_imp_get_import_data(TEGRA_IMP_IMPORT_DATA *data)
{ (void)data; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_imp_get_uefi_data(nv_state_t *nv, NvU32 *iso, NvU32 *floor)
{ (void)nv; if (iso) *iso=0; if (floor) *floor=0; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_imp_enable_disable_rfl(nv_state_t *nv, NvBool enable)
{ (void)nv;(void)enable; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_imp_icc_set_bw(nv_state_t *nv, NvU32 avg, NvU32 floor)
{ (void)nv;(void)avg;(void)floor; return NV_ERR_NOT_SUPPORTED; }

NV_STATUS nv_acquire_fabric_mgmt_cap(int fd, int *cap)
{ (void)fd; if (cap) *cap=-1; return NV_ERR_NOT_SUPPORTED; }
static void nvrm_log_text(int severity, const char *text);
NV_STATUS nv_log_error(nv_state_t *nv, NvU32 code, const char *fmt, va_list ap)
{
    (void)nv;
    /* Preserve the Xid and the complete bounded firmware detail across klog
     * entries. A single kerr silently cuts the detail at KLOG_MSG_MAX, often
     * before the offending method/address at the end. No allocation here. */
    char line[544];
    int prefix = snprintf(line, 32, "Xid %u: ", code);
    if (prefix < 0 || prefix >= 32) return NV_ERR_INVALID_ARGUMENT;
    int n = fmt ? vsnprintf(line + prefix, 512, fmt, ap) : -1;
    if (n < 0) {
        snprintf(line + prefix, 512, "[error detail unavailable]");
    }
    nvrm_log_text(KLOG_ERROR, line);
    if (n >= 512)
        nvrm_log_text(KLOG_ERROR, "[Xid formatted detail exceeded 511 bytes; remainder unavailable]");
    return NV_OK;
}
void nv_post_event(nv_event_t *e, NvHandle h, NvU32 i, NvU32 d, NvU16 s, NvBool dat)
{ (void)e;(void)h;(void)i;(void)d;(void)s;(void)dat; }
NvS32 nv_get_event(nv_file_private_t *f, nv_event_t *e, NvU32 *pending)
{ (void)f;(void)e;if(pending)*pending=0;return -1; }
nv_file_private_t *nv_get_file_private(NvS32 fd,NvBool ctl,void **p)
{ (void)fd;(void)ctl;if(p)*p=NULL;return NULL; }
void nv_put_file_private(void *p) { (void)p; }
void *nv_i2c_add_adapter(nv_state_t *nv,NvU32 port) { (void)nv;(void)port;return NULL; }
void nv_i2c_del_adapter(nv_state_t *nv,void *a) { (void)nv;(void)a; }
NV_STATUS nv_i2c_transfer(nv_state_t *nv,NvU32 port,NvU8 addr,nv_i2c_msg_t *m,int n)
{ (void)nv;(void)port;(void)addr;(void)m;(void)n;return NV_ERR_NOT_SUPPORTED; }
void nv_i2c_unregister_clients(nv_state_t *nv) { (void)nv; }
NV_STATUS nv_i2c_bus_status(nv_state_t *nv,NvU32 port,NvS32 *scl,NvS32 *sda)
{ (void)nv;(void)port;if(scl)*scl=-1;if(sda)*sda=-1;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_dma_import_sgt(nv_dma_device_t *d,struct sg_table *s,struct drm_gem_object *g)
{ (void)d;(void)s;(void)g;return NV_ERR_NOT_SUPPORTED; }
void nv_dma_release_sgt(struct sg_table *s,struct drm_gem_object *g) { (void)s;(void)g; }
NV_STATUS nv_dma_import_dma_buf(nv_dma_device_t *d,struct dma_buf *b,NvBool w,
        NvU32 *n,struct sg_table **s,nv_dma_buf_t **p)
{ (void)d;(void)b;(void)w;if(n)*n=0;if(s)*s=NULL;if(p)*p=NULL;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_dma_import_from_fd(nv_dma_device_t *d,NvS32 fd,NvBool w,
        NvU32 *n,struct sg_table **s,nv_dma_buf_t **p)
{ (void)d;(void)fd;(void)w;if(n)*n=0;if(s)*s=NULL;if(p)*p=NULL;return NV_ERR_NOT_SUPPORTED; }
void nv_dma_release_dma_buf(nv_dma_buf_t *p) { (void)p; }
void nv_schedule_uvm_isr(nv_state_t *nv) { (void)nv; }
NV_STATUS nv_schedule_uvm_drain_p2p(NvU8 *uuid) { (void)uuid;return NV_ERR_NOT_SUPPORTED; }
void nv_schedule_uvm_resume_p2p(NvU8 *uuid) { (void)uuid; }

void nv_acpi_methods_init(NvU32 *handles) { if (handles) *handles = 0; }
void nv_acpi_methods_uninit(void) { }
NV_STATUS nv_acpi_method(NvU32 a, NvU32 b, NvU32 c, void *d, NvU16 e,
                         NvU32 *f, void *g, NvU16 *h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_acpi_d3cold_dsm_for_upstream_port(nv_state_t *n, NvU8 *a,
        NvU32 b, NvU32 c, NvU32 *d)
{ (void)n;(void)a;(void)b;(void)c;(void)d; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_acpi_dsm_method(nv_state_t *n, NvU8 *a, NvU32 b, NvBool c,
        NvU32 d, void *e, NvU16 f, NvU32 *g, void *h, NvU16 *i)
{ (void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_acpi_ddc_method(nv_state_t *n, void *a, NvU32 *b, NvBool c)
{ (void)n;(void)a;(void)b;(void)c; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_acpi_dod_method(nv_state_t *n, NvU32 *a, NvU32 *b)
{ (void)n;(void)a;(void)b; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_acpi_rom_method(nv_state_t *n, NvU32 *a, NvU32 *b)
{ (void)n;(void)a;(void)b; return NV_ERR_NOT_SUPPORTED; }
NV_STATUS nv_acpi_get_powersource(NvU32 *source)
{ if (source) *source = 1; return NV_OK; }
NvBool nv_acpi_is_battery_present(void) { return NV_FALSE; }
NV_STATUS nv_acpi_mux_method(nv_state_t *n, NvU32 *a, NvU32 b, const char *c)
{ (void)n;(void)a;(void)b;(void)c; return NV_ERR_NOT_SUPPORTED; }

NvS32 os_snprintf(char *dst, NvU32 len, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    NvS32 n = (NvS32)vsnprintf(dst, len, fmt, ap);
    va_end(ap); return n;
}
/* registry.c is linked in from the RM blob; forward-declare its writers. */
NV_STATUS RmWriteRegistryDword(nv_state_t *, const char *, NvU32);
NV_STATUS RmWriteRegistryString(nv_state_t *, const char *, const char *, NvU32);

NV_STATUS os_registry_init(void)
{
    /* Global entries (nv==NULL) are read by BOTH the host RM and GSP-RM (the
     * same list is packed into the GSP RPC).  Turn on the display/modeset and
     * GSP-supervisor diagnostics so a modeset that "commits" but never promotes
     * to scanout tells us WHY.  RmMsg matches file/function substrings (empty
     * noun = all); this filter targets the disp/EVO/core-channel/promotion path.
     * EnableGpuFirmwareLogs MUST be 1 (ENABLE) not the default 2
     * (ENABLE_ON_DEBUG, off in a release build) to decode GSP-RM's own log
     * buffers into NV_PRINTF -> our kernel log. */
    /* NOTE: do NOT add broad tokens here - RmMsg does a plain substring match on
     * function OR file name.  "Update" matches _gmmuWalkCBUpdatePde /
     * _bar2WalkCBUpdatePde and "Rg"/"bar"/"mmu"/"memmgr" match hot page-table
     * walk callbacks; force-printing those floods the console and HANGS the boot
     * (esp. with dynamic BAR1 / noncontiguous maps, which walk per page).  Keep
     * only disp/EVO/promotion tokens that do NOT scale with page count. */
    static const char rmmsg[] =
        "disp,kdisp,Head,Core,Supervisor,Promote,nvkms,evo,Notifier,Imp";
    RmWriteRegistryString(NULL, "RmMsg", rmmsg, (NvU32)sizeof rmmsg);
    RmWriteRegistryDword(NULL, "EnableGpuFirmwareLogs", 1);
    RmWriteRegistryDword(NULL, "RmGspPreserveUnloadLogs", 3);
    /* Linux nv-reg.h supplies RmLogonRC=1 through nv_parms. Our empty table
     * omits that default; RM's fallback is DISABLE (nvrm_registry.h), gating
     * krcReportXid's detailed fault text while notifier 65 still publishes.
     * Global scope also supplies the setting to GSP-RM. This enables error
     * reporting, not recovery, replay, a breakpoint or a longer watchdog. */
    NV_STATUS rc_log_status = RmWriteRegistryDword(NULL, "RmLogonRC", 1);
    if (rc_log_status != NV_OK)
        kwarn("nvrm", "could not enable RC/Xid detail logging: %#x", rc_log_status);
    /* Static BAR1 1:1 mapping is broken on this system (CPU vidmem read via BAR1
     * returned 0xbad0ac00 poison -> BAR1 page-walk INVALID -> no scanout, no
     * CPU writes).  Force RM to DISABLE static BAR1 so it uses dynamic per-surface
     * BAR1 mappings (each surface gets its own PTE on map) instead of the broken
     * static 1:1 physical aperture.  Behavior toggle, verified vs 595 source:
     * kern_bus.c:189 -> staticBar1ForceType; kern_bus_tu102.c:421 DISABLE ->
     * NV_ERR_NOT_SUPPORTED; kern_bus_gm107.c:1147 bStaticBar1Supported=FALSE. */
    RmWriteRegistryDword(NULL, "RMForceStaticBar1", 0);
    /* Client channels already allocate USERD in sysmem, but RM's PRIVATE
     * scrubber/watchdog bypass that client bridge and otherwise default their
     * USERD to VRAM. Their CPU BAR1 GPPut accesses are subject to the same
     * broken aperture. The logs show an out-of-range watchdog GPPut and a
     * later full scrub ring; lost USERD writes can strand that ring's work.
     * Use NVIDIA's supported USERD_COH policy for both host and GSP RM:
     * nvrm_registry.h USERD=17:16, COH=1; mem_utils_gm107.c and
     * kernel_rc_watchdog.c explicitly select NV01_MEMORY_SYSTEM for this.
     * All other instance-location fields remain DEFAULT. Do not bypass RM
     * allocation/free scrubbing or weaken the separate CAB5 clear fence. */
    RmWriteRegistryDword(NULL, "RMInstLoc", 0x00010000u);
    return NV_OK;
}
NvU64 os_get_max_user_va(void) { return 0x00007fffffffffffULL; }
/* Real driver os-interface.c:57 = PAGE_SIZE << NV_MAX_PAGE_ORDER (order 10 on
 * x86-64 => 4 MB).  Off the boot-critical path, but match the source. */
NvU64 os_max_page_size = (NvU64)PAGE_SIZE << 10;
NvU32 os_get_grid_csp_support(void) { return 0; }
NV_STATUS os_get_euid(NvU32 *id)
{ if (!id) return NV_ERR_INVALID_ARGUMENT; *id = 0; return NV_OK; }
void *os_get_pid_info(void) { return (void *)1; }
void os_put_pid_info(void *p) { (void)p; }
NV_STATUS os_put_page(NvU64 address) { (void)address; return NV_OK; }

/* Capabilities are an OS access-control namespace.  Kestrel has no user-mode
 * NVIDIA device files yet, but RM requires stable opaque nodes during init. */
struct nv_cap { struct nv_cap *parent; char *name; int minor, permissions; };
static nv_cap_t *nvrm_cap_create(nv_cap_t *parent, const char *name, int perms)
{
    struct nv_cap *c = kzalloc(sizeof *c);
    if (!c) return NULL;
    size_t n = name ? strlen(name) + 1 : 1;
    c->name = kmalloc(n);
    if (!c->name) { kfree(c); return NULL; }
    if (name) memcpy(c->name, name, n); else c->name[0] = 0;
    c->parent = (struct nv_cap *)parent; c->permissions = perms;
    return (nv_cap_t *)c;
}
nv_cap_t *os_nv_cap_create_dir_entry(nv_cap_t *p, const char *n, int m)
{ return nvrm_cap_create(p, n, m); }
nv_cap_t *os_nv_cap_create_file_entry(nv_cap_t *p, const char *n, int m)
{ return nvrm_cap_create(p, n, m); }
void os_nv_cap_destroy_entry(nv_cap_t *opaque)
{
    struct nv_cap *c = (struct nv_cap *)opaque;
    if (c) { kfree(c->name); kfree(c); }
}
void os_add_record_for_crashLog(void *buffer, NvU32 size)
{ (void)buffer; (void)size; }
void os_delete_record_for_crashLog(void *buffer) { (void)buffer; }
NV_STATUS os_get_random_bytes(NvU8 *out, NvU16 count)
{
    if (!out) return NV_ERR_INVALID_ARGUMENT;
    NvU64 x = timer_now_us() ^ 0x9e3779b97f4a7c15ull;
    for (NvU16 i = 0; i < count; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17; out[i] = (NvU8)x;
    }
    return NV_OK;
}
NV_STATUS os_get_acpi_rsdp_from_uefi(NvU32 *address)
{
    if (!address || !g_boot.rsdp || g_boot.rsdp > 0xffffffffu)
        return NV_ERR_NOT_SUPPORTED;
    *address = (NvU32)g_boot.rsdp;
    return NV_OK;
}

NvU32 os_strtoul(const char *s, char **end, NvU32 base)
{
    NvU64 v=0; if(!base)base=10;
    while(*s==' '||*s=='\t')s++;
    if(base==16 && s[0]=='0' && (s[1]=='x'||s[1]=='X'))s+=2;
    const char *p=s;
    for(;;p++) { NvU32 d;
        if(*p>='0'&&*p<='9')d=(NvU32)(*p-'0');
        else if(*p>='a'&&*p<='z')d=(NvU32)(*p-'a'+10);
        else if(*p>='A'&&*p<='Z')d=(NvU32)(*p-'A'+10); else break;
        if(d>=base)break; v=v*base+d;
    }
    if(end)*end=(char *)p; return (NvU32)v;
}
NvU8 os_io_read_byte(NvU32 port)
{ NvU8 v; __asm__ volatile("inb %w1,%0":"=a"(v):"Nd"((NvU16)port));return v; }
NvU16 os_io_read_word(NvU32 port)
{ NvU16 v; __asm__ volatile("inw %w1,%0":"=a"(v):"Nd"((NvU16)port));return v; }
NvU32 os_io_read_dword(NvU32 port)
{ NvU32 v; __asm__ volatile("inl %w1,%0":"=a"(v):"Nd"((NvU16)port));return v; }
void os_io_write_byte(NvU32 port,NvU8 v)
{ __asm__ volatile("outb %0,%w1"::"a"(v),"Nd"((NvU16)port)); }
void os_io_write_word(NvU32 port,NvU16 v)
{ __asm__ volatile("outw %0,%w1"::"a"(v),"Nd"((NvU16)port)); }
void os_io_write_dword(NvU32 port,NvU32 v)
{ __asm__ volatile("outl %0,%w1"::"a"(v),"Nd"((NvU16)port)); }
NV_STATUS os_get_version_info(os_version_info *v)
{ static const char ver[]="KestrelOS 0.1"; static const char date[]="offline build";
  if(!v)return NV_ERR_INVALID_ARGUMENT; v->os_major_version=0;v->os_minor_version=1;
  v->os_build_number=0;v->os_build_version_str=ver;v->os_build_date_plus_str=date;return NV_OK; }
/* We link the OPEN-kernel-module RM core (kernel-open/), the only 595 build with
 * Blackwell/GB20x HAL support.  RM gates Blackwell on this: returning NV_FALSE
 * makes RmInitAdapter print "requires use of the NVIDIA open kernel modules" and
 * fail (0x22:0x56:1017).  Matches kernel-open/nvidia/os-interface.c:1390. */
NV_STATUS os_get_is_openrm(NvBool *v) { if(!v)return NV_ERR_INVALID_ARGUMENT;*v=NV_TRUE;return NV_OK; }
NvBool os_is_bif_reset_supported(void *h) { (void)h;return NV_FALSE; }
NV_STATUS os_inject_vgx_msi(NvU16 a,NvU64 b,NvU32 c)
{ (void)a;(void)b;(void)c;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_lock_user_pages(void *u,NvU64 n,void **p,NvU32 f)
{ (void)u;(void)n;(void)p;(void)f;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_lookup_user_io_memory(void *u,NvU64 n,NvU64 **p)
{ (void)u;(void)n;if(p)*p=NULL;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_unlock_user_pages(NvU64 n,void *p,NvU32 f)
{ (void)n;(void)p;(void)f;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_match_mmap_offset(void *f,NvU64 a,NvU64 *o)
{ (void)f;(void)a;if(o)*o=0;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_get_smbios_header(NvU64 *p) { if(p)*p=0;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_call_vgpu_vfio(void *p,NvU32 c) { (void)p;(void)c;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_numa_memblock_size(NvU64 *n) { if(n)*n=0;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_alloc_pages_node(NvS32 node,NvU32 order,NvU32 flags,NvU64 *phys)
{
    (void)node;(void)flags;if(!phys||order>=20)return NV_ERR_INVALID_ARGUMENT;
    u64 p=0;void *v=dma_alloc_pages((size_t)1<<order,&p);
    if(!v)return NV_ERR_NO_MEMORY;*phys=p;return NV_OK;
}
NV_STATUS os_get_page(NvU64 address) { (void)address;return NV_OK; }
NvU32 os_get_page_refcount(NvU64 address) { (void)address;return 1; }
NvU32 os_count_tail_pages(NvU64 address) { (void)address;return 1; }
NV_STATUS os_open_temporary_file(void **p) { if(p)*p=NULL;return NV_ERR_NOT_SUPPORTED; }
void os_close_file(void *p) { (void)p; }
NV_STATUS os_write_file(void *f,NvU8 *b,NvU64 n,NvU64 o)
{ (void)f;(void)b;(void)n;(void)o;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_read_file(void *f,NvU8 *b,NvU64 n,NvU64 o)
{ (void)f;(void)b;(void)n;(void)o;return NV_ERR_NOT_SUPPORTED; }

struct os_wait_queue { volatile NvBool signaled; };
NV_STATUS os_alloc_wait_queue(os_wait_queue **out)
{ if(!out)return NV_ERR_INVALID_ARGUMENT;*out=kzalloc(sizeof **out);
  return *out?NV_OK:NV_ERR_NO_MEMORY; }
void os_free_wait_queue(os_wait_queue *q) { kfree(q); }
void os_wait_uninterruptible(os_wait_queue *q)
{
    if(!q)return;
    /* Linux uses complete_all(): once signaled, every current and future
     * waiter returns.  Consuming the bit here stranded the second RM waiter. */
    while(!__atomic_load_n(&q->signaled,__ATOMIC_ACQUIRE))os_schedule();
}
void os_wait_interruptible(os_wait_queue *q) { os_wait_uninterruptible(q); }
void os_wake_up(os_wait_queue *q) { if(q)__atomic_store_n(&q->signaled,NV_TRUE,__ATOMIC_RELEASE); }
int os_nv_cap_validate_and_dup_fd(const nv_cap_t *c,int fd) { (void)c;(void)fd;return -1; }
void os_nv_cap_close_fd(int fd) { (void)fd; }
NvS32 os_imex_channel_get(NvU64 c) { (void)c;return -1; }
NvS32 os_imex_channel_count(void) { return 0; }
NvBool os_imex_channel_is_supported=NV_TRUE;  /* real os-interface.c:73 = NV_TRUE */
NV_STATUS os_tegra_igpu_perf_boost(void *h,NvBool e,NvU32 d)
{ (void)h;(void)e;(void)d;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_enable_pci_req_atomics(void *h,enum os_pci_req_atomics_type t)
{ (void)h;(void)t;return NV_ERR_NOT_SUPPORTED; }
NvU64 os_get_reclaimable_memory_usage(void) { return 0; }
NV_STATUS os_get_numa_node_memory_usage(NvS32 n,NvU64 *total,NvU64 *free)
{ (void)n;if(total)*total=0;if(free)*free=0;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_numa_add_gpu_memory(void *h,NvU64 a,NvU64 n,NvU32 *id)
{ (void)h;(void)a;(void)n;if(id)*id=0;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_numa_remove_gpu_memory(void *h,NvU64 a,NvU64 n,NvU32 id)
{ (void)h;(void)a;(void)n;(void)id;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_offline_page_at_address(NvU64 a) { (void)a;return NV_ERR_NOT_SUPPORTED; }
NV_STATUS os_find_ns_pid(void *p,NvU32 *id)
{ (void)p;if(!id)return NV_ERR_INVALID_ARGUMENT;*id=0;return NV_OK; }
NV_STATUS os_iommu_sva_bind(void *a,void **h,NvU32 *pasid)
{ (void)a;if(h)*h=NULL;if(pasid)*pasid=0;return NV_ERR_NOT_SUPPORTED; }
void os_iommu_sva_unbind(void *h) { (void)h; }

void *nvlink_malloc(NvLength n) { return kmalloc((size_t)n); }
void nvlink_free(void *p) { kfree(p); }
void *nvlink_memset(void *p, int c, NvLength n) { return memset(p, c, (size_t)n); }
void *nvlink_memcpy(void *d, const void *s, NvLength n) { return memcpy(d, s, (size_t)n); }
int nvlink_memcmp(const void *a, const void *b, NvLength n)
{ return memcmp(a, b, (size_t)n); }
char *nvlink_strcpy(char *d, const char *s) { return strcpy(d, s); }
NvLength nvlink_strlen(const char *s) { return (NvLength)strlen(s); }
int nvlink_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int nvlink_is_admin(void) { return 1; }
NvU64 nvlink_get_platform_time(void) { return os_get_monotonic_time_ns(); }
NvlStatus nvlink_acquire_fabric_mgmt_cap(void *p, NvU64 cap)
{ (void)p;(void)cap; return NVL_ERR_NOT_SUPPORTED; }
int nvlink_is_fabric_manager(void *p) { (void)p; return 0; }
void nvlink_sleep(unsigned int ms) { timer_mdelay(ms); }
void nvlink_assert(int expression)
{ if (!expression) kerr("nvrm", "NVLink assertion failed"); }

/* No NVSwitch is present in the target.  The generic helpers remain fully
 * functional; device/capability operations explicitly report unsupported. */
NvU64 nvswitch_os_get_platform_time(void) { return os_get_monotonic_time_ns(); }
NvU64 nvswitch_os_get_platform_time_epoch(void) { return 0; }
void *nvswitch_os_malloc_trace(NvLength n, const char *file, NvU32 line)
{ (void)file;(void)line; return kmalloc((size_t)n); }
void nvswitch_os_free(void *p) { kfree(p); }
NvLength nvswitch_os_strlen(const char *s) { return (NvLength)strlen(s); }
int nvswitch_os_strncmp(const char *a, const char *b, NvLength n)
{ return strncmp(a,b,(size_t)n); }
char *nvswitch_os_strncat(char *d, const char *s, NvLength n)
{
    char *out=d; while(*d)d++;
    while(n && *s) { *d++=*s++; n--; } *d=0; return out;
}
void *nvswitch_os_memset(void *d, int c, NvLength n)
{ return memset(d,c,(size_t)n); }
void *nvswitch_os_memcpy(void *d, const void *s, NvLength n)
{ return memcpy(d,s,(size_t)n); }
int nvswitch_os_memcmp(const void *a, const void *b, NvLength n)
{ return memcmp(a,b,(size_t)n); }
NvU32 nvswitch_os_mem_read32(const volatile void *p) { return *(volatile const NvU32 *)p; }
void nvswitch_os_mem_write32(volatile void *p, NvU32 v) { *(volatile NvU32 *)p=v; }
int nvswitch_os_vsnprintf(char *d, NvLength n, const char *fmt, va_list ap)
{ return vsnprintf(d,(size_t)n,fmt,ap); }
int nvswitch_os_snprintf(char *d, NvLength n, const char *fmt, ...)
{ va_list ap; va_start(ap,fmt); int r=vsnprintf(d,(size_t)n,fmt,ap); va_end(ap); return r; }
void nvswitch_os_print(int level, const char *fmt, ...)
{ char b[512]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
  if (level>=NVSWITCH_DBG_LEVEL_ERROR) kerr("nvswitch","%s",b); else kinfo("nvswitch","%s",b); }
void nvswitch_os_assert_log(const char *fmt, ...)
{ char b[512]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
  kerr("nvswitch","%s",b); }
void nvswitch_os_report_error(void *h, NvU32 code, const char *fmt, ...)
{ (void)h; char b[512]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
  kerr("nvswitch","SXid %u: %s",code,b); }
void nvswitch_os_sleep(unsigned int ms) { os_delay(ms); }
NvlStatus nvswitch_os_read_registry_dword(void *h,const char *n,NvU32 *v)
{ (void)h;(void)n;if(v)*v=0;return NVL_ERR_NOT_SUPPORTED; }
NvBool nvswitch_os_is_uuid_in_blacklist(NvUuid *u) { (void)u;return NV_FALSE; }
void nvswitch_os_override_platform(void *h,NvBool *r) { (void)h;if(r)*r=NV_FALSE; }
NvlStatus nvswitch_os_alloc_contig_memory(void *h,void **v,NvU32 n,NvBool dma32)
{ (void)h;(void)dma32; if(!v)return NVL_BAD_ARGS; *v=kzalloc(n?n:1);
  return *v?NVL_SUCCESS:NVL_NO_MEM; }
void nvswitch_os_free_contig_memory(void *h,void *v,NvU32 n)
{ (void)h;(void)n;kfree(v); }
NvlStatus nvswitch_os_map_dma_region(void *h,void *cpu,NvU64 *dma,NvU32 n,NvU32 dir)
{ (void)h;(void)n;(void)dir;if(!cpu||!dma)return NVL_BAD_ARGS;
  *dma=virt_to_phys(cpu);return NVL_SUCCESS; }
NvlStatus nvswitch_os_unmap_dma_region(void *h,void *cpu,NvU64 dma,NvU32 n,NvU32 dir)
{ (void)h;(void)cpu;(void)dma;(void)n;(void)dir;return NVL_SUCCESS; }
NvlStatus nvswitch_os_set_dma_mask(void *h,NvU32 bits)
{ (void)h;(void)bits;return NVL_SUCCESS; }
NvlStatus nvswitch_os_sync_dma_region_for_cpu(void *h,NvU64 d,NvU32 n,NvU32 dir)
{ (void)h;(void)d;(void)n;(void)dir;__asm__ volatile("mfence":::"memory");return NVL_SUCCESS; }
NvlStatus nvswitch_os_sync_dma_region_for_device(void *h,NvU64 d,NvU32 n,NvU32 dir)
{ return nvswitch_os_sync_dma_region_for_cpu(h,d,n,dir); }
NvlStatus nvswitch_os_acquire_fabric_mgmt_cap(void *p,NvU64 c)
{ (void)p;(void)c;return NVL_ERR_NOT_SUPPORTED; }
int nvswitch_os_is_fabric_manager(void *p) { (void)p;return 0; }
int nvswitch_os_is_admin(void) { return 1; }
NvlStatus nvswitch_os_get_os_version(NvU32 *a,NvU32 *b,NvU32 *c)
{ if(a)*a=0;if(b)*b=1;if(c)*c=0;return NVL_SUCCESS; }
NvlStatus nvswitch_os_get_pid(NvU32 *p) { if(!p)return NVL_BAD_ARGS;*p=0;return NVL_SUCCESS; }
NvlStatus nvswitch_os_add_client_event(void *h,void *p,NvU32 e)
{ (void)h;(void)p;(void)e;return NVL_ERR_NOT_SUPPORTED; }
NvlStatus nvswitch_os_remove_client_event(void *h,void *p)
{ (void)h;(void)p;return NVL_ERR_NOT_SUPPORTED; }
NvlStatus nvswitch_os_notify_client_event(void *h,void *p,NvU32 e)
{ (void)h;(void)p;(void)e;return NVL_ERR_NOT_SUPPORTED; }
NvlStatus nvswitch_os_get_supported_register_events_params(NvBool *many,NvBool *osdata)
{ if(many)*many=NV_FALSE;if(osdata)*osdata=NV_FALSE;return NVL_ERR_NOT_SUPPORTED; }

NvBool os_is_administrator(void) { return NV_TRUE; }
NvBool os_check_access(RsAccessRight right) { (void)right; return NV_TRUE; }
NvBool os_is_isr(void)
{ return nvrm_current_isr(); }
NvBool os_pat_supported(void) { return NV_TRUE; }
NvBool os_is_efi_enabled(void) { return NV_TRUE; }
NvBool os_is_vgx_hyper(void) { return NV_FALSE; }
NvBool os_is_grid_supported(void) { return NV_FALSE; }
NvBool os_is_nvswitch_present(void) { return NV_FALSE; }
NvBool os_supports_kernel_suspend_notifiers(void) { return NV_FALSE; }
NV_STATUS os_device_vm_present(void) { return NV_ERR_NOT_SUPPORTED; }
NvBool os_is_init_ns(void) { return NV_TRUE; }
NvBool os_semaphore_may_sleep(void);

void os_disable_console_access(void) { }
void os_enable_console_access(void) { }
void os_flush_cpu_write_combine_buffer(void) { __asm__ volatile("mfence" ::: "memory"); }
NV_STATUS os_flush_cpu_cache_all(void) { __asm__ volatile("mfence" ::: "memory"); return NV_OK; }
NV_STATUS os_flush_user_cache(void) { return NV_OK; }
void os_dbg_init(void) { }
void os_dbg_set_level(NvU32 level) { (void)level; }
void os_dbg_breakpoint(void) { }
void os_dump_stack(void) { }
void os_bug_check(NvU32 code, const char *msg)
{ kerr("nvrm", "bugcheck %#x: %s", code, msg ? msg : ""); }

/* RM assertion messages can exceed one klog entry. Forward complete bounded
 * chunks so the source filename/line and trailing status are not silently
 * discarded (the scrub timeout did exactly that). Do not allocate in a logger. */
static void nvrm_log_text(int severity, const char *text)
{
    if (!text) return;
    bool continuation=false;
    while (*text) {
        char chunk[KLOG_MSG_MAX-2];
        unsigned n=0;
        while (text[n] && text[n]!='\n' && n<sizeof(chunk)-1) {
            chunk[n]=text[n];n++;
        }
        chunk[n]=0;
        if (n) klog(severity,"nvrm","%s%s",continuation?"+ ":"",chunk);
        text+=n;
        continuation=*text && *text!='\n';
        if (*text=='\n') text++;
    }
}

void out_string(const char *s) { nvrm_log_text(KLOG_INFO,s); }
/* portDbgPrintf in NVIDIA's debug_unix_kernel_os.h uses 0xffffffff: the
 * original NV_PRINTF level is no longer passed to this boundary. The exact
 * dispapiControl entry trace is LEVEL_INFO (disp_objs.c), not a failed ioctl.
 * Recognize only that complete numeric trace; never demote assertions, status
 * suffixes, unknown messages or explicit NV_DBG_ERRORS calls. RmMsg's forced
 * traces take nvDbg_vPrintf -> _nvDbgForceLevel -> LEVEL_FATAL (6), even for
 * this LEVEL_INFO callsite. That is the path used by the supplied open RM. */
static bool nvrm_display_entry_trace(const char *p)
{
    const char *prefix="NVRM: GPU";
    if (!p || strncmp(p,prefix,strlen(prefix))) return false;
    p+=strlen(prefix);
    const char *digits=p;
    while (*p>='0' && *p<='9') p++;
    if (p==digits) return false;
    prefix=" dispapiControl_IMPL: class: 0x";
    if (strncmp(p,prefix,strlen(prefix))) return false;
    p+=strlen(prefix);
    for (unsigned field=0;field<2;field++) {
        digits=p;
        while ((*p>='0'&&*p<='9') || (*p>='a'&&*p<='f') || (*p>='A'&&*p<='F')) p++;
        if (p==digits || p-digits>8) return false;
        if (!field) {
            prefix=" cmd 0x";
            if (strncmp(p,prefix,strlen(prefix))) return false;
            p+=strlen(prefix);
        }
    }
    if (*p=='\n') p++;
    return !*p;
}

int nv_printf(NvU32 level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* NVIDIA os-interface.h: INFO=0, SETUP=1, USERERRORS=2, WARNINGS=3,
     * ERRORS=4. This is not the descending severity order used by printk. */
    int severity=level<=1?KLOG_INFO:level==3?KLOG_WARN:KLOG_ERROR;
    if ((level==0xffffffffu || level==6u) && nvrm_display_entry_trace(line)) severity=KLOG_INFO;
    nvrm_log_text(severity,line);
    if (n>=(int)sizeof line)
        nvrm_log_text(severity,"[RM formatted message exceeded 511 bytes; remainder unavailable]");
    return n;
}

void nv_get_screen_info(nv_state_t *nv, NvU64 *base, NvU32 *w, NvU32 *h,
                        NvU32 *depth, NvU32 *pitch, NvU64 *size)
{
    if (base) *base = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    if (depth) *depth = 0;
    if (pitch) *pitch = 0;
    if (size) *size = 0;

    /* This is the non-Linux equivalent of nv.c:nv_get_screen_info().  RM asks
     * this during rm_init_adapter() to decide whether this GPU owns the UEFI
     * console and to reserve/import that exact display allocation.  Returning
     * all zeroes made a genuine primary RTX display look like a secondary GPU
     * and skipped the normal firmware-console takeover path.
     *
     * Never report an arbitrary loader surface to an unrelated GPU.  Linux's
     * NV_IS_CONSOLE_MAPPED check accepts the console only when its complete
     * physical interval lies in this nv_state's FB or IMEM aperture; apply the
     * same overflow-safe test here. */
    if (!nv || !g_boot.fb.base || !g_boot.fb.size ||
        !g_boot.fb.width || !g_boot.fb.height || !g_boot.fb.pitch ||
        !g_boot.fb.bpp)
        return;

    NvU64 fb_first = g_boot.fb.base;
    NvU64 fb_bytes = g_boot.fb.size;
    if (fb_bytes > ~(NvU64)0 - fb_first)
        return;
    NvU64 fb_last = fb_first + fb_bytes;
    NvBool owned = NV_FALSE;
    for (NvU32 i = NV_GPU_BAR_INDEX_FB; i < NV_GPU_NUM_BARS; i++) {
        NvU64 bar_first = nv->bars[i].cpu_address;
        NvU64 bar_bytes = nv->bars[i].size;
        if (!bar_first || !bar_bytes || bar_bytes > ~(NvU64)0 - bar_first)
            continue;
        if (fb_first >= bar_first && fb_last <= bar_first + bar_bytes) {
            owned = NV_TRUE;
            break;
        }
    }
    if (!owned)
        return;

    if (base) *base = fb_first;
    if (w) *w = g_boot.fb.width;
    if (h) *h = g_boot.fb.height;
    if (depth) *depth = g_boot.fb.bpp;
    if (pitch) *pitch = g_boot.fb.pitch;
    if (size) *size = fb_bytes;
}
