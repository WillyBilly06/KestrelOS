/* klog.c - the kernel event log.
 *
 * Every subsystem reports through klog().  Entries land in a ring buffer that
 * survives until reboot, are mirrored to the serial port, and are shown on the
 * console when they are at least as severe as the console level.  Once a
 * writable filesystem is mounted, warnings and worse are also appended to
 * /data/logs/events.log so a crash leaves a trail behind.
 */
#include "kernel.h"
#include "klog.h"
#include "vfs.h"
#include "time.h"

extern volatile u64 g_uptime_ms;    /* timer.c; zero until the clock is up */

static klog_entry ring[KLOG_RING_SIZE];
static u64  next_seq = 1;
static u64  dropped;                     /* entries lost to a slow reader   */
static u32  level_counts[5];
static int  console_level = KLOG_INFO;
static bool in_klog;                     /* re-entrancy guard for panics    */

static char persist_path[128];
static bool persist_on;
static u64  persist_seq = 1;             /* first entry not yet written out */
static volatile u32 persist_busy;   /* atomic: this flush runs on any thread */

const char *klog_level_name(int level) {
    switch (level) {
    case KLOG_DEBUG: return "DEBUG";
    case KLOG_INFO:  return "INFO";
    case KLOG_WARN:  return "WARN";
    case KLOG_ERROR: return "ERROR";
    case KLOG_CRIT:  return "CRIT";
    default:         return "?";
    }
}

static const u8 level_color[5] = { C_DGRAY, C_LGRAY, C_YELLOW, C_LRED, C_LRED };

void klog_init(void) {
    memset(ring, 0, sizeof ring);
    next_seq = 1;
    dropped = 0;
    memset(level_counts, 0, sizeof level_counts);
}

void klog_set_console_level(int level) { console_level = level; }

void klog(int level, const char *subsys, const char *fmt, ...) {
    if (level < 0) level = 0;
    if (level > KLOG_CRIT) level = KLOG_CRIT;

    bool irq = irq_save();

    /* A fault inside the logger must not recurse forever; fall back to serial. */
    if (in_klog) {
        irq_restore(irq);
        serial_write("[klog reentry] ", 15);
        serial_write(fmt, strlen(fmt));
        serial_putc('\n');
        return;
    }
    in_klog = true;

    klog_entry *e = &ring[next_seq % KLOG_RING_SIZE];
    e->seq     = next_seq;
    e->time_ms = g_uptime_ms;
    e->level   = (u32)level;
    e->pad     = 0;
    strlcpy(e->subsys, subsys ? subsys : "kernel", sizeof e->subsys);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);

    level_counts[level]++;
    next_seq++;
    if (next_seq > KLOG_RING_SIZE && persist_seq + KLOG_RING_SIZE < next_seq) {
        dropped += next_seq - KLOG_RING_SIZE - persist_seq;
        persist_seq = next_seq - KLOG_RING_SIZE;
    }

    /* Serial always gets everything: it is the only channel that survives a
     * console failure or an early-boot fault. */
    {
        char line[256];
        int n = snprintf(line, sizeof line, "[%5u.%03u] %-5s %-8s %s\n",
                         (unsigned)(e->time_ms / 1000), (unsigned)(e->time_ms % 1000),
                         klog_level_name(level), e->subsys, e->msg);
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        serial_write(line, (size_t)n);

        /* And into memory that outlives this boot.  On a machine whose storage
         * driver never came up this is the only copy that reaches a file, and
         * it costs a copy whether or not it is ever needed. */
        ramlog_write(line, (size_t)n);
    }

    in_klog = false;
    irq_restore(irq);

    if (level >= console_level) {
        u8 c = level_color[level];
        /* The serial port already has the full line above; echoing the console
         * copy there too would double every message. */
        console_mirror_serial(false);
        if (level >= KLOG_WARN) kprintf("\x1b[%dm%s\x1b[0m: %s\n", (c == C_YELLOW ? 33 : 31), klog_level_name(level), e->msg);
        else kprintf("%s\n", e->msg);
        console_mirror_serial(true);
    }
}

int klog_read(u64 from_seq, klog_entry *out, int max) {
    bool irq = irq_save();
    if (from_seq == 0) from_seq = 1;
    u64 oldest = next_seq > KLOG_RING_SIZE ? next_seq - KLOG_RING_SIZE : 1;
    if (from_seq < oldest) from_seq = oldest;

    int n = 0;
    for (u64 s = from_seq; s < next_seq && n < max; s++) {
        klog_entry *e = &ring[s % KLOG_RING_SIZE];
        if (e->seq != s) continue;     /* overwritten while we were copying */
        out[n++] = *e;
    }
    irq_restore(irq);
    return n;
}

u64 klog_next_seq(void) { return next_seq; }
u64 klog_dropped(void)  { return dropped; }

void klog_counts(u32 counts[5]) {
    bool irq = irq_save();
    memcpy(counts, level_counts, sizeof level_counts);
    irq_restore(irq);
}

void klog_clear(void) {
    bool irq = irq_save();
    memset(ring, 0, sizeof ring);
    memset(level_counts, 0, sizeof level_counts);
    dropped = 0;
    persist_seq = next_seq;
    irq_restore(irq);
}

void klog_dump_tail(int n) {
    if (n > KLOG_RING_SIZE) n = KLOG_RING_SIZE;
    u64 start = next_seq > (u64)n ? next_seq - (u64)n : 1;
    for (u64 s = start; s < next_seq; s++) {
        klog_entry *e = &ring[s % KLOG_RING_SIZE];
        if (e->seq != s) continue;
        kprintf("  [%5u.%03u] %-5s %-8s %s\n",
                (unsigned)(e->time_ms / 1000), (unsigned)(e->time_ms % 1000),
                klog_level_name((int)e->level), e->subsys, e->msg);
    }
}

/* ------------------------------------------------------------------------- */
/* persistence                                                               */
/* ------------------------------------------------------------------------- */

bool klog_persist_active(void) { return persist_on; }

/* How much of the log reaches the disk.
 *
 * An installed system keeps its log for months and writes only what went
 * wrong, because the rest is noise that costs writes on somebody's disk.  A
 * system booted off a stick onto hardware it has never seen is the opposite
 * case: the interesting lines are the ordinary ones - which card, how much
 * memory, which connectors, how far the bring-up got - and there is exactly
 * one boot to capture them from. */
static int persist_level = KLOG_WARN;

void klog_persist_level(int level) { persist_level = level; }

/* How large the log on disk may grow before it starts again.
 *
 * Four megabytes is several hundred boots' worth at the size these run to, and
 * a twenty-fifth of the smallest boot volume this is likely to be written to.
 * Large enough that nobody loses history they wanted; small enough that a
 * driver writing in a loop cannot fill the partition the machine boots from.
 */
#define PERSIST_MAX_BYTES (4u * 1024 * 1024)

void klog_persist_enable(const char *path) {
    strlcpy(persist_path, path, sizeof persist_path);
    persist_on = true;
    /* Start from the beginning of the ring so the boot-time warnings that led
     * up to mounting the disk are recorded too. */
    persist_seq = next_seq > KLOG_RING_SIZE ? next_seq - KLOG_RING_SIZE : 1;

    /* Write a session marker straight away.  A boot that produces no warnings
     * would otherwise leave no trace at all, and an empty file looks
     * indistinguishable from logging having failed. */
    {
        char stamp[32], line[160];
        time_format(stamp, sizeof stamp, time_unix_seconds());
        int n = snprintf(line, sizeof line,
                         "\n=== KestrelOS started %s UTC ===\n", stamp);
        /* What is already there, before anything is added to it.
         *
         * A log that is appended to grows across boots; one that is being
         * overwritten stays the same size and reads as though only the last
         * boot ever happened - and the two are indistinguishable from the
         * outside without this line. */
        {
            vstat_t st;
            if (vfs_stat(persist_path, &st) == 0) {
                kinfo("log", "%s already holds %llu byte(s)", persist_path,
                      (unsigned long long)st.size);

                /* And started again when it has grown too large.
                 *
                 * This appended for ever and nothing bounded it.  On an
                 * installed machine that is slow to matter; on a stick it is
                 * not, because everything falls back to the boot volume and
                 * that is the hundred-megabyte partition the machine starts
                 * from - and this log records every line, not only the
                 * warnings, precisely because a stick on unfamiliar hardware
                 * is where the ordinary lines are the whole point.
                 *
                 * A driver stuck in a retry loop writes quickly, and a full
                 * boot volume is a machine that will not start.  Losing the
                 * older boots is a far smaller loss than that, so past the cap
                 * it begins again and says it did.
                 */
                if (st.size > PERSIST_MAX_BYTES) {
                    if (vfs_write_file(persist_path, "", 0) == 0)
                        kinfo("log", "it had grown past %u KiB, so it starts "
                                     "again from here - the boots it held are "
                                     "gone, and the volume this machine starts "
                                     "from is not full",
                              (unsigned)(PERSIST_MAX_BYTES / 1024));
                    else
                        kwarn("log", "it has grown past %u KiB and could not be "
                                     "cleared; the boot volume may fill",
                              (unsigned)(PERSIST_MAX_BYTES / 1024));
                }
            } else {
                kinfo("log", "%s does not exist yet", persist_path);
            }
        }

        persist_busy = 1;
        if (n > 0 && vfs_append(persist_path, line, (size_t)n) < 0) {
            persist_on = false;
            persist_busy = 0;
            kerr("log", "cannot write to %s; the event log will stay in memory only", persist_path);
            return;
        }
        persist_busy = 0;
    }

    klog_persist_flush();
}

/* Where entries are being written, or nothing when they are not.  The desktop
 * shows it, because "not being written" and "written to the stick you are
 * about to unplug" are both things worth knowing and only one of them used to
 * be sayable. */
const char *klog_persist_path(void) {
    return persist_on ? persist_path : "";
}

void klog_persist_flush(void) {
    if (!persist_on) return;
    if (persist_seq >= next_seq) return;

    /* Atomic: two threads that both log will both reach here, and the batch/
     * text buffers below are shared statics - a plain `if (persist_busy)`
     * check-then-set let both through on SMP and they clobbered each other.
     * If another thread is already flushing, leave it to that one (it advances
     * persist_seq past these entries anyway). */
    if (__atomic_exchange_n(&persist_busy, 1u, __ATOMIC_ACQUIRE)) return;

    /* Not on the stack.  This runs underneath a system call, on the same
     * kernel stack as the filesystem and disk code it is about to call into,
     * and thirteen kilobytes of buffers here is most of that stack gone before
     * the write even starts.  There is only ever one flush in progress -
     * `persist_busy` guarantees it - so these can be shared. */
    static klog_entry batch[32];
    static char text[32 * 220];

    for (;;) {
        int n = klog_read(persist_seq, batch, 32);
        if (n <= 0) break;

        size_t len = 0;
        for (int i = 0; i < n; i++) {
            if ((int)batch[i].level < persist_level) continue;
            int w = snprintf(text + len, sizeof text - len,
                             "[%5u.%03u] %-5s %-8s %s\n",
                             (unsigned)(batch[i].time_ms / 1000), (unsigned)(batch[i].time_ms % 1000),
                             klog_level_name((int)batch[i].level), batch[i].subsys, batch[i].msg);
            if (w > 0) len += (size_t)w;
            if (len >= sizeof text - 256) break;
        }
        persist_seq = batch[n - 1].seq + 1;

        if (len) {
            int wr = vfs_append(persist_path, text, len);
            if (wr < 0) {
                /* DIAGNOSTIC: say WHY, straight to the serial port (klog itself
                 * cannot be used here without recursing).  This used to fail
                 * silently, which is how the on-disk log could just stop with
                 * no explanation. */
                char d[96];
                int dn = snprintf(d, sizeof d,
                    "PERSIST FAIL: vfs_append(%s) = %d at seq %llu\n",
                    persist_path, wr, (unsigned long long)persist_seq);
                serial_write(d, (size_t)dn);
                persist_on = false;     /* the disk went away; stop trying */
                __atomic_store_n(&persist_busy, 0u, __ATOMIC_RELEASE);
                return;
            }
        }
        if (n < 32) break;
    }
    __atomic_store_n(&persist_busy, 0u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------- */
/* panic                                                                     */
/* ------------------------------------------------------------------------- */

void panic(const char *fmt, ...) {
    /* On the motherboard's own display, before anything else is
     * attempted: a panic that cannot reach a screen still leaves this
     * behind, and FF says the machine knew it was in trouble rather
     * than simply stopping. */
    post(POST_K_PANIC);
    cli();

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* Record it before touching the console, which may itself be broken. */
    if (!in_klog) klog(KLOG_CRIT, "panic", "%s", msg);
    else { serial_write("PANIC (in klog): ", 17); serial_write(msg, strlen(msg)); serial_putc('\n'); }

    console_set_color(C_WHITE, C_RED);
    console_clear();
    kprintf("\n  KERNEL PANIC\n  ============\n\n  %s\n\n", msg);
    console_set_color(C_LGRAY, C_BLACK);
    kprintf("  Recent events:\n");
    klog_dump_tail(18);

    /* Try once to get the record onto disk; if the filesystem is what broke,
     * the flush is a no-op because persist_on was already cleared. */
    klog_persist_flush();

    kprintf("\n  System halted. Power off or reset the machine.\n");
    for (;;) { cli(); hlt(); }
}
