/* NVIDIA nvgputypes.h NvNotification / nvos.h ERROR notification slot 0.
 * Kept CPU mapped for the codec channel's entire (including quarantined)
 * lifetime. A zero record is NOT evidence that a timed-out job is healthy. */
#ifndef NV_ERROR_NOTIFIER_H
#define NV_ERROR_NOTIFIER_H
typedef struct {
    /* method_notification.c writes nanoseconds[0]=TimeHi, [1]=TimeLo. */
    u32 time_hi, time_lo, info32;
    u16 info16, status;
} nv_error_notification_t;
_Static_assert(sizeof(nv_error_notification_t) == 16, "NvNotification ABI");
_Static_assert(__builtin_offsetof(nv_error_notification_t, status) == 14,
               "NvNotification status offset");

static nv_error_notification_t nv_error_notification_read(
    const volatile nv_error_notification_t *p) {
    nv_error_notification_t n;
    n.status = p->status;
    __asm__ volatile("lfence" ::: "memory");
    n.time_lo = p->time_lo; n.time_hi = p->time_hi;
    n.info32 = p->info32; n.info16 = p->info16;
    return n;
}

/* Two identical reads are a best-effort diagnostic snapshot, not a seqlock:
 * RM may update an already-published error again. Never authorize reuse from
 * this result. No RM call or extra GPU work is needed to read system memory. */
static bool nv_error_notification_snapshot(
    const volatile nv_error_notification_t *p, nv_error_notification_t *out) {
    if (!p || !out) return false;
    nv_error_notification_t a = nv_error_notification_read(p);
    __asm__ volatile("lfence" ::: "memory");
    nv_error_notification_t b = nv_error_notification_read(p);
    *out = b;
    return a.status == b.status && a.time_lo == b.time_lo &&
           a.time_hi == b.time_hi && a.info32 == b.info32 && a.info16 == b.info16;
}

/* krcErrorSetNotifier_IMPL publishes 0xffff, not a completion cookie. This
 * boot-lifetime error slot is initialized once and never cleared/reused.
 * A published error can therefore veto submission/retirement; absence cannot
 * authorize either. Matching reads are NOT a guarantee of recovery/quiescence. */
static bool nv_error_notification_failed(
    const volatile nv_error_notification_t *p) {
    nv_error_notification_t n;
    return nv_error_notification_snapshot(p, &n) && n.status == 0xffffu;
}
#endif
