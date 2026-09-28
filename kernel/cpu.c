/* cpu.c - descriptor tables, exception handling and CPU feature detection. */
#include "kernel.h"
#include "cpu.h"
#include "cpu_entry_offsets.h"
#include "apic.h"
#include "acpi.h"
#include "mm.h"
#include "klog.h"
#include "smp.h"

cpu_info_t g_cpu;

/* ------------------------------------------------------------------------- */
/* GDT and TSS                                                               */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  flags_limit_hi;
    u8  base_hi;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} tss_t;

/* A TSS descriptor becomes busy when LTR loads it. Each processor must own
 * both its descriptor and its TSS, including the IST stacks: NMI/MCE can
 * arrive even while an AP worker has maskable interrupts disabled. These
 * boot-lifetime objects are preallocated, so AP startup needs no heap lock.
 * Keep GDT first so SGDT identifies this CPU without consuming user FS/GS. */
typedef struct {
    u64 kernel_rsp, user_rsp;
} cpu_syscall_entry_t;
_Static_assert(__builtin_offsetof(cpu_syscall_entry_t, kernel_rsp) == CPU_ENTRY_KERNEL_RSP,
               "syscall entry kernel stack offset");
_Static_assert(__builtin_offsetof(cpu_syscall_entry_t, user_rsp) == CPU_ENTRY_USER_RSP,
               "syscall entry user stack offset");

typedef struct {
    gdt_entry_t gdt[7]; /* null, kcode, kdata, udata, ucode, tss(2 slots) */
    tss_t tss __attribute__((aligned(16)));
    cpu_syscall_entry_t entry __attribute__((aligned(16)));
    u8 df_stack[16 * 1024] __attribute__((aligned(16)));
    u8 nmi_stack[8 * 1024] __attribute__((aligned(16)));
    u8 mce_stack[8 * 1024] __attribute__((aligned(16)));
} cpu_descriptor_state_t;
static cpu_descriptor_state_t cpu_descriptors[ACPI_MAX_CPUS];
typedef struct __attribute__((packed)) { u16 limit; u64 base; } gdtr_t;
_Static_assert(sizeof(tss_t) == 104, "x86-64 TSS layout");
_Static_assert(__builtin_offsetof(cpu_descriptor_state_t, gdt) == 0,
               "SGDT identifies the current CPU descriptor state");

static void gdt_set(gdt_entry_t *gdt, int i, u32 base, u32 limit, u8 access, u8 flags) {
    gdt[i].limit_lo       = (u16)(limit & 0xFFFF);
    gdt[i].base_lo        = (u16)(base & 0xFFFF);
    gdt[i].base_mid       = (u8)((base >> 16) & 0xFF);
    gdt[i].access         = access;
    gdt[i].flags_limit_hi = (u8)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[i].base_hi        = (u8)((base >> 24) & 0xFF);
}

static cpu_descriptor_state_t *cpu_local_descriptors(void) {
    gdtr_t active;
    __asm__ volatile("sgdt %0" : "=m"(active));
    return (cpu_descriptor_state_t *)active.base;
}

u32 cpu_current_slot(void) {
    u64 base = (u64)cpu_local_descriptors();
    u64 first = (u64)&cpu_descriptors[0];
    if (base < first || base - first >= sizeof cpu_descriptors ||
        (base - first) % sizeof(cpu_descriptor_state_t)) return 0xffffffffu;
    return (u32)((base - first) / sizeof(cpu_descriptor_state_t));
}

void tss_set_rsp0(u64 rsp0) {
    cpu_descriptor_state_t *local = cpu_local_descriptors();
    local->tss.rsp0 = rsp0;
    local->entry.kernel_rsp = rsp0;
}

/* Called once on the CPU which owns this slot, before loading the shared IDT.
 * Never rebuild another processor's tables or clear a live busy descriptor. */
static void gdt_init_slot(u32 slot) {
    cpu_descriptor_state_t *local = &cpu_descriptors[slot];
    gdt_entry_t *gdt = local->gdt;
    tss_t *tss = &local->tss;
    memset(gdt, 0, sizeof local->gdt);
    memset(tss, 0, sizeof *tss);

    /* access: P|DPL|S|type.  flags: G|D/B|L|AVL in the high nibble. */
    gdt_set(gdt, 1, 0, 0xFFFFF, 0x9A, 0xA0);   /* kernel code, long mode  */
    gdt_set(gdt, 2, 0, 0xFFFFF, 0x92, 0xC0);   /* kernel data             */
    gdt_set(gdt, 3, 0, 0xFFFFF, 0xF2, 0xC0);   /* user data,   DPL 3      */
    gdt_set(gdt, 4, 0, 0xFFFFF, 0xFA, 0xA0);   /* user code,   DPL 3      */

    /* The TSS descriptor is 16 bytes and spans two GDT slots. */
    u64 base = (u64)tss;
    u32 limit = sizeof(*tss) - 1;
    gdt_set(gdt, 5, (u32)base, limit, 0x89, 0x00); /* available 64-bit TSS */
    u32 *hi = (u32 *)&gdt[6];
    hi[0] = (u32)(base >> 32);
    hi[1] = 0;

    tss->ist[IST_DOUBLE_FAULT - 1] = (u64)local->df_stack  + sizeof local->df_stack;
    tss->ist[IST_NMI - 1]          = (u64)local->nmi_stack + sizeof local->nmi_stack;
    tss->ist[IST_MCE - 1]          = (u64)local->mce_stack + sizeof local->mce_stack;
    tss->iomap_base = sizeof(*tss); /* no I/O permission bitmap */

    gdtr_t gdtr = { sizeof local->gdt - 1, (u64)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        /* Reload CS with a far return, since a plain jmp cannot set it in
         * long mode. */
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov %w2, %%ds\n"
        "mov %w2, %%es\n"
        "mov %w2, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        :
        : "m"(gdtr), "i"((u64)SEL_KCODE), "r"((u16)SEL_KDATA)
        : "rax", "memory");

    __asm__ volatile("ltr %0" :: "r"((u16)SEL_TSS));
}

void gdt_init(void) { gdt_init_slot(0); }

bool gdt_load_on_this_cpu(u32 slot) {
    if (!slot || slot >= ACPI_MAX_CPUS) return false;
    gdt_init_slot(slot);
    return true;
}

/* ------------------------------------------------------------------------- */
/* IDT                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    u16 offset_lo;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_hi;
    u32 reserved;
} idt_entry_t;

static idt_entry_t idt[256];
extern u64 isr_table[256];

static void idt_set(int vec, u64 handler, u8 ist, u8 dpl) {
    idt[vec].offset_lo  = (u16)(handler & 0xFFFF);
    idt[vec].selector   = SEL_KCODE;
    idt[vec].ist        = ist;
    idt[vec].type_attr  = (u8)(0x8E | (dpl << 5));   /* present, interrupt gate */
    idt[vec].offset_mid = (u16)((handler >> 16) & 0xFFFF);
    idt[vec].offset_hi  = (u32)(handler >> 32);
    idt[vec].reserved   = 0;
}

void idt_init(void) {
    memset(idt, 0, sizeof idt);
    for (int i = 0; i < 256; i++) idt_set(i, isr_table[i], 0, 0);

    idt_set(8,  isr_table[8],  IST_DOUBLE_FAULT, 0);
    idt_set(2,  isr_table[2],  IST_NMI, 0);
    idt_set(18, isr_table[18], IST_MCE, 0);
    /* int3 stays reachable from user mode so a debugger trap does not become a
     * general protection fault. */
    idt_set(3, isr_table[3], 0, 3);

    struct __attribute__((packed)) { u16 limit; u64 base; } idtr = { sizeof idt - 1, (u64)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

void idt_load_on_this_cpu(void) {
    struct __attribute__((packed)) { u16 limit; u64 base; } idtr =
        { sizeof idt - 1, (u64)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}


/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

static irq_handler_fn handlers[256];
static void          *handler_ctx[256];
static bool           vector_taken[256];

void irq_install(int vector, irq_handler_fn fn, void *ctx) {
    if (vector < 0 || vector > 255) return;
    bool irq = irq_save();
    handlers[vector] = fn;
    handler_ctx[vector] = ctx;
    vector_taken[vector] = true;
    irq_restore(irq);
}

void irq_uninstall(int vector) {
    if (vector < 0 || vector > 255) return;
    bool irq = irq_save();
    handlers[vector] = NULL;
    handler_ctx[vector] = NULL;
    vector_taken[vector] = false;
    irq_restore(irq);
}

int irq_alloc_vector(void) {
    bool irq = irq_save();
    for (int v = VEC_MSI_BASE; v <= VEC_MSI_TOP; v++) {
        if (!vector_taken[v]) { vector_taken[v] = true; irq_restore(irq); return v; }
    }
    irq_restore(irq);
    kerr("irq", "no interrupt vectors left to allocate");
    return -1;
}

void irq_free_vector(int vector) { irq_uninstall(vector); }

static const char *const exc_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS", "segment not present",
    "stack-segment fault", "general protection fault", "page fault", "reserved",
    "x87 floating-point exception", "alignment check", "machine check",
    "SIMD floating-point exception", "virtualisation exception", "control protection exception",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "hypervisor injection", "VMM communication", "security exception", "reserved",
};

const char *exception_name(int vector) {
    return (vector >= 0 && vector < 32) ? exc_names[vector] : "interrupt";
}

void dump_regs(const regs_t *r) {
    kprintf("  RIP %016lx  CS  %04lx  RFLAGS %016lx\n", r->rip, r->cs, r->rflags);
    kprintf("  RSP %016lx  SS  %04lx  ERR    %016lx\n", r->rsp, r->ss, r->error);
    kprintf("  RAX %016lx  RBX %016lx  RCX %016lx\n", r->rax, r->rbx, r->rcx);
    kprintf("  RDX %016lx  RSI %016lx  RDI %016lx\n", r->rdx, r->rsi, r->rdi);
    kprintf("  RBP %016lx  R8  %016lx  R9  %016lx\n", r->rbp, r->r8, r->r9);
    kprintf("  R10 %016lx  R11 %016lx  R12 %016lx\n", r->r10, r->r11, r->r12);
    kprintf("  R13 %016lx  R14 %016lx  R15 %016lx\n", r->r13, r->r14, r->r15);
    kprintf("  CR2 %016lx  CR3 %016lx\n", read_cr2(), read_cr3());
}

/* Defined in proc.c once processes exist; weak so the kernel links and runs
 * before the process layer is initialised. */
__attribute__((weak)) bool user_fault(regs_t *r, u64 cr2) { (void)r; (void)cr2; return false; }
__attribute__((weak)) bool user_exception(regs_t *r, int vec, u64 addr) {
    (void)r; (void)vec; (void)addr; return false;
}
__attribute__((weak)) void proc_describe_current(char *buf, size_t cap) { strlcpy(buf, "kernel", cap); }
__attribute__((weak)) void proc_kernel_enter(void) { }
__attribute__((weak)) void proc_kernel_leave(void) { }

static void describe_page_fault(u64 err, u64 cr2) {
    kerr("fault", "page fault at %p: %s while %s in %s mode%s",
         (void *)cr2,
         (err & 1) ? "protection violation" : "page not present",
         (err & 2) ? "writing" : ((err & 16) ? "fetching an instruction" : "reading"),
         (err & 4) ? "user" : "kernel",
         (err & 8) ? " (reserved bit set)" : "");
}

static regs_t *interrupt_dispatch_inner(regs_t *r, u64 fault_address) {
    int vec = (int)r->vector;

    if (handlers[vec]) {
        regs_t *next = handlers[vec](r, handler_ctx[vec]);
        return next ? next : r;
    }

    if (vec < 32) {
        u64 cr2 = fault_address;
        bool from_user = (r->cs & 3) == 3;

        /* Give the process layer a chance to fix it up (demand paging, stack
         * growth) or to kill just the offending process. */
        if (vec == 14 && user_fault(r, cr2)) return r;

        if (vec == 14) describe_page_fault(r->error, cr2);
        else kcrit("fault", "%s (vector %d, error %#lx) at rip %p",
                   exception_name(vec), vec, r->error, (void *)r->rip);

        /* Divide by zero, an invalid instruction, an alignment fault: all of
         * them are things a program can be written to survive, so it is asked
         * before it is killed. */
        if (from_user && user_exception(r, vec, cr2)) return r;

        if (from_user) {
            char who[64];
            proc_describe_current(who, sizeof who);
            kerr("fault", "terminating %s after an unhandled %s", who, exception_name(vec));
            if (user_fault(r, cr2)) return r;    /* proc layer killed and rescheduled */
        }

        console_set_color(C_WHITE, C_RED);
        console_clear();
        kprintf("\n  KERNEL PANIC - unhandled CPU exception\n  =====================================\n\n");
        kprintf("  %s (vector %d)\n\n", exception_name(vec), vec);
        console_set_color(C_LGRAY, C_BLACK);
        dump_regs(r);
        kprintf("\n  Recent events:\n");
        klog_dump_tail(14);
        kprintf("\n  System halted.\n");
        for (;;) { cli(); hlt(); }
    }

    /* An unexpected device interrupt.
     *
     * Acknowledging it is not optional, and getting this wrong stops the
     * machine dead rather than merely losing an interrupt.  The local APIC
     * keeps a bit per vector saying "this one is being serviced", and it will
     * not deliver anything at that priority or below until the bit is cleared
     * by an end-of-interrupt.  Vectors are prioritised by their number, so an
     * unacknowledged interrupt at 254 blocks everything beneath it - which is
     * every other vector there is, including the timer.  The machine then
     * stops receiving ticks, and everything that waits on time waits forever.
     *
     * That is not a hypothetical.  It is what happened on the first machine
     * this ran on that was not a virtual one: the APIC raised its error
     * interrupt, nothing claimed it, no acknowledgement was sent, and the boot
     * stopped at the next thing that waited.  Sending the acknowledgement here
     * costs one register write and means a device nobody wrote a driver for
     * can no longer take the whole system down with it.
     *
     * The spurious vector is the exception, and it is a real one: the hardware
     * raises it precisely when it has decided NOT to mark a vector as being
     * serviced, so acknowledging it would clear somebody else's bit.  It is
     * the one interrupt that must be returned from in silence.
     */
    static bool warned[256];
    if (!warned[vec]) {
        warned[vec] = true;
        kwarn("irq", "unhandled interrupt on vector %d (no driver claimed it)", vec);
    }
    if (vec != VEC_SPURIOUS) lapic_eoi();
    return r;
}

/* Track user-origin entries across any scheduling inside the handler. Stop
 * requests are acted on only AFTER its EOI and driver cleanup, never before
 * an interrupt has been acknowledged. Kernel-origin nested IRQs add no entry. */
regs_t *interrupt_dispatch(regs_t *r) {
    bool user = (r->cs & 3) == 3;
    /* CR2 is CPU-local. Snapshot it before an AP fault continuation moves to
     * the BSP; reading CR2 inside the resumed handler would name a different
     * processor's last fault. */
    u64 fault_address = r->vector < 32 ? read_cr2() : 0;
    /* AP-local LAPIC vectors must be acknowledged by their originating CPU.
     * Other user-origin handlers enter through the BSP kernel boundary. */
    bool local_ap = user && cpu_current_slot() != 0 &&
                    smp_interrupt_local_on_ap((u32)r->vector);
    if (user && cpu_current_slot() != 0 && r->vector >= 32 && !local_ap) {
        lapic_eoi();
        panic("cpu: unexpected device vector %#lx on application AP", r->vector);
    }
    if (user && !local_ap) proc_kernel_enter();
    regs_t *result = interrupt_dispatch_inner(r, fault_address);
    if (user && !local_ap) proc_kernel_leave();
    return result;
}

/* ------------------------------------------------------------------------- */
/* syscall MSRs                                                              */
/* ------------------------------------------------------------------------- */

void syscall_init(void) {
    if (!g_cpu.has_syscall) panic("cpu: SYSCALL/SYSRET is not available");

    /* Reserved MSR, local to this logical processor. SWAPGS uses it only
     * while syscall_entry captures RSP; ordinary kernel C and interrupt code
     * retain their existing user-GS convention. APs must call syscall_init
     * after installing their kernel stack before ever entering ring 3. */
    wrmsr(MSR_KGS_BASE, (u64)&cpu_local_descriptors()->entry);

    u64 efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);          /* SCE */

    /* STAR: [47:32] kernel CS for SYSCALL, [63:48] base for SYSRET's CS/SS. */
    wrmsr(MSR_STAR, ((u64)SEL_KCODE << 32) | ((u64)SEL_KDATA << 48));
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    /* Mask IF so the kernel entry runs with interrupts off until it has a
     * stack, plus DF and AC for a sane environment. */
    wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x40000);
}

/* ------------------------------------------------------------------------- */
/* features                                                                  */
/* ------------------------------------------------------------------------- */

/* Shared policy, applied independently on each logical processor. Detection
 * and logging remain BSP-only; calling cpu_features_init on an AP would race
 * the global identification data. This intentionally does not enable AVX or
 * OSXSAVE: the scheduler currently saves only the legacy FXSAVE state. */
static void cpu_enable_local_features(void) {
    u64 efer = rdmsr(MSR_EFER);
    if (g_cpu.has_nx) wrmsr(MSR_EFER, efer | (1ULL << 11));

    u64 cr4 = read_cr4();
    cr4 |= (1ULL << 9) | (1ULL << 10) | (1ULL << 7);
    /* As on the BSP before this refactor, do not enable SMAP: current kernel
     * user-buffer accessors do not bracket their loads/stores with STAC/CLAC. */
    if (g_cpu.has_smep) cr4 |= (1ULL << 20);
    write_cr4(cr4);

    u64 cr0 = read_cr0();
    /* EM and TS both prohibit the initialization below. Eager FPU restore is
     * used, not a lazy #NM handler, so no running CPU should retain TS. */
    cr0 &= ~((1ULL << 2) | (1ULL << 3));
    /* MP, native x87 exceptions (#MF rather than legacy FERR), and WP. */
    cr0 |= (1ULL << 1) | (1ULL << 5) | (1ULL << 16);
    write_cr0(cr0);
    fpu_init_state();
    if (g_cpu.has_sse2) {
        const u32 mxcsr = 0x1f80; /* mask exceptions, round to nearest */
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    }
}

void cpu_features_init(void) {
    u32 a, b, c, d;

    cpuid_raw(0, 0, &a, &b, &c, &d);
    u32 max_leaf = a;
    memcpy(g_cpu.vendor + 0, &b, 4);
    memcpy(g_cpu.vendor + 4, &d, 4);
    memcpy(g_cpu.vendor + 8, &c, 4);
    g_cpu.vendor[12] = 0;

    if (max_leaf >= 1) {
        cpuid_raw(1, 0, &a, &b, &c, &d);
        u32 base_family = (a >> 8) & 0xF, base_model = (a >> 4) & 0xF;
        g_cpu.stepping = a & 0xF;
        g_cpu.family = base_family == 0xF ? base_family + ((a >> 20) & 0xFF) : base_family;
        g_cpu.model  = (base_family == 0x6 || base_family == 0xF)
                     ? base_model + (((a >> 16) & 0xF) << 4) : base_model;
        g_cpu.has_sse2  = (d >> 26) & 1;
        g_cpu.has_x2apic = (c >> 21) & 1;
        g_cpu.has_xsave = (c >> 26) & 1;
        g_cpu.has_avx   = (c >> 28) & 1;
        g_cpu.has_rdrand = (c >> 30) & 1;
        g_cpu.has_tsc_deadline = (c >> 24) & 1;
        g_cpu.has_pat   = (d >> 16) & 1;
        g_cpu.has_mtrr  = (d >> 12) & 1;
    }
    if (max_leaf >= 7) {
        cpuid_raw(7, 0, &a, &b, &c, &d);
        g_cpu.has_smep = (b >> 7) & 1;
        g_cpu.has_smap = (b >> 20) & 1;
    }

    cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
    u32 max_ext = a;
    if (max_ext >= 0x80000001) {
        cpuid_raw(0x80000001, 0, &a, &b, &c, &d);
        g_cpu.has_syscall   = (d >> 11) & 1;
        g_cpu.has_nx        = (d >> 20) & 1;
        g_cpu.has_1gb_pages = (d >> 26) & 1;
    }
    if (max_ext >= 0x80000004) {
        u32 *p = (u32 *)g_cpu.brand;
        for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_raw(leaf, 0, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        g_cpu.brand[48] = 0;
    }
    if (max_ext >= 0x80000008) {
        cpuid_raw(0x80000008, 0, &a, &b, &c, &d);
        g_cpu.phys_bits = a & 0xFF;
        g_cpu.virt_bits = (a >> 8) & 0xFF;
    } else {
        g_cpu.phys_bits = 36;
        g_cpu.virt_bits = 48;
    }

    cpu_enable_local_features();

    /* Trim the trailing spaces Intel pads the brand string with. */
    char *e = g_cpu.brand + strlen(g_cpu.brand);
    while (e > g_cpu.brand && e[-1] == ' ') *--e = 0;
    char *s = g_cpu.brand;
    while (*s == ' ') s++;
    if (s != g_cpu.brand) memmove(g_cpu.brand, s, strlen(s) + 1);

    kinfo("cpu", "%s", g_cpu.brand[0] ? g_cpu.brand : g_cpu.vendor);
    kinfo("cpu", "family %u model %u stepping %u, %u-bit physical addressing",
          g_cpu.family, g_cpu.model, g_cpu.stepping, g_cpu.phys_bits);
    kinfo("cpu", "features:%s%s%s%s%s%s%s",
          g_cpu.has_sse2 ? " sse2" : "", g_cpu.has_avx ? " avx" : "",
          g_cpu.has_nx ? " nx" : "", g_cpu.has_1gb_pages ? " pdpe1gb" : "",
          g_cpu.has_x2apic ? " x2apic" : "", g_cpu.has_smep ? " smep" : "",
          g_cpu.has_tsc_deadline ? " tsc-deadline" : "");

    if (!g_cpu.has_sse2) kwarn("cpu", "no SSE2: user programs built for x86-64 may fault");
    if (!g_cpu.has_nx)   kwarn("cpu", "no NX support: pages cannot be marked non-executable");
}

/* ------------------------------------------------------------ memory types */

/* The page attribute table gives each page one of eight memory types, chosen
 * by three bits in its page table entry.  As the firmware leaves it there is no
 * write-combining entry at all, so a framebuffer can only be mapped uncached:
 * every pixel written becomes its own bus transaction.
 *
 * Entry 1 - selected by PWT alone - is write-through as it comes, which
 * nothing here uses.  Turning it into write-combining costs nothing and makes
 * every existing PWT mapping, which is exactly the framebuffer, gather its
 * writes into whole cache lines before they leave the processor.
 *
 * Write-combining also wins over an uncached range in the memory type range
 * registers, so this works even where the firmware has marked video memory
 * uncached - which it usually has.
 */
/* Boot-only local installation. All CPUs use the same interpretation before
 * consuming shared PWT/WC mappings. Not a runtime PAT-reconfiguration API:
 * changing an in-use policy requires a rendezvous on every affected CPU. */
static const u64 kernel_pat = 0x0007040600070106ULL;
static bool kernel_pat_initialized;

static void pat_install_local(u64 pat) {
    if (rdmsr(0x277) == pat) return;
    bool irq = irq_save();

    /* The prescribed sequence: stop caching, write everything back, drop the
     * old translations, change the table, then drop them again. */
    u64 cr4 = read_cr4(), cr3 = read_cr3();
    u64 cr0 = read_cr0();
    write_cr0((cr0 & ~(1ULL << 29)) | (1ULL << 30));        /* CD=1, NW=0 */
    __asm__ volatile("wbinvd" ::: "memory");
    /* A CR3 reload alone does not invalidate globals or other PCIDs. Switch
     * to PCID zero before disabling PCIDE so its bits cannot become PWT/PCD,
     * then drop both translation-retention features for the PAT transition. */
    if (cr4 & (1ULL << 17)) write_cr3(cr3 & ~0xfffULL);
    u64 flush_cr4 = cr4 & ~((1ULL << 7) | (1ULL << 17));
    if (flush_cr4 != cr4) write_cr4(flush_cr4);
    write_cr3(read_cr3());

    wrmsr(0x277, pat);

    write_cr3(read_cr3());
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr0(cr0);
    if (flush_cr4 != cr4) write_cr4(cr4);
    if (cr4 & (1ULL << 17)) write_cr3(cr3);

    irq_restore(irq);
}

void pat_init(void) {
    if (!g_cpu.has_pat) {
        kwarn("cpu", "no page attribute table; video memory stays uncached");
        return;
    }
    pat_install_local(kernel_pat);
    __atomic_store_n(&kernel_pat_initialized, true, __ATOMIC_RELEASE);
    kinfo("cpu", "write-combining enabled for video memory");
}

cpu_ap_init_status_t cpu_init_application_processor(u32 slot, u64 stack_top) {
    if (!slot || slot >= ACPI_MAX_CPUS || (read_flags() & 0x200) ||
        !stack_top || (stack_top & 15) || (stack_top >> 48) != 0xffff ||
        cpu_local_descriptors() != &cpu_descriptors[slot])
        return CPU_AP_INIT_CONTEXT;

    u32 a, b, c, d;
    cpuid_raw(0, 0, &a, &b, &c, &d);
    u32 max_leaf = a;
    if (max_leaf < 1) return CPU_AP_INIT_FPU;
    cpuid_raw(1, 0, &a, &b, &c, &d);
    const u32 legacy_required = (1u << 0) | (1u << 13) | (1u << 24) |
                                (1u << 25) | (1u << 26);
    if ((d & legacy_required) != legacy_required) return CPU_AP_INIT_FPU;
    if (g_cpu.has_pat && (!(d & (1u << 16)) ||
                         !__atomic_load_n(&kernel_pat_initialized, __ATOMIC_ACQUIRE)))
        return CPU_AP_INIT_PAT;
    if (g_cpu.has_smep) {
        if (max_leaf < 7) return CPU_AP_INIT_SMEP;
        cpuid_raw(7, 0, &a, &b, &c, &d);
        if (!(b & (1u << 7))) return CPU_AP_INIT_SMEP;
    }
    cpuid_raw(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000001) return CPU_AP_INIT_SYSCALL;
    cpuid_raw(0x80000001, 0, &a, &b, &c, &d);
    if (!(d & (1u << 11)) || !g_cpu.has_syscall) return CPU_AP_INIT_SYSCALL;
    if (g_cpu.has_nx && !(d & (1u << 20))) return CPU_AP_INIT_NX;
    if (g_cpu.has_1gb_pages && !(d & (1u << 26))) return CPU_AP_INIT_PAGING;

    cpu_enable_local_features();
    if (g_cpu.has_pat) pat_install_local(kernel_pat);
    /* INIT preserves the AP's CD/NW bits. Establish normal cached operation
     * explicitly instead of relying on firmware to have left both clear.
     * This AP has not used shared device mappings yet; PAT is now consistent. */
    u64 cr0 = read_cr0();
    if (cr0 & ((1ULL << 30) | (1ULL << 29))) {
        __asm__ volatile("wbinvd" ::: "memory");
        write_cr0(cr0 & ~((1ULL << 30) | (1ULL << 29)));
    }

    /* No application TLS exists on this AP yet. Set its private interrupt and
     * SYSCALL stacks first, reset user TLS bases, then install KERNEL_GS_BASE.
     * Never clear KGSBASE after syscall_init: SWAPGS needs that private entry.
     * The eventual scheduler must install the selected thread's FS/GS/FPU and
     * replace this bootstrap RSP0 before entering ring 3. */
    tss_set_rsp0(stack_top);
    wrmsr(MSR_FS_BASE, 0);
    wrmsr(MSR_GS_BASE, 0);
    syscall_init();
    return CPU_AP_INIT_OK;
}
