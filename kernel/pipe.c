/* pipe.c - anonymous pipes.
 *
 * A pipe is a ring buffer with two ends.  A read blocks while the buffer is
 * empty and at least one writer is still open, and returns zero once the last
 * writer closes; a write blocks while the buffer is full.  That is what lets
 * the desktop's terminal run the ordinary shell with its output redirected
 * into a window, rather than reimplementing every command.
 */
#include "kernel.h"
#include "vfs.h"
#include "proc.h"
#include "mm.h"
#include "klog.h"
#include "spinlock.h"
#include "smp.h"

#define PIPE_CAPACITY 8192

typedef struct {
    u8   *buffer;
    u32   head, tail;       /* head is where the next byte is written */
    u32   count;
    int   readers, writers;
    spinlock_t lock;
} pipe_t;

typedef struct {
    pipe_t *pipe;
    bool    is_writer;
} pipe_end_t;

static const vnode_ops_t pipe_vops;

/* ------------------------------------------------------------------ waiting */

/* Permanent event counters: no waiter points into a freed pipe or reusable
 * proc slot. Cross-pipe wakeups are harmless; each waiter rechecks its pipe.
 * Snapshot before testing the predicate, then use the runqueue's atomic wait
 * handoff. A write/close between test and sleep can no longer lose its wake. */
static u64 pipe_read_event, pipe_write_event;
static bool pipe_lock(pipe_t *p) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&p->lock.held, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}
static void pipe_unlock(pipe_t *p, bool irq) { spin_unlock_irqrestore(&p->lock, irq); }

static void pipe_wait(u64 *event, u64 expected) {
    if (sched_wait_event(event, expected, ~0ull) < 0 && !proc_stop_requested())
        sched_sleep_ms(1); /* early/non-task caller cannot join the wait queue */
}

/* ------------------------------------------------------------------- vnode */

static ssize_t_k pipe_read(vnode_t *vn, void *buf, size_t len, u64 off) {
    (void)off;
    pipe_end_t *end = vn->priv;
    if (!end || end->is_writer) return -E_BADF;
    pipe_t *p = end->pipe;
    if (!len) return 0;

    size_t n = 0;
    u8 *out = buf;

    for (;;) {
        if (proc_stop_requested()) return -E_INTR;
        u64 expected = __atomic_load_n(&pipe_read_event, __ATOMIC_ACQUIRE);
        bool irq = pipe_lock(p);
        if (p->count) {
            while (n < len && p->count) {
                out[n++] = p->buffer[p->tail];
                p->tail = (p->tail + 1) % PIPE_CAPACITY;
                p->count--;
            }
            pipe_unlock(p, irq);
            break;
        }
        if (p->writers == 0) { pipe_unlock(p, irq); return 0; }   /* end of stream */
        pipe_unlock(p, irq);
        pipe_wait(&pipe_read_event, expected);
    }

    sched_signal_event(&pipe_write_event);
    return (ssize_t_k)n;
}

static ssize_t_k pipe_write(vnode_t *vn, const void *buf, size_t len, u64 off) {
    (void)off;
    pipe_end_t *end = vn->priv;
    if (!end || !end->is_writer) return -E_BADF;
    pipe_t *p = end->pipe;

    const u8 *in = buf;
    size_t written = 0;

    while (written < len) {
        if (proc_stop_requested()) return written ? (ssize_t_k)written : -E_INTR;
        u64 expected = __atomic_load_n(&pipe_write_event, __ATOMIC_ACQUIRE);
        bool irq = pipe_lock(p);
        if (p->readers == 0) {
            pipe_unlock(p, irq);
            /* Nobody will ever read this; report it rather than block forever. */
            return written ? (ssize_t_k)written : -E_PERM;
        }
        if (p->count == PIPE_CAPACITY) {
            pipe_unlock(p, irq);
            pipe_wait(&pipe_write_event, expected);
            continue;
        }
        while (written < len && p->count < PIPE_CAPACITY) {
            p->buffer[p->head] = in[written++];
            p->head = (p->head + 1) % PIPE_CAPACITY;
            p->count++;
        }
        pipe_unlock(p, irq);
        sched_signal_event(&pipe_read_event);
    }
    return (ssize_t_k)written;
}

/* Command 1 reports how many bytes are buffered, which is what lets a reader
 * poll a pipe instead of blocking on it. */
static int pipe_ioctl(vnode_t *vn, u32 cmd, void *arg) {
    pipe_end_t *end = vn->priv;
    if (!end) return -E_BADF;

    pipe_t *p = end->pipe;
    bool irq = pipe_lock(p);
    u32 count = p->count;
    int peers = end->is_writer ? p->readers : p->writers;
    pipe_unlock(p, irq);
    switch (cmd) {
    case 1:
        if (arg) *(u32 *)arg = count;
        return (int)count;
    case 2:
        /* Is the far end still open? */
        return peers;
    default:
        return -E_INVAL;
    }
}

static int pipe_ioctl_shape(vnode_t *vn, u32 cmd, vfs_ioctl_shape_t *shape) {
    (void)vn;
    *shape = (vfs_ioctl_shape_t){0};
    if (cmd == 1) shape->out_bytes = 4;
    else if (cmd != 2) return -E_INVAL;
    return 0;
}

static int pipe_stat(vnode_t *vn, vstat_t *st) {
    pipe_end_t *end = vn->priv;
    st->type = VN_CHR;
    st->size = 0;
    if (end) {
        bool irq = pipe_lock(end->pipe);
        st->size = end->pipe->count;
        pipe_unlock(end->pipe, irq);
    }
    st->mode = 0600;
    st->mtime = 0;
    return 0;
}

static void pipe_release(vnode_t *vn) {
    pipe_end_t *end = vn->priv;
    if (end) {
        pipe_t *p = end->pipe;
        bool irq = pipe_lock(p);
        if (end->is_writer) p->writers--; else p->readers--;
        bool dead = (p->readers == 0 && p->writers == 0);
        pipe_unlock(p, irq);

        /* Whichever side is left must be woken so it can observe the closure
         * rather than block for ever. */
        /* Do not access p after unlocking except for the unique final free:
         * the other endpoint may concurrently drop the final reference. */
        sched_signal_event(&pipe_read_event);
        sched_signal_event(&pipe_write_event);

        if (dead) { kfree(p->buffer); kfree(p); }
        kfree(end);
    }
    kfree(vn);
}

static const vnode_ops_t pipe_vops = {
    .read    = pipe_read,
    .write   = pipe_write,
    .ioctl   = pipe_ioctl,
    .ioctl_shape = pipe_ioctl_shape,
    .stat    = pipe_stat,
    .release = pipe_release,
};

/* ------------------------------------------------------------------ create */

static vnode_t *make_end(pipe_t *p, bool writer) {
    pipe_end_t *end = kzalloc(sizeof *end);
    vnode_t *vn = kzalloc(sizeof *vn);
    if (!end || !vn) { kfree(end); kfree(vn); return NULL; }

    end->pipe = p;
    end->is_writer = writer;

    vn->type = VN_CHR;
    vn->refs = 1;
    vn->priv = end;
    vn->ops = &pipe_vops;
    return vn;
}

/* Build a pipe and hand back an already-open file at each end. */
int pipe_create(file_t **read_end, file_t **write_end) {
    if (!read_end || !write_end) return -E_INVAL;
    *read_end = *write_end = NULL;
    pipe_t *p = kzalloc(sizeof *p);
    if (!p) return -E_NOMEM;
    p->buffer = kmalloc(PIPE_CAPACITY);
    if (!p->buffer) { kfree(p); return -E_NOMEM; }
    p->readers = 1;
    p->writers = 1;

    vnode_t *rv = make_end(p, false);
    vnode_t *wv = make_end(p, true);
    if (!rv || !wv) {
        if (rv) { kfree(rv->priv); kfree(rv); }
        if (wv) { kfree(wv->priv); kfree(wv); }
        kfree(p->buffer);
        kfree(p);
        return -E_NOMEM;
    }

    int r = vfs_open_vnode(rv, O_RDONLY, read_end);
    if (r < 0) { vnode_unref(rv); vnode_unref(wv); return r; }
    r = vfs_open_vnode(wv, O_WRONLY, write_end);
    if (r < 0) { vfs_close(*read_end); *read_end = NULL; vnode_unref(wv); return r; }
    return 0;
}
