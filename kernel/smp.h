#ifndef KESTREL_SMP_H
#define KESTREL_SMP_H

#include "kernel.h"

/* One piece of a batch.  `piece` is which of `pieces` this call is for, and a
 * job may touch only memory it was handed - see the note at the top of smp.c
 * for why that restriction is the whole design. */
typedef void (*smp_job_fn)(void *arg, int piece, int pieces);

void smp_init(void);
int  smp_worker_count(void);
/* Stable slot/stack ownership is established when a restricted AP joins its
 * worker loop. The scheduler uses this to prepare a separate future idle
 * context without treating the AP as application capacity. */
bool smp_worker_slot_online(u32 slot);
/* Retire one worker at a batch boundary, retaining at least one worker for
 * existing restricted jobs, then enter its prepared application idle stack. */
bool smp_promote_one_application_cpu(void);

typedef struct {
    u64 busy_us;   /* time APs spent executing restricted kernel batches */
    u64 idle_us;   /* time online APs waited for work, including current wait */
    u32 workers;  /* APs actually represented; excludes BSP and unstarted CPUs */
} smp_time_snapshot_t;
/* Add these cumulative AP counters to BSP scheduler time for system-wide
 * utilization. False means unavailable/racing; do not replace it with zero. */
bool smp_worker_time_snapshot(smp_time_snapshot_t *out);

/* VMM's synchronous translation barrier. Publish page-table changes before
 * calling; keep removed frames/tables owned until it returns. full=true drops
 * all cached translations, otherwise one page in pml4 (kernel-half mappings
 * are shared). Does not pin mappings or permit freeing an active CR3 root.
 * No allocation/scheduler/driver callbacks; not callable from NMI context. */
void smp_tlb_invalidate(u64 pml4, u64 va, bool full);
/* Service a mailbox while waiting for a VM/rendezvous lock, so a CPU cannot
 * block a remote shootdown by spinning with its TLB IPI masked. Saves/restores
 * local IRQ state internally to exclude a simultaneous local TLB ISR. */
void smp_tlb_poll(void);

/* Nudge an enrolled application CPU after runnable work is published. Slot
 * zero is the BSP; restricted AP workers ignore the scheduler vector. */
void smp_reschedule_cpu(u32 slot);
/* These vectors belong to the receiving AP's LAPIC and must be acknowledged
 * there before any user-origin interrupt frame can migrate to the BSP. */
bool smp_interrupt_local_on_ap(u32 vector);

/* Split work across every processor, including this one, and return when all
 * of it is finished and every AP has released the batch. Jobs must be bounded,
 * must not sleep or recursively call smp_run, and may touch only supplied
 * buffers. With no workers this runs the pieces in order. This is NOT a
 * general application-thread scheduler. */
void smp_run(smp_job_fn fn, void *arg, int pieces);

/* The same, with the jobs running in a given address space rather than the
 * kernel's own - which is what a job touching a program's memory needs, since
 * the woken cores hold the kernel's tables and those map no program.  See the
 * note in smp.c for why this is safe. */
/* User mappings stay protected by BSP IRQ exclusion for now; a process-resource
 * pin alone does not stop sibling munmap. Kernel-only batches are preemptible
 * when the caller has interrupts enabled. Not callable from arbitrary IRQs. */
void smp_run_in(smp_job_fn fn, void *arg, int pieces, u64 cr3);

#endif
