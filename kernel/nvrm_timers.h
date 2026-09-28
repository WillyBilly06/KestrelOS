/* RM timer lifetime. Included after the stack/context helpers in nvrm_os.c.
 * Callback arguments remain owned by RM; cancel/destroy drain callbacks before
 * their caller may free those arguments. No callback/allocation under the lock. */
struct nv_nano_timer {
    nv_state_t *nv;
    void *event;
    u64 due_us;
    bool active, running, destroying, rc;
    u32 cancellers;
    proc_t *runner;
    struct nv_nano_timer *next;
};
static struct nv_nano_timer *nvrm_nano_timers;
static volatile u32 nvrm_timer_lock, nvrm_pumping;
static u64 nvrm_timer_event;
static bool nvrm_timer_lock_acquire(void) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&nvrm_timer_lock, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}
static void nvrm_timer_lock_release(bool irq) {
    __atomic_store_n(&nvrm_timer_lock, 0u, __ATOMIC_RELEASE);
    irq_restore(irq);
}
static u64 nvrm_timer_deadline(u64 now, u64 delay_us) {
    return delay_us > ~0ull - now ? ~0ull : now + delay_us;
}
static bool nvrm_timer_registered(struct nv_nano_timer *timer) {
    for (struct nv_nano_timer *t = nvrm_nano_timers; t; t = t->next)
        if (t == timer) return true;
    return false;
}
static void nvrm_timer_wait(u64 expected) {
    if (!os_semaphore_may_sleep()) panic("RM timer: blocking cancellation in atomic context");
    if (sched_wait_event(&nvrm_timer_event, expected, ~0ull) < 0) sched_yield();
}
void nv_create_nano_timer(nv_state_t *nv, void *event, nv_nano_timer_t **out) {
    if (!out) return;
    *out = NULL;
    struct nv_nano_timer *t = kzalloc(sizeof *t);
    if (!t) return;
    t->nv = nv; t->event = event;
    bool irq = nvrm_timer_lock_acquire();
    t->next = nvrm_nano_timers; nvrm_nano_timers = t;
    nvrm_timer_lock_release(irq);
    *out = t;
}
void nv_start_nano_timer(nv_state_t *nv, nv_nano_timer_t *timer, NvU64 ns) {
    if (!timer) return;
    /* Round up without overflowing ns + 999. */
    u64 delay = ns / 1000u + (ns % 1000u != 0);
    bool irq = nvrm_timer_lock_acquire();
    if (nvrm_timer_registered(timer) && !timer->destroying && !timer->cancellers) {
        timer->nv = nv;
        timer->due_us = nvrm_timer_deadline(timer_now_us(), delay);
        timer->active = true;
    }
    nvrm_timer_lock_release(irq);
}
static void nvrm_timer_stop(struct nv_nano_timer *t, bool destroy) {
    if (!t) return;
    bool irq = nvrm_timer_lock_acquire();
    if (!nvrm_timer_registered(t)) { nvrm_timer_lock_release(irq); return; }
    if (t->running && t->runner == proc_current())
        panic("RM timer: synchronous self-cancellation");
    if (destroy && t->destroying) panic("RM timer: concurrent double destroy");
    if (t->cancellers == 0xffffffffu) panic("RM timer: cancellation count overflow");
    t->active = false;
    if (destroy) t->destroying = true;
    t->cancellers++; /* retain registration until this canceller leaves */
    nvrm_timer_lock_release(irq);
    for (;;) {
        u64 expected = __atomic_load_n(&nvrm_timer_event, __ATOMIC_ACQUIRE);
        irq = nvrm_timer_lock_acquire();
        if (!t->running && (!destroy || t->cancellers == 1u)) {
            t->cancellers--;
            if (destroy) {
                struct nv_nano_timer **link = &nvrm_nano_timers;
                while (*link != t) link = &(*link)->next;
                *link = t->next;
            }
            nvrm_timer_lock_release(irq);
            if (destroy) kfree(t);
            sched_signal_event(&nvrm_timer_event);
            return;
        }
        nvrm_timer_lock_release(irq);
        nvrm_timer_wait(expected);
    }
}
void nv_cancel_nano_timer(nv_state_t *nv, nv_nano_timer_t *t) {
    (void)nv; nvrm_timer_stop(t, false);
}
void nv_destroy_nano_timer(nv_state_t *nv, nv_nano_timer_t *t) {
    (void)nv; nvrm_timer_stop(t, true);
}
NvS32 nv_start_rc_timer(nv_state_t *nv) {
    if (!nv) return -1;
    struct nv_nano_timer *t = kzalloc(sizeof *t);
    if (!t) return -1;
    t->nv = nv; t->rc = true;
    bool irq = nvrm_timer_lock_acquire();
    bool exists = nv->rc_timer_enabled;
    for (struct nv_nano_timer *p = nvrm_nano_timers; p; p = p->next)
        if (p->rc && p->nv == nv) exists = true;
    if (exists) { nvrm_timer_lock_release(irq); kfree(t); return -1; }
    nv->rc_timer_enabled = 1;
    t->active = true;
    t->due_us = nvrm_timer_deadline(timer_now_us(), 1000000u);
    t->next = nvrm_nano_timers; nvrm_nano_timers = t;
    nvrm_timer_lock_release(irq);
    return 0;
}
NvS32 nv_stop_rc_timer(nv_state_t *nv) {
    if (!nv) return -1;
    bool irq = nvrm_timer_lock_acquire();
    struct nv_nano_timer *t = NULL;
    for (struct nv_nano_timer *p = nvrm_nano_timers; p; p = p->next)
        if (p->rc && p->nv == nv) { t = p; break; }
    if (!t || !nv->rc_timer_enabled) { nvrm_timer_lock_release(irq); return -1; }
    nv->rc_timer_enabled = 0;
    /* Arm/start is refused while the old registration drains. */
    t->active = false;
    nvrm_timer_lock_release(irq);
    nvrm_timer_stop(t, true);
    return 0;
}
void nvrm_os_pump(void) {
    /* This port services callbacks in task context, not inline in an RM ISR.
     * Atomic delay callers leave due timers to the dedicated timer thread. */
    if (!os_semaphore_may_sleep()) return;
    if (__atomic_exchange_n(&nvrm_pumping, 1u, __ATOMIC_ACQUIRE)) return;
    bool scan_irq = nvrm_timer_lock_acquire(), ready = false;
    u64 scan_now = timer_now_us();
    for (struct nv_nano_timer *t = nvrm_nano_timers; t; t = t->next)
        if (t->active && !t->destroying && !t->cancellers && t->due_us <= scan_now) {
            ready = true; break;
        }
    nvrm_timer_lock_release(scan_irq);
    if (!ready) { __atomic_store_n(&nvrm_pumping, 0u, __ATOMIC_RELEASE); return; }
    nvidia_stack_t *sp = nvrm_stack_alloc();
    if (!sp) { __atomic_store_n(&nvrm_pumping, 0u, __ATOMIC_RELEASE); return; }
    /* A callback rearming at zero delay must not keep an os_delay/os_schedule
     * caller inside this pump forever. Rotate selected timers for fairness. */
    for (u32 budget = 0; budget < 64u; budget++) {
        bool irq = nvrm_timer_lock_acquire();
        struct nv_nano_timer *due = NULL;
        struct nv_nano_timer **link = &nvrm_nano_timers;
        u64 now = timer_now_us();
        while (*link) {
            struct nv_nano_timer *t = *link;
            if (t->active && !t->destroying && !t->cancellers && t->due_us <= now) {
                due = t; *link = t->next;
                t->next = NULL;
                struct nv_nano_timer **tail = &nvrm_nano_timers;
                while (*tail) tail = &(*tail)->next;
                *tail = t;
                t->active = false; t->running = true; t->runner = proc_current();
                break;
            }
            link = &t->next;
        }
        /* Snapshot mutable rearm fields before dropping metadata ownership. */
        nv_state_t *nv = due ? due->nv : NULL;
        void *event = due ? due->event : NULL;
        bool rc = due && due->rc;
        nvrm_timer_lock_release(irq);
        if (!due) break;
        NV_STATUS status = rc ? rm_run_rc_callback(sp, nv) : rm_run_nano_timer_callback(sp, nv, event);
        irq = nvrm_timer_lock_acquire();
        if (rc && status == NV_OK && nv->rc_timer_enabled && !due->destroying && !due->cancellers) {
            due->due_us = nvrm_timer_deadline(timer_now_us(), 1000000u);
            due->active = true;
        }
        due->running = false; due->runner = NULL;
        nvrm_timer_lock_release(irq);
        /* No use of due/nv/event after publishing callback retirement. */
        sched_signal_event(&nvrm_timer_event);
    }
    kfree(sp);
    __atomic_store_n(&nvrm_pumping, 0u, __ATOMIC_RELEASE);
}
