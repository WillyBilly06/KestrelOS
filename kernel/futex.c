/* futex.c - native word-wait API.
 *
 * The scheduler owns registrations and the check-to-sleep handoff. IRQ
 * exclusion alone cannot serialize wait/wake calls on separate processors.
 * Physical word keys preserve the existing shared-mapping behavior; callers
 * must keep a waiting futex's backing mapping alive. This is not a full Linux
 * futex implementation (PI, requeue and robust lists are separate). The Linux
 * ABI uses the extended scheduler entry points for private keys and bitsets.
 */
#include "kernel.h"
#include "proc.h"

int futex_wait(u64 uaddr, u32 expect, int timeout_ms) {
    return sched_futex_wait(uaddr, expect, timeout_ms);
}

int futex_wake(u64 uaddr, int count) {
    /* A request to wake zero threads must not accidentally release one. */
    return sched_futex_wake(uaddr, count);
}
