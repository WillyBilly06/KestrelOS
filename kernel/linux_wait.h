/* Linux time/futex translation. Millisecond resolution matches the scheduler;
 * never fabricate nanosecond precision or treat an absolute timeout as relative. */
static u64 linux_deadline_add(u64 now, u64 delay) {
    return delay >= ~0ull - now ? ~0ull - 1 : now + delay;
}

static bool linux_timespec_ms(const linux_timespec_t *t, u64 *ms) {
    if (t->sec < 0 || t->nsec < 0 || t->nsec >= 1000000000ll) return false;
    u64 tail = ((u64)t->nsec + 999999u) / 1000000u;
    *ms = (u64)t->sec > (~0ull - 1 - tail) / 1000u
        ? ~0ull - 1 : (u64)t->sec * 1000u + tail;
    return true;
}

static int linux_clock_read(u64 clock, linux_timespec_t *out) {
    if (clock != 0 && clock != 1) return -E_INVAL; /* REALTIME / MONOTONIC */
    u64 mono, real;
    time_clock_snapshot(&mono, &real);
    u64 ms = clock == 0 ? real : mono;
    out->sec = (s64)(ms / 1000u);
    out->nsec = (s64)(ms % 1000u) * 1000000;
    return 0;
}

static int linux_futex_call(u64 address, u32 operation, u32 value, u64 timeout, u32 bitset) {
    u32 command = operation & 127u;
    bool private_key = !!(operation & FUTEX_PRIVATE_FLAG);
    bool realtime = !!(operation & FUTEX_CLOCK_REALTIME);
    if (operation & ~(127u | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) return -E_NOSYS;
    if (realtime && command != FUTEX_WAIT_BITSET) return -E_NOSYS;
    switch (command) {
    case FUTEX_WAIT: case FUTEX_WAIT_BITSET: {
        u32 mask = command == FUTEX_WAIT ? ~0u : bitset;
        if (!mask) return -E_INVAL;
        u64 deadline = ~0ull;
        if (timeout) {
            linux_timespec_t snapshot;
            if (!syscall_user_copy(&snapshot, timeout, sizeof snapshot, false)) return -E_FAULT;
            u64 ms, mono, real;
            if (!linux_timespec_ms(&snapshot, &ms)) return -E_INVAL;
            time_clock_snapshot(&mono, &real);
            if (command == FUTEX_WAIT) deadline = linux_deadline_add(mono, ms);
            else if (!realtime) deadline = ms;
            else deadline = ms <= real ? mono : linux_deadline_add(mono, ms - real);
        }
        return sched_futex_wait_ex(address, value, deadline, mask, private_key);
    }
    case FUTEX_WAKE: case FUTEX_WAKE_BITSET: {
        u32 mask = command == FUTEX_WAKE ? ~0u : bitset;
        if (value > 0x7fffffffu) return -E_INVAL;
        return sched_futex_wake_ex(address, (int)value, mask, private_key);
    }
    default: return -E_NOSYS;
    }
}
