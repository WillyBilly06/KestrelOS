/* sched.c - processes, threads and the round-robin scheduler.
 *
 * Every thread owns a kernel stack and is switched by context_switch, which
 * means a thread can block anywhere in the kernel and resume exactly where it
 * left off.  That is what lets read() wait for a keystroke inside the system
 * call rather than having to be restarted.
 */
#include "kernel.h"
#include "proc.h"
#include "cpu.h"
#include "mm.h"
#include "vfs.h"
#include "klog.h"
#include "time.h"
#include "smp.h"
#include "acpi.h"
#include "spinlock.h"

/* The stack a process runs on while it is inside the kernel.  Everything below
 * a system call shares it: the filesystem, the block driver, the network
 * stack.  An overflow does not fault - it walks off the bottom into whatever
 * is next in memory, and what is next is the saved registers of the process
 * that made the call, so the symptom is a program that returns from a
 * perfectly ordinary call to address zero.  Sixty-four kilobytes leaves room
 * for the deepest path there is, which is a write reaching disk. */
#define KSTACK_SIZE      (64 * 1024)
#define BOOT_STACK_BYTES (32 * 1024)   /* must match main.c's BOOT_STACK_SIZE */
_Static_assert(sizeof(regs_t) == 176 && sizeof(regs_t) % 16 == 0,
               "thread_bootstrap C call requires a 16-byte-aligned register frame");

extern void context_switch(u64 *save_rsp, u64 new_rsp);
extern void thread_bootstrap(void);
extern void switch_stack_and_call(u64 stack_top, void (*fn)(void)) __attribute__((noreturn));

static proc_t  procs[PROC_MAX];
/* AP fallback contexts are scheduler objects, not processes. Keeping them
 * outside the process table preserves all 64 user/kernel task slots and keeps
 * unjoined APs out of process reports and runnable selection. */
static proc_t application_idle[ACPI_MAX_CPUS];
/* Running context belongs to the logical CPU, not the whole machine. APs that
 * have only joined the restricted worker pool deliberately have no application
 * context. Never let an AP accidentally borrow the BSP's process identity,
 * resource pins, kernel stack, or time slice through a global `current`. */
typedef struct {
    proc_t *running;
    proc_t *idle_task;
    u64 last_switch;
    u64 busy_ms, idle_ms;
    u32 application_online;
    bool queue_locked;
    proc_t *switch_from;
} __attribute__((aligned(64))) sched_cpu_t;
static sched_cpu_t sched_cpus[ACPI_MAX_CPUS];

static sched_cpu_t *scheduler_local(void) {
    u32 cpu = cpu_current_slot();
    if (cpu >= ACPI_MAX_CPUS) panic("scheduler: CPU has no private descriptor slot");
    return &sched_cpus[cpu];
}

#define current (scheduler_local()->running)
#define idle    (scheduler_local()->idle_task)

static bool scheduler_local_online(void) {
    u32 cpu = cpu_current_slot();
    return cpu < ACPI_MAX_CPUS &&
           __atomic_load_n(&sched_cpus[cpu].application_online, __ATOMIC_ACQUIRE);
}

/* Selection, CPU ownership, wakeups and runnable publication share one lock.
 * It crosses context_switch and is released on the incoming stack, never while
 * the outgoing CPU is still writing that thread's saved registers/stack. */
static spinlock_t runqueue_lock;

static bool queue_lock(void) {
    bool irq = irq_save();
    sched_cpu_t *local = scheduler_local();
    if (local->queue_locked) panic("scheduler: recursive runqueue lock");
    while (__atomic_exchange_n(&runqueue_lock.held, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    local->queue_locked = true;
    return irq;
}

static void queue_unlock(bool irq) {
    sched_cpu_t *local = scheduler_local();
    if (!local->queue_locked || local->switch_from)
        panic("scheduler: invalid runqueue unlock");
    local->queue_locked = false;
    spin_unlock_irqrestore(&runqueue_lock, irq);
}

/* Runqueue held. An idle CPU has no local work to make it leave HLT; its
 * timer will eventually wake it, but a new runnable thread should not wait
 * for that tick. Never send the IPI while holding the queue lock. */
static u32 idle_wakeup_target(void) {
    for (u32 cpu = 0; cpu < ACPI_MAX_CPUS; cpu++) {
        sched_cpu_t *local = &sched_cpus[cpu];
        if (cpu == cpu_current_slot() ||
            !__atomic_load_n(&local->application_online, __ATOMIC_ACQUIRE))
            continue;
        if (local->idle_task && local->running == local->idle_task)
            return cpu;
    }
    return PROC_CPU_NONE;
}

static void wake_idle_cpu(u32 target) {
    if (target != PROC_CPU_NONE) smp_reschedule_cpu(target);
}

static void publish_ready(proc_t *p) {
    bool irq = queue_lock();
    if (p->state != PROC_SETUP || p->running_cpu != PROC_CPU_NONE)
        panic("scheduler: publishing an owned or initialized thread twice");
    p->state = PROC_READY;
    u32 target = idle_wakeup_target();
    queue_unlock(irq);
    wake_idle_cpu(target);
}

static void discard_setup(proc_t *p) {
    bool irq = queue_lock();
    if (p->state != PROC_SETUP || p->running_cpu != PROC_CPU_NONE)
        panic("scheduler: discarding a live context");
    p->state = PROC_UNUSED;
    queue_unlock(irq);
}

/* BSP scheduler tick accounting. System reports add independently measured AP
 * worker intervals in sched_cpu_time_snapshot; these two counters alone must
 * not be labelled utilization of the whole machine. */
u64 g_cpu_busy_ms;
u64 g_cpu_idle_ms;
static int     next_pid = 1;
static u64     next_gpu_owner = 1;
static bool    sched_running;

/* Where each thread's kernel stack pointer is parked while it is not running.
 * Kept beside the process table rather than inside proc_t so the assembly only
 * ever sees a plain u64. */
static u64 saved_rsp[PROC_MAX + ACPI_MAX_CPUS];

typedef struct {
    sched_device_probe_fn probe;
    void *context;
    u64 deadline_ms;
} device_wait_t;
static device_wait_t device_waits[PROC_MAX];
static u32 device_wait_count;

static inline int slot_of(proc_t *p) {
    u64 address = (u64)p;
    if (address >= (u64)procs && address < (u64)(procs + PROC_MAX) &&
        (address - (u64)procs) % sizeof(proc_t) == 0)
        return (int)(p - procs);
    if (address >= (u64)application_idle &&
        address < (u64)(application_idle + ACPI_MAX_CPUS) &&
        (address - (u64)application_idle) % sizeof(proc_t) == 0)
        return PROC_MAX + (int)(p - application_idle);
    panic("scheduler: context is outside the task and idle tables");
}

proc_t *proc_current(void) {
    u32 cpu = cpu_current_slot();
    return cpu < ACPI_MAX_CPUS ? sched_cpus[cpu].running : NULL;
}

/* AP kernel workers are not application scheduling capacity. Update this only
 * when a processor actually joins a safe application run queue. */
u32 sched_application_cpu_count(void) {
    u32 count = 0;
    for (u32 cpu = 0; cpu < ACPI_MAX_CPUS; cpu++)
        count += !!__atomic_load_n(&sched_cpus[cpu].application_online, __ATOMIC_ACQUIRE);
    return count;
}

bool sched_application_cpu_online(u32 slot) {
    return slot < ACPI_MAX_CPUS &&
           __atomic_load_n(&sched_cpus[slot].application_online, __ATOMIC_ACQUIRE);
}

bool sched_application_cpu_prepared(u32 slot) {
    if (!slot || slot >= ACPI_MAX_CPUS) return false;
    proc_t *p = __atomic_load_n(&sched_cpus[slot].idle_task, __ATOMIC_ACQUIRE);
    return p == &application_idle[slot] && p->kstack && p->kstack_top > p->kstack &&
           p->is_idle && p->is_kernel && p->state == PROC_SETUP &&
           p->running_cpu == PROC_CPU_NONE &&
           !__atomic_load_n(&sched_cpus[slot].application_online, __ATOMIC_ACQUIRE);
}

bool sched_cpu_time_snapshot(u64 *busy_ms, u64 *idle_ms) {
    if (!busy_ms || !idle_ms) return false;
    bool irq = irq_save();
    smp_time_snapshot_t ap = {0};
    if (smp_worker_count() && !smp_worker_time_snapshot(&ap)) {
        irq_restore(irq);
        return false;
    }
    /* Sum measured AP intervals, never multiply BSP usage by a core count.
     * Keep the existing ABI's millisecond counters and coherent BSP snapshot. */
    u64 busy = g_cpu_busy_ms + ap.busy_us / 1000u;
    u64 idle_time = g_cpu_idle_ms + ap.idle_us / 1000u;
    for (u32 cpu = 1; cpu < ACPI_MAX_CPUS; cpu++) {
        if (!__atomic_load_n(&sched_cpus[cpu].application_online, __ATOMIC_ACQUIRE))
            continue;
        busy += __atomic_load_n(&sched_cpus[cpu].busy_ms, __ATOMIC_RELAXED);
        idle_time += __atomic_load_n(&sched_cpus[cpu].idle_ms, __ATOMIC_RELAXED);
    }
    *busy_ms = busy;
    *idle_ms = idle_time;
    irq_restore(irq);
    return true;
}

/* Process contexts still run on the BSP, but shared runqueue ownership makes
 * pin/kill decisions atomic with process-table transitions; interrupts
 * remain enabled throughout the device work itself. A token names the exact
 * thread even if another task runs before the transaction retires. */
bool proc_resource_pin(proc_t **token) {
    if (!token) return false;
    *token = NULL;
    bool irq = queue_lock();
    proc_t *p = current;
    if (!p) {
        /* A boot caller may operate before processes exist. An AP after
         * scheduler bring-up is not that caller and cannot borrow its rights. */
        bool boot = !__atomic_load_n(&sched_running, __ATOMIC_ACQUIRE) && cpu_current_slot() == 0;
        queue_unlock(irq);
        return boot;
    }
    proc_t *lead = proc_shared(p);
    if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE ||
        p->resource_pins == 0xffffffffu ||
        (lead->resource_closing && !p->resource_closing)) {
        queue_unlock(irq);
        return false;
    }
    /* The exiting thread may perform its own device cleanup; siblings cannot
     * start new transactions once the leader has begun draining them. */
    p->resource_pins++;
    *token = p;
    queue_unlock(irq);
    return true;
}

void proc_resource_unpin(proc_t *token) {
    if (!token) return;
    bool irq = queue_lock();
    if (!token->resource_pins) panic("unbalanced process resource pin");
    token->resource_pins--;
    queue_unlock(irq);
}

/* Called with runqueue ownership held. */
static bool proc_resource_group_busy(proc_t *lead) {
    if (lead->resource_pins || (lead != current && lead->kernel_active) ||
        (lead != current && lead->running_cpu != PROC_CPU_NONE)) return true;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &procs[i];
        if (t->state != PROC_UNUSED && t->leader == lead &&
            (t->resource_pins || t->state == PROC_SETUP ||
             (t != current && (t->running_cpu != PROC_CPU_NONE || t->kernel_active ||
                               t->clear_child_tid)))) return true;
    }
    return false;
}

static void retire_stopped_threads(void);

static void proc_resource_drain(proc_t *p) {
    /* Exiting inside a still-owned transaction is a kernel programming error;
     * never free the live stack or "recover" by unlocking an active GPU job. */
    if (p->resource_pins) panic("process exits with a live resource transaction");
    for (;;) {
        bool irq = queue_lock();
        p->resource_closing = true;
        retire_stopped_threads();
        bool busy = proc_resource_group_busy(p);
        queue_unlock(irq);
        if (!busy) return;
        sched_yield(); /* let pinned siblings finish with interrupts enabled */
    }
}

static proc_t *proc_by_pid(int pid) {
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state != PROC_UNUSED && procs[i].pid == pid) return &procs[i];
    return NULL;
}

/* How many processes there are.
 *
 * A thread is an entry in this same table with its leader set, so counting
 * every used entry counts threads as processes - which is what this did, and
 * is why a system with a handful of programs running reported many more
 * "processes" than anybody had started.  A process is an entry that leads
 * itself. */
int proc_count(void) {
    u32 count;
    proc_count_snapshot(&count, NULL);
    return (int)count;
}

/* And how many threads, which is every used entry: a process is its own first
 * thread, so the two counts are equal on a system where nothing has started a
 * second one. */
int proc_thread_count(void) {
    u32 count;
    proc_count_snapshot(NULL, &count);
    return (int)count;
}

void proc_count_snapshot(u32 *processes, u32 *threads) {
    bool irq = queue_lock();
    u32 leaders = 0, entries = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) continue;
        entries++;
        if (!procs[i].leader) leaders++;
    }
    if (processes) *processes = leaders;
    if (threads) *threads = entries;
    queue_unlock(irq);
}

bool proc_pid_alive(int pid) {
    bool irq = queue_lock();
    proc_t *p = proc_by_pid(pid);
    bool alive = p && p->state != PROC_ZOMBIE;
    queue_unlock(irq);
    return alive;
}

bool proc_report_snapshot(u32 index, proc_report_t *out) {
    if (!out || index >= PROC_MAX) return false;
    bool irq = queue_lock();
    bool found = false;
    for (int i = 0; i < PROC_MAX; i++) {
        const proc_t *p = &procs[i];
        if (p->state == PROC_UNUSED) continue;
        if (index--) continue;
        memset(out, 0, sizeof *out);
        out->pid = p->pid;
        out->parent_pid = p->parent_pid;
        out->state = (u32)p->state;
        memcpy(out->name, p->name, sizeof out->name);
        out->name[sizeof out->name - 1] = 0;
        out->cpu_ms = p->cpu_ms;
        /* ELF setup writes image bounds without the queue lock; READY
         * publication is their release barrier. Never inspect mutable setup
         * fields, and never follow pml4/leader pointers for reporting. */
        if (p->state != PROC_SETUP && p->image_end > p->image_base)
            out->image_bytes = p->image_end - p->image_base;
        found = true;
        break;
    }
    queue_unlock(irq);
    return found;
}

void proc_describe_current(char *buf, size_t cap) {
    if (current && !current->is_kernel) snprintf(buf, cap, "%s (pid %d)", current->name, current->pid);
    else if (current) snprintf(buf, cap, "kernel thread %s", current->name);
    else strlcpy(buf, "the kernel", cap);
}

/* ------------------------------------------------------------------------- */
/* allocation                                                                */
/* ------------------------------------------------------------------------- */

static proc_t *proc_alloc(const char *name) {
    bool irq = queue_lock();
    if (!next_gpu_owner) { queue_unlock(irq); return NULL; }
    for (int i = 0; i < PROC_MAX; i++) {
        if (procs[i].state != PROC_UNUSED) continue;
        proc_t *p = &procs[i];
        memset(p, 0, sizeof *p);
        p->running_cpu = PROC_CPU_NONE;
        p->pid = next_pid++;
        p->gpu_owner_id = next_gpu_owner++;
        p->parent_pid = current ? current->pid : 0;
        strlcpy(p->name, name, sizeof p->name);
        strlcpy(p->cwd, "/", sizeof p->cwd);
        p->waiting_for = -1;
        p->started_ms = g_uptime_ms;
        p->state = PROC_SETUP;      /* made READY once it can actually run */
        queue_unlock(irq);
        return p;
    }
    queue_unlock(irq);
    kerr("proc", "process table is full (%d entries)", PROC_MAX);
    return NULL;
}

static void proc_release(proc_t *p) {
    proc_fd_close_all(p);
    /* A thread borrowed its address space from the process; tearing it down
     * here would take the whole process with it. */
    if (p->pml4 && !p->leader) vmm_destroy_address_space(p->pml4);
    p->pml4 = 0;
    if (p->kstack) { pmm_free_pages(virt_to_phys((void *)p->kstack), KSTACK_SIZE / PAGE_SIZE); p->kstack = 0; }
    discard_setup(p);
}

bool proc_gpu_owner_alive(u64 owner) {
    if (!owner) return false;
    bool irq = queue_lock();
    bool alive = false;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &procs[i];
        if (!p->leader && p->gpu_owner_id == owner &&
            p->state != PROC_UNUSED && p->state != PROC_ZOMBIE &&
            !p->resource_closing) { alive = true; break; }
    }
    queue_unlock(irq);
    return alive;
}

/* Lay out a fresh kernel stack so that context_switch "returns" into
 * thread_bootstrap with `frame` sitting immediately above. */
static bool prepare_stack(proc_t *p, const regs_t *frame) {
    u64 phys = pmm_alloc_pages(KSTACK_SIZE / PAGE_SIZE);
    if (!phys) { kerr("proc", "no memory for %s's kernel stack", p->name); return false; }
    p->kstack = (u64)phys_to_virt(phys);
    p->kstack_top = p->kstack + KSTACK_SIZE;
    memset((void *)p->kstack, 0, KSTACK_SIZE);

    u64 top = p->kstack_top;

    /* The register frame the bootstrap will restore. */
    regs_t *f = (regs_t *)(top - sizeof(regs_t));
    *f = *frame;
    p->frame = f;

    /* Below it, the exact image context_switch expects to pop. */
    u64 *sp = (u64 *)f;
    *--sp = (u64)thread_bootstrap;   /* the address `ret` will jump to */
    *--sp = 0;                       /* rbp */
    *--sp = 0;                       /* rbx */
    *--sp = 0;                       /* r12 */
    *--sp = 0;                       /* r13 */
    *--sp = 0;                       /* r14 */
    *--sp = 0;                       /* r15 */
    *--sp = 0x002;                   /* rflags: interrupts off until iretq */

    saved_rsp[slot_of(p)] = (u64)sp;
    return true;
}

/* ------------------------------------------------------------------------- */
/* switching                                                                 */
/* ------------------------------------------------------------------------- */

/* Device-wait state protected by the shared runqueue lock.
 * A waiter remains registered until its own pinned stack resumes. Generic
 * wakeups can be spurious; the waiter rechecks its predicate before returning.
 * Hardware IRQ and timer paths both use this bounded probe, so an absent or
 * coalesced device interrupt cannot strand a thread past its deadline. */
static proc_t *poll_device_waiters(void) {
    /* Device predicates belong to the BSP's legacy driver entry context.
     * Future application APs may schedule user code, but cannot run these
     * callbacks while driver/IRQ ownership remains BSP-only. */
    if (cpu_current_slot() != 0) return NULL;
    if (!device_wait_count) return NULL;
    int start = current ? slot_of(current) : 0;
    proc_t *ready = NULL;
    for (int i = 1; i <= PROC_MAX; i++) {
        int slot = (start + i) % PROC_MAX;
        device_wait_t *w = &device_waits[slot];
        proc_t *p = &procs[slot];
        if (!w->probe) continue;
        if (p->state == PROC_BLOCKED &&
            (w->probe(w->context) || g_uptime_ms >= w->deadline_ms))
            p->state = PROC_READY;
        if (!ready && p->state == PROC_READY && p->running_cpu == PROC_CPU_NONE)
            ready = p;
    }
    return ready;
}

/* User-only AP dispatch rule. A suspended syscall or stopped thread must
 * resume on the BSP for its kernel continuation and cleanup. This predicate
 * alone does not admit AP application execution. */
static bool sched_can_run_on_cpu(const proc_t *p, u32 cpu) {
    return cpu == 0 || (!p->is_kernel && !p->kernel_active && !p->stop_requested);
}

static proc_t *pick_next(void) {
    u32 cpu = cpu_current_slot();
    if (cpu == 0) retire_stopped_threads();
    proc_t *device = poll_device_waiters();
    if (device) return device;
    int start = current ? slot_of(current) : 0;

    /* Round robin from just after the current slot, so nothing starves. */
    for (int i = 1; i <= PROC_MAX; i++) {
        proc_t *p = &procs[(start + i) % PROC_MAX];
        if (p->is_idle || p->running_cpu != PROC_CPU_NONE ||
            !sched_can_run_on_cpu(p, cpu)) continue;
        if (p->state == PROC_READY) return p;
        if (p->state == PROC_SLEEPING && g_uptime_ms >= p->wake_at_ms) {
            p->state = PROC_READY;
            return p;
        }
    }
    /* Nothing runnable: wake anything whose sleep has expired, else idle. */
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].running_cpu == PROC_CPU_NONE && !procs[i].is_idle &&
            sched_can_run_on_cpu(&procs[i], cpu) &&
            procs[i].state == PROC_SLEEPING && g_uptime_ms >= procs[i].wake_at_ms) {
            procs[i].state = PROC_READY;
            return &procs[i];
        }
    if (current && current->state == PROC_RUNNING &&
        sched_can_run_on_cpu(current, cpu)) return current;
    return idle;
}

static void reap_dead_threads(void);
void sched_switch_complete(void);

static void switch_to(proc_t *next) {
    if (!scheduler_local_online() || !next)
        panic("scheduler: switch without an enrolled application CPU");
    sched_cpu_t *local = scheduler_local();
    u32 cpu = cpu_current_slot();
    if (!local->queue_locked || local->switch_from)
        panic("scheduler: context selection without runqueue ownership");
    proc_t *prev = current;
    if (next == prev) {
        if (prev->running_cpu != cpu) panic("scheduler: current context lost its CPU");
        prev->state = PROC_RUNNING;
        if (cpu == 0) reap_dead_threads();
        queue_unlock(false);
        return;
    }
    if (!prev || prev->running_cpu != cpu || next->running_cpu != PROC_CPU_NONE ||
        (next->state != PROC_READY && !next->is_idle))
        panic("scheduler: selecting an owned or non-runnable context");
    /* Keep prev owned until its registers and RSP have actually been saved. */
    local->switch_from = prev;
    next->running_cpu = cpu;
    next->state = PROC_RUNNING;
    current = next;

    /* An interrupt or syscall arriving from user mode must land on this
     * thread's kernel stack, not the previous one's. */
    tss_set_rsp0(next->kstack_top);

    if (prev && !prev->is_kernel) fpu_save(prev->fpu);
    if (!next->is_kernel) fpu_restore(next->fpu);

    u64 want_cr3 = next->pml4 ? next->pml4 : vmm_kernel_pml4();
    if ((read_cr3() & ~PAGE_MASK) != want_cr3) write_cr3(want_cr3);

    /* Ordinary kernel/interrupt code does not use GS. SYSCALL temporarily
     * swaps to this CPU's reserved KGS scratch, then restores user GS before
     * entering C. Keep the reserved per-CPU KGS base unchanged here. */
    if (!next->is_kernel) {
        wrmsr(MSR_GS_BASE, next->gsbase);
        wrmsr(MSR_FS_BASE, next->fsbase);
    }

    context_switch(&saved_rsp[slot_of(prev)], saved_rsp[slot_of(next)]);

    /* These are another invocation's locals after a resume. Obtain the outgoing
     * thread from CPU-local handoff state, not stale locals on this stack. */
    sched_switch_complete();
}

/* First entry calls this from thread_bootstrap too. IF remains clear: restoring
 * the caller's IRQ state belongs to schedule/block/iret on the resumed thread. */
void sched_switch_complete(void) {
    sched_cpu_t *local = scheduler_local();
    proc_t *prev = local->switch_from;
    u32 cpu = cpu_current_slot();
    if (!local->queue_locked || !prev || prev == current ||
        prev->running_cpu != cpu || !current || current->running_cpu != cpu)
        panic("scheduler: invalid saved-context handoff");
    if (prev->state == PROC_RUNNING) prev->state = PROC_READY;
    prev->running_cpu = PROC_CPU_NONE;
    local->switch_from = NULL;
    if (cpu == 0) reap_dead_threads();
    queue_unlock(false);
}

/* A thread has no parent waiting to collect it, so the next thread to run
 * does it instead. */
static void reap_dead_threads(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &procs[i];
        if (p->state != PROC_ZOMBIE || !p->reapable ||
            p->running_cpu != PROC_CPU_NONE) continue;
        if (p->kstack) {
            pmm_free_pages(virt_to_phys((void *)p->kstack), KSTACK_SIZE / PAGE_SIZE);
            p->kstack = 0;
        }
        p->state = PROC_UNUSED;
    }
}

/* Give up the CPU.  Returns once this thread is scheduled again. */
static void schedule(void) {
    if (!scheduler_local_online()) return;
    bool irq = queue_lock();
    switch_to(pick_next());
    irq_restore(irq);
}

void sched_yield(void) { schedule(); }

void sched_block(proc_state_t why) {
    if (!scheduler_local_online())
        panic("scheduler: restricted CPU attempted a blocking operation");
    bool irq = queue_lock();
    if (current && current->kernel_active && current->stop_requested) {
        queue_unlock(irq);
        return;
    }
    if (current) current->state = why;
    switch_to(pick_next());
    irq_restore(irq);
}

void sched_wake(proc_t *p) {
    if (!p) return;
    bool irq = queue_lock();
    bool changed = p->state == PROC_BLOCKED || p->state == PROC_SLEEPING;
    if (changed) p->state = PROC_READY;
    u32 target = changed ? idle_wakeup_target() : PROC_CPU_NONE;
    queue_unlock(irq);
    wake_idle_cpu(target);
}

void sched_sleep_ms(u64 ms) {
    if (!scheduler_local_online() || !current) { timer_mdelay((u32)ms); return; }
    bool irq = queue_lock();
    if (current->kernel_active && current->stop_requested) { queue_unlock(irq); return; }
    u64 now = g_uptime_ms;
    current->wake_at_ms = ms >= ~0ull - now ? ~0ull - 1 : now + ms;
    current->state = PROC_SLEEPING;
    switch_to(pick_next());
    irq_restore(irq);
}

/* Sleeping metadata mutex, not the page-table spinlock. Never hold a driver
 * lock while acquiring it. All checks/registration/publication share the queue
 * lock, so unlock cannot race past a waiter preparing to sleep. */
bool proc_vm_begin(proc_t **token) {
    if (!token) return false;
    *token = NULL;
    if (!scheduler_local_online() || !current || current->is_kernel || !current->pml4)
        return false;
    proc_t *p;
    if (!proc_resource_pin(&p)) return false;
    bool irq = queue_lock();
    proc_t *lead = proc_shared(p);
    if (lead->vm_owner == p || p->vm_waiting)
        panic("scheduler: recursive address-space transaction");
    for (;;) {
        if (p->resource_closing || lead->resource_closing) {
            p->vm_waiting = false;
            queue_unlock(irq);
            proc_resource_unpin(p);
            return false;
        }
        if (!lead->vm_owner) break;
        p->vm_waiting = true;
        p->state = PROC_BLOCKED;
        switch_to(pick_next());
        queue_lock();
        p->vm_waiting = false;
    }
    lead->vm_owner = p;
    *token = p;
    queue_unlock(irq);
    return true;
}

void proc_vm_end(proc_t *token) {
    bool irq = queue_lock();
    if (!token || token != current || !token->resource_pins ||
        proc_shared(token)->vm_owner != token)
        panic("scheduler: invalid address-space transaction release");
    proc_t *lead = proc_shared(token);
    lead->vm_owner = NULL;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &procs[i];
        if (p->vm_waiting && proc_shared(p) == lead && p->state == PROC_BLOCKED)
            p->state = PROC_READY;
    }
    queue_unlock(irq);
    proc_resource_unpin(token);
}

int sched_futex_wait_ex(u64 uaddr, u32 expect, u64 deadline, u32 mask, bool private_key) {
    if (!scheduler_local_online() || !current || current == idle || !current->pml4)
        return -E_INVAL;
    if (!mask || (uaddr & 3u)) return -E_INVAL;
    bool irq = queue_lock();
    proc_t *p = current;
    u64 key;
    u32 value;
    if (p->futex_waiting) { queue_unlock(irq); return -E_INVAL; }
    if (!vmm_user_word_key(p->pml4, uaddr, &key, &value)) {
        queue_unlock(irq);
        return -E_FAULT;
    }
    if (value != expect) { queue_unlock(irq); return -E_AGAIN; }
    if (deadline != ~0ull && g_uptime_ms >= deadline) { queue_unlock(irq); return -E_TIMEDOUT; }
    p->futex_key = private_key ? uaddr : key;
    p->futex_space = private_key ? p->pml4 : 0;
    p->futex_mask = mask;
    p->futex_woken = false;
    p->futex_waiting = true;
    /* VMM validation/sample above and registration share this queue lock with
     * wake. The nested order is runqueue -> VMM; VMM does not enter scheduler
     * code and lock contenders service TLB requests. No VM lock crosses sleep.
     * After sleeping only the key is retained, never a user-memory pointer. */
    while (!p->futex_woken && !p->stop_requested &&
           (deadline == ~0ull || g_uptime_ms < deadline)) {
        p->wake_at_ms = deadline;
        p->state = deadline == ~0ull ? PROC_BLOCKED : PROC_SLEEPING;
        switch_to(pick_next());
        queue_lock();
    }
    bool woken = p->futex_woken;
    bool stopped = p->stop_requested;
    p->futex_waiting = p->futex_woken = false;
    p->futex_key = p->futex_space = 0;
    p->futex_mask = 0;
    queue_unlock(irq);
    return woken ? 0 : stopped ? -E_INTR : -E_TIMEDOUT;
}

int sched_futex_wait(u64 uaddr, u32 expect, int timeout_ms) {
    u64 now = g_uptime_ms;
    u64 deadline = timeout_ms < 0 ? ~0ull :
        ((u64)timeout_ms >= ~0ull - now ? ~0ull - 1 : now + (u64)timeout_ms);
    int result = sched_futex_wait_ex(uaddr, expect, deadline, ~0u, false);
    if (result == -E_FAULT) return -E_INVAL;
    if (result == -E_INTR || result == -E_TIMEDOUT) return -E_BUSY;
    return result;
}

/* Runqueue held; reused by the exit-time clear-and-wake transaction. */
static int futex_wake_key_locked(u64 key, u64 space, u32 mask, int count) {
    int woke = 0;
    for (int i = 0; i < PROC_MAX && woke < count; i++) {
        proc_t *p = &procs[i];
        if (!p->futex_waiting || p->futex_woken || p->futex_key != key ||
            p->futex_space != space || !(p->futex_mask & mask) ||
            (p->state != PROC_BLOCKED && p->state != PROC_SLEEPING &&
             p->state != PROC_READY && p->state != PROC_RUNNING)) continue;
        p->futex_woken = true;
        if (p->state == PROC_BLOCKED || p->state == PROC_SLEEPING)
            p->state = PROC_READY;
        woke++;
    }
    /* Killed slots own no external waiter record: dead states are excluded and
     * proc_alloc clears registration before slot reuse. A duplicate wake does
     * not consume count for a waiter that was already selected. */
    return woke;
}

int sched_futex_wake_ex(u64 uaddr, int count, u32 mask, bool private_key) {
    if (!scheduler_local_online() || !current || !current->pml4) return -E_INVAL;
    if (!mask || count < 0 || (uaddr & 3u)) return -E_INVAL;
    bool irq = queue_lock();
    u64 key = uaddr;
    /* Private wake never reads user memory. Its key survives mapping changes;
     * only the user-address bounds and retained address-space identity matter. */
    if (private_key ? (uaddr < PAGE_SIZE || uaddr > (1ull << 47) - sizeof(u32)) :
        !vmm_user_word_key(current->pml4, uaddr, &key, NULL)) {
        queue_unlock(irq);
        return -E_FAULT;
    }
    int woke = futex_wake_key_locked(private_key ? uaddr : key,
                                    private_key ? current->pml4 : 0, mask, count);
    queue_unlock(irq);
    return woke;
}

int sched_futex_wake(u64 uaddr, int count) {
    int result = sched_futex_wake_ex(uaddr, count < 0 ? 0 : count, ~0u, false);
    return result == -E_FAULT ? -E_INVAL : result;
}

int proc_set_tid_address(u64 address) {
    if (!scheduler_local_online() || !current || current->is_kernel) return -E_PERM;
    bool irq = queue_lock();
    /* Linux permits an invalid pointer here; failure to clear it on exit is
     * ignored. Registration itself never dereferences or pins user memory. */
    current->clear_child_tid = address;
    int tid = current->pid;
    queue_unlock(irq);
    return tid;
}

static void thread_clear_tid_locked(proc_t *p) {
    u64 address = p->clear_child_tid;
    p->clear_child_tid = 0;
    if (!address || !p->pml4) return;
    if (p != current) panic("scheduler: clearing TID outside its owning thread");
    u64 key;
    /* Registration, zero store and waiter selection serialize under runqueue
     * -> VMM. Never resolve a second key after an intervening unmap/remap. */
    if (vmm_user_store_word_key(p->pml4, address, 0, &key))
        (void)futex_wake_key_locked(key, 0, ~0u, 1);
}

int sched_wait_event(u64 *sequence, u64 expected, u64 deadline_ms) {
    if (!sequence || !scheduler_local_online() || !current || current == idle)
        return -1;
    bool irq = queue_lock();
    proc_t *p = current;
    if (p->event_sequence) { queue_unlock(irq); return -1; }
    p->event_sequence = sequence;
    /* Signal takes this same lock. It either changes the counter before our
     * recheck, or observes the blocked state after context save releases it.
     * There is no unlocked test-to-sleep window, even on another CPU. */
    while (!p->stop_requested && __atomic_load_n(sequence, __ATOMIC_ACQUIRE) == expected &&
           (deadline_ms == ~0ull || g_uptime_ms < deadline_ms)) {
        p->wake_at_ms = deadline_ms;
        p->state = deadline_ms == ~0ull ? PROC_BLOCKED : PROC_SLEEPING;
        switch_to(pick_next());
        queue_lock();
    }
    bool changed = __atomic_load_n(sequence, __ATOMIC_ACQUIRE) != expected;
    bool stopped = p->stop_requested;
    p->event_sequence = NULL;
    queue_unlock(irq);
    return stopped ? -1 : changed ? 1 : 0;
}

void sched_signal_event(u64 *sequence) {
    if (!sequence) return;
    bool irq = queue_lock();
    __atomic_fetch_add(sequence, 1ull, __ATOMIC_RELEASE);
    bool woke = false;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &procs[i];
        if (p->event_sequence == sequence &&
            (p->state == PROC_BLOCKED || p->state == PROC_SLEEPING)) {
            p->state = PROC_READY;
            woke = true;
        }
    }
    /* A dead waiter can never be made runnable; proc_alloc clears its old
     * registration before reusing the slot. No external proc_t pointer lives
     * in the event source or has to be removed by asynchronous kill. */
    u32 target = woke ? idle_wakeup_target() : PROC_CPU_NONE;
    queue_unlock(irq);
    wake_idle_cpu(target);
}

bool sched_device_wait_allowed(void) {
    return scheduler_local_online() && current && current != idle &&
           current->state == PROC_RUNNING && current->resource_pins &&
           (read_flags() & 0x200u);
}

int sched_wait_device(sched_device_probe_fn probe, void *context, u64 deadline_ms) {
    if (!probe || !sched_device_wait_allowed()) return -1;
    bool irq = queue_lock();
    proc_t *p = current;
    device_wait_t *w = &device_waits[slot_of(p)];
    if (w->probe) { queue_unlock(irq); return -1; }
    bool complete;
    while (!(complete = probe(context)) && g_uptime_ms < deadline_ms) {
        *w = (device_wait_t){probe, context, deadline_ms};
        device_wait_count++;
        p->state = PROC_BLOCKED;
        switch_to(pick_next());
        /* We own this same stack again, still with interrupts disabled. */
        queue_lock();
        *w = (device_wait_t){0};
        device_wait_count--;
    }
    queue_unlock(irq);
    return complete ? 1 : 0;
}

regs_t *sched_device_irq(regs_t *frame) {
    if (!scheduler_local_online() || !current || !device_wait_count) return frame;
    /* Exactly like the timer switch, the interrupt frame stays on the old
     * thread's stack. Do not call while an ISR still owns a device/RM lock. */
    bool irq = queue_lock();
    proc_t *next = poll_device_waiters();
    if (next) switch_to(next);
    else queue_unlock(false);
    irq_restore(irq);
    return frame;
}

/* The cross-core wake interrupt requests a scheduling decision, not elapsed
 * CPU time. Accounting remains solely in the periodic local timer path. */
regs_t *sched_reschedule(regs_t *frame) {
    if (!scheduler_local_online()) return frame;
    bool irq = queue_lock();
    if (current == idle) switch_to(pick_next());
    else queue_unlock(false);
    irq_restore(irq);
    return frame;
}

/* Called from the timer interrupt.  Switching here is safe because the
 * interrupt frame lives on this thread's own kernel stack. */
regs_t *sched_tick(regs_t *r) {
    if (!scheduler_local_online()) return r;
    bool irq = queue_lock();
    u32 cpu = cpu_current_slot();

    if (current) current->cpu_ms++;

    /* And the same tick counted once more, against the machine rather than
     * against a process.  Process time alone cannot answer "how busy is this
     * computer": the idle thread is a process too, and it accumulates time
     * exactly like the others, so a total over all processes always comes to
     * one hundred per cent no matter what the machine is doing.  Splitting the
     * idle thread out is the whole measurement. */
    if (cpu == 0) {
        if (current == idle) g_cpu_idle_ms++;
        else                 g_cpu_busy_ms++;
    } else if (current == idle) {
        __atomic_fetch_add(&scheduler_local()->idle_ms, 1ull, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&scheduler_local()->busy_ms, 1ull, __ATOMIC_RELAXED);
    }

    /* Wake anything whose sleep has expired. */
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_SLEEPING && g_uptime_ms >= procs[i].wake_at_ms)
            procs[i].state = PROC_READY;

    /* Preempt every few milliseconds rather than on every tick, so short
     * syscalls are not constantly interrupted. */
    sched_cpu_t *local = scheduler_local();
    /* Idle has no useful time slice to protect. A completed input/device wait
     * or expired sleep must not wait another 9 ms just because idle was picked
     * recently. Keep the normal 10 ms slice for runnable work. */
    proc_t *device = poll_device_waiters();
    bool stop_handoff = cpu != 0 && current != idle && current->stop_requested;
    if (!device && !stop_handoff && current != idle &&
        g_uptime_ms - local->last_switch < 10) {
        queue_unlock(irq);
        return r;
    }
    local->last_switch = g_uptime_ms;

    if (cpu != 0 && current != idle) {
        /* A preempted AP user interrupt frame is a live kernel continuation.
         * The BSP must complete it before the user frame can be returned. */
        current->state = PROC_READY;
        smp_reschedule_cpu(0);
    }

    switch_to(device ? device : pick_next());
    irq_restore(irq);
    return r;
}

/* ------------------------------------------------------------------------- */
/* creation                                                                  */
/* ------------------------------------------------------------------------- */

int kthread_create(const char *name, void (*fn)(void *), void *arg) {
    bool irq = irq_save();
    proc_t *p = proc_alloc(name);
    if (!p) { irq_restore(irq); return -E_NOMEM; }

    p->is_kernel = true;
    p->pml4 = 0;                     /* runs in the kernel address space */

    regs_t frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = (u64)fn;
    frame.cs = SEL_KCODE;
    frame.ss = SEL_KDATA;
    frame.rflags = 0x202;            /* IF set */
    frame.rdi = (u64)arg;

    if (!prepare_stack(p, &frame)) { discard_setup(p); irq_restore(irq); return -E_NOMEM; }

    /* A kernel thread runs on the top of its own kernel stack once iretq has
     * consumed the frame that sits there now. */
    p->frame->rsp = p->kstack_top;

    int pid = p->pid;
    publish_ready(p);
    irq_restore(irq);
    kinfo("proc", "kernel thread %s started as pid %d", name, pid);
    return pid;
}

/* ------------------------------------------------------------------------- */
/* user processes                                                            */
/* ------------------------------------------------------------------------- */

/* Copy argv into the new address space, laid out the way the C runtime
 * expects: the strings, then a NULL-terminated pointer array. */
static bool push_args(proc_t *p, const char *const argv[], int argc, u64 *rsp_out, u64 *argv_out) {
    u64 sp = USER_STACK_TOP;
    u64 ptrs[PROC_ARGS_MAX + 1];
    if (argc > PROC_ARGS_MAX) argc = PROC_ARGS_MAX;

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        sp &= ~7ULL;

        /* The string may straddle a page, so copy through the direct map one
         * page at a time. */
        size_t done = 0;
        while (done < len) {
            u64 page_phys = vmm_translate(p->pml4, sp + done);
            if (!page_phys) return false;
            size_t chunk = PAGE_SIZE - ((sp + done) & PAGE_MASK);
            if (chunk > len - done) chunk = len - done;
            memcpy(phys_to_virt(page_phys), argv[i] + done, chunk);
            done += chunk;
        }
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;

    sp -= (u64)(argc + 1) * 8;
    sp &= ~15ULL;
    for (int i = 0; i <= argc; i++) {
        u64 slot = sp + (u64)i * 8;
        u64 phys = vmm_translate(p->pml4, slot);
        if (!phys) return false;
        *(u64 *)phys_to_virt(phys) = ptrs[i];
    }

    *argv_out = sp;
    /* The SysV ABI wants RSP 16-byte aligned at the entry point. */
    sp &= ~15ULL;
    *rsp_out = sp;
    return true;
}

static bool map_user_stack(proc_t *p) {
    u64 low = USER_STACK_TOP - USER_STACK_SIZE;
    for (u64 va = low; va < USER_STACK_TOP; va += PAGE_SIZE) {
        u64 phys = pmm_alloc_zeroed();
        if (!phys) return false;
        if (!vmm_map(p->pml4, va, phys, PTE_W | PTE_U | PTE_NX)) return false;
    }
    p->stack_low = low;
    return true;
}

int proc_spawn(const char *path, const char *const argv[], int argc, int *out_pid) {
    /* Read the image before touching the process table, so a bad path costs
     * nothing. */
    file_t *f = NULL;
    int r = vfs_open(path, O_RDONLY, &f);
    if (r < 0) return r;

    vstat_t st;
    if (vfs_stat(path, &st) < 0 || st.type != VN_FILE) { vfs_close(f); return -E_NOENT; }
    if (st.size == 0 || st.size > (32u << 20)) { vfs_close(f); return -E_INVAL; }

    void *image = kmalloc((size_t)st.size);
    if (!image) { vfs_close(f); return -E_NOMEM; }
    ssize_t_k got = vfs_read(f, image, (size_t)st.size);
    vfs_close(f);
    if (got < 0 || (u64)got != st.size) { kfree(image); return -E_IO; }

    bool irq = irq_save();
    const char *leaf = strrchr(path, '/');
    proc_t *p = proc_alloc(leaf ? leaf + 1 : path);
    if (!p) { irq_restore(irq); kfree(image); return -E_NOMEM; }
    irq_restore(irq);

    p->pml4 = vmm_new_address_space();
    if (!p->pml4) { discard_setup(p); kfree(image); return -E_NOMEM; }

    u64 entry = 0;
    r = elf_load(p, image, (size_t)st.size, &entry);
    kfree(image);
    if (r < 0) { proc_release(p); return r; }

    if (!map_user_stack(p)) { proc_release(p); return -E_NOMEM; }

    u64 rsp = 0, argv_ptr = 0;
    if (!push_args(p, argv, argc, &rsp, &argv_ptr)) { proc_release(p); return -E_NOMEM; }

    p->heap_base = p->heap_end = PAGE_ALIGN_UP(p->image_end + PAGE_SIZE);
    p->mmap_next = USER_MMAP_BASE;
    if (current) strlcpy(p->cwd, current->cwd, sizeof p->cwd);

    regs_t frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = entry;
    frame.cs = SEL_UCODE3;
    frame.ss = SEL_UDATA3;
    frame.rflags = 0x202;
    frame.rsp = rsp;
    frame.rdi = (u64)(argc > PROC_ARGS_MAX ? PROC_ARGS_MAX : argc);
    frame.rsi = argv_ptr;

    if (!prepare_stack(p, &frame)) { proc_release(p); return -E_NOMEM; }

    /* Inherit the parent's standard descriptors so output goes to the console. */
    if (current && !proc_fd_inherit_stdio(p, current)) { proc_release(p); return -E_MFILE; }

    /* A fresh FPU state, so the child does not inherit stale registers. */
    memset(p->fpu, 0, sizeof p->fpu);
    *(u16 *)(p->fpu + 0) = 0x037F;      /* FCW */
    *(u32 *)(p->fpu + 24) = 0x1F80;     /* MXCSR */

    /* Only now is the process complete enough to run.  Until this point the
     * slot is claimed but unschedulable, which matters because everything
     * above happens with interrupts enabled and the timer can preempt us. */
    publish_ready(p);

    if (out_pid) *out_pid = p->pid;
    kinfo("proc", "started %s as pid %d (entry %p)", p->name, p->pid, (void *)entry);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* threads                                                                   */
/* ------------------------------------------------------------------------- */

/* Starting a thread the way Linux starts one.
 *
 * The two systems disagree about what a new thread is.  Here, a thread is an
 * entry point and an argument, and the kernel finds it a stack.  Linux's clone
 * hands back control at the same instruction in both threads - the child
 * returns from the call that made it, with zero in RAX - and runs on a stack
 * the program allocated itself, usually inside the block it also put the
 * thread's local storage in.
 *
 * So this cannot go through proc_thread_create: it has to copy the caller's
 * whole register frame rather than build a fresh one, and it must not allocate
 * a stack, because the program already did and is about to free that same one
 * when the thread ends.
 */
/* Setup pins its creator before borrowing any leader-owned storage. Group
 * drain/kill sees that pin even while the child is still PROC_SETUP and while
 * allocations happen without the runqueue lock. Closing groups reject both
 * new setup and late publication. */
static int thread_setup_begin(proc_t **child, proc_t **creator, bool native) {
    bool irq = queue_lock();
    proc_t *me = current, *lead = proc_shared(me);
    if (!me || me->is_kernel || !me->pml4) {
        queue_unlock(irq);
        return -E_PERM;
    }
    if (me->resource_closing || lead->resource_closing ||
        me->state != PROC_RUNNING || lead->state == PROC_DYING ||
        lead->state == PROC_ZOMBIE || lead->state == PROC_UNUSED ||
        me->resource_pins == 0xffffffffu) {
        queue_unlock(irq);
        return -E_BUSY;
    }
    me->resource_pins++;
    queue_unlock(irq);

    proc_t *t = proc_alloc(lead->name);
    if (!t) { proc_resource_unpin(me); return -E_NOMEM; }
    irq = queue_lock();
    u64 low = lead->tstack_next ? lead->tstack_next : USER_TSTACK_BASE;
    const u64 stack_limit = USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE;
    int error = (me->resource_closing || lead->resource_closing) ? -E_BUSY : 0;
    if (!error && native &&
        (low < USER_TSTACK_BASE || low > stack_limit - USER_TSTACK_SLOT ||
         ((low - USER_TSTACK_BASE) % USER_TSTACK_SLOT))) error = -E_NOMEM;
    if (error) {
        t->state = PROC_UNUSED;
        me->resource_pins--;
        queue_unlock(irq);
        return error;
    }
    t->leader = lead;
    t->pml4 = lead->pml4;
    t->parent_pid = lead->parent_pid;
    if (native) {
        t->stack_low = low;
        lead->tstack_next = low + USER_TSTACK_SLOT;
    }
    queue_unlock(irq);
    *child = t;
    *creator = me;
    return 0;
}

regs_t *sched_ap_tick(regs_t *frame) {
    bool user = (frame->cs & 3u) == 3u;
    if (user) {
        bool irq = queue_lock();
        if (!scheduler_local_online() || !current || current->is_kernel ||
            current->kernel_active)
            panic("scheduler: invalid AP user timer entry");
        current->kernel_active = true;
        queue_unlock(irq);
    }
    regs_t *result = sched_tick(frame);
    if (user) proc_kernel_leave();
    return result;
}

static bool thread_setup_publish(proc_t *t, proc_t *creator) {
    bool irq = queue_lock();
    if (t->state != PROC_SETUP || t->running_cpu != PROC_CPU_NONE ||
        !creator->resource_pins) panic("scheduler: invalid thread setup publication");
    bool ok = !creator->resource_closing && !t->leader->resource_closing;
    u32 target = PROC_CPU_NONE;
    if (ok) {
        /* Must precede READY: the child can immediately exit and clear the
         * same word. Linux likewise ignores a failed parent-TID store. */
        u32 tid = (u32)t->pid;
        if (t->parent_tid)
            (void)vmm_user_copy(creator->pml4, &tid, t->parent_tid, sizeof tid, true);
        t->parent_tid = 0;
        t->state = PROC_READY;
        target = idle_wakeup_target();
        creator->resource_pins--;
    }
    queue_unlock(irq);
    wake_idle_cpu(target);
    return ok;
}

static void thread_setup_abort(proc_t *t, proc_t *creator, bool stack_mapped) {
    /* Creator remains pinned through VM retirement and kernel-stack free.
     * No allocator or VM operation runs while holding the runqueue lock. */
    if (stack_mapped) vmm_unmap_user_range(t->pml4, t->stack_low, USER_TSTACK_SIZE);
    if (t->kstack) pmm_free_pages(virt_to_phys((void *)t->kstack), KSTACK_SIZE / PAGE_SIZE);
    bool irq = queue_lock();
    if (t->state != PROC_SETUP || t->running_cpu != PROC_CPU_NONE ||
        !creator->resource_pins) panic("scheduler: invalid thread setup rollback");
    /* Reclaim only the newest reservation. Never rewind across another
     * creator's slot. Older failed reservations remain harmless guard holes. */
    if (t->stack_low && t->leader->tstack_next == t->stack_low + USER_TSTACK_SLOT)
        t->leader->tstack_next = t->stack_low;
    t->leader = NULL;
    t->pml4 = t->kstack = t->stack_low = 0;
    t->state = PROC_UNUSED;
    creator->resource_pins--;
    queue_unlock(irq);
}

int proc_thread_clone(const regs_t *caller, const proc_clone_options_t *options, int *out_tid) {
    if (!caller || !options || options->stack < PAGE_SIZE || options->stack >= (1ull << 47) ||
        (options->set_tls && options->tls >= (1ull << 47))) return -E_INVAL;
    proc_t *me, *t;
    int r = thread_setup_begin(&t, &me, false);
    if (r < 0) return r;

    /* The caller's frame, with two things changed: where the stack is, and
     * what the call appears to have returned. */
    regs_t frame = *caller;
    frame.rsp = options->stack;
    frame.rax = 0;

    t->stack_low = 0;          /* the program owns this stack, not the kernel */
    t->gsbase = me->gsbase;
    t->fsbase = options->set_tls ? options->tls : me->fsbase;
    t->linux_abi = me->linux_abi;
    t->parent_tid = options->set_parent_tid ? options->parent_tid : 0;
    t->set_child_tid = options->set_child_tid ? options->child_tid : 0;
    t->clear_child_tid = options->clear_child_tid ? options->child_tid : 0;

    if (!prepare_stack(t, &frame)) {
        thread_setup_abort(t, me, false);
        return -E_NOMEM;
    }

    /* clone inherits the caller's floating-point environment, not defaults. */
    fpu_save(t->fpu);

    int tid = t->pid;
    if (!thread_setup_publish(t, me)) {
        thread_setup_abort(t, me, false);
        return -E_BUSY;
    }
    if (out_tid) *out_tid = tid;
    return 0;
}

int proc_thread_set_base(bool fs, u64 base) {
    if (base >= (1ull << 47)) return -E_PERM;
    bool irq = irq_save();
    proc_t *p = current;
    if (!scheduler_local_online() || !p || p->is_kernel) {
        irq_restore(irq);
        return -E_INVAL;
    }
    /* C runs with user GS active: the syscall entry's second SWAPGS has
     * already restored it. Never overwrite this CPU's KERNEL_GS_BASE. */
    wrmsr(fs ? MSR_FS_BASE : MSR_GS_BASE, base);
    if (fs) p->fsbase = base;
    else p->gsbase = base;
    irq_restore(irq);
    return 0;
}

int proc_thread_create(u64 entry, u64 arg, int *out_tid) {
    proc_t *me, *t;
    int r = thread_setup_begin(&t, &me, true);
    if (r < 0) return r;
    u64 low = t->stack_low;

    regs_t frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = entry;
    frame.cs = SEL_UCODE3;
    frame.ss = SEL_UDATA3;
    frame.rflags = 0x202;
    /* A function is entered just after a call pushed a return address, so it
     * expects the stack eight bytes off a sixteen-byte boundary.  Handing it
     * anything else misaligns every aligned spill in its prologue. */
    frame.rsp = ((low + USER_TSTACK_SIZE) & ~15ULL) - 8;
    frame.rdi = arg;
    t->gsbase = me->gsbase;
    t->fsbase = me->fsbase;
    t->linux_abi = me->linux_abi;

    if (!prepare_stack(t, &frame)) {
        thread_setup_abort(t, me, false);
        return -E_NOMEM;
    }

    /* Allocate last: a failed kernel-stack allocation never leaves user pages.
     * The VM helper rejects collisions and rolls back every installed leaf on
     * OOM while mutations remain serialized. */
    if (!vmm_alloc_user_stack(t->pml4, low, USER_TSTACK_SIZE)) {
        thread_setup_abort(t, me, false);
        return -E_NOMEM;
    }

    memset(t->fpu, 0, sizeof t->fpu);
    *(u16 *)(t->fpu + 0) = 0x037F;
    *(u32 *)(t->fpu + 24) = 0x1F80;

    int tid = t->pid;
    if (!thread_setup_publish(t, me)) {
        thread_setup_abort(t, me, true);
        return -E_BUSY;
    }
    if (out_tid) *out_tid = tid;
    return 0;
}

void proc_thread_exit(int code) {
    proc_t *t = current;
    if (!t) panic("proc_thread_exit with no current thread");

    /* The leader leaving means the program is over, threads and all. */
    if (!t->leader) proc_exit(code);

    bool boundary_irq = queue_lock();
    t->kernel_active = false;
    queue_unlock(boundary_irq);

    proc_resource_drain(t);

    bool irq = queue_lock();
    thread_clear_tid_locked(t);
    t->exit_code = code;
    t->state = PROC_ZOMBIE;
    t->reapable = true;             /* the next thread to run frees the stack */
    switch_to(pick_next());
    irq_restore(irq);
    panic("a dead thread was scheduled again (pid %d)", t->pid);
}

extern int fb_owner_pid;

/* All helpers below serialize with selection and saved-context handoff.
 * A user thread parked between entries has no live syscall stack to unwind.
 * An owned context, constructor or kernel entry must reach its own boundary. */
static void retire_stopped_threads(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &procs[i];
        if (!t->leader || t->is_kernel || !t->stop_requested || t->kernel_active || t->clear_child_tid ||
            t->resource_pins || t->running_cpu != PROC_CPU_NONE ||
            t->state == PROC_UNUSED || t->state == PROC_SETUP ||
            t->state == PROC_ZOMBIE || t->state == PROC_DYING) continue;
        t->state = PROC_ZOMBIE;
        t->exit_code = t->stop_code;
        t->reapable = true;
        t->pml4 = 0;
    }
}

static void request_group_stop(proc_t *lead, int code) {
    if (!lead->group_stop) { lead->group_stop = true; lead->stop_code = code; }
    lead->resource_closing = true; /* closes construction and new device pins */
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &procs[i];
        if (proc_shared(t) != lead || t->state == PROC_UNUSED ||
            t->state == PROC_ZOMBIE) continue;
        t->stop_code = lead->stop_code;
        __atomic_store_n(&t->stop_requested, true, __ATOMIC_RELEASE);
        /* Wake into the existing C cleanup path, never jump over its locks or
         * waiter deregistration. Device waits recheck their retirement fence. */
        if (t->state == PROC_BLOCKED || t->state == PROC_SLEEPING) t->state = PROC_READY;
    }
}

bool proc_stop_requested(void) {
    proc_t *p = proc_current();
    return p && __atomic_load_n(&p->stop_requested, __ATOMIC_ACQUIRE);
}

void proc_kernel_enter(void) {
    if (!scheduler_local_online() || !current || current->is_kernel) return;
    bool irq = queue_lock();
    if (current->kernel_active) panic("scheduler: nested user kernel entry");
    current->kernel_active = true;
    if (cpu_current_slot() != 0) {
        u32 origin = cpu_current_slot();
        /* The AP may execute user instructions, but legacy syscall and fault
         * handlers (including their driver callbacks) belong on the BSP.
         * Park this live kernel stack before making it selectable there. */
        if (!idle || idle->running_cpu != PROC_CPU_NONE || !idle->is_idle ||
            current->running_cpu != origin)
            panic("scheduler: AP user entry has no owned idle fallback");
        current->state = PROC_READY;
        smp_reschedule_cpu(0);
        switch_to(pick_next());
        irq_restore(irq);
        if (cpu_current_slot() != 0)
            panic("scheduler: AP kernel continuation did not migrate to BSP");
        return;
    }
    queue_unlock(irq);
}

void proc_kernel_leave(void) {
    if (!scheduler_local_online() || !current || current->is_kernel) return;
    bool irq = queue_lock();
    proc_t *p = current;
    if (!p->kernel_active || p->resource_pins)
        panic("scheduler: kernel return with unbalanced entry or resource pins");
    bool stop = p->stop_requested;
    int code = p->stop_code;
    if (stop && cpu_current_slot() != 0) {
        /* Stop cleanup closes descriptors and address spaces on the BSP.
         * Keep the live entry pinned while moving its stack there. */
        p->state = PROC_READY;
        smp_reschedule_cpu(0);
        switch_to(pick_next());
        irq_restore(irq);
        if (cpu_current_slot() != 0)
            panic("scheduler: stopped AP thread did not migrate to BSP");
        proc_kernel_leave();
        return;
    }
    p->kernel_active = false;
    if (!stop && cpu_current_slot() == 0) {
        /* The BSP has finished all legacy kernel and device work, including
         * the interrupt's local EOI. Release this return-to-user continuation
         * so an idle application AP can execute the same thread in ring 3.
         * Ownership remains on the BSP until context_switch saves its stack. */
        u32 target = idle_wakeup_target();
        if (target != PROC_CPU_NONE) {
            p->state = PROC_READY;
            smp_reschedule_cpu(target);
            switch_to(pick_next());
            irq_restore(irq);
            return;
        }
    }
    queue_unlock(irq);
    if (stop) proc_thread_exit(code);
}

/* Called once by thread_bootstrap after the saved-context handoff releases
 * runqueue ownership, with this thread's CR3 and kernel stack already loaded. */
void proc_thread_first_entry(void) {
    proc_t *p = current;
    if (!p || p->is_kernel) return;
    if (cpu_current_slot() != 0) {
        /* First entry needs only the protected child-TID store. It has no
         * legacy driver work and can finish before the AP's first user IRET.
         * A concurrent stop is handed to the BSP through the entry boundary. */
        u64 address = p->set_child_tid;
        p->set_child_tid = 0;
        if (address) {
            u32 tid = (u32)p->pid;
            (void)vmm_user_copy(p->pml4, &tid, address, sizeof tid, true);
        }
        if (p->stop_requested) {
            proc_kernel_enter();
            proc_kernel_leave();
        }
        return;
    }
    proc_kernel_enter();
    u64 address = p->set_child_tid;
    p->set_child_tid = 0;
    if (address) {
        u32 tid = (u32)p->pid;
        (void)vmm_user_copy(p->pml4, &tid, address, sizeof tid, true);
    }
    /* A child stopped during setup must exit instead of executing user code. */
    proc_kernel_leave();
}

/* Everything belonging to the process ends when the leader finishes draining.
 * All siblings have either unwound or been parked outside a kernel entry. */
static bool stop_sibling_threads(proc_t *lead) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &procs[i];
        if (t != lead && t->leader == lead && t->state != PROC_UNUSED &&
            (t->running_cpu != PROC_CPU_NONE || t->state == PROC_SETUP ||
              t->kernel_active || t->resource_pins || t->clear_child_tid)) return false;
    }
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &procs[i];
        if (t == lead || t->leader != lead) continue;
        if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE) continue;
        t->state = PROC_ZOMBIE;
        t->reapable = true;
        t->pml4 = 0;                /* the leader owns it and is about to free it */
    }
    return true;
}

void proc_exit(int code) {
    proc_t *p = current;
    if (!p) panic("proc_exit with no current process");

    bool request_irq = queue_lock();
    proc_t *lead = proc_shared(p);
    request_group_stop(lead, code);
    code = lead->stop_code;
    p->kernel_active = false;
    queue_unlock(request_irq);
    /* A sibling requests whole-process exit, but the leader owns descriptors,
     * mappings and parent wait status. It completes cleanup exactly once. */
    if (p != lead) proc_thread_exit(code);

    proc_resource_drain(p);

    kinfo("proc", "pid %d (%s) exited with status %d", p->pid, p->name, code);
    bool stop_irq = queue_lock();
    if (!stop_sibling_threads(p)) panic("scheduler: group exit still has a live CPU context");
    thread_clear_tid_locked(p);
    queue_unlock(stop_irq);

    /* If this process held the screen, the console takes it back and repaints,
     * rather than leaving the last frame of a dead program on the display. */
    if (fb_owner_pid == p->pid) {
        fb_owner_pid = 0;
        console_take_framebuffer();
    }

    bool irq = irq_save();
    p->exit_code = code;

    /* Free the heavy resources now; the entry lingers so a parent can collect
     * the exit status. */
    proc_fd_close_all(p);
    if (p->pml4 && !p->leader) {
        /* We may still be running on this address space; move to the kernel's
         * before tearing it down. */
        write_cr3(vmm_kernel_pml4());
        vmm_destroy_address_space(p->pml4);
    }
    p->pml4 = 0;

    /* Wake a parent that is waiting on us. */
    queue_lock();
    p->state = PROC_ZOMBIE;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *q = &procs[i];
        if (q->state != PROC_BLOCKED) continue;
        if (q->pid != p->parent_pid) continue;
        if (q->waiting_for == p->pid || q->waiting_for == -1) q->state = PROC_READY;
    }

    switch_to(pick_next());
    irq_restore(irq);
    panic("a zombie process was scheduled again (pid %d)", p->pid);
}

int proc_kill(int pid, int code) {
    bool irq = queue_lock();
    proc_t *p = proc_by_pid(pid);
    if (!p || p->state == PROC_ZOMBIE) { queue_unlock(irq); return -E_NOENT; }
    proc_t *lead = proc_shared(p);
    if (lead->is_kernel) { queue_unlock(irq); return -E_PERM; }
    if (lead->state == PROC_SETUP || lead->state == PROC_DYING) {
        queue_unlock(irq);
        return -E_BUSY;
    }
    request_group_stop(lead, code);
    bool self = current && proc_shared(current) == lead;
    queue_unlock(irq);
    if (self) proc_exit(code);
    /* Accepted asynchronously. The target leader, not this caller, drains
     * live kernel entries and destroys its CR3 before publishing wait status. */
    return 0;
}

int proc_wait(int pid, int *status) {
    if (!current) return -E_INVAL;

    for (;;) {
        bool irq = queue_lock();
        bool found = false;
        for (int i = 0; i < PROC_MAX; i++) {
            proc_t *p = &procs[i];
            if (p->state == PROC_UNUSED || p->state == PROC_SETUP) continue;
            if (p->leader) continue;                /* a thread, not a child */
            if (p->parent_pid != current->pid) continue;
            if (pid != -1 && p->pid != pid) continue;
            found = true;

            if (p->state == PROC_ZOMBIE && p->running_cpu == PROC_CPU_NONE) {
                int child = p->pid;
                if (status) *status = p->exit_code;
                if (p->kstack) {
                    pmm_free_pages(virt_to_phys((void *)p->kstack), KSTACK_SIZE / PAGE_SIZE);
                    p->kstack = 0;
                }
                p->state = PROC_UNUSED;
                queue_unlock(irq);
                return child;
            }
        }
        if (!found) { queue_unlock(irq); return -E_NOENT; }
        if (current->stop_requested) { queue_unlock(irq); return -E_BUSY; }

        current->waiting_for = pid;
        current->state = PROC_BLOCKED;
        switch_to(pick_next());
        queue_lock();
        current->waiting_for = -1;
        queue_unlock(irq);
    }
}

/* ------------------------------------------------------------------------- */
/* bring-up                                                                  */
/* ------------------------------------------------------------------------- */

static void scheduler_idle_thread(void *unused) {
    (void)unused;
    /* No driver calls, locks, logging or boot work belong in the fallback
     * context. A runnable lock waiter must never starve its owner here. */
    for (;;) __asm__ volatile("sti; hlt" ::: "memory");
}

static void application_idle_entry(void) {
    u32 slot = cpu_current_slot();
    if (!slot || slot >= ACPI_MAX_CPUS) panic("scheduler: invalid AP idle entry");
    sched_cpu_t *local = &sched_cpus[slot];
    proc_t *p = &application_idle[slot];
    bool irq = queue_lock();
    if (local->running || local->idle_task != p || p->state != PROC_SETUP ||
        p->running_cpu != PROC_CPU_NONE || !p->kstack)
        panic("scheduler: AP idle context was not exclusively prepared");
    p->state = PROC_RUNNING;
    p->running_cpu = slot;
    local->running = p;
    local->last_switch = g_uptime_ms;
    __atomic_store_n(&local->application_online, 1u, __ATOMIC_RELEASE);
    queue_unlock(irq);
    scheduler_idle_thread(NULL);
}

void sched_join_application_cpu(void) {
    cli();
    u32 slot = cpu_current_slot();
    if (!sched_application_cpu_prepared(slot))
        panic("scheduler: AP admission without a private idle context");
    proc_t *p = &application_idle[slot];
    write_cr3(vmm_kernel_pml4());
    tss_set_rsp0(p->kstack_top);
    switch_stack_and_call(p->kstack_top, application_idle_entry);
}

/* Reserve a private fallback stack while the AP is still a restricted worker.
 * It is never published READY or counted as application capacity here. The
 * stack and descriptor have boot lifetime; a later AP admission path must
 * leave its worker loop before claiming this context as RUNNING. */
bool sched_prepare_application_cpu(u32 slot) {
    if (!slot || slot >= ACPI_MAX_CPUS || cpu_current_slot() != 0 ||
        !__atomic_load_n(&sched_running, __ATOMIC_ACQUIRE) ||
        !smp_worker_slot_online(slot)) return false;
    if (sched_application_cpu_prepared(slot)) return true;
    if (__atomic_load_n(&sched_cpus[slot].idle_task, __ATOMIC_ACQUIRE)) return false;

    u64 phys = pmm_alloc_pages(KSTACK_SIZE / PAGE_SIZE);
    if (!phys) return false;
    u64 stack = (u64)phys_to_virt(phys);
    memset((void *)stack, 0, KSTACK_SIZE);

    bool irq = queue_lock();
    sched_cpu_t *local = &sched_cpus[slot];
    if (local->idle_task || local->application_online) {
        queue_unlock(irq);
        pmm_free_pages(phys, KSTACK_SIZE / PAGE_SIZE);
        return false;
    }
    proc_t *p = &application_idle[slot];
    memset(p, 0, sizeof *p);
    p->state = PROC_SETUP; /* reserved, never runnable while the AP is a worker */
    p->running_cpu = PROC_CPU_NONE;
    p->is_kernel = p->is_idle = true;
    p->kstack = stack;
    p->kstack_top = stack + KSTACK_SIZE;
    strlcpy(p->name, "ap-idle", sizeof p->name);
    __atomic_store_n(&local->idle_task, p, __ATOMIC_RELEASE);
    queue_unlock(irq);
    return true;
}

void proc_init(void) {
    /* The BSP creates the shared run queue and idle contexts. APs join it
     * separately after their restricted-worker batch boundary. */
    if (cpu_current_slot() != 0 || __atomic_load_n(&sched_running, __ATOMIC_ACQUIRE))
        panic("scheduler: invalid bootstrap enrollment");
    bool irq = irq_save();
    memset(procs, 0, sizeof procs);

    /* Boot still performs synchronous driver initialization and may own the
     * NVKMS core lock across a yield. It is ordinary runnable work, NOT idle.
     * Otherwise a worker yielding for that lock permanently starves boot. */
    proc_t *boot = &procs[0];
    boot->pid = 0;
    boot->state = PROC_RUNNING;
    boot->running_cpu = cpu_current_slot();
    boot->is_kernel = true;
    strlcpy(boot->name, "boot", sizeof boot->name);
    strlcpy(boot->cwd, "/", sizeof boot->cwd);
    boot->waiting_for = -1;

    /* It is already running on the boot stack the kernel moved onto during
     * start-up, which lives in the direct map and so exists in every address
     * space.  That matters: the scheduler changes CR3 while still standing on
     * the outgoing thread's stack. */
    extern u64 g_boot_stack;
    boot->kstack = g_boot_stack;
    boot->kstack_top = g_boot_stack + BOOT_STACK_BYTES;
    current = boot;

    /* Create the fallback before enabling scheduling. Its saved context must
     * be separate even when boot blocks and no other thread is ready. */
    idle = proc_alloc("idle");
    if (!idle) panic("scheduler: cannot allocate idle context");
    idle->is_kernel = true;
    idle->is_idle = true;
    regs_t frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = (u64)scheduler_idle_thread;
    frame.cs = SEL_KCODE;
    frame.ss = SEL_KDATA;
    frame.rflags = 0x202;
    if (!prepare_stack(idle, &frame)) panic("scheduler: cannot allocate idle stack");
    idle->frame->rsp = idle->kstack_top;
    publish_ready(idle);
    tss_set_rsp0(boot->kstack_top);

    __atomic_store_n(&sched_running, true, __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_local()->application_online, 1u, __ATOMIC_RELEASE);
    irq_restore(irq);
    u32 prepared = 0, refused = 0;
    for (u32 slot = 1; slot < ACPI_MAX_CPUS; slot++) {
        if (!smp_worker_slot_online(slot)) continue;
        if (sched_prepare_application_cpu(slot)) prepared++;
        else refused++;
    }
    if (refused) kwarn("proc", "%u AP application idle contexts could not be prepared", refused);
    kinfo("proc", "%u AP idle stacks prepared for application admission", prepared);
    u32 requested = 0;
    while (smp_promote_one_application_cpu()) requested++;
    if (requested) {
        for (u32 wait = 0; wait < 100 && sched_application_cpu_count() < requested + 1; wait++)
            timer_mdelay(1);
        kinfo("proc", "%u application scheduler CPUs online, %d restricted workers retained",
              sched_application_cpu_count(), smp_worker_count());
    }
    kinfo("proc", "scheduler ready, %d process slots; boot task and idle stack are separate", PROC_MAX);
}
