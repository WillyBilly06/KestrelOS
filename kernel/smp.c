/* smp.c - AP startup, restricted jobs, TLB barriers and application admission.
 * APs first join the restricted batch-worker pool. After the scheduler has
 * prepared private idle stacks, all but one eligible worker may retire at a
 * batch boundary and run application user code. Syscalls, exceptions and
 * legacy device/IRQ handling remain BSP-owned: AP user-origin entries park
 * their saved kernel stack and resume that continuation on the BSP. Restricted
 * jobs still run only on workers and may touch only their supplied memory.
 * Each role retains its private GDT/TSS/IST, LAPIC timer and TLB mailbox. */
#include "kernel.h"
#include "acpi.h"
#include "apic.h"
#include "mm.h"
#include "klog.h"
#include "smp.h"
#include "time.h"
#include "cpu.h"
#include "spinlock.h"
#include "proc.h"

bool cmdline_has(const char *flag);

/* Must match TRAMP_BASE in smp_trampoline.S.  Low, because a startup message
 * carries a page number and only the first megabyte can be named that way. */
#define SMP_TRAMPOLINE_PHYS  0x8000ULL
#define SMP_VECTOR           (SMP_TRAMPOLINE_PHYS >> 12)

#define PARAM_PML4    0xF00
#define PARAM_STACK   0xF08
#define PARAM_ENTRY   0xF10
#define PARAM_ALIVE   0xF18

#define AP_STACK_SIZE (32 * 1024)

extern const u8 smp_trampoline_start[];
extern const u8 smp_trampoline_end[];

/* The two messages that wake a core.  Sending them is apic.c's job, because
 * which of the two ways of reaching the interrupt command register is in use
 * is that file's business - and getting it wrong is silent. */
#define ICR_INIT       (5u << 8)
#define ICR_STARTUP    (6u << 8)
#define ICR_ASSERT     (1u << 14)
#define ICR_LEVEL      (1u << 15)


/* ------------------------------------------------------------- the work queue
 *
 * One queue, taken from by every worker.  `next` is advanced atomically, so a
 * job is handed to exactly one core without a lock: whoever wins the exchange
 * owns that index and nobody else will see it.
 */
#define MAX_JOBS 256
/* Outside the PCI/MSI allocation range (0x40..0xef), and distinct from the
 * BSP's 0x30 scheduler timer and the APIC error/spurious vectors. */
#define SMP_WAKE_VECTOR  0xf0
#define SMP_RESCHED_VECTOR 0xf3
#define SMP_APP_TICK_VECTOR 0xf4
#define SMP_SAMPLE_VECTOR 0xf1
#define SMP_TLB_VECTOR    0xf2
_Static_assert(SMP_WAKE_VECTOR > VEC_MSI_TOP &&
               SMP_SAMPLE_VECTOR < SMP_TLB_VECTOR &&
               SMP_TLB_VECTOR < SMP_RESCHED_VECTOR &&
               SMP_RESCHED_VECTOR < SMP_APP_TICK_VECTOR &&
               SMP_APP_TICK_VECTOR < VEC_APIC_ERROR, "SMP vectors are reserved");

bool smp_interrupt_local_on_ap(u32 vector) {
    return (vector >= SMP_WAKE_VECTOR && vector <= SMP_APP_TICK_VECTOR) ||
           vector == VEC_APIC_ERROR || vector == VEC_SPURIOUS;
}

static smp_job_fn  job_fn;
static void       *job_arg;

/* The address space a batch's jobs run in.
 *
 * The woken cores hold the kernel's own page tables, which do not map any
 * process's memory - so a job handed a pointer into a program's buffer would
 * fault on a core other than the one that started the batch.  Loading the
 * caller's tables for the length of the batch fixes that, and is safe for one
 * specific reason: the kernel is mapped at the same addresses in every address
 * space, so the worker keeps running the same kernel code either way.  It
 * never executes anything belonging to the program - only reads and writes
 * memory the program owns, which is what it was handed.
 *
 * Zero means the kernel's own, which is what every job that touches no user
 * memory wants. */
static volatile u64 job_cr3;
static volatile u32 job_count;      /* how many pieces this batch has         */
static volatile u32 job_next;       /* the next piece nobody has claimed      */
static volatile u32 job_done;       /* how many have been finished            */
static u64 job_generation;          /* release/acquire publication barrier    */
static spinlock_t batch_lock;

typedef struct {
    u32 apic_id;
    u32 online;
    u32 promote_requested;
    u32 init_error;
    u64 acknowledged;
    u32 time_sequence;
    u32 executing;
    u64 busy_us, idle_us, transition_us;
} worker_state_t;
static worker_state_t worker_state[ACPI_MAX_CPUS];

static volatile u32 workers_online;
static volatile u32 processors_arrived;
static u32 bsp_apic_id;

/* A request is immutable until every targeted CPU has acknowledged it. Each
 * target has its own release-published pending generation, so a non-target
 * never reads a concurrently replaced request. Online registration shares the
 * publisher lock: an arriving AP either participates or flushes before joining.
 * Slot zero is the BSP; only successfully registered APs enter tlb_online. */
static spinlock_t tlb_lock;
static u64 tlb_online;
static u64 tlb_generation;
static u64 tlb_pml4, tlb_va;
static bool tlb_full;
typedef struct { u64 pending, acknowledged; } tlb_mailbox_t;
static tlb_mailbox_t tlb_mailboxes[ACPI_MAX_CPUS];
_Static_assert(ACPI_MAX_CPUS <= 64, "TLB target mask capacity");

static void tlb_flush_local(u64 pml4, u64 va, bool full) {
    (void)pml4;
    u64 cr4 = read_cr4();
    /* Changing CR4.PGE invalidates all PCIDs and global translations. A CR3
     * reload alone does neither when retention is enabled. No INVPCID feature
     * assumption is needed. Restore the exact original control bits. */
    if (full || (cr4 & (1ULL << 17))) {
        if (cr4 & ((1ULL << 7) | (1ULL << 17))) {
            write_cr4(cr4 ^ (1ULL << 7));
            write_cr4(cr4);
        } else write_cr3(read_cr3());
    } else invlpg(va); /* also invalidates a global entry, independent of CR3 */
}

void smp_tlb_poll(void) {
    bool irq = irq_save();
    u32 slot = cpu_current_slot();
    if (slot >= ACPI_MAX_CPUS) { irq_restore(irq); return; }
    tlb_mailbox_t *mailbox = &tlb_mailboxes[slot];
    u64 pending = __atomic_load_n(&mailbox->pending, __ATOMIC_ACQUIRE);
    if (pending == __atomic_load_n(&mailbox->acknowledged, __ATOMIC_RELAXED)) {
        irq_restore(irq);
        return;
    }
    tlb_flush_local(tlb_pml4, tlb_va, tlb_full);
    /* Nothing from this request may be read after the acknowledgment. */
    __atomic_store_n(&mailbox->acknowledged, pending, __ATOMIC_RELEASE);
    irq_restore(irq);
}

static bool tlb_lock_acquire(void) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&tlb_lock.held, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}

static regs_t *tlb_isr(regs_t *r, void *ctx) {
    (void)ctx;
    smp_tlb_poll();
    lapic_eoi();
    return r;
}

void smp_tlb_invalidate(u64 pml4, u64 va, bool full) {
    bool irq = tlb_lock_acquire();
    u32 self = cpu_current_slot();
    u64 targets = tlb_online;
    if (self < ACPI_MAX_CPUS) targets &= ~(1ULL << self);
    u64 gen = ++tlb_generation;
    if (!gen) panic("smp: TLB generation exhausted");
    tlb_pml4 = pml4;
    tlb_va = va;
    tlb_full = full;
    tlb_flush_local(pml4, va, full);
    for (u32 slot = 0; slot < ACPI_MAX_CPUS; slot++) {
        if (!(targets & (1ULL << slot))) continue;
        __atomic_store_n(&tlb_mailboxes[slot].pending, gen, __ATOMIC_RELEASE);
        lapic_send_ipi(slot ? worker_state[slot].apic_id : bsp_apic_id, SMP_TLB_VECTOR);
    }
    u64 started = timer_now_us();
    for (u32 slot = 0; slot < ACPI_MAX_CPUS; slot++) {
        if (!(targets & (1ULL << slot))) continue;
        while (__atomic_load_n(&tlb_mailboxes[slot].acknowledged, __ATOMIC_ACQUIRE) != gen) {
            smp_tlb_poll();
            /* Fail closed: a missing CPU must never turn into permission to
             * recycle a frame it may still be accessing. */
            if (timer_now_us() - started > 10000000ULL)
                panic("smp: CPU %u did not acknowledge TLB invalidation", slot);
            __asm__ volatile("pause");
        }
    }
    spin_unlock_irqrestore(&tlb_lock, irq);
}

static u32 fetch_add(volatile u32 *p, u32 by) {
    return __atomic_fetch_add(p, by, __ATOMIC_SEQ_CST);
}

/* Restricted-worker wakeups never enter the scheduler, allocate, or call
 * drivers. Frequency sampling happens in worker context after a wake. */
static regs_t *worker_wake_isr(regs_t *r, void *ctx) {
    (void)ctx;
    lapic_eoi();
    return r;
}

static regs_t *application_reschedule_isr(regs_t *r, void *ctx) {
    (void)ctx;
    /* The APIC is acknowledged before a scheduler switch can park this frame
     * on an idle thread's kernel stack. */
    lapic_eoi();
    return sched_reschedule(r);
}

static regs_t *application_timer_isr(regs_t *r, void *ctx) {
    (void)ctx;
    cpu_frequency_sample(cpu_current_slot());
    /* The AP must retire its own in-service timer before any context may
     * resume this user-origin frame on the BSP. */
    lapic_eoi();
    return sched_ap_tick(r);
}

void smp_reschedule_cpu(u32 slot) {
    if (slot >= ACPI_MAX_CPUS || !apic_available()) return;
    if (slot && !__atomic_load_n(&worker_state[slot].online, __ATOMIC_ACQUIRE) &&
        !sched_application_cpu_online(slot))
        return;
    lapic_send_ipi(slot ? worker_state[slot].apic_id : bsp_apic_id,
                   SMP_RESCHED_VECTOR);
}

/* Single writer (the owning AP), sequence-protected snapshots for the BSP.
 * Counts worker execution versus waiting, not advertised CPU frequency or
 * capacity for processors that did not actually join the worker pool. */
static void worker_account_state(worker_state_t *state, bool executing) {
    u64 now = timer_now_us();
    __atomic_fetch_add(&state->time_sequence, 1u, __ATOMIC_SEQ_CST);
    u64 last = __atomic_load_n(&state->transition_us, __ATOMIC_RELAXED);
    if (now >= last) {
        u64 *counter = __atomic_load_n(&state->executing, __ATOMIC_RELAXED) ?
                       &state->busy_us : &state->idle_us;
        __atomic_fetch_add(counter, now - last, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&state->transition_us, now, __ATOMIC_RELAXED);
    __atomic_store_n(&state->executing, executing ? 1u : 0u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&state->time_sequence, 1u, __ATOMIC_RELEASE);
}

/* CLI + recheck + adjacent STI/HLT closes the lost-wakeup window: an IPI
 * arriving after the check remains pending until HLT is ready to wake. The
 * private 4 Hz timer keeps per-core frequency samples fresh even with no jobs. */
static void worker_loop(u32 cpu_slot) {
    worker_state_t *state = &worker_state[cpu_slot];
    bool irq = spin_lock_irqsave(&batch_lock);
    u64 seen = __atomic_load_n(&job_generation, __ATOMIC_ACQUIRE);
    state->apic_id = lapic_id();
    bool tlb_irq = tlb_lock_acquire();
    tlb_flush_local(0, 0, true);
    tlb_online |= 1ULL << cpu_slot;
    spin_unlock_irqrestore(&tlb_lock, tlb_irq);
    __atomic_store_n(&state->transition_us, timer_now_us(), __ATOMIC_RELAXED);
    __atomic_store_n(&state->acknowledged, seen, __ATOMIC_RELEASE);
    __atomic_store_n(&state->online, 1u, __ATOMIC_RELEASE);
    fetch_add(&workers_online, 1);
    fetch_add(&processors_arrived, 1);
    spin_unlock_irqrestore(&batch_lock, irq);

    for (;;) {
        cpu_frequency_sample(cpu_slot);

        cli();
        smp_tlb_poll();
        if (__atomic_load_n(&state->promote_requested, __ATOMIC_ACQUIRE)) {
            bool gate_irq = irq_save();
            while (__atomic_exchange_n(&batch_lock.held, 1u, __ATOMIC_ACQUIRE)) {
                smp_tlb_poll();
                __asm__ volatile("pause");
            }
            if (__atomic_load_n(&job_generation, __ATOMIC_ACQUIRE) == seen &&
                __atomic_load_n(&state->acknowledged, __ATOMIC_ACQUIRE) == seen &&
                __atomic_load_n(&state->online, __ATOMIC_ACQUIRE) &&
                lapic_timer_start_local(1000, SMP_APP_TICK_VECTOR)) {
                /* No published batch can still target us. The TLB mailbox
                 * remains online through the change of execution role. */
                __atomic_store_n(&state->online, 0u, __ATOMIC_RELEASE);
                __atomic_fetch_sub(&workers_online, 1u, __ATOMIC_ACQ_REL);
                spin_unlock_irqrestore(&batch_lock, gate_irq);
                sched_join_application_cpu();
            }
            spin_unlock_irqrestore(&batch_lock, gate_irq);
        }
        u64 gen = __atomic_load_n(&job_generation, __ATOMIC_ACQUIRE);
        if (gen == seen) {
            __asm__ volatile("sti; hlt; cli" ::: "memory");
            continue;
        }

        /* Snapshot everything before acknowledging. A new publication cannot
         * replace these fields until every participating AP has left its CR3. */
        smp_job_fn fn = job_fn;
        void *arg = job_arg;
        u32 count = job_count;
        worker_account_state(state, true);

        u64 mine = read_cr3();
        u64 want = job_cr3;
        if (want && want != mine) write_cr3(want);

        for (;;) {
            smp_tlb_poll();
            /* Jobs run with IF clear. A large batch must not starve the local
             * clock sampler until the entire batch ends. This is rate limited
             * to 100 ms and never schedules, allocates, or calls a driver.
             * A single long callback still cannot be sampled until it returns. */
            cpu_frequency_sample(cpu_slot);
            u32 i = fetch_add(&job_next, 1);
            if (i >= count) break;
            fn(arg, (int)i, (int)count);
            fetch_add(&job_done, 1);
        }

        smp_tlb_poll();

        if (want && want != mine) write_cr3(mine);
        seen = gen;
        worker_account_state(state, false);
        /* Release orders the last buffer writes and CR3 restoration before the
         * submitting core may free arguments/address-space mappings. Do not
         * touch any old batch fields after publishing this acknowledgment. */
        __atomic_store_n(&state->acknowledged, gen, __ATOMIC_RELEASE);
    }
}

/* Somewhere safe for a woken core to arrive while the path is being proved.
 *
 * It loads nothing, calls nothing and touches no shared state - so reaching it
 * says the trampoline is correct and separates that from whatever the core
 * does next, which is the only way to tell those two apart when the failure
 * mode of both is the machine restarting. */
static void ap_park(void) {
    fetch_add(&processors_arrived, 1);
    for (;;) __asm__ volatile("cli; hlt");
}

/* Where a core arrives from the trampoline. */
static void ap_entry(u64 stack_top) {
    static volatile u32 next_cpu_slot = 1;
    u32 slot = fetch_add(&next_cpu_slot, 1);
    /* Each AP needs its own available TSS descriptor and fault stacks before
     * the shared IDT's IST gates can be used. Never touch the BSP's busy TSS. */
    if (!gdt_load_on_this_cpu(slot))
        for (;;) __asm__ volatile("cli; hlt");
    idt_load_on_this_cpu();
    cpu_ap_init_status_t init = cpu_init_application_processor(slot, stack_top);
    if (init != CPU_AP_INIT_OK) {
        __atomic_store_n(&worker_state[slot].init_error, (u32)init, __ATOMIC_RELEASE);
        fetch_add(&processors_arrived, 1);
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (!lapic_init_worker() || !lapic_timer_start_local(4, SMP_SAMPLE_VECTOR)) {
        fetch_add(&processors_arrived, 1);
        for (;;) __asm__ volatile("cli; hlt");
    }
    cpu_topology_capture(slot);
    worker_loop(slot);
}

/* --------------------------------------------------------------- bring-up */

static bool start_one(u32 apic_id, u64 stack_top) {
    u8 *tramp = phys_to_virt(SMP_TRAMPOLINE_PHYS);

    *(volatile u64 *)(tramp + PARAM_STACK) = stack_top;
    *(volatile u64 *)(tramp + PARAM_ALIVE) = 0;
    tramp[0xF20] = tramp[0xF21] = 0;

    /* INIT, edge triggered, asserted once.
     *
     * Not the level-triggered assert-then-deassert pair that older writing
     * describes.  That pair is not supported in x2APIC mode at all, and this
     * machine uses x2APIC: sending it there does not start a core, it resets
     * the machine.  The symptom is a boot loop with no error - every log line
     * repeating at the same timestamp, because each one is a fresh boot.
     *
     * Edge-triggered assert on its own is what modern systems take, and works
     * either way round. */
    lapic_send_ipi(apic_id, ICR_INIT | ICR_ASSERT);
    timer_mdelay(10);

    /* Two startup messages, which is what the specification asks for: the
     * first is missed by some processors if it arrives too soon after INIT,
     * and a second costs nothing on one that already took the first. */
    for (int attempt = 0; attempt < 2; attempt++) {
        lapic_send_ipi(apic_id, ICR_STARTUP | ICR_ASSERT | SMP_VECTOR);

        for (int i = 0; i < 200; i++) {
            if (*(volatile u64 *)(tramp + PARAM_ALIVE)) return true;
            timer_mdelay(1);
        }
    }
    return false;
}

void smp_init(void) {
    cpu_topology_capture(0);
    if (!apic_available()) {
        kinfo("smp", "no local APIC, so this machine runs on one core");
        return;
    }
    if (g_acpi_cpu_count <= 1) {
        kinfo("smp", "firmware reports one processor");
        return;
    }
    bsp_apic_id = lapic_id();
    /* Install before any AP can enable interrupts. These permanent vector
     * handlers have no shared scheduler/driver state and require only EOI. */
    irq_install(SMP_WAKE_VECTOR, worker_wake_isr, NULL);
    irq_install(SMP_SAMPLE_VECTOR, worker_wake_isr, NULL);
    irq_install(SMP_TLB_VECTOR, tlb_isr, NULL);
    irq_install(SMP_RESCHED_VECTOR, application_reschedule_isr, NULL);
    irq_install(SMP_APP_TICK_VECTOR, application_timer_isr, NULL);
    /* Publish BSP participation before starting any other processor. */
    tlb_online = 1;

    u64 pml4 = vmm_kernel_pml4();
    if (pml4 >= 0x100000000ULL) {
        /* The trampoline loads CR3 while still in 32-bit mode, so the tables
         * have to be addressable there.  Saying so beats faulting each core. */
        kerr("smp", "page tables live above 4 GiB (%p); the startup path "
                    "cannot reach them, so this machine stays on one core",
             (void *)pml4);
        return;
    }

    /* The page the startup message names.  Reserved rather than allocated,
     * because its address is fixed by the message format. */
    pmm_reserve(SMP_TRAMPOLINE_PHYS, 1);

    size_t len = (size_t)(smp_trampoline_end - smp_trampoline_start);
    if (len > PAGE_SIZE) {
        kerr("smp", "the startup code is %zu bytes and must fit in one page", len);
        return;
    }

    u8 *tramp = phys_to_virt(SMP_TRAMPOLINE_PHYS);
    memcpy(tramp, smp_trampoline_start, len);
    *(volatile u64 *)(tramp + PARAM_PML4)  = pml4;
    /* ap_park stays reachable with `smppark` on the command line: it is what
     * separates "the startup path is wrong" from "what the core does next is
     * wrong", and those two fail the same way - the machine restarting. */
    *(volatile u64 *)(tramp + PARAM_ENTRY) =
        cmdline_has("smppark") ? (u64)&ap_park : (u64)&ap_entry;

    /* A core that does not start says nothing about why, so what it was given
     * is recorded here.  Every one of these has a way of being quietly wrong:
     * an empty blob, page tables the 32-bit half cannot reach, or a low page
     * that is not really memory. */
    kinfo("smp", "startup code %zu bytes at %#x, page tables %p, woken cores "
                 "%s", len, (u32)SMP_TRAMPOLINE_PHYS, (void *)pml4,
          cmdline_has("smppark") ? "park" : "take work");

    /* This page has to be mapped at its own address, and mapped executable.
     *
     * The trampoline turns paging on and the very next instruction it runs is
     * the one after that - out of this page, at this address.  If the kernel's
     * tables do not map it there, the waking core takes a fault it has no
     * handler for, which on this hardware means the machine resets.  From the
     * outside that is a boot loop with nothing logged: every line repeating at
     * the same timestamp because each one is a fresh boot.
     *
     * Checking that the top-level entry exists is not enough - it covers half
     * a terabyte and says nothing about this page in particular - so the
     * mapping is made rather than tested.  Mapping something already mapped
     * the same way costs nothing.
     */
    /* Mapped, and mapped so it can be executed.
     *
     * The trampoline turns paging on and the very next instruction it runs is
     * out of this page, at this address.  Being present is not enough: if the
     * entry covering it forbids execution, the fetch faults, and a fault on a
     * core with no interrupt table yet takes the whole machine down and
     * restarts it - a boot loop with nothing logged.
     *
     * The mapping here comes from the loader and is usually one large page
     * covering all of low memory, so the flag has to be found by walking to
     * whichever level actually holds the leaf rather than assumed.
     */
    {
        u64 *t = phys_to_virt(pml4);
        u64 e = t[(SMP_TRAMPOLINE_PHYS >> 39) & 0x1FF];
        int level = 4;

        while (e & PTE_P) {
            if (level == 1 || (e & PTE_PS)) break;
            t = phys_to_virt(e & PTE_ADDR);
            level--;
            int shift = level == 3 ? 30 : level == 2 ? 21 : 12;
            e = t[(SMP_TRAMPOLINE_PHYS >> shift) & 0x1FF];
        }

        if (!(e & PTE_P)) {
            kerr("smp", "the startup page is not mapped at its own address; "
                        "this machine stays on one core");
            return;
        }

        kinfo("smp", "the startup page is mapped by a level-%d entry %#llx%s",
              level, (unsigned long long)e,
              (e & PTE_NX) ? " which forbids execution" : "");

        if (e & PTE_NX) {
            /* Clearing it on the entry that actually covers the page, whatever
             * level that is.  The alternative - refusing to start any core -
             * is worse, and this page is ours: it was reserved above. */
            t[(SMP_TRAMPOLINE_PHYS >> (level == 3 ? 30 : level == 2 ? 21 : 12)) & 0x1FF]
                = e & ~PTE_NX;
            write_cr3(read_cr3());
            kinfo("smp", "cleared it, so the waking cores can run from there");
        }
    }

    u32 self = lapic_id();
    int started = 0, failed = 0;

    kinfo("smp", "this processor is APIC id %u; firmware lists %d",
          self, g_acpi_cpu_count);

    for (int i = 0; i < g_acpi_cpu_count; i++) {
        u32 id = g_acpi_cpus[i].apic_id;
        if (id == self) continue;
        if (!(g_acpi_cpus[i].flags & 1)) continue;   /* not enabled           */

        void *stack = kmalloc(AP_STACK_SIZE);
        if (!stack) { failed++; continue; }

        if (start_one(id, (u64)stack + AP_STACK_SIZE)) started++;
        else {
            u8 *t = phys_to_virt(SMP_TRAMPOLINE_PHYS);
            kwarn("smp", "processor with APIC id %u did not start: reached "
                         "protected mode %s, long mode %s",
                  id, t[0xF20] ? "yes" : "NO", t[0xF21] ? "yes" : "NO");
            failed++;
        }
    }

    /* Wait for the ones that answered to actually reach the worker loop, so
     * that a count reported here is a count of cores that can be given work. */
    for (int i = 0; i < 100 && (int)processors_arrived < started; i++)
        timer_mdelay(1);

    kinfo("smp", "%d of %d processors started; %u are taking work%s",
          started + 1, g_acpi_cpu_count, workers_online,
          failed ? " (some did not start)" : "");
    for (u32 slot = 1; slot < ARRAY_LEN(worker_state); slot++) {
        u32 error = __atomic_load_n(&worker_state[slot].init_error, __ATOMIC_ACQUIRE);
        if (error)
            kwarn("smp", "CPU slot %u remains offline: architectural setup error %u",
                  slot, error);
    }
}

/* ------------------------------------------------------------------ running */

int smp_worker_count(void) { return (int)__atomic_load_n(&workers_online, __ATOMIC_ACQUIRE); }

bool smp_worker_slot_online(u32 slot) {
    return slot > 0 && slot < ACPI_MAX_CPUS &&
           __atomic_load_n(&worker_state[slot].online, __ATOMIC_ACQUIRE);
}

bool smp_promote_one_application_cpu(void) {
    if (cpu_current_slot() != 0) return false;
    bool irq = spin_lock_irqsave(&batch_lock);
    /* A promotion is asynchronous. Count in-flight requests as reserved so
     * repeated callers cannot accidentally consume the final restricted AP
     * before any worker has changed its online role. */
    u32 pending = 0;
    for (u32 slot = 1; slot < ACPI_MAX_CPUS; slot++)
        pending += __atomic_load_n(&worker_state[slot].online, __ATOMIC_ACQUIRE) &&
                   __atomic_load_n(&worker_state[slot].promote_requested, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&workers_online, __ATOMIC_ACQUIRE) <= pending + 1) {
        spin_unlock_irqrestore(&batch_lock, irq);
        return false;
    }
    for (u32 slot = 1; slot < ACPI_MAX_CPUS; slot++) {
        worker_state_t *state = &worker_state[slot];
        if (!smp_worker_slot_online(slot) ||
            !sched_application_cpu_prepared(slot)) continue;
        u32 expected = 0;
        if (!__atomic_compare_exchange_n(&state->promote_requested, &expected, 1u,
                                         false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            continue;
        spin_unlock_irqrestore(&batch_lock, irq);
        lapic_send_ipi(state->apic_id, SMP_WAKE_VECTOR);
        return true;
    }
    spin_unlock_irqrestore(&batch_lock, irq);
    return false;
}

bool smp_worker_time_snapshot(smp_time_snapshot_t *out) {
    if (!out || !timer_cycles_per_us()) return false;
    smp_time_snapshot_t result = {0};
    for (u32 slot = 1; slot < ARRAY_LEN(worker_state); slot++) {
        worker_state_t *state = &worker_state[slot];
        if (!__atomic_load_n(&state->online, __ATOMIC_ACQUIRE)) continue;
        bool valid = false;
        for (u32 attempt = 0; attempt < 8; attempt++) {
            u32 seq = __atomic_load_n(&state->time_sequence, __ATOMIC_ACQUIRE);
            if (seq & 1u) continue;
            u64 busy = __atomic_load_n(&state->busy_us, __ATOMIC_RELAXED);
            u64 idle = __atomic_load_n(&state->idle_us, __ATOMIC_RELAXED);
            u64 then = __atomic_load_n(&state->transition_us, __ATOMIC_RELAXED);
            u32 executing = __atomic_load_n(&state->executing, __ATOMIC_RELAXED);
            u64 now = timer_now_us();
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (seq != __atomic_load_n(&state->time_sequence, __ATOMIC_RELAXED)) continue;
            if (now < then) return false;
            if (executing) busy += now - then; else idle += now - then;
            result.busy_us += busy;
            result.idle_us += idle;
            result.workers++;
            valid = true;
            break;
        }
        /* Never report an incomplete subset of online CPUs as a full sample. */
        if (!valid) return false;
    }
    *out = result;
    return true;
}

void smp_run(smp_job_fn fn, void *arg, int pieces) {
    smp_run_in(fn, arg, pieces, 0);
}

static void run_serial(smp_job_fn fn, void *arg, int pieces, u64 cr3) {
    u64 mine = read_cr3();
    bool switch_space = cr3 && cr3 != mine;
    bool irq = false;
    if (switch_space) {
        irq = irq_save();
        write_cr3(cr3);
    }
    for (int i = 0; i < pieces; i++) fn(arg, i, pieces);
    if (switch_space) {
        write_cr3(mine);
        irq_restore(irq);
    }
}

void smp_run_in(smp_job_fn fn, void *arg, int pieces, u64 cr3) {
    if (!fn || pieces <= 0) return;

    /* With no workers this is still correct, just not parallel - which is what
     * should happen on a machine with one core rather than a refusal. */
    if (!smp_worker_count() || pieces > MAX_JOBS) {
        run_serial(fn, arg, pieces, cr3);
        return;
    }

    /* AP jobs may not enter the BSP's process bookkeeping. A nested request
     * from a worker stays local rather than waiting on its own outer batch. */
    if (lapic_id() != bsp_apic_id) {
        run_serial(fn, arg, pieces, cr3);
        return;
    }
    bool irq = irq_save();
    proc_t *token = NULL;
    if (!proc_resource_pin(&token))
        panic("smp: cannot pin submitting process");
    /* A resource pin protects stack/exit, not individual mappings against a
     * sibling munmap. Borrowed user tables therefore still require BSP IRQ
     * exclusion until the VM layer gains retained mapping pins. Remote TLB
     * invalidation now exists, but cannot keep a removed mapping usable.
     * Kernel-buffer batches may be preempted; never spin for batch_lock on
     * the BSP because its owner could be the task we just preempted. */
    bool preemptible = irq && !cr3;
    u32 expected = 0;
    if (!__atomic_compare_exchange_n(&batch_lock.held, &expected, 1u, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        if (preemptible) sti();
        run_serial(fn, arg, pieces, cr3);
        cli();
        proc_resource_unpin(token);
        irq_restore(irq);
        return;
    }
    u64 targets = 0;
    for (u32 slot = 1; slot < ARRAY_LEN(worker_state); slot++)
        if (__atomic_load_n(&worker_state[slot].online, __ATOMIC_ACQUIRE))
            targets |= 1ULL << slot;

    job_fn    = fn;
    job_arg   = arg;
    job_cr3   = cr3;
    job_count = (u32)pieces;
    job_done  = 0;
    job_next  = 0;

    u64 gen = __atomic_load_n(&job_generation, __ATOMIC_RELAXED) + 1;
    __atomic_store_n(&job_generation, gen, __ATOMIC_RELEASE);
    for (u32 slot = 1; slot < ARRAY_LEN(worker_state); slot++)
        if (targets & (1ULL << slot))
            lapic_send_ipi(worker_state[slot].apic_id, SMP_WAKE_VECTOR);
    if (preemptible) sti();

    /* This core takes work too rather than watching: it is one of the twenty
     * and the batch finishes sooner for its help. */
    for (;;) {
        u32 i = fetch_add(&job_next, 1);
        if (i >= job_count) break;
        fn(arg, (int)i, pieces);
        fetch_add(&job_done, 1);
    }

    /* And wait for the rest, because the caller is about to read what they
     * wrote. */
    while (__atomic_load_n(&job_done, __ATOMIC_ACQUIRE) < (u32)pieces) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    /* A completed piece is not a completed AP: its stack may still be reading
     * the job record or restoring CR3. Wait for every notified AP, even one
     * that found no unclaimed pieces, before allowing the caller to return. */
    for (u32 slot = 1; slot < ARRAY_LEN(worker_state); slot++) {
        if (!(targets & (1ULL << slot))) continue;
        while (__atomic_load_n(&worker_state[slot].acknowledged, __ATOMIC_ACQUIRE) != gen) {
            smp_tlb_poll();
            __asm__ volatile("pause");
        }
    }
    cli();
    __atomic_store_n(&batch_lock.held, 0u, __ATOMIC_RELEASE);
    proc_resource_unpin(token);
    irq_restore(irq);
}
