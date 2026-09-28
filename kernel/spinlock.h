#ifndef KESTREL_SPINLOCK_H
#define KESTREL_SPINLOCK_H
#include "kernel.h"

/* Zero-initialized, non-recursive lock for short, non-sleeping kernel sections.
 * Local IRQ exclusion prevents a timer from descheduling a lock owner or an
 * interrupt handler from waiting on its own interrupted CPU. Acquire/release
 * order shared data across CPUs; CLI alone never provides that exclusion.
 * Not NMI-safe. Never allocate, yield, or invoke a driver while holding one
 * unless the complete nested lock order has been audited. */
typedef struct { u32 held; } spinlock_t;

static inline bool spin_lock_irqsave(spinlock_t *lock) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&lock->held, 1u, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->held, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
    return irq;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, bool irq) {
    __atomic_store_n(&lock->held, 0u, __ATOMIC_RELEASE);
    irq_restore(irq);
}
#endif
