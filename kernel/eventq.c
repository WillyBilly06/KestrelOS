/* eventq.c - the /dev/input event queue.
 *
 * The keyboard and mouse drivers push here; a graphical program reads whole
 * event records and blocks when the queue is empty.  Keeping both devices in
 * one stream means the window system has a single ordered view of what the
 * user did, which is what makes click-then-type behave.
 */
#include "kernel.h"
#include "vfs.h"
#include "input.h"
#include "proc.h"
#include "klog.h"
#include "time.h"
#include "spinlock.h"
#include "../include/kestrel/input.h"

#define QUEUE_SIZE 256

static kinput_event_t queue[QUEUE_SIZE];
static u32            head, tail;
static u64            dropped;
static spinlock_t     event_lock;
static u64            event_sequence; /* permanent scheduler wait key */

/* Caller owns event_lock. Signal only after releasing it: no event-lock ->
 * runqueue-lock nesting, including input interrupt producers. */
static void push_locked(const kinput_event_t *ev) {
    u32 next = (head + 1) % QUEUE_SIZE;
    if (next == tail) {
        /* Drop the oldest: a stale pointer position is worth less than the
         * one that just arrived. */
        tail = (tail + 1) % QUEUE_SIZE;
        dropped++;
    }
    queue[head] = *ev;
    head = next;
}

/* ------------------------------------------------------------------------- */
/* producers                                                                 */
/* ------------------------------------------------------------------------- */

void input_push_key(u16 code, u8 mods, bool pressed) {
    kinput_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = KEV_KEY;
    ev.code = code;
    ev.mods = mods;
    ev.pressed = pressed ? 1 : 0;
    ev.time_ms = g_uptime_ms;
    bool irq = spin_lock_irqsave(&event_lock);
    push_locked(&ev);
    spin_unlock_irqrestore(&event_lock, irq);
    sched_signal_event(&event_sequence);
}

void input_push_mouse(int x, int y, int dx, int dy, int wheel, u8 buttons, u8 changed) {
    kinput_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.x = x;
    ev.y = y;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.time_ms = g_uptime_ms;

    bool irq = spin_lock_irqsave(&event_lock);
    bool pushed = false;
    if (dx || dy) {
        ev.type = KEV_MOUSE_MOVE;
        push_locked(&ev);
        pushed = true;
    }
    if (wheel) {
        ev.type = KEV_MOUSE_WHEEL;
        ev.wheel = wheel;
        push_locked(&ev);
        pushed = true;
        ev.wheel = 0;
    }
    /* One event per button that actually changed, so a program never has to
     * work out which of several bits moved. */
    for (int bit = 0; bit < 3; bit++) {
        u8 mask = (u8)(1 << bit);
        if (!(changed & mask)) continue;
        ev.type = KEV_MOUSE_BUTTON;
        ev.code = mask;
        ev.pressed = (buttons & mask) ? 1 : 0;
        push_locked(&ev);
        pushed = true;
    }
    spin_unlock_irqrestore(&event_lock, irq);
    if (pushed) sched_signal_event(&event_sequence);
}

/* ------------------------------------------------------------------------- */
/* the device                                                                */
/* ------------------------------------------------------------------------- */

static u32 queue_count(void) {
    bool irq = spin_lock_irqsave(&event_lock);
    u32 count = (head + QUEUE_SIZE - tail) % QUEUE_SIZE;
    spin_unlock_irqrestore(&event_lock, irq);
    return count;
}

/* Snapshot the wake sequence BEFORE inspecting the condition. A producer can
 * publish between this inspection and the scheduler call without losing its
 * wake: changed sequence prevents sleeping on that old observation. */
static int wait_for_event(u64 deadline) {
    for (;;) {
        u64 sequence = __atomic_load_n(&event_sequence, __ATOMIC_ACQUIRE);
        u32 count = queue_count();
        if (count) return (int)count;
        if (deadline != ~0ull && g_uptime_ms >= deadline) return 0;
        int rc = sched_wait_event(&event_sequence, sequence, deadline);
        if (rc < 0) return -E_AGAIN;
        /* Another reader may consume the event first; recheck after every wake.
         * A timeout also rechecks the queue before reporting no input. */
    }
}

static ssize_t_k input_dev_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx; (void)off;
    if (len < sizeof(kinput_event_t)) return -E_INVAL;

    size_t want = len / sizeof(kinput_event_t);
    /* Bound the lock hold and use a private kernel snapshot. Never touch a
     * caller's possibly user-mapped buffer while owning the input queue lock. */
    kinput_event_t batch[32];
    if (want > ARRAY_LEN(batch)) want = ARRAY_LEN(batch);
    size_t n;
    for (;;) {
        int rc = wait_for_event(~0ull);
        if (rc < 0) return rc;
        n = 0;
        bool irq = spin_lock_irqsave(&event_lock);
        while (n < want && head != tail) {
            batch[n++] = queue[tail];
            tail = (tail + 1) % QUEUE_SIZE;
        }
        spin_unlock_irqrestore(&event_lock, irq);
        if (n) break;
    }
    memcpy(buf, batch, n * sizeof(kinput_event_t));
    return (ssize_t_k)(n * sizeof(kinput_event_t));
}

static int input_dev_ioctl(void *ctx, u32 cmd, void *arg) {
    (void)ctx;
    switch (cmd) {
    case 1:                                   /* how many events are waiting */
        if (arg) *(u32 *)arg = queue_count();
        return 0;
    case 2:                                   /* discard everything queued   */
        { bool irq = spin_lock_irqsave(&event_lock); tail = head;
          spin_unlock_irqrestore(&event_lock, irq); }
        return 0;

    case 3: {
        /* Wait until something arrives, or the given number of milliseconds
         * passes.  Without this a window system has two choices, and both are
         * bad: block for ever on a read, and then nothing else - no clock, no
         * animation - ever happens; or poll and sleep, which puts a fixed
         * delay between a movement of the mouse and anything appearing on the
         * screen.  That delay is most of what makes a desktop feel slow, and
         * it is entirely avoidable. */
        u32 ms = arg ? *(u32 *)arg : 0;
        u64 deadline = g_uptime_ms + ms;

        int waiting = wait_for_event(deadline);
        if (waiting < 0) return waiting;
        if (arg) *(u32 *)arg = (u32)waiting;
        return 0;
    }
    default:
        return -E_INVAL;
    }
}

static u64 input_dev_size(void *ctx) { (void)ctx; return sizeof(kinput_event_t); }

static int input_dev_ioctl_shape(void *ctx, u32 cmd, vfs_ioctl_shape_t *shape) {
    (void)ctx;
    *shape = (vfs_ioctl_shape_t){0};
    switch (cmd) {
    case 1: shape->out_bytes = 4; break;
    case 2: break;
    case 3: shape->in_bytes = shape->out_bytes = 4; break;
    default: return -E_INVAL;
    }
    return 0;
}

static const devfs_ops_t input_ops = {
    .read = input_dev_read, .ioctl = input_dev_ioctl, .size = input_dev_size,
    .ioctl_shape = input_dev_ioctl_shape,
};

void eventq_init(void) {
    head = tail = 0;
    dropped = 0;
    devfs_register("input", VN_CHR, &input_ops, NULL);
    kinfo("input", "event queue ready (%d slots)", QUEUE_SIZE);
}

u64 eventq_dropped(void) {
    bool irq = spin_lock_irqsave(&event_lock);
    u64 count = dropped;
    spin_unlock_irqrestore(&event_lock, irq);
    return count;
}
