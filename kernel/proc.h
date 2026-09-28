#ifndef KESTREL_PROC_H
#define KESTREL_PROC_H

#include "kernel.h"
#include "cpu.h"
#include "vfs.h"

#define PROC_MAX_FDS   32
#define PROC_NAME_MAX  32
#define PROC_MAX       64
#define PROC_ARGS_MAX  16
#define PROC_CPU_NONE  0xffffffffu

/* User address space layout. */
#define USER_IMAGE_BASE   0x0000000000400000ULL
#define USER_HEAP_BASE    0x0000000010000000ULL
#define USER_HEAP_MAX     0x0000000040000000ULL
#define USER_STACK_TOP    0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE   (256 * 1024)
#define USER_MMAP_BASE    0x0000000060000000ULL

/* Threads of one process share its address space, so each needs a stack of its
 * own somewhere the main stack will never reach.  They are laid out in slots
 * well below it, a fraction of each slot mapped so that running off the end of
 * one lands in unmapped memory rather than in the next thread's. */
#define USER_TSTACK_BASE  0x00007FF000000000ULL
#define USER_TSTACK_SLOT  (1024 * 1024)
#define USER_TSTACK_SIZE  (256 * 1024)

typedef enum {
    PROC_UNUSED = 0,
    /* Claimed but not yet runnable: the address space, image and kernel stack
     * are still being built.  The scheduler must never pick one of these, so
     * the slot is reserved without being schedulable. */
    PROC_SETUP,
    PROC_READY,
    PROC_RUNNING,
    PROC_SLEEPING,      /* on a timer */
    PROC_BLOCKED,       /* on input or a child */
    PROC_ZOMBIE,
    PROC_DYING,         /* reserved during cleanup, not runnable or reapable */
} proc_state_t;

typedef struct proc {
    int          pid;
    int          parent_pid;
    char         name[PROC_NAME_MAX];
    proc_state_t state;
    /* Runqueue-locked ownership of the saved/live kernel context. It remains
     * owned until context_switch has saved the outgoing stack on another
     * stack. PROC_READY alone is never permission to run or reap a context. */
    u32          running_cpu;

    u64          pml4;            /* address space, 0 for kernel threads */
    u64          kstack;          /* base of the kernel stack allocation */
    u64          kstack_top;      /* first byte past it; what TSS.rsp0 gets   */
    regs_t      *frame;           /* saved register frame while not running */
    u8           fpu[512] __attribute__((aligned(16)));

    /* Address space bookkeeping. */
    u64 image_base, image_end;
    u64 heap_base, heap_end;
    u64 stack_low;
    u64 mmap_next;

    file_t      *fds[PROC_MAX_FDS];
    char         cwd[VFS_PATH_MAX];

    u64          wake_at_ms;      /* for PROC_SLEEPING */
    /* A boot-lifetime kernel event counter, registered only while waiting.
     * Accessed under runqueue ownership; never points into user memory. */
    u64         *event_sequence;
    /* Futex registration is scheduler-owned, never an external raw proc_t
     * reference. A separate live flag permits physical address/key zero. */
    u64          futex_key;
    u64          futex_space;     /* 0: physical/shared; pml4: private VA key */
    u32          futex_mask;
    bool         futex_waiting, futex_woken;
    int          exit_code;
    int          waiting_for;     /* pid a parent is blocked on, -1 for any */
    bool         is_kernel;
    bool         is_idle;

    u64          cpu_ms;          /* accumulated run time */
    u64          started_ms;

    /* Threads.  A thread is an ordinary entry in this table that shares the
     * leader's address space, descriptors, working directory and heap; only
     * the registers, the stacks and the FPU state are its own.  `leader` is
     * NULL for a process, and points at the process for each of its threads,
     * which is the single test everything else keys off. */
    struct proc *leader;
    u64          tstack_next;     /* leader only: where the next stack goes */
    bool         reapable;        /* a dead thread nobody will wait for */

    /* Kernel resource transactions may sleep, but must not lose their stack
     * or address space to an asynchronous kill while they own device state. */
    u32          resource_pins;
    bool         resource_closing;
    /* Address-space allocation metadata is shared by siblings. Ownership and
     * wait registration are runqueue protected; both owners and waiters keep
     * resource pins so exit cannot recycle their process/group/CR3. */
    struct proc *vm_owner;         /* leader only */
    bool         vm_waiting;
    /* Runqueue-owned user/kernel boundary and asynchronous group-stop state.
     * kernel_active spans an entire syscall or user-origin interrupt, including
     * any time it sleeps. Such a stack must unwind, not be discarded by kill. */
    bool         kernel_active;
    bool         stop_requested;
    bool         group_stop;       /* leader only, first requested code wins */
    int          stop_code;
    u64          gpu_owner_id; /* monotonic address-space resource identity */

    /* The GS base, which the kernel itself never uses, so a thread can point
     * it wherever its runtime wants - which is how a Windows program finds its
     * thread environment block. */
    u64          gsbase;

    /* The other one.  A Linux program keeps its thread-local storage through
     * FS, not GS, and sets it with arch_prctl before it does anything else -
     * so a kernel that does not carry FS across a context switch loses the
     * program's own variables the first time it is descheduled. */
    u64          fsbase;

    /* Whose system call numbers this program is speaking.  Both kinds of
     * program are static x86-64 ELF and enter the kernel the same way; they
     * disagree only about what the number in RAX means. */
    bool         linux_abi;
    /* Linux thread lifecycle addresses, not retained user-memory pointers.
     * parent_tid is setup-only; set_child_tid is consumed at first entry;
     * clear_child_tid is consumed before exit releases the address space. */
    u64          parent_tid, set_child_tid, clear_child_tid;

    /* Where a fault is delivered instead of killing the process, and how deep
     * the delivery is nested - a handler that faults again is a real
     * possibility and must not loop for ever. */
    u64          fault_handler;
    int          fault_depth;

    struct proc *next;
    /* Leader-owned framebuffer aliases, independent of which process owns
     * presentation. FB_RELEASE deliberately preserves them for resume. */
    u64 fb_mapped_va, fb_mapped_span, fb_mapped_phys;
    /* Whether this program was built for Linux rather than for this system.
     * The register convention is the same; the call numbers and what they
     * mean are not, so its system calls go through a different table. */
} proc_t;

void proc_init(void);
void proc_start_init(void);

proc_t *proc_current(void);
bool proc_resource_pin(proc_t **token);
void proc_resource_unpin(proc_t *token);
bool proc_vm_begin(proc_t **token);
void proc_vm_end(proc_t *token);
s64 syscall_brk(proc_t *p, u64 requested);
bool proc_gpu_owner_alive(u64 owner);
bool proc_pid_alive(int pid);
void proc_kernel_enter(void);
void proc_kernel_leave(void);
void proc_thread_first_entry(void);
int  proc_set_tid_address(u64 address);
bool proc_stop_requested(void);
int     proc_count(void);
int     proc_thread_count(void);
/* Reporting copies values into permanent kernel storage while owning the
 * runqueue lock. No returned pointer aliases a process-table entry. Individual
 * rows are coherent; separate indexed calls are not a frozen whole-table view. */
typedef struct {
    int pid, parent_pid;
    u32 state;
    char name[PROC_NAME_MAX];
    u64 cpu_ms, image_bytes;
} proc_report_t;
bool proc_report_snapshot(u32 index, proc_report_t *out);
void proc_count_snapshot(u32 *processes, u32 *threads);

/* Create a user process from an ELF image on disk. */
int  proc_spawn(const char *path, const char *const argv[], int argc, int *out_pid);
void proc_exit(int code) __attribute__((noreturn));
int  proc_kill(int pid, int code);
int  proc_wait(int pid, int *status);

/* Kernel threads share the kernel address space. */
int  kthread_create(const char *name, void (*fn)(void *), void *arg);

/* Threads of the calling process.  `entry` is called with `arg` in the first
 * argument register on a stack of its own; it must not return. */
int  proc_thread_create(u64 entry, u64 arg, int *out_tid);
/* The same, started the way Linux's clone starts one. */
typedef struct {
    u64 stack, tls, parent_tid, child_tid;
    bool set_tls, set_parent_tid, set_child_tid, clear_child_tid;
} proc_clone_options_t;
int  proc_thread_clone(const regs_t *caller, const proc_clone_options_t *options,
                       int *out_tid);
void proc_thread_exit(int code) __attribute__((noreturn));
/* Set the current user thread's TLS base without a preemptible MSR/bookkeeping
 * split. Only lower-half canonical addresses are supported (zero is valid). */
int proc_thread_set_base(bool fs, u64 base);

/* Everything a thread shares with the process it belongs to lives on the
 * leader: descriptors, the working directory and the heap.  Every path that
 * touches those goes through here, so a thread and its process cannot end up
 * with two of anything. */
static inline proc_t *proc_shared(proc_t *p) {
    return (p && p->leader) ? p->leader : p;
}

/* Sleeping on a word of the process's own memory, which is what every lock and
 * event in a threaded program eventually comes down to.  `wait` sleeps only if
 * the word still holds `expect`, so a wake that arrives first is not lost. */
int  futex_wait(u64 uaddr, u32 expect, int timeout_ms);
int  futex_wake(u64 uaddr, int count);
/* Internal scheduler-owned futex queue entry points. */
int  sched_futex_wait(u64 uaddr, u32 expect, int timeout_ms);
int  sched_futex_wake(u64 uaddr, int count);
/* Extended wait uses an absolute uptime-ms deadline; UINT64_MAX is infinite.
 * Private keys are address-space + VA; shared keys are physical backing words.
 * Returns Linux-style EFAULT/EINTR/ETIMEDOUT. Native wrappers retain their ABI. */
int  sched_futex_wait_ex(u64 uaddr, u32 expect, u64 deadline, u32 mask, bool private_key);
int  sched_futex_wake_ex(u64 uaddr, int count, u32 mask, bool private_key);

/* Scheduling. */
regs_t *sched_tick(regs_t *r);
u32     sched_application_cpu_count(void);
bool    sched_application_cpu_online(u32 slot);
bool    sched_application_cpu_prepared(u32 slot);
bool    sched_prepare_application_cpu(u32 slot);
void    sched_join_application_cpu(void) __attribute__((noreturn));
regs_t *sched_ap_tick(regs_t *frame);
bool    sched_cpu_time_snapshot(u64 *busy_ms, u64 *idle_ms);
regs_t *sched_reschedule(regs_t *frame);
void    sched_yield(void);
void    sched_block(proc_state_t why);
void    sched_wake(proc_t *p);
void    sched_sleep_ms(u64 ms);

/* Atomic condition-to-sleep handoff for permanent kernel event counters.
 * Publish the payload under its own lock, release that lock, then signal.
 * Readers snapshot the sequence BEFORE inspecting the protected condition.
 * Both APIs take the runqueue lock: never call while holding a driver lock.
 * Wait is task-context only, not an ISR/NMI API. Signal is IRQ-safe after
 * private CPU descriptor initialization (not an early-boot/NMI API).
 * Wait returns 1 for changed sequence, 0 for deadline, -1 if unable to sleep.
 * UINT64_MAX deadline means indefinite. Spurious wakeups recheck the sequence.
 * Counter storage must remain mapped/alive for the whole boot. */
int  sched_wait_event(u64 *sequence, u64 expected, u64 deadline_ms);
void sched_signal_event(u64 *sequence);

/* Device-owned completion wait. Probe runs with interrupts disabled, including
 * from IRQ/timer context: it must only inspect permanently kernel-mapped state,
 * must not block/allocate/log or acquire a driver lock, and must be bounded.
 * The caller must hold a resource pin and keep probe/context/storage alive.
 * 1 = predicate observed, 0 = deadline, -1 = cannot sleep in this context.
 * Deadlines use g_uptime_ms. Interrupts are only wake hints, never completion. */
typedef bool (*sched_device_probe_fn)(void *context);
bool sched_device_wait_allowed(void);
int sched_wait_device(sched_device_probe_fn probe, void *context, u64 deadline_ms);
/* Called only after the device ISR releases its locks and acknowledges EOI. */
regs_t *sched_device_irq(regs_t *frame);

/* Called from the page fault path. */
bool user_fault(regs_t *r, u64 cr2);

/* Called for any exception in user code that would otherwise be fatal.  True
 * means it was redirected into the process's own handler and execution should
 * resume there. */
bool user_exception(regs_t *r, int vector, u64 address);
void proc_describe_current(char *buf, size_t cap);

/* ELF loading, shared with the process layer. */
int elf_load(proc_t *p, const void *image, size_t len, u64 *entry_out);

/* fd helpers */
int  proc_fd_alloc(proc_t *p, file_t *f);
file_t *proc_fd_acquire(proc_t *p, int fd); /* caller must vfs_close() */
int proc_fd_close(proc_t *p, int fd);
int proc_fd_dup(proc_t *p, int source, int target); /* -1 target: first free */
int proc_fd_install_pipe(proc_t *p, file_t *reader, file_t *writer, u64 address);
bool proc_fd_inherit_stdio(proc_t *child, proc_t *parent);
void proc_fd_close_all(proc_t *p);

/* Path resolution relative to a process's working directory. */
void proc_resolve_path(proc_t *p, const char *in, char *out, size_t cap);

#endif
