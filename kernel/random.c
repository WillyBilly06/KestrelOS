/* random.c - unpredictable numbers.
 *
 * A connection's security rests entirely on the other side not being able to
 * guess the private key this machine picked.  If those numbers are
 * predictable, everything above them - the curve, the cipher, the
 * certificate - is decoration.  So this is one of the few places where being
 * approximately right is the same as being wrong.
 *
 * There are two sources.  The processor's own generator, where it has one, is
 * a hardware noise source and is what should be used.  Where it does not, or
 * where it fails - and it does fail, on some parts, by returning zeros - what
 * is left is a pool stirred from whatever the machine can observe that an
 * outsider cannot: the exact timing of interrupts, the low bits of the cycle
 * counter at unpredictable moments, the contents of uninitialised memory at
 * start-up.
 *
 * The pool is not a substitute for a hardware source and this file says so
 * when it has to fall back on it.  What it is is better than a counter, which
 * is what a system with neither would otherwise be using.
 */
#include "kernel.h"
#include "cpu.h"
#include "time.h"
#include "klog.h"
#include "crypto.h"

/* ---------------------------------------------------------------- the pool
 *
 * Everything stirred in goes through a hash, so no single contribution can
 * steer the output and observing the output says nothing about the pool.
 */
static u8   pool[SHA256_SIZE];
static u64  stirred;
static bool have_hardware;
static bool warned;

void random_stir(const void *data, size_t len) {
    /* The new material and the current pool, hashed together.  Feeding the
     * pool back in is what makes this accumulate rather than replace. */
    sha256_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, pool, sizeof pool);
    sha256_update(&ctx, data, len);

    u64 now = rdtsc();
    sha256_update(&ctx, &now, sizeof now);
    sha256_final(&ctx, pool);
    stirred++;
}

/* The processor's own generator.  It can fail, and the specification says to
 * retry a bounded number of times and then give up rather than loop. */
static bool hardware_random(u64 *out) {
    if (!g_cpu.has_rdrand) return false;

    for (int attempt = 0; attempt < 10; attempt++) {
        u64 value;
        u8 ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) {
            /* All zeros or all ones from a hardware generator means it has
             * failed, whatever the flag says. */
            if (value != 0 && value != ~0ULL) { *out = value; return true; }
        }
    }
    return false;
}

void random_init(void) {
    /* Whatever the machine can offer at start-up: the cycle counter, the
     * clock, the addresses the allocator happened to hand out, and the
     * contents of a stack buffer nobody has written. */
    u8 uninitialised[64];
    u64 stamps[8];
    for (int i = 0; i < 8; i++) {
        stamps[i] = rdtsc();
        timer_udelay(1);
    }
    random_stir(stamps, sizeof stamps);
    random_stir(uninitialised, sizeof uninitialised);

    u64 test;
    have_hardware = hardware_random(&test);
    if (have_hardware) {
        random_stir(&test, sizeof test);
        kinfo("random", "the processor's own generator is present and working");
    } else {
        kwarn("random", "this processor has no working random number generator; "
                        "keys will come from a pool stirred from the machine's "
                        "own timing, which is weaker");
    }
}

/* Called from the interrupt path: the exact moment an interrupt arrives is
 * something an outsider cannot see, and there are thousands a second. */
void random_event(void) {
    static int counter;
    /* Not every one - hashing on every interrupt would cost more than it is
     * worth - but often enough that the pool keeps moving. */
    if ((++counter & 63) != 0) return;
    u64 now = rdtsc();
    random_stir(&now, sizeof now);
}

void random_bytes(void *out, size_t len) {
    u8 *bytes = out;

    /* Straight from the processor where there is one. */
    if (have_hardware) {
        size_t at = 0;
        while (at < len) {
            u64 value;
            if (!hardware_random(&value)) {
                have_hardware = false;      /* it stopped working */
                kwarn("random", "the processor's generator stopped answering; "
                                "falling back on the pool");
                break;
            }
            size_t chunk = len - at < 8 ? len - at : 8;
            memcpy(bytes + at, &value, chunk);
            at += chunk;
        }
        if (at >= len) {
            /* Mixed with the pool as well, so a backdoored generator is not
             * the only thing standing between a key and an attacker. */
            u8 extra[SHA256_SIZE];
            random_stir(bytes, len);
            memcpy(extra, pool, sizeof extra);
            for (size_t i = 0; i < len; i++) bytes[i] ^= extra[i % sizeof extra];
            return;
        }
    }

    if (!warned) {
        warned = true;
        kwarn("random", "producing key material from the pool alone");
    }

    /* From the pool: each block is a hash of the pool and a counter, and the
     * pool is stirred afterwards so the same block never comes out twice. */
    size_t at = 0;
    u64 counter = 0;
    while (at < len) {
        sha256_t ctx;
        u8 block[SHA256_SIZE];
        sha256_init(&ctx);
        sha256_update(&ctx, pool, sizeof pool);
        sha256_update(&ctx, &counter, sizeof counter);
        sha256_final(&ctx, block);

        size_t chunk = len - at < sizeof block ? len - at : sizeof block;
        memcpy(bytes + at, block, chunk);
        at += chunk;
        counter++;
    }
    random_stir(&counter, sizeof counter);
}

bool random_is_strong(void) { return have_hardware; }
u64  random_stir_count(void) { return stirred; }

/* ------------------------------------------------------------------- test */

int random_selftest(void) {
    int failures = 0;

    /* Two draws must not be the same.  That is a weak test and it is the one
     * that catches the failure that actually happens: a generator that returns
     * a constant. */
    u8 a[32], b[32];
    random_bytes(a, sizeof a);
    random_bytes(b, sizeof b);
    if (!memcmp(a, b, sizeof a)) {
        kerr("random", "two draws came out identical");
        failures++;
    }

    /* And neither may be all one value. */
    bool all_same = true;
    for (size_t i = 1; i < sizeof a; i++) if (a[i] != a[0]) all_same = false;
    if (all_same) {
        kerr("random", "a draw came out as thirty-two copies of one byte");
        failures++;
    }

    /* A crude count of set bits.  In 256 bits from a fair source the count is
     * almost always between 96 and 160; outside that is either extraordinary
     * luck or a broken generator, and the second is far more likely. */
    int ones = 0;
    for (size_t i = 0; i < sizeof a; i++)
        for (int bit = 0; bit < 8; bit++) if ((a[i] >> bit) & 1) ones++;
    if (ones < 96 || ones > 160) {
        kerr("random", "a draw had %d bits set out of 256, which is not "
                       "plausible from a working generator", ones);
        failures++;
    }

    if (!failures)
        kinfo("random", "the generator produces different, balanced output%s",
              have_hardware ? "" : " (from the pool; there is no hardware source)");
    return failures;
}
