/* CPU-only VM integration guest. Packaged as /bin/init by test_smp_qemu.py,
 * never by the normal OS build. No graphics or codec calls. */
#include "kestrel.h"

#define WORKERS 32
#define ROUNDS 512
static uint32_t ready, go, finished, failures, cpu_mask, increments;
static int tids[WORKERS];
static uint64_t results[WORKERS], tls_cookie[WORKERS];
static ksysinfo_t system_info;

static unsigned apic_id(void) {
    unsigned a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return b >> 24;
}

static void sample_cpu(void) {
    unsigned id = apic_id();
    if (id < 32) __atomic_fetch_or(&cpu_mask, 1u << id, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
}

static void work(void *arg) {
    unsigned index = (unsigned)(uintptr_t)arg;
    tids[index] = thread_id();
    uint64_t *heap = malloc(16384);
    if (heap) {
        for (unsigned i = 0; i < 2048; i++) heap[i] = ((uint64_t)index << 32) | i;
    } else {
        __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    }
    tls_cookie[index] = 0x1234567800000000ull + index;
    thread_set_gs(&tls_cookie[index]);
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) yield();
    uint64_t value = index + 1;
    for (unsigned round = 0; round < ROUNDS; round++) {
        sample_cpu();
        /* Register/stack-dependent work between interrupts and BSP syscalls. */
        for (unsigned i = 0; i < 8192; i++)
            value = (value ^ (value >> 13)) * 0x9e3779b97f4a7c15ull + i;
        __atomic_fetch_add(&increments, 1, __ATOMIC_RELAXED);
        if (!(round & 7)) {
            if (thread_id() != tids[index])
                __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
            sleep_ms(1);
        }
        uint64_t observed;
        __asm__ volatile("movq %%gs:0, %0" : "=r"(observed));
        if (observed != tls_cookie[index])
            __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    }
    results[index] = value;
    if (heap) {
        for (unsigned i = 0; i < 2048; i++)
            if (heap[i] != (((uint64_t)index << 32) | i))
                __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        free(heap);
    }
    __atomic_fetch_add(&finished, 1, __ATOMIC_RELEASE);
    futex_wake(&finished, WORKERS);
}

static void stop_worker(void *arg) {
    (void)arg;
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile("pause");
}

static int fail(const char *reason) {
    log_write(3, "smp-test", reason);
    log_write(3, "smp-test", "SMP_QEMU_FAIL");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--stop-child")) {
        if (thread_create(stop_worker, 0) < 0 ||
            thread_create(stop_worker, 0) < 0) return 9;
        uint64_t deadline = uptime_ms() + 5000;
        while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 2) {
            if (uptime_ms() > deadline) return 10;
            sleep_ms(1);
        }
        /* Process exit must stop siblings executing remotely before freeing
         * the shared address space. Parent's wait verifies group drain. */
        return 0;
    }
    log_write(1, "smp-test", "CPU-only integration guest entered user mode");
    if (sysinfo(&system_info) != 0 || system_info.cpu_application_threads < 2 ||
        system_info.cpu_application_threads > WORKERS)
        return fail("invalid reported application CPU count");
    for (unsigned i = 0; i < WORKERS; i++)
        if (thread_create(work, (void *)(uintptr_t)i) < 0)
            return fail("thread creation failed");
    uint64_t deadline = uptime_ms() + 120000;
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != WORKERS) {
        if (uptime_ms() > deadline) return fail("thread startup timed out");
        sleep_ms(1);
    }
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) != WORKERS) {
        if (uptime_ms() > deadline) return fail("thread work timed out");
        uint32_t seen = __atomic_load_n(&finished, __ATOMIC_ACQUIRE);
        futex_wait(&finished, seen, 50);
    }
    for (unsigned n = 0; n < WORKERS; n++) {
        uint64_t expected = n + 1;
        for (unsigned round = 0; round < ROUNDS; round++)
            for (unsigned i = 0; i < 8192; i++)
                expected = (expected ^ (expected >> 13)) *
                           0x9e3779b97f4a7c15ull + i;
        if (results[n] != expected) return fail("register/work checksum mismatch");
        if (tids[n] <= 0) return fail("invalid thread identity");
        for (unsigned j = 0; j < n; j++)
            if (tids[n] == tids[j]) return fail("duplicate thread identity");
    }
    if (failures || increments != WORKERS * ROUNDS)
        return fail("GS, heap, thread identity, or atomic work count failed");
    char report[160];
    unsigned observed_cpus = 0;
    for (unsigned mask = cpu_mask; mask; mask &= mask - 1) observed_cpus++;
    snprintf(report, sizeof report,
             "CPU work complete: APIC mask=0x%x increments=%u observed=%u reported=%u",
             cpu_mask, increments, observed_cpus, system_info.cpu_application_threads);
    log_write(1, "smp-test", report);
    if (!cpu_mask || !(cpu_mask & (cpu_mask - 1)))
        return fail("application work never executed on a second CPU");
    if (observed_cpus != system_info.cpu_application_threads)
        return fail("not every reported application CPU executed sampled work");
    const char *linux_argv[] = { "threads.linux" };
    int rc = run("/bin/threads.linux", linux_argv, 1);
    if (rc != 0) return fail("Linux clone/futex test failed");
    log_write(1, "smp-test", "Linux clone/futex test passed");
    const char *child_argv[] = { "smp-test", "--stop-child" };
    for (unsigned i = 0; i < 8; i++) {
        rc = run("/bin/init", child_argv, 2);
        if (rc != 0) return fail("remote group exit/drain failed");
    }
    log_write(1, "smp-test", "SMP_QEMU_PASS: CPU masks, TLS, work, clone/futex, group exit");
    for (;;) sleep_ms(60000);
}
