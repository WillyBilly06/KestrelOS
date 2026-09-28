#ifndef KESTREL_CPU_H
#define KESTREL_CPU_H

#include "kernel.h"

/* Segment selectors.  The user pair is placed so that SYSRET's fixed
 * arithmetic (CS = STAR[63:48]+16, SS = STAR[63:48]+8) lands on them. */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28

#define SEL_UDATA3 (SEL_UDATA | 3)
#define SEL_UCODE3 (SEL_UCODE | 3)

/* Interrupt stack table slots for faults that cannot trust the current stack. */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MCE          3

/* Vector assignments. */
#define VEC_IRQ_BASE   0x20   /* legacy IRQ n -> 0x20 + n           */
#define VEC_APIC_TIMER 0x30
#define VEC_MSI_BASE   0x40   /* PCI MSI/MSI-X allocations start here */
#define VEC_MSI_TOP    0xEF
#define VEC_SYSCALL    0x80
/* The interrupt controller's own two.  The error vector was previously written
 * as "one below spurious", which is the same number and says nothing about
 * what it is - and what it is turned out to matter. */
#define VEC_APIC_ERROR 0xFE
#define VEC_SPURIOUS   0xFF

/* The frame isr.S builds.  Field order matches the push order exactly; do not
 * reorder without changing isr.S. */
typedef struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} regs_t;

void gdt_init(void);
void idt_init(void);

/* Give an AP its own GDT/TSS/IST stacks. A unique nonzero slot is assigned
 * once at startup; never rebuild a slot belonging to a running processor.
 * Returns false for an invalid slot, before loading any tables. */
bool gdt_load_on_this_cpu(u32 slot);
/* Stable boot-assigned logical CPU slot, independent of user FS/GS. Returns
 * UINT32_MAX before this processor has installed one of our private GDTs. */
u32 cpu_current_slot(void);
void idt_load_on_this_cpu(void);
/* Update this CPU's interrupt privilege stack and SYSCALL entry stack together.
 * Scheduler callers hold local interrupts disabled during the transition. */
void tss_set_rsp0(u64 rsp0);
void syscall_init(void);
void cpu_features_init(void);

/* Quiet AP-local setup after private GDT/TSS and shared IDT are installed,
 * with IF clear. Does not start a scheduler or permit application execution.
 * A nonzero result leaves the AP offline; its BSP reports the reason. */
typedef enum {
    CPU_AP_INIT_OK = 0,
    CPU_AP_INIT_CONTEXT = 1,
    CPU_AP_INIT_FPU = 2,
    CPU_AP_INIT_NX = 3,
    CPU_AP_INIT_PAGING = 4,
    CPU_AP_INIT_PAT = 5,
    CPU_AP_INIT_SMEP = 6,
    CPU_AP_INIT_SYSCALL = 7
} cpu_ap_init_status_t;
cpu_ap_init_status_t cpu_init_application_processor(u32 slot, u64 stack_top);

/* Put write-combining into the page attribute table.  Without it every store
 * to video memory goes out on its own; with it the processor gathers a whole
 * cache line before writing, which is the difference between a desktop that
 * redraws instantly and one that crawls. */
void pat_init(void);

/* Register a handler for a vector.  Returning non-NULL replaces the frame that
 * will be restored, which is how a context switch happens. */
typedef regs_t *(*irq_handler_fn)(regs_t *r, void *ctx);
void irq_install(int vector, irq_handler_fn fn, void *ctx);
void irq_uninstall(int vector);
int  irq_alloc_vector(void);        /* for MSI; -1 when exhausted */
void irq_free_vector(int vector);

extern void isr_return(void);
extern void thread_trampoline(void);
extern void syscall_entry(void);
extern void fpu_save(void *area);
extern void fpu_restore(void *area);
extern void fpu_init_state(void);

const char *exception_name(int vector);
void dump_regs(const regs_t *r);

/* CPU identification, filled in by cpu_features_init. */
typedef struct {
    char vendor[13];
    char brand[49];
    u32  family, model, stepping;
    bool has_x2apic, has_1gb_pages, has_nx, has_sse2, has_xsave, has_avx;
    bool has_syscall, has_tsc_deadline, has_smep, has_smap, has_rdrand;
    bool has_pat, has_mtrr;
    u32  phys_bits, virt_bits;
} cpu_info_t;

extern cpu_info_t g_cpu;

/* How the processors in this machine are arranged, and what each one holds.
 * Filled in by cpu_topology_init() once the firmware's processor table has
 * been read, because the count of processors comes from there and the shape
 * they are arranged in comes from CPUID. */
#define CPU_MAX_CACHES 8

typedef struct {
    u32  level;          /* 1, 2, 3 ...                                     */
    u32  type;           /* 1 data, 2 instruction, 3 unified                */
    u64  bytes;
    u32  line;           /* bytes per line                                  */
    u32  ways;
    u32  shared;         /* logical processors that reach this one cache    */
    char kind[16];
} cpu_cache_t;

typedef struct {
    u32  sockets;
    u32  cores;              /* physical, across every socket               */
    u32  threads;            /* logical processors the firmware listed      */
    u32  threads_per_core;

    u32  base_mhz, max_mhz, bus_mhz;   /* 0 when the processor will not say */
    bool clocks_from_cpuid;            /* false when the firmware supplied them */

    bool virtualization;
    char virt_name[8];       /* "VT-x" or "AMD-V"                           */

    bool hybrid;             /* performance and efficiency cores mixed      */

    /* Set when the core count may be low because this is a hybrid part and the
     * width used to decompose the identifiers came from the boot processor
     * alone.  Reported rather than hidden: a number nobody qualified is
     * believed. */
    bool cores_uncertain;
    char boot_core_kind[16]; /* which kind this code is running on          */

    /* How many of each kind, once every core has been asked its own type
     * (CPUID 0x1A run on each - see cpu_topology_refine).  Zero until that has
     * happened, or on a part that is not hybrid.  When both are known the true
     * core count is their sum and the uncertainty above is cleared. */
    u32  perf_cores;         /* Intel P-cores (or the only kind on a non-hybrid) */
    u32  eff_cores;          /* Intel E-cores                                    */
    bool per_core_kinds;     /* set once each core has reported its own kind     */

    cpu_cache_t cache[CPU_MAX_CACHES];
    u32         cache_count;
} cpu_topology_t;

extern cpu_topology_t g_topo;

void cpu_topology_init(void);
/* Ask every started core its own kind and cache, so a hybrid part's P/E split
 * is exact rather than derived from the boot core alone.  Call after smp_init. */
void cpu_topology_refine(void);
/* Called on each processor, never through the load-balanced job queue. */
void cpu_topology_capture(u32 slot);
void cpu_frequency_sample(u32 slot);
bool cpu_logical_info(u32 index, void *out);
u64  cpu_cache_total(u32 level);

#endif
