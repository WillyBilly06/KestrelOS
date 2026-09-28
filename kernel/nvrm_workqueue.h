/* RM host work queues; included by nvrm_os.c after stack/scheduler helpers.
 * Queue records and event counters have boot lifetime: RM's opaque queue key
 * is never dereferenced here. Item data remains RM-owned until execution. */
typedef struct nvrm_work {
    void *data;
    u64 serial;
    struct nvrm_work *next;
} nvrm_work_t;
typedef struct nvrm_work_queue {
    struct os_work_queue *key;
    struct nvrm_work_queue *next;
    nvrm_work_t *head, *tail;
    u64 submitted, completed;
    u32 started, unload_flushes; /* 0 absent, 1 creating, 2 runnable */
    proc_t *runner;
} nvrm_work_queue_t;
static nvrm_work_queue_t nvrm_global_work_queue;
static nvrm_work_queue_t *nvrm_private_work_queues;
static volatile u32 nvrm_work_lock;
static u64 nvrm_work_event;

static bool nvrm_work_lock_acquire(void) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&nvrm_work_lock, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}
static void nvrm_work_lock_release(bool irq) {
    __atomic_store_n(&nvrm_work_lock, 0u, __ATOMIC_RELEASE);
    irq_restore(irq);
}
static void nvrm_work_wait(u64 expected) {
    /* Do not abandon RM-owned item/stack lifetimes on a process stop. */
    if (sched_wait_event(&nvrm_work_event, expected, ~0ull) < 0) sched_yield();
}

static nvrm_work_queue_t *nvrm_work_find(struct os_work_queue *key) {
    if (!key) return &nvrm_global_work_queue;
    for (nvrm_work_queue_t *q = nvrm_private_work_queues; q; q = q->next)
        if (q->key == key) return q;
    return NULL;
}
static nvrm_work_queue_t *nvrm_work_get(struct os_work_queue *key) {
    bool irq = nvrm_work_lock_acquire();
    nvrm_work_queue_t *q = nvrm_work_find(key);
    nvrm_work_lock_release(irq);
    if (q) return q;
    nvrm_work_queue_t *made = kzalloc(sizeof *made);
    if (!made) return NULL;
    made->key = key;
    irq = nvrm_work_lock_acquire();
    q = nvrm_work_find(key);
    if (!q) {
        made->next = nvrm_private_work_queues;
        nvrm_private_work_queues = made;
        q = made;
        made = NULL;
    }
    nvrm_work_lock_release(irq);
    kfree(made);
    return q;
}

/* One item at a time on this queue, separate queues have separate workers.
 * Split from the permanent loop so production publication/retirement can be
 * exercised without emulating kernel context switches. */
static bool nvrm_work_process_one(nvrm_work_queue_t *q, nvidia_stack_t *sp) {
    if (!sp) return false;
    bool irq = nvrm_work_lock_acquire();
    nvrm_work_t *work = q->head;
    if (work) {
        q->head = work->next;
        if (!q->head) q->tail = NULL;
    }
    nvrm_work_lock_release(irq);
    if (!work) return false;
    u64 serial = work->serial;
    rm_execute_work_item(sp, work->data);
    kfree(work);
    irq = nvrm_work_lock_acquire();
    if (serial != q->completed + 1u) panic("RM queue: unordered completion");
    q->completed = serial;
    nvrm_work_lock_release(irq);
    sched_signal_event(&nvrm_work_event);
    return true;
}
static void nvrm_work_thread(void *arg) {
    nvrm_work_queue_t *q = arg;
    bool irq = nvrm_work_lock_acquire();
    q->runner = proc_current();
    nvrm_work_lock_release(irq);
    nvidia_stack_t *sp = NULL;
    /* Never acknowledge or discard an accepted item on stack-allocation OOM. */
    while (!(sp = nvrm_stack_alloc())) sched_sleep_ms(1);
    for (;;) {
        u64 expected = __atomic_load_n(&nvrm_work_event, __ATOMIC_ACQUIRE);
        if (!nvrm_work_process_one(q, sp)) nvrm_work_wait(expected);
    }
}
static NV_STATUS nvrm_work_start(nvrm_work_queue_t *q) {
    for (;;) {
        u64 expected = __atomic_load_n(&nvrm_work_event, __ATOMIC_ACQUIRE);
        bool irq = nvrm_work_lock_acquire();
        u32 state = q->started;
        if (state == 2u) { nvrm_work_lock_release(irq); return NV_OK; }
        nvrm_work_lock_release(irq);
        /* Linux initializes queues before atomic enqueue. Never create a
         * scheduler thread or wait for another creator from an ISR. */
        if (!os_semaphore_may_sleep()) return NV_ERR_NOT_READY;
        if (state == 1u) { nvrm_work_wait(expected); continue; }
        irq = nvrm_work_lock_acquire();
        if (q->started) { nvrm_work_lock_release(irq); continue; }
        q->started = 1u;
        nvrm_work_lock_release(irq);
        int pid = kthread_create("nvrm-work", nvrm_work_thread, q);
        irq = nvrm_work_lock_acquire();
        q->started = pid < 0 ? 0u : 2u;
        nvrm_work_lock_release(irq);
        sched_signal_event(&nvrm_work_event);
        return pid < 0 ? NV_ERR_OPERATING_SYSTEM : NV_OK;
    }
}
NV_STATUS os_queue_work_item(struct os_work_queue *key, void *data) {
    nvrm_work_queue_t *q = nvrm_work_get(key);
    if (!q) return NV_ERR_NO_MEMORY;
    NV_STATUS status = nvrm_work_start(q);
    if (status != NV_OK) return status;
    nvrm_work_t *work = kzalloc(sizeof *work);
    if (!work) return NV_ERR_NO_MEMORY;
    work->data = data;
    bool irq = nvrm_work_lock_acquire();
    if (q->submitted == ~0ull) {
        nvrm_work_lock_release(irq); kfree(work); return NV_ERR_INVALID_STATE;
    }
    work->serial = ++q->submitted;
    if (q->tail) q->tail->next = work; else q->head = work;
    q->tail = work;
    nvrm_work_lock_release(irq);
    sched_signal_event(&nvrm_work_event);
    return NV_OK;
}
NV_STATUS os_flush_work_queue(struct os_work_queue *key, NvBool is_unload) {
    if (!os_semaphore_may_sleep()) return NV_ERR_ILLEGAL_ACTION;
    bool irq = nvrm_work_lock_acquire();
    nvrm_work_queue_t *q = nvrm_work_find(key);
    if (!q) { nvrm_work_lock_release(irq); return NV_OK; }
    if (q->runner && q->runner == proc_current()) {
        nvrm_work_lock_release(irq); return NV_ERR_ILLEGAL_ACTION;
    }
    if (is_unload && q->unload_flushes == 0xffffffffu) {
        nvrm_work_lock_release(irq); return NV_ERR_INVALID_STATE;
    }
    if (is_unload) q->unload_flushes++;
    nvrm_work_lock_release(irq);
    /* NVIDIA nv_kthread_q_flush performs TWO FIFO barriers, explicitly covering
     * an item that requeues itself. A false argument still flushes; it merely
     * does not mark this operation as an unload flush. */
    for (u32 pass = 0; pass < 2u; pass++) {
        irq = nvrm_work_lock_acquire();
        u64 target = q->submitted;
        nvrm_work_lock_release(irq);
        for (;;) {
            u64 expected = __atomic_load_n(&nvrm_work_event, __ATOMIC_ACQUIRE);
            irq = nvrm_work_lock_acquire();
            bool done = q->completed >= target;
            nvrm_work_lock_release(irq);
            if (done) break;
            nvrm_work_wait(expected);
        }
    }
    if (is_unload) {
        irq = nvrm_work_lock_acquire();
        q->unload_flushes--;
        nvrm_work_lock_release(irq);
    }
    return NV_OK;
}
NvBool os_is_queue_flush_ongoing(struct os_work_queue *key) {
    bool irq = nvrm_work_lock_acquire();
    nvrm_work_queue_t *q = nvrm_work_find(key);
    NvBool ongoing = q && q->unload_flushes ? NV_TRUE : NV_FALSE;
    nvrm_work_lock_release(irq);
    return ongoing;
}
